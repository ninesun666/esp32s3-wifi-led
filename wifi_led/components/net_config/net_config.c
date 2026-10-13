/*
 * Wi-Fi configuration protocol shared by every transport that can change it.
 *
 * Keeping the command parsing, the reply shape and the validation in one place
 * is what makes "the app and the web page behave the same" a fact rather than a
 * coincidence: the BLE characteristic (0008) and GET/POST /api/wifi are thin
 * adapters over these two functions.
 *
 * The reply always answers three questions a phone needs before it can ask a
 * fourth: is anything configured, what is it pointed at, and did the last scan
 * finish. Passwords are never echoed back, in any form.
 */

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "link_state.h"
#include "net_config.h"
#include "wifi_creds.h"

static const char *TAG = "net_config";

#define SCAN_LIST_MAX 8

static const char *json_str(const cJSON *body, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(body, key));
}

char *net_config_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    /* proto lets a phone tell "old firmware, no provisioning" (read fails /
     * characteristic missing) from "new firmware, nothing configured". */
    cJSON_AddNumberToObject(root, "proto", 1);
    cJSON_AddStringToObject(root, "source", wifi_creds_source());

    char ssid[WIFI_CRED_SSID_MAX + 1] = "";
    link_state_target_ssid(ssid, sizeof(ssid));
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddBoolToObject(root, "configured", ssid[0] != '\0');
    cJSON_AddBoolToObject(root, "has_password", wifi_creds_has_password());
    cJSON_AddStringToObject(root, "state", link_state_name(link_state_get()));

    link_scan_phase_t phase = link_state_scan_phase();
    const char *phase_name = "idle";
    switch (phase) {
    case LINK_SCAN_RUNNING: phase_name = "running"; break;
    case LINK_SCAN_DONE:    phase_name = "done"; break;
    case LINK_SCAN_ERROR:   phase_name = "error"; break;
    default: break;
    }
    cJSON_AddStringToObject(root, "scan", phase_name);
    if (phase == LINK_SCAN_ERROR) {
        cJSON_AddStringToObject(root, "scan_error",
                                esp_err_to_name(link_state_scan_error()));
    }
    if (phase == LINK_SCAN_DONE) {
        link_state_net_t nets[SCAN_LIST_MAX];
        int n = link_state_scan_results(nets, SCAN_LIST_MAX);
        cJSON *list = cJSON_AddArrayToObject(root, "networks");
        for (int i = 0; list && i < n; i++) {
            cJSON *one = cJSON_CreateObject();
            if (!one) {
                break;
            }
            bool open = strcmp(nets[i].auth, "OPEN") == 0;
            cJSON_AddStringToObject(one, "ssid", nets[i].ssid);
            cJSON_AddNumberToObject(one, "rssi", nets[i].rssi);
            cJSON_AddStringToObject(one, "auth", nets[i].auth);
            cJSON_AddBoolToObject(one, "open", open);
            cJSON_AddItemToArray(list, one);
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

bool net_config_apply(const cJSON *body, const char **what_out, const char **why_out)
{
    const char *cmd = json_str(body, "cmd");
    if (!cmd) {
        /* A bare {"ssid":...,"password":...} is the shape a hand-written curl
         * or a small web form produces; treat it as "set". */
        cmd = json_str(body, "ssid") ? "set" : NULL;
    }
    if (what_out) {
        *what_out = cmd ? cmd : "(none)";
    }

    if (!cmd) {
        if (why_out) {
            *why_out = "missing cmd (set / scan / forget)";
        }
        return false;
    }

    if (strcmp(cmd, "scan") == 0) {
        if (!link_state_scan_start()) {
            if (why_out) {
                *why_out = "a scan is already running";
            }
            return false;
        }
        if (why_out) {
            *why_out = NULL;
        }
        return true;
    }

    if (strcmp(cmd, "forget") == 0) {
        wifi_creds_err_t err = wifi_creds_clear();
        if (err != WIFI_CREDS_OK) {
            if (why_out) {
                *why_out = wifi_creds_err_name(err);
            }
            return false;
        }
        link_state_apply_new_config();
        if (why_out) {
            *why_out = NULL;
        }
        return true;
    }

    if (strcmp(cmd, "set") != 0) {
        if (why_out) {
            *why_out = "unknown cmd";
        }
        return false;
    }

    const char *ssid = json_str(body, "ssid");
    const char *pass = json_str(body, "password");
    wifi_creds_err_t err = wifi_creds_save(ssid, pass ? pass : "");
    if (err != WIFI_CREDS_OK) {
        if (why_out) {
            *why_out = wifi_creds_err_name(err);
        }
        ESP_LOGW(TAG, "provisioning rejected: %s", wifi_creds_err_name(err));
        return false;
    }
    link_state_apply_new_config();
    if (why_out) {
        *why_out = NULL;
    }
    /* Deliberately not the association result -- that arrives on the status
     * side. A write this deep only says "stored and handed to the ladder". */
    ESP_LOGI(TAG, "network set to \"%s\", re-associating", ssid);
    return true;
}
