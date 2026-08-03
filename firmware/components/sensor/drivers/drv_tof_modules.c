// drv_tof_modules.c — named drivers for ToF *module boards* (onboard MCU, simple I2C read),
// as opposed to bare ST chips (see drv_vl53l1x.c). Output: dist (mm).
//
//   tof10120  — TOF10120 module. I2C 0x52, distance = 2 bytes BE at register 0x00.
//   tofi2c    — generic family driver for the TOF050C/TOF050F/TOF0200C/TOF0400C/TOF400C
//               module boards: reads `recipe.length` bytes (default 2) from `recipe.reg`
//               (default 0x00) at `addr` (default 0x29), byte order from `recipe.byte_order`,
//               value = raw*scale + offset. Adjust reg/addr/scale in the UI if a board differs.
#include "sensor.h"
#include "esp_log.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

static int32_t decode(const uint8_t *d, int len, bool be, bool is_signed)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++) v = be ? (v << 8) | d[i] : v | ((uint32_t)d[i] << (8 * i));
    // Sign-extend when the recipe says the value is signed — previously the flag was ignored,
    // so e.g. a signed 16-bit -1 decoded as 65535.
    if (is_signed && len < 4 && (v & (1u << (8 * len - 1))))
        v |= ~((1u << (8 * len)) - 1);
    return (int32_t)v;
}

// Clamp to this setup's configured measuring range, if set (0/0 = no clamp — these module
// boards don't report a fixed native range the way the bare vl53l1x chip does).
static float clamp_dist(const sensor_cfg_t *cfg, float mm)
{
    if (cfg->dist_min_mm != 0.0f && mm < cfg->dist_min_mm) mm = cfg->dist_min_mm;
    if (cfg->dist_max_mm != 0.0f && mm > cfg->dist_max_mm) mm = cfg->dist_max_mm;
    return mm;
}

// ── TOF10120 ────────────────────────────────────────────────────────────────
static esp_err_t tof10120_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;
    uint8_t addr = cfg->addr ? cfg->addr : 0x52;
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    uint8_t d[2];
    if ((err = bus_i2c_read_reg(addr, 0x00, d, 2)) != ESP_OK) return err;   // distance, big-endian mm
    out[0] = clamp_dist(cfg, (float)((d[0] << 8) | d[1]));
    *out_count = 1;
    return ESP_OK;
}

// ── TOFxxxC / TOFxxxF module family (configurable simple I2C read) ──────────
static esp_err_t tofi2c_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;
    uint8_t addr = cfg->addr ? cfg->addr : 0x29;
    int len = cfg->recipe.length > 0 ? cfg->recipe.length : 2;
    if (len > 4) len = 4;
    uint8_t reg = (uint8_t)cfg->recipe.reg;        // default 0x00
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    uint8_t d[4] = {0};
    if ((err = bus_i2c_read_reg(addr, reg, d, len)) != ESP_OK) return err;
    int32_t raw = decode(d, len, cfg->recipe.big_endian, cfg->recipe.is_signed);
    float scale = cfg->recipe.scale != 0.0f ? cfg->recipe.scale : 1.0f;
    out[0] = clamp_dist(cfg, raw * scale + cfg->recipe.offset);
    *out_count = 1;
    return ESP_OK;
}

static int dist_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    if (max < 1) return 0;
    names[0] = "dist";
    return 1;
}

const sensor_driver_t drv_tof10120 = { .type = "tof10120", .probe = NULL, .read = tof10120_read, .describe = dist_describe };
const sensor_driver_t drv_tofi2c   = { .type = "tofi2c",   .probe = NULL, .read = tofi2c_read,   .describe = dist_describe };
