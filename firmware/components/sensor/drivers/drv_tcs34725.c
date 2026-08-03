// drv_tcs34725.c — named driver: AMS TCS34725 RGB colour sensor over I2C (addr 0x29).
// Outputs: clear, red, green, blue (raw 16-bit counts). Configured once, then polled.
#include "sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

static const char *TAG = "tcs34725";

// Register access needs the COMMAND bit (0x80); 0xA0 also sets auto-increment for block reads.
#define CMD         0x80
#define CMD_AINC    0xA0
#define REG_ENABLE  0x00
#define REG_ATIME   0x01
#define REG_CONTROL 0x0F
#define REG_ID      0x12   // 0x44 = TCS34725, 0x4D = TCS34727
#define REG_CDATA   0x14   // clear,red,green,blue (8 bytes, little-endian)

#define EN_PON 0x01
#define EN_AEN 0x02

// Integration time = (256 - ATIME) * 2.4ms, so ATIME 0..255 spans ~2.4ms..614.4ms. We derive it
// from the sensor's poll_ms so a new conversion is ready every time it's polled, instead of a
// fixed ~154ms integration that both under-uses a slow poll interval (leaving light on the
// table, more read noise than necessary) and over-runs a fast one (re-reading a stale sample).
#define TCS_INTEG_MIN_MS 2.4f
#define TCS_INTEG_MAX_MS 614.4f
#define TCS_I2C_MARGIN_MS 10.0f   // headroom for the I2C read + mux switch within poll_ms

static uint8_t tcs_compute_atime(uint32_t poll_ms)
{
    float target = (float)poll_ms - TCS_I2C_MARGIN_MS;
    if (target < TCS_INTEG_MIN_MS) target = TCS_INTEG_MIN_MS;
    if (target > TCS_INTEG_MAX_MS) target = TCS_INTEG_MAX_MS;
    int atime = 256 - (int)(target / 2.4f + 0.5f);
    if (atime < 0) atime = 0;
    if (atime > 255) atime = 255;
    return (uint8_t)atime;
}

// Gain scales inversely with integration time so overall exposure stays roughly consistent
// across the whole poll_ms range: short integration (fast polling) collects fewer photons and
// needs more gain to stay usable; long integration (slow polling) already collects plenty of
// light and needs less gain to avoid saturating on a bright/white target.
static uint8_t tcs_compute_gain(float integ_ms)
{
    if (integ_ms < 20.0f)  return 0x03;   // 60x
    if (integ_ms < 80.0f)  return 0x02;   // 16x
    if (integ_ms < 200.0f) return 0x01;   // 4x
    return 0x00;                         // 1x
}

// Tracks which (addr,mux,channel) have been configured, and at what poll_ms — re-run configure()
// if poll_ms changes so ATIME/gain stay derived from the sensor's *current* setting.
static struct { uint8_t addr, mux; int8_t ch; uint32_t poll_ms; bool used; } s_done[MC_MAX_SENSORS];
static int s_done_n;

static int find_done(const sensor_cfg_t *cfg)
{
    for (int i = 0; i < s_done_n; i++)
        if (s_done[i].used && s_done[i].addr == cfg->addr && s_done[i].mux == cfg->mux_addr && s_done[i].ch == cfg->mux_channel)
            return i;
    return -1;
}
static void mark_configured(const sensor_cfg_t *cfg, int idx)
{
    if (idx < 0) {
        if (s_done_n >= MC_MAX_SENSORS) return;
        idx = s_done_n++;
    }
    s_done[idx] = (typeof(s_done[idx])){
        .used = true, .addr = cfg->addr, .mux = cfg->mux_addr, .ch = cfg->mux_channel, .poll_ms = cfg->poll_ms,
    };
}

static esp_err_t wreg(const sensor_cfg_t *cfg, uint8_t reg, uint8_t val)
{
    return bus_i2c_write(cfg->addr, (uint8_t[]){CMD | reg, val}, 2);
}

static esp_err_t configure(const sensor_cfg_t *cfg)
{
    uint8_t id;
    esp_err_t err = bus_i2c_read_reg(cfg->addr, CMD | REG_ID, &id, 1);
    if (err != ESP_OK) return err;
    if (id != 0x44 && id != 0x4D && id != 0x10) {
        ESP_LOGW(TAG, "unexpected id 0x%02x at 0x%02x", id, cfg->addr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t atime = tcs_compute_atime(cfg->poll_ms);
    uint8_t gain = tcs_compute_gain((256 - atime) * 2.4f);
    if ((err = wreg(cfg, REG_ATIME, atime)) != ESP_OK) return err;
    if ((err = wreg(cfg, REG_CONTROL, gain)) != ESP_OK) return err;
    if ((err = wreg(cfg, REG_ENABLE, EN_PON)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(3));
    return wreg(cfg, REG_ENABLE, EN_PON | EN_AEN);                       // enable RGBC engine
}

static esp_err_t tcs_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 4) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    int di = find_done(cfg);
    if (di < 0 || s_done[di].poll_ms != cfg->poll_ms) {
        if ((err = configure(cfg)) != ESP_OK) return err;
        mark_configured(cfg, di);
    }

    uint8_t d[8];
    if ((err = bus_i2c_read_reg(cfg->addr, CMD_AINC | REG_CDATA, d, sizeof(d))) != ESP_OK) return err;
    out[0] = (d[1] << 8) | d[0];   // clear
    out[1] = (d[3] << 8) | d[2];   // red
    out[2] = (d[5] << 8) | d[4];   // green
    out[3] = (d[7] << 8) | d[6];   // blue
    *out_count = 4;
    return ESP_OK;
}

static int tcs_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    static const char *n[] = {"clear", "red", "green", "blue"};
    int c = max < 4 ? max : 4;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_tcs34725 = {
    .type = "tcs34725",
    .probe = NULL,
    .read = tcs_read,
    .describe = tcs_describe,
};
