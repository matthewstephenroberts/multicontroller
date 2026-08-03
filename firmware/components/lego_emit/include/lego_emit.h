// lego_emit.h — emit MultiController sensor readings to a LEGO hub as a fake Color Sensor.
//
// Wraps the LPF2 protocol engine (lpf2.h, C++) behind a C API so the C firmware can use
// it. The emitter runs its own FreeRTOS task: it services the LPF2 handshake/keepalive on
// a dedicated UART, packs the latest readings (fed via lego_emit_update) into the Color
// Sensor's 4×uint16 RGBI payload per the configured bit-field map, and replies to the hub.
//
//   main.c:  lego_emit_init() once → lego_emit_start()
//            scheduler reading callback → lego_emit_update(id, values, n)
//            set_config handler → lego_emit_apply(cfg)
#pragma once

#include "esp_err.h"
#include "sensor.h"        // lego_cfg_t, lego_field_t
#include "scheduler.h"     // reading_t

#ifdef __cplusplus
extern "C" {
#endif

// Create the emitter task (idle until a config with enabled=true is applied) and load the
// current config from config_store. Call once after scheduler_init().
esp_err_t lego_emit_init(void);

// Re-read the LEGO config from config_store and (re)start or stop the UART accordingly.
// Call after a set_config that may have changed the lego object.
void lego_emit_apply(void);

// reading_cb_t-compatible sink: cache the latest values of sensor r->id so the emitter
// task can pack them. Cheap and lock-guarded; safe to call from the scheduler task.
void lego_emit_on_reading(const reading_t *r);

// True while the LEGO hub handshake has completed (for the status display / diagnostics).
bool lego_emit_is_connected(void);

// Register a sink for decoded 3×3 Light Matrix pixels (9 RGB565 cells), called when the hub
// WRITEs to the matrix profile's PIX mode. Used to forward pixels to the frontend over BLE.
// Pass NULL to clear. (The onboard TFT is driven directly via display_show_matrix.)
void lego_emit_set_matrix_cb(void (*cb)(const uint16_t cells[9]));

#ifdef __cplusplus
}
#endif
