#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bring up I2C port 0 and keep scanning until something ACKs, so a display can be
 * wired in at any time without reflashing. Results are read by the monitor page. */
void i2c_probe_start(void);

/* Copy the 7-bit addresses that ACKed into out, returns how many there are. */
int i2c_probe_list(uint8_t *out, int max);

/* Line levels measured at boot with no pull-up of our own. A connected, powered
 * module holds both high; a floating pair reads random. */
void i2c_probe_levels(bool *sda_high, bool *scl_high);
