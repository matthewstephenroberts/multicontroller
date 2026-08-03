// drv_vl53l1x.c — named driver: ST VL53L1X time-of-flight distance sensor (I2C 0x29).
// Output: dist (mm). Ported from the Pololu VL53L1X Arduino library (which mirrors ST's
// VL53L1X API): software reset → DataInit → StaticInit (long range, 50 ms budget) →
// continuous ranging, then per-poll read with low-power-auto manual calibration + DSS.
#include "sensor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

static const char *TAG = "vl53l1x";

#define VL_ADDR_DEFAULT 0x29
#define TARGET_RATE  0x0A00
#define TIMING_GUARD 4528

// 16-bit register addresses (subset used; from the ST register map).
enum {
    SOFT_RESET = 0x0000, VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND = 0x0008, VHV_CONFIG__INIT = 0x000B,
    OSC_MEASURED__FAST_OSC__FREQUENCY = 0x0006,
    ALGO__PART_TO_PART_RANGE_OFFSET_MM = 0x001E, MM_CONFIG__OUTER_OFFSET_MM = 0x0022,
    DSS_CONFIG__TARGET_TOTAL_RATE_MCPS = 0x0024, PAD_I2C_HV__EXTSUP_CONFIG = 0x002E,
    GPIO__TIO_HV_STATUS = 0x0031, SIGMA_ESTIMATOR__EFFECTIVE_PULSE_WIDTH_NS = 0x0036,
    SIGMA_ESTIMATOR__EFFECTIVE_AMBIENT_WIDTH_NS = 0x0037, ALGO__CROSSTALK_COMPENSATION_VALID_HEIGHT_MM = 0x0039,
    ALGO__RANGE_IGNORE_VALID_HEIGHT_MM = 0x003E, ALGO__RANGE_MIN_CLIP = 0x003F,
    ALGO__CONSISTENCY_CHECK__TOLERANCE = 0x0040, CAL_CONFIG__VCSEL_START = 0x0047,
    PHASECAL_CONFIG__TIMEOUT_MACROP = 0x004B, PHASECAL_CONFIG__OVERRIDE = 0x004D,
    DSS_CONFIG__ROI_MODE_CONTROL = 0x004F, SYSTEM__THRESH_RATE_HIGH = 0x0050, SYSTEM__THRESH_RATE_LOW = 0x0052,
    DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT = 0x0054, DSS_CONFIG__APERTURE_ATTENUATION = 0x0057,
    MM_CONFIG__TIMEOUT_MACROP_A = 0x005A, MM_CONFIG__TIMEOUT_MACROP_B = 0x005C,
    RANGE_CONFIG__TIMEOUT_MACROP_A = 0x005E, RANGE_CONFIG__VCSEL_PERIOD_A = 0x0060,
    RANGE_CONFIG__TIMEOUT_MACROP_B = 0x0061, RANGE_CONFIG__VCSEL_PERIOD_B = 0x0063,
    RANGE_CONFIG__SIGMA_THRESH = 0x0064, RANGE_CONFIG__MIN_COUNT_RATE_RTN_LIMIT_MCPS = 0x0066,
    RANGE_CONFIG__VALID_PHASE_HIGH = 0x0069, SYSTEM__INTERMEASUREMENT_PERIOD = 0x006C,
    SYSTEM__GROUPED_PARAMETER_HOLD_0 = 0x0071, SYSTEM__SEED_CONFIG = 0x0077,
    SD_CONFIG__WOI_SD0 = 0x0078, SD_CONFIG__WOI_SD1 = 0x0079, SD_CONFIG__INITIAL_PHASE_SD0 = 0x007A,
    SD_CONFIG__INITIAL_PHASE_SD1 = 0x007B, SYSTEM__GROUPED_PARAMETER_HOLD_1 = 0x007C,
    SD_CONFIG__QUANTIFIER = 0x007E, SYSTEM__SEQUENCE_CONFIG = 0x0081, SYSTEM__GROUPED_PARAMETER_HOLD = 0x0082,
    SYSTEM__INTERRUPT_CLEAR = 0x0086, SYSTEM__MODE_START = 0x0087, RESULT__RANGE_STATUS = 0x0089,
    PHASECAL_RESULT__VCSEL_START = 0x00D8, RESULT__OSC_CALIBRATE_VAL = 0x00DE,
    FIRMWARE__SYSTEM_STATUS = 0x00E5, IDENTIFICATION__MODEL_ID = 0x010F,
};

// Native measuring range per distance mode (ST datasheet: short ~1.3m, long ~4m).
static const struct { float min_mm, max_mm; } k_native_range[2] = {
    { 40.0f, 1300.0f },   // short
    { 40.0f, 4000.0f },   // long
};

typedef struct {
    bool     used;
    uint8_t  addr, mux;
    int8_t   ch;
    uint8_t  mode;                 // dist_mode this state was initialised with
    uint16_t fast_osc_freq, osc_cal_val;
    bool     started, calibrated;
    uint8_t  saved_vhv_init, saved_vhv_timeout;
    float    last_dist;            // last accepted reading, repeated between ranging cycles
    bool     has_last;
    int64_t  last_fresh_us;        // when the last fresh measurement arrived (staleness watchdog)
} vl_state_t;

// Continuous ranging produces a measurement every budget (20/50ms); a full second without one
// means the sensor stopped talking — error out and re-init rather than serving a stale value.
#define VL_STALE_US (1000 * 1000)
static vl_state_t s_state[MC_MAX_SENSORS];

// ── I2C with 16-bit register pointers (write-pointer then read) ─────────────
static esp_err_t v_write(uint8_t addr, uint16_t reg, const uint8_t *data, int n)
{
    uint8_t b[6];
    b[0] = reg >> 8; b[1] = reg & 0xFF;
    for (int i = 0; i < n; i++) b[2 + i] = data[i];
    return bus_i2c_write(addr, b, n + 2);
}
static esp_err_t v_read(uint8_t addr, uint16_t reg, uint8_t *buf, int n)
{
    uint8_t p[2] = { reg >> 8, reg & 0xFF };
    esp_err_t err = bus_i2c_write(addr, p, 2);
    if (err != ESP_OK) return err;
    return bus_i2c_read(addr, buf, n);
}
static esp_err_t wr8(uint8_t a, uint16_t r, uint8_t v) { return v_write(a, r, &v, 1); }
static esp_err_t wr16(uint8_t a, uint16_t r, uint16_t v) { uint8_t d[2] = {v >> 8, v}; return v_write(a, r, d, 2); }
static esp_err_t wr32(uint8_t a, uint16_t r, uint32_t v) { uint8_t d[4] = {v >> 24, v >> 16, v >> 8, v}; return v_write(a, r, d, 4); }
static uint8_t  rd8(uint8_t a, uint16_t r)  { uint8_t v = 0; v_read(a, r, &v, 1); return v; }
static uint16_t rd16(uint8_t a, uint16_t r) { uint8_t d[2] = {0}; v_read(a, r, d, 2); return (d[0] << 8) | d[1]; }

// ── timeout encode/decode + macro period (verbatim from the library) ────────
static uint32_t calc_macro_period(uint16_t fast_osc_freq, uint8_t vcsel_period)
{
    uint32_t pll_period_us = ((uint32_t)0x01 << 30) / fast_osc_freq;
    uint8_t  vcsel_pclks = (vcsel_period + 1) << 1;
    uint32_t macro = (uint32_t)2304 * pll_period_us;
    macro >>= 6; macro *= vcsel_pclks; macro >>= 6;
    return macro;
}
static uint32_t timeout_us_to_mclks(uint32_t timeout_us, uint32_t macro_us)
{
    return (((uint32_t)timeout_us << 12) + (macro_us >> 1)) / macro_us;
}
static uint16_t encode_timeout(uint32_t mclks)
{
    if (mclks == 0) return 0;
    uint32_t ls = mclks - 1; uint16_t ms = 0;
    while ((ls & 0xFFFFFF00) > 0) { ls >>= 1; ms++; }
    return (ms << 8) | (ls & 0xFF);
}

static void set_timing_budget(uint8_t a, uint16_t fast_osc, uint32_t budget_us)
{
    if (budget_us <= TIMING_GUARD) return;
    uint32_t range_us = budget_us - TIMING_GUARD;
    if (range_us > 1100000) return;
    range_us /= 2;
    uint32_t macro = calc_macro_period(fast_osc, rd8(a, RANGE_CONFIG__VCSEL_PERIOD_A));
    uint32_t phasecal = timeout_us_to_mclks(1000, macro);
    if (phasecal > 0xFF) phasecal = 0xFF;
    wr8(a, PHASECAL_CONFIG__TIMEOUT_MACROP, phasecal);
    wr16(a, MM_CONFIG__TIMEOUT_MACROP_A, encode_timeout(timeout_us_to_mclks(1, macro)));
    wr16(a, RANGE_CONFIG__TIMEOUT_MACROP_A, encode_timeout(timeout_us_to_mclks(range_us, macro)));
    macro = calc_macro_period(fast_osc, rd8(a, RANGE_CONFIG__VCSEL_PERIOD_B));
    wr16(a, MM_CONFIG__TIMEOUT_MACROP_B, encode_timeout(timeout_us_to_mclks(1, macro)));
    wr16(a, RANGE_CONFIG__TIMEOUT_MACROP_B, encode_timeout(timeout_us_to_mclks(range_us, macro)));
}

// VL53L1_DataInit + StaticInit + distance-mode preset + timing budget + continuous start.
// mode 0 = short range (VCSEL/phase preset per ST's SetDistanceMode(Short), ~20 ms budget,
// good to ~1.3 m); mode 1 = long range (the original preset here, 50 ms budget, ~4 m) — the
// longer budget is why long range needs a slower poll interval (see config_store's floor).
static esp_err_t vl_init(vl_state_t *st, uint8_t a, uint8_t mode)
{
    if (rd16(a, IDENTIFICATION__MODEL_ID) != 0xEACC) return ESP_ERR_INVALID_RESPONSE;

    wr8(a, SOFT_RESET, 0x00);
    esp_rom_delay_us(100);
    wr8(a, SOFT_RESET, 0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
    for (int i = 0; i < 100 && (rd8(a, FIRMWARE__SYSTEM_STATUS) & 0x01) == 0; i++) vTaskDelay(pdMS_TO_TICKS(1));

    wr8(a, PAD_I2C_HV__EXTSUP_CONFIG, rd8(a, PAD_I2C_HV__EXTSUP_CONFIG) | 0x01);  // 2V8 mode
    st->fast_osc_freq = rd16(a, OSC_MEASURED__FAST_OSC__FREQUENCY);
    st->osc_cal_val   = rd16(a, RESULT__OSC_CALIBRATE_VAL);

    wr16(a, DSS_CONFIG__TARGET_TOTAL_RATE_MCPS, TARGET_RATE);
    wr8(a, GPIO__TIO_HV_STATUS, 0x02);
    wr8(a, SIGMA_ESTIMATOR__EFFECTIVE_PULSE_WIDTH_NS, 8);
    wr8(a, SIGMA_ESTIMATOR__EFFECTIVE_AMBIENT_WIDTH_NS, 16);
    wr8(a, ALGO__CROSSTALK_COMPENSATION_VALID_HEIGHT_MM, 0x01);
    wr8(a, ALGO__RANGE_IGNORE_VALID_HEIGHT_MM, 0xFF);
    wr8(a, ALGO__RANGE_MIN_CLIP, 0);
    wr8(a, ALGO__CONSISTENCY_CHECK__TOLERANCE, 2);
    wr16(a, SYSTEM__THRESH_RATE_HIGH, 0x0000);
    wr16(a, SYSTEM__THRESH_RATE_LOW, 0x0000);
    wr8(a, DSS_CONFIG__APERTURE_ATTENUATION, 0x38);
    wr16(a, RANGE_CONFIG__SIGMA_THRESH, 360);
    wr16(a, RANGE_CONFIG__MIN_COUNT_RATE_RTN_LIMIT_MCPS, 192);
    wr8(a, SYSTEM__GROUPED_PARAMETER_HOLD_0, 0x01);
    wr8(a, SYSTEM__GROUPED_PARAMETER_HOLD_1, 0x01);
    wr8(a, SD_CONFIG__QUANTIFIER, 2);
    wr8(a, SYSTEM__GROUPED_PARAMETER_HOLD, 0x00);
    wr8(a, SYSTEM__SEED_CONFIG, 1);
    wr8(a, SYSTEM__SEQUENCE_CONFIG, 0x8B);
    wr16(a, DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, 200 << 8);
    wr8(a, DSS_CONFIG__ROI_MODE_CONTROL, 2);

    // Distance-mode preset (timing + dynamic config), then apply the budget.
    uint32_t budget_us;
    if (mode == 0) {                                   // short
        wr8(a, RANGE_CONFIG__VCSEL_PERIOD_A, 0x07);
        wr8(a, RANGE_CONFIG__VCSEL_PERIOD_B, 0x05);
        wr8(a, RANGE_CONFIG__VALID_PHASE_HIGH, 0x38);
        wr8(a, SD_CONFIG__WOI_SD0, 0x07);
        wr8(a, SD_CONFIG__WOI_SD1, 0x05);
        wr8(a, SD_CONFIG__INITIAL_PHASE_SD0, 6);
        wr8(a, SD_CONFIG__INITIAL_PHASE_SD1, 6);
        budget_us = 20000;
    } else {                                            // long
        wr8(a, RANGE_CONFIG__VCSEL_PERIOD_A, 0x0F);
        wr8(a, RANGE_CONFIG__VCSEL_PERIOD_B, 0x0D);
        wr8(a, RANGE_CONFIG__VALID_PHASE_HIGH, 0xB8);
        wr8(a, SD_CONFIG__WOI_SD0, 0x0F);
        wr8(a, SD_CONFIG__WOI_SD1, 0x0D);
        wr8(a, SD_CONFIG__INITIAL_PHASE_SD0, 14);
        wr8(a, SD_CONFIG__INITIAL_PHASE_SD1, 14);
        budget_us = 50000;
    }
    set_timing_budget(a, st->fast_osc_freq, budget_us);

    wr16(a, ALGO__PART_TO_PART_RANGE_OFFSET_MM, rd16(a, MM_CONFIG__OUTER_OFFSET_MM) * 4);

    // startContinuous(budget_ms): back-to-back measurements at the mode's own budget.
    uint32_t budget_ms = budget_us / 1000;
    wr32(a, SYSTEM__INTERMEASUREMENT_PERIOD, budget_ms * st->osc_cal_val);
    wr8(a, SYSTEM__INTERRUPT_CLEAR, 0x01);
    wr8(a, SYSTEM__MODE_START, 0x40);
    st->mode = mode;
    st->started = true;
    st->calibrated = false;
    return ESP_OK;
}

static void manual_calibration(vl_state_t *st, uint8_t a)
{
    st->saved_vhv_init = rd8(a, VHV_CONFIG__INIT);
    st->saved_vhv_timeout = rd8(a, VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND);
    wr8(a, VHV_CONFIG__INIT, st->saved_vhv_init & 0x7F);
    wr8(a, VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND, (st->saved_vhv_timeout & 0x03) + (3 << 2));
    wr8(a, PHASECAL_CONFIG__OVERRIDE, 0x01);
    wr8(a, CAL_CONFIG__VCSEL_START, rd8(a, PHASECAL_RESULT__VCSEL_START));
}

static vl_state_t *state_of(const sensor_cfg_t *cfg, uint8_t a)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_state[i].used && s_state[i].addr == a && s_state[i].mux == cfg->mux_addr && s_state[i].ch == cfg->mux_channel)
            return &s_state[i];
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (!s_state[i].used) {
            s_state[i] = (vl_state_t){ .used = true, .addr = a, .mux = cfg->mux_addr, .ch = cfg->mux_channel };
            return &s_state[i];
        }
    return &s_state[0];
}

static esp_err_t vl_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;
    uint8_t a = cfg->addr ? cfg->addr : VL_ADDR_DEFAULT;
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    vl_state_t *st = state_of(cfg, a);
    if (!st->started || st->mode != cfg->dist_mode) {
        if ((err = vl_init(st, a, cfg->dist_mode)) != ESP_OK) { st->used = false; return err; }
    }

    // Non-blocking steady state: continuous ranging produces a measurement every budget
    // (20/50ms) on its own — if this poll lands between completions, repeat the last reading
    // instead of busy-waiting up to a full cycle (which serialised badly with several sensors
    // on one scheduler, same disease drv_as7341.c had). Only the very first reading — nothing
    // cached yet — blocks for a measurement; and if no fresh data arrives for over a second,
    // the sensor stopped talking: error out and force a re-init on the next poll.
    if ((rd8(a, GPIO__TIO_HV_STATUS) & 0x01) != 0) {         // data not ready yet
        if (st->has_last) {
            if (esp_timer_get_time() - st->last_fresh_us > VL_STALE_US) {
                st->started = false;
                st->has_last = false;
                return ESP_ERR_TIMEOUT;
            }
            out[0] = st->last_dist;
            *out_count = 1;
            return ESP_OK;
        }
        bool ready = false;
        for (int i = 0; i < 120; i++) {
            if ((rd8(a, GPIO__TIO_HV_STATUS) & 0x01) == 0) { ready = true; break; }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!ready) return ESP_ERR_TIMEOUT;
    }

    uint8_t r[17];
    if ((err = v_read(a, RESULT__RANGE_STATUS, r, sizeof(r))) != ESP_OK) return err;

    if (!st->calibrated) { manual_calibration(st, a); st->calibrated = true; }

    // updateDSS: keep effective-SPAD selection accurate (low-power-auto)
    uint16_t spads   = (r[3] << 8) | r[4];
    uint16_t ambient = (r[7] << 8) | r[8];
    uint16_t peakx   = (r[15] << 8) | r[16];
    uint16_t finalmm = (r[13] << 8) | r[14];
    if (spads != 0) {
        uint32_t total = (uint32_t)peakx + ambient;
        if (total > 0xFFFF) total = 0xFFFF;
        total <<= 16; total /= spads;
        uint32_t req = total ? (((uint32_t)TARGET_RATE << 16) / total) : 0x8000;
        if (req > 0xFFFF) req = 0xFFFF;
        wr16(a, DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, req);
    } else {
        wr16(a, DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, 0x8000);
    }

    wr8(a, SYSTEM__INTERRUPT_CLEAR, 0x01);

    // apply the library's 2011/2048 ranging gain factor
    float dist = (float)(((uint32_t)finalmm * 2011 + 0x0400) / 0x0800);

    float lo = cfg->dist_min_mm != 0.0f ? cfg->dist_min_mm : k_native_range[cfg->dist_mode ? 1 : 0].min_mm;
    float hi = cfg->dist_max_mm != 0.0f ? cfg->dist_max_mm : k_native_range[cfg->dist_mode ? 1 : 0].max_mm;

    // Gate on RESULT__RANGE_STATUS (r[0], low 5 bits) — previously ignored entirely. The
    // VL53L1X ranges by phase, so beyond its unambiguous distance the measurement WRAPS: the
    // chip flags it but still reports an aliased, much *shorter* finalmm — trusting it made
    // the reading drop as the real distance grew. NOTE these are the raw DEVICE status codes,
    // not the remapped "user" codes ST's ULD / Pololu expose (an earlier gate used the user
    // numbering here and misread device 4 — weak signal, routine past ~40 cm — as wraparound,
    // snapping mid-range readings to max):
    //   9 = range complete (valid), 8 = valid but min-range clipped        → accept
    //   4 = signal fail (weak return), 6 = sigma fail (noisy)              → accept: the
    //       distance is still the best estimate, just noisier — common far from the target
    //       or on dark surfaces; rejecting these truncated usable range to ~0.4 m.
    //   5 = phase out of bounds, 7 = wrapped-target phase inconsistency    → the aliasing
    //       cases this gate exists for: no trustworthy target in range — report the
    //       configured maximum, like the VL53L0X's 8190 sentinel path.
    //   anything else (hardware/test failures)                             → also max.
    uint8_t rs = r[0] & 0x1F;
    bool accepted = (rs == 9 || rs == 8 || rs == 4 || rs == 6);
    if (!accepted) dist = hi;

    // Range diagnostics on the serial log, at most twice a second: raw device status +
    // measured vs reported distance, so a range gate misfire shows up as (say) "rs=5
    // raw=520mm -> 1300mm" instead of having to guess which status the chip emits at a
    // given distance/target. Gated behind the web's Settings > verbose sensor debug toggle
    // instead of unconditional — this used to spam the serial log on every board by default.
    static int64_t s_last_log_us;
    int64_t now_us = esp_timer_get_time();
    if (sensor_get_verbose_debug() && now_us - s_last_log_us > 500 * 1000) {
        s_last_log_us = now_us;
        ESP_LOGI(TAG, "rs=%u raw=%umm -> %.0fmm%s (spads=%u peak=%u amb=%u)",
                 rs, finalmm, dist, accepted ? "" : " [rejected->max]", spads, peakx, ambient);
    }

    // Clamp to this setup's configured range (0/0 = the mode's native range).
    if (dist < lo) dist = lo;
    if (dist > hi) dist = hi;
    st->last_dist = dist;
    st->has_last = true;
    st->last_fresh_us = esp_timer_get_time();
    out[0] = dist;
    *out_count = 1;
    return ESP_OK;
}

static int vl_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    if (max < 1) return 0;
    names[0] = "dist";
    return 1;
}

const sensor_driver_t drv_vl53l1x = {
    .type = "vl53l1x",
    .probe = NULL,
    .read = vl_read,
    .describe = vl_describe,
};
