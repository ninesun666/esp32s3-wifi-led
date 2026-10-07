#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Creates the render task. Returns immediately; the strip is initialised on the
 * task's first run, so check status_light_ready() before believing a colour was
 * applied. */
void status_light_start(void);

/* Paint over the link colour for `hold_ms`, then hand control back to the link
 * state machine. `hold_ms` 0 clears a pending override without painting. */
void status_light_override(uint8_t r, uint8_t g, uint8_t b, uint32_t hold_ms);

bool status_light_ready(void);
