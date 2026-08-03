// bus_uart.h — wrapper over the IDF uart driver for the auxiliary sensor port.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the auxiliary UART (pins from board_config.h) at the given baud.
esp_err_t bus_uart_init(int baud);

// Write bytes to the UART.
esp_err_t bus_uart_write(const uint8_t *data, size_t len);

// Read up to `max` bytes, blocking up to timeout_ms. Returns bytes read (>=0) or -1.
int bus_uart_read(uint8_t *buf, size_t max, int timeout_ms);

// Discard buffered RX data.
void bus_uart_flush(void);

#ifdef __cplusplus
}
#endif
