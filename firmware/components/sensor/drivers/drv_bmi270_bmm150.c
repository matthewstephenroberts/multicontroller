// drv_bmi270_bmm150.c — named driver: Bosch BMI270 6-axis IMU + BMM150 3-axis magnetometer, both
// built into the M5Stack AtomS3R's onboard sensor stack (a second, internal I2C bus — see imu.h —
// not a user-wired I2C device, so unlike every other named driver this one ignores cfg->addr/
// mux_addr/bus entirely: there's exactly one physical chip pair, always in the same place, same
// spirit as drv_gamepad.c's virtual sensor ignoring bus/pin config for a fixed singleton resource).
//
// BMM150 is reachable only through BMI270's aux port (see imu.c) — it was previously its own
// "bmm150" driver/sensor entry, but that made a 9-axis (mag-corrected yaw) orientation transform
// impossible: sensor_transform.c only ever sees one sensor's own reading at a time, so two
// separate BLE sensor entries could never be fused together. Since the two chips are physically
// inseparable anyway (BMM150 has no independent bus presence), merging them into one driver/
// sensor entry — type "bmi270_bmm150", naming both chips so it's clear at a glance this is a
// combined sensor, not just the accel/gyro — is both simpler for the user (one entry to add, not
// two) and the only way to get mag-corrected yaw (see sensor_transform.c's imu_orient9).
//
// Outputs: ax ay az (g), gx gy gz (deg/s), mx my mz (µT), temp (degC).
#include "sensor.h"
#include "imu.h"

static esp_err_t bmi270_bmm150_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    (void)cfg;
    if (max < 10) return ESP_ERR_INVALID_SIZE;
    float ag[7];
    esp_err_t err = imu_read_accel_gyro(ag);
    if (err != ESP_OK) return err;
    out[0] = ag[0]; out[1] = ag[1]; out[2] = ag[2];
    out[3] = ag[3]; out[4] = ag[4]; out[5] = ag[5];

    float mag[3] = { 0, 0, 0 };
    // A transient aux-passthrough glitch on the magnetometer shouldn't sink an otherwise-good
    // accel/gyro reading — zero mx/my/mz on failure rather than returning err; imu_orient9 in
    // sensor_transform.c already falls back to accel+gyro-only fusion when mag reads all-zero.
    (void)imu_read_mag(mag);
    out[6] = mag[0]; out[7] = mag[1]; out[8] = mag[2];
    out[9] = ag[6];   // temp

    *out_count = 10;
    return ESP_OK;
}

static int bmi270_bmm150_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    static const char *n[] = {"ax", "ay", "az", "gx", "gy", "gz", "mx", "my", "mz", "temp"};
    int c = max < 10 ? max : 10;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_bmi270_bmm150 = {
    .type = "bmi270_bmm150",
    .probe = NULL,
    .read = bmi270_bmm150_read,
    .describe = bmi270_bmm150_describe,
};
