#pragma once

#include <stdbool.h>
#include "cJSON.h"

/* Starts the HTTP server (port from Kconfig), publishes the board over mDNS, and
 * serves the monitor page plus /api/status and /api/led. */
void monitor_server_start(void);

/* The /api/status object as one malloc'd string (cJSON-printed); the caller
 * frees it. with_clients=false drops the hotspot client list, so a BLE client
 * can carry the whole payload in a single ATT read. */
char *monitor_server_status_json(bool with_clients);

/* Applies one /api/screen-style command: {"emote":"happy"} shows a face from
 * the asset pack, {"pattern":"bars"} the wiring bars, {"r","g","b"} a full-
 * panel colour fill. Shared by the HTTP handler and the BLE screen
 * characteristic. what/why are optional human-readable labels for logging. */
bool monitor_server_screen_apply(const cJSON *body, const char **what_out,
                                 const char **why_out);
