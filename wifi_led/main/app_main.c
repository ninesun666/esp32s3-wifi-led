/*
 * Wi-Fi link monitor: association state on the onboard WS2812, live diagnostics
 * on a built-in web page.
 *
 * Everything is a component; this file only wires them together.
 */

#include "esp_log.h"
#include "link_state.h"
#include "status_light.h"
#include "display_tft.h"
#include "monitor_server.h"
#include "i2c_probe.h"
#include "ota_update.h"
#include "usb_hid.h"
#if CONFIG_EXAMPLE_BLE_ENABLE
#include "ble_gatt.h"
#endif

static const char *TAG = "wifi_led";

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 Wi-Fi link monitor ===");

    link_state_init();
    status_light_start();
    display_tft_start();
#if CONFIG_WIFI_LED_I2C_PROBE_ENABLE
    i2c_probe_start();
#endif
    /* HID must own GPIO19/20 before anything could touch them; no-op when the
     * feature is compiled off. */
    usb_hid_start();
    ota_update_init();
    monitor_server_start();
#if CONFIG_EXAMPLE_BLE_ENABLE
    ble_gatt_start();
#endif

    /* Booted from a just-flashed OTA slot: tell the bootloader this image
     * works, so a crash later does not roll back to the previous slot. */
    ota_update_confirm_running();

    ESP_LOGI(TAG, "LED breathing blue = still connecting");
}
