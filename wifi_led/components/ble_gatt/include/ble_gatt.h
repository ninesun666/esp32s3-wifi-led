#pragma once

/* Brings up the NimBLE host and starts advertising the LED/status GATT service.
 * Call once, after the Wi-Fi interfaces are running (link_state_init has
 * returned); Wi-Fi/BLE airtime sharing is handled by the IDF coexistence layer.
 * Only registered into the build when CONFIG_EXAMPLE_BLE_ENABLE is set. */
void ble_gatt_start(void);
