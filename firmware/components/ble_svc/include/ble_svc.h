// ble_svc.h — NimBLE GATT server (Nordic-UART-style) + framed JSON command transport.
#pragma once

#include "esp_err.h"
#include "scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start NimBLE: host stack, GATT service, advertising as config_store's device name (defaults
// to BOARD_BLE_NAME the first time the board ever boots, then whatever it's been renamed to).
esp_err_t ble_svc_init(void);

// Re-applies config_store's current device name to the GAP device-name characteristic — call
// after config_store_set_device_name() so a rename is visible immediately without a reconnect.
// The advertised name (what shows up in a BLE scan) picks up the change on its own the next
// time advertising restarts (i.e. after the current connection, if any, disconnects).
void ble_svc_refresh_device_name(void);

// reading_cb_t-compatible sink: frame a `reading` event and notify, if subscribed.
void ble_svc_on_reading(const reading_t *r);

// lego_emit matrix sink: frame a `lego_matrix` pixel event (9 RGB565 cells) and notify.
// Matches lego_emit_set_matrix_cb's signature; register it after lego_emit_init().
void ble_svc_on_matrix(const uint16_t cells[9]);

// hid_host status sink: frame a `hid` controller connect/disconnect event and notify.
// Matches hid_host_set_status_cb's signature; register it after hid_host_init().
void ble_svc_on_hid(bool connected, const char *name);

// True while a central is connected (for the status display).
bool ble_svc_is_connected(void);

// Check if BLE is enabled (vs. disabled by button hold).
bool ble_svc_is_enabled(void);

// Enable or disable Bluetooth (callable from display button handler).
void ble_svc_set_enabled(bool enable);

// Set/get the per-sensor minimum time between BLE notifications (in microseconds).
// Default is 20ms (50Hz). Setting <= 0 resets to default. Controls max polling rate to the client.
void ble_svc_set_notify_min_us(int64_t us);
int64_t ble_svc_get_notify_min_us(void);

#ifdef __cplusplus
}
#endif
