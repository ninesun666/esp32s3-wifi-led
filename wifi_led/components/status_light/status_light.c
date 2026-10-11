/*
 * Status light: renders the published link state on the onboard WS2812, unless a
 * manual takeover (web page or BLE) is in effect, in which case the stored
 * effect wins until the caller hands control back.
 *
 * Runs its own task at 50 Hz because led_strip_refresh() blocks on the RMT
 * transmission, and the breathing ramp needs a frame rate.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "cJSON.h"
#include "link_state.h"
#include "status_light.h"

static const char *TAG = "status_light";

static volatile bool s_ready = false;

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

/* Written by the httpd and NimBLE tasks, read by led_task; copied whole under
 * the mutex. The setting lives in RAM only -- a reboot returns to auto. */
static SemaphoreHandle_t s_ctl_mux;
static status_light_ctl_t s_ctl = {
    .manual = false, .fx = STATUS_FX_SOLID,
    .r = 255, .g = 255, .b = 255, .brightness = 50,
};

void status_light_ctl_get(status_light_ctl_t *out)
{
    xSemaphoreTake(s_ctl_mux, portMAX_DELAY);
    *out = s_ctl;
    xSemaphoreGive(s_ctl_mux);
}

void status_light_ctl_set(const status_light_ctl_t *in)
{
    xSemaphoreTake(s_ctl_mux, portMAX_DELAY);
    s_ctl = *in;
    xSemaphoreGive(s_ctl_mux);
}

static const char *fx_name(status_fx_t fx)
{
    switch (fx) {
    case STATUS_FX_SOLID:   return "solid";
    case STATUS_FX_BREATHE: return "breathe";
    case STATUS_FX_RAINBOW: return "rainbow";
    default:                return "off";
    }
}

static bool fx_from_name(const char *name, status_fx_t *out)
{
    if (!strcmp(name, "solid"))   { *out = STATUS_FX_SOLID;   return true; }
    if (!strcmp(name, "breathe")) { *out = STATUS_FX_BREATHE; return true; }
    if (!strcmp(name, "rainbow")) { *out = STATUS_FX_RAINBOW; return true; }
    if (!strcmp(name, "off"))     { *out = STATUS_FX_OFF;     return true; }
    return false;
}

bool status_light_ctl_from_json(const char *body, const char **errmsg)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        *errmsg = "invalid JSON";
        return false;
    }

    status_light_ctl_t ctl;
    status_light_ctl_get(&ctl);
    bool ok = true;

    cJSON *item = cJSON_GetObjectItem(root, "manual");
    if (cJSON_IsBool(item)) {
        ctl.manual = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItem(root, "fx");
    if (cJSON_IsString(item)) {
        ok = fx_from_name(item->valuestring, &ctl.fx);
    } else if (item) {
        ok = false;
    }
    item = cJSON_GetObjectItem(root, "rgb");
    if (cJSON_IsArray(item) && cJSON_GetArraySize(item) == 3) {
        uint8_t *dst[3] = { &ctl.r, &ctl.g, &ctl.b };
        int idx = 0;
        cJSON *ch = NULL;
        cJSON_ArrayForEach(ch, item) {
            if (!cJSON_IsNumber(ch) || ch->valuedouble < 0 || ch->valuedouble > 255) {
                ok = false;
                break;
            }
            *dst[idx++] = (uint8_t)ch->valuedouble;
        }
    } else if (item) {
        ok = false;
    }
    item = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0 || item->valuedouble > 100) {
            ok = false;
        } else {
            ctl.brightness = (uint8_t)item->valuedouble;
        }
    }
    cJSON_Delete(root);

    if (!ok) {
        *errmsg = "fx must be solid|breathe|rainbow|off, rgb 0-255, brightness 0-100";
        return false;
    }

    status_light_ctl_set(&ctl);
    ESP_LOGI(TAG, "LED control: %s fx=%s rgb=(%u,%u,%u) brightness=%u",
             ctl.manual ? "manual" : "auto", fx_name(ctl.fx),
             ctl.r, ctl.g, ctl.b, ctl.brightness);
    return true;
}

int status_light_ctl_to_json(char *buf, size_t len)
{
    status_light_ctl_t ctl;
    status_light_ctl_get(&ctl);
    return snprintf(buf, len,
                    "{\"manual\":%s,\"fx\":\"%s\",\"rgb\":[%u,%u,%u],\"brightness\":%u}",
                    ctl.manual ? "true" : "false", fx_name(ctl.fx),
                    ctl.r, ctl.g, ctl.b, ctl.brightness);
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
    bool shown_manual = false;

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

        status_light_ctl_t ctl;
        status_light_ctl_get(&ctl);
        if (ctl.manual != shown_manual) {
            ESP_LOGI(TAG, "LED control -> %s",
                     ctl.manual ? "manual (web/BLE override)" : "auto (link state)");
            shown_manual = ctl.manual;
        }

        if (ctl.manual) {
            switch (ctl.fx) {
            case STATUS_FX_SOLID:
                r = ctl.r;
                g = ctl.g;
                b = ctl.b;
                break;
            case STATUS_FX_BREATHE: {
                float phase = sinf(tick * pi / 60.0f);
                float k = (phase + 1.0f) * 0.5f * ctl.brightness / 100.0f;
                r = (uint8_t)(ctl.r * k);
                g = (uint8_t)(ctl.g * k);
                b = (uint8_t)(ctl.b * k);
                break;
            }
            case STATUS_FX_RAINBOW: /* resolved per pixel below */
            case STATUS_FX_OFF:
                break;
            }
        } else {
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
        }

        uint8_t hue = (uint8_t)(tick * 2);
        for (int i = 0; i < CONFIG_WIFI_LED_RGB_COUNT; i++) {
            if (ctl.manual && ctl.fx == STATUS_FX_RAINBOW) {
                /* set_pixel_hsv() wants hue in [0,360); spread it across the
                 * strip so multi-LED setups get a travelling rainbow. */
                uint16_t h = ((uint32_t)hue * 360 / 256
                              + i * 360 / CONFIG_WIFI_LED_RGB_COUNT) % 360;
                led_strip_set_pixel_hsv(strip, i, h, 100, ctl.brightness * 255 / 100);
            } else {
                led_strip_set_pixel(strip, i, r, g, b);
            }
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
    s_ctl_mux = xSemaphoreCreateMutex();
    mono_led_init();
    xTaskCreate(led_task, "led", 4096, NULL, 5, NULL);
}
