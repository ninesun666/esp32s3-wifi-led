#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Firmware update state machine shared by POST /api/ota and the BLE OTA
 * characteristics. One update at a time: begin refuses while busy. The new
 * image is only made bootable after size and (if given) sha256 verify, so a
 * broken transfer leaves the running system untouched. */

void ota_update_init(void);

bool ota_update_busy(void);

esp_err_t ota_update_begin(size_t total, bool has_sha, const uint8_t sha[32]);

esp_err_t ota_update_write(const void *data, size_t len);

esp_err_t ota_update_finish(void);

esp_err_t ota_update_abort(void);

void ota_update_progress(size_t *written, size_t *total, bool *busy);

/* Reply to the client first, restart one second later. */
void ota_update_reboot_later(void);

/* A freshly OTA'd image calls this once it is fully up; without it the
 * bootloader rolls back to the previous slot. */
void ota_update_confirm_running(void);
