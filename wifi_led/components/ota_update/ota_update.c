/*
 * OTA state machine over esp_ota_ops: exactly one update at a time, guarded by
 * a mutex, verified by size and optional sha256 before the boot slot flips.
 * Both POST /api/ota and BLE characteristics 0005/0006 drive it.
 */

#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#include "ota_update.h"

static const char *TAG = "ota_update";

static SemaphoreHandle_t s_lock;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static size_t s_total;
static size_t s_written;
static size_t s_since_yield;
static bool s_busy;
static bool s_has_sha;
static uint8_t s_sha_expect[32];
static mbedtls_sha256_context s_sha_ctx;
static esp_timer_handle_t s_reboot_timer;

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void ota_update_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    const esp_timer_create_args_t timer_args = {
        .callback = reboot_cb,
        .name = "ota_reboot",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reboot_timer));
}

bool ota_update_busy(void)
{
    return s_busy;
}

esp_err_t ota_update_begin(size_t total, bool has_sha, const uint8_t sha[32])
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (total > target->size) {
        ESP_LOGW(TAG, "image %zu bytes does not fit %s (%lu bytes)",
                 total, target->label, (unsigned long)target->size);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Sequential writes erase sector-by-sector during the transfer instead of
     * one long blocking erase up front, which keeps BLE and Wi-Fi responsive
     * while an image lands over the air. */
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_lock);
        return err;
    }

    s_target = target;
    s_total = total;
    s_written = 0;
    s_since_yield = 0;
    s_busy = true;
    s_has_sha = has_sha;
    if (has_sha) {
        memcpy(s_sha_expect, sha, sizeof(s_sha_expect));
        mbedtls_sha256_init(&s_sha_ctx);
        mbedtls_sha256_starts(&s_sha_ctx, 0);
    }

    ESP_LOGI(TAG, "ota begin: slot %s, %zu bytes%s", target->label, total,
             has_sha ? ", sha256 checked" : "");
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ota_update_write(const void *data, size_t len)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_total && s_written + len > s_total) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err == ESP_OK) {
        if (s_has_sha) {
            mbedtls_sha256_update(&s_sha_ctx, data, len);
        }
        s_written += len;
        s_since_yield += len;
        if (s_since_yield >= 8192) {
            s_since_yield = 0;
            vTaskDelay(1);
        }
    } else {
        ESP_LOGE(TAG, "ota write failed: %s", esp_err_to_name(err));
        esp_ota_abort(s_handle);
        s_busy = false;
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ota_update_finish(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_total && s_written != s_total) {
        ESP_LOGW(TAG, "transfer truncated: %zu of %zu bytes", s_written, s_total);
        esp_ota_abort(s_handle);
        s_busy = false;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_has_sha) {
        uint8_t got[32];
        mbedtls_sha256_finish(&s_sha_ctx, got);
        mbedtls_sha256_free(&s_sha_ctx);
        if (memcmp(got, s_sha_expect, sizeof(got)) != 0) {
            ESP_LOGW(TAG, "sha256 mismatch, keeping the current slot");
            esp_ota_abort(s_handle);
            s_busy = false;
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_CRC;
        }
    }

    esp_err_t err = esp_ota_end(s_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "image rejected by esp_ota_end: %s", esp_err_to_name(err));
        s_busy = false;
        xSemaphoreGive(s_lock);
        return err;
    }

    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        s_busy = false;
        xSemaphoreGive(s_lock);
        return err;
    }

    ESP_LOGI(TAG, "boot slot set to %s (%zu bytes)", s_target->label, s_written);
    s_busy = false;
    s_target = NULL;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ota_update_abort(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (s_busy) {
        err = esp_ota_abort(s_handle);
        s_busy = false;
    }

    xSemaphoreGive(s_lock);
    return err;
}

void ota_update_progress(size_t *written, size_t *total, bool *busy)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (written) {
        *written = s_written;
    }
    if (total) {
        *total = s_total;
    }
    if (busy) {
        *busy = s_busy;
    }
    xSemaphoreGive(s_lock);
}

void ota_update_reboot_later(void)
{
    esp_err_t err = esp_timer_start_once(s_reboot_timer, 1000000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reboot timer: %s", esp_err_to_name(err));
    }
}

void ota_update_confirm_running(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "running image confirmed valid (%s)", esp_err_to_name(err));
    }
#endif
}
