/*
 * Wi-Fi link monitor: association state on the onboard WS2812, live diagnostics
 * on a built-in web page.
 *
 * Everything is a component; this file only wires them together.
 */

#include "esp_log.h"
#include "link_state.h"
#include "status_light.h"
#include "monitor_server.h"
#include "i2c_probe.h"

static const char *TAG = "wifi_led";

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 Wi-Fi link monitor ===");

    link_state_init();
    status_light_start();
    i2c_probe_start();
    monitor_server_start();

    ESP_LOGI(TAG, "LED breathing blue = still connecting");
}
