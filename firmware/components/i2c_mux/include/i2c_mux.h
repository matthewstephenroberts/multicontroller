// i2c_mux.h — TCA9548A 8-channel I2C multiplexer control.
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Select a single downstream channel (0..7) on the mux at `mux_addr` by writing the
// 1-byte bitmask (1<<channel). channel < 0 deselects all (writes 0x00).
// mux_addr == 0 is a no-op (sensor is wired directly on the main bus).
// The write is skipped when the mux is already on that channel (cached per mux).
esp_err_t i2c_mux_select(uint8_t mux_addr, int8_t channel);

// Drop the per-mux "current channel" cache so the next select really writes — call after
// driving the muxes outside i2c_mux_select, or when their state can no longer be trusted.
void i2c_mux_invalidate(void);

// Deselect all channels on every mux address (0x70..0x77) and mark the cache known. Call once
// after bus_i2c_init(): a mux keeps its channel selection across an ESP32 reset, so until each
// one has been written the cache cannot tell "no channel live" from "a channel left over from
// before the reboot" — and a stale live channel bridges two subtrees onto the shared bus.
void i2c_mux_reset_all(void);

// Convenience: select for the given config (handles direct vs muxed) before a transaction.
esp_err_t i2c_mux_route(uint8_t mux_addr, int8_t channel);

#ifdef __cplusplus
}
#endif
