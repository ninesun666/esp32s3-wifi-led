/*
 * I2C bus watchdog.
 *
 * The point of this component is to answer "is the external module wired and
 * talking" from the web page instead of from a serial cable -- opening the port
 * resets the board, which destroys the very state being observed.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "i2c_probe.h"

static const char *TAG = "i2c_probe";

#define ADDR_FIRST 0x08
#define ADDR_LAST  0x77
/* A probe on an address nobody owns has to time out, so this bounds the scan. */
#define PROBE_TIMEOUT_MS 30
#define RETRY_DELAY_MS   5000
#define MAX_FOUND        8

static i2c_master_bus_handle_t s_bus;
static uint8_t s_found[MAX_FOUND];
static int s_found_n;
static bool s_sda_high;
static bool s_scl_high;

int i2c_probe_list(uint8_t *out, int max)
{
    int n = s_found_n < max ? s_found_n : max;
    if (out && n > 0) {
        memcpy(out, s_found, n);
    }
    return n;
}

void i2c_probe_levels(bool *sda_high, bool *scl_high)
{
    if (sda_high) {
        *sda_high = s_sda_high;
    }
    if (scl_high) {
        *scl_high = s_scl_high;
    }
}

/* A free I2C bus idles HIGH. Measured with no pull of our own and nothing driven,
 * that says whether a module is even attached: an unconnected pin floats to a
 * random level, and a module with no supply cannot hold the line up either. This
 * is what separates a real slave from the phantom ACKs a floating bus produces. */
static void measure_idle_levels(void)
{
    const int pins[2] = { CONFIG_WIFI_LED_I2C_SDA_GPIO, CONFIG_WIFI_LED_I2C_SCL_GPIO };
    bool high[2];

    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    for (int i = 0; i < 2; i++) {
        high[i] = gpio_get_level(pins[i]) == 1;
    }
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(pins[i]);
    }

    s_sda_high = high[0];
    s_scl_high = high[1];
    ESP_LOGI(TAG, "idle levels with no pull-up: SDA=%d SCL=%d (%s)",
             s_sda_high, s_scl_high,
             (s_sda_high && s_scl_high) ? "pulled up externally"
                                        : "floating or held low -- nothing attached, or mis-wired");
}

static void describe(uint8_t addr)
{
    switch (addr) {
    case 0x3C:
    case 0x3D:
        ESP_LOGI(TAG, "0x%02x -> SSD1306/SH1106 class display (0x3D = the SA0/DC pin is high)", addr);
        break;
    case 0x34:
    case 0x36:
        ESP_LOGI(TAG, "0x%02x -> SH1106 alternative address", addr);
        break;
    default:
        break;
    }
}

static void collect(uint8_t *out, int *n)
{
    *n = 0;
    for (uint8_t a = ADDR_FIRST; a <= ADDR_LAST; a++) {
        if (i2c_master_probe(s_bus, a, PROBE_TIMEOUT_MS) == ESP_OK) {
            if (*n < MAX_FOUND) {
                out[(*n)++] = a;
            }
            ESP_LOGD(TAG, "ACK from 0x%02x", a);
        }
    }
}

static bool in_list(const uint8_t *list, int n, uint8_t addr)
{
    for (int i = 0; i < n; i++) {
        if (list[i] == addr) {
            return true;
        }
    }
    return false;
}

static void probe_task(void *arg)
{
    uint8_t prev[MAX_FOUND];
    int prev_n = 0;

    /* An unconnected bus floats and ACKs at random addresses, so an address only
     * counts once it repeats across two scans. */
    while (s_found_n == 0) {
        uint8_t now[MAX_FOUND];
        int now_n = 0;
        collect(now, &now_n);

        for (int i = 0; i < now_n; i++) {
            if (in_list(prev, prev_n, now[i]) && s_found_n < MAX_FOUND) {
                s_found[s_found_n++] = now[i];
                ESP_LOGI(TAG, "0x%02x ACKed twice -> real device", now[i]);
                describe(now[i]);
            }
        }

        if (s_found_n == 0) {
            memcpy(prev, now, now_n);
            prev_n = now_n;
            ESP_LOGI(TAG, "nothing stable on SDA=%d SCL=%d (%d flaky ACK%s), retry in %d s "
                          "(check VCC/GND and that the module is in I2C mode)",
                     CONFIG_WIFI_LED_I2C_SDA_GPIO, CONFIG_WIFI_LED_I2C_SCL_GPIO,
                     now_n, now_n == 1 ? "" : "s", RETRY_DELAY_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
    }
    vTaskDelete(NULL);
}

void i2c_probe_start(void)
{
    measure_idle_levels();

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_WIFI_LED_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_WIFI_LED_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Weak (~45k) and only good enough for a short jumper-wired bus at
         * 100 kHz; proper 4.7k pull-ups beat it. */
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init on SDA=%d SCL=%d failed: %s",
                 cfg.sda_io_num, cfg.scl_io_num, esp_err_to_name(err));
        return;
    }
    xTaskCreate(probe_task, "i2c_probe", 3072, NULL, 3, NULL);
}
