/*
 * 1.54" 240x240 IPS TFT on the ST7789 controller, 4-wire SPI.
 *
 * The panel driver is IDF's own esp_lcd ST7789 -- nothing is hand-rolled here
 * beyond the bus wiring and the strip buffer. Draws in horizontal strips because
 * a whole 240x240 RGB565 frame is 112 KiB and does not need to be DMA'd at once.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "display_tft.h"

static const char *TAG = "display_tft";

#define BYTES_PER_PIXEL 2
#define STRIP_ROWS      32
#define STRIP_PIXELS    (CONFIG_WIFI_LED_TFT_WIDTH * STRIP_ROWS)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_strip;
static int s_width = CONFIG_WIFI_LED_TFT_WIDTH;
static int s_height = CONFIG_WIFI_LED_TFT_HEIGHT;

bool display_tft_ready(void)
{
    return s_panel != NULL && s_strip != NULL;
}

int display_tft_width(void)
{
    return s_width;
}

int display_tft_height(void)
{
    return s_height;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);
}

bool display_tft_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!display_tft_ready()) {
        return false;
    }

    uint16_t px = rgb565(r, g, b);
    for (int i = 0; i < STRIP_PIXELS; i++) {
        s_strip[i] = px;
    }

    for (int y = 0; y < s_height; y += STRIP_ROWS) {
        int rows = s_height - y < STRIP_ROWS ? s_height - y : STRIP_ROWS;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_width, y + rows, s_strip);
    }
    return true;
}

static void backlight_on(void)
{
    /* The module's BLK floats and floats mean "off", so it has to be driven. */
    if (CONFIG_WIFI_LED_TFT_BLK_GPIO < 0) {
        return;
    }
    gpio_reset_pin(CONFIG_WIFI_LED_TFT_BLK_GPIO);
    gpio_set_direction(CONFIG_WIFI_LED_TFT_BLK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_WIFI_LED_TFT_BLK_GPIO, 1);
}

static esp_err_t bring_up(void)
{
    esp_err_t err;

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_WIFI_LED_TFT_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_WIFI_LED_TFT_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STRIP_PIXELS * BYTES_PER_PIXEL,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_WIFI_LED_TFT_DC_GPIO,
        .cs_gpio_num = CONFIG_WIFI_LED_TFT_CS_GPIO,
        .pclk_hz = CONFIG_WIFI_LED_TFT_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_WIFI_LED_TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* IPS panels of this family read inverted without it: colours come out as
     * their complements and a white fill looks black. */
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_strip = heap_caps_malloc(STRIP_PIXELS * BYTES_PER_PIXEL, MALLOC_CAP_DMA);
    return s_strip ? ESP_OK : ESP_ERR_NO_MEM;
}

void display_tft_start(void)
{
#ifndef CONFIG_WIFI_LED_TFT_ENABLE
    ESP_LOGI(TAG, "TFT support compiled out");
#else
    backlight_on();

    esp_err_t err = bring_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bring-up failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "ST7789 %dx%d on SCLK=%d MOSI=%d DC=%d CS=%d RST=%d at %d kHz",
             s_width, s_height, CONFIG_WIFI_LED_TFT_SCLK_GPIO, CONFIG_WIFI_LED_TFT_MOSI_GPIO,
             CONFIG_WIFI_LED_TFT_DC_GPIO, CONFIG_WIFI_LED_TFT_CS_GPIO,
             CONFIG_WIFI_LED_TFT_RST_GPIO, CONFIG_WIFI_LED_TFT_SPI_HZ / 1000);

    /* Three vertical bars: if the wiring is right they are red/green/blue in that
     * order left to right; a garbled or shifted pattern means the pin map is wrong. */
    uint16_t px;
    for (int y = 0; y < s_height; y += STRIP_ROWS) {
        int rows = s_height - y < STRIP_ROWS ? s_height - y : STRIP_ROWS;
        for (int i = 0; i < s_width * rows; i++) {
            int x = i % s_width;
            px = x < s_width / 3 ? rgb565(255, 0, 0)
                                 : (x < 2 * s_width / 3 ? rgb565(0, 255, 0)
                                                        : rgb565(0, 0, 255));
            s_strip[i] = px;
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_width, y + rows, s_strip);
    }
#endif
}
