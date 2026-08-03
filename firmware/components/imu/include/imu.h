// imu.h — glue between MultiController's sensor drivers and Bosch's official BMI270/BMM150
// Sensor APIs (vendored unmodified under vendor/ — see vendor/NOTICE.md). Handles the fact that,
// on the M5Stack AtomS3R, this chip pair sits on a second I2C bus (bus_i2c2, not the external
// Port.A bus every other driver uses) and that BMM150 is only reachable through BMI270's
// auxiliary I2C port, not directly — see drv_bmi270_bmm150.c for how these are exposed as a
// single combined MultiController sensor type.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the bus + both chips (BMI270 host, BMM150 via aux passthrough). Idempotent — safe to
// call every read, only does real work once. Returns ESP_ERR_NOT_SUPPORTED on a board without
// this onboard IMU (no BOARD_TFT_BL_I2C_ADDR).
esp_err_t imu_init(void);

// ax,ay,az (g), gx,gy,gz (deg/s), temp (degC) — 7 floats, same order/shape as this project's
// existing qmi8658 driver so downstream code (LEGO fields, dashboard, imu_orient/imu_tilt
// transforms) treats it identically.
esp_err_t imu_read_accel_gyro(float out[7]);

// mx,my,mz in microtesla (compensated, via BMM150's own trim-register compensation).
esp_err_t imu_read_mag(float out[3]);

#ifdef __cplusplus
}
#endif
