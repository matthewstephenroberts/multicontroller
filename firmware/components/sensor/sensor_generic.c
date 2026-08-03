// sensor_generic.c — data-driven register-recipe engine (no sensor-specific code).
//
// Reads `length` bytes from the configured bus/address, splits them into `value_count`
// equal words, decodes each per byte_order/signed, then applies value = raw*scale + offset.
//
#include "sensor.h"
#include <string.h>
#include "esp_log.h"
#include "bus_i2c.h"
#include "bus_spi.h"
#include "bus_uart.h"
#include "i2c_mux.h"

#define RAW_MAX 32

static int32_t decode_word(const uint8_t *p, int n, bool be, bool is_signed)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = be ? p[i] : p[n - 1 - i];
        v = (v << 8) | b;
    }
    if (is_signed && n > 0 && n < 4) {
        uint32_t sign_bit = 1u << (n * 8 - 1);
        if (v & sign_bit) v |= ~((1u << (n * 8)) - 1);   // sign-extend
    }
    return (int32_t)v;
}

static esp_err_t read_raw(const sensor_cfg_t *cfg, uint8_t *raw, int len)
{
    switch (cfg->bus) {
    case BUS_I2C: {
        esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
        if (err != ESP_OK) return err;
        return bus_i2c_read_reg(cfg->addr, (uint8_t)cfg->recipe.reg, raw, len);
    }
    case BUS_SPI:
        return bus_spi_read_reg(cfg->cs_index, (uint8_t)cfg->recipe.reg, raw, len);
    case BUS_UART: {
        uint8_t cmd = (uint8_t)cfg->recipe.reg;
        bus_uart_flush();
        if (bus_uart_write(&cmd, 1) != ESP_OK) return ESP_FAIL;
        int n = bus_uart_read(raw, len, 250);
        return (n == len) ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t generic_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    const recipe_t *r = &cfg->recipe;
    int len = r->length;
    if (len < 1) len = 1;
    if (len > RAW_MAX) len = RAW_MAX;

    uint8_t raw[RAW_MAX] = {0};
    esp_err_t err = read_raw(cfg, raw, len);
    if (err != ESP_OK) return err;

    int vc = r->value_count > 0 ? r->value_count : 1;
    if (vc > max) vc = max;
    int word = len / vc;
    if (word < 1) { word = len; vc = 1; }

    for (int i = 0; i < vc; i++) {
        int32_t rawv = decode_word(&raw[i * word], word, r->big_endian, r->is_signed);
        out[i] = (float)rawv * r->scale + r->offset;
    }
    *out_count = vc;
    return ESP_OK;
}

const sensor_driver_t sensor_generic_driver = {
    .type = "generic",
    .probe = NULL,
    .read = generic_read,
    .describe = NULL,        // falls back to recipe value_names
};
