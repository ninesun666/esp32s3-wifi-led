#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    STATUS_FX_SOLID,
    STATUS_FX_BREATHE,
    STATUS_FX_RAINBOW,
    STATUS_FX_OFF,
} status_fx_t;

/* The manual takeover setting: manual=false renders the link-state colours,
 * manual=true renders fx with the stored colour/brightness. Shared by the web
 * /api/led endpoint and the BLE LED characteristic. */
typedef struct {
    bool manual;
    status_fx_t fx;
    uint8_t r, g, b;
    uint8_t brightness; /* 0..100 */
} status_light_ctl_t;

/* Creates the render task. Returns immediately; the strip is initialised on the
 * task's first run, so check status_light_ready() before believing a colour was
 * applied. */
void status_light_start(void);

void status_light_ctl_get(status_light_ctl_t *out);
void status_light_ctl_set(const status_light_ctl_t *in);

/* Parse and apply a partial takeover update. Accepts the same JSON as POST
 * /api/led; absent keys keep their current values, and any value outside its
 * range fails the whole write with *errmsg set. */
bool status_light_ctl_from_json(const char *body, const char **errmsg);

/* Serialises the current setting: {"manual":…,"fx":"…","rgb":[r,g,b],"brightness":…} */
int status_light_ctl_to_json(char *buf, size_t len);

bool status_light_ready(void);
