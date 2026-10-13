#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "wifi_creds.h"

typedef enum {
    LINK_STATE_CONNECTING,     /* not associated */
    LINK_STATE_LINKED_NO_NET,  /* associated + IP, internet probe failed */
    LINK_STATE_ONLINE,         /* associated + IP + internet confirmed */
} link_state_t;

/* Snapshot of the station side, read live from the driver so a display can never
 * drift from what the chip is actually associated to. */
typedef struct {
    bool associated;
    int rssi;
    int channel;
    const char *auth;
    uint8_t sta_mac[6];
} link_state_ap_info_t;

/* Brings up NVS/netif/event loop/wi-fi, starts the connect and probe tasks, and
 * returns once the interfaces are running. Association itself stays async. */
void link_state_init(void);

link_state_t link_state_get(void);
const char *link_state_name(link_state_t state);
const char *link_state_auth_name(wifi_auth_mode_t mode);
bool link_state_has_ip(void);
bool link_state_internet(void);
uint32_t link_state_uptime_s(void);
void link_state_ip(char *out, size_t out_len);
bool link_state_sta_info(link_state_ap_info_t *out);
void link_state_ap_ssid(char *out, size_t out_len);

/* Devices currently joined to the board's own SoftAP; 0 when the fallback AP is
 * compiled out. Fills `clients` when the caller wants the per-device detail. */
int link_state_ap_clients(wifi_sta_list_t *clients);

/* SSID the connect ladder is currently pointed at (store, build-time fallback,
 * or empty when nothing is configured). Not the association report -- use
 * link_state_sta_info() for what the chip actually joined. */
void link_state_target_ssid(char *out, size_t out_len);

/* ---- Provisioning support -------------------------------------------------
 * The board's own network picker: one on-demand scan whose result a phone or
 * the web page reads back, plus the hook that moves the ladder to a newly
 * stored network. Both are independent of the ladder's pre-connect scan. */

typedef enum {
    LINK_SCAN_IDLE,
    LINK_SCAN_RUNNING,
    LINK_SCAN_DONE,
    LINK_SCAN_ERROR,
} link_scan_phase_t;

typedef struct {
    char ssid[WIFI_CRED_SSID_MAX + 1];
    int rssi;
    const char *auth; /* link_state_auth_name() of the strongest BSS */
} link_state_net_t;

/* Starts a scan in the background. False when one is already running. */
bool link_state_scan_start(void);
link_scan_phase_t link_state_scan_phase(void);
esp_err_t link_state_scan_error(void);

/* Copies the last completed scan (strongest signal first) into `out`, at most
 * 8 entries; returns how many. 0 while no result is available. */
int link_state_scan_results(link_state_net_t *out, int max);

/* Reload the stored credentials and re-associate with them, waking the ladder
 * even when it is sitting on a healthy link. */
void link_state_apply_new_config(void);
