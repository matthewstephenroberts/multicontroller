// button_ctrl.h — Standalone button controller for boards with BOARD_BUTTON_GPIO.
// Provides button polling and press detection independent of the display component.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize button polling task. Safe to call on boards without a button (no-op).
// Creates a FreeRTOS task that polls the button GPIO and detects:
//   - 3-second hold: toggles BLE on/off
// Returns ESP_OK on success, ESP_FAIL if task creation fails.
esp_err_t button_ctrl_init(void);

#ifdef __cplusplus
}
#endif
