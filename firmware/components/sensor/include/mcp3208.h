// mcp3208.h — MCP3208 8-channel 12-bit SPI ADC: raw channel read, shared by the "mcp3208",
// "qre1113" and "tssp_ir" drivers (they're all "one MCP3208 channel + some math" underneath).
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reads one single-ended channel (0..7) from the MCP3208 on SPI CS `cs_index` (see bus_spi.h).
// *out gets the 12-bit result (0..4095).
esp_err_t mcp3208_read_raw(int cs_index, int channel, uint16_t *out);

#ifdef __cplusplus
}
#endif
