// bus_i2c.h — thin wrapper over the IDF i2c_master driver with per-address device caching.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the shared I2C master bus (pins/freq from board_config.h). Idempotent.
esp_err_t bus_i2c_init(void);

// Underlying bus handle (used by bus_scan's i2c_master_probe).
i2c_master_bus_handle_t bus_i2c_handle(void);

// Write `len` bytes to a 7-bit address.
esp_err_t bus_i2c_write(uint8_t addr, const uint8_t *data, size_t len);

// Write a register pointer then read `len` bytes (repeated-start).
esp_err_t bus_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

// Raw read of `len` bytes (no register write).
esp_err_t bus_i2c_read(uint8_t addr, uint8_t *buf, size_t len);

// ACK-probe a 7-bit address. ESP_OK = present.
esp_err_t bus_i2c_probe(uint8_t addr);

// Recursive lock guarding the bus. Hold this around a mux channel select plus the
// transaction(s) that depend on it, so no other task can re-route the mux in between.
// Safe to nest: bus_i2c_write/read/probe take the same lock internally.
void bus_i2c_lock(void);
void bus_i2c_unlock(void);

#ifdef __cplusplus
}
#endif
