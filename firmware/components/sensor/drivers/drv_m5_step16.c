// drv_m5_step16.c — M5Stack Unit Step16 (STM32-based I2C unit, 16-position detented rotary
// switch + RGB ring, default address 0x48).
//
// Register map (from the official M5Unit-Step16 Arduino library source, unit_step16.hpp/.cpp —
// no public plain-text datasheet):
//   0x00  current step position, 1 byte, 0x0-0xF (0-15) — the only live-readable value; there is
//         no separate pushbutton/press register in this library version
//   0x10  7-segment position display mode: 0x00 = always off, 0x01-0xFE = auto-off timeout in
//         seconds, 0xFF = always on (unit default 0xFE). What the digit SHOWS is fixed by the
//         unit's own firmware (always the current position, 0-F hex) — only lit/unlit/timeout
//         and brightness are controllable.
//   0x20  7-segment brightness 0-100
//   0x30  rotation-direction invert config (0=CCW, 1=CW) — a configuration bit, not a runtime
//         switch state; not exposed as a sensor value
//   0x40  RGB ring on/off (0/1)          } driven by the optional position-colour visualisation
//   0x41  RGB ring brightness 0-100      } below — the sensor's `led` config field is both the
//   0x50  RGB colour, [R, G, B] 3 bytes  } on/off switch (0 = ring off) and the brightness %.
//         One colour for the whole ring — NOT per-LED addressable (unlike the 8Angle unit's
//         per-knob LEDs), so the ring as a whole shows the selected position's colour.
//   0xF0  save-to-flash trigger — deliberately never written (each write wears the unit's
//         internal flash; the visualisation re-applies its state on every boot anyway)
//   0xFE  firmware version (read-only, unused here)
//   0xFF  I2C address (read/write, unused here)
//
// Read protocol: the reference library does a plain write-register-byte-then-STOP followed by a
// separate read (not a repeated-start combined transaction like most of this codebase's other
// I2C sensors) — mirrored here with bus_i2c_write()+bus_i2c_read() instead of
// bus_i2c_read_reg() to match exactly. Config/colour writes are single plain write transactions
// ([reg, data...]), the library's writeBytes() shape.
//
// I2C clock: undocumented in the library; treat as standard-mode (100kHz) safe. Like the
// 8Angle unit this is an STM32-based peripheral, not a dedicated ASIC — the visualisation only
// writes when the position (or the led setting) actually changed, so a ring nobody's turning
// adds zero extra I2C traffic per poll.
#include "sensor.h"
#include "bus_i2c.h"
#include "i2c_mux.h"
#include "esp_timer.h"

#define STEP16_ADDR_DEFAULT   0x48
#define STEP16_REG_VALUE      0x00
#define STEP16_REG_SEG_MODE   0x10
#define STEP16_REG_SEG_BRIGHT 0x20
#define STEP16_REG_RGB_ON     0x40
#define STEP16_REG_RGB_BRIGHT 0x41
#define STEP16_REG_RGB_VALUE  0x50
#define STEP16_POSITIONS      16
#define STEP16_SEG_OFF        0x00
#define STEP16_SEG_ALWAYS_ON  0xFF

// Map a 0-15 position onto the same blue→red hue sweep the 8Angle unit's knob LEDs use
// (see drv_m5_8angle.c's angle8_value_colour — duplicated rather than shared because these are
// the only two users and the mapping is ~10 lines; keep the two in sync if the scheme changes).
static void step16_position_colour(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned hue = 240u - (unsigned)pos * 240u / (STEP16_POSITIONS - 1);   // 240 (blue) .. 0 (red)
    unsigned seg = hue / 60, rem = hue % 60;
    uint8_t rise = (uint8_t)(rem * 255u / 60u), fall = (uint8_t)(255u - rise);
    switch (seg) {
    case 0:  *r = 255;  *g = rise; *b = 0;    break;   // red → yellow
    case 1:  *r = fall; *g = 255;  *b = 0;    break;   // yellow → green
    case 2:  *r = 0;    *g = 255;  *b = rise; break;   // green → cyan
    default: *r = 0;    *g = fall; *b = 255;  break;   // cyan → blue (seg 3, hue 180-240)
    }
}

// Last ring state actually written per sensor — writes happen only on change. Same slot
// pattern as drv_vk36n16.c/drv_m5_8angle.c.
typedef struct {
    int     id;
    bool    used;
    bool    dark;         // true once the lighting was switched off after led→0 (or on sleep)
    bool    sleeping;     // auto-sleep (led_sleep_s) blanked the lighting; wake on movement
    int64_t last_move_us; // last time the dial position changed
    int16_t seenpos;      // last position observed (movement detection; -1 = none yet)
    uint8_t bright;       // brightness the ring was last written with
    int16_t lastpos;      // last position the ring colour was written for (-1 = never)
} ring_state_t;
static ring_state_t s_ring[MC_MAX_SENSORS];

static ring_state_t *ring_slot(int id)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_ring[i].used && s_ring[i].id == id) return &s_ring[i];
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (!s_ring[i].used) {
            s_ring[i] = (ring_state_t){ .id = id, .used = true, .lastpos = -1, .seenpos = -1,
                                        .last_move_us = esp_timer_get_time() };
            return &s_ring[i];
        }
    return NULL;   // table full (shouldn't happen) — visualisation silently skipped
}

static void step16_reg_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[4] = { reg };
    if (len > sizeof(buf) - 1) return;
    for (size_t i = 0; i < len; i++) buf[1 + i] = data[i];
    bus_i2c_write(addr, buf, len + 1);   // best-effort — a missed update self-corrects next change
}

// Colour the ring by the selected position (blue = 0 .. red = 15). cfg->led (0-100) is the one
// switch/brightness for ALL the unit's lighting — the RGB ring and the 7-segment position digit
// together: 0 turns both off once then stays silent; >0 keeps both on at that brightness (the
// digit set to always-on rather than the unit's default auto-off timeout), rewriting the ring
// colour only when the position actually changes. The digit's content isn't controllable — the
// unit's own firmware always shows the current position (0-F hex); only lit/brightness are.
static void step16_ring_update(const sensor_cfg_t *cfg, uint8_t addr, uint8_t pos)
{
    ring_state_t *st = ring_slot(cfg->id);
    if (!st) return;

    if (cfg->led == 0) {
        if (!st->dark) {
            uint8_t off = 0, seg_off = STEP16_SEG_OFF;
            step16_reg_write(addr, STEP16_REG_RGB_ON, &off, 1);
            step16_reg_write(addr, STEP16_REG_SEG_MODE, &seg_off, 1);
            st->dark = true;
            st->lastpos = -1;
        }
        st->sleeping = false;
        return;
    }

    // Auto-sleep (cfg->led_sleep_s, -1 = never): blank the ring + digit after that many
    // seconds without the dial moving, wake instantly on the next detent — same idea as the
    // display's own auto-sleep. seenpos tracks observed movement independently of lastpos
    // (which is a "what's written" cache that gets force-reset by brightness changes).
    int64_t now = esp_timer_get_time();
    bool moved = (st->seenpos != (int16_t)pos);
    st->seenpos = (int16_t)pos;
    if (moved) st->last_move_us = now;

    if (st->sleeping) {
        if (!moved) return;
        st->sleeping = false;   // wake: st->dark is still true from sleep entry → full re-apply below
    } else if (cfg->led_sleep_s >= 0 && !moved &&
               now - st->last_move_us > (int64_t)cfg->led_sleep_s * 1000000) {
        uint8_t off = 0, seg_off = STEP16_SEG_OFF;
        step16_reg_write(addr, STEP16_REG_RGB_ON, &off, 1);
        step16_reg_write(addr, STEP16_REG_SEG_MODE, &seg_off, 1);
        st->dark = true;
        st->sleeping = true;
        return;
    }

    if (st->dark || st->bright != cfg->led) {
        uint8_t on = 1, seg_on = STEP16_SEG_ALWAYS_ON;
        step16_reg_write(addr, STEP16_REG_RGB_ON, &on, 1);
        step16_reg_write(addr, STEP16_REG_RGB_BRIGHT, &cfg->led, 1);
        step16_reg_write(addr, STEP16_REG_SEG_MODE, &seg_on, 1);
        step16_reg_write(addr, STEP16_REG_SEG_BRIGHT, &cfg->led, 1);
        st->dark = false;
        st->bright = cfg->led;
        st->lastpos = -1;   // force a colour write below
    }

    if (st->lastpos != (int16_t)pos) {
        uint8_t rgb[3];
        step16_position_colour(pos, &rgb[0], &rgb[1], &rgb[2]);
        step16_reg_write(addr, STEP16_REG_RGB_VALUE, rgb, 3);
        st->lastpos = (int16_t)pos;
    }
}

static esp_err_t step16_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;
    uint8_t addr = cfg->addr ? cfg->addr : STEP16_ADDR_DEFAULT;

    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    uint8_t reg = STEP16_REG_VALUE;
    if ((err = bus_i2c_write(addr, &reg, 1)) != ESP_OK) return err;
    uint8_t v = 0;
    if ((err = bus_i2c_read(addr, &v, 1)) != ESP_OK) return err;
    if (v > STEP16_POSITIONS - 1) v = STEP16_POSITIONS - 1;   // defensive: register is documented 0x0-0xF

    step16_ring_update(cfg, addr, v);

    out[0] = (float)v;
    *out_count = 1;
    return ESP_OK;
}

static int step16_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    if (max < 1) return 0;
    names[0] = "position";
    return 1;
}

const sensor_driver_t drv_m5_step16 = {
    .type = "m5_step16",
    .probe = NULL,
    .read = step16_read,
    .describe = step16_describe,
};
