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

#if CONFIG_WIFI_LED_TFT_EMOTE_ENABLE
#include "expression_emote.h"
#endif

static const char *TAG = "display_tft";

#define BYTES_PER_PIXEL 2
#define STRIP_ROWS      32
#define STRIP_PIXELS    (CONFIG_WIFI_LED_TFT_WIDTH * STRIP_ROWS)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_strip;
static int s_width = CONFIG_WIFI_LED_TFT_WIDTH;
static int s_height = CONFIG_WIFI_LED_TFT_HEIGHT;

#if CONFIG_WIFI_LED_TFT_EMOTE_ENABLE
static emote_handle_t s_emote;

/* The gfx lock is what the render task holds while it waits for a flush, so
 * taking it is also the cheapest way to prove no renderer transaction is in
 * flight before drawing over its face. */
static void panel_lock(void)
{
    if (s_emote) {
        emote_lock(s_emote);
    }
}

static void panel_unlock(void)
{
    if (s_emote) {
        emote_unlock(s_emote);
    }
}
#else
static void panel_lock(void)
{
}

static void panel_unlock(void)
{
}
#endif

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

    display_tft_emote_visible(false);

    uint16_t px = rgb565(r, g, b);
    for (int i = 0; i < STRIP_PIXELS; i++) {
        s_strip[i] = px;
    }

    panel_lock();
    for (int y = 0; y < s_height; y += STRIP_ROWS) {
        int rows = s_height - y < STRIP_ROWS ? s_height - y : STRIP_ROWS;
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_width, y + rows, s_strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fill strip at y=%d failed: %s", y, esp_err_to_name(err));
            break;
        }
    }
    panel_unlock();
    return true;
}

/* Three vertical bars: if the wiring is right they are red/green/blue in that
 * order left to right, and a shifted or garbled pattern means the pin map is
 * wrong. Repaintable on purpose -- it is the test you can run from the browser
 * instead of pressing the board's reset button. */
void display_tft_bars(void)
{
    if (!display_tft_ready()) {
        return;
    }

    display_tft_emote_visible(false);

    panel_lock();
    for (int y = 0; y < s_height; y += STRIP_ROWS) {
        int rows = s_height - y < STRIP_ROWS ? s_height - y : STRIP_ROWS;
        for (int i = 0; i < s_width * rows; i++) {
            int x = i % s_width;
            s_strip[i] = x < s_width / 3 ? rgb565(255, 0, 0)
                                         : (x < 2 * s_width / 3 ? rgb565(0, 255, 0)
                                                                : rgb565(0, 0, 255));
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_width, y + rows, s_strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bars strip at y=%d failed: %s", y, esp_err_to_name(err));
            break;
        }
    }
    panel_unlock();
}

#if CONFIG_WIFI_LED_TFT_EMOTE_ENABLE

/* The panel io announces *every* finished transaction, including the strip draws
 * started by display_tft_fill() and display_tft_bars(). The renderer wants exactly
 * one acknowledgement per flush it asks for, so the announces have to be sorted:
 * this is set only while a transaction came from emote_flush(). */
static volatile bool s_emote_awaits_ack;

/* The renderer hands over RGB565 chunks and blocks until the panel says the
 * bytes are gone, so the transfer-done callback is part of the contract, not
 * an optimisation -- forget it and the render task stalls on the first frame. */
static void emote_flush(int x_start, int y_start, int x_end, int y_end, const void *data,
                        emote_handle_t handle)
{
    s_emote_awaits_ack = true;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, data);
    if (err != ESP_OK) {
        /* No transaction went out, so no transfer-done will ever arrive to ack it.
         * Ack now anyway: the render task is waiting on this signal while holding
         * the gfx lock, and the HTTP handlers wait on that same lock. */
        s_emote_awaits_ack = false;
        ESP_LOGE(TAG, "emote flush %dx%d failed: %s", x_end - x_start, y_end - y_start,
                 esp_err_to_name(err));
        emote_notify_flush_finished(handle);
    }
}

static bool emote_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                             void *user_ctx)
{
    (void)io;
    (void)edata;
    if (s_emote_awaits_ack) {
        s_emote_awaits_ack = false;
        emote_notify_flush_finished((emote_handle_t)user_ctx);
    }
    return true;
}

static bool emote_bring_up(void)
{
    emote_config_t cfg = {
        /* swap: the S3 SPI path expects big-endian RGB565 on the wire. */
        .flags = { .swap = true, .double_buffer = true, .buff_dma = true },
        .gfx_emote = { .h_res = s_width, .v_res = s_height, .fps = 20 },
        .buffers = { .buf_pixels = (size_t)s_width * 16 },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = -1,
            .task_stack_in_ext = false,
        },
        .flush_cb = emote_flush,
    };

    s_emote = emote_init(&cfg);
    if (!s_emote) {
        ESP_LOGE(TAG, "emote_init failed");
        return false;
    }

    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = emote_trans_done };
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, s_emote);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flush-ready callback not registered: %s", esp_err_to_name(err));
        return false;
    }

    emote_data_t assets = {
        .type = EMOTE_SOURCE_PARTITION,
        .source = { .partition_label = CONFIG_WIFI_LED_TFT_EMOTE_PARTITION },
        .flags = { .mmap_enable = true },
    };
    err = emote_mount_and_load_assets(s_emote, &assets);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "assets from partition '%s' failed: %s -- flash the anim_icon partition",
                 CONFIG_WIFI_LED_TFT_EMOTE_PARTITION, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "emote ready: %dx%d, assets in partition %s",
             s_width, s_height, CONFIG_WIFI_LED_TFT_EMOTE_PARTITION);
    return true;
}

bool display_tft_emote_show(const char *name)
{
    if (!s_emote || !name) {
        return false;
    }

    /* Reject names the asset pack does not carry before touching the renderer, so
     * a typo in an API call answers with false instead of half-drawing. */
    emoji_data_t *emoji = NULL;
    if (emote_get_emoji_data_by_name(s_emote, name, &emoji) != ESP_OK || !emoji) {
        ESP_LOGW(TAG, "no such emote in the asset pack: %s", name);
        return false;
    }

    display_tft_emote_visible(true);
    return emote_set_anim_emoji(s_emote, name) == ESP_OK;
}

void display_tft_emote_visible(bool visible)
{
    if (s_emote) {
        emote_set_anim_visible(s_emote, visible);
    }
}

bool display_tft_emote_ready(void)
{
    return s_emote != NULL;
}

#else  /* !CONFIG_WIFI_LED_TFT_EMOTE_ENABLE */

bool display_tft_emote_show(const char *name)
{
    (void)name;
    return false;
}

bool display_tft_emote_ready(void)
{
    return false;
}

void display_tft_emote_visible(bool visible)
{
    (void)visible;
}

#endif

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
    s_io = io;
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

    display_tft_bars();

#if CONFIG_WIFI_LED_TFT_EMOTE_ENABLE
    if (emote_bring_up()) {
        display_tft_emote_show("happy");
    }
#endif
#endif
}
