#pragma once

#include <stdbool.h>

/* Starts the HTTP server (port from Kconfig), publishes the board over mDNS, and
 * serves the monitor page plus /api/status and /api/led. */
void monitor_server_start(void);

/* The /api/status object as one malloc'd string (cJSON-printed); the caller
 * frees it. with_clients=false drops the hotspot client list, so a BLE client
 * can carry the whole payload in a single ATT read. */
char *monitor_server_status_json(bool with_clients);
