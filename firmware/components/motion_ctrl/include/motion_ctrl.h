// motion_ctrl.h — M5Stack Atomic Motion Base motor controller initialization.
// Initializes and safely disables the STM32 motor controller (0x38) if present,
// preventing excessive current draw from an uninitialized controller at boot.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Probe for the motor controller on the Motion Base I2C bus and disable it if found.
// Safe to call even if a Motion Base is not attached.
// Returns ESP_OK on success (or if no base detected), ESP_FAIL on I2C error.
esp_err_t motion_ctrl_init(void);

#ifdef __cplusplus
}
#endif
