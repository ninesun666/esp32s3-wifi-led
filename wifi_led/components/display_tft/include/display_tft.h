#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bring up the SPI TFT (IDF's own esp_lcd ST7789 driver) and paint a test pattern.
 * Returns without doing anything when CONFIG_WIFI_LED_TFT_ENABLE is off. */
void display_tft_start(void);

bool display_tft_ready(void);
int display_tft_width(void);
int display_tft_height(void);

/* Fill the whole panel with one colour; false when the panel is not up. */
bool display_tft_fill(uint8_t r, uint8_t g, uint8_t b);
