/*
 * BLE GATT server: one custom service exposing the strip takeover setting, the
 * device status JSON, the screen/emote command, the OTA transfer (control on
 * 0005 + raw image chunks on 0006), the HID dispatcher (0007) and the Wi-Fi
 * provisioning control (0008), so a phone can drive and configure the board
 * without the web page. The LED characteristic shares status_light's control
 * state with POST /api/led; the status characteristic shares monitor_server's
 * /api/status object (client list dropped so one ATT read carries it); the
 * screen characteristic reuses /api/screen's apply logic; the Wi-Fi
 * characteristic is GET/POST /api/wifi over ATT.
 *
 * Everything below compiles away when CONFIG_EXAMPLE_BLE_ENABLE is off (the
 * NimBLE headers do not exist with Bluetooth disabled).
 */

#include "sdkconfig.h"

#if CONFIG_EXAMPLE_BLE_ENABLE

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"
#include "status_light.h"
#include "monitor_server.h"
#include "net_config.h"
#include "ota_update.h"
#include "usb_hid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_gatt";

/* One custom 128-bit service, 5d400001-7a9b-4c8e-b2f4-38a1c6e0d951, with a
 * LED characteristic (0002, same JSON as POST /api/led), a read-only
 * status characteristic (0003, same fields as /api/status), a screen
 * characteristic (0004, same JSON as POST /api/screen) and a Wi-Fi
 * provisioning characteristic (0008, same JSON as GET/POST /api/wifi).
 * UUID bytes are listed LSB-first, as BLE_UUID128_INIT and the on-air format
 * want. */
static const ble_uuid128_t k_ble_svc_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x01, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_led_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x02, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_status_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x03, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_screen_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x04, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_ota_ctl_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x05, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_ota_data_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x06, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_hid_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x07, 0x00, 0x40, 0x5d);
static const ble_uuid128_t k_ble_wifi_uuid =
    BLE_UUID128_INIT(0x51, 0xd9, 0xe0, 0xc6, 0xa1, 0x38, 0xf4, 0xb2,
                     0x8e, 0x4c, 0x9b, 0x7a, 0x08, 0x00, 0x40, 0x5d);

static uint8_t s_ble_addr_type;

/* The rest of this profile is open on purpose (a demo lamp anyone in range can
 * drive). Wi-Fi credentials are not that harmless, so the write side can be
 * gated on an encrypted link: with CONFIG_WIFI_LED_BLE_PROV_ENCRYPT the client
 * must be paired/bonded first, and NimBLE starts the security procedure when an
 * unauthenticated write arrives. Off by default because an unbonded phone then
 * simply fails the write -- see docs/wifi_provisioning.md. */
#if CONFIG_WIFI_LED_BLE_PROV_ENCRYPT
#define WIFI_CHR_FLAGS (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC)
#else
#define WIFI_CHR_FLAGS (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE)
#endif

/* JSON number with clamped range, for fields like usage/modifier/size. */
static long json_long(const cJSON *item)
{
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    double v = item->valuedouble;
    return v < 0 ? 0 : (v > 0x7fffffff ? 0x7fffffff : (long)v);
}

/* The public store header does not declare this; official examples forward-
 * declare it the same way. */
extern void ble_store_config_init(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

static int ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* arg names the characteristic: 2 = LED, 3 = status, 4 = screen,
     * 5 = OTA control, 6 = OTA data, 7 = HID, 8 = Wi-Fi provisioning. */
    if ((uintptr_t)arg == 2) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            char buf[256] = {0};
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            if (om_len >= sizeof(buf)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, NULL);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            const char *errmsg = NULL;
            if (!status_light_ctl_from_json(buf, &errmsg)) {
                ESP_LOGW(TAG, "BLE LED write rejected: %s", errmsg);
                return 0x81; /* application error: invalid LED JSON */
            }
            return 0;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            char json[96];
            status_light_ctl_to_json(json, sizeof(json));
            return os_mbuf_append(ctxt->om, json, strlen(json));
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    if ((uintptr_t)arg == 3) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        /* No hotspot client list, so one ATT read carries the payload at the
         * 512 MTU; phones that negotiate smaller fall back to long reads,
         * which the NimBLE server handles by itself. */
        char *json = monitor_server_status_json(false);
        if (!json) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        int rc = os_mbuf_append(ctxt->om, json, strlen(json));
        free(json);
        return rc;
    }

    if ((uintptr_t)arg == 4) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        char buf[128] = {0};
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len >= sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, NULL);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        cJSON *body = cJSON_Parse(buf);
        if (!body) {
            ESP_LOGW(TAG, "BLE screen write rejected: bad JSON");
            return 0x81; /* application error: invalid screen JSON */
        }
        const char *what = "(none)";
        const char *why = "panel not ready";
        bool ok = monitor_server_screen_apply(body, &what, &why);
        cJSON_Delete(body);
        if (!ok) {
            ESP_LOGW(TAG, "BLE screen write failed: %s %s", what, why);
            return 0x81;
        }
        ESP_LOGI(TAG, "BLE screen <- %s", what);
        return 0;
    }

    if ((uintptr_t)arg == 5) {
        /* OTA control: JSON commands, raw image data flows through 0006. */
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            size_t written = 0, total = 0;
            bool busy = false;
            ota_update_progress(&written, &total, &busy);
            char json[96];
            snprintf(json, sizeof(json),
                     "{\"busy\":%s,\"written\":%u,\"total\":%u}",
                     busy ? "true" : "false", (unsigned)written, (unsigned)total);
            return os_mbuf_append(ctxt->om, json, strlen(json));
        }
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        char buf[256] = {0};
        if (OS_MBUF_PKTLEN(ctxt->om) >= sizeof(buf) ||
            ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, NULL) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        cJSON *body = cJSON_Parse(buf);
        if (!body) {
            return 0x81;
        }
        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(body, "cmd"));
        esp_err_t err = ESP_OK;
        if (cmd && strcmp(cmd, "begin") == 0) {
            uint8_t sha[32];
            bool has_sha = false;
            const char *hex = cJSON_GetStringValue(cJSON_GetObjectItem(body, "sha256"));
            if (hex && strlen(hex) == 64) {
                has_sha = true;
                for (int i = 0; i < 32; i++) {
                    unsigned byte;
                    sscanf(hex + 2 * i, "%2x", &byte);
                    sha[i] = (uint8_t)byte;
                }
            }
            long size = json_long(cJSON_GetObjectItem(body, "size"));
            err = size > 0 ? ota_update_begin((size_t)size, has_sha, sha)
                           : ESP_ERR_INVALID_ARG;
        } else if (cmd && strcmp(cmd, "end") == 0) {
            err = ota_update_finish();
            if (err == ESP_OK) {
                ota_update_reboot_later();
            }
        } else if (cmd && strcmp(cmd, "abort") == 0) {
            err = ota_update_abort();
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
        cJSON_Delete(body);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BLE OTA ctl failed: %s", esp_err_to_name(err));
            return 0x83; /* application error: operation failed */
        }
        return 0;
    }

    if ((uintptr_t)arg == 6) {
        /* OTA data: raw firmware chunks, no JSON. A chunk landing while no
         * transfer is active (or a flash write error) is a 0x83; the sender
         * restarts the whole transfer. */
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        uint8_t buf[512];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > sizeof(buf) ||
            ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), NULL) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (ota_update_write(buf, len) != ESP_OK) {
            return 0x83;
        }
        return 0;
    }

    if ((uintptr_t)arg == 7) {
        /* HID control: media keys / key presses / text typing. Always present,
         * answers 0x82 when the usb_hid component is disabled so clients can
         * tell "not compiled in" from "flash error". */
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        char buf[192] = {0};
        if (OS_MBUF_PKTLEN(ctxt->om) >= sizeof(buf) ||
            ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, NULL) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (!usb_hid_ready()) {
            return 0x82; /* application error: HID not available */
        }
        cJSON *body = cJSON_Parse(buf);
        if (!body) {
            return 0x81;
        }
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(body, "type"));
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (type && strcmp(type, "text") == 0) {
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(body, "text"));
            if (text) {
                err = usb_hid_text(text);
            }
        } else if (type && strcmp(type, "media") == 0) {
            const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(body, "key"));
            if (key) {
                err = usb_hid_media_named(key);
            }
        } else if (type && strcmp(type, "key") == 0) {
            long usage = json_long(cJSON_GetObjectItem(body, "usage"));
            long modifier = json_long(cJSON_GetObjectItem(body, "modifier"));
            if (usage > 0 && usage <= 255 && modifier >= 0 && modifier <= 255) {
                err = usb_hid_key((uint8_t)usage, (uint8_t)modifier);
            }
        }
        cJSON_Delete(body);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BLE HID write failed: %s", esp_err_to_name(err));
            return err == ESP_ERR_NOT_SUPPORTED ? 0x82 : 0x83;
        }
        return 0;
    }

    if ((uintptr_t)arg == 8) {
        /* Wi-Fi provisioning: same JSON as GET/POST /api/wifi. A 512-byte ATT
         * payload covers a 32-byte SSID plus a 64-byte passphrase with room for
         * JSON escaping; anything longer is a malformed command, not a truncation
         * risk, because the store rejects over-length values anyway. */
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            char *json = net_config_status_json();
            if (!json) {
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            int rc = os_mbuf_append(ctxt->om, json, strlen(json));
            free(json);
            return rc;
        }
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        char buf[512] = {0};
        if (OS_MBUF_PKTLEN(ctxt->om) >= sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, NULL) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        cJSON *body = cJSON_Parse(buf);
        if (!body) {
            ESP_LOGW(TAG, "BLE wifi write rejected: bad JSON");
            return 0x81; /* application error: invalid command JSON */
        }
        const char *what = "(none)";
        const char *why = "rejected";
        bool ok = net_config_apply(body, &what, &why);
        cJSON_Delete(body);
        if (!ok) {
            ESP_LOGW(TAG, "BLE wifi write failed: %s: %s", what, why ? why : "?");
            return 0x81; /* application error: command rejected */
        }
        ESP_LOGI(TAG, "BLE wifi command <- %s", what);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def k_ble_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_ble_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = &k_ble_led_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)2,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &k_ble_status_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)3,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &k_ble_screen_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)4,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &k_ble_ota_ctl_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)5,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                /* Write-without-response keeps the transfer at full speed;
                 * writes-with-response still work for slow senders. */
                .uuid = &k_ble_ota_data_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)6,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &k_ble_hid_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)7,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &k_ble_wifi_uuid.u,
                .access_cb = ble_gatt_access,
                .arg = (void *)8,
                .flags = WIFI_CHR_FLAGS,
            },
            { 0 }, /* no more characteristics in this service */
        },
    },
    { 0 }, /* no more services */
};

static int ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    /* Flags are mandatory: general discoverability, BLE-only (the S3 has no
     * classic Bluetooth). The full name rides along in the same packet. */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)CONFIG_EXAMPLE_BLE_NAME;
    fields.name_len = strlen(CONFIG_EXAMPLE_BLE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE adv fields failed: %d (name too long?)", rc);
        return rc;
    }

    /* The 18-byte 128-bit service UUID does not fit next to the name in the
     * 31-byte legacy packet, so scanners find it in the scan response. */
    memset(&fields, 0, sizeof(fields));
    fields.uuids128 = &k_ble_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE scan-rsp fields failed: %d", rc);
        return rc;
    }

    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE adv start failed: %d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "BLE advertising as \"%s\"", CONFIG_EXAMPLE_BLE_NAME);
    return 0;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "BLE connect failed, status=%d", event->connect.status);
            ble_advertise();
        } else {
            /* MAX_CONNECTIONS=1: no advertising while a link is up. */
            ESP_LOGI(TAG, "BLE connected (conn=%u), advertising paused",
                     event->connect.conn_handle);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d), advertising again",
                 event->disconnect.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE mtu update: conn=%u mtu=%u",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void ble_on_sync(void)
{
    /* No privacy (0) → the controller's public address. */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE infer_addr failed: %d", rc);
        return;
    }
    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run(); /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Mirrors the official Wi-Fi/BLE coexistence example: Wi-Fi fully up first,
 * then NimBLE. Airtime sharing between the two radios is handled by the
 * ESP-IDF coexistence layer without any configuration here. */
void ble_gatt_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Security-manager fields stay zero: no pairing, plaintext characteristics.
     * Anyone in radio range can drive the LED -- the point of a demo; add
     * BLE_GATT_CHR_F_WRITE_ENC later to require encryption. */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(k_ble_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(k_ble_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE GATT registration failed: %d", rc);
        return;
    }
    rc = ble_svc_gap_device_name_set(CONFIG_EXAMPLE_BLE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE device name set failed: %d", rc);
        return;
    }
    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);
}

#endif /* CONFIG_EXAMPLE_BLE_ENABLE */
