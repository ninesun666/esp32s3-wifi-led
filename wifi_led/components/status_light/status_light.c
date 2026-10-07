/*
 * Status light: renders the published link state on the onboard WS2812, and lets
 * an external caller paint over it for a bounded time.
 *
 * Runs its own task at 50 Hz because led_strip_refresh() blocks on the RMT
 * transmission, and the breathing ramp needs a frame rate.
 */

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "link_state.h"
#include "status_light.h"

static const char *TAG = "status_light";

static volatile bool s_ready = false;
static volatile uint8_t s_manual_r, s_manual_g, s_manual_b;
static volatile int64_t s_manual_until_us = 0;

#if CONFIG_WIFI_LED_MONO_GPIO >= 0
#define MONO_ON()  gpio_set_level(CONFIG_WIFI_LED_MONO_GPIO, 1)
#define MONO_OFF() gpio_set_level(CONFIG_WIFI_LED_MONO_GPIO, 0)

static void mono_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_WIFI_LED_MONO_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}
#else
#define MONO_ON()  do {} while (0)
#define MONO_OFF() do {} while (0)
static void mono_led_init(void) {}
#endif

void status_light_override(uint8_t r, uint8_t g, uint8_t b, uint32_t hold_ms)
{
    /* Colour first, deadline last: a renderer that samples between the two writes
     * sees the old deadline with the old colour, never a new colour with no
     * deadline. */
    s_manual_r = r;
    s_manual_g = g;
    s_manual_b = b;
    s_manual_until_us = hold_ms ? esp_timer_get_time() + (int64_t)hold_ms * 1000 : 0;
}

bool status_light_ready(void)
{
    return s_ready;
}

static void led_task(void *arg)
{
    led_strip_handle_t strip = NULL;
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_WIFI_LED_RGB_GPIO,
        .max_leds = CONFIG_WIFI_LED_RGB_COUNT,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %s (is GPIO %d the right pin?)",
                 esp_err_to_name(err), CONFIG_WIFI_LED_RGB_GPIO);
        vTaskDelete(NULL);
        return;
    }
    led_strip_clear(strip);
    s_ready = true;
    ESP_LOGI(TAG, "WS2812 ready on GPIO %d", CONFIG_WIFI_LED_RGB_GPIO);

    const float pi = 3.14159265f;
    uint32_t tick = 0;
    link_state_t shown = LINK_STATE_CONNECTING;
    int devices_shown = 0;

    while (true) {
        link_state_t state = link_state_get();
        uint8_t r = 0, g = 0, b = 0;

        /* Once a second, not every frame: this loop refreshes the strip at 50 Hz. */
        if (tick % 50 == 0) {
            int d = link_state_ap_clients(NULL);
            if (d != devices_shown) {
                ESP_LOGI(TAG, "hotspot devices: %d", d);
                devices_shown = d;
            }
        }

        switch (state) {
        case LINK_STATE_CONNECTING: {
            float phase = sinf(tick * pi / 60.0f);
            b = (uint8_t)((phase + 1.0f) * 0.5f * 200.0f);
            break;
        }
        case LINK_STATE_LINKED_NO_NET:
            /* joined the router, internet probe failed */
            g = 120;
            break;
        case LINK_STATE_ONLINE:
            /* router and internet both confirmed */
            g = 120;
            b = 120;
            break;
        }

        /* Something attached to the board's own hotspot outranks the WAN colour,
         * which is the "有设备接入就变色" behaviour asked for. */
        if (devices_shown > 0) {
            r = 140;
            b = 140;
            g = 0;
        }

        if (esp_timer_get_time() < s_manual_until_us) {
            r = s_manual_r;
            g = s_manual_g;
            b = s_manual_b;
        }

        for (int i = 0; i < CONFIG_WIFI_LED_RGB_COUNT; i++) {
            led_strip_set_pixel(strip, i, r, g, b);
        }
        led_strip_refresh(strip);

        if (state != shown) {
            ESP_LOGI(TAG, "LED state -> %s (r=%u g=%u b=%u)",
                     state == LINK_STATE_CONNECTING ? "CONNECTING/breathing-blue"
                     : state == LINK_STATE_LINKED_NO_NET ? "LINKED_NO_NET/green"
                     : "ONLINE/cyan",
                     (unsigned)r, (unsigned)g, (unsigned)b);
            shown = state;
        }

        /* Nothing above ever fires while the link stays in CONNECTING, so this is
         * the only proof that RMT is driving the strip rather than having failed
         * to init or been given the wrong GPIO. */
        if (tick % 500 == 0) {
            ESP_LOGI(TAG, "WS2812 alive on GPIO %d, driving rgb=(%u,%u,%u)",
                     CONFIG_WIFI_LED_RGB_GPIO, (unsigned)r, (unsigned)g, (unsigned)b);
        }

        if (state == LINK_STATE_CONNECTING) {
            if (tick % 15 == 0) { MONO_ON(); } else { MONO_OFF(); }
        } else {
            MONO_OFF();
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void status_light_start(void)
{
    mono_led_init();
    xTaskCreate(led_task, "led", 4096, NULL, 5, NULL);
}
