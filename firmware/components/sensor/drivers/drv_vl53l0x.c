// drv_vl53l0x.c — named driver: ST VL53L0X time-of-flight distance sensor (I2C 0x29).
// Output: dist (mm). Ported from the Pololu VL53L0X Arduino library (which mirrors ST's
// VL53L0X API): data init → reference SPAD management → default tuning settings → interrupt
// config → reference (VHV + phase) calibration → back-to-back continuous ranging, then a
// cheap per-poll result read. NOT protocol-compatible with the VL53L1X (different register
// map and init) despite sharing the same 0x29 address — the bus scanner tells them apart by
// ID register (see bus_scan.c).
//
// Native range ~30-2000 mm (white target, default 33 ms timing budget). Out-of-range reads
// report the sensor's 8190/8191 sentinel — clamped to the configured dist_max_mm when set.
#include "sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

static const char *TAG = "vl53l0x";

#define L0X_ADDR_DEFAULT 0x29
#define IO_TIMEOUT_MS    60

// 8-bit registers (subset used; names per the ST API / Pololu library).
enum {
    SYSRANGE_START                              = 0x00,
    SYSTEM_SEQUENCE_CONFIG                      = 0x01,
    SYSTEM_INTERRUPT_CONFIG_GPIO                = 0x0A,
    SYSTEM_INTERRUPT_CLEAR                      = 0x0B,
    RESULT_INTERRUPT_STATUS                     = 0x13,
    RESULT_RANGE_STATUS                         = 0x14,
    MSRC_CONFIG_CONTROL                         = 0x60,
    FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT = 0x44,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_0            = 0xB0,
    GLOBAL_CONFIG_REF_EN_START_SELECT           = 0xB6,
    DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD         = 0x4E,
    DYNAMIC_SPAD_REF_EN_START_OFFSET            = 0x4F,
    GPIO_HV_MUX_ACTIVE_HIGH                     = 0x84,
    IDENTIFICATION_MODEL_ID                     = 0xC0,   // reads 0xEE
};

typedef struct {
    bool    used;
    uint8_t addr, mux;
    int8_t  ch;
    bool    initialised;
    uint8_t stop_variable;
    float   last_mm;
    bool    has_last;
    int64_t last_fresh_us;   // when the last *new* sample arrived (staleness watchdog)
} l0x_state_t;

// Continuous ranging produces a sample every ~33ms; going a full second without one means the
// sensor stopped talking (unplugged, mux fault, brown-out) — without this cutoff, the "repeat
// the last reading between samples" path below would keep serving the cached value with OK
// status forever, freezing the dashboard at a stale distance with no error in sight.
#define L0X_STALE_US (1000 * 1000)
static l0x_state_t s_state[MC_MAX_SENSORS];

static l0x_state_t *state_for(uint8_t addr, uint8_t mux, int8_t ch)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_state[i].used && s_state[i].addr == addr && s_state[i].mux == mux && s_state[i].ch == ch)
            return &s_state[i];
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (!s_state[i].used) {
            s_state[i] = (l0x_state_t){ .used = true, .addr = addr, .mux = mux, .ch = ch };
            return &s_state[i];
        }
    return NULL;
}

// ── register helpers (8-bit register addresses) ─────────────────────────────
static esp_err_t w_multi(uint8_t a, uint8_t r, const uint8_t *d, int n)
{
    uint8_t buf[8];
    if (n > 7) return ESP_ERR_INVALID_SIZE;
    buf[0] = r;
    for (int i = 0; i < n; i++) buf[1 + i] = d[i];
    return bus_i2c_write(a, buf, n + 1);
}
static esp_err_t w8(uint8_t a, uint8_t r, uint8_t v)   { return w_multi(a, r, &v, 1); }
static esp_err_t w16(uint8_t a, uint8_t r, uint16_t v) { uint8_t d[2] = { v >> 8, v }; return w_multi(a, r, d, 2); }
static uint8_t  r8(uint8_t a, uint8_t r)  { uint8_t v = 0; bus_i2c_read_reg(a, r, &v, 1); return v; }
static uint16_t r16(uint8_t a, uint8_t r) { uint8_t d[2] = {0}; bus_i2c_read_reg(a, r, d, 2); return (uint16_t)((d[0] << 8) | d[1]); }

static bool wait_bit(uint8_t a, uint8_t reg, uint8_t mask)
{
    int64_t t0 = esp_timer_get_time();
    while ((r8(a, reg) & mask) == 0) {
        if (esp_timer_get_time() - t0 > IO_TIMEOUT_MS * 1000) return false;
        vTaskDelay(1);
    }
    return true;
}

// ── init (ST/Pololu sequence) ────────────────────────────────────────────────

// VL53L0X_load_tuning_settings(): ST's "default tuning settings" register blob, applied
// verbatim by every port of the API. Pairs of (register, value).
static const uint8_t k_tuning[][2] = {
    {0xFF,0x01},{0x00,0x00},{0xFF,0x00},{0x09,0x00},{0x10,0x00},{0x11,0x00},{0x24,0x01},{0x25,0xFF},
    {0x75,0x00},{0xFF,0x01},{0x4E,0x2C},{0x48,0x00},{0x30,0x20},{0xFF,0x00},{0x30,0x09},{0x54,0x00},
    {0x31,0x04},{0x32,0x03},{0x40,0x83},{0x46,0x25},{0x60,0x00},{0x27,0x00},{0x50,0x06},{0x51,0x00},
    {0x52,0x96},{0x56,0x08},{0x57,0x30},{0x61,0x00},{0x62,0x00},{0x64,0x00},{0x65,0x00},{0x66,0xA0},
    {0xFF,0x01},{0x22,0x32},{0x47,0x14},{0x49,0xFF},{0x4A,0x00},{0xFF,0x00},{0x7A,0x0A},{0x7B,0x00},
    {0x78,0x21},{0xFF,0x01},{0x23,0x34},{0x42,0x00},{0x44,0xFF},{0x45,0x26},{0x46,0x05},{0x40,0x40},
    {0x0E,0x06},{0x20,0x1A},{0x43,0x40},{0xFF,0x00},{0x34,0x03},{0x35,0x44},{0xFF,0x01},{0x31,0x04},
    {0x4B,0x09},{0x4C,0x05},{0x4D,0x04},{0xFF,0x00},{0x44,0x00},{0x45,0x20},{0x47,0x08},{0x48,0x28},
    {0x67,0x00},{0x70,0x04},{0x71,0x01},{0x72,0xFE},{0x76,0x00},{0x77,0x00},{0xFF,0x01},{0x0D,0x01},
    {0xFF,0x00},{0x80,0x01},{0x01,0xF8},{0xFF,0x01},{0x8E,0x01},{0x00,0x01},{0xFF,0x00},{0x80,0x00},
};

static bool get_spad_info(uint8_t a, uint8_t *count, bool *is_aperture)
{
    w8(a, 0x80, 0x01); w8(a, 0xFF, 0x01); w8(a, 0x00, 0x00); w8(a, 0xFF, 0x06);
    w8(a, 0x83, r8(a, 0x83) | 0x04);
    w8(a, 0xFF, 0x07); w8(a, 0x81, 0x01); w8(a, 0x80, 0x01);
    w8(a, 0x94, 0x6B); w8(a, 0x83, 0x00);
    if (!wait_bit(a, 0x83, 0xFF)) return false;      // wait until reg 0x83 != 0
    w8(a, 0x83, 0x01);
    uint8_t tmp = r8(a, 0x92);
    *count = tmp & 0x7F;
    *is_aperture = (tmp >> 7) & 0x01;
    w8(a, 0x81, 0x00); w8(a, 0xFF, 0x06);
    w8(a, 0x83, r8(a, 0x83) & ~0x04);
    w8(a, 0xFF, 0x01); w8(a, 0x00, 0x01); w8(a, 0xFF, 0x00); w8(a, 0x80, 0x00);
    return true;
}

// Single ranging with a specific SYSRANGE_START "vhv init" flavour — used by the two
// reference-calibration steps (VHV then phase cal).
static bool single_ref_cal(uint8_t a, uint8_t vhv_init_byte)
{
    w8(a, SYSRANGE_START, 0x01 | vhv_init_byte);
    if (!wait_bit(a, RESULT_INTERRUPT_STATUS, 0x07)) return false;
    w8(a, SYSTEM_INTERRUPT_CLEAR, 0x01);
    w8(a, SYSRANGE_START, 0x00);
    return true;
}

static esp_err_t l0x_init(l0x_state_t *st)
{
    uint8_t a = st->addr;

    if (r8(a, IDENTIFICATION_MODEL_ID) != 0xEE) {
        ESP_LOGW(TAG, "model id mismatch at 0x%02x — not a VL53L0X (a VL53L1X needs type vl53l1x)", a);
        return ESP_ERR_NOT_SUPPORTED;
    }

    w8(a, 0x88, 0x00);                                       // I2C standard mode

    w8(a, 0x80, 0x01); w8(a, 0xFF, 0x01); w8(a, 0x00, 0x00);
    st->stop_variable = r8(a, 0x91);
    w8(a, 0x00, 0x01); w8(a, 0xFF, 0x00); w8(a, 0x80, 0x00);

    // Disable SIGNAL_RATE_MSRC and SIGNAL_RATE_PRE_RANGE limit checks; set the final range
    // signal rate limit to 0.25 MCPS (fixed point 9.7: 0.25 * 128 = 32).
    w8(a, MSRC_CONFIG_CONTROL, r8(a, MSRC_CONFIG_CONTROL) | 0x12);
    w16(a, FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, 32);
    w8(a, SYSTEM_SEQUENCE_CONFIG, 0xFF);

    // Reference SPAD management: enable the NVM-designated set starting from the right offset.
    uint8_t spad_count; bool spad_is_aperture;
    if (!get_spad_info(a, &spad_count, &spad_is_aperture)) return ESP_ERR_TIMEOUT;
    uint8_t ref_spad_map[6];
    if (bus_i2c_read_reg(a, GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6) != ESP_OK)
        return ESP_FAIL;
    w8(a, 0xFF, 0x01);
    w8(a, DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00);
    w8(a, DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C);
    w8(a, 0xFF, 0x00);
    w8(a, GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4);
    uint8_t first_spad = spad_is_aperture ? 12 : 0, enabled = 0;
    for (int i = 0; i < 48; i++) {
        if (i < first_spad || enabled == spad_count)
            ref_spad_map[i / 8] &= ~(1 << (i % 8));
        else if ((ref_spad_map[i / 8] >> (i % 8)) & 0x01)
            enabled++;
    }
    if (w_multi(a, GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6) != ESP_OK) return ESP_FAIL;

    for (size_t i = 0; i < sizeof(k_tuning) / sizeof(k_tuning[0]); i++)
        w8(a, k_tuning[i][0], k_tuning[i][1]);

    // Interrupt on new sample, active low (matches the Pololu default), and clear it.
    w8(a, SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04);
    w8(a, GPIO_HV_MUX_ACTIVE_HIGH, r8(a, GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10);
    w8(a, SYSTEM_INTERRUPT_CLEAR, 0x01);

    // Reference calibration (VHV then phase), then restore the ranging sequence steps.
    // Default ~33 ms timing budget is kept as-is — fine for the 40 ms poll floor.
    w8(a, SYSTEM_SEQUENCE_CONFIG, 0x01);
    if (!single_ref_cal(a, 0x40)) return ESP_ERR_TIMEOUT;
    w8(a, SYSTEM_SEQUENCE_CONFIG, 0x02);
    if (!single_ref_cal(a, 0x00)) return ESP_ERR_TIMEOUT;
    w8(a, SYSTEM_SEQUENCE_CONFIG, 0xE8);

    // Back-to-back continuous ranging.
    w8(a, 0x80, 0x01); w8(a, 0xFF, 0x01); w8(a, 0x00, 0x00);
    w8(a, 0x91, st->stop_variable);
    w8(a, 0x00, 0x01); w8(a, 0xFF, 0x00); w8(a, 0x80, 0x00);
    w8(a, SYSRANGE_START, 0x02);

    st->initialised = true;
    st->has_last = false;
    ESP_LOGI(TAG, "initialised at 0x%02x (continuous ranging, ~33ms budget)", a);
    return ESP_OK;
}

// ── read ─────────────────────────────────────────────────────────────────────
static float clamp_dist(const sensor_cfg_t *cfg, float mm)
{
    if (cfg->dist_min_mm != 0.0f && mm < cfg->dist_min_mm) mm = cfg->dist_min_mm;
    if (cfg->dist_max_mm != 0.0f && mm > cfg->dist_max_mm) mm = cfg->dist_max_mm;
    return mm;
}

static esp_err_t l0x_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;
    uint8_t addr = cfg->addr ? cfg->addr : L0X_ADDR_DEFAULT;

    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    l0x_state_t *st = state_for(addr, cfg->mux_addr, cfg->mux_channel);
    if (!st) return ESP_ERR_NO_MEM;

    if (!st->initialised) {
        err = l0x_init(st);
        if (err != ESP_OK) return err;
    }

    if (r8(addr, RESULT_INTERRUPT_STATUS) & 0x07) {
        uint16_t mm = r16(addr, RESULT_RANGE_STATUS + 10);
        // Device range status lives in bits 6:3 of the same result block's first byte — and
        // was previously ignored, trusting mm unconditionally. Like the VL53L1X, this chip
        // ranges by phase: beyond its unambiguous distance the measurement wraps and it
        // reports an aliased *shorter* mm with a phase-fail status (6/9) rather than the
        // 8190 sentinel — trusting those made the reading decrease as real distance grew.
        //   11      = range complete (valid)          → accept
        //   8, 10   = valid but min-range clipped     → accept
        //   6, 9    = phase fail (wrap/ambiguous), plus anything else (no target, hw)
        //             → out of range: report the 8190 "infinity" sentinel, which clamp_dist
        //               caps to dist_max_mm when configured — same as a native far miss.
        uint8_t devstat = (r8(addr, RESULT_RANGE_STATUS) >> 3) & 0x0F;
        bool accepted = (devstat == 11 || devstat == 8 || devstat == 10);
        w8(addr, SYSTEM_INTERRUPT_CLEAR, 0x01);
        if (mm == 0) {                        // no valid return at all — treat as a failed poll
            st->initialised = st->has_last;   // keep going if it worked before; else re-init
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint16_t raw_mm = mm;
        if (!accepted) mm = 8190;
        // Range diagnostics, at most twice a second (mirrors drv_vl53l1x.c) — shows the raw
        // device status and measured vs reported distance on the serial log. Gated behind the
        // web's Settings > verbose sensor debug toggle (was unconditional serial-log spam).
        static int64_t s_last_log_us;
        int64_t now_us = esp_timer_get_time();
        if (sensor_get_verbose_debug() && now_us - s_last_log_us > 500 * 1000) {
            s_last_log_us = now_us;
            ESP_LOGI(TAG, "devstat=%u raw=%umm -> %.0fmm%s", devstat, raw_mm,
                     clamp_dist(cfg, (float)mm), accepted ? "" : " [rejected->max]");
        }
        // 8190/8191 = out of range ("infinity"); clamp_dist caps it to dist_max_mm when set.
        st->last_mm = clamp_dist(cfg, (float)mm);
        st->has_last = true;
        st->last_fresh_us = esp_timer_get_time();
    } else if (!st->has_last) {
        return ESP_ERR_TIMEOUT;               // first sample not ready yet
    } else if (esp_timer_get_time() - st->last_fresh_us > L0X_STALE_US) {
        // No new sample for over a second — the sensor stopped delivering. Surface an error
        // (instead of the frozen cached value) and force a re-init on the next poll.
        st->initialised = false;
        st->has_last = false;
        return ESP_ERR_TIMEOUT;
    }
    // No new sample this poll (ranging cadence ~33ms): repeat the last reading rather than
    // erroring — the scheduler treats an error as a failed poll and the dashboard flags it.

    out[0] = st->last_mm;
    *out_count = 1;
    return ESP_OK;
}

static int l0x_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    if (max < 1) return 0;
    names[0] = "dist";
    return 1;
}

const sensor_driver_t drv_vl53l0x = {
    .type = "vl53l0x",
    .probe = NULL,
    .read = l0x_read,
    .describe = l0x_describe,
};
