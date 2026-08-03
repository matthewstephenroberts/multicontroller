// bus_i2c3.h — third I2C bus: the Atomic Motion Base v1.2's own I2C (STM32 motor/servo
// controller at 0x38, INA226 power monitor at 0x40), which runs over the bottom pogo-pin
// header's G38(SDA)/G39(SCL) — BOARD_MOTION_I2C_* in board_config.h — NOT the external Port.A
// connector (bus_i2c.h) and NOT the internal display/IMU bus (bus_i2c2.h). Confirmed against
// M5Stack's own Atomic Motion Base pin-compatibility diagram and Arduino example, both of which
// put this base's I2C on G38(SDA)/G39(SCL) — an earlier assumption that it ran over the shared
// external bus (G1/G2) was wrong and meant this base could never actually be found by a scan.
//
// Bit-banged (plain GPIO), not a hardware I2C peripheral: the ESP32-S3 only has two I2C
// controllers (SOC_I2C_NUM == 2), and both are already committed — bus_i2c.c owns port 0
// (external Port.A), bus_i2c2.c owns port 1 (internal display/IMU bus). There is no third
// hardware controller to give this bus, so it's a straightforward polling software master
// instead — perfectly adequate here since this bus only ever needs occasional, low-rate reads
// (an INA226 read every ~20ms, not a high-throughput sensor).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configure the GPIO pins for bit-banged I2C (pins/freq from board_config.h's
// BOARD_MOTION_I2C_* macros). Idempotent. Returns ESP_ERR_NOT_SUPPORTED on a board without a
// Motion Base I2C pin pair defined (no BOARD_MOTION_I2C_SDA_GPIO) — e.g. the non-Atom boards.
esp_err_t bus_i2c3_init(void);

// Write `len` bytes to a 7-bit address.
esp_err_t bus_i2c3_write(uint8_t addr, const uint8_t *data, size_t len);

// Write a register pointer then read `len` bytes (repeated-start).
esp_err_t bus_i2c3_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

// Raw read of `len` bytes (no register write).
esp_err_t bus_i2c3_read(uint8_t addr, uint8_t *buf, size_t len);

// ACK-probe a 7-bit address. ESP_OK = present.
esp_err_t bus_i2c3_probe(uint8_t addr);

// Recursive lock guarding the bus.
void bus_i2c3_lock(void);
void bus_i2c3_unlock(void);

#ifdef __cplusplus
}
#endif
