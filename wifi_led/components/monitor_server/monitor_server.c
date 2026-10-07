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
#include "monitor_server.h"
#include "web_index.h"

static const char *TAG = "monitor_server";

#define LED_BODY_MAX 192
#define LED_HOLD_MAX 600000

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

static void mac_string(const uint8_t *m, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static esp_err_t status_handler(httpd_req_t *req)
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
    int ap_stas = link_state_ap_clients(&clients);

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

    cJSON *list = cJSON_AddArrayToObject(root, "clients");
    for (int i = 0; i < ap_stas; i++) {
        char client_mac[18];
        mac_string(clients.sta[i].mac, client_mac);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", client_mac);
        cJSON_AddNumberToObject(item, "rssi", clients.sta[i].rssi);
        cJSON_AddItemToArray(list, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
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

/* POST /api/led {"r":200,"g":40,"b":40,"hold_ms":5000} -- hold_ms 0 hands the
 * strip back to the link state machine. This is the hook an external brain
 * (Home Assistant, a 米家 scene, curl) drives the board through. */
static esp_err_t led_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > LED_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or empty");
        return ESP_FAIL;
    }

    char buf[LED_BODY_MAX + 1];
    int got = httpd_req_recv(req, buf, (int)req->content_len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[got] = '\0';

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    long r = json_int(body, "r", 0, 0, 255);
    long g = json_int(body, "g", 0, 0, 255);
    long b = json_int(body, "b", 0, 0, 255);
    long hold = json_int(body, "hold_ms", 5000, 0, LED_HOLD_MAX);
    cJSON_Delete(body);

    status_light_override((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint32_t)hold);
    ESP_LOGI(TAG, "led <- rgb(%ld,%ld,%ld) hold %ld ms", r, g, b, hold);

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddBoolToObject(ack, "ok", true);
    cJSON_AddBoolToObject(ack, "led_ready", status_light_ready());
    char *text = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{\"ok\":true}");
    free(text);
    return err;
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
        { .uri = "/api/led",    .method = HTTP_POST, .handler = led_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    start_mdns();

    ESP_LOGI(TAG, "web monitor on port %d:  open the URL logged at \"got IP\"",
             cfg.server_port);
}
