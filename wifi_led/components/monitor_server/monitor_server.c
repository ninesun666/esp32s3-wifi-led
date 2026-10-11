/*
 * Monitor server: one HTML page plus a JSON status API and a LED override API.
 *
 * No authentication by design -- this is a LAN diagnostic on a board that also
 * has no way to store a secret. Anything beyond the LAN should be gated by the
 * router, not by this code.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mdns.h"
#include "link_state.h"
#include "status_light.h"
#include "display_tft.h"
#include "i2c_probe.h"
#include "monitor_server.h"
#include "web_index.h"

static const char *TAG = "monitor_server";

#define POST_BODY_MAX 192
#define MAX_I2C_REPORT 8
#define EMOTE_NAME_MAX 24

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

static void mac_string(const uint8_t *m, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

char *monitor_server_status_json(bool with_clients)
{
    /* Read the live driver state rather than caching it, so the page cannot drift
     * from what the chip is actually associated to. */
    link_state_ap_info_t ap;
    bool associated = link_state_sta_info(&ap);

    char ip[16], ap_ssid[40], mac[18], host[64];
    link_state_ip(ip, sizeof(ip));
    link_state_ap_ssid(ap_ssid, sizeof(ap_ssid));
    mac_string(associated ? ap.sta_mac : (const uint8_t[6]){0}, mac);
    snprintf(host, sizeof(host), "%s.local", CONFIG_WIFI_LED_MDNS_HOSTNAME);

    wifi_sta_list_t clients;
    int ap_stas = link_state_ap_clients(with_clients ? &clients : NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", link_state_name(link_state_get()));
    cJSON_AddStringToObject(root, "ssid", CONFIG_WIFI_LED_WIFI_SSID);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "rssi", associated ? ap.rssi : 0);
    cJSON_AddNumberToObject(root, "channel", associated ? ap.channel : 0);
    cJSON_AddStringToObject(root, "auth", associated ? ap.auth : "-");
    cJSON_AddNumberToObject(root, "uptime_s", link_state_uptime_s());
    cJSON_AddNumberToObject(root, "heap_internal",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_psram",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(root, "internet", link_state_internet());
    cJSON_AddBoolToObject(root, "led_ready", status_light_ready());
    /* Report the AP honestly: with the fallback AP compiled out there is no
     * hotspot, and the page should not claim one. */
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    cJSON_AddStringToObject(root, "ap_ip", ap_stas > 0 ? "192.168.4.1" : "-");
    cJSON_AddNumberToObject(root, "ap_stas", ap_stas);
    cJSON_AddStringToObject(root, "host", host);
    cJSON_AddBoolToObject(root, "screen_ready", display_tft_ready());
    cJSON_AddBoolToObject(root, "emote_ready", display_tft_emote_ready());

#if CONFIG_WIFI_LED_I2C_PROBE_ENABLE
    uint8_t addrs[MAX_I2C_REPORT];
    int n_addr = i2c_probe_list(addrs, MAX_I2C_REPORT);
    cJSON *i2c = cJSON_AddArrayToObject(root, "i2c");
    for (int i = 0; i < n_addr; i++) {
        char addr[8];
        sprintf(addr, "0x%02x", addrs[i]);
        cJSON_AddItemToArray(i2c, cJSON_CreateString(addr));
    }

    bool sda_high, scl_high;
    i2c_probe_levels(&sda_high, &scl_high);
    cJSON *lvl = cJSON_CreateObject();
    cJSON_AddBoolToObject(lvl, "sda", sda_high);
    cJSON_AddBoolToObject(lvl, "scl", scl_high);
    cJSON_AddItemToObject(root, "i2c_levels", lvl);
#endif

    if (with_clients) {
        cJSON *list = cJSON_AddArrayToObject(root, "clients");
        for (int i = 0; i < ap_stas; i++) {
            char client_mac[18];
            mac_string(clients.sta[i].mac, client_mac);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "mac", client_mac);
            cJSON_AddNumberToObject(item, "rssi", clients.sta[i].rssi);
            cJSON_AddItemToArray(list, item);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char *json = monitor_server_status_json(true);
    if (!json) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static long json_int(const cJSON *root, const char *key, long fallback, long lo, long hi)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) {
        return fallback;
    }
    double v = item->valuedouble;
    return v < lo ? lo : (v > hi ? hi : (long)v);
}

/* Shared body reader for the POST endpoints: small JSON, hard cap, no streaming. */
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > POST_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or empty");
        return NULL;
    }

    char buf[POST_BODY_MAX + 1];
    int got = httpd_req_recv(req, buf, (int)req->content_len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return NULL;
    }
    buf[got] = '\0';

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }
    return body;
}

static esp_err_t send_ok(httpd_req_t *req, bool ok, const char *why)
{
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddBoolToObject(ack, "ok", ok);
    if (why) {
        cJSON_AddStringToObject(ack, "error", why);
    }
    char *text = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    free(text);
    return err;
}

/* GET/POST /api/led -- persistent takeover of the strip. POST accepts the same
 * JSON the BLE LED characteristic does ({"manual":…,"fx":"…","rgb":[…],
 * "brightness":…}); manual:false hands the strip back to the link state machine.
 * The setting lives in RAM, so a reboot returns to automatic. */
static esp_err_t led_get_handler(httpd_req_t *req)
{
    char json[96];
    status_light_ctl_to_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t led_post_handler(httpd_req_t *req)
{
    /* The UI always posts the full object, well under POST_BODY_MAX. */
    char buf[POST_BODY_MAX + 1] = {0};
    int len = httpd_req_recv(req, buf, POST_BODY_MAX);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    const char *errmsg = NULL;
    if (!status_light_ctl_from_json(buf, &errmsg)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errmsg);
        return ESP_FAIL;
    }

    char json[96];
    status_light_ctl_to_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/screen {"emote":"happy"} shows a face from the anim_icon asset pack;
 * {"r":255,"g":0,"b":0} repaints the whole TFT one colour; {"pattern":"bars"}
 * repaints the red/green/blue wiring-proof bars. */
static esp_err_t screen_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return ESP_FAIL;
    }

    long r = json_int(body, "r", 0, 0, 255);
    long g = json_int(body, "g", 0, 0, 255);
    long b = json_int(body, "b", 0, 0, 255);
    cJSON *pattern = cJSON_GetObjectItemCaseSensitive(body, "pattern");
    cJSON *emote = cJSON_GetObjectItemCaseSensitive(body, "emote");
    bool bars = cJSON_IsString(pattern) && strcmp(pattern->valuestring, "bars") == 0;
    char emote_name[EMOTE_NAME_MAX] = {0};
    bool emote_too_long = false;
    if (cJSON_IsString(emote)) {
        emote_too_long = strlen(emote->valuestring) >= EMOTE_NAME_MAX;
        if (!emote_too_long) {
            strcpy(emote_name, emote->valuestring);
        }
    }
    cJSON_Delete(body);

    bool ok;
    const char *what;
    const char *why = "panel not ready";
    if (emote_name[0]) {
        what = emote_name;
        ok = display_tft_emote_show(emote_name);
        if (!ok && display_tft_ready()) {
            why = "no such emote in the asset pack";
        }
    } else if (emote_too_long) {
        what = "emote (name too long)";
        ok = false;
        why = "emote name too long";
    } else if (bars) {
        what = "bars";
        ok = display_tft_ready();
        if (ok) {
            display_tft_bars();
        }
    } else {
        what = "rgb fill";
        ok = display_tft_fill((uint8_t)r, (uint8_t)g, (uint8_t)b);
    }

    ESP_LOGI(TAG, "screen <- %s %s", what, ok ? "done" : why);
    return send_ok(req, ok, ok ? NULL : why);
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s (use the IP address instead)",
                 esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_WIFI_LED_MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32-S3 Wi-Fi 监控"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp",
                                    CONFIG_WIFI_LED_HTTP_PORT, NULL, 0));
    ESP_LOGI(TAG, "mDNS: http://%s.local:%d/", CONFIG_WIFI_LED_MDNS_HOSTNAME,
             CONFIG_WIFI_LED_HTTP_PORT);
}

void monitor_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.server_port = CONFIG_WIFI_LED_HTTP_PORT;
    /* cJSON building both the status object and the reply needs more than the
     * 4096 default once the client list is full. */
    cfg.stack_size = 8192;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %d", cfg.server_port);
        return;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/api/led",    .method = HTTP_GET,  .handler = led_get_handler },
        { .uri = "/api/led",    .method = HTTP_POST, .handler = led_post_handler },
        { .uri = "/api/screen", .method = HTTP_POST, .handler = screen_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    start_mdns();

    ESP_LOGI(TAG, "web monitor on port %d:  open the URL logged at \"got IP\"",
             cfg.server_port);
}
