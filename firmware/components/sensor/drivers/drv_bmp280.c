// drv_bmp280.c — named driver: Bosch BMP280 over I2C (temp + pressure; no humidity).
// Reads from cfg->addr (0x76 or 0x77 typically). Outputs: temp (degC), pressure (hPa).
#include "sensor.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

static const char *TAG = "bmp280";

#define REG_ID        0xD0   // 0x58 = BMP280, 0x60 = BME280
#define REG_CALIB     0x88   // T1..P9 (24 bytes)
#define REG_CTRL_MEAS 0xF4
#define REG_DATA      0xF7   // press(3) temp(3)

typedef struct {
    bool valid;
    uint8_t key_addr, key_mux; int8_t key_ch;
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
} calib_t;

static calib_t s_cal[MC_MAX_SENSORS];
static int s_cal_n;

static calib_t *cal_lookup(const sensor_cfg_t *cfg)
{
    for (int i = 0; i < s_cal_n; i++)
        if (s_cal[i].key_addr == cfg->addr && s_cal[i].key_mux == cfg->mux_addr &&
            s_cal[i].key_ch == cfg->mux_channel)
            return &s_cal[i];
    if (s_cal_n < MC_MAX_SENSORS) {
        calib_t *c = &s_cal[s_cal_n++];
        memset(c, 0, sizeof(*c));
        c->key_addr = cfg->addr; c->key_mux = cfg->mux_addr; c->key_ch = cfg->mux_channel;
        return c;
    }
    return NULL;
}

static esp_err_t load_calib(const sensor_cfg_t *cfg, calib_t *c)
{
    uint8_t a[24];
    esp_err_t err = bus_i2c_read_reg(cfg->addr, REG_CALIB, a, sizeof(a));
    if (err != ESP_OK) return err;
    c->T1 = (a[1] << 8) | a[0];
    c->T2 = (int16_t)((a[3] << 8) | a[2]);
    c->T3 = (int16_t)((a[5] << 8) | a[4]);
    c->P1 = (a[7] << 8) | a[6];
    c->P2 = (int16_t)((a[9] << 8) | a[8]);
    c->P3 = (int16_t)((a[11] << 8) | a[10]);
    c->P4 = (int16_t)((a[13] << 8) | a[12]);
    c->P5 = (int16_t)((a[15] << 8) | a[14]);
    c->P6 = (int16_t)((a[17] << 8) | a[16]);
    c->P7 = (int16_t)((a[19] << 8) | a[18]);
    c->P8 = (int16_t)((a[21] << 8) | a[20]);
    c->P9 = (int16_t)((a[23] << 8) | a[22]);
    c->valid = true;
    return ESP_OK;
}

// Bosch fixed-point compensation (datasheet reference).
static int32_t comp_temp(const calib_t *c, int32_t adc_T, int32_t *t_fine)
{
    int32_t v1 = ((((adc_T >> 3) - ((int32_t)c->T1 << 1))) * (int32_t)c->T2) >> 11;
    int32_t v2 = (((((adc_T >> 4) - (int32_t)c->T1) * ((adc_T >> 4) - (int32_t)c->T1)) >> 12)
                  * (int32_t)c->T3) >> 14;
    *t_fine = v1 + v2;
    return (*t_fine * 5 + 128) >> 8;          // 0.01 degC
}
static uint32_t comp_press(const calib_t *c, int32_t adc_P, int32_t t_fine)
{
    int64_t v1 = (int64_t)t_fine - 128000;
    int64_t v2 = v1 * v1 * (int64_t)c->P6;
    v2 += (v1 * (int64_t)c->P5) << 17;
    v2 += ((int64_t)c->P4) << 35;
    v1 = ((v1 * v1 * (int64_t)c->P3) >> 8) + ((v1 * (int64_t)c->P2) << 12);
    v1 = (((((int64_t)1) << 47) + v1) * (int64_t)c->P1) >> 33;
    if (v1 == 0) return 0;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = (((int64_t)c->P9) * (p >> 13) * (p >> 13)) >> 25;
    v2 = (((int64_t)c->P8) * p) >> 19;
    p = ((p + v1 + v2) >> 8) + (((int64_t)c->P7) << 4);
    return (uint32_t)p;                       // Q24.8 Pa
}

static esp_err_t bmp_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 2) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    uint8_t id;
    if ((err = bus_i2c_read_reg(cfg->addr, REG_ID, &id, 1)) != ESP_OK) return err;
    if (id != 0x58 && id != 0x60) {           // not a BMP280/BME280
        ESP_LOGW(TAG, "unexpected chip id 0x%02x at 0x%02x", id, cfg->addr);
        return ESP_ERR_INVALID_RESPONSE;
    }

    calib_t *c = cal_lookup(cfg);
    if (!c) return ESP_ERR_NO_MEM;
    if (!c->valid && (err = load_calib(cfg, c)) != ESP_OK) return err;

    // Forced mode: temp x1, press x1, mode = 01.
    uint8_t meas = (1 << 5) | (1 << 2) | 0x01;
    if ((err = bus_i2c_write(cfg->addr, (uint8_t[]){REG_CTRL_MEAS, meas}, 2)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t d[6];
    if ((err = bus_i2c_read_reg(cfg->addr, REG_DATA, d, sizeof(d))) != ESP_OK) return err;
    int32_t adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
    int32_t adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);

    int32_t t_fine;
    int32_t T = comp_temp(c, adc_T, &t_fine);
    uint32_t P = comp_press(c, adc_P, t_fine);

    out[0] = T / 100.0f;             // degC
    out[1] = P / 256.0f / 100.0f;    // hPa
    *out_count = 2;
    return ESP_OK;
}

static int bmp_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    static const char *n[] = {"temp", "pressure"};
    int c = max < 2 ? max : 2;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_bmp280 = {
    .type = "bmp280",
    .probe = NULL,
    .read = bmp_read,
    .describe = bmp_describe,
};
