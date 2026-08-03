// drv_as7341.c — named driver: AMS AS7341 11-channel spectral sensor over I2C (addr 0x39).
// Outputs the 10 useful channels: F1..F8 (visible bands), Clear, NIR (raw counts). Reading
// takes two SMUX integration passes (F1-F4 then F5-F8); the SMUX photodiode maps and read
// sequence are ported from the Adafruit AS7341 library. Pair with the "as_lego" transform
// (sensor_transform.c) for spectral colour classification.
#include "sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bus_i2c.h"
#include "i2c_mux.h"

static const char *TAG = "as7341";

#define AS7341_ADDR_DEFAULT 0x39
#define REG_ENABLE   0x80   // PON=bit0, SP_EN=bit1, SMUXEN=bit4
#define REG_ATIME    0x81
#define REG_WHOAMI   0x92
#define REG_STATUS2  0xA3   // AVALID = bit6
#define REG_CFG0     0xA9   // REG_BANK = bit4 (1 = access 0x60-0x74 registers)
#define REG_CFG1     0xAA   // AGAIN
#define REG_CFG6     0xAF   // SMUX command (bits 4:3)
#define REG_ASTEP_L  0xCA
#define REG_ASTEP_H  0xCB
#define REG_CH0_DATA_L 0x95

// Low-bank registers (only reachable with CFG0.REG_BANK set).
#define REG_CONFIG   0x70   // LED_SEL = bit3 (connect LED control to the LDR pin)
#define REG_LED      0x74   // LED_ACT = bit7, LED_DRIVE = bits 6:0 (current = 4 + 2*drive mA)

// LED brightness (cfg->led, 0-100%) → LED_DRIVE. Capped well below the chip's 258mA maximum:
// 100% maps to drive 60 (= 124mA), plenty for the breakout illumination LED without cooking it.
#define LED_DRIVE_MAX 60

#define EN_PON   0x01
#define EN_SP_EN 0x02
#define EN_SMUX  0x10

// Analog gain: 64x (code 7; gain = 2^(code-1)), with counts scaled back up ×4 below so every
// consumer still sees the original 256x-equivalent scale. At the original AGAIN=256x the
// unfiltered Clear photodiode — which collects the whole spectrum, several times the light of
// any single F-band — pinned at the 10000-count ADC ceiling ((ATIME+1)*(ASTEP+1)) on any
// LED-lit target at working distance, erasing exactly the intensity information that separates
// black/white/silver and pinning reflect% at 100. At 64x, Clear stays in range (scaled
// ceiling 40000) while the F-channels keep plenty of signal. Existing white calibrations and
// taught F-channel refs keep their scale; only previously-clipped Clear values change (they
// now read their true, higher value) — re-Calibrate + re-Teach to benefit.
#define AS7341_AGAIN_CODE  0x07
#define AS7341_COUNT_SCALE 4.0f

// SMUX configuration RAM (registers 0x00..0x13) — photodiode → ADC channel maps.
// Verbatim from Adafruit_AS7341 setup_F1F4_Clear_NIR / setup_F5F8_Clear_NIR.
static const uint8_t SMUX_F1F4[20] = {
    0x30, 0x01, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x50, 0x00,
    0x00, 0x00, 0x20, 0x04, 0x00, 0x30, 0x01, 0x50, 0x00, 0x06,
};
static const uint8_t SMUX_F5F8[20] = {
    0x00, 0x00, 0x00, 0x40, 0x02, 0x00, 0x10, 0x03, 0x50, 0x10,
    0x03, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x50, 0x00, 0x06,
};

// Per-device state. led: last-applied LED brightness (-1 = never applied), so a config change
// from the web takes effect on the next poll without re-running the full sensor setup.
// phase/lo/frame implement the overlapped (non-blocking) read: an integration pass runs
// on-chip while the scheduler services other sensors, and each poll visit only harvests a
// finished pass + starts the next (see as7341_read).
typedef struct {
    uint8_t  addr, mux;
    int8_t   ch;
    int16_t  led;
    uint8_t  phase;        // 0 = idle, 1 = F1-F4 pass integrating, 2 = F5-F8 pass integrating
    uint16_t lo[6];        // harvested F1-F4 pass, waiting for the F5-F8 pass to complete
    float    frame[10];    // last complete 10-channel frame (already count-scaled)
    bool     have_frame;
} as_state_t;
static as_state_t s_done[MC_MAX_SENSORS];
static int s_done_n;

static uint8_t addr_of(const sensor_cfg_t *cfg) { return cfg->addr ? cfg->addr : AS7341_ADDR_DEFAULT; }

static int done_slot(const sensor_cfg_t *cfg)
{
    for (int i = 0; i < s_done_n; i++)
        if (s_done[i].addr == addr_of(cfg) && s_done[i].mux == cfg->mux_addr && s_done[i].ch == cfg->mux_channel)
            return i;
    return -1;
}
static int mark_configured(const sensor_cfg_t *cfg)
{
    if (s_done_n >= MC_MAX_SENSORS) return -1;
    s_done[s_done_n] = (as_state_t){
        .addr = addr_of(cfg), .mux = cfg->mux_addr, .ch = cfg->mux_channel, .led = -1,
    };
    return s_done_n++;
}

static esp_err_t wreg(uint8_t addr, uint8_t reg, uint8_t val)
{
    return bus_i2c_write(addr, (uint8_t[]){reg, val}, 2);
}
static esp_err_t rmw(uint8_t addr, uint8_t reg, uint8_t clear, uint8_t set)
{
    uint8_t v;
    esp_err_t err = bus_i2c_read_reg(addr, reg, &v, 1);
    if (err != ESP_OK) return err;
    v = (v & ~clear) | set;
    return wreg(addr, reg, v);
}

// Apply an LED brightness (0 = off). The CONFIG/LED registers live in the low bank
// (0x60-0x74), only reachable while CFG0.REG_BANK is set — set it, write, and restore, so
// normal spectral access (0x80+) is unaffected.
static esp_err_t apply_led(uint8_t addr, uint8_t pct)
{
    if (pct > 100) pct = 100;
    esp_err_t err;
    if ((err = rmw(addr, REG_CFG0, 0, 0x10)) != ESP_OK) return err;         // bank: low registers
    if (pct == 0) {
        rmw(addr, REG_CONFIG, 0x08, 0);                                     // LED_SEL off
        wreg(addr, REG_LED, 0x00);                                          // LED_ACT off
    } else {
        // Quadratic (perceptual) mapping, not linear-in-current: the eye's response to LED
        // brightness is roughly logarithmic, so a linear pct→mA map crams all the visible
        // change into the bottom few percent (5% already looked "quite bright"). pct² spreads
        // the low end out: 20%→8mA, 50%→34mA, 100%→124mA. Note the hardware floor — the chip's
        // dimmest ON state is drive 0 = 4mA, which is still clearly visible up close; there is
        // no sub-4mA drive or PWM in the chip, so 1% and ~15% both bottom out at that floor.
        uint8_t drive = (uint8_t)(((uint32_t)pct * pct * LED_DRIVE_MAX) / 10000);
        rmw(addr, REG_CONFIG, 0, 0x08);                                     // LED_SEL on
        wreg(addr, REG_LED, 0x80 | drive);                                  // LED_ACT + drive
    }
    return rmw(addr, REG_CFG0, 0x10, 0);                                    // bank: back to normal
}

static esp_err_t configure(uint8_t addr)
{
    uint8_t id = 0;
    bus_i2c_read_reg(addr, REG_WHOAMI, &id, 1);     // (id & 0xFC)>>2 == 0x09 on a real AS7341
    if (((id & 0xFC) >> 2) != 0x09)
        ESP_LOGW(TAG, "unexpected WHOAMI 0x%02x at 0x%02x (continuing)", id, addr);

    esp_err_t err;
    if ((err = wreg(addr, REG_ENABLE, EN_PON)) != ESP_OK) return err;   // power on
    vTaskDelay(pdMS_TO_TICKS(2));
    if ((err = wreg(addr, REG_ATIME, 9)) != ESP_OK) return err;         // ATIME = 9
    if ((err = wreg(addr, REG_ASTEP_L, 0xE7)) != ESP_OK) return err;    // ASTEP = 999 (0x03E7)
    if ((err = wreg(addr, REG_ASTEP_H, 0x03)) != ESP_OK) return err;
    if ((err = wreg(addr, REG_CFG1, AS7341_AGAIN_CODE)) != ESP_OK) return err;
    return ESP_OK;
}

// Load a SMUX band-group map and start an integration. Returns once the integration is
// RUNNING (not finished) — ~1-2ms of I2C traffic; the ~28ms integration itself then proceeds
// on-chip with no bus access needed, so the scheduler is free to service other sensors
// (including switching the I2C mux away) until the result is harvested.
static esp_err_t start_group(uint8_t addr, const uint8_t *smux)
{
    esp_err_t err;
    if ((err = rmw(addr, REG_ENABLE, EN_SP_EN, 0)) != ESP_OK) return err;   // spectral off
    if ((err = wreg(addr, REG_CFG6, 0x10)) != ESP_OK) return err;          // SMUX cmd = WRITE
    for (int i = 0; i < 20; i++)
        if ((err = wreg(addr, (uint8_t)i, smux[i])) != ESP_OK) return err;  // write SMUX RAM

    if ((err = rmw(addr, REG_ENABLE, 0, EN_SMUX)) != ESP_OK) return err;    // start SMUX load
    for (int i = 0; i < 100; i++) {                                        // wait for SMUXEN to clear
        uint8_t en;
        if (bus_i2c_read_reg(addr, REG_ENABLE, &en, 1) == ESP_OK && !(en & EN_SMUX)) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return rmw(addr, REG_ENABLE, 0, EN_SP_EN);                             // spectral on (integrate)
}

// True when the running integration has finished (AVALID).
static bool group_ready(uint8_t addr)
{
    uint8_t st;
    return bus_i2c_read_reg(addr, REG_STATUS2, &st, 1) == ESP_OK && (st & 0x40);
}

// Read the 6 harvested channels of a finished integration (12 bytes).
static esp_err_t read_group_data(uint8_t addr, uint16_t *six)
{
    uint8_t d[12];
    esp_err_t err = bus_i2c_read_reg(addr, REG_CH0_DATA_L, d, sizeof(d));
    if (err != ESP_OK) return err;
    for (int i = 0; i < 6; i++) six[i] = (uint16_t)(d[i * 2] | (d[i * 2 + 1] << 8));
    return ESP_OK;
}

// Blocking convenience (first frame only): start, wait for AVALID, read.
static esp_err_t read_group(uint8_t addr, const uint8_t *smux, uint16_t *six)
{
    esp_err_t err = start_group(addr, smux);
    if (err != ESP_OK) return err;
    bool ready = false;
    for (int i = 0; i < 200; i++) {                                        // wait for AVALID
        if (group_ready(addr)) { ready = true; break; }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!ready) return ESP_ERR_TIMEOUT;
    return read_group_data(addr, six);
}

static esp_err_t as7341_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 10) return ESP_ERR_INVALID_SIZE;
    uint8_t addr = addr_of(cfg);
    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    int slot = done_slot(cfg);
    if (slot < 0) {
        if ((err = configure(addr)) != ESP_OK) return err;
        slot = mark_configured(cfg);
        if (slot < 0) return ESP_ERR_NO_MEM;    // state table full
    }

    // Re-apply the illumination LED whenever the configured brightness changes (web slider →
    // set_config → next poll picks it up here); slot->led starts at -1 so the very first poll
    // always applies the saved setting, including turning a previously-lit LED back off.
    if (slot >= 0 && s_done[slot].led != (int16_t)cfg->led) {
        if (apply_led(addr, cfg->led) == ESP_OK) s_done[slot].led = (int16_t)cfg->led;
    }

    as_state_t *st = &s_done[slot];

    // Assemble a full 10-channel frame from the two passes, ×4 back to the 256x-equivalent
    // scale (see AS7341_COUNT_SCALE). A channel saturated at the current gain therefore reads
    // ~40000, which the transform layer's clip gates detect.
    // lo: F1,F2,F3,F4,Clear,NIR ; hi: F5,F6,F7,F8,Clear,NIR
    #define AS_ASSEMBLE(dst, lo6, hi6) do { \
        (dst)[0] = (lo6)[0] * AS7341_COUNT_SCALE; (dst)[1] = (lo6)[1] * AS7341_COUNT_SCALE; \
        (dst)[2] = (lo6)[2] * AS7341_COUNT_SCALE; (dst)[3] = (lo6)[3] * AS7341_COUNT_SCALE; \
        (dst)[4] = (hi6)[0] * AS7341_COUNT_SCALE; (dst)[5] = (hi6)[1] * AS7341_COUNT_SCALE; \
        (dst)[6] = (hi6)[2] * AS7341_COUNT_SCALE; (dst)[7] = (hi6)[3] * AS7341_COUNT_SCALE; \
        (dst)[8] = (hi6)[4] * AS7341_COUNT_SCALE; \
        (dst)[9] = (hi6)[5] * AS7341_COUNT_SCALE; \
    } while (0)

    if (!st->have_frame) {
        // Very first read after (re)configuration: block through both passes once so the
        // sensor has a valid reading immediately, then leave a pass running for the
        // overlapped path below to harvest on the next visit.
        uint16_t lo[6], hi[6];
        if ((err = read_group(addr, SMUX_F1F4, lo)) != ESP_OK) return err;
        if ((err = read_group(addr, SMUX_F5F8, hi)) != ESP_OK) return err;
        AS_ASSEMBLE(st->frame, lo, hi);
        st->have_frame = true;
        st->phase = (start_group(addr, SMUX_F1F4) == ESP_OK) ? 1 : 0;
    } else {
        // Overlapped read: integrations run on-chip between visits (no bus access needed, so
        // the mux can point elsewhere meanwhile). Each visit harvests at most one finished
        // pass and immediately starts the next — a fresh frame completes every two visits,
        // but the scheduler is never blocked for the ~28ms an integration takes. A pass that
        // isn't finished yet simply leaves the previous frame as this poll's reading.
        if (st->phase == 0) {
            st->phase = (start_group(addr, SMUX_F1F4) == ESP_OK) ? 1 : 0;
            if (st->phase == 0) return ESP_ERR_TIMEOUT;
        } else if (group_ready(addr)) {
            if (st->phase == 1) {                              // F1-F4 done → start F5-F8
                if ((err = read_group_data(addr, st->lo)) != ESP_OK) { st->phase = 0; return err; }
                st->phase = (start_group(addr, SMUX_F5F8) == ESP_OK) ? 2 : 0;
            } else {                                           // F5-F8 done → frame complete
                uint16_t hi[6];
                if ((err = read_group_data(addr, hi)) != ESP_OK) { st->phase = 0; return err; }
                AS_ASSEMBLE(st->frame, st->lo, hi);
                st->phase = (start_group(addr, SMUX_F1F4) == ESP_OK) ? 1 : 0;
            }
        }
    }
    #undef AS_ASSEMBLE

    for (int i = 0; i < 10; i++) out[i] = st->frame[i];
    *out_count = 10;
    return ESP_OK;
}

static int as7341_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    static const char *n[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "clear", "nir"};
    int c = max < 10 ? max : 10;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_as7341 = {
    .type = "as7341",
    .probe = NULL,
    .read = as7341_read,
    .describe = as7341_describe,
};
