// bus_spi.h — shared SPI bus with up to 5 chip-select lines (indexed 0..4).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the SPI host and register a device for every wired CS in board_config.h.
esp_err_t bus_spi_init(void);

// True once bus_spi_init() has brought the shared SPI host up. The display (esp_lcd) shares
// this host — it checks here so it doesn't call spi_bus_initialize() on an already-up bus,
// which succeeds-by-tolerance but makes the IDF driver print a spurious error log at boot.
bool bus_spi_is_initialized(void);

// True if cs_index (0..4) maps to a wired CS GPIO.
bool bus_spi_cs_valid(int cs_index);

// Full-duplex transfer of `len` bytes on the device at cs_index. tx/rx may be NULL.
esp_err_t bus_spi_transfer(int cs_index, const uint8_t *tx, uint8_t *rx, size_t len);

// Send register byte `reg`, then clock in `len` bytes into buf (half-duplex style).
// The caller sets any read/auto-increment bits inside `reg` (sensor-specific).
esp_err_t bus_spi_read_reg(int cs_index, uint8_t reg, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
