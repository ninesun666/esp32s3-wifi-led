/*
 * Link state: Wi-Fi bring-up, association with a security-ladder retry, and an
 * internet reachability probe. Publishes one of three states that the status
 * light and the web monitor both read.
 *
 * The probe and the connect ladder run in their own tasks because both block:
 * esp_wifi_scan_start(sync) for seconds, and a TCP connect has to be waited on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "link_state.h"
#include "lwip/sockets.h"

static const char *TAG = "link_state";

#define BIT_STA_STARTED BIT0
#define BIT_GOT_IP      BIT1
#define BIT_LOST_IP     BIT2

/* 114.114.114.114:53 rather than a hostname, so the probe measures raw internet
 * reachability and cannot be masked by a broken local DNS server. */
#define PROBE_IP   "114.114.114.114"
#define PROBE_PORT 53

/* Fallback access path, off by default. The SoftAP is a pure L2 AP with its own
 * DHCP but no NAT to the STA uplink, and a joining Windows PC moves its default
 * gateway here -- which silently kills that PC's internet. Keep it only for
 * boards whose router-assigned LAN address really is unreachable. */
#if CONFIG_WIFI_LED_AP_ENABLE
#define AP_SSID    CONFIG_WIFI_LED_AP_SSID
#define AP_PASS    CONFIG_WIFI_LED_AP_PASSWORD
#define AP_MAX_STA 4
#endif

static volatile link_state_t s_state = LINK_STATE_CONNECTING;
static volatile bool s_got_ip = false;
static volatile bool s_online = false;
static char s_ip_str[16] = "0.0.0.0";
static EventGroupHandle_t s_wifi_events;

/* An authmode threshold above WPA2 that yields reason 211 proves the AP is not
 * WPA3-class, so the ladder is ordered for a WPA2 router and covers the ways a
 * 4-way handshake (reason 204) still fails against one: wrong band assumption,
 * TKIP-only pairwise suite, and a mesh/eMesh SSID where the strongest-looking
 * BSS is not the one that will complete the handshake. */
typedef struct {
    wifi_auth_mode_t threshold;
    bool pin_bssid;
    const char *name;
} attempt_cfg_t;

static const attempt_cfg_t k_attempts[] = {
    { WIFI_AUTH_WPA2_PSK,     false, "WPA2, any BSS" },
    { WIFI_AUTH_WPA2_PSK,     true,  "WPA2, pinned BSS" },
    { WIFI_AUTH_WPA_PSK,      false, "WPA/WPA2 (TKIP allowed)" },
    { WIFI_AUTH_OPEN,         false, "accept whatever the AP offers" },
    { WIFI_AUTH_WPA3_PSK,     false, "WPA3" },
};
#define N_ATTEMPTS (sizeof(k_attempts) / sizeof(k_attempts[0]))

/* Strongest BSS advertising the target SSID, refreshed by every scan. */
static uint8_t s_target_bssid[6];
static bool s_target_seen = false;

const char *link_state_name(link_state_t state)
{
    switch (state) {
    case LINK_STATE_CONNECTING:     return "connecting";
    case LINK_STATE_LINKED_NO_NET:  return "linked";
    case LINK_STATE_ONLINE:         return "online";
    default:                        return "unknown";
    }
}

link_state_t link_state_get(void)
{
    return s_state;
}

bool link_state_has_ip(void)
{
    return s_got_ip;
}

bool link_state_internet(void)
{
    return s_online;
}

uint32_t link_state_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void link_state_ip(char *out, size_t out_len)
{
    strlcpy(out, s_ip_str, out_len);
}

int link_state_ap_clients(wifi_sta_list_t *clients)
{
    if (clients) {
        memset(clients, 0, sizeof(*clients));
    }
    wifi_sta_list_t local;
    wifi_sta_list_t *target = clients ? clients : &local;
    return (esp_wifi_ap_get_sta_list(target) == ESP_OK) ? target->num : 0;
}

static const char *reason_name(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:  return "4-way handshake timeout - wrong password";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:       return "handshake timeout - password, MAC whitelist / max-clients, or a BSS that stops answering";
    case WIFI_REASON_AUTH_FAIL:               return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL:              return "association failed";
    case WIFI_REASON_CONNECTION_FAIL:         return "connection failed";
    case WIFI_REASON_NO_AP_FOUND:             return "AP not found - wrong SSID or 5GHz-only";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
                                              return "AP found but no mutually supported security mode";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                                              return "AP found but below the RSSI threshold";
    case WIFI_REASON_BEACON_TIMEOUT:          return "beacon timeout - out of range";
    default:                                  return "(see esp_wifi_types_generic.h)";
    }
}

const char *link_state_auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA+WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2+WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI_PSK";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "?";
    }
}

static const char *cipher_name(wifi_cipher_type_t c)
{
    switch (c) {
    case WIFI_CIPHER_TYPE_NONE:      return "NONE";
    case WIFI_CIPHER_TYPE_WEP40:     return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104:    return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:      return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+CCMP";
    default:                         return "?";
    }
}

static void describe_ap(const wifi_ap_record_t *ap, const char *prefix)
{
    ESP_LOGI(TAG, "%s\"%s\" ch=%u rssi=%d auth=%s(%d) pairwise=%s group=%s cc=%.2s"
                  " bssid=%02x:%02x:%02x:%02x:%02x:%02x%s%s",
             prefix, (const char *)ap->ssid, (unsigned)ap->primary, (int)ap->rssi,
             link_state_auth_name(ap->authmode), (int)ap->authmode,
             cipher_name(ap->pairwise_cipher), cipher_name(ap->group_cipher),
             ap->country.cc,
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5],
             ap->wps ? " wps" : "",
             ap->phy_11ax ? " ax" : (ap->phy_11n ? " n" : " legacy"));
}

bool link_state_sta_info(link_state_ap_info_t *out)
{
    memset(out, 0, sizeof(*out));
    wifi_ap_record_t rec;
    if (esp_wifi_sta_get_ap_info(&rec) != ESP_OK) {
        return false;
    }
    out->associated = true;
    out->rssi = rec.rssi;
    out->channel = rec.primary;
    out->auth = link_state_auth_name(rec.authmode);
    esp_wifi_get_mac(WIFI_IF_STA, out->sta_mac);
    return true;
}

void link_state_ap_ssid(char *out, size_t out_len)
{
#if CONFIG_WIFI_LED_AP_ENABLE
    strlcpy(out, AP_SSID, out_len);
#else
    strlcpy(out, "off", out_len);
#endif
}

/* Reports nearby networks and records the target SSID's strongest BSS. Called
 * before every connect attempt, because the authmode/cipher/BSSID fields are what
 * turns an opaque reason 204 into a specific diagnosis, and a serial monitor that
 * joins late still gets them. */
static void scan_for_target(void)
{
    wifi_scan_config_t scan = { .show_hidden = true };
    ESP_LOGI(TAG, "scanning...");
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        ESP_LOGW(TAG, "scan found nothing");
        return;
    }
    uint16_t report = found > 12 ? 12 : found;

    wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * report);
    if (!aps) {
        return;
    }
    esp_wifi_scan_get_ap_records(&report, aps);

    s_target_seen = false;
    int8_t best_rssi = -127;
    int matches = 0;
    for (int i = 0; i < report; i++) {
        bool is_target = strcmp((const char *)aps[i].ssid, CONFIG_WIFI_LED_WIFI_SSID) == 0;
        if (is_target) {
            matches++;
            describe_ap(&aps[i], "  >");
            if ((int8_t)aps[i].rssi > best_rssi) {
                best_rssi = (int8_t)aps[i].rssi;
                memcpy(s_target_bssid, aps[i].bssid, sizeof(s_target_bssid));
                s_target_seen = true;
            }
        } else {
            describe_ap(&aps[i], "   ");
        }
    }
    free(aps);

    if (!s_target_seen) {
        ESP_LOGE(TAG, "target \"%s\" NOT in scan results", CONFIG_WIFI_LED_WIFI_SSID);
    } else {
        ESP_LOGI(TAG, "target \"%s\" seen on %d BSS, strongest pinned above",
                 CONFIG_WIFI_LED_WIFI_SSID, matches);
    }
}

static void apply_wifi_config(size_t try_index)
{
    wifi_config_t c = {0};
    snprintf((char *)c.sta.ssid, sizeof(c.sta.ssid), "%s", CONFIG_WIFI_LED_WIFI_SSID);
    snprintf((char *)c.sta.password, sizeof(c.sta.password), "%s", CONFIG_WIFI_LED_WIFI_PASSWORD);
    c.sta.threshold.authmode = k_attempts[try_index].threshold;
    /* pmf_cfg.capable is deprecated and ignored by the driver; only `required`
     * has any effect, and leaving it false accepts both PMF and non-PMF APs. */
    c.sta.pmf_cfg.capable = true;
    c.sta.pmf_cfg.required = false;
    if (k_attempts[try_index].pin_bssid && s_target_seen) {
        memcpy(c.sta.bssid, s_target_bssid, sizeof(c.sta.bssid));
        c.sta.bssid_set = true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_events, BIT_STA_STARTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_got_ip = false;
        s_state = LINK_STATE_CONNECTING;
        xEventGroupClearBits(s_wifi_events, BIT_GOT_IP);
        xEventGroupSetBits(s_wifi_events, BIT_LOST_IP);
        ESP_LOGW(TAG, "disconnected, reason=%u: %s", d->reason, reason_name(d->reason));
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    s_got_ip = true;
    s_state = LINK_STATE_LINKED_NO_NET;
    xEventGroupClearBits(s_wifi_events, BIT_LOST_IP);
    xEventGroupSetBits(s_wifi_events, BIT_GOT_IP);
    /* esp_ip4_addr_t and lwip's ip4_addr_t share a layout; the cast is what the
     * netif/lwip boundary needs to reuse the lwip printer. */
    strlcpy(s_ip_str, ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip), sizeof(s_ip_str));
    ESP_LOGI(TAG, "got IP: %s  ->  http://%s/  or  http://%s.local/",
             s_ip_str, s_ip_str, CONFIG_WIFI_LED_MDNS_HOSTNAME);
}

static void connect_task(void *arg)
{
    xEventGroupWaitBits(s_wifi_events, BIT_STA_STARTED, pdTRUE, pdFALSE, portMAX_DELAY);
    size_t try_index = 0;
    while (true) {
        xEventGroupClearBits(s_wifi_events, BIT_GOT_IP | BIT_LOST_IP);
        scan_for_target();
        apply_wifi_config(try_index);
        ESP_LOGI(TAG, "connecting (attempt %u/%u: %s)",
                 (unsigned)(try_index + 1), (unsigned)N_ATTEMPTS,
                 k_attempts[try_index].name);

        if (esp_wifi_connect() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            try_index = (try_index + 1) % N_ATTEMPTS;
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, BIT_GOT_IP | BIT_LOST_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

        if (bits & BIT_GOT_IP) {
            try_index = 0;
            /* Remain in this branch until the link drops; esp_wifi_connect() must
             * not be called again while associated. */
            xEventGroupWaitBits(s_wifi_events, BIT_LOST_IP, pdTRUE, pdFALSE, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            try_index = (try_index + 1) % N_ATTEMPTS;
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

static bool probe_internet(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROBE_PORT);
    inet_pton(AF_INET, PROBE_IP, &addr.sin_addr);

    bool ok = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(sock);
    return ok;
}

static void probe_task(void *arg)
{
    bool was_online = false;
    while (true) {
        if (!s_got_ip) {
            s_state = LINK_STATE_CONNECTING;
            s_online = false;
            was_online = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool online = probe_internet();
        s_online = online;
        s_state = online ? LINK_STATE_ONLINE : LINK_STATE_LINKED_NO_NET;
        if (online != was_online) {
            ESP_LOGI(TAG, "internet probe: %s", online ? "reachable" : "unreachable");
            was_online = online;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void report_memory(void)
{
    size_t internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "heap: internal=%u KiB, psram=%u KiB%s",
             (unsigned)(internal / 1024), (unsigned)(psram / 1024),
             psram > 0 ? " (Octal PSRAM OK)" : " (PSRAM NOT ENABLED)");

    /* Printed every attempt because a router that filters by MAC drops the
     * handshake silently, and the address to whitelist is the one below. */
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "sta mac: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void link_state_init(void)
{
    ESP_LOGI(TAG, "target SSID: \"%s\" (password %u chars)",
             CONFIG_WIFI_LED_WIFI_SSID, (unsigned)strlen(CONFIG_WIFI_LED_WIFI_PASSWORD));

    report_memory();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
#if CONFIG_WIFI_LED_AP_ENABLE
    esp_netif_create_default_wifi_ap();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &ip_event_handler, NULL, NULL);

#if CONFIG_WIFI_LED_AP_ENABLE
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    /* The AP config has to precede start(): after start the beacon is already
     * out with the empty default SSID and the new one does not replace it. */
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", AP_PASS);
    ap.ap.channel = 0;   /* follow the STA channel; AP+STA cannot straddle two */
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_STA;
    ap.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Both require an enabled interface, hence after start(). RAM storage because
     * every attempt writes a fresh config and a stale credential in NVS would
     * silently override it; modem sleep can drop EAPOL frames to finicky
     * routers, so it stays off while this is a diagnostic. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    xTaskCreate(probe_task, "probe", 4096, NULL, 5, NULL);
    xTaskCreate(connect_task, "connect", 4096, NULL, 6, NULL);
}
