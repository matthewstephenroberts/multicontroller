// imu.c — glue between MultiController and Bosch's vendored BMI270/BMM150 Sensor APIs. Follows
// the call sequence in Bosch's own bmi270_examples/read_aux_data_mode/read_aux_data_mode.c
// (fetched and reviewed while writing this) almost exactly, swapping their COINES-platform I2C
// calls for bus_i2c2 (the AtomS3R's internal I2C bus — see bus_i2c2.h) and dropping the
// interrupt-pin/FIFO paths that example also demonstrates, since this driver just polls.
#include "imu.h"
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_config.h"

#if defined(BOARD_TFT_BL_I2C_ADDR)

#include "bus_i2c2.h"
#include "bmi270.h"
#include "bmm150.h"

static const char *TAG = "imu";

// BMI270's primary I2C address (SDO pulled low, as wired on the AtomS3R).
#define BMI2_I2C_ADDR 0x68

static struct bmi2_dev s_bmi;
static struct bmm150_dev s_bmm;
static bool s_ready;
static bool s_init_failed;

// A period under ~1ms is short enough to just busy-wait (esp_rom_delay_us) — the config-file
// upload inside bmi270_init() also asks for a few longer (multi-ms) delays, which get a real
// vTaskDelay instead so that one-time init doesn't hold other tasks off the CPU.
static void delay_us_impl(uint32_t period_us)
{
    if (period_us > 1000) vTaskDelay(pdMS_TO_TICKS(period_us / 1000 + 1));
    else esp_rom_delay_us(period_us);
}

// ---- BMI270 primary interface (real I2C, via bus_i2c2) ----
static BMI2_INTF_RETURN_TYPE bmi2_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    return bus_i2c2_read_reg(BMI2_I2C_ADDR, reg_addr, reg_data, len) == ESP_OK ? 0 : -1;
}
static BMI2_INTF_RETURN_TYPE bmi2_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    (void)intf_ptr;
    if (len > 63) return -1;
    uint8_t buf[64];
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);
    return bus_i2c2_write(BMI2_I2C_ADDR, buf, len + 1) == ESP_OK ? 0 : -1;
}
static void bmi2_delay_us_cb(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;
    delay_us_impl(period);
}

// ---- BMM150 auxiliary interface (NOT real I2C — routed through BMI270's manual-aux mode, per
// bmi2_read_aux_man_mode/bmi2_write_aux_man_mode, since BMM150 isn't wired to the host bus at
// all; see imu.h's file comment) ----
static int8_t bmi2_aux_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    struct bmi2_dev *dev = (struct bmi2_dev *)intf_ptr;
    return bmi2_read_aux_man_mode(reg_addr, reg_data, (uint16_t)length, dev);
}
static int8_t bmi2_aux_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    struct bmi2_dev *dev = (struct bmi2_dev *)intf_ptr;
    return bmi2_write_aux_man_mode(reg_addr, reg_data, (uint16_t)length, dev);
}
static void bmm150_delay_us_cb(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;
    delay_us_impl(period);
}

esp_err_t imu_init(void)
{
    if (s_ready) return ESP_OK;
    if (s_init_failed) return ESP_FAIL;

    esp_err_t err = bus_i2c2_init();
    if (err != ESP_OK) { s_init_failed = true; return err; }

    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.intf = BMI2_I2C_INTF;
    s_bmi.read = bmi2_i2c_read;
    s_bmi.write = bmi2_i2c_write;
    s_bmi.delay_us = bmi2_delay_us_cb;
    s_bmi.intf_ptr = NULL;
    s_bmi.read_write_len = 32;
    s_bmi.config_file_ptr = NULL;   // NULL = use bmi270.c's own built-in bmi270_config_file[]

    int8_t rslt = bmi270_init(&s_bmi);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_init failed: %d (check wiring on the internal G45/G0 bus)", rslt);
        s_init_failed = true;
        return ESP_FAIL;
    }

    // Pull-up resistor 10k on the aux SDA line, per the reference example — needed for reliable
    // aux-mode communication with BMM150.
    uint8_t regdata = BMI2_ASDA_PUPSEL_10K;
    bmi2_set_regs(BMI2_AUX_IF_TRIM, &regdata, 1, &s_bmi);

    // Accel + gyro configuration.
    struct bmi2_sens_config cfg[2];
    cfg[0].type = BMI2_ACCEL;
    cfg[1].type = BMI2_GYRO;
    rslt = bmi2_get_sensor_config(cfg, 2, &s_bmi);
    if (rslt != BMI2_OK) { ESP_LOGE(TAG, "bmi2_get_sensor_config failed: %d", rslt); s_init_failed = true; return ESP_FAIL; }

    cfg[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    cfg[0].cfg.acc.bwp = BMI2_ACC_OSR2_AVG2;
    cfg[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
    cfg[0].cfg.acc.range = BMI2_ACC_RANGE_4G;   // ±4g — see imu.h; mirrored in web/src/types.ts

    cfg[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    cfg[1].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
    cfg[1].cfg.gyr.bwp = BMI2_GYR_OSR2_MODE;
    cfg[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
    cfg[1].cfg.gyr.range = BMI2_GYR_RANGE_1000;   // ±1000 deg/s — see imu.h; mirrored web-side
    cfg[1].cfg.gyr.ois_range = BMI2_GYR_OIS_2000;

    rslt = bmi2_set_sensor_config(cfg, 2, &s_bmi);
    if (rslt != BMI2_OK) { ESP_LOGE(TAG, "bmi2_set_sensor_config (accel/gyro) failed: %d", rslt); s_init_failed = true; return ESP_FAIL; }

    // Auxiliary interface (BMM150), manual mode first so bmm150_init()/set_presetmode()/
    // set_op_mode() can actually talk to it, then switched to automatic polling mode.
    s_bmm.intf_ptr = &s_bmi;
    s_bmm.read = bmi2_aux_i2c_read;
    s_bmm.write = bmi2_aux_i2c_write;
    s_bmm.delay_us = bmm150_delay_us_cb;
    s_bmm.intf = BMM150_I2C_INTF;

    struct bmi2_sens_config aux_cfg;
    aux_cfg.type = BMI2_AUX;
    aux_cfg.cfg.aux.odr = BMI2_AUX_ODR_100HZ;
    aux_cfg.cfg.aux.aux_en = BMI2_ENABLE;
    aux_cfg.cfg.aux.i2c_device_addr = BMM150_DEFAULT_I2C_ADDRESS;
    aux_cfg.cfg.aux.fcu_write_en = BMI2_ENABLE;
    aux_cfg.cfg.aux.manual_en = BMI2_ENABLE;
    aux_cfg.cfg.aux.aux_rd_burst = BMI2_AUX_READ_LEN_3;
    aux_cfg.cfg.aux.man_rd_burst = BMI2_AUX_READ_LEN_3;
    aux_cfg.cfg.aux.read_addr = BMM150_REG_DATA_X_LSB;

    rslt = bmi2_set_sensor_config(&aux_cfg, 1, &s_bmi);
    if (rslt != BMI2_OK) { ESP_LOGE(TAG, "bmi2_set_sensor_config (aux, manual) failed: %d", rslt); s_init_failed = true; return ESP_FAIL; }

    rslt = bmm150_init(&s_bmm);
    if (rslt != BMM150_OK || s_bmm.chip_id != BMM150_CHIP_ID) {
        ESP_LOGE(TAG, "bmm150_init failed: %d (chip_id 0x%02x, expected 0x%02x)", rslt, s_bmm.chip_id, BMM150_CHIP_ID);
        s_init_failed = true;
        return ESP_FAIL;
    }

    struct bmm150_settings bmm_sett = { .preset_mode = BMM150_PRESETMODE_REGULAR };
    bmm150_set_presetmode(&bmm_sett, &s_bmm);
    bmm_sett.pwr_mode = BMM150_POWERMODE_FORCED;
    bmm150_set_op_mode(&bmm_sett, &s_bmm);

    // Switch aux back to automatic (non-manual) polling mode now that BMM150 itself is configured.
    aux_cfg.cfg.aux.manual_en = BMI2_DISABLE;
    rslt = bmi2_set_sensor_config(&aux_cfg, 1, &s_bmi);
    if (rslt != BMI2_OK) { ESP_LOGE(TAG, "bmi2_set_sensor_config (aux, auto) failed: %d", rslt); s_init_failed = true; return ESP_FAIL; }

    uint8_t sensor_list[3] = { BMI2_ACCEL, BMI2_GYRO, BMI2_AUX };
    rslt = bmi2_sensor_enable(sensor_list, 3, &s_bmi);
    if (rslt != BMI2_OK) { ESP_LOGE(TAG, "bmi2_sensor_enable failed: %d", rslt); s_init_failed = true; return ESP_FAIL; }

    ESP_LOGI(TAG, "BMI270 (chip_id 0x%02x) + BMM150 (chip_id 0x%02x) ready", s_bmi.chip_id, s_bmm.chip_id);
    s_ready = true;
    return ESP_OK;
}

// Raw LSB -> physical unit, matching the fixed FS configured above (see BMI2_ACC_RANGE_4G_VAL /
// BMI2_GYR_RANGE_1000_VAL — half_scale = 2^(resolution-1), resolution is always 16-bit on BMI270).
static float lsb_to_g(int16_t v)   { return ((float)v * BMI2_ACC_RANGE_4G_VAL) / 32768.0f; }
static float lsb_to_dps(int16_t v) { return ((float)v * BMI2_GYR_RANGE_1000_VAL) / 32768.0f; }

esp_err_t imu_read_accel_gyro(float out[7])
{
    esp_err_t err = imu_init();
    if (err != ESP_OK) return err;

    struct bmi2_sens_data data;
    int8_t rslt = bmi2_get_sensor_data(&data, &s_bmi);
    if (rslt != BMI2_OK) return ESP_FAIL;

    out[0] = lsb_to_g(data.acc.x);
    out[1] = lsb_to_g(data.acc.y);
    out[2] = lsb_to_g(data.acc.z);
    out[3] = lsb_to_dps(data.gyr.x);
    out[4] = lsb_to_dps(data.gyr.y);
    out[5] = lsb_to_dps(data.gyr.z);

    int16_t temp_raw = 0;
    rslt = bmi2_get_temperature_data(&temp_raw, &s_bmi);
    out[6] = (rslt == BMI2_OK) ? ((float)temp_raw / 512.0f) + 23.0f : 0.0f;
    return ESP_OK;
}

esp_err_t imu_read_mag(float out[3])
{
    esp_err_t err = imu_init();
    if (err != ESP_OK) return err;

    struct bmi2_sens_data data;
    int8_t rslt = bmi2_get_sensor_data(&data, &s_bmi);
    if (rslt != BMI2_OK) return ESP_FAIL;

    struct bmm150_mag_data mag;
    rslt = bmm150_aux_mag_data(data.aux_data, &mag, &s_bmm);
    if (rslt != BMM150_OK) return ESP_FAIL;

    out[0] = mag.x;   // µT — BMM150_USE_FLOATING_POINT (set in CMakeLists) gives compensated µT directly
    out[1] = mag.y;
    out[2] = mag.z;
    return ESP_OK;
}

#else   // !defined(BOARD_TFT_BL_I2C_ADDR) — no onboard IMU on this board

esp_err_t imu_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t imu_read_accel_gyro(float out[7]) { (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t imu_read_mag(float out[3]) { (void)out; return ESP_ERR_NOT_SUPPORTED; }

#endif
