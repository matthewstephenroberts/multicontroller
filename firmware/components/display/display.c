// display.c — configurable status display.
//
// The display is a device described by display_cfg_t (from config_store): enabled, controller
// (st7789/ili9341/gc9107 over SPI, ssd1306 over I2C), pins/addr, geometry and mode. If no display
// hardware is configured (SPI CS < 0 and not I2C), this is a no-op — the same firmware runs on
// boards with no screen. Text is drawn with an 8x8 font: RGB565 panels render one line-strip at
// a time; the mono SSD1306 renders a full 1bpp frame.
//
#include "display.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "bus_spi.h"
#include "bus_i2c2.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_ili9341.h"
#include "panel_gc9107.h"
#include "panel_st7735s.h"

#include "board_config.h"
#include "font8x8.h"
#include "config_store.h"
#include "scheduler.h"
#include "ble_svc.h"
#include "bus_i2c.h"
#include "sensor_transform.h"
#include "hid_host.h"

static const char *TAG = "display";

#ifndef BOARD_BUTTON_GPIO
#define BOARD_BUTTON_GPIO -1
#endif

// RGB565 colours (native order; byte-swapped on write).
#define COL_BG    0x0000
#define COL_TITLE 0x07FF
#define COL_TEXT  0xFFFF
#define COL_OK    0x07E0
#define COL_WARN  0xFD20

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static display_cfg_t s_cfg;

#if defined(BOARD_TFT_BL_I2C_ADDR)
// LP5562 4-channel I2C RGB+White LED driver — its White channel output drives this board's
// backlight FET. Per M5Stack's own official AtomS3R pinout diagram ("BL: G0 G45"), this lives
// on its own dedicated internal I2C bus (BOARD_TFT_BL_I2C_*, shared with the onboard
// BMI270/BMM150 IMU — see the imu component) — separate from BOARD_I2C_SDA_GPIO/SCL_GPIO (the
// external Port.A bus user sensors plug into). The bus itself is owned by bus_i2c2 (shared with
// the imu component, which also needs it) rather than a private handle here; this file just adds
// its own device on it, same as bus_i2c2's other callers. Register map cross-checked against the
// Linux kernel's own LP5562 driver (drivers/leds/leds-lp5562.c), the authoritative public source
// for this part.
static i2c_master_dev_handle_t s_bl_dev;

#define LP5562_REG_ENABLE   0x00
#define LP5562_REG_CONFIG   0x08
#define LP5562_REG_ENG_SEL  0x70
#define LP5562_REG_W_PWM    0x0E
// ENABLE/CONFIG values matched exactly to M5Stack's own M5GFX Light_M5StackAtomS3R::init() —
// the only real-world-proven bring-up for this chip/board pairing (see init_i2c_backlight()'s
// comment for why this is now preferred over the fuller Linux-kernel-driver-derived sequence
// this used to send). 0x40 not the Linux driver's 0xC0 (M5Stack never sets LOGARITHMIC_PWM);
// 0x01 not 0x61 (M5Stack never sets PWM_HF/PWRSAVE_EN, just CLK_INT).
#define LP5562_ENABLE_DEFAULT 0x40   // MASTER_ENABLE only
#define LP5562_CONFIG_DEFAULT 0x01   // CLK_INT (internal clock) only
#define LP5562_ENG_SEL_PWM    0x00   // no engine assigned to any channel incl. White -> direct PWM

static esp_err_t bl_i2c_write(uint8_t reg, uint8_t val)
{
    if (!s_bl_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_bl_dev, buf, sizeof(buf), 100);
}

static esp_err_t init_i2c_backlight(void)
{
    esp_err_t err = bus_i2c2_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "backlight I2C bus init: %s", esp_err_to_name(err)); return err; }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TFT_BL_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    if ((err = i2c_master_bus_add_device(bus_i2c2_handle(), &dev_cfg, &s_bl_dev)) != ESP_OK) {
        ESP_LOGE(TAG, "backlight I2C add_device (addr 0x%02x): %s", BOARD_TFT_BL_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    // Sequence and timing matched exactly to M5Stack's own M5GFX Light_M5StackAtomS3R::init() —
    // ENABLE, 1ms settle, CONFIG, ENG_SEL, brightness. Deliberately NOT the Linux kernel LP5562
    // driver's fuller sequence (which also sets OP_MODE) — that's more complete per the generic
    // datasheet, but M5Stack's simpler sequence is what's actually proven working on real
    // shipped hardware with this exact chip/board pairing, and OP_MODE has no documented effect
    // on the W channel (it only selects engine-vs-direct-PWM for R/G/B, which this board leaves
    // unpopulated — confirmed by an R/G/B cycle test during bring-up that lit nothing while W
    // worked fine).
    if ((err = bl_i2c_write(LP5562_REG_ENABLE, LP5562_ENABLE_DEFAULT)) != ESP_OK) {
        ESP_LOGE(TAG, "backlight reg 0x00 (ENABLE) write: %s", esp_err_to_name(err)); return err;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    if ((err = bl_i2c_write(LP5562_REG_CONFIG, LP5562_CONFIG_DEFAULT)) != ESP_OK) {
        ESP_LOGE(TAG, "backlight reg 0x08 (CONFIG) write: %s", esp_err_to_name(err)); return err;
    }
    if ((err = bl_i2c_write(LP5562_REG_ENG_SEL, LP5562_ENG_SEL_PWM)) != ESP_OK) {
        ESP_LOGE(TAG, "backlight reg 0x70 (ENG_SEL) write: %s", esp_err_to_name(err)); return err;
    }
    // W_PWM deliberately left at 0 here: the panel's GRAM is still uncleared at this point, and
    // display_init() turns the backlight on (via bl_set(true), at the configured brightness)
    // right after the first blank() — so power-on GRAM noise is never lit.
    ESP_LOGI(TAG, "backlight I2C bring-up ok (addr 0x%02x)", BOARD_TFT_BL_I2C_ADDR);
    return ESP_OK;
}
#endif

// Backlight on/off — a plain GPIO on most boards (binary: brightness > 0 = on), but AtomS3R
// drives it through the I2C LED driver above (BOARD_TFT_BL_GPIO is -1 there, so the GPIO branch
// is a no-op), whose W-channel PWM gives real 0-100% dimming via s_cfg.brightness.
static void bl_set(bool on)
{
    // 0-100% config → 0-255 PWM. Older saved configs predating the brightness field parse to
    // the seeded default (100), so this can't accidentally dim to zero on upgrade.
    uint8_t pct = s_cfg.brightness > 100 ? 100 : s_cfg.brightness;
#if defined(BOARD_TFT_BL_I2C_ADDR)
    bl_i2c_write(LP5562_REG_W_PWM, on ? (uint8_t)((unsigned)pct * 255u / 100u) : 0);
#endif
    if (s_cfg.bl >= 0) gpio_set_level(s_cfg.bl, (on && pct > 0) ? 1 : 0);
}

static bool s_mono;          // ssd1306 = 1bpp, else RGB565
static int  s_w, s_h;        // panel resolution after rotation
static int  s_scale, s_lh;   // font scale and line height
static uint16_t *s_strip;    // RGB565 line strip (DMA)
static uint8_t  *s_fb;       // mono full framebuffer

static inline uint16_t swap16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)io; (void)e; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

// ---- text rendering -------------------------------------------------------
// A "frame" is built top-to-bottom: begin_frame, then draw_line per text row, then end_frame.

static void begin_frame(void)
{
    if (s_mono) memset(s_fb, 0, s_w * s_h / 8);
}

// `has_swatch` draws a small filled colour box in the line's rightmost s_lh pixel columns —
// used for a colour sensor's LEGO id (colour name -> its reference RGB) or an r/g/b trio (the
// actual mixed colour), so the value is visible as a colour, not just a number, on RGB565
// panels. Mono (ssd1306) can't show colour at all, so it's simply skipped there.
//
// `small` draws at a fixed 1x glyph scale (8px) instead of the body text's s_scale, vertically
// centred in the row and with a proportionally smaller swatch — for the device-name header,
// which otherwise eats the same oversized cell width as body text for no benefit: at s_scale=2
// a 240px-wide panel only fits 14 header characters after the status swatch, truncating any
// name past that (the 15-character default included) — 1x fits up to 29.
static void draw_line(int row, const char *text, uint16_t fg, bool has_swatch, uint16_t swatch, bool small,
                       bool has_swatch2, uint16_t swatch2)
{
    int scale = small ? 1 : s_scale;
    int cols = s_w / (8 * scale);
    int y0 = row * s_lh;
    if (y0 + s_lh > s_h) return;

    if (s_mono) {
        // Mono is already fixed at native 8px glyphs (s_scale is always 1 there), so `small`
        // changes nothing on this path — just render as usual.
        for (int ci = 0; ci < cols && text[ci]; ci++) {
            char c = text[ci];
            if (c < FONT_FIRST || c > FONT_LAST) c = '?';
            const uint8_t *g = font8x8[c - FONT_FIRST];
            for (int gy = 0; gy < 8; gy++) {
                uint8_t bits = g[gy];
                for (int gx = 0; gx < 8; gx++) {
                    if (!(bits & (1 << gx))) continue;
                    int px = ci * 8 + gx, py = y0 + gy;
                    s_fb[(py / 8) * s_w + px] |= (1 << (py % 8));
                }
            }
        }
        return;
    }

    // RGB565: paint a width x line_h strip and blit it. Reserve the last one or two character
    // cells for the swatch(es) (each exactly one cell wide) so they never overwrite a digit
    // that was meant to be visible instead of silently truncating one column earlier than the
    // text expects. The header uses both: BLE connection status (existing swatch) plus the
    // gamepad connection status (swatch2), side by side.
    // A blank cell's worth of gap between the two swatches when both are present, so the two
    // status dots read as clearly separate rather than touching.
    bool swatch_gap = has_swatch && has_swatch2;
    int reserved_cells = (has_swatch ? 1 : 0) + (has_swatch2 ? 1 : 0) + (swatch_gap ? 1 : 0);
    int text_cols = (reserved_cells > 0 && cols > reserved_cells) ? cols - reserved_cells : cols;
    uint16_t bg = swap16(COL_BG), f = swap16(fg);
    for (int i = 0; i < s_w * s_lh; i++) s_strip[i] = bg;
    int y_off = small ? (s_lh - 8) / 2 : 0;      // centre the smaller glyph height in the row
    for (int ci = 0; ci < text_cols && text[ci]; ci++) {
        char c = text[ci];
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const uint8_t *g = font8x8[c - FONT_FIRST];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = g[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (!(bits & (1 << gx))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = ci * 8 * scale + gx * scale + sx;
                        int py = y_off + gy * scale + sy;
                        if (px < s_w && py < s_lh) s_strip[py * s_w + px] = f;
                    }
            }
        }
    }
    int swatch_sz = small ? 8 : (s_lh < s_w ? s_lh : s_w);   // small header: match the glyph height
    int swatch_sy0 = small ? (s_lh - swatch_sz) / 2 : 0;
    if (has_swatch) {
        uint16_t sc = swap16(swatch);
        for (int py = swatch_sy0; py < swatch_sy0 + swatch_sz; py++)
            for (int px = s_w - swatch_sz; px < s_w; px++)
                s_strip[py * s_w + px] = sc;
    }
    if (has_swatch2) {
        // One blank cell to the left of the first swatch (see swatch_gap above) — text, then
        // [gamepad][gap][BLE] reading left-to-right, the two dots clearly separated instead of
        // touching edge-to-edge.
        uint16_t sc2 = swap16(swatch2);
        int x0 = s_w - swatch_sz * (swatch_gap ? 3 : 2);
        for (int py = swatch_sy0; py < swatch_sy0 + swatch_sz; py++)
            for (int px = x0; px < x0 + swatch_sz; px++)
                s_strip[py * s_w + px] = sc2;
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, s_w, y0 + s_lh, s_strip);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

static void end_frame(void)
{
    if (s_mono) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_w, s_h, s_fb);
        xSemaphoreTake(s_done, portMAX_DELAY);
    }
}

// Fill the *entire* panel with the background, covering the full height including any
// partial row at the bottom (panel height need not be a multiple of the text line height),
// so no GDDRAM garbage shows through. Call once at startup and on blank.
static void fill_bg(void)
{
    if (s_mono) {
        memset(s_fb, 0, s_w * s_h / 8);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_w, s_h, s_fb);
        xSemaphoreTake(s_done, portMAX_DELAY);
        return;
    }
    uint16_t bg = swap16(COL_BG);
    for (int i = 0; i < s_w * s_lh; i++) s_strip[i] = bg;
    for (int y = 0; y < s_h; y += s_lh) {
        int h = (y + s_lh <= s_h) ? s_lh : (s_h - y);   // shorter final strip
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_w, y + h, s_strip);
        xSemaphoreTake(s_done, portMAX_DELAY);
    }
}

static void blank(void)
{
    fill_bg();
}

// ---- 3x3 matrix overlay ---------------------------------------------------
// Fed from the LEGO emitter (lego_emit_set_matrix_cb path) when emulating a 3×3 Color Light
// Matrix and the hub WRITEs pixels. display_show_matrix() only stores the cells + timestamp
// (it runs on the emitter task); the display task owns all panel access and renders the grid.
static uint16_t       s_matrix[9];
static volatile int64_t s_matrix_ts;        // ms; 0 = never shown
#define MATRIX_HOLD_MS 30000                 // show the grid for 30 s after the last write

void display_show_matrix(const uint16_t cells[9])
{
    for (int i = 0; i < 9; i++) s_matrix[i] = cells[i];
    s_matrix_ts = esp_timer_get_time() / 1000;
}

static void render_matrix(void)
{
    int cw = s_w / 3, ch = s_h / 3, margin = 3;
    if (s_mono) {
        memset(s_fb, 0, s_w * s_h / 8);
        for (int py = 0; py < s_h; py++) {
            int row = py / ch; if (row > 2) row = 2;
            int ly = py - row * ch;
            for (int px = 0; px < s_w; px++) {
                int col = px / cw; if (col > 2) col = 2;
                int lx = px - col * cw;
                bool on = s_matrix[row * 3 + col] != 0 &&
                          !(lx < margin || ly < margin || lx >= cw - margin || ly >= ch - margin);
                if (on) s_fb[(py / 8) * s_w + px] |= (1 << (py % 8));
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_w, s_h, s_fb);
        xSemaphoreTake(s_done, portMAX_DELAY);
        return;
    }
    for (int y = 0; y < s_h; y += s_lh) {
        int bh = (y + s_lh <= s_h) ? s_lh : (s_h - y);
        for (int sy = 0; sy < bh; sy++) {
            int py = y + sy;
            int row = py / ch; if (row > 2) row = 2;
            int ly = py - row * ch;
            for (int px = 0; px < s_w; px++) {
                int col = px / cw; if (col > 2) col = 2;
                int lx = px - col * cw;
                uint16_t c = (lx < margin || ly < margin || lx >= cw - margin || ly >= ch - margin)
                             ? COL_BG : s_matrix[row * 3 + col];
                s_strip[sy * s_w + px] = swap16(c);
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_w, y + bh, s_strip);
        xSemaphoreTake(s_done, portMAX_DELAY);
    }
}

// ---- content --------------------------------------------------------------
// Content is built as a flat list of lines, then split into physical screens that fit the
// panel height. Overflow (long lists, or multi-value sensors) wraps to extra screens that
// the BOOT button cycles through. In paged mode each sensor `page` group starts a new screen.

typedef struct { char text[40]; uint16_t color; uint16_t swatch; bool has_swatch; bool brk; } dline_t;
#define MAX_LINES 96

static void addl(dline_t *L, int *n, const char *t, uint16_t c, bool brk)
{
    if (*n >= MAX_LINES) return;
    strncpy(L[*n].text, t, sizeof L[*n].text - 1);
    L[*n].text[sizeof L[*n].text - 1] = '\0';
    L[*n].color = c;
    L[*n].swatch = 0;
    L[*n].has_swatch = false;
    L[*n].brk = brk;
    (*n)++;
}

// Attach a colour swatch to the line just added (the caller's *n has already advanced).
static void set_swatch(dline_t *L, int n, uint16_t swatch)
{
    if (n <= 0) return;
    L[n - 1].swatch = swatch;
    L[n - 1].has_swatch = true;
}

// One compact line if a single value is selected, else a name header + one line per
// Format a reading value: integral values print without decimals (e.g. colour ids and the
// whole-percent reflect match what the LEGO hub shows), everything else keeps 2 decimals.
static const char *val_fmt(float v) { return (v == (float)(long)v) ? "%-9.9s %.0f" : "%-9.9s %.2f"; }
static const char *val_fmt2(float v) { return (v == (float)(long)v) ? " %-7.7s %.0f" : " %-7.7s %.2f"; }

// Resolve a reported colour id to a display name: prefer the sensor's own taught colour (so a
// custom-named entry shows the user's name, matching the web dashboard's legoColour()), else
// fall back to the standard SPIKE reference name.
static void colour_name(const sensor_cfg_t *s, int id, char *out, size_t outsz)
{
    for (int i = 0; i < s->colour_count; i++) {
        if (s->colours[i].learned && s->colours[i].out_id == id) {
            snprintf(out, outsz, "%s", s->colours[i].name);
            return;
        }
    }
    snprintf(out, outsz, "%s", sensor_transform_colour_name(id));
}

// Resolve a reported colour id to its swatch colour: the sensor's own taught reference if one
// was captured for this id, else the standard/ideal reference palette. Deliberately NOT the
// live raw reading — that shows genuine sensor/measurement noise (crosstalk, lighting) baked
// in, which is useful for the raw r/g/b lines but not what "this is classified as X" should
// visually mean; the id swatch should show what X ideally looks like.
static uint16_t colour_swatch(const sensor_cfg_t *s, int id)
{
    for (int i = 0; i < s->colour_count; i++)
        if (s->colours[i].learned && s->colours[i].out_id == id)
            return sensor_transform_ref_rgb565(s->type, s->colours[i].ref);
    return sensor_transform_colour_rgb565(id);
}

// selected value. `brk` marks the first line as a screen-break point (paged mode).
static void add_sensor(dline_t *L, int *n, const sensor_cfg_t *s, bool brk)
{
    const char *names[MC_MAX_VALUES];
    int nn = sensor_describe(s, names, MC_MAX_VALUES);
    reading_t r;
    bool have = scheduler_get_reading(s->id, &r);
    char buf[40];

    // r/g/b trio (col_rgb255, col_full, as_full): shown as one swatched "rgb r,g,b" line
    // instead of three plain numbers — see the same grouping in the web dashboard.
    int ri = -1, gi = -1, bi = -1;
    for (int v = 0; v < nn; v++) {
        if (!strcmp(names[v], "r")) ri = v;
        else if (!strcmp(names[v], "g")) gi = v;
        else if (!strcmp(names[v], "b")) bi = v;
    }
    // rgb_present: this reading has a live r/g/b at all (regardless of value_mask). rgb_group:
    // r/g/b are actually selected, so their own lines get rendered.
    bool rgb_present = ri >= 0 && gi >= 0 && bi >= 0 && have && ri < r.count && gi < r.count && bi < r.count;
    bool rgb_group = rgb_present &&
        ((s->value_mask & (1u << ri)) || (s->value_mask & (1u << gi)) || (s->value_mask & (1u << bi)));

    // Index of "colour"/"reflect" in this sensor's value set, if present at all (regardless of
    // value_mask) — used to resolve the classified id's swatch colour.
    int ci = -1, reflect_i = -1;
    for (int v = 0; v < nn; v++) {
        if (!strcmp(names[v], "colour")) ci = v;
        else if (!strcmp(names[v], "reflect")) reflect_i = v;
    }
    bool have_colour = have && ci >= 0 && ci < r.count;
    uint16_t colour_sw = have_colour ? colour_swatch(s, (int)lroundf(r.values[ci])) : 0;
    // Attach the swatch to "reflect" instead of "colour" when reflect will actually be
    // rendered — frees the colour line to show its full name text (e.g. "light blue", or a long
    // taught name) without reserving a column for a swatch, since that reserved column was
    // truncating longer names.
    bool swatch_on_reflect = reflect_i >= 0 && (s->value_mask & (1u << reflect_i));

    // Scale against the *mode's* fixed declared range, not this sample's own peak channel —
    // own-max normalisation always stretches the brightest channel to full scale regardless of
    // true saturation, and stacked with gamma (which disproportionately boosts the weaker
    // channels) that washes any colour with two comparable channels toward white. Mirrors the
    // web dashboard's RgbGroup, which scales against sensorValueMeta's max (255 for col_rgb255,
    // 1024 for col_full/as_full) for the same reason.
    uint16_t live_sw = 0;
    if (rgb_present) {
        float mx = !strcmp(s->transform, "col_rgb255") ? 255.0f : 1024.0f;
        uint8_t rc = sensor_transform_gamma_u8((uint8_t)clampf(r.values[ri] / mx * 255.0f, 0, 255));
        uint8_t gc = sensor_transform_gamma_u8((uint8_t)clampf(r.values[gi] / mx * 255.0f, 0, 255));
        uint8_t bc = sensor_transform_gamma_u8((uint8_t)clampf(r.values[bi] / mx * 255.0f, 0, 255));
        live_sw = (uint16_t)(((rc >> 3) << 11) | ((gc >> 2) << 5) | (bc >> 3));
    }

    int sel = 0, first = -1;
    for (int v = 0; v < nn; v++)
        if (s->value_mask & (1u << v)) { sel++; if (first < 0) first = v; }

    bool first_is_colour = sel == 1 && have && first < r.count && !strcmp(names[first], "colour");
    if (sel <= 1) {
        if (first_is_colour) {
            char cname[24];
            colour_name(s, (int)lroundf(r.values[first]), cname, sizeof cname);
            snprintf(buf, sizeof buf, "%-9.9s %s", s->name, cname);
        } else if (sel == 1 && have && first < r.count) {
            snprintf(buf, sizeof buf, val_fmt(r.values[first]), s->name, r.values[first]);
        } else if (sel == 1) {
            snprintf(buf, sizeof buf, "%-9.9s --", s->name);
        } else {
            snprintf(buf, sizeof buf, "%-9.9s (none)", s->name);
        }
        if (sel == 1 && have && first < r.count && !strcmp(names[first], "reflect"))
            strncat(buf, "%", sizeof buf - strlen(buf) - 1);
        addl(L, n, buf, COL_TEXT, brk);
        // Only one line exists in this compact view, so the swatch (if this reading has a
        // colour at all) always attaches here regardless of which single value is shown.
        if (sel == 1 && have_colour) set_swatch(L, *n, colour_sw);
        return;
    }
    addl(L, n, s->name, COL_TITLE, brk);
    for (int v = 0; v < nn; v++) {
        if (!(s->value_mask & (1u << v))) continue;
        if (rgb_group && (v == gi || v == bi)) continue;   // emitted together with the r-index line below
        if (rgb_group && v == ri) {
            float rgb_val[3] = { r.values[ri], r.values[gi], r.values[bi] };
            // Separate red/green/blue lines (full width each, so larger values aren't cramped
            // into a shared comma list) all carry the *same* swatch colour — adjacent rows
            // drawing the same colour merge visually into one larger box spanning all three
            // lines, instead of one small per-line swatch.
            static const char *rgb_label[3] = { "red", "green", "blue" };
            for (int k = 0; k < 3; k++) {
                snprintf(buf, sizeof buf, val_fmt2(rgb_val[k]), rgb_label[k], rgb_val[k]);
                addl(L, n, buf, COL_TEXT, false);
                set_swatch(L, *n, live_sw);
            }
            continue;
        }
        bool v_is_colour = have && v < r.count && !strcmp(names[v], "colour");
        bool v_is_reflect = have && v < r.count && !strcmp(names[v], "reflect");
        if (v_is_colour) {
            char cname[24];
            colour_name(s, (int)lroundf(r.values[v]), cname, sizeof cname);
            snprintf(buf, sizeof buf, " %-7.7s %s", names[v], cname);
        } else if (have && v < r.count) {
            snprintf(buf, sizeof buf, val_fmt2(r.values[v]), names[v], r.values[v]);
        } else {
            snprintf(buf, sizeof buf, " %-7.7s --", names[v]);
        }
        if (v_is_reflect)
            strncat(buf, "%", sizeof buf - strlen(buf) - 1);
        addl(L, n, buf, COL_TEXT, false);
        // Swatch goes on "reflect" when it's shown (freeing the colour line's full width for
        // its name text); otherwise falls back to the colour line itself.
        if ((v_is_colour && !swatch_on_reflect) || (v_is_reflect && swatch_on_reflect))
            set_swatch(L, *n, colour_sw);
    }
}

// ---- tiles (visual) mode ---------------------------------------------------
// Redesign goal: the "summary"/"paged" text modes are for reading exact values (and reuse the
// same custom per-sensor `name` that's also the identifier in web polling and generated
// Pybricks/SPIKE code — nothing new needed there, config_store's existing sensor name already
// serves as that shared label). Tiles mode is the "glance at it across the room" counterpart:
// one small tile per shown sensor, arranged in a grid, each rendered by *shape* — a colour
// sensor's tile IS the colour, a distance sensor's tile is a fill bar, a boolean-shaped value
// is a filled-vs-outline swatch — rather than digits.
#define TILE_LABEL_H  8     // fixed-scale label row: name, or "--"/"none" when unavailable
// One tile per ticked value now (not one per sensor — see tile_value_indices), so the bound
// is sensors x an average handful of ticked values each, not just MC_MAX_SENSORS.
#define MC_MAX_TILES  48
#define TILE_VALUE_H  8     // fixed-scale value row: formatted number/name under the graphic
#define TILE_BORDER   0x39C7  // muted grey-blue outline (RGB565, native order)

typedef enum { TILE_COLOUR, TILE_BAR, TILE_BIPOLAR, TILE_BOOL, TILE_NUM, TILE_DPAD, TILE_BUTTONS } tile_kind_t;

typedef struct {
    tile_kind_t kind;
    char        label[MC_NAME_LEN + 12];   // "<sensor name> <value name>" — further truncated to fit the tile at render time
    char        value[24];            // formatted value/colour name shown under the graphic
    uint16_t    colour;      // TILE_COLOUR fill / accent colour for bar/bool/dpad/buttons
    float       frac;        // TILE_BAR: 0..1 fill. TILE_BIPOLAR: -1..1 (0 = centred)
    bool        on;          // TILE_BOOL state
    int         code;        // TILE_DPAD: 0-8 compass code (0=released, 1=up, clockwise, matches dpad's own encoding)
    uint16_t    bits;         // TILE_BUTTONS: raw bitmask, one lit cell per set bit (up to 16)
} tile_t;

// Near/mid/far colour for a distance bar: red at the close end, through orange and yellow,
// to green at the far end — reads at a glance as "something's close" vs "all clear" the way
// hazard colours normally do, instead of one fixed colour that says nothing about how close it
// is. Five stops (not the original two-segment red/amber/green) so the transition stays visibly
// graded even over a narrow slice of the range — a plain red->amber->green blend shares the
// same red channel (31) across the *entire* first half, so within any test that only explores
// part of the range (a hand waved over 0-300mm of a sensor calibrated to 2000mm max, say) the
// colour barely moved before "suddenly" reading as gold — perceptually binary despite being a
// mathematically continuous blend. More, closer-spaced stops fixes that regardless of how the
// sensor's near/far calibration happens to line up with what's actually being tested.
static uint16_t heat_colour(float frac)
{
    frac = clampf(frac, 0.0f, 1.0f);
    static const uint8_t stops[5][3] = {
        { 31, 0,  0  },   // 0.00 red    (near)
        { 31, 16, 0  },   // 0.25 orange
        { 31, 31, 0  },   // 0.50 yellow (mid)
        { 16, 40, 0  },   // 0.75 yellow-green
        { 0,  63, 0  },   // 1.00 green  (far)
    };
    float pos = frac * 4.0f;                 // 0..4 across the 5 stops
    int seg = (int)pos;
    if (seg > 3) seg = 3;
    float t = pos - (float)seg;
    const uint8_t *a = stops[seg], *b = stops[seg + 1];
    uint8_t r = (uint8_t)(a[0] + (b[0] - a[0]) * t);
    uint8_t g = (uint8_t)(a[1] + (b[1] - a[1]) * t);
    uint8_t bl = (uint8_t)(a[2] + (b[2] - a[2]) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Same blue(low)->red(high) hue sweep the M5 8Angle/Step16 units' own physical LEDs show
// (angle8_value_colour in drv_m5_8angle.c, step16_position_colour in drv_m5_step16.c —
// duplicated rather than shared for the same reason those two duplicate each other: different
// compilation units, tiny and cheap to keep in sync by eye), just re-scaled from 8-bit-per-
// channel LED output to RGB565's native 5/6/5 bit depths (like heat_colour() above) instead of
// 0-255. `frac` is 0..1 (blue) .. 1 (red)... i.e. 0 = blue, 1 = red, matching hue 240°..0°.
static uint16_t hue_sweep_colour(float frac)
{
    frac = clampf(frac, 0.0f, 1.0f);
    unsigned hue = 240u - (unsigned)(frac * 240u);
    unsigned seg = hue / 60, rem = hue % 60;
    // rem01: how far through this 60°-wide segment (0..1) — scaled to each channel's own bit
    // depth (5-bit R/B 0-31, 6-bit G 0-63) at the point it's actually used, rather than
    // precomputing both rise and fall for every channel up front (most go unused per segment).
    float rem01 = rem / 60.0f;
    uint8_t r, g, b;
    switch (seg) {
    case 0:  r = 31;                        g = (uint8_t)(rem01 * 63u); b = 0;  break;   // red -> yellow
    case 1:  r = (uint8_t)(31u - rem01*31u); g = 63;                    b = 0;  break;   // yellow -> green
    case 2:  r = 0;                         g = 63;  b = (uint8_t)(rem01 * 31u); break;   // green -> cyan
    default: r = 0;    g = (uint8_t)(63u - rem01*63u); b = 31;                  break;   // cyan -> blue (seg 3, hue 180-240)
    }
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Which of a sensor's values should each become their own tile: every explicitly ticked
// value_mask bit, or just the first value if none are ticked at all (so a freshly-added
// sensor with no selection yet still shows something). Previously compute_tile picked only
// the FIRST ticked value for the sensor's one-and-only tile — ticking multiple monitor boxes
// (e.g. a gamepad's rx/ry/dpad) silently dropped every box after the first instead of giving
// each its own tile, which is what "tick multiple, see multiple" actually implies.
static int tile_value_indices(const sensor_cfg_t *s, int *out, int max)
{
    const char *names[MC_MAX_VALUES];
    int nn = sensor_describe(s, names, MC_MAX_VALUES);
    int n = 0;
    for (int v = 0; v < nn && n < max; v++)
        if (s->value_mask & (1u << v)) out[n++] = v;
    if (n == 0 && nn > 0 && max > 0) out[n++] = 0;   // nothing ticked — still show the first
    return n;
}

// Reduce one (sensor, value index) pair to a single tile, classifying it by name/shape —
// mirrors the same name-based recognition add_sensor() already uses for colour/reflect (and
// the web dashboard's equivalent), plus recognition for distance, quantized gamepad values,
// and plain booleans. The label combines sensor name + value name ("gamepad-1 rx") — tail
// truncation (see tail_trunc) naturally shrinks that down to just the value name on a small
// tile and keeps more of the sensor name on a bigger one, so a multi-tile sensor's tiles stay
// distinguishable without needing separate narrow/wide label logic.
static void compute_tile(const sensor_cfg_t *s, int value_index, tile_t *t)
{
    memset(t, 0, sizeof(*t));
    t->colour = COL_OK;

    const char *names[MC_MAX_VALUES];
    int nn = sensor_describe(s, names, MC_MAX_VALUES);
    reading_t r;
    bool have = scheduler_get_reading(s->id, &r);

    if (value_index >= 0 && value_index < nn)
        snprintf(t->label, sizeof t->label, "%s %s", s->name, names[value_index]);
    else
        snprintf(t->label, sizeof t->label, "%s", s->name);

    if (!have || value_index < 0 || value_index >= nn || value_index >= r.count) {
        t->kind = TILE_NUM;
        snprintf(t->value, sizeof t->value, "--");
        t->colour = COL_WARN;
        return;
    }

    const char *nm = names[value_index];
    float v = r.values[value_index];

    if (!strcmp(nm, "colour")) {
        int id = (int)lroundf(v);
        if (id < 0) {
            t->kind = TILE_NUM;
            snprintf(t->value, sizeof t->value, "none");
            t->colour = COL_BG;
        } else {
            t->kind = TILE_COLOUR;
            colour_name(s, id, t->value, sizeof t->value);
            t->colour = colour_swatch(s, id);
        }
        return;
    }
    if (!strcmp(nm, "dist")) {
        float lo = s->dist_min_mm;
        // Fallback matches the sensor's OWN native ceiling (sensor_dist_native_max_mm), not a
        // generic guess — a VL53L1X in short-range mode maxes out its own reading at 1300mm,
        // so if the display used a different fallback (the old hardcoded 2000mm) a genuinely
        // "no target in range" reading landed at 1300/2000 = 65% fill, never reaching the top
        // of the bar even though the sensor is reporting its true maximum.
        float hi = s->dist_max_mm > lo ? s->dist_max_mm : sensor_dist_native_max_mm(s);
        t->kind = TILE_BAR;
        t->frac = clampf((v - lo) / (hi - lo), 0, 1);
        t->colour = heat_colour(t->frac);   // red (near) -> orange -> yellow -> green (far)
        snprintf(t->value, sizeof t->value, "%.0f", v);
        return;
    }
    if (!strcmp(nm, "reflect")) {
        t->kind = TILE_BAR;
        t->frac = clampf(v / 100.0f, 0, 1);
        snprintf(t->value, sizeof t->value, "%.0f%%", v);
        t->colour = COL_TITLE;
        return;
    }
    if (!strcmp(nm, "lt15") || !strcmp(nm, "rt15")) {          // quantized gamepad triggers 0-15
        t->kind = TILE_BAR;
        t->frac = clampf(v / 15.0f, 0, 1);
        snprintf(t->value, sizeof t->value, "%.0f", v);
        t->colour = COL_WARN;
        return;
    }
    if (!strcmp(nm, "lx7") || !strcmp(nm, "ly7") || !strcmp(nm, "rx7") || !strcmp(nm, "ry7")) {
        t->kind = TILE_BIPOLAR;                                 // quantized gamepad sticks -7..7
        t->frac = clampf(v / 7.0f, -1, 1);
        snprintf(t->value, sizeof t->value, "%.0f", v);
        return;
    }
    // dpad's 0-8 hat code isn't a magnitude (unlike distance/reflect/triggers) or a simple
    // on/off (unlike a detected flag) — it's a discrete DIRECTION, so neither a bar nor a
    // single swatch says anything useful. A 3x3 grid mirroring the physical layout of a real
    // D-pad — one lit cell showing which of the 8 directions (or centre/released) is current —
    // reads at a glance the way the number never would. Rendered in render_tile_grid.
    if (!strcmp(nm, "dpad") || !strcmp(nm, "ldir") || !strcmp(nm, "rdir")) {
        static const char *dir_name[9] = { "released", "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        t->kind = TILE_DPAD;
        t->code = (int)lroundf(v);
        if (t->code < 0 || t->code > 8) t->code = 0;
        t->colour = COL_OK;
        snprintf(t->value, sizeof t->value, "%s", dir_name[t->code]);
        return;
    }
    // buttons is a 16-bit bitmask (one flag per button, including the dpad folded into 4 of
    // the bits) — not a single state at all, so it gets a grid of small indicator cells, one
    // per bit, lit exactly when that button is currently held. Genuinely the "coloured or
    // uncoloured swatch per button" idea, generalised to all 16 at once instead of one bit.
    if (!strcmp(nm, "buttons")) {
        t->kind = TILE_BUTTONS;
        t->bits = (uint16_t)clampf(v, 0, 65535);
        t->colour = COL_WARN;
        int pressed = 0;
        for (int bit = 0; bit < 16; bit++) if (t->bits & (1u << bit)) pressed++;
        snprintf(t->value, sizeof t->value, "%d pressed", pressed);
        return;
    }
    // M5 8Angle knobs (k0-k7, 12-bit 0-4095) and Step16's position (0-15): without a case here
    // both fell through to the generic TILE_NUM branch below, which renders as a plain digit —
    // no bar, no fill, no colour at all — so turning the physical knob only changed the printed
    // number while the tile's body stayed a flat, unmoving background. Bar + the same blue->red
    // hue sweep the unit's own LED shows, so the display visually agrees with the hardware.
    if (nm[0] == 'k' && nm[1] >= '0' && nm[1] <= '7' && nm[2] == '\0') {
        t->kind = TILE_BAR;
        t->frac = clampf(v / 4095.0f, 0, 1);
        t->colour = hue_sweep_colour(t->frac);
        snprintf(t->value, sizeof t->value, "%.0f", v);
        return;
    }
    if (!strcmp(nm, "position")) {
        t->kind = TILE_BAR;
        t->frac = clampf(v / 15.0f, 0, 1);
        t->colour = hue_sweep_colour(t->frac);
        snprintf(t->value, sizeof t->value, "%.0f", v);
        return;
    }
    // INA226 power monitor: voltage (mV), current (mA), power (mW), pct (battery %)
    if (!strcmp(nm, "voltage_mV")) {
        t->kind = TILE_BAR;
        // Li-ion: 3000-4200mV nominal range
        t->frac = clampf((v - 3000.0f) / 1200.0f, 0, 1);
        t->colour = heat_colour(t->frac);   // red (low) -> orange -> yellow -> green (high)
        snprintf(t->value, sizeof t->value, "%.0fmV", v);
        return;
    }
    if (!strcmp(nm, "current_mA")) {
        t->kind = TILE_BIPOLAR;
        // Typical range: -8000 to +8000 mA (discharge/charge)
        t->frac = clampf(v / 8000.0f, -1, 1);
        snprintf(t->value, sizeof t->value, "%.0fmA", v);
        return;
    }
    if (!strcmp(nm, "power_mW")) {
        t->kind = TILE_BAR;
        // Typical range: 0-25000 mW (based on 3000mA * 4200mV max)
        t->frac = clampf(v / 25000.0f, 0, 1);
        t->colour = heat_colour(t->frac);   // red (high power) -> orange -> yellow -> green (low)
        snprintf(t->value, sizeof t->value, "%.0fmW", v);
        return;
    }
    if (!strcmp(nm, "pct")) {
        t->kind = TILE_BAR;
        // Battery percentage: 0-100%
        t->frac = clampf(v / 100.0f, 0, 1);
        t->colour = heat_colour(t->frac);   // red (low) -> orange -> yellow -> green (full)
        snprintf(t->value, sizeof t->value, "%.0f%%", v);
        return;
    }
    // Plain boolean-shaped values (a digital "detected" flag, a gpio pin's high/low state, the
    // 8Angle's physical slide switch): exactly 0 or 1 and named like a flag rather than a
    // genuinely 0/1-ranged measurement.
    if ((v == 0.0f || v == 1.0f) && (strstr(nm, "detected") || !strcmp(nm, "state") || !strcmp(nm, "switch"))) {
        t->kind = TILE_BOOL;
        t->on = v != 0.0f;
        snprintf(t->value, sizeof t->value, t->on ? "on" : "off");
        return;
    }

    t->kind = TILE_NUM;
    snprintf(t->value, sizeof t->value, (v == (float)(long)v) ? "%.0f" : "%.2f", v);
}

// Draw one glyph at an arbitrary pixel offset within a scanline chunk buffer (RGB565). Used
// for tile label/value text, which is laid out on pixel boundaries a tile grid computes, not
// the fixed row-height grid draw_line() assumes.
static void put_glyph(uint16_t *dst, int dst_w, int dst_h, int x0, int y0, char c, uint16_t fg)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *g = font8x8[c - FONT_FIRST];
    for (int gy = 0; gy < 8; gy++) {
        int py = y0 + gy;
        if (py < 0 || py >= dst_h) continue;
        uint8_t bits = g[gy];
        for (int gx = 0; gx < 8; gx++) {
            if (!(bits & (1 << gx))) continue;
            int px = x0 + gx;
            if (px < 0 || px >= dst_w) continue;
            dst[py * dst_w + px] = swap16(fg);
        }
    }
}

// Centre `text` (<= maxch already truncated by the caller) within [x, x+w) at pixel row y0.
static void put_text_centered(uint16_t *dst, int dst_w, int dst_h, int x, int w, int y0, const char *text, uint16_t fg)
{
    int len = (int)strlen(text);
    int tw = len * 8;
    int x0 = x + (w - tw) / 2;
    for (int i = 0; i < len; i++) put_glyph(dst, dst_w, dst_h, x0 + i * 8, y0, text[i], fg);
}

static void set_mono_px(int px, int py, bool on)
{
    if (px < 0 || px >= s_w || py < 0 || py >= s_h) return;
    if (on) s_fb[(py / 8) * s_w + px] |= (1 << (py % 8));
}

// Draw one glyph directly into the mono framebuffer at an arbitrary pixel offset — same idea
// as put_glyph()'s RGB565 path, needed here because the boot splash centres text vertically at
// pixel offsets that don't line up with draw_line()'s fixed row grid.
static void put_glyph_mono(int x0, int y0, char c)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *g = font8x8[c - FONT_FIRST];
    for (int gy = 0; gy < 8; gy++) {
        uint8_t bits = g[gy];
        for (int gx = 0; gx < 8; gx++)
            if (bits & (1 << gx)) set_mono_px(x0 + gx, y0 + gy, true);
    }
}

// Same red as the web app's header brick logo (App.tsx's <LogoBrick>, #D5192A body / #f0505f
// stud highlight), converted to RGB565, so the physical board's splash reads as "the same
// product" instead of a generic bootloader message.
#define BRICK_BODY 0xD0C5
#define BRICK_STUD 0xF9CB

// Is (px,py) part of the brick glyph occupying [x0,x0+w) x [y0,y0+h)? A body rectangle (bottom
// ~62%) with two round studs sitting fully above the body's top edge (not overlapping it) — the
// same 2x2-brick shape and proportions as the web logo (App.tsx's <LogoBrick>, viewBox 0-28:
// body y=10..26, studs r=4.1 @ cy=6.4), just filled solid rather than traced/gradient-shaded,
// since that's simplest to render as plain pixel tests with no image/vector dependency. `*stud`
// reports which part it was, so the RGB565 caller can two-tone it (mono just fills either the
// same).
static bool brick_px(int px, int py, int x0, int y0, int w, int h, bool *stud)
{
    int body_y0 = y0 + (h * 10) / 26;
    bool in_body = px >= x0 && px < x0 + w && py >= body_y0 && py < y0 + h;
    int r = (w * 41) / 280; if (r < 2) r = 2;
    int cy = y0 + (h * 64) / 260;
    int cx1 = x0 + w / 4, cx2 = x0 + w * 3 / 4;
    bool in_stud1 = (px - cx1) * (px - cx1) + (py - cy) * (py - cy) <= r * r;
    bool in_stud2 = (px - cx2) * (px - cx2) + (py - cy) * (py - cy) <= r * r;
    if (stud) *stud = in_stud1 || in_stud2;
    return in_body || in_stud1 || in_stud2;
}

// "Brix", the web app's mascot (web/src/components/Mascot.tsx), ported to a geometric pixel test
// the same way brick_px() above renders the logo: a body test per pixel against Mascot.tsx's own
// 0-96 viewBox coordinates (scaled to whatever [x0,y0,w,h] box it's drawn into here), rather than
// loading an actual image — this firmware has no image/vector asset pipeline, only primitive
// shape tests. Only the "happy" pose (Mascot's default) — arms drawn straight down rather than
// reproducing the mood-dependent rotation, which isn't worth the added trig for a boot splash
// shown once for a second or two. Colours/coordinates copied directly from that file so the
// physical board's splash reads as "the same character" as the web app's welcome screen/Guide
// tab, not a generic mascot.
typedef enum { BX_NONE, BX_LEG, BX_BODY, BX_BELT, BX_ARM, BX_HEAD, BX_STUD, BX_EYE, BX_MOUTH } brix_part_t;

#define BRIX_BLUE   0x02D5   // #0059ab body
#define BRIX_YELLOW 0xFEA0   // #ffd500 head/arms/stud/belt
#define BRIX_LEG    0x3A0A   // #3a4250 legs
#define BRIX_DARK   0x1926   // #1c2431 eyes/mouth

static brix_part_t brix_part(int px, int py, int x0, int y0, int w, int h)
{
    // Normalise (px,py) into Mascot.tsx's own 0-96 square coordinate space so every region test
    // below can just copy that file's literal rect/circle coordinates.
    float fx = (px - x0) * 96.0f / (float)w;
    float fy = (py - y0) * 96.0f / (float)h;

    if (fx >= 34 && fx < 44 && fy >= 76 && fy < 92) return BX_LEG;
    if (fx >= 52 && fx < 62 && fy >= 76 && fy < 92) return BX_LEG;

    float sdx = fx - 48, sdy = fy - 6;
    if (sdx * sdx + sdy * sdy <= 36) return BX_STUD;   // head stud, circle r6 @ (48,6)

    if (fx >= 30 && fx < 66 && fy >= 14 && fy < 46) {   // head rect
        float e1x = fx - 40, e1y = fy - 30; if (e1x * e1x + e1y * e1y <= 6) return BX_EYE;
        float e2x = fx - 56, e2y = fy - 30; if (e2x * e2x + e2y * e2y <= 6) return BX_EYE;
        if (fy >= 37 && fy < 40 && fx >= 39 && fx < 57) return BX_MOUTH;   // smile band
        return BX_HEAD;
    }
    if (fx >= 26 && fx < 35 && fy >= 46 && fy < 72) return BX_ARM;   // left arm
    if (fx >= 61 && fx < 70 && fy >= 46 && fy < 72) return BX_ARM;   // right arm
    if (fx >= 26 && fx < 70 && fy >= 46 && fy < 80) {                 // body rect
        float bdx = fx - 48, bdy = fy - 63;
        if (bdx * bdx + bdy * bdy <= 36) return BX_BELT;   // belt circle r6 @ (48,63)
        return BX_BODY;
    }
    return BX_NONE;
}

static uint16_t brix_colour(brix_part_t part)
{
    switch (part) {
    case BX_LEG:  return BRIX_LEG;
    case BX_BODY: return BRIX_BLUE;
    case BX_EYE: case BX_MOUTH: return BRIX_DARK;
    default: return BRIX_YELLOW;   // belt/arm/head/stud
    }
}

// Brief splash shown once at boot (before the panel starts its normal status/tile screens):
// Brix, the brick logo, and the board's own name, centred — so a glance at the physical device
// confirms which board this is (and that it's a MultiController) the instant it powers on.
// Skippable via display_cfg_t.show_boot_logo for the fastest possible startup (see
// display_init()). Brix/the logo are skipped on panels too short to fit them without cramming
// the text (e.g. a 64px-tall mono OLED) — falls back to the text-only layout there.
static void render_boot_splash(void)
{
    const char *name = config_store_get_device_name();
    const char *sub = "starting...";
    const int gap_brix_logo = 8, gap_logo_title = 10, band_h = 8, gap_title_sub = 5;
    const int text_h = band_h + gap_title_sub + band_h;   // title + subtitle, always shown
    bool show_logo = s_h >= 96;
    int logo_w = 0, logo_h = 0, logo_x0 = 0, logo_y0 = 0;
    int brix_w = 0, brix_h = 0, brix_x0 = 0, brix_y0 = 0;
    if (show_logo) {
        logo_w = s_w / 4; if (logo_w < 24) logo_w = 24; if (logo_w > 56) logo_w = 56;
        brix_w = s_w / 3; if (brix_w < 32) brix_w = 32; if (brix_w > 72) brix_w = 72;
        logo_h = logo_w; brix_h = brix_w;

        // Brix + the brick logo are both square boxes sized off s_w alone (no idea yet how tall
        // the panel actually is) — on a wide-but-short panel (e.g. 240x135) their combined
        // height easily overshoots what's actually available above the text, which would push
        // the title/subtitle bands off the bottom of the screen entirely (or, worse, feed
        // esp_lcd_panel_draw_bitmap y-coordinates beyond s_h with no bounds check of its own).
        // Scale both down together, proportionally, until they actually fit.
        int char_budget = s_h - text_h - gap_logo_title;   // room left for Brix + logo + their gap
        int char_h = brix_h + gap_brix_logo + logo_h;
        if (char_h > char_budget && char_h > 0) {
            float scale = (float)(char_budget > 0 ? char_budget : 0) / (float)char_h;
            brix_w = (int)(brix_w * scale); brix_h = brix_w;
            logo_w = (int)(logo_w * scale); logo_h = logo_w;
            if (brix_w < 16 || logo_w < 16) { show_logo = false; brix_w = brix_h = logo_w = logo_h = 0; }
        }
        if (show_logo) {
            logo_x0 = (s_w - logo_w) / 2;
            brix_x0 = (s_w - brix_w) / 2;
        }
    }
    // Centre the whole splash (Brix + logo + title + subtitle) as one block, top to bottom —
    // vertical position is derived from the block's total height, not eyeballed per-element
    // offsets from the middle (that previously put a tall logo's y0 above row 0 on a 135px
    // panel, clipping its top off instead of centring it).
    int total_h = (show_logo ? brix_h + gap_brix_logo + logo_h + gap_logo_title : 0) + text_h;
    int block_y0 = (s_h - total_h) / 2;
    if (block_y0 < 0) block_y0 = 0;
    if (show_logo) {
        brix_y0 = block_y0;
        logo_y0 = brix_y0 + brix_h + gap_brix_logo;
    }
    int ty = (show_logo ? logo_y0 + logo_h + gap_logo_title : block_y0);
    int sy = ty + band_h + gap_title_sub;

    if (s_mono) {
        memset(s_fb, 0, s_w * s_h / 8);
        if (show_logo) {
            for (int py = brix_y0; py < brix_y0 + brix_h; py++)
                for (int px = brix_x0; px < brix_x0 + brix_w; px++) {
                    brix_part_t part = brix_part(px, py, brix_x0, brix_y0, brix_w, brix_h);
                    // Eyes/mouth are gaps punched out of the head silhouette rather than a
                    // separate colour — mono has no colour, so this is the only way the face
                    // reads as anything but a plain blob.
                    if (part != BX_NONE && part != BX_EYE && part != BX_MOUTH) set_mono_px(px, py, true);
                }
            for (int py = logo_y0; py < logo_y0 + logo_h; py++)
                for (int px = logo_x0; px < logo_x0 + logo_w; px++)
                    if (brick_px(px, py, logo_x0, logo_y0, logo_w, logo_h, NULL)) set_mono_px(px, py, true);
        }
        int tx = (s_w - (int)strlen(name) * 8) / 2;
        for (int i = 0; name[i]; i++) put_glyph_mono(tx + i * 8, ty, name[i]);
        int sx = (s_w - (int)strlen(sub) * 8) / 2;
        for (int i = 0; sub[i]; i++) put_glyph_mono(sx + i * 8, sy, sub[i]);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_w, s_h, s_fb);
        xSemaphoreTake(s_done, portMAX_DELAY);
        return;
    }

    fill_bg();

    if (show_logo) {
        for (int y0 = brix_y0; y0 < brix_y0 + brix_h; y0 += s_lh) {
            int band_rows = (y0 + s_lh <= brix_y0 + brix_h) ? s_lh : (brix_y0 + brix_h - y0);
            for (int i = 0; i < s_w * s_lh; i++) s_strip[i] = swap16(COL_BG);
            for (int ry = 0; ry < band_rows; ry++) {
                int py = y0 + ry;
                for (int px = brix_x0; px < brix_x0 + brix_w; px++) {
                    brix_part_t part = brix_part(px, py, brix_x0, brix_y0, brix_w, brix_h);
                    if (part != BX_NONE) s_strip[ry * s_w + px] = swap16(brix_colour(part));
                }
            }
            esp_lcd_panel_draw_bitmap(s_panel, 0, y0, s_w, y0 + band_rows, s_strip);
            xSemaphoreTake(s_done, portMAX_DELAY);
        }

        for (int y0 = logo_y0; y0 < logo_y0 + logo_h; y0 += s_lh) {
            int band_rows = (y0 + s_lh <= logo_y0 + logo_h) ? s_lh : (logo_y0 + logo_h - y0);
            for (int i = 0; i < s_w * s_lh; i++) s_strip[i] = swap16(COL_BG);
            for (int ry = 0; ry < band_rows; ry++) {
                int py = y0 + ry;
                for (int px = logo_x0; px < logo_x0 + logo_w; px++) {
                    bool stud = false;
                    if (brick_px(px, py, logo_x0, logo_y0, logo_w, logo_h, &stud))
                        s_strip[ry * s_w + px] = swap16(stud ? BRICK_STUD : BRICK_BODY);
                }
            }
            esp_lcd_panel_draw_bitmap(s_panel, 0, y0, s_w, y0 + band_rows, s_strip);
            xSemaphoreTake(s_done, portMAX_DELAY);
        }
    }

    for (int i = 0; i < s_w * band_h; i++) s_strip[i] = swap16(COL_BG);
    put_text_centered(s_strip, s_w, band_h, 0, s_w, 0, name, COL_TITLE);
    esp_lcd_panel_draw_bitmap(s_panel, 0, ty, s_w, ty + band_h, s_strip);
    xSemaphoreTake(s_done, portMAX_DELAY);

    for (int i = 0; i < s_w * band_h; i++) s_strip[i] = swap16(COL_BG);
    put_text_centered(s_strip, s_w, band_h, 0, s_w, 0, sub, TILE_BORDER);
    esp_lcd_panel_draw_bitmap(s_panel, 0, sy, s_w, sy + band_h, s_strip);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

// When a label must be truncated to fit, keep the TAIL rather than the head: auto-generated
// sensor names are "type-N" (vl53l1x-4, vl53l1x-6, as7341-12, ...) — the distinguishing part
// is the trailing number, so a head-truncated "vl53l" is not just less informative, it's
// IDENTICAL across every sensor of the same type, reading as a missing/broken label rather
// than a merely short one.
static const char *tail_trunc(const char *s, int maxch)
{
    int len = (int)strlen(s);
    return (len > maxch) ? s + (len - maxch) : s;
}

// Render a tile grid into the vertical slice [y0, y0+avail_h) of the panel — one screen's worth
// of shown sensors normally (y0 = the one header line's height, avail_h = the rest of the
// panel), or one category's own block when render_tile_category_blocks() below is splitting the
// screen into several stacked grids. `fixed_count` is the web's "tiles per screen" setting (0 =
// auto-fit by aspect ratio; else 1/2/4/8 with a dedicated clean layout) — see the tiles_per_page
// comment on display_cfg_t. A fixed layout keeps its shape even when this particular screen has
// fewer than `fixed_count` tiles (the last, partially-filled screen of a group): consistent tile
// size/position screen-to-screen was judged more predictable than the grid shrinking on the
// final page.
// `prev_tiles` is the previous frame's tile array at the same grid positions (NULL if the
// layout changed since last frame, e.g. page/tpp/tile-count — in that case labels must be
// redrawn unconditionally since index i no longer means the same tile). Used only to skip
// re-blitting label bands whose text hasn't changed — see the label-row loop below.
static void render_tile_grid(const tile_t *tiles, int nt, int y0_top, int avail_h, int fixed_count, const tile_t *prev_tiles)
{
    if (nt <= 0) return;
    if (avail_h < 16) return;                     // no realistic room for tiles
    int header_h = y0_top;   // kept as `header_h` below — it's just this block's own top offset

    int cols, rows;
    if (fixed_count > 0) {
        switch (fixed_count) {
            case 1:  cols = 1; rows = 1; break;
            case 2:  cols = 2; rows = 1; break;
            case 4:  cols = 2; rows = 2; break;
            case 8:  cols = 4; rows = 2; break;
            default: cols = 1; rows = 1; break;
        }
    } else {
        // Pick a column count that keeps tiles roughly square given the panel's own aspect
        // ratio (a 240x135 landscape panel wants more columns than rows for the same tile
        // count) rather than a fixed grid — looks right on both little square-ish OLEDs and
        // wide TFTs.
        float aspect = (float)s_w / (float)avail_h;
        cols = (int)ceilf(sqrtf((float)nt * aspect));
        if (cols < 1) cols = 1;
        if (cols > nt) cols = nt;
        // A tile narrower than ~7 characters (8px font) truncates every label to a
        // near-identical prefix — e.g. 4 sensors named "vl53l1x-4/6/8/10" all showed as just
        // "vl53l", making them indistinguishable and reading as "the name is missing". Trade
        // some grid squareness for a wider column floor instead.
        #define TILE_MIN_W 56
        while (cols > 1 && s_w / cols < TILE_MIN_W) cols--;
        rows = (nt + cols - 1) / cols;
    }
    int tile_w = s_w / cols;
    int tile_h = avail_h / rows;
    if (nt > cols * rows) nt = cols * rows;   // safety net: never index past this layout's cells

    // A dense grid (many values from one sensor — e.g. m5_8angle's 9 tiles: k0-k7 + switch) can
    // make tile_h too short to fit both the label band (top) and value band (bottom) without
    // overlapping. The value/body fill redraws every single frame (values change constantly),
    // but the label is only redrawn when its text changes (see label_row_changed below, kept
    // that way specifically to avoid flicker) — so when the two bands overlap, the every-frame
    // value/bar redraw progressively overwrites the once-drawn label underneath it, and it never
    // comes back since nothing redraws that band again. Rather than let that happen, skip
    // drawing the value band (and shrink the body/bar to fit above it) whenever there isn't
    // genuinely enough room for both — the label identifies which sensor/value this even is,
    // which matters more than the redundant numeric readout the bar/colour already conveys.
    bool value_fits = tile_h >= TILE_LABEL_H + TILE_VALUE_H + 2;

    if (s_mono) {
        for (int i = 0; i < nt; i++) {
            const tile_t *t = &tiles[i];
            int col = i % cols, row = i / cols;
            int x = col * tile_w, y = header_h + row * tile_h;
            // Border.
            for (int px = x; px < x + tile_w; px++) { set_mono_px(px, y, true); set_mono_px(px, y + tile_h - 1, true); }
            for (int py = y; py < y + tile_h; py++) { set_mono_px(x, py, true); set_mono_px(x + tile_w - 1, py, true); }
            // Body fill fraction (mono has no colour, so TILE_COLOUR falls back to the name
            // text only — same as TILE_NUM below).
            int body_y0 = y + TILE_LABEL_H + 1, body_y1 = value_fits ? y + tile_h - TILE_VALUE_H - 1 : y + tile_h - 1;
            if (body_y1 > body_y0) {
                if (t->kind == TILE_BAR) {
                    int fill_y = body_y1 - (int)((body_y1 - body_y0) * t->frac);
                    for (int py = fill_y; py < body_y1; py++)
                        for (int px = x + 2; px < x + tile_w - 2; px++) set_mono_px(px, py, true);
                } else if (t->kind == TILE_BIPOLAR) {
                    int midx = x + tile_w / 2;
                    int half = (tile_w - 4) / 2;
                    int span = (int)(half * fabsf(t->frac));
                    int from = t->frac >= 0 ? midx : midx - span;
                    int to   = t->frac >= 0 ? midx + span : midx;
                    for (int py = body_y0; py < body_y1; py++)
                        for (int px = from; px < to; px++) set_mono_px(px, py, true);
                    for (int py = body_y0; py < body_y1; py++) set_mono_px(midx, py, true);
                } else if (t->kind == TILE_BOOL && t->on) {
                    for (int py = body_y0; py < body_y1; py++)
                        for (int px = x + 2; px < x + tile_w - 2; px++) set_mono_px(px, py, true);
                }
            }
            // Label + value text (mono font is drawn straight into s_fb via put_glyph's RGB
            // path being unavailable here — reuse the bit-set primitive per glyph column).
            const char *texts[2] = { t->label, t->value };
            for (int li = 0; li < (value_fits ? 2 : 1); li++) {
                int ty = li == 0 ? y + 1 : y + tile_h - TILE_VALUE_H - 1;
                char buf[24];
                int maxch = (tile_w - 2) / 8;
                if (maxch < 1) continue;
                if (maxch > (int)sizeof(buf) - 1) maxch = (int)sizeof(buf) - 1;
                snprintf(buf, sizeof buf, "%.*s", maxch, tail_trunc(texts[li], maxch));
                int len = (int)strlen(buf);
                int tx = x + (tile_w - len * 8) / 2;
                for (int ci = 0; ci < len; ci++) {
                    char c = buf[ci];
                    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
                    const uint8_t *g = font8x8[c - FONT_FIRST];
                    for (int gy = 0; gy < 8; gy++) {
                        uint8_t bits = g[gy];
                        for (int gx = 0; gx < 8; gx++)
                            if (bits & (1 << gx)) set_mono_px(tx + ci * 8 + gx, ty + gy, true);
                    }
                }
            }
        }
        return;
    }

    // Per grid row: does any tile's label text differ from last frame? Computed once up front
    // (not per-chunk) so both the periodic body/border repaint below and the dedicated label
    // pass further down agree on exactly which rows are safe to leave untouched. This is the
    // fix for labels getting silently erased: the periodic repaint used to blit every tile's
    // FULL rectangle — including its label rows — every single frame regardless of this flag,
    // so a label drawn once by the (skip-if-unchanged) label pass was immediately painted over
    // with plain background by the very next frame's body/bar redraw, which runs unconditionally
    // every frame since values change constantly. The label pass's "only redraw when changed"
    // optimization only helps if nothing else re-touches that pixel region in between.
    bool row_label_changed[MC_MAX_TILES];   // rows <= nt <= MC_MAX_TILES (worst case: 1 column)
    for (int r = 0; r < rows && r < MC_MAX_TILES; r++) {
        bool changed = (prev_tiles == NULL);
        if (!changed) {
            for (int col = 0; col < cols; col++) {
                int i = r * cols + col;
                if (i >= nt) break;
                if (strcmp(tiles[i].label, prev_tiles[i].label) != 0) { changed = true; break; }
            }
        }
        row_label_changed[r] = changed;
    }

    // RGB565: composite scanline chunks into s_strip (sized s_w x s_lh) and blit each chunk,
    // same buffer/approach render_matrix() already uses — keeps memory to one line-strip
    // regardless of how tall a tile is.
    int y_end = header_h + avail_h;
    for (int y0 = header_h; y0 < y_end; y0 += s_lh) {
        int chunk_h = (y0 + s_lh <= y_end) ? s_lh : (y_end - y0);
        for (int i = 0; i < chunk_h * s_w; i++) s_strip[i] = swap16(COL_BG);

        for (int i = 0; i < nt; i++) {
            const tile_t *t = &tiles[i];
            int col = i % cols, row = i / cols;
            int tx = col * tile_w, ty = header_h + row * tile_h;
            if (ty + tile_h <= y0 || ty >= y0 + chunk_h) continue;   // this tile isn't in this chunk

            int body_y0 = ty + TILE_LABEL_H + 1, body_y1 = value_fits ? ty + tile_h - TILE_VALUE_H - 1 : ty + tile_h - 1;
            for (int py = ty; py < ty + tile_h && py < y0 + chunk_h; py++) {
                if (py < y0) continue;
                int ly = py - y0;
                for (int px = tx; px < tx + tile_w; px++) {
                    bool border = (py == ty || py == ty + tile_h - 1 || px == tx || px == tx + tile_w - 1);
                    uint16_t c = COL_BG;
                    if (border) {
                        c = TILE_BORDER;
                    } else if (py >= body_y0 && py < body_y1) {
                        switch (t->kind) {
                        case TILE_COLOUR: c = t->colour; break;
                        case TILE_BAR: {
                            int fill_y = body_y1 - (int)((body_y1 - body_y0) * t->frac);
                            c = (py >= fill_y) ? t->colour : COL_BG;
                            break;
                        }
                        case TILE_BIPOLAR: {
                            int midx = tx + tile_w / 2;
                            int half = (tile_w - 4) / 2;
                            int span = (int)(half * fabsf(t->frac));
                            bool filled = t->frac >= 0 ? (px >= midx && px < midx + span)
                                                        : (px >= midx - span && px < midx);
                            c = (filled || px == midx) ? t->colour : COL_BG;
                            break;
                        }
                        case TILE_BOOL: c = t->on ? t->colour : COL_BG; break;
                        case TILE_DPAD: {
                            // 3x3 grid of small dots mirroring a real D-pad's physical layout
                            // (corners = diagonals, edges = cardinal directions, centre =
                            // released) — one lit, the rest dim, instead of a bare direction
                            // number. Gaps between dots (the `margin` band) come from the
                            // per-pixel modulo test below, so no separate "clear the gaps" pass
                            // is needed.
                            static const int grid[3][3] = { { 8, 1, 2 }, { 7, 0, 3 }, { 6, 5, 4 } };
                            int cw = tile_w / 3, ch = (body_y1 - body_y0) / 3;
                            if (cw < 1) cw = 1;
                            if (ch < 1) ch = 1;
                            int rx = px - tx, ry = py - body_y0;
                            int cc = rx / cw; if (cc > 2) cc = 2;
                            int rr = ry / ch; if (rr > 2) rr = 2;
                            int margin = (cw < ch ? cw : ch) / 4;
                            int lx = rx % cw, lyy = ry % ch;
                            bool dot = lx >= margin && lx < cw - margin && lyy >= margin && lyy < ch - margin;
                            c = !dot ? COL_BG : (grid[rr][cc] == t->code ? t->colour : TILE_BORDER);
                            break;
                        }
                        case TILE_BUTTONS: {
                            // One small indicator dot per button bit (up to 16, laid out 4x4):
                            // lit when that button is currently held, dim otherwise — the
                            // "coloured or uncoloured swatch per button" idea, all 16 at once.
                            int cw = tile_w / 4, ch = (body_y1 - body_y0) / 4;
                            if (cw < 1) cw = 1;
                            if (ch < 1) ch = 1;
                            int rx = px - tx, ry = py - body_y0;
                            int cc = rx / cw; if (cc > 3) cc = 3;
                            int rr = ry / ch; if (rr > 3) rr = 3;
                            int bit = rr * 4 + cc;
                            int margin = (cw < ch ? cw : ch) / 4;
                            int lx = rx % cw, lyy = ry % ch;
                            bool dot = lx >= margin && lx < cw - margin && lyy >= margin && lyy < ch - margin;
                            c = !dot ? COL_BG : (((t->bits >> bit) & 1) ? t->colour : TILE_BORDER);
                            break;
                        }
                        default: c = COL_BG; break;
                        }
                    }
                    s_strip[ly * s_w + px] = swap16(c);
                }
            }
        }

        // Blit only the sub-spans of this chunk that are safe to repaint every frame — a row is
        // excluded when it falls in a tile's label band (rows 0..TILE_LABEL_H of its grid row)
        // AND that row's label text hasn't changed since last frame: s_strip's composited
        // content for those rows is correct background/border, but blitting it would still
        // overwrite the label text a human just read there with that background, since nothing
        // re-draws the label itself this frame. Everything else (body/bar/value bands, or a
        // label row whose text DID just change) blits normally.
        int ly = 0;
        while (ly < chunk_h) {
            int py = y0 + ly;
            int rel = py - header_h;
            int r = rel / tile_h;
            int off = rel - r * tile_h;
            bool stable_label_row = (r >= 0 && r < rows && r < MC_MAX_TILES) &&
                                     off <= TILE_LABEL_H && !row_label_changed[r];
            if (stable_label_row) { ly++; continue; }
            int start = ly;
            do {
                ly++;
                if (ly >= chunk_h) break;
                py = y0 + ly; rel = py - header_h; r = rel / tile_h; off = rel - r * tile_h;
                stable_label_row = (r >= 0 && r < rows && r < MC_MAX_TILES) &&
                                    off <= TILE_LABEL_H && !row_label_changed[r];
            } while (!stable_label_row);
            esp_lcd_panel_draw_bitmap(s_panel, 0, y0 + start, s_w, y0 + ly, s_strip + start * s_w);
            xSemaphoreTake(s_done, portMAX_DELAY);
        }
    }

    // Label/value text: composited one shared full-width 8px strip per grid ROW (not per
    // tile) — every tile in that row draws into the same buffer before it's blitted once.
    // Blitting per-tile at full panel width would each overwrite the *other* tiles already
    // drawn in that row's band (draw_bitmap always covers the full width here), silently
    // leaving only the last tile's text behind; row-at-a-time composition is what makes every
    // tile's label/value survive.
    for (int row = 0; row < rows; row++) {
        // Labels are static per tile (sensor name + value name) — they only ever change on a
        // layout change (handled by prev_tiles==NULL above) or a device rename, unlike values
        // which redraw every frame something moved. Re-blitting an unchanged label band anyway
        // is what read as label flicker: each blit is a visible SPI write even when the pixels
        // it writes are identical to what's already there. Skip it whenever every tile in this
        // row's label text matches last frame's — row_label_changed was computed once above and
        // is shared with the periodic body/border repaint, so both agree on which rows are safe
        // to leave alone.
        bool label_row_changed = (row < MC_MAX_TILES) ? row_label_changed[row] : true;
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0 && !label_row_changed) continue;
            if (pass == 1 && !value_fits) continue;   // no room without overlapping the label band
            int ty = header_h + row * tile_h;
            int y0 = pass == 0 ? ty : ty + tile_h - TILE_VALUE_H;
            for (int i2 = 0; i2 < s_w * 8; i2++) s_strip[i2] = swap16(COL_BG);

            bool any = false;
            for (int col = 0; col < cols; col++) {
                int i = row * cols + col;
                if (i >= nt) break;
                const tile_t *t = &tiles[i];
                int tx = col * tile_w;
                char buf[24];
                int maxch = (tile_w - 2) / 8;
                if (maxch < 1) continue;
                if (maxch > (int)sizeof(buf) - 1) maxch = (int)sizeof(buf) - 1;
                const char *src = pass == 0 ? t->label : t->value;
                snprintf(buf, sizeof buf, "%.*s", maxch, tail_trunc(src, maxch));
                uint16_t fg = (pass == 0 || t->kind == TILE_NUM || t->kind == TILE_COLOUR) ? COL_TEXT : t->colour;
                put_text_centered(s_strip, s_w, 8, tx, tile_w, 0, buf, fg);
                any = true;
            }
            if (!any) continue;
            // Text rows sit just inside the tile border (label at the top edge, value at the
            // bottom edge — see body_y0/body_y1 in the fill pass above, which reserve exactly
            // these bands) so this blit only ever overwrites border/background pixels there,
            // never a colour/bar fill.
            esp_lcd_panel_draw_bitmap(s_panel, 0, y0, s_w, y0 + 8, s_strip);
            xSemaphoreTake(s_done, portMAX_DELAY);
        }
    }
}

// Distinct page numbers among shown sensors (sorted ascending). Returns count (<=max).
static int collect_pages(int *pages, int max)
{
    const sensor_cfg_t *arr;
    size_t n;
    config_store_get(&arr, &n);
    int np = 0;
    for (size_t i = 0; i < n; i++) {
        if (!arr[i].show) continue;
        int p = arr[i].page, seen = 0;
        for (int k = 0; k < np; k++) if (pages[k] == p) seen = 1;
        if (!seen && np < max) pages[np++] = p;
    }
    for (int a = 0; a < np; a++)
        for (int b = a + 1; b < np; b++)
            if (pages[b] < pages[a]) { int t = pages[a]; pages[a] = pages[b]; pages[b] = t; }
    return np;
}

// Header: the board's own configured name (config_store device_name — the same name shown in
// a BLE scan and editable from the web header), with a small connection-status swatch (green =
// a client is connected, grey = advertising/idle) instead of a separate status line. Previously
// this was a fixed "MultiController" title plus a whole line of "BLE connected/advertising" —
// generic boilerplate that didn't say anything about *this* board and cost two lines on small
// panels. One custom, informative line instead.
static void add_header(dline_t *L, int *n)
{
    addl(L, n, config_store_get_device_name(), COL_TITLE, false);
    set_swatch(L, *n, ble_svc_is_connected() ? COL_OK : TILE_BORDER);
}

static int build_lines(dline_t *L, bool paged)
{
    int n = 0;
    const sensor_cfg_t *arr;
    size_t cnt;
    config_store_get(&arr, &cnt);
    char buf[40];

    if (!paged) {
        add_header(L, &n);
        for (size_t i = 0; i < cnt; i++)
            if (arr[i].show) add_sensor(L, &n, &arr[i], false);
        if (n == 1) addl(L, &n, "no sensors shown", COL_WARN, false);
        return n;
    }

    int pages[16];
    int np = collect_pages(pages, 16);
    add_header(L, &n);
    if (np == 0) {
        addl(L, &n, "no sensors shown", COL_WARN, false);
        return n;
    }
    for (int pi = 0; pi < np; pi++) {
        snprintf(buf, sizeof buf, "Page %d", pages[pi]);
        // brk=true forces paginate() to start a fresh physical screen right here — needed for
        // every group *after* the first, but not the first: paginate()'s do-while consumes at
        // least one line before it can check the next line's brk flag, so a brk immediately
        // after the header (pi==0) made it stop right there, stranding the header alone on its
        // own screen — showing as a blank "page 1" with every group's sensors pushed one
        // screen later than expected (observed with a single page-0 group: header alone on
        // screen 1, "Page 0" + all sensors on screen 2).
        addl(L, &n, buf, COL_TITLE, pi > 0);
        for (size_t i = 0; i < cnt; i++)
            if (arr[i].show && arr[i].page == pages[pi]) add_sensor(L, &n, &arr[i], false);
    }
    return n;
}

// Record the start line of each physical screen (<= body lines, breaking early on `brk`).
static int paginate(const dline_t *L, int n, int body, int *starts, int max)
{
    int ns = 0, i = 0;
    while (i < n && ns < max) {
        starts[ns++] = i;
        int c = 0;
        do { c++; i++; } while (i < n && c < body && !L[i].brk);
    }
    return ns < 1 ? 1 : ns;
}

// Draw the screen at *page_idx; returns the total screen count (caller wraps page_idx).
// `lines`/`starts` are static (BSS, not stack) — they are large and only the display task
// touches them, so keeping them off the 4 KB task stack avoids a stack overflow.
static int render_screen(int rows, bool paged, int *page_idx)
{
    static dline_t lines[MAX_LINES];
    static int starts[MAX_LINES];
    int n = build_lines(lines, paged);

    int screens = paginate(lines, n, rows, starts, MAX_LINES);
    int body = rows;
    if (screens > 1) { body = rows - 1; screens = paginate(lines, n, body, starts, MAX_LINES); }  // reserve a row for the indicator
    if (*page_idx >= screens) *page_idx = 0;

    int start = starts[*page_idx];
    int end   = (*page_idx + 1 < screens) ? starts[*page_idx + 1] : n;
    int count = end - start;
    if (count > body) count = body;

    begin_frame();
    for (int i = 0; i < count; i++) {
        // add_header() always adds the device-name line first (dline index 0), and it only
        // ever lands on the first physical screen (start==0) — that's the one header row to
        // render small AND to carry the second (gamepad) status swatch alongside the existing
        // BLE one; every other line is plain body text at the normal scale.
        bool is_header = (start + i) == 0;
        draw_line(i, lines[start + i].text, lines[start + i].color, lines[start + i].has_swatch,
                  lines[start + i].swatch, is_header,
                  is_header, hid_host_is_connected() ? COL_OK : TILE_BORDER);
    }
    int clear_to = (screens > 1) ? rows - 1 : rows;
    for (int r = count; r < clear_to; r++) draw_line(r, "", COL_BG, false, 0, false, false, 0);   // blank remaining rows
    if (screens > 1) {
        char ind[40];
        snprintf(ind, sizeof ind, "%d/%d %s", *page_idx + 1, screens, paged ? "BOOT>" : "more>");
        draw_line(rows - 1, ind, COL_WARN, false, 0, false, false, 0);
    }
    end_frame();
    return screens;
}

// Sensor-type -> display category, ported directly from sensorCategory() in the web dashboard
// (web/src/components/Dashboard.tsx) so "group tiles by sensor type" on the physical display
// matches the Dashboard's own "group by type" grouping exactly — kept in two languages by
// necessity (different compilation units/runtimes), small enough to keep in sync by eye, same
// as the hue-sweep colour helper above. Order matters: CAT_DISTANCE..CAT_OTHER is also the
// display order of category blocks, matching the web's CATEGORY_ORDER.
typedef enum { CAT_DISTANCE, CAT_COLOUR, CAT_MOTION, CAT_ENV, CAT_LINE, CAT_INPUT, CAT_OTHER, CAT_COUNT } tile_category_t;
static const char *CAT_LABEL[CAT_COUNT] = { "Distance", "Colour", "Motion", "Environment", "Line / IR", "Input", "Other" };

static tile_category_t category_for_type(const char *type)
{
    if (!strcmp(type, "vl53l1x") || !strcmp(type, "vl53l0x") || !strcmp(type, "tof10120") || !strcmp(type, "tofi2c")) return CAT_DISTANCE;
    if (!strcmp(type, "tcs34725") || !strcmp(type, "as7341")) return CAT_COLOUR;
    if (!strcmp(type, "qmi8658") || !strcmp(type, "bmi270_bmm150")) return CAT_MOTION;
    if (!strcmp(type, "bmp280") || !strcmp(type, "bme280")) return CAT_ENV;
    if (!strcmp(type, "qre1113") || !strcmp(type, "tssp_ir")) return CAT_LINE;
    if (!strcmp(type, "gamepad") || !strcmp(type, "gpio") || !strcmp(type, "adc") || !strcmp(type, "mcp3208") || !strcmp(type, "vk36n16")) return CAT_INPUT;
    return CAT_OTHER;   // includes m5_8angle/m5_step16 (knobs/rotary) — same fallthrough as the web
}

// One-line category label, drawn centred at absolute row `y` — mono and RGB565 need different
// primitives (mono sets bits straight into s_fb via set_mono_px; RGB565 composites into s_strip
// and blits), same split render_tile_grid() itself already makes.
// Must be <= s_lh (the scanline strip buffer's allocated height, s_w * s_lh uint16s — see
// heap_caps_malloc for s_strip): CAT_HEADER_H used to be 10, but s_lh is 8*s_scale (8 on an
// unscaled panel), so the RGB565 path below was writing s_w*10 entries into a buffer sized for
// only s_w*8 — a heap buffer overflow on every category header drawn, which is what crashed the
// display task. TILE_LABEL_H (also 8) is guaranteed <= s_lh for any s_scale >= 1, so reusing it
// keeps this always in bounds.
#define CAT_HEADER_H TILE_LABEL_H
static void draw_category_header(int y, const char *text)
{
    if (s_mono) {
        int len = (int)strlen(text);
        if (len > 24) len = 24;
        int tx = (s_w - len * 8) / 2;
        if (tx < 0) tx = 0;
        for (int ci = 0; ci < len; ci++) {
            char c = text[ci];
            if (c < FONT_FIRST || c > FONT_LAST) c = '?';
            const uint8_t *g = font8x8[c - FONT_FIRST];
            for (int gy = 0; gy < 8; gy++) {
                uint8_t bits = g[gy];
                for (int gx = 0; gx < 8; gx++)
                    if (bits & (1 << gx)) set_mono_px(tx + ci * 8 + gx, y + gy, true);
            }
        }
        return;
    }
    for (int i = 0; i < s_w * CAT_HEADER_H; i++) s_strip[i] = swap16(COL_BG);
    put_text_centered(s_strip, s_w, CAT_HEADER_H, 0, s_w, 0, text, COL_TITLE);
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_w, y + CAT_HEADER_H, s_strip);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

// Render each non-empty category as its own header + tile block, stacked vertically, each
// getting a slice of the remaining height proportional to how many tiles it holds (a bigger
// category gets more room, rather than every block getting an identical, often-wasteful share).
// `cat_count[c]` is how many of `tiles`' entries (already laid out contiguously in category
// order by render_tiles()) belong to category `c`; `prev_tiles`, if non-NULL, is the previous
// frame's tiles in that SAME order (guaranteed by render_tiles() only ever passing a non-NULL
// prev_tiles when the tile set/order provably hasn't changed since last frame) — passed straight
// through to each category's own render_tile_grid() call at the matching offset, so the
// stable-label-row optimisation still works per block, no separate bookkeeping needed here.
static void render_tile_category_blocks(const tile_t *tiles, const int *cat_count, int y0_top, const tile_t *prev_tiles)
{
    int total = 0, nonempty = 0;
    for (int c = 0; c < CAT_COUNT; c++) { total += cat_count[c]; if (cat_count[c] > 0) nonempty++; }
    if (total <= 0 || nonempty == 0) return;

    int avail_h = s_h - y0_top;
    int body_avail = avail_h - nonempty * CAT_HEADER_H;
    if (body_avail < 16) {
        // Not enough room to usefully split into blocks — fall back to one flat grid rather
        // than render nothing or something illegibly small.
        render_tile_grid(tiles, total, y0_top, avail_h, 0, prev_tiles);
        return;
    }

    int y = y0_top, offset = 0, drawn = 0;
    for (int c = 0; c < CAT_COUNT; c++) {
        if (cat_count[c] <= 0) continue;
        drawn++;
        // The last drawn category absorbs any rounding remainder so the blocks always exactly
        // fill the available height instead of leaving a gap at the bottom.
        int slice_h = (drawn == nonempty)
            ? (y0_top + avail_h) - y
            : CAT_HEADER_H + body_avail * cat_count[c] / total;

        draw_category_header(y, CAT_LABEL[c]);
        render_tile_grid(tiles + offset, cat_count[c], y + CAT_HEADER_H, slice_h - CAT_HEADER_H, 0,
                          prev_tiles ? prev_tiles + offset : NULL);

        y += slice_h;
        offset += cat_count[c];
    }
}

// Tiles ("visual") mode entry point: same device-name header as the text modes, then every
// shown sensor as a grid tile (render_tile_grid). Always groups by each sensor's `page` (like
// "paged" text mode) so BOOT still cycles between groups once there are more sensors than fit
// legibly on one grid — harmless when every sensor is on page 0 (the common case), which just
// yields a single screen.
// `force`: bypass the unchanged-skip below and redraw even if nothing changed — needed when
// waking from auto-sleep, since blank() actually erased the panel's pixels while asleep, so
// "nothing changed since last render" is no longer true of what's physically on screen.
static int render_tiles(int rows, int *page_idx, bool force)
{
    const sensor_cfg_t *arr;
    size_t cnt;
    config_store_get(&arr, &cnt);

    int pages[16];
    int np = collect_pages(pages, 16);
    int tpp = s_cfg.tiles_per_page;      // 0 = auto; else a fixed 1/2/4/8 tiles-per-screen cap
    // A fixed tiles_per_page count (1/2/4/8) takes precedence over grouping: when tpp is set,
    // grouping is disabled and tiles are shown in a flat list. Grouping only applies in auto mode.
    bool use_grouping = s_cfg.group_tiles && (tpp == 0);  // grouping ignored if fixed tpp

    // Each page GROUP (existing per-sensor `page` field) can now also split across several
    // physical SCREENS when tpp caps it below the group's TILE count (a sensor with several
    // ticked monitor boxes contributes one tile per box, not one) — e.g. 9 tiles from sensors
    // on page 0 with tpp=4 becomes 3 screens of that one group, BOOT-cycled just like distinct
    // page groups always were. group_chunks[] holds how many screens each group contributes;
    // the flat *page_idx is mapped to (group, chunk-within-group) below.
    int group_count[16] = {0};
    for (int pi = 0; pi < np; pi++)
        for (size_t i = 0; i < cnt; i++) {
            if (!arr[i].show || arr[i].page != pages[pi]) continue;
            int idxs[MC_MAX_VALUES];
            group_count[pi] += tile_value_indices(&arr[i], idxs, MC_MAX_VALUES);
        }

    int group_chunks[16];
    int screens = 0;
    for (int pi = 0; pi < np; pi++) {
        group_chunks[pi] = (tpp > 0) ? (group_count[pi] + tpp - 1) / tpp : 1;
        if (group_chunks[pi] < 1) group_chunks[pi] = 1;
        screens += group_chunks[pi];
    }
    if (screens < 1) screens = 1;
    if (*page_idx >= screens) *page_idx = 0;

    int idx = *page_idx, gi = 0, chunk = 0;
    for (gi = 0; gi < np; gi++) {
        if (idx < group_chunks[gi]) { chunk = idx; break; }
        idx -= group_chunks[gi];
    }
    int page_no = np > 0 ? pages[gi] : 0;

    // Collect this screen's tiles: every ticked value of every shown sensor on `page_no`
    // (tile_value_indices — one tile per ticked box, not one per sensor), sliced to the
    // tpp-sized window [chunk*tpp, chunk*tpp+tpp) in that group's own order (or the whole
    // group when tpp==0 — auto mode never sub-chunks, a group is always exactly one screen).
    int slice_lo = (tpp > 0) ? chunk * tpp : 0;
    int slice_hi = (tpp > 0) ? slice_lo + tpp : MC_MAX_TILES;   // MC_MAX_TILES = effectively unbounded here
    static tile_t tiles[MC_MAX_TILES];
    int nt = 0, seen = 0;
    // cat_count[c] = how many of tiles[]'s entries belong to category c — meaningless (left
    // zeroed) when grouping is off. Populated by laying tiles[] out in category order below
    // instead of sensor-array order, so each category's tiles end up contiguous and
    // render_tile_category_blocks() can slice tiles[]/prev_tiles[] by a plain offset+count per
    // category rather than needing its own separate storage.
    int cat_count[CAT_COUNT] = {0};
    if (use_grouping) {
        for (int c = 0; c < CAT_COUNT; c++) {
            int start = nt;
            for (size_t i = 0; i < cnt && nt < MC_MAX_TILES; i++) {
                if (!arr[i].show || (np > 0 && arr[i].page != page_no)) continue;
                if (category_for_type(arr[i].type) != c) continue;
                int idxs[MC_MAX_VALUES];
                int nv = tile_value_indices(&arr[i], idxs, MC_MAX_VALUES);
                for (int k = 0; k < nv && nt < MC_MAX_TILES; k++) { compute_tile(&arr[i], idxs[k], &tiles[nt]); nt++; }
            }
            cat_count[c] = nt - start;
        }
    } else {
        for (size_t i = 0; i < cnt && nt < MC_MAX_TILES; i++) {
            if (!arr[i].show || (np > 0 && arr[i].page != page_no)) continue;
            int idxs[MC_MAX_VALUES];
            int nv = tile_value_indices(&arr[i], idxs, MC_MAX_VALUES);
            for (int k = 0; k < nv && nt < MC_MAX_TILES; k++) {
                if (seen >= slice_lo && seen < slice_hi) { compute_tile(&arr[i], idxs[k], &tiles[nt]); nt++; }
                seen++;
            }
        }
    }

    // Skip the actual repaint when nothing has changed since last time: with no double
    // buffering, a full tile-grid redraw is many sequential DMA blits (one per scanline chunk
    // plus one per label/value row), and re-running that every 500ms regardless of whether any
    // reading actually moved is what read as flicker — most readings are static far more often
    // than they change. compute_tile() memsets every tile_t up front, so a plain memcmp is a
    // reliable "did the rendered content change" check (no uninitialised-padding false
    // positives). Button presses/page changes and connection-status flips still force a redraw.
    static tile_t prev_tiles[MC_MAX_TILES];
    static int prev_nt = -1, prev_page_idx = -1, prev_tpp = -1;
    static bool prev_connected, prev_hid_connected, prev_group_tiles;
    bool connected = ble_svc_is_connected();
    bool hid_connected = hid_host_is_connected();
    bool unchanged = !force && nt == prev_nt && *page_idx == prev_page_idx && tpp == prev_tpp &&
                     connected == prev_connected && hid_connected == prev_hid_connected &&
                     (nt == 0 || memcmp(tiles, prev_tiles, sizeof(tile_t) * (size_t)nt) == 0);
    if (unchanged) return screens;
    // Same grid layout as last frame (nt/page/tpp unchanged) means tiles[i] and prev_tiles[i]
    // are the same logical tile position-for-position, so render_tile_grid can compare labels
    // to skip re-blitting rows whose labels haven't changed. A layout change means index i no
    // longer names the same tile, so force a full label redraw (prev_tiles = NULL) instead.
    // Also invalidated by grouping mode flipping: tiles[] is laid out in a completely different
    // order (category-grouped vs sensor order) even if nt/page/tpp all happen to stay the same.
    bool layout_same = nt == prev_nt && *page_idx == prev_page_idx && tpp == prev_tpp &&
                        use_grouping == prev_group_tiles;

    begin_frame();                      // mono: clears s_fb; RGB565: no-op (each draw blits itself)
    draw_line(0, config_store_get_device_name(), COL_TITLE, true, connected ? COL_OK : TILE_BORDER, true,
              true, hid_connected ? COL_OK : TILE_BORDER);
    if (nt == 0) {
        draw_line(1 < rows ? 1 : 0, "no sensors shown", COL_WARN, false, 0, false, false, 0);
        for (int r = 2; r < rows; r++) draw_line(r, "", COL_BG, false, 0, false, false, 0);
    } else if (use_grouping) {
        // mono: writes more bits into s_fb; RGB565: blits its own bands
        render_tile_category_blocks(tiles, cat_count, s_lh, layout_same ? prev_tiles : NULL);
    } else {
        // mono: writes more bits into s_fb; RGB565: blits its own bands
        render_tile_grid(tiles, nt, s_lh, s_h - s_lh, tpp, layout_same ? prev_tiles : NULL);
    }
    end_frame();                        // mono: one blit of the whole frame; RGB565: no-op

    memcpy(prev_tiles, tiles, sizeof(tile_t) * (size_t)nt);
    prev_nt = nt; prev_page_idx = *page_idx; prev_tpp = tpp; prev_connected = connected; prev_hid_connected = hid_connected;
    prev_group_tiles = use_grouping;
    return screens;
}

// ---- task -----------------------------------------------------------------

static bool button_is_held(void)
{
    if (BOARD_BUTTON_GPIO < 0) return false;
    return gpio_get_level(BOARD_BUTTON_GPIO) == 0;  // active-low
}

static bool button_pressed(bool *last_high)
{
    if (BOARD_BUTTON_GPIO < 0) return false;
    bool high = gpio_get_level(BOARD_BUTTON_GPIO);
    bool press = (*last_high && !high);     // active-low; falling edge = press
    *last_high = high;
    return press;
}

static void refresh_task(void *arg)
{
    (void)arg;
    int rows = s_h / s_lh;
    int page_idx = 0, screens = 1;
    bool last_high = true;
    int64_t last_draw = 0;
    bool was_enabled = true, was_blank = false;
    // Auto-sleep (idle backlight-off, see display_cfg_t.sleep_after_s): separate from the
    // enabled/disabled toggle above so the two don't fight over the backlight GPIO — a config
    // change to disabled always wins, and re-enabling always wakes.
    bool asleep = false;
    int64_t last_activity_ms = esp_timer_get_time() / 1000;
    uint8_t last_brightness = s_cfg.brightness;

    // Button hold tracking: this task tracks the hold only to suppress the page-advance on
    // release (see button_hold_3s_fired at the page_idx step below). The BLE toggle itself
    // belongs to button_ctrl.c and must NOT also be done here: both tasks poll the same GPIO,
    // so a single 3-second hold fired both, one enabling BLE and the other immediately
    // disabling it again milliseconds later. It looked like BLE refusing to stay on, and only
    // button_ctrl logs the toggle, so the log showed an unexplained "BLE enabled" followed by
    // "BLE toggled (on → off)". button_ctrl owns it because it debounces properly (20 ms poll
    // + DEBOUNCE_MS) and runs even when the display fails to init or is turned off, whereas
    // this loop's period stretches to 150 ms when the display is disabled.
    int64_t button_press_time = 0;   // when button went down (ms), 0 if not held
    bool button_hold_3s_fired = false;  // hold already recognised (suppresses the page advance)

    for (;;) {
        config_store_get_display(&s_cfg);                 // pick up live mode/enable changes
        bool paged = !strcmp(s_cfg.mode, "paged");
        bool tiled = !strcmp(s_cfg.mode, "tiles");

        // Button hold tracking runs independently of display state so BLE toggle works always
        int64_t now = esp_timer_get_time() / 1000;
        bool pressed = button_pressed(&last_high);
        bool held = button_is_held();

        if (pressed) {
            button_press_time = now;
            button_hold_3s_fired = false;
            last_activity_ms = now;
        }

        // Recognise the same 3-second hold button_ctrl acts on, purely so the release that
        // follows doesn't also flip the page. The toggle itself is button_ctrl's job.
        if (held && button_press_time > 0 && !button_hold_3s_fired) {
            int64_t hold_time = now - button_press_time;
            if (hold_time >= 3000) button_hold_3s_fired = true;
        }

        // Button released
        if (!held && button_press_time > 0) {
            button_press_time = 0;
        }

        // Apply a brightness change from a config save immediately — bl_set() reads
        // s_cfg.brightness itself, but is otherwise only called on enable/sleep transitions,
        // so without this a new brightness wouldn't take effect until the next such transition
        // (or a reboot).
        if (s_cfg.brightness != last_brightness) {
            last_brightness = s_cfg.brightness;
            if (s_cfg.enabled && !asleep) bl_set(true);
        }

        if (!s_cfg.enabled) {
            if (!was_blank) { blank(); bl_set(false); was_blank = true; }
            was_enabled = false;
            asleep = false;
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        if (!was_enabled) { bl_set(true); was_blank = false; }
        was_enabled = true;

        // Auto-sleep: after `sleep_after_s` seconds with no button press, turn the backlight
        // off to save power/panel life on a status display that mostly nobody's looking at.
        // -1 disables this entirely (screen always on — the previous, only behaviour). Sensors
        // keep polling and the config keeps updating in RAM regardless; a press instantly wakes
        // and forces a redraw (only wake source — this board has no touchscreen).
        bool woke = false;
        if (s_cfg.sleep_after_s >= 0 && now - last_activity_ms >= (int64_t)s_cfg.sleep_after_s * 1000) {
            if (!asleep) { blank(); bl_set(false); asleep = true; }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        if (asleep) { bl_set(true); asleep = false; woke = true; }

        // Button advances page in any mode (except the very press that just woke the display,
        // and except long holds which toggle BLE instead).
        bool redraw = woke;
        if (pressed && !woke && !button_hold_3s_fired) { page_idx = (page_idx + 1) % (screens > 0 ? screens : 1); redraw = true; }

        // While a 3×3 matrix write is fresh, show the grid instead of the status screen.
        if (s_matrix_ts && now - s_matrix_ts < MATRIX_HOLD_MS) {
            if (now - last_draw >= 100) { render_matrix(); last_draw = now; }
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        if (now - last_draw >= 500) { redraw = true; last_draw = now; }

        if (redraw) screens = tiled ? render_tiles(rows, &page_idx, woke) : render_screen(rows, paged, &page_idx);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// ---- init -----------------------------------------------------------------

static esp_err_t init_spi_panel(void)
{
    // bus_spi shares this host and normally initialises it first (main.c calls bus_spi_init()
    // before display_init()), with a max_transfer_sz that comfortably covers our line strips —
    // only bring the bus up ourselves if it hasn't. Calling spi_bus_initialize() on an already-
    // up bus "works" (INVALID_STATE, tolerated below as a backstop) but makes the IDF driver
    // print a spurious "SPI bus already initialized" error log every boot.
    esp_err_t err = ESP_OK;
    if (!bus_spi_is_initialized()) {
        // BOARD_TFT_* pins, NOT BOARD_SPI_* — on boards whose display shares the sensor SPI bus
        // the two macro sets are identical so either works, but on a board with a dedicated
        // display-only bus (AtomS3R: TFT SCLK=15/MOSI=21, while the sensor BOARD_SPI_* pins are
        // all -1 because no external SPI exists) using BOARD_SPI_* here initialised the SPI
        // peripheral with NO output pins routed at all. Every transaction then "succeeds"
        // internally — CS/DC/RST still toggle (they're configured separately below from the
        // right pins) and every init log reads clean — but SCLK/MOSI never reach the physical
        // pins, so the panel receives nothing and the screen stays black with zero errors
        // anywhere. The hardest kind of wrong: fully green logs, dead glass.
        spi_bus_config_t bus = {
            .sclk_io_num = BOARD_TFT_SCLK_GPIO,
            .mosi_io_num = BOARD_TFT_MOSI_GPIO,
            .miso_io_num = BOARD_TFT_MISO_GPIO,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = s_w * s_lh * (int)sizeof(uint16_t) + 64,
        };
        err = spi_bus_initialize(BOARD_TFT_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;   // shared with bus_spi
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = s_cfg.cs, .dc_gpio_num = s_cfg.dc,
        .spi_mode = 0, .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 7,
        .on_color_trans_done = on_trans_done,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    if ((err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_TFT_HOST, &io_cfg, &s_io)) != ESP_OK)
        return err;

    // st7735s/gc9107 panels take BGR element order: LovyanGFX/M5GFX's Panel_LCD default
    // (rgb_order=false) sets the MADCTL BGR bit for these families, a detail missed when the
    // init tables were first ported — empirically confirmed on the AtomS3R's ST7735S, where a
    // pure-red RGB565 test fill (0xF800) rendered blue until this was flipped. st7789/ili9341
    // keep RGB, matching the boards they were already rendering correctly on.
    bool bgr = !strcmp(s_cfg.controller, "st7735s") || !strcmp(s_cfg.controller, "gc9107");
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = s_cfg.rst,
        .rgb_ele_order = bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (!strcmp(s_cfg.controller, "ili9341"))
        err = esp_lcd_new_panel_ili9341(s_io, &pcfg, &s_panel);
    else if (!strcmp(s_cfg.controller, "gc9107"))
        err = esp_lcd_new_panel_gc9107(s_io, &pcfg, &s_panel);
    else if (!strcmp(s_cfg.controller, "st7735s"))
        err = esp_lcd_new_panel_st7735s(s_io, &pcfg, &s_panel);
    else
        err = esp_lcd_new_panel_st7789(s_io, &pcfg, &s_panel);
    if (err != ESP_OK) return err;

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, s_cfg.invert);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, s_cfg.mirror_x, s_cfg.mirror_y);
    esp_lcd_panel_set_gap(s_panel, s_cfg.x_gap, s_cfg.y_gap);
    esp_lcd_panel_disp_on_off(s_panel, true);

    if (s_cfg.bl >= 0) {
        gpio_config_t bl = { .pin_bit_mask = 1ULL << s_cfg.bl, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&bl);
    }
#if defined(BOARD_TFT_BL_I2C_ADDR)
    if (init_i2c_backlight() != ESP_OK) ESP_LOGE(TAG, "backlight I2C bring-up failed");
#endif
    // Deliberately NOT bl_set(true) here — the panel's GRAM is uninitialised random noise at
    // power-on, and DISPON just happened above, so lighting the backlight now shows a flash of
    // scrambled pixels until the first clear. display_init() turns it on right after blank().
    return ESP_OK;
}

static esp_err_t init_i2c_panel(void)
{
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = s_cfg.addr, .scl_speed_hz = 400000,
        .control_phase_bytes = 1, .dc_bit_offset = 6,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = on_trans_done,
    };
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus_i2c_handle(), &io_cfg, &s_io);
    if (err != ESP_OK) return err;

    esp_lcd_panel_ssd1306_config_t vendor = { .height = (uint8_t)s_cfg.height };
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = s_cfg.rst, .bits_per_pixel = 1, .vendor_config = &vendor,
    };
    if ((err = esp_lcd_new_panel_ssd1306(s_io, &pcfg, &s_panel)) != ESP_OK) return err;

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_mirror(s_panel, s_cfg.mirror_x, s_cfg.mirror_y);
    esp_lcd_panel_invert_color(s_panel, s_cfg.invert);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return ESP_OK;
}

esp_err_t display_init(void)
{
    config_store_get_display(&s_cfg);

    bool is_i2c = !strcmp(s_cfg.controller, "ssd1306");
    // No display hardware described → run display-less (also covers BOARD_HAS_DISPLAY 0).
    if (!is_i2c && s_cfg.cs < 0) { ESP_LOGI(TAG, "no display configured"); return ESP_OK; }
    if (s_cfg.width <= 0 || s_cfg.height <= 0) { ESP_LOGI(TAG, "no display size"); return ESP_OK; }

    s_mono  = is_i2c;
    s_w     = s_cfg.width;
    s_h     = s_cfg.height;
    // 2x font scale was sized for 240px-wide panels (15 chars/line); on a small 128px panel it
    // leaves only 8 chars per line — sensor names don't fit. Native 8px glyphs there instead
    // (16 chars/line on 128px, and 16 rows on a square 128px panel).
    s_scale = (s_mono || s_w < 200) ? 1 : 2;
    s_lh    = 8 * s_scale;

    s_done = xSemaphoreCreateBinary();
    if (!s_done) return ESP_ERR_NO_MEM;

    esp_err_t err = is_i2c ? init_i2c_panel() : init_spi_panel();
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel init (%s): %s", s_cfg.controller, esp_err_to_name(err)); return err; }

    if (s_mono) {
        s_fb = heap_caps_malloc(s_w * s_h / 8, MALLOC_CAP_DMA);
        if (!s_fb) return ESP_ERR_NO_MEM;
    } else {
        s_strip = heap_caps_malloc((size_t)s_w * s_lh * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!s_strip) return ESP_ERR_NO_MEM;
    }

    if (BOARD_BUTTON_GPIO >= 0) {
        gpio_config_t btn = {
            .pin_bit_mask = (BOARD_BUTTON_GPIO >= 0) ? (1ULL << BOARD_BUTTON_GPIO) : 0ULL,
            .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&btn);
    }

    // Clear the panel's power-on GRAM noise BEFORE the backlight comes on (init_spi_panel
    // deliberately leaves it off — see its comment) so the scrambled-pixels flash is never lit.
    blank();
    bl_set(true);

    if (s_cfg.enabled && s_cfg.show_boot_logo) {
        render_boot_splash();
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    if (xTaskCreate(refresh_task, "display", 5120, NULL, 3, NULL) != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "%s %dx%d up (%s)", s_cfg.controller, s_w, s_h, s_mono ? "mono" : "rgb565");
    return ESP_OK;
}
