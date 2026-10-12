/*
 * USB HID keyboard + consumer control on the OTG port.
 *
 * Report layout (one HID interface, report IDs):
 *   ID 1 -- boot-style 6KRO keyboard (modifier byte + 6 keycodes)
 *   ID 2 -- consumer control bitmap (next/prev/play-pause/mute/vol+/vol-)
 *
 * The configuration descriptor is hand-rolled and handed to
 * tinyusb_driver_install() so the interface layout does not depend on
 * esp_tinyusb's auto-generated composite descriptor.
 */

#include "sdkconfig.h"
#include "usb_hid.h"
#include "esp_log.h"
#include <string.h>

#if CONFIG_WIFI_LED_USB_HID_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb_hid";

/* Keyboard modifier bits */
#define MOD_LCTRL 0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT 0x04
#define MOD_LGUI 0x08

#define HID_ITF 0
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_CONSUMER 2

static const uint8_t k_report_desc[] = {
    /* Report ID 1: keyboard */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, REPORT_ID_KEYBOARD,
    0x05, 0x07,       /*   Usage Page (Key Codes) */
    0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,       /*   Input: modifier byte */
    0x95, 0x01, 0x75, 0x08,
    0x81, 0x01,       /*   Input: reserved byte */
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,       /*   Input: 6 keycode slots (array) */
    0x05, 0x08,       /*   Usage Page (LEDs) -- host->dev caps/num/scroll */
    0x19, 0x01, 0x29, 0x05,
    0x95, 0x05, 0x75, 0x01,
    0x91, 0x02,       /*   Output */
    0x95, 0x03, 0x75, 0x01,
    0x91, 0x03,       /*   Output: 3 bits padding */
    0xC0,

    /* Report ID 2: consumer control */
    0x05, 0x0C,       /* Usage Page (Consumer) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, REPORT_ID_CONSUMER,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01,
    0x09, 0xB5, 0x81, 0x02,  /*   Scan Next Track */
    0x09, 0xB6, 0x81, 0x02,  /*   Scan Previous Track */
    0x09, 0xCD, 0x81, 0x02,  /*   Play / Pause */
    0x09, 0xE2, 0x81, 0x02,  /*   Mute */
    0x09, 0xE9, 0x81, 0x02,  /*   Volume Up */
    0x09, 0xEA, 0x81, 0x02,  /*   Volume Down */
    0x95, 0x0A, 0x81, 0x03,  /*   10 bits padding */
    0xC0,
};

/* One interface: config(9) + interface(9) + HID(9) + endpoint IN(7). */
#define EP_SIZE 64
static uint8_t k_config_desc[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 100, /* 100 mA */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, sizeof(k_report_desc) & 0xFF,
        (sizeof(k_report_desc) >> 8) & 0xFF,
    0x07, 0x05, 0x81, 0x03, EP_SIZE & 0xFF, (EP_SIZE >> 8) & 0xFF, 0x01,
};

/* TinyUSB HID class callbacks (report descriptor + GET/SET_REPORT). */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf)
{
    (void)itf;
    return k_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)itf; (void)report_id; (void)report_type;
    memset(buffer, 0, reqlen);
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    /* Keyboard LED state arrives here; nothing to do with it. */
    (void)itf; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

/* Print-ASCII to (HID usage, shift flag), US layout, indexed by c - 0x20.
 * 0 = no mapping (skipped when typing). */
static const struct { uint8_t usage; uint8_t shift; } s_ascii[96] = {
    [' ' - 0x20] = { 0x2C, 0 },
    ['!' - 0x20] = { 0x1E, 1 }, ['"' - 0x20] = { 0x34, 1 },
    ['#' - 0x20] = { 0x20, 1 }, ['$' - 0x20] = { 0x21, 1 },
    ['%' - 0x20] = { 0x22, 1 }, ['&' - 0x20] = { 0x24, 1 },
    ['\'' - 0x20] = { 0x34, 0 }, ['(' - 0x20] = { 0x26, 1 },
    [')' - 0x20] = { 0x27, 1 }, ['*' - 0x20] = { 0x25, 1 },
    ['+' - 0x20] = { 0x2E, 1 }, [',' - 0x20] = { 0x36, 0 },
    ['-' - 0x20] = { 0x2D, 0 }, ['.' - 0x20] = { 0x37, 0 },
    ['/' - 0x20] = { 0x38, 0 },
    ['0' - 0x20] = { 0x27, 0 }, ['1' - 0x20] = { 0x1E, 0 },
    ['2' - 0x20] = { 0x1F, 0 }, ['3' - 0x20] = { 0x20, 0 },
    ['4' - 0x20] = { 0x21, 0 }, ['5' - 0x20] = { 0x22, 0 },
    ['6' - 0x20] = { 0x23, 0 }, ['7' - 0x20] = { 0x24, 0 },
    ['8' - 0x20] = { 0x25, 0 }, ['9' - 0x20] = { 0x26, 0 },
    [':' - 0x20] = { 0x33, 1 }, [';' - 0x20] = { 0x33, 0 },
    ['<' - 0x20] = { 0x36, 1 }, ['=' - 0x20] = { 0x2E, 0 },
    ['>' - 0x20] = { 0x37, 1 }, ['?' - 0x20] = { 0x38, 1 },
    ['@' - 0x20] = { 0x1F, 1 },
    ['A' - 0x20] = { 0x04, 1 }, ['B' - 0x20] = { 0x05, 1 },
    ['C' - 0x20] = { 0x06, 1 }, ['D' - 0x20] = { 0x07, 1 },
    ['E' - 0x20] = { 0x08, 1 }, ['F' - 0x20] = { 0x09, 1 },
    ['G' - 0x20] = { 0x0A, 1 }, ['H' - 0x20] = { 0x0B, 1 },
    ['I' - 0x20] = { 0x0C, 1 }, ['J' - 0x20] = { 0x0D, 1 },
    ['K' - 0x20] = { 0x0E, 1 }, ['L' - 0x20] = { 0x0F, 1 },
    ['M' - 0x20] = { 0x10, 1 }, ['N' - 0x20] = { 0x11, 1 },
    ['O' - 0x20] = { 0x12, 1 }, ['P' - 0x20] = { 0x13, 1 },
    ['Q' - 0x20] = { 0x14, 1 }, ['R' - 0x20] = { 0x15, 1 },
    ['S' - 0x20] = { 0x16, 1 }, ['T' - 0x20] = { 0x17, 1 },
    ['U' - 0x20] = { 0x18, 1 }, ['V' - 0x20] = { 0x19, 1 },
    ['W' - 0x20] = { 0x1A, 1 }, ['X' - 0x20] = { 0x1B, 1 },
    ['Y' - 0x20] = { 0x1C, 1 }, ['Z' - 0x20] = { 0x1D, 1 },
    ['[' - 0x20] = { 0x2F, 0 }, ['\\' - 0x20] = { 0x31, 0 },
    [']' - 0x20] = { 0x30, 0 }, ['^' - 0x20] = { 0x23, 1 },
    ['_' - 0x20] = { 0x2D, 1 }, ['`' - 0x20] = { 0x35, 0 },
    ['a' - 0x20] = { 0x04, 0 }, ['b' - 0x20] = { 0x05, 0 },
    ['c' - 0x20] = { 0x06, 0 }, ['d' - 0x20] = { 0x07, 0 },
    ['e' - 0x20] = { 0x08, 0 }, ['f' - 0x20] = { 0x09, 0 },
    ['g' - 0x20] = { 0x0A, 0 }, ['h' - 0x20] = { 0x0B, 0 },
    ['i' - 0x20] = { 0x0C, 0 }, ['j' - 0x20] = { 0x0D, 0 },
    ['k' - 0x20] = { 0x0E, 0 }, ['l' - 0x20] = { 0x0F, 0 },
    ['m' - 0x20] = { 0x10, 0 }, ['n' - 0x20] = { 0x11, 0 },
    ['o' - 0x20] = { 0x12, 0 }, ['p' - 0x20] = { 0x13, 0 },
    ['q' - 0x20] = { 0x14, 0 }, ['r' - 0x20] = { 0x15, 0 },
    ['s' - 0x20] = { 0x16, 0 }, ['t' - 0x20] = { 0x17, 0 },
    ['u' - 0x20] = { 0x18, 0 }, ['v' - 0x20] = { 0x19, 0 },
    ['w' - 0x20] = { 0x1A, 0 }, ['x' - 0x20] = { 0x1B, 0 },
    ['y' - 0x20] = { 0x1C, 0 }, ['z' - 0x20] = { 0x1D, 0 },
    ['{' - 0x20] = { 0x2F, 1 }, ['|' - 0x20] = { 0x31, 1 },
    ['}' - 0x20] = { 0x30, 1 }, ['~' - 0x20] = { 0x35, 1 },
    [0x7F - 0x20] = { 0x2A, 0 }, /* DEL -> Backspace */
};

esp_err_t usb_hid_start(void)
{
    tinyusb_config_t cfg = {
        .device_descriptor = NULL, /* esp_tinyusb default VID/PID + strings */
        .string_descriptor = NULL,
        .external_phy = false,
        .configuration_descriptor = k_config_desc,
    };
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB HID mounted (keyboard + consumer control)");
    return ESP_OK;
}

bool usb_hid_ready(void)
{
    return tud_ready() && tud_hid_n_ready(HID_ITF);
}

/* Send one report, waiting for the previous one to leave (endpoint interval).
 * Budget is generous (~200 ms): macOS can stall draining for tens of ms when
 * its input stack is busy, and a short budget aborts mid-string typing. */
static uint32_t s_send_ok, s_send_fail;

static bool hid_send(uint8_t report_id, const void *data, uint16_t len)
{
    for (int i = 0; i < 100; i++) {
        if (tud_hid_n_report(HID_ITF, report_id, data, len)) {
            s_send_ok++;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_send_fail++;
    return false;
}

void usb_hid_stats(usb_hid_stats_t *out)
{
    out->mounted = tud_mounted();
    out->suspended = tud_suspended();
    out->ready = usb_hid_ready();
    out->send_ok = s_send_ok;
    out->send_fail = s_send_fail;
}

esp_err_t usb_hid_key(uint8_t usage, uint8_t modifier)
{
    /* No ready() pre-check here: the endpoint stays flagged busy until the
     * host drains the previous report, so gating each key on ready() races
     * the host poll and fails back-to-back keystrokes. hid_send retries. */
    uint8_t down[8] = { modifier, 0, usage };
    uint8_t up[8] = { 0 };
    if (!hid_send(REPORT_ID_KEYBOARD, down, sizeof(down))) {
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    return hid_send(REPORT_ID_KEYBOARD, up, sizeof(up)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t usb_hid_text(const char *text)
{
    if (!usb_hid_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    for (const char *p = text; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '\n' || c == '\r') {
            err = usb_hid_key(0x28, 0); /* Enter */
            if (err != ESP_OK) {
                return err;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c < 0x20 || c > 0x7F || !s_ascii[c - 0x20].usage) {
            continue;
        }
        err = usb_hid_key(s_ascii[c - 0x20].usage,
                          s_ascii[c - 0x20].shift ? MOD_LSHIFT : 0);
        if (err != ESP_OK) {
            return err;
        }
        /* Let the host drain the release report before the next keystroke. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

esp_err_t usb_hid_media_named(const char *name)
{
    static const struct { const char *name; uint8_t bit; } media[] = {
        { "next", 1 << 0 }, { "prev", 1 << 1 }, { "play", 1 << 2 },
        { "pause", 1 << 2 }, { "mute", 1 << 3 }, { "volup", 1 << 4 },
        { "voldown", 1 << 5 },
    };
    if (!usb_hid_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(name, "lock") == 0) {
        return usb_hid_key(0x0F /* L */, MOD_LGUI);
    }
    for (size_t i = 0; i < sizeof(media) / sizeof(media[0]); i++) {
        if (strcmp(name, media[i].name) == 0) {
            /* 2 bytes: 6 usage bits + 10 padding bits, per the descriptor.
             * A 1-byte report made macOS drop the event silently. */
            uint8_t down[2] = { media[i].bit, 0 };
            uint8_t up[2] = { 0 };
            if (!hid_send(REPORT_ID_CONSUMER, down, sizeof(down))) {
                return ESP_ERR_TIMEOUT;
            }
            /* Consumer keys need a human-ish hold; 20 ms got debounced. */
            vTaskDelay(pdMS_TO_TICKS(120));
            return hid_send(REPORT_ID_CONSUMER, up, sizeof(up)) ? ESP_OK : ESP_ERR_TIMEOUT;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* !CONFIG_WIFI_LED_USB_HID_ENABLE */

esp_err_t usb_hid_start(void)
{
    return ESP_OK;
}

bool usb_hid_ready(void)
{
    return false;
}

esp_err_t usb_hid_text(const char *text)
{
    (void)text;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t usb_hid_key(uint8_t usage, uint8_t modifier)
{
    (void)usage; (void)modifier;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t usb_hid_media_named(const char *name)
{
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

void usb_hid_stats(usb_hid_stats_t *out)
{
    out->mounted = false;
    out->suspended = false;
    out->ready = false;
    out->send_ok = 0;
    out->send_fail = 0;
}

#endif /* CONFIG_WIFI_LED_USB_HID_ENABLE */
