// lego_emit.cpp — pack MultiController readings into a fake LEGO Color Sensor.
//
// Owns a PoweredUpDevice (lpf2) on a dedicated UART and a FreeRTOS task that:
//   • runs / retries the LPF2 handshake and keepalive,
//   • caches the latest reading per sensor (fed by lego_emit_on_reading),
//   • packs the configured bit-fields into the Color Sensor's 4×uint16 RGBI payload,
//   • answers the hub — via the combo callback for color.rgbi(), or a single mode-5
//     DATA frame when the hub selects RGB I directly.
//
// All UART setup/teardown happens on the task thread; config changes are handed over
// through a pending-copy + reload flag so apply() never touches the driver directly.

#include "lego_emit.h"
#include "lpf2.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "config_store.h"
#include "display.h"       // display_show_matrix() — onboard TFT 3×3 sink

static const char *TAG = "lego_emit";

// ── State ──────────────────────────────────────────────────────────────────
static SemaphoreHandle_t s_lock;
static lego_cfg_t        s_pending;        // staged by apply(); guarded by s_lock
static bool              s_reload;         // pending → active handoff requested
static lego_cfg_t        s_active;         // task-thread only (no lock needed to read here)
static PoweredUpDevice  *s_dev;            // task-thread only
static volatile bool     s_connected;      // published for lego_emit_is_connected()
static uint32_t s_data_reqs;               // combo data callbacks since last log (hub polls); task-thread only

// Latest reading per sensor id (guarded by s_lock).
typedef struct {
    bool  valid;
    int   id;
    int   count;
    float values[MC_MAX_VALUES];
} cache_entry_t;
static cache_entry_t s_cache[MC_MAX_SENSORS];

// ── Value cache ─────────────────────────────────────────────────────────────
void lego_emit_on_reading(const reading_t *r)
{
    if (!s_lock || !r) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cache_entry_t *slot = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++) {
        if (s_cache[i].valid && s_cache[i].id == r->id) { slot = &s_cache[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < MC_MAX_SENSORS; i++) {
            if (!s_cache[i].valid) { slot = &s_cache[i]; break; }
        }
    }
    if (slot) {
        slot->valid = true;
        slot->id    = r->id;
        slot->count = r->count;
        int n = r->count > MC_MAX_VALUES ? MC_MAX_VALUES : r->count;
        for (int i = 0; i < n; i++) slot->values[i] = r->values[i];
    }
    xSemaphoreGive(s_lock);
}

// Look up a cached value (caller holds s_lock). Returns 0 when not yet seen.
static float cache_lookup(int sensor_id, int value_index)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++) {
        if (s_cache[i].valid && s_cache[i].id == sensor_id) {
            if (value_index >= 0 && value_index < s_cache[i].count)
                return s_cache[i].values[value_index];
            return 0.0f;
        }
    }
    return 0.0f;
}

// ── Bit-packer ──────────────────────────────────────────────────────────────
// Pack the LEGO_TARGET_RGBI fields LSB-first into a 64-bit word, then split into R/G/B/I.
// raw = round((value - offset) / scale), clamped to the field's bit width. Fields targeting
// COLOR/REFLT instead are handled separately by current_color_reflt() below — they're
// independent DATA8 slots the hub can request without ever touching this word (mode 5 RGB I),
// so a field living only here would be invisible to color()/reflection().
static void current_rgbi(uint16_t out[4])
{
    uint64_t word = 0;
    int bitoff = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_active.field_count && i < MC_MAX_LEGO_FIELDS; i++) {
        const lego_field_t *f = &s_active.fields[i];
        if (f->target != LEGO_TARGET_RGBI) continue;
        int bits = f->bits;
        if (bits < 1 || bits > 16) continue;   // any 1..16-bit width (1/2-bit = flags/tiny codes)
        if (bitoff + bits > MC_LEGO_TOTAL_BITS) break;

        double scale = (f->scale == 0.0) ? 1.0 : f->scale;
        long raw = lround((cache_lookup(f->sensor_id, f->value_index) - f->offset) / scale);
        // Same code→value table as COLOR/REFLT (see current_color_reflt() below) — RGBI has no
        // colour semantics of its own (it's a plain passthrough word), but picking a fixed value
        // per code is just as useful there: e.g. a dpad/stick-direction code driving one 16-bit
        // RGBI channel as a small palette of numbers instead of the raw code value.
        // An unmapped ("none") code sends MC_LEGO_COLOUR_NONE (255/0xFF) same as COLOR/REFLT
        // below — one consistent sentinel value regardless of target, rather than a different
        // "none" value per target (there's no equivalent of color()'s −1 at the raw-byte level
        // for any of these; 255 is simply the fixed value that means "nothing mapped").
        // colour_map is a fixed-size C array — a negative raw (e.g. a signed stick code before
        // its offset/scale land it in 0..15) would read out of bounds. current_color_reflt()
        // below is already safe here because it clamps to 0-255 before this same lookup; this
        // path has no such clamp yet at this point, so guard it explicitly.
        if (f->use_colour_map)
            raw = (raw >= 0 && raw < MC_LEGO_COLOUR_MAP_N) ? f->colour_map[raw] : MC_LEGO_COLOUR_NONE;

        // Optional second-stage output scaling: map raw field value (0..2^bits-1) to custom LEGO output range
        // e.g., 0-15 (4-bit field) → 48-108 (piano scale): output_scale=4, output_offset=48
        if (f->output_scale != 0.0)
            raw = lround(raw * f->output_scale + f->output_offset);

        long mask = (1L << bits) - 1;
        if (f->is_signed) {
            long lo = -(1L << (bits - 1));
            long hi =  (1L << (bits - 1)) - 1;
            if (raw < lo) raw = lo;
            if (raw > hi) raw = hi;
        } else {
            if (raw < 0) raw = 0;
            if (raw > mask) raw = mask;
        }
        word |= ((uint64_t)(raw & mask)) << bitoff;
        bitoff += bits;
    }
    xSemaphoreGive(s_lock);

    out[0] = (uint16_t)(word & 0xFFFF);
    out[1] = (uint16_t)((word >> 16) & 0xFFFF);
    out[2] = (uint16_t)((word >> 32) & 0xFFFF);
    out[3] = (uint16_t)((word >> 48) & 0xFFFF);
}

// The last-configured LEGO_TARGET_COLOR/LEGO_TARGET_REFLT field (if any) drives the emulated
// sensor's COLOR (mode 0, 0-255) / REFLT (mode 1, 0-255) byte directly — same raw = round((value
// - offset) / scale) as the bit-packer, just a single byte with no bit-width/sign to apply.
// *has_color/*has_reflect report whether a field actually targets that slot, so the caller can
// tell "configured, value 0" apart from "nothing configured for this slot".
static void current_color_reflt(uint8_t *color_out, bool *has_color, uint8_t *reflect_out, bool *has_reflect)
{
    *has_color = false;
    *has_reflect = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_active.field_count && i < MC_MAX_LEGO_FIELDS; i++) {
        const lego_field_t *f = &s_active.fields[i];
        if (f->target != LEGO_TARGET_COLOR && f->target != LEGO_TARGET_REFLT) continue;
        double scale = (f->scale == 0.0) ? 1.0 : f->scale;
        long raw = lround((cache_lookup(f->sensor_id, f->value_index) - f->offset) / scale);
        // A negative value on the COLOR slot means "no colour" — the classifier's own -1, which a
        // real LEGO colour sensor reports as 0xFF and the hub's color() surfaces as -1/none. This
        // used to clamp to 0, i.e. BLACK: "nothing there" became indistinguishable from a black
        // target, and disagreed with colour *passthrough*, which does carry the -1 through
        // (combo_value casts via int8_t). REFLT is a 0-100 light percentage with no such
        // sentinel, so a negative there still clamps to 0.
        if (raw < 0 && f->target == LEGO_TARGET_COLOR) {
            *color_out = MC_LEGO_COLOUR_NONE;
            *has_color = true;
            continue;
        }
        if (raw < 0) raw = 0;
        if (raw > 255) raw = 255;
        // Code→value lookup: for COLOR this picks a hub-supported colour id per code (the hub
        // coerces ids it doesn't support — 0,1,3,4,6,7,9,10 are safe); for REFLT the same table
        // just picks a fixed byte per code (REFLT has no colour semantics on the hub side, it's
        // read as a plain 0-100 light %, so the table's "colour id" values 0-10 simply become
        // that plain number there) — same 16-slot mechanism, works on either target so a code
        // like a dpad/stick direction can drive REFLT the same way it can drive COLOR. An
        // unmapped ("none") code always sends MC_LEGO_COLOUR_NONE (255/0xFF), COLOR or REFLT —
        // one consistent sentinel value for "nothing mapped" regardless of target.
        if ((f->target == LEGO_TARGET_COLOR || f->target == LEGO_TARGET_REFLT) && f->use_colour_map)
            raw = (raw < MC_LEGO_COLOUR_MAP_N) ? f->colour_map[raw] : MC_LEGO_COLOUR_NONE;
        if (f->target == LEGO_TARGET_COLOR) { *color_out = (uint8_t)raw; *has_color = true; }
        else                                { *reflect_out = (uint8_t)raw; *has_reflect = true; }
    }
    xSemaphoreGive(s_lock);
}

// Colour passthrough: pull colour id / reflect% / RGB (0-1024) / raw clear from the configured
// source sensor (which must be in "col_full"/"as_full" mode → values
// [colour,reflect,r,g,b,clear] — clear is the 6th value, added alongside the other 5).
// Returns false when passthrough is off, so the caller falls back to the bit-packer.
static bool passthrough_get(int *colour, int *reflect, uint16_t rgb[3], uint16_t *clear)
{
    if (s_active.colour_source <= 0) return false;
    float v[6] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MC_MAX_SENSORS; i++) {
        if (s_cache[i].valid && s_cache[i].id == s_active.colour_source) {
            for (int k = 0; k < 6 && k < s_cache[i].count; k++) v[k] = s_cache[i].values[k];
            break;
        }
    }
    xSemaphoreGive(s_lock);
    *colour  = (int)lroundf(v[0]);
    *reflect = (int)lroundf(v[1]);
    for (int i = 0; i < 3; i++) {
        long c = lroundf(v[2 + i]);
        rgb[i] = (uint16_t)(c < 0 ? 0 : (c > 0xFFFF ? 0xFFFF : c));
    }
    long c = lroundf(v[5]);
    *clear = (uint16_t)(c < 0 ? 0 : (c > 0xFFFF ? 0xFFFF : c));
    return true;
}

// Per-mode value width (bytes) in our colour-sensor profile: DATA8 modes = 1 byte,
// DATA16 modes = 2 bytes. Matches build_modes() below.
static int combo_mode_width(int mode)
{
    switch (mode) {
        case 0: case 1: case 2: case 3: return 1;   // COLOR/REFLT/AMBI/LIGHT  (DATA8)
        default:                        return 2;   // RREFL/RGB I/HSV/SHSV     (DATA16)
    }
}

// Value for one (mode,dataset) pair, given the current passthrough / bit-pack data.
static int32_t combo_value(int mode, int ds, bool pass, int pcolour, int preflect,
                           const uint16_t rgb[3], uint16_t pclear, const uint16_t v[4],
                           uint8_t ccolor, bool has_color, uint8_t creflect, bool has_reflect)
{
    switch (mode) {
        case 0:                                                            // COLOR id
            if (pass) return (int8_t)pcolour;
            return has_color ? ccolor : 0;                                 // no field targets it: 0, not v[0]&0xFF garbage
        case 1:                                                            // REFLT %
            if (pass) return preflect;
            return has_reflect ? creflect : 0;
        case 5:                                                            // RGB I
            if (ds < 3) return pass ? rgb[ds] : v[ds];
            return pass ? pclear : v[3];                                   // I: raw clear when passthrough
        default: return 0;                                                // AMBI/LIGHT/HSV…
    }
}

// ── Combo frame ──────────────────────────────────────────────────────────────
// Per the LPF2 spec the device replies with a single MODE_0 DATA frame whose payload is
// the requested datasets concatenated in the hub's CMD_COMBO_SET order (each sized by its
// mode's format), padded to the next power-of-2 — NO per-mode prefix. We honour the
// requested pairs so color()/reflection()/rgbi() all decode correctly. Sent both from the
// NACK callback (hub poll) and streamed by the task (keepalive / idle device view).
static void send_combo_frame()
{
    if (!s_dev) return;

    uint16_t v[4];
    int colour = -1, reflect = 0; uint16_t rgb[3] = {0}; uint16_t clear = 0;
    bool pass = passthrough_get(&colour, &reflect, rgb, &clear);
    current_rgbi(v);
    uint8_t ccolor = 0, creflect = 0; bool has_color = false, has_reflect = false;
    current_color_reflt(&ccolor, &has_color, &creflect, &has_reflect);

    byte modes[8], dsets[8];
    int  npairs = s_dev->getComboPairs(modes, dsets, 8);

    byte pkt[16] = {0};
    int  off = 0;
    if (npairs > 0) {
        for (int i = 0; i < npairs && off < (int)sizeof(pkt); i++) {
            int32_t  val = combo_value(modes[i], dsets[i], pass, colour, reflect, rgb, clear, v, ccolor, has_color, creflect, has_reflect);
            int      w   = combo_mode_width(modes[i]);
            pkt[off++] = (byte)(val & 0xFF);
            if (w == 2 && off < (int)sizeof(pkt)) pkt[off++] = (byte)((val >> 8) & 0xFF);
        }
    } else {
        // No combo-set parsed (shouldn't normally happen): fall back to the classic RGB I
        // layout so rgbi() still works.
        for (int i = 0; i < 4; i++) {
            uint16_t x = !pass ? v[i] : (i < 3 ? rgb[i] : clear);
            pkt[off++] = (byte)(x & 0xFF);
            pkt[off++] = (byte)(x >> 8);
        }
    }

    // Pad the payload up to the next valid LPF2 length (power of 2: 1/2/4/8/16).
    int len = 1;
    while (len < off) len <<= 1;
    s_dev->send_data8_mode(pkt, len, 0);
}

// NACK callback — fires on every hub poll while combo mode is active.
static void combo_data_callback()
{
    s_data_reqs++;
    send_combo_frame();
}

// ── Color-sensor mode profile (must match a real 0x3D sensor for the hub) ───
static void build_color_modes(PoweredUpDevice *d)
{
    d->create_mode("COLOR", true, DATA8,  1, 2, 0,   0,    10,  0, 100,  0,    10,    "IDX", 0xE4, 0);
    d->create_mode("REFLT", true, DATA8,  1, 3, 0,   0,   100,  0, 100,  0,   100,    "PCT", 0x30, 0);
    d->create_mode("AMBI",  true, DATA8,  1, 3, 0,   0,   100,  0, 100,  0,   100,    "PCT", 0x30, 0);
    d->create_mode("LIGHT", true, DATA8,  3, 3, 0,   0,   100,  0, 100,  0,   100,    "PCT", 0, ABSOLUTE);
    d->create_mode("RREFL", true, DATA16, 2, 4, 0,   0,  2000,  0, 100,  0,  2000,    "RAW", ABSOLUTE, 0);
    d->create_mode("RGB I", true, DATA16, 4, 5, 0,   0, 65535,  0, 100,  0, 65535,    "RAW", ABSOLUTE, 0);
    d->create_mode("HSV",   true, DATA16, 3, 4, 0,   0,   360,  0, 100,  0,   360,    "RAW", ABSOLUTE, 0);
    d->create_mode("SHSV",  true, DATA16, 4, 4, 0,   0,  2000,  0, 100,  0,  2000,    "RAW", ABSOLUTE, 0);
    d->set_combo_modes(0x0063);                 // modes 0,1,5,6 combinable (matches real sensor)
    d->set_combo_callback(combo_data_callback);
}

// ── 3×3 Color Light Matrix (0x40) — receives pixel writes from the hub ──────
// LEGO 3×3 matrix colour ids (0–10) → 24-bit RGB. Palette per the Raspberry Pi BuildHAT
// serial-protocol doc (mode 1 COL O): 0=off 1=red 2=magenta 3=blue 4=cyan 5=pale-green
// 6=green 7=yellow 8=orange 9=red 10=white.
static const uint32_t LEGO_COLOR_RGB[11] = {
    0x000000, // 0  off
    0xFF0000, // 1  red
    0xFF00FF, // 2  magenta
    0x0000FF, // 3  blue
    0x00FFFF, // 4  cyan
    0x90EE90, // 5  pale green
    0x00C000, // 6  green
    0xFFFF00, // 7  yellow
    0xFF8000, // 8  orange
    0xFF0000, // 9  red
    0xFFFFFF, // 10 white
};

// Registered sink for decoded matrix pixels (BLE notify → frontend grid). Set by host.
static void (*s_matrix_cb)(const uint16_t cells[9]) = NULL;

static inline uint16_t rgb888_to_565(uint32_t c)
{
    uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// PIX 0 WRITE callback: 9 bytes, each = (brightness<<4)|colour_id (Pybricks light-matrix
// format). Decode to RGB565, dim by brightness/9, and fan out to the TFT + frontend.
static void matrix_pix_callback(byte *data, byte len)
{
    uint16_t cells[9] = {0};
    for (int i = 0; i < 9; i++) {
        byte b = (i < len) ? data[i] : 0;
        int bright = (b >> 4) & 0x0F;          // high nibble: brightness 0..0x0A (10 = full)
        int cid    = b & 0x0F;                  // low nibble: colour id 0..0x0A
        if (bright > 10) bright = 10;
        uint32_t rgb = (cid <= 10) ? LEGO_COLOR_RGB[cid] : 0;
        if (bright < 10) {                      // scale by brightness/10
            uint8_t r = ((rgb >> 16) & 0xFF) * bright / 10;
            uint8_t g = ((rgb >> 8)  & 0xFF) * bright / 10;
            uint8_t bl = (rgb & 0xFF) * bright / 10;
            rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
        }
        cells[i] = rgb888_to_565(rgb);
    }
    display_show_matrix(cells);                 // onboard TFT 3×3
    if (s_matrix_cb) s_matrix_cb(cells);        // frontend virtual grid (BLE)
    if (lpf2_debug_mask & DBG_EVT)
        ESP_LOGI(TAG, "matrix pixels: %02X %02X %02X / %02X %02X %02X / %02X %02X %02X",
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8]);
}

// Replicates the genuine Technic 3×3 Color Light Matrix mode set so the hub (and the SPIKE
// app's matrix blocks) bind it as a matrix. PIX O (mode 2) is the writable 9-pixel mode.
static void build_matrix_modes(PoweredUpDevice *d)
{
    d->create_mode("LEV O",  true, DATA8, 1, 4, 0,  0,   9,  0, 100,  0,   9, "LEV", 0, ABSOLUTE);
    d->create_mode("COL O",  true, DATA8, 1, 2, 0,  0,  10,  0, 100,  0,  10, "COL", 0, ABSOLUTE);
    d->create_mode("PIX O",  true, DATA8, 9, 3, 0,  0, 255,  0, 100,  0, 255, "PIX", 0, ABSOLUTE);
    d->create_mode("TRANS",  true, DATA8, 1, 1, 0,  0,   2,  0, 100,  0,   2, "TRN", 0, ABSOLUTE);
    PoweredUpMode *pix = d->get_mode(2);
    if (pix) pix->setCallback(matrix_pix_callback);
}

// Register the frontend pixel sink (called once from the host before init).
void lego_emit_set_matrix_cb(void (*cb)(const uint16_t cells[9])) { s_matrix_cb = cb; }

// Internal UART loopback self-test: connect TX→RX inside the peripheral, send a known
// pattern, read it back. Confirms the UART driver, baud/format config, and GPIO-matrix
// routing are good — independent of any external wiring. Runs on the configured port/pins
// (sequentially with the handshake; no conflict). Note: this does NOT exercise the physical
// pads or the wire to the hub — a PASS means "the ESP side is fine, look at the connector".
static bool uart_loopback_selftest(int port, int tx, int rx, uint32_t baud)
{
    const uint8_t pattern[] = { 0x00, 0x40, 0x3D, 0x82, 0x55, 0xAA, 0x04 };
    uint8_t rxbuf[sizeof(pattern)] = {0};

    uart_config_t cfg = {};
    cfg.baud_rate  = (int)baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    uart_driver_install((uart_port_t)port, 256, 0, 0, NULL, 0);
    uart_param_config((uart_port_t)port, &cfg);
    uart_set_pin((uart_port_t)port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_loop_back((uart_port_t)port, true);

    uart_flush_input((uart_port_t)port);
    uart_write_bytes((uart_port_t)port, (const char *)pattern, sizeof(pattern));
    uart_wait_tx_done((uart_port_t)port, pdMS_TO_TICKS(100));
    int n = uart_read_bytes((uart_port_t)port, rxbuf, sizeof(pattern), pdMS_TO_TICKS(100));

    uart_set_loop_back((uart_port_t)port, false);
    uart_driver_delete((uart_port_t)port);

    bool ok = (n == (int)sizeof(pattern)) && memcmp(pattern, rxbuf, sizeof(pattern)) == 0;
    if (ok) {
        ESP_LOGI(TAG, "UART self-test PASS (internal loopback, UART%d, %u baud) — driver + "
                      "config OK. If the hub still won't ACK, it's the external wiring "
                      "(data lines / connector), not the ESP.", port, (unsigned)baud);
    } else {
        ESP_LOGE(TAG, "UART self-test FAIL on UART%d: got %d/%d bytes back "
                      "(driver/config/pin problem) — check uart_port and tx/rx GPIOs",
                 port, n, (int)sizeof(pattern));
    }
    return ok;
}

// Tear down the current device (task thread only).
static void teardown(void)
{
    if (s_dev) {
        s_dev->disconnect();
        delete s_dev;
        s_dev = NULL;
    }
    s_connected = false;
}

// ── Device-profile table ────────────────────────────────────────────────────
// Bundles the LPF2 type byte, mode descriptors, and whether the colour data-out path runs.
typedef struct {
    const char *name;
    uint8_t     type;                          // LPF2 type byte
    void      (*build_modes)(PoweredUpDevice *);
    bool        sends;                          // colour data-out path active in the task
} lego_profile_def_t;

static const lego_profile_def_t PROFILES[] = {
    [LEGO_PROFILE_COLOR]  = { "Color Sensor",      0x3D, build_color_modes,  true  },
    // [LEGO_PROFILE_MATRIX] = { "3x3 Light Matrix",  0x40, build_matrix_modes, false }, // Disabled — still in development
};
static const int PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);

static const lego_profile_def_t *active_profile(void)
{
    int p = s_active.profile;
    if (p < 0 || p >= PROFILE_COUNT) p = LEGO_PROFILE_COLOR;
    return &PROFILES[p];
}

// Build a fresh device from s_active (task thread only).
static void rebuild(void)
{
    teardown();

    // Turn the LPF2 trace on/off live with the config. "events" = high-level only (mode
    // SELECT / combo / WRITE); "debug" = full byte-level trace.
    uint32_t mask = 0;
    if (s_active.events) mask |= DBG_CONN | DBG_EVT;
    if (s_active.debug)  mask |= DBG_CONN | DBG_INIT | DBG_TX | DBG_RX | DBG_EVT;
    lpf2_set_debug_mask(mask);

    if (!s_active.enabled) {
        ESP_LOGI(TAG, "emitter disabled");
        return;
    }
    uint32_t baud = s_active.baud ? s_active.baud : 115200;

    // Self-test the UART before the first handshake (debug only). Proves the ESP side
    // works without rewiring; isolates "ESP problem" from "connector/wiring problem".
    if (s_active.debug)
        uart_loopback_selftest(s_active.uart_port, s_active.tx_gpio, s_active.rx_gpio, 2400);

    const lego_profile_def_t *prof = active_profile();
    s_dev = new PoweredUpDevice(s_active.uart_port, s_active.rx_gpio, s_active.tx_gpio,
                                prof->type, baud);
    prof->build_modes(s_dev);
    // Colour profile: start in the hub's standard 6-pair combo (COLOR + REFLT + RGB I —
    // the exact CMD_COMBO_SET payload a SPIKE hub sends at program start) so colour, reflect
    // AND RGB are all live from first connect. A real CMD_COMBO_SET from the hub overwrites
    // this seed, so the hub still drives the combo shape.
    if (prof->sends) {
        static const byte COMBO6[9] = { 0x26, 0x00, 0x00, 0x10, 0x50, 0x51, 0x52, 0x53, 0x95 };
        s_dev->seedComboSet(COMBO6);
    }
    ESP_LOGI(TAG, "emitter on UART%d tx=%d rx=%d profile=%s type=0x%02X baud=%u fields=%d debug=%d",
             s_active.uart_port, s_active.tx_gpio, s_active.rx_gpio,
             prof->name, prof->type, (unsigned)baud, s_active.field_count, s_active.debug);
}

// ── Task ────────────────────────────────────────────────────────────────────
static void lego_task(void *arg)
{
    (void)arg;
    bool     prev_conn = false;
    int64_t  last_state_log = 0;

    for (;;) {
        // Pick up a staged config change.
        bool reload = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_reload) { s_active = s_pending; s_reload = false; reload = true; }
        xSemaphoreGive(s_lock);
        if (reload) { rebuild(); prev_conn = false; }

        if (!s_dev || !s_active.enabled) {
            s_connected = false;
            prev_conn = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        s_dev->heart_beat();

        if (!s_dev->isConnected()) {
            if (prev_conn) {
                ESP_LOGW(TAG, "hub link lost — re-running handshake");
                prev_conn = false;
            }
            ESP_LOGI(TAG, "handshake: connecting (UART%d tx=%d rx=%d, 2400→%u baud)…",
                     s_active.uart_port, s_active.tx_gpio, s_active.rx_gpio,
                     (unsigned)(s_active.baud ? s_active.baud : 115200));
            s_dev->reset();           // blocks; retries internally
            s_connected = s_dev->isConnected();
            if (s_connected) {
                ESP_LOGI(TAG, "hub connected — handshake complete");
                prev_conn = true;
            } else {
                ESP_LOGW(TAG, "handshake failed (no hub ACK) — retrying. "
                              "Check TX/RX wiring + shared GND; enable debug for the byte trace");
            }
            continue;
        }
        s_connected = true;

        // Matrix profile is receive-only: the hub WRITEs pixels (matrix_pix_callback fires
        // from heart_beat); we send nothing, just keep the link serviced.
        if (!active_profile()->sends) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Colour passthrough drives COLOR/REFLT/RGB from a colour sensor; else bit-pack.
        int pcolour = 0, preflect = 0; uint16_t prgb[3] = {0}; uint16_t pclear = 0;
        bool pass = passthrough_get(&pcolour, &preflect, prgb, &pclear);
        uint16_t v[4];
        if (pass) { v[0] = prgb[0]; v[1] = prgb[1]; v[2] = prgb[2]; v[3] = pclear; }
        else current_rgbi(v);
        uint8_t ccolor = 0, creflect = 0; bool has_color = false, has_reflect = false;
        if (!pass) current_color_reflt(&ccolor, &has_color, &creflect, &has_reflect);
        byte mode = s_dev->get_current_mode();

        // Periodic state line while debugging/events — confirms the link is live, the
        // current mode, whether combo is active, and how often the hub is polling for data.
        if (s_active.debug || s_active.events) {
            int64_t now = esp_timer_get_time();
            if (now - last_state_log > 2000000) {   // every ~2 s
                uint32_t reqs = s_data_reqs; s_data_reqs = 0;
                last_state_log = now;
                ESP_LOGI(TAG, "link: mode=%d combo=%d data-reqs=%u  R=%u G=%u B=%u I=%u",
                         mode, s_dev->isComboActive(), (unsigned)reqs, v[0], v[1], v[2], v[3]);
            }
        }

        // In combo mode, stream the combo-format frame every tick (never a single-mode frame,
        // which would decode as garbage against the combo shape). The NACK callback also
        // answers hub polls with the same frame; identical shape, so duplicates are harmless.
        // Streaming (rather than waiting for NACKs) keeps the hub's idle device view live —
        // at idle the hub doesn't NACK-poll, it just consumes what we send.
        if (s_dev->isComboActive()) {
            send_combo_frame();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Stream a DATA frame for the current mode on every tick. This is the keepalive:
        // an LPF2 hub does not start its SYNC/NACK polling until it has received data, so
        // without this it stays silent and our watchdog times out. Frame sizes match the
        // declared mode formats (ported from the original taskLPF2 switch).
        switch (mode) {
        case 0: { byte pkt[1]; pkt[0] = pass ? (byte)(int8_t)pcolour : (has_color ? ccolor : 0);   s_dev->send_data8_mode(pkt, 1, 0); break; }
        case 1: { byte pkt[1]; pkt[0] = pass ? (byte)preflect        : (has_reflect ? creflect : 0); s_dev->send_data8_mode(pkt, 1, 1); break; }
        case 2: { byte pkt[1] = {0};                     s_dev->send_data8_mode(pkt, 1, 2); break; }
        case 3: break;                                   // LIGHT (3×DATA8) — original skips
        case 4: {
            byte pkt[4] = { (byte)(v[2] & 0xFF), (byte)(v[2] >> 8),
                            (byte)(v[1] & 0xFF), (byte)(v[1] >> 8) };
            s_dev->send_data8_mode(pkt, 4, 4);
            break;
        }
        case 5: {
            byte pkt[8];
            for (int i = 0; i < 4; i++) { pkt[i*2] = (byte)(v[i] & 0xFF); pkt[i*2+1] = (byte)(v[i] >> 8); }
            s_dev->send_data8_mode(pkt, 8, 5);
            break;
        }
        case 6: {
            byte pkt[8] = {0};
            for (int i = 0; i < 3; i++) { pkt[i*2] = (byte)(v[i] & 0xFF); pkt[i*2+1] = (byte)(v[i] >> 8); }
            s_dev->send_data8_mode(pkt, 8, 6);
            break;
        }
        case 7: {
            byte pkt[8];
            for (int i = 0; i < 4; i++) { pkt[i*2] = (byte)(v[i] & 0xFF); pkt[i*2+1] = (byte)(v[i] >> 8); }
            s_dev->send_data8_mode(pkt, 8, 7);
            break;
        }
        default: { byte pkt[1] = {0}; s_dev->send_data8_mode(pkt, 1, 0); break; }
        }

        vTaskDelay(pdMS_TO_TICKS(5));   // ≥20 Hz keepalive
    }
}

// ── Public API ──────────────────────────────────────────────────────────────
esp_err_t lego_emit_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    config_store_get_lego(&s_pending);
    s_reload = true;

    // Pin to core 1 (APP CPU) at a priority above the BLE worker so the LPF2
    // keepalive is never starved during a bus scan.
    if (xTaskCreatePinnedToCore(lego_task, "lego_emit", 4096, NULL, 6, NULL, 1) != pdPASS)
        return ESP_FAIL;
    return ESP_OK;
}

void lego_emit_apply(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    config_store_get_lego(&s_pending);
    s_reload = true;
    xSemaphoreGive(s_lock);
}

bool lego_emit_is_connected(void) { return s_connected; }
