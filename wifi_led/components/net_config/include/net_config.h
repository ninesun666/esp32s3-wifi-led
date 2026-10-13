#pragma once

#include <stdbool.h>
#include "cJSON.h"

/* The Wi-Fi configuration protocol, shared by the BLE characteristic and
 * POST /api/wifi so a phone and a browser cannot drift apart. One command per
 * call, answers as one JSON object; see docs/wifi_provisioning.md for the wire
 * format and the security notes. */

/* Snapshot of what the board is pointed at, plus the last provisioning scan.
 * cJSON-printed string the caller frees; NULL on allocation failure. The
 * passphrase is never included. */
char *net_config_status_json(void);

/* Applies {"cmd":"set|scan|forget", ...}. On rejection returns false and, when
 * provided, what_out names the command and why_out says why -- transports
 * surface these as their own error (HTTP 400 text, BLE 0x81). */
bool net_config_apply(const cJSON *body, const char **what_out, const char **why_out);
