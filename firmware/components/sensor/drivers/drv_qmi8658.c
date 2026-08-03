// drv_qmi8658.c — named driver: QST QMI8658C 6-axis IMU over I2C.
// Reads from cfg->addr (0x6A or 0x6B typically). Configured once, then polled.
// Outputs: ax ay az (g), gx gy gz (deg/s), temp (degC).
#include "sensor.h"
#include <string.h>
#include "esp_log.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

static const char *TAG = "qmi8658";

#define REG_WHOAMI 0x00   // = 0x05
#define REG_CTRL1  0x02
#define REG_CTRL2  0x03   // accel: aFS[6:4], aODR[3:0]
#define REG_CTRL3  0x04   // gyro:  gFS[6:4], gODR[3:0]
#define REG_CTRL7  0x08   // enable: gEN(bit1) aEN(bit0)
#define REG_TEMP_L 0x33   // temp(2) accel(6) gyro(6) — 14 bytes from here

// Full-scale choices below -> sensitivities (LSB per unit).
#define ACC_LSB_PER_G   4096.0f   // +-8 g
#define GYR_LSB_PER_DPS 64.0f     // +-512 dps

// Track which sensors have been configured (keyed by addr+mux+channel).
static struct { uint8_t addr, mux; int8_t ch; } s_done[MC_MAX_SENSORS];
static int s_done_n;

static bool already_configured(const sensor_cfg_t *cfg)
{
    for (int i = 0; i < s_done_n; i++)
        if (s_done[i].addr == cfg->addr && s_done[i].mux == cfg->mux_addr && s_done[i].ch == cfg->mux_channel)
            return true;
    return false;
}
static void mark_configured(const sensor_cfg_t *cfg)
{
    if (s_done_n >= MC_MAX_SENSORS) return;
    s_done[s_done_n].addr = cfg->addr;
    s_done[s_done_n].mux = cfg->mux_addr;
    s_done[s_done_n].ch = cfg->mux_channel;
    s_done_n++;
}

static esp_err_t w(const sensor_cfg_t *cfg, uint8_t reg, uint8_t val)
{
    return bus_i2c_write(cfg->addr, (uint8_t[]){reg, val}, 2);
}

static esp_err_t configure(const sensor_cfg_t *cfg)
{
    uint8_t id;
    esp_err_t err = bus_i2c_read_reg(cfg->addr, REG_WHOAMI, &id, 1);
    if (err != ESP_OK) return err;
    if (id != 0x05) {
        ESP_LOGW(TAG, "unexpected WHO_AM_I 0x%02x at 0x%02x", id, cfg->addr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((err = w(cfg, REG_CTRL1, 0x40)) != ESP_OK) return err;   // addr auto-increment
    if ((err = w(cfg, REG_CTRL2, 0x23)) != ESP_OK) return err;   // accel +-8g, ~235Hz
    if ((err = w(cfg, REG_CTRL3, 0x53)) != ESP_OK) return err;   // gyro +-512dps, ~235Hz
    if ((err = w(cfg, REG_CTRL7, 0x03)) != ESP_OK) return err;   // enable accel + gyro
    return ESP_OK;
}

static int16_t le16(const uint8_t *p) { return (int16_t)((p[1] << 8) | p[0]); }

static esp_err_t qmi_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 7) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    if (!already_configured(cfg)) {
        if ((err = configure(cfg)) != ESP_OK) return err;
        mark_configured(cfg);
    }

    uint8_t d[14];   // temp(2) accel(6) gyro(6)
    if ((err = bus_i2c_read_reg(cfg->addr, REG_TEMP_L, d, sizeof(d))) != ESP_OK) return err;

    out[0] = le16(&d[2])  / ACC_LSB_PER_G;     // ax (g)
    out[1] = le16(&d[4])  / ACC_LSB_PER_G;     // ay
    out[2] = le16(&d[6])  / ACC_LSB_PER_G;     // az
    out[3] = le16(&d[8])  / GYR_LSB_PER_DPS;   // gx (dps)
    out[4] = le16(&d[10]) / GYR_LSB_PER_DPS;   // gy
    out[5] = le16(&d[12]) / GYR_LSB_PER_DPS;   // gz
    out[6] = le16(&d[0])  / 256.0f;            // temp (degC)
    *out_count = 7;
    return ESP_OK;
}

static int qmi_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    static const char *n[] = {"ax", "ay", "az", "gx", "gy", "gz", "temp"};
    int c = max < 7 ? max : 7;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_qmi8658 = {
    .type = "qmi8658",
    .probe = NULL,
    .read = qmi_read,
    .describe = qmi_describe,
};
