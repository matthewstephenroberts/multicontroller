// config_store.h — NVS-backed, JSON-serialised sensor configuration.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max length (incl. NUL) of the BLE device name — kept short to comfortably fit the ~31-byte
// advertising packet alongside the flags/tx-power fields ble_svc.c also sets.
#define MC_DEVICE_NAME_LEN 20

// Load config from NVS (namespace "mcfg", key "sensors"). Seeds an empty config the
// first time. Must be called after nvs_flash_init().
esp_err_t config_store_init(void);

// Current monotonically-increasing config version (bumped on every successful save).
uint32_t config_store_version(void);

// Borrow the parsed sensor array. Pointer valid until the next set_json().
// Treat as read-only; copy under your own lock if you keep it across a poll cycle.
void config_store_get(const sensor_cfg_t **arr, size_t *count);

// Copy the current display config.
void config_store_get_display(display_cfg_t *out);

// Copy the current LEGO color-sensor emitter config.
void config_store_get_lego(lego_cfg_t *out);

// Current BLE device (advertised) name — never NULL/empty. Pointer valid until the next
// config_store_set_device_name() call.
const char *config_store_get_device_name(void);

// Rename the BLE device, persist to NVS, bump version. Takes effect on the GAP device-name
// characteristic immediately (see ble_svc_refresh_device_name()); the advertised name updates
// the next time advertising (re)starts (i.e. after the current connection, if any, disconnects).
// Returns ESP_ERR_INVALID_ARG for an empty name or one that doesn't fit MC_DEVICE_NAME_LEN.
esp_err_t config_store_set_device_name(const char *name);

// Overwrite sensor `id`'s calibration blob, persist to NVS, bump version. Returns
// ESP_ERR_NOT_FOUND if no sensor has that id.
esp_err_t config_store_set_calib(int id, const double *calib, int n);

// Overwrite sensor `id`'s learnable colour palette, persist to NVS, bump version. Returns
// ESP_ERR_NOT_FOUND if no sensor has that id.
esp_err_t config_store_set_colours(int id, const colour_ref_t *colours, int n);

// Factory reset: erase the stored config from NVS and restore board defaults (empty sensor
// list, default display/lego). Bumps version.
esp_err_t config_store_factory_reset(void);

// Snapshot sensor `id`'s current calib/colours as freshly malloc'd JSON array text (caller
// frees both with free()) — lets a calibrate/learn_colour/reset_colour/reset_sensor response
// hand back just the one sensor's captured data, so the client can patch it in locally instead
// of a full get_config refetch after every action. Either pointer comes back NULL on a bad id
// or OOM; the caller should omit that field rather than send an empty/wrong one.
void config_store_get_calib_colours_json(int id, char **calib_json, char **colours_json);

// Serialise the current config to a freshly malloc'd JSON string
// ({"version","display":{...},"sensors":[...]}). Caller frees. Returns NULL on OOM.
char *config_store_to_json(void);

// Replace config from a full config object string ({"sensors":[...],"display":{...}}),
// validate, persist to NVS, bump version. `display` is optional (unset fields keep their
// current values). On success writes *out_version. Returns ESP_ERR_INVALID_ARG on
// malformed/oversized input (existing config is left untouched).
esp_err_t config_store_set_json(const char *config_json, uint32_t *out_version);

// Get/set the BLE notification rate cap (minimum microseconds between notifications per sensor).
// Default is 20000 us (50Hz). Setting <= 0 resets to default.
int64_t config_store_get_polling_cap_us(void);
esp_err_t config_store_set_polling_cap_us(int64_t us);

// Get/set verbose sensor debug logging (default off): gates the throttled distance-sensor
// range diagnostics (drv_vl53l1x.c/drv_vl53l0x.c: raw device status + measured/reported
// distance, twice a second) and the BLE-HID controller's first-report hex dumps
// (hid_host.c — used to map a new controller's report layout). These were previously
// unconditional serial-log spam; toggle from the web Settings page instead of editing code.
bool config_store_get_verbose_debug(void);
esp_err_t config_store_set_verbose_debug(bool on);

// LEGO emitter serial-log verbosity (lego_cfg_t.events/debug) — split out of the full LEGO
// config's Save flow so it behaves like every other device debug toggle: takes effect
// immediately and persists on its own, no "Save to device" needed. Restarts the emitter task
// via lego_emit_apply() so the new verbosity applies to the very next hub transaction.
esp_err_t config_store_set_lego_debug(bool events, bool debug);

// BLE power settings: connection interval (1.25ms units), TX power (dBm), idle disconnect timeout (sec).
// Defaults: conn 48-96 (60-120ms), tx 0 (max), idle 0 (disabled).
// These are independent of sensor polling and take effect on the next BLE connection.
typedef struct {
    uint16_t conn_itvl_min;     // 1.25ms units (48 = 60ms default)
    uint16_t conn_itvl_max;     // 1.25ms units (96 = 120ms default)
    int8_t   tx_power;          // dBm (0 = max power, -4 to -20 for lower power)
    uint16_t idle_disconnect_s; // seconds (0 = disabled)
} ble_power_cfg_t;

ble_power_cfg_t config_store_get_ble_power(void);
esp_err_t config_store_set_ble_power(const ble_power_cfg_t *cfg);

// Whether the most recently queued background flash write actually succeeded. set_config's
// synchronous result only covers the RAM update + handing the snapshot to the async persist
// task — the real nvs_commit() happens afterwards, off the BLE-command path (see config_store.c
// persist_task). A caller can poll this after a save to detect a background persist failure
// that its own "ok:true" response wouldn't have seen yet.
bool config_store_last_persist_ok(void);

#ifdef __cplusplus
}
#endif
