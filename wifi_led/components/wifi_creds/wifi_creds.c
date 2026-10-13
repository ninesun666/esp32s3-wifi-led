/*
 * Where the target access point comes from. One NVS namespace owns the runtime
 * answer, so BLE provisioning, the HTTP API and the connect ladder all read the
 * same store instead of each carrying its own copy of the credentials.
 *
 * Precedence (highest first):
 *   1. NVS keys "ssid"/"pass" in namespace "wifi" -- written by provisioning.
 *   2. Build-time Kconfig (CONFIG_WIFI_LED_WIFI_SSID/_PASSWORD via
 *      sdkconfig.local), used only while nothing is stored. It is the recovery
 *      path: a board stuck on a dead network can always be reflashed with
 *      sdkconfig.local to get it back on the air.
 *   3. Nothing -- the board stays in "connecting" and waits to be provisioned.
 *
 * The password never leaves this file: no log line, no getter that a display
 * can accidentally print. Callers get "is a password set" instead.
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "wifi_creds.h"

static const char *TAG = "wifi_creds";

#define NVS_NS       "wifi"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

/* A build that never got an sdkconfig.local still carries these placeholders. */
#define IS_PLACEHOLDER(s) (strcmp(s, "CHANGE_ME") == 0 || (s)[0] == '\0')

static esp_err_t open_store(nvs_handle_t *h, bool read_write)
{
    return nvs_open(NVS_NS, read_write ? NVS_READWRITE : NVS_READONLY, h);
}

bool wifi_creds_has_stored(void)
{
    nvs_handle_t h;
    if (open_store(&h, false) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    bool ok = nvs_get_str(h, NVS_KEY_SSID, NULL, &len) == ESP_OK && len > 1;
    nvs_close(h);
    return ok;
}

static void load_stored(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (open_store(&h, false) != ESP_OK) {
        return;
    }
    size_t len = ssid_len;
    if (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) != ESP_OK) {
        ssid[0] = '\0';
    }
    len = pass_len;
    if (nvs_get_str(h, NVS_KEY_PASS, pass, &len) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
}

const char *wifi_creds_source(void)
{
    if (wifi_creds_has_stored()) {
        return "stored";
    }
    if (!IS_PLACEHOLDER(CONFIG_WIFI_LED_WIFI_SSID)) {
        return "build";
    }
    return "none";
}

void wifi_creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (ssid_len == 0 || pass_len == 0) {
        return;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    load_stored(ssid, ssid_len, pass, pass_len);
    if (ssid[0] != '\0') {
        return;
    }

    /* Build-time fallback, unless it is still the template value. */
    if (!IS_PLACEHOLDER(CONFIG_WIFI_LED_WIFI_SSID)) {
        strlcpy(ssid, CONFIG_WIFI_LED_WIFI_SSID, ssid_len);
        if (!IS_PLACEHOLDER(CONFIG_WIFI_LED_WIFI_PASSWORD)) {
            strlcpy(pass, CONFIG_WIFI_LED_WIFI_PASSWORD, pass_len);
        }
    }
}

bool wifi_creds_has_password(void)
{
    char ssid[WIFI_CRED_SSID_MAX + 1];
    char pass[WIFI_CRED_PASS_MAX + 1];
    wifi_creds_load(ssid, sizeof(ssid), pass, sizeof(pass));
    bool set = pass[0] != '\0';
    memset(pass, 0, sizeof(pass));
    return set;
}

wifi_creds_err_t wifi_creds_validate(const char *ssid, const char *pass)
{
    if (!ssid) {
        return WIFI_CREDS_BAD_SSID;
    }
    size_t ssid_bytes = strlen(ssid);
    if (ssid_bytes == 0 || ssid_bytes > WIFI_CRED_SSID_MAX) {
        return WIFI_CREDS_BAD_SSID;
    }
    if (pass) {
        size_t pass_bytes = strlen(pass);
        /* Empty means an open network; anything else must be a real passphrase. */
        if (pass_bytes != 0 && (pass_bytes < 8 || pass_bytes > WIFI_CRED_PASS_MAX)) {
            return WIFI_CREDS_BAD_PASSWORD;
        }
    }
    return WIFI_CREDS_OK;
}

wifi_creds_err_t wifi_creds_save(const char *ssid, const char *pass)
{
    wifi_creds_err_t err = wifi_creds_validate(ssid, pass);
    if (err != WIFI_CREDS_OK) {
        return err;
    }

    nvs_handle_t h;
    esp_err_t esp = open_store(&h, true);
    if (esp != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(esp));
        return WIFI_CREDS_STORE_FAILED;
    }
    esp = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (esp == ESP_OK) {
        esp = nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    }
    if (esp == ESP_OK) {
        esp = nvs_commit(h);
    }
    nvs_close(h);
    if (esp != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed: %s", esp_err_to_name(esp));
        return WIFI_CREDS_STORE_FAILED;
    }
    /* SSID is safe to log (it is broadcast anyway); the length of the
     * passphrase is the only thing that ever appears. */
    ESP_LOGI(TAG, "stored network \"%s\" (passphrase %u chars)", ssid,
             (unsigned)(pass ? strlen(pass) : 0));
    return WIFI_CREDS_OK;
}

const char *wifi_creds_err_name(wifi_creds_err_t err)
{
    switch (err) {
    case WIFI_CREDS_OK:            return "ok";
    case WIFI_CREDS_BAD_SSID:      return "ssid must be 1-32 bytes";
    case WIFI_CREDS_BAD_PASSWORD:  return "passphrase must be empty or 8-64 bytes";
    case WIFI_CREDS_STORE_FAILED:  return "storage write failed";
    default:                       return "unknown";
    }
}

wifi_creds_err_t wifi_creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t esp = open_store(&h, true);
    if (esp != ESP_OK) {
        return WIFI_CREDS_STORE_FAILED;
    }
    /* erase returns NVS_KEY_NOT_FOUND for an absent key; that is the state we
     * wanted anyway, so it is not an error here. */
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    esp = nvs_commit(h);
    nvs_close(h);
    if (esp != ESP_OK) {
        return WIFI_CREDS_STORE_FAILED;
    }
    ESP_LOGI(TAG, "stored network cleared");
    return WIFI_CREDS_OK;
}
