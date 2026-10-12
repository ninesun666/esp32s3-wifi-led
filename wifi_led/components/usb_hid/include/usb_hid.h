#pragma once

/*
 * USB HID: the OTG port shows up on the PC as a keyboard + consumer-control
 * device. Types here stay free of TinyUSB headers so callers (ble_gatt,
 * monitor_server) do not need the stack installed to compile.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Installs the TinyUSB driver. Compiles to a no-op returning ESP_OK when
 * CONFIG_WIFI_LED_USB_HID_ENABLE is off (GPIO19/20 stay on USB-Serial-JTAG). */
esp_err_t usb_hid_start(void);

/* True once the USB stack is mounted and the host is polling the interface. */
bool usb_hid_ready(void);

/* Type ASCII text (printable range + newline); other characters are skipped. */
esp_err_t usb_hid_text(const char *text);

/* Raw HID keyboard press: usage = USB HID Keyboard/Keypad page code
 * (e.g. 0x0F = 'L'), modifier bit mask (0x01 LCtrl .. 0x80 RGui). Presses and
 * releases in one call. */
esp_err_t usb_hid_key(uint8_t usage, uint8_t modifier);

/* Named consumer-control keys: "play", "pause", "next", "prev", "mute",
 * "volup", "voldown", and "lock" (Win/Mac lock-screen combo). */
esp_err_t usb_hid_media_named(const char *name);

/* Live USB/HID state for /api/status diagnostics. */
typedef struct {
    bool mounted;    /* tud_mounted(): host has configured the device */
    bool suspended;
    bool ready;      /* same as usb_hid_ready() */
    uint32_t send_ok;
    uint32_t send_fail;
} usb_hid_stats_t;

void usb_hid_stats(usb_hid_stats_t *out);
