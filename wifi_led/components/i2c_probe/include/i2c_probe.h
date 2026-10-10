#pragma once

#include <stdint.h>

/* Bring up I2C port 0 and keep scanning until something ACKs, so a display can be
 * wired in at any time without reflashing. Results are read by the monitor page. */
void i2c_probe_start(void);

/* Copy the 7-bit addresses that ACKed into out, returns how many there are. */
int i2c_probe_list(uint8_t *out, int max);
