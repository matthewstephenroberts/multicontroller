// drv_m5_8angle.c — M5Stack 8Angle Unit (STM32-based I2C unit, 8x potentiometer knobs, a
// physical slide switch, and 9 RGB LEDs — one per knob plus one beside the switch; default
// address 0x43).
//
// Register map + protocol verified against the official M5Unit-8Angle Arduino library source
// (M5_ANGLE8.h/.cpp, github.com/m5stack/M5Unit-8Angle) — there is no public plain-text
// datasheet, only a PDF protocol doc this wasn't cross-checked against.
//
//   0x00-0x0F  12-bit ADC per knob, 2 bytes each, little-endian, register = 0x00 + ch*2
//   0x10-0x17  8-bit (downsampled) ADC per knob, 1 byte each — not read here, the 12-bit values
//              at 0x00 are strictly more precise
//   0x20       physical slide switch state, 1 byte (0/1) — exposed as the 9th value "switch"
//   0x30+ch*4  RGB LED control, write [R, G, B, brightness] per LED (9 LEDs; 0-7 sit under the
//              knobs, 8 beside the switch) — used by the optional knob-value visualisation
//              below, driven by the sensor's `led` config field (0 = off, else brightness %)
//   0xFE       firmware version (read-only, unused here)
//   0xFF       I2C address (read/write, unused here — set via the unit's own configuration)
//
// Read shape: M5_ANGLE8::readBytes() does beginTransmission→write(reg)→ENDTRANSMISSION (a STOP)
// → THEN a separate requestFrom() read — a plain write-with-STOP followed by a distinct read
// transaction, not a repeated-start combined read. Two earlier versions of this driver got this
// wrong in different ways: first a single 16-byte burst read across all 8 channels (assumed
// burst-readable since the registers are contiguous — never verified), then per-channel reads
// but still via bus_i2c_read_reg() (repeated-start, no STOP between the register-select write
// and the data read). Either shape can desync/hang this unit's I2C slave firmware, wedging the
// whole bus (clock-stretch/SDA held low) and taking every other device on Port.A down with it —
// not just failing to read this one. bus_i2c_write()+bus_i2c_read() (separate transactions,
// matching the vendor library exactly) is what's actually verified to work. LED writes are a
// single plain write [reg, R, G, B, bright] — same shape as the library's writeBytes(), no
// read leg, so no repeated-start concern.
//
// I2C clock: undocumented in the library; treat as standard-mode (100kHz) safe, matching
// bus_i2c's board-wide default. Poll rate: see sensor.c's sensor_poll_floor_ms() — kept
// conservative; this unit's slave firmware is not a dedicated ASIC and can misbehave under
// sustained rapid polling, which is also why the LED visualisation below only writes an LED
// when its knob's value has actually moved (a steady panel costs zero extra transactions).
#include "sensor.h"
#include "bus_i2c.h"
#include "i2c_mux.h"
#include "esp_timer.h"

#define ANGLE8_ADDR_DEFAULT 0x43
#define ANGLE8_REG_ANALOG12 0x00
#define ANGLE8_REG_SWITCH   0x20
#define ANGLE8_REG_RGB      0x30
#define ANGLE8_CHANNELS     8
#define ANGLE8_ADC_MAX      4095

// Knob movement below this many counts doesn't rewrite that knob's LED — the pots jitter a few
// counts at rest, and rewriting 8 LEDs every poll is exactly the sustained transaction load this
// unit's slave firmware tolerates poorly (see file-level comment). ~32/4095 ≈ 0.8% of travel.
#define ANGLE8_LED_DELTA    32

// One retried write-reg+read pair (see angle8_read_channel's comment for why retries exist).
static esp_err_t angle8_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2 && err != ESP_OK; attempt++) {
        err = bus_i2c_write(addr, &reg, 1);
        if (err == ESP_OK) err = bus_i2c_read(addr, buf, len);
    }
    return err;
}

// Map a knob's 0-4095 value onto a blue→red hue sweep (low = blue 240°, high = red 0°) at full
// saturation. Only the hue third of full HSV→RGB is needed since s and v are fixed.
static void angle8_value_colour(uint16_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned hue = 240u - (unsigned)v * 240u / ANGLE8_ADC_MAX;   // 240 (blue) .. 0 (red)
    unsigned seg = hue / 60, rem = hue % 60;
    uint8_t rise = (uint8_t)(rem * 255u / 60u), fall = (uint8_t)(255u - rise);
    switch (seg) {
    case 0:  *r = 255;  *g = rise; *b = 0;    break;   // red → yellow
    case 1:  *r = fall; *g = 255;  *b = 0;    break;   // yellow → green
    case 2:  *r = 0;    *g = 255;  *b = rise; break;   // green → cyan
    default: *r = 0;    *g = fall; *b = 255;  break;   // cyan → blue (seg 3, hue 180-240)
    }
}

// Last LED state actually written per sensor, so a poll only writes LEDs whose knob moved (or
// whose brightness setting changed / visualisation just turned off). Matched by sensor id —
// same slot pattern as drv_vk36n16.c's hold table.
typedef struct {
    int      id;
    bool     used;
    bool     dark;                       // true once LEDs were blanked after led→0 (or on sleep)
    bool     sleeping;                   // auto-sleep (led_sleep_s) blanked the LEDs; wake on movement
    int64_t  last_move_us;               // last time any knob moved beyond ANGLE8_LED_DELTA
    uint8_t  bright;                     // brightness the entries below were written with
    int32_t  lastv[ANGLE8_CHANNELS];     // -1 = never written
} led_state_t;
static led_state_t s_led[MC_MAX_SENSORS];

static led_state_t *led_slot(int id)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_led[i].used && s_led[i].id == id) return &s_led[i];
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (!s_led[i].used) {
            s_led[i] = (led_state_t){ .id = id, .used = true, .last_move_us = esp_timer_get_time() };
            for (int c = 0; c < ANGLE8_CHANNELS; c++) s_led[i].lastv[c] = -1;
            return &s_led[i];
        }
    return NULL;   // table full (shouldn't happen) — visualisation silently skipped
}

// Single LED write: [reg, R, G, B, brightness] in one plain transaction, exactly the vendor
// library's setLEDColor() shape. Best-effort — an LED that misses an update self-corrects the
// next time its knob moves, so failures aren't worth failing the whole poll over.
static void angle8_led_write(uint8_t addr, int ch, uint8_t r, uint8_t g, uint8_t b, uint8_t bright)
{
    uint8_t buf[5] = { (uint8_t)(ANGLE8_REG_RGB + ch * 4), r, g, b, bright };
    bus_i2c_write(addr, buf, sizeof(buf));
}

// Update the per-knob LEDs from this poll's values. cfg->led (0-100) is both the on/off switch
// and the brightness: 0 blanks all 8 once and then stays silent; >0 colours each knob's LED by
// its value (blue = low, red = high), rewriting only LEDs whose knob actually moved.
// cfg->led_sleep_s (-1 = never) additionally blanks the LEDs after that many seconds with no
// knob movement, waking instantly on the next movement — same idea as the display's own
// auto-sleep, for a panel that mostly sits untouched.
static void angle8_leds_update(const sensor_cfg_t *cfg, uint8_t addr, const float *vals)
{
    led_state_t *st = led_slot(cfg->id);
    if (!st) return;

    if (cfg->led == 0) {
        if (!st->dark) {
            for (int ch = 0; ch < ANGLE8_CHANNELS; ch++) angle8_led_write(addr, ch, 0, 0, 0, 0);
            for (int ch = 0; ch < ANGLE8_CHANNELS; ch++) st->lastv[ch] = -1;
            st->dark = true;
        }
        st->sleeping = false;
        return;
    }

    // Movement detection for auto-sleep: any knob beyond the same delta the per-LED updates
    // use. Compared against the last *written* values — frozen while sleeping, so a knob turned
    // during sleep still registers as movement and wakes the panel.
    int64_t now = esp_timer_get_time();
    bool moved = false;
    for (int ch = 0; ch < ANGLE8_CHANNELS; ch++) {
        int32_t v = (int32_t)vals[ch];
        if (st->lastv[ch] < 0 ||
            v <= st->lastv[ch] - ANGLE8_LED_DELTA || v >= st->lastv[ch] + ANGLE8_LED_DELTA) {
            moved = true;
            break;
        }
    }
    if (moved) st->last_move_us = now;

    if (st->sleeping) {
        if (!moved) return;
        st->sleeping = false;   // wake: st->dark is still true from sleep entry → full rewrite below
    } else if (cfg->led_sleep_s >= 0 && !moved &&
               now - st->last_move_us > (int64_t)cfg->led_sleep_s * 1000000) {
        for (int ch = 0; ch < ANGLE8_CHANNELS; ch++) angle8_led_write(addr, ch, 0, 0, 0, 0);
        st->dark = true;        // deliberately NOT resetting lastv — sleep-time movement detection needs it
        st->sleeping = true;
        return;
    }

    bool bright_changed = (st->bright != cfg->led) || st->dark;
    st->bright = cfg->led;
    st->dark = false;

    for (int ch = 0; ch < ANGLE8_CHANNELS; ch++) {
        int32_t v = (int32_t)vals[ch];
        if (!bright_changed && st->lastv[ch] >= 0 &&
            v > st->lastv[ch] - ANGLE8_LED_DELTA && v < st->lastv[ch] + ANGLE8_LED_DELTA)
            continue;
        uint8_t r, g, b;
        angle8_value_colour((uint16_t)v, &r, &g, &b);
        angle8_led_write(addr, ch, r, g, b, cfg->led);
        st->lastv[ch] = v;
    }
}

// Reading smoothing: an exponential moving average over the 8 knob channels, applied AFTER the
// LED visualisation above so the LEDs stay an immediate, snappy reflection of the physical knob
// position (they have their own separate ANGLE8_LED_DELTA filtering already, for write-count
// reasons, not reading stability) — only the *reported* value (dashboard/LEGO field) is
// smoothed. cfg->knob_smooth is the EMA weight given to the *old* value (0 = off/immediate, up
// to 0.95 = heavy smoothing), same semantics as sensor_transform.c's colour_smooth. Kept as its
// own small per-sensor table here (matching this file's own led_slot pattern) rather than
// sensor_transform.c's colour-scoped one — that helper is sized/keyed for the colour drivers'
// call sites, and this needs its own 8-channel state anyway.
typedef struct { int id; bool used; bool init; float buf[ANGLE8_CHANNELS]; } knob_smooth_t;
static knob_smooth_t s_ksmooth[MC_MAX_SENSORS];

static void angle8_smooth_apply(int sensor_id, float alpha, float *vals)
{
    if (alpha <= 0.0f) return;
    knob_smooth_t *st = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_ksmooth[i].used && s_ksmooth[i].id == sensor_id) { st = &s_ksmooth[i]; break; }
    if (!st)
        for (int i = 0; i < MC_MAX_SENSORS; i++)
            if (!s_ksmooth[i].used) { st = &s_ksmooth[i]; st->used = true; st->id = sensor_id; st->init = false; break; }
    if (!st) return;   // table full (shouldn't happen) — leave values unsmoothed rather than block
    if (!st->init) { for (int i = 0; i < ANGLE8_CHANNELS; i++) st->buf[i] = vals[i]; st->init = true; }
    else           { for (int i = 0; i < ANGLE8_CHANNELS; i++) st->buf[i] = st->buf[i] * alpha + vals[i] * (1.0f - alpha); }
    for (int i = 0; i < ANGLE8_CHANNELS; i++) vals[i] = st->buf[i];
}

static esp_err_t angle8_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < ANGLE8_CHANNELS + 1) return ESP_ERR_INVALID_SIZE;
    uint8_t addr = cfg->addr ? cfg->addr : ANGLE8_ADDR_DEFAULT;

    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    // Per-channel reads, each retried once — the scheduler has no retry of its own, and a
    // single transient NACK across this many transactions per poll used to throw away the
    // whole poll's worth of already-good values (surfacing as intermittent dropped readings).
    for (int ch = 0; ch < ANGLE8_CHANNELS; ch++) {
        uint8_t d[2];
        if ((err = angle8_read_reg(addr, (uint8_t)(ANGLE8_REG_ANALOG12 + ch * 2), d, sizeof(d))) != ESP_OK)
            return err;
        uint16_t raw = (uint16_t)(d[0] | (d[1] << 8));   // little-endian, per M5_ANGLE8::getAnalogInput()
        out[ch] = (float)(cfg->knob_invert ? (ANGLE8_ADC_MAX - raw) : raw);
    }

    uint8_t sw = 0;
    if ((err = angle8_read_reg(addr, ANGLE8_REG_SWITCH, &sw, 1)) != ESP_OK) return err;
    out[ANGLE8_CHANNELS] = sw ? 1.0f : 0.0f;

    angle8_leds_update(cfg, addr, out);      // raw values — LED feedback stays immediate
    angle8_smooth_apply(cfg->id, cfg->knob_smooth, out);   // then smooth what's actually reported

    *out_count = ANGLE8_CHANNELS + 1;
    return ESP_OK;
}

static int angle8_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    if (max < ANGLE8_CHANNELS + 1) return 0;
    static const char *n[ANGLE8_CHANNELS] = { "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7" };
    for (int i = 0; i < ANGLE8_CHANNELS; i++) names[i] = n[i];
    names[ANGLE8_CHANNELS] = "switch";
    return ANGLE8_CHANNELS + 1;
}

const sensor_driver_t drv_m5_8angle = {
    .type = "m5_8angle",
    .probe = NULL,
    .read = angle8_read,
    .describe = angle8_describe,
};
