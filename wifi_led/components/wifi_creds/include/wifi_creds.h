#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

/* Runtime-changeable Wi-Fi credentials. See wifi_creds.c for the precedence
 * rules; this header is the only place the store is described. */

/* 802.11 limits, which are also the wifi_config_t field sizes. */
#define WIFI_CRED_SSID_MAX 32
#define WIFI_CRED_PASS_MAX 64

typedef enum {
    WIFI_CREDS_OK = 0,
    WIFI_CREDS_BAD_SSID,       /* empty, or longer than the 32 bytes 802.11 allows */
    WIFI_CREDS_BAD_PASSWORD,   /* not empty and not 8..64 bytes */
    WIFI_CREDS_STORE_FAILED,   /* NVS open/commit failed */
} wifi_creds_err_t;

/* "stored" (NVS), "build" (Kconfig fallback) or "none". */
const char *wifi_creds_source(void);
bool wifi_creds_has_stored(void);

/* Effective credentials, store first then build-time fallback. Empty ssid means
 * "nothing configured". Pass may be an empty string for an open network. */
void wifi_creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* One-time copy of real build-time credentials into the store (no-op when the
 * store is filled or the build still carries the CHANGE_ME template), so a
 * board that only ever knew sdkconfig.local keeps its network across an OTA
 * to a credential-free CI image. */
void wifi_creds_seed_from_build(void);

/* False when the effective network is open (no passphrase). The passphrase
 * itself never leaves this component -- callers get this instead of a getter. */
bool wifi_creds_has_password(void);

wifi_creds_err_t wifi_creds_validate(const char *ssid, const char *pass);
wifi_creds_err_t wifi_creds_save(const char *ssid, const char *pass);
wifi_creds_err_t wifi_creds_clear(void);

const char *wifi_creds_err_name(wifi_creds_err_t err);
