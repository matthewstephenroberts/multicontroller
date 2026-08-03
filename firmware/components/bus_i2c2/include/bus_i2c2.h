// bus_i2c2.h — second I2C master bus, for onboard peripherals that live on a separate physical
// bus from the external Port.A connector (see bus_i2c.h). On the M5Stack AtomS3R this is the
// internal bus (SDA=G45, SCL=G0) shared by the LP5562 backlight driver and the onboard
// BMI270/BMM150 IMU — board_config.h's BOARD_TFT_BL_I2C_* macros. Same shape as bus_i2c.h
// (per-address device caching, recursive lock) so callers already familiar with that one need
// nothing new.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the second I2C master bus (pins/freq from board_config.h's BOARD_TFT_BL_I2C_* macros).
// Idempotent — safe to call from multiple independent owners (display.c's backlight driver, the
// imu component) without coordinating who goes first.
esp_err_t bus_i2c2_init(void);

// Underlying bus handle.
i2c_master_bus_handle_t bus_i2c2_handle(void);

// Write `len` bytes to a 7-bit address.
esp_err_t bus_i2c2_write(uint8_t addr, const uint8_t *data, size_t len);

// Write a register pointer then read `len` bytes (repeated-start).
esp_err_t bus_i2c2_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

// Raw read of `len` bytes (no register write).
esp_err_t bus_i2c2_read(uint8_t addr, uint8_t *buf, size_t len);

// ACK-probe a 7-bit address. ESP_OK = present.
esp_err_t bus_i2c2_probe(uint8_t addr);

// Recursive lock guarding the bus.
void bus_i2c2_lock(void);
void bus_i2c2_unlock(void);

#ifdef __cplusplus
}
#endif
