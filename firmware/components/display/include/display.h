// display.h — onboard ST7789 TFT status display.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the TFT (SPI + ST7789 + backlight) and start a task that renders a
// live status screen: device name, BLE state, sensor count, and latest readings.
esp_err_t display_init(void);

// Show a 3×3 colour grid (9 RGB565 cells, row-major) on the panel, overriding the status
// screen for ~30 s. Fed by the LEGO 3×3 Light Matrix emitter when the hub writes pixels.
// Safe to call from another task: it only stores the cells; the display task renders them.
void display_show_matrix(const uint16_t cells[9]);

#ifdef __cplusplus
}
#endif
