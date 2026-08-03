// bus_scan.h — discover attached devices across the buses.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Probe the buses and return a freshly malloc'd JSON array string of discovered devices:
//   [ {"bus":"i2c","addr":118,"mux_addr":112,"channel":2,"guess":"bme280"}, ... ]
// I2C is probed directly and across every channel of any configured/standard mux address.
// SPI reports each wired CS index; UART reports the configured port. Caller frees.
char *bus_scan_run_json(void);

#ifdef __cplusplus
}
#endif
