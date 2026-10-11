#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_wifi.h"

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
