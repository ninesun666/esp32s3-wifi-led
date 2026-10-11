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
    monitor_server_start();
#if CONFIG_EXAMPLE_BLE_ENABLE
    ble_gatt_start();
#endif

    ESP_LOGI(TAG, "LED breathing blue = still connecting");
}
