// ble_internal.h — private interface between ble_svc.c (transport) and ble_protocol.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

// Handle one decoded JSON command, return a freshly malloc'd JSON response (caller frees),
// or NULL if there is nothing to send.
char *protocol_handle(const char *json_in);

// Build a freshly malloc'd JSON "reading" event for `r`. Caller frees.
char *protocol_reading_event(const reading_t *r);

// Build a freshly malloc'd JSON "lego_matrix" event from 9 RGB565 cells. Caller frees.
char *protocol_matrix_event(const uint16_t cells[9]);

// Build a freshly malloc'd JSON "hid" controller-status event. Caller frees.
char *protocol_hid_event(bool connected, const char *name);

// Subscription state for streamed reading events (owned by ble_svc, toggled by protocol).
void ble_svc_set_subscribed(bool on);
bool ble_svc_is_subscribed(void);

// Re-applies config_store's current device name to the GAP device-name characteristic — call
// after config_store_set_device_name() (see ble_svc.h for the full explanation).
void ble_svc_refresh_device_name(void);

// Per-sensor minimum time between BLE notifications (owned by ble_svc, set by protocol's
// set_polling_cap command). See ble_svc.h for the full explanation.
void ble_svc_set_notify_min_us(int64_t us);
int64_t ble_svc_get_notify_min_us(void);

#ifdef __cplusplus
}
#endif
