// drv_bme280.c — named driver: Bosch BME280 over I2C (behind the TCA9548A mux).
// Demonstrates the "named driver" path: calibration read + Bosch compensation math.
// Outputs: temp (degC), pressure (hPa), humidity (%RH).
#include "sensor.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

// Registers
#define REG_CALIB00  0x88   // T1..P9 (26 bytes)
#define REG_ID       0xD0
#define REG_RESET    0xE0
#define REG_CALIB26  0xE1   // H2..H6 (7 bytes)
#define REG_CTRL_HUM 0xF2
#define REG_CTRL_MEAS 0xF4
#define REG_DATA     0xF7   // press(3) temp(3) hum(2)

typedef struct {
    bool valid;
    uint8_t key_addr; uint8_t key_mux; int8_t key_ch;
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1; int16_t H2; uint8_t H3; int16_t H4, H5; int8_t H6;
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
    uint8_t a[26], b[7];
    esp_err_t err = bus_i2c_read_reg(cfg->addr, REG_CALIB00, a, sizeof(a));
    if (err != ESP_OK) return err;
    err = bus_i2c_read_reg(cfg->addr, REG_CALIB26, b, sizeof(b));
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
    c->H1 = a[25];
    c->H2 = (int16_t)((b[1] << 8) | b[0]);
    c->H3 = b[2];
    c->H4 = (int16_t)((b[3] << 4) | (b[4] & 0x0F));
    c->H5 = (int16_t)((b[5] << 4) | (b[4] >> 4));
    c->H6 = (int8_t)b[6];
    c->valid = true;
    return ESP_OK;
}

// Bosch fixed-point compensation (datasheet reference implementation).
static int32_t comp_temp(const calib_t *c, int32_t adc_T, int32_t *t_fine)
{
    int32_t v1 = ((((adc_T >> 3) - ((int32_t)c->T1 << 1))) * (int32_t)c->T2) >> 11;
    int32_t v2 = (((((adc_T >> 4) - (int32_t)c->T1) * ((adc_T >> 4) - (int32_t)c->T1)) >> 12)
                  * (int32_t)c->T3) >> 14;
    *t_fine = v1 + v2;
    return (*t_fine * 5 + 128) >> 8;   // 0.01 degC
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
    return (uint32_t)p;                // Q24.8 Pa
}
static uint32_t comp_hum(const calib_t *c, int32_t adc_H, int32_t t_fine)
{
    int32_t v = t_fine - 76800;
    v = ((((adc_H << 14) - (((int32_t)c->H4) << 20) - (((int32_t)c->H5) * v)) + 16384) >> 15)
        * (((((((v * (int32_t)c->H6) >> 10) * (((v * (int32_t)c->H3) >> 11) + 32768)) >> 10)
            + 2097152) * (int32_t)c->H2 + 8192) >> 14);
    v -= (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->H1) >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    return (uint32_t)(v >> 12);        // Q22.10 %RH
}

static esp_err_t bme_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 3) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    calib_t *c = cal_lookup(cfg);
    if (!c) return ESP_ERR_NO_MEM;
    if (!c->valid && (err = load_calib(cfg, c)) != ESP_OK) return err;

    // Forced mode: humidity x1, then temp x1 / press x1 / mode=forced(0b01).
    uint8_t hum = 0x01;
    if ((err = bus_i2c_write(cfg->addr, (uint8_t[]){REG_CTRL_HUM, hum}, 2)) != ESP_OK) return err;
    uint8_t meas = (1 << 5) | (1 << 2) | 0x01;
    if ((err = bus_i2c_write(cfg->addr, (uint8_t[]){REG_CTRL_MEAS, meas}, 2)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t d[8];
    if ((err = bus_i2c_read_reg(cfg->addr, REG_DATA, d, sizeof(d))) != ESP_OK) return err;

    int32_t adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
    int32_t adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
    int32_t adc_H = ((int32_t)d[6] << 8) | d[7];

    int32_t t_fine;
    int32_t T = comp_temp(c, adc_T, &t_fine);
    uint32_t P = comp_press(c, adc_P, t_fine);
    uint32_t H = comp_hum(c, adc_H, t_fine);

    out[0] = T / 100.0f;            // degC
    out[1] = P / 256.0f / 100.0f;  // hPa
    out[2] = H / 1024.0f;          // %RH
    *out_count = 3;
    return ESP_OK;
}

static int bme_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    static const char *n[] = {"temp", "pressure", "humidity"};
    int c = max < 3 ? max : 3;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_bme280 = {
    .type = "bme280",
    .probe = NULL,
    .read = bme_read,
    .describe = bme_describe,
};
