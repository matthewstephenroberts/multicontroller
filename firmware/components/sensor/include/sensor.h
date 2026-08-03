// sensor.h — shared sensor model + read dispatch (hybrid: generic recipe + named drivers).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Limits ----
#define MC_MAX_SENSORS 16
#define MC_MAX_VALUES  16    // AS7341 needs 10 (F1-F8 + Clear + NIR); grouped qre1113/tssp_ir
                              // need 2 values × up to 8 mcp3208 channels (see sensor_transform.c)
#define MC_NAME_LEN    24
#define MC_TYPE_LEN    16
#define MC_XFORM_LEN   16    // transform mode id, e.g. "raw" | "imu_orient" | "col_hue"
#define MC_MAX_CALIB   12    // per-sensor calibration scalars (AS7341 white ref = 10)
#define MC_COL_CH      10    // colour reference vector length (AS7341 = 10; TCS uses 3)
#define MC_MAX_COLOURS 16    // learnable colour palette entries per colour sensor
#define MC_COL_NAME_LEN 16

// One learnable colour: a captured reference vector + the id it reports. out_id 0..10 are the
// official SPIKE colours (the hub's color() shows them); a custom out_id (>10 or <0) is still
// emitted but the hub shows no colour for it. learned=false means "use the built-in default ref".
typedef struct {
    char  name[MC_COL_NAME_LEN];
    int   out_id;
    bool  learned;
    float ref[MC_COL_CH];
} colour_ref_t;

typedef enum {
    BUS_I2C = 0,
    BUS_SPI,
    BUS_UART,
} bus_type_t;

// A generic register-read recipe: value[i] = raw * scale + offset.
typedef struct {
    int   reg;                              // start register / command byte
    int   length;                           // bytes to read
    bool  big_endian;                       // multi-byte raw decode order
    bool  is_signed;                        // sign-extend the raw value
    float scale;                            // raw * scale ...
    float offset;                           // ... + offset
    int   value_count;                      // number of decoded values
    char  value_names[MC_MAX_VALUES][MC_NAME_LEN];
} recipe_t;

// One configured sensor. Only the addressing fields relevant to `bus` are used.
typedef struct {
    int        id;
    char       name[MC_NAME_LEN];
    char       type[MC_TYPE_LEN];           // "generic" or a named driver, e.g. "bme280"
    bus_type_t bus;

    uint8_t    addr;                        // i2c 7-bit address
    uint8_t    mux_addr;                    // i2c TCA9548A address (0 = direct)
    int8_t     mux_channel;                 // i2c mux channel 0..7 (-1 = direct)
    int8_t     cs_index;                    // spi CS index 0..4
    int        port;                        // uart port, or (legacy) MCP3208 ADC channel 0..7
                                             // for a single-channel "mcp3208"/"qre1113"/
                                             // "tssp_ir" sensor — superseded by channel_mask
                                             // below for "qre1113"/"tssp_ir" when it's nonzero.

    // "qre1113"/"tssp_ir" only: bit i = MCP3208 channel i is part of this sensor's group, so one
    // line-sensor-bar or IR-receiver-ring reads as a single sensor (one poll, one BLE reading
    // event) instead of one sensor per channel — cheaper to poll/stream and groups naturally for
    // display/LEGO-field selection (both already work per described value, and a group just
    // describes N values instead of 1/2). 0 = legacy single-channel mode using `port` above.
    // Calibration (white/black reference, IR idle baseline) is shared across every channel in
    // the group rather than captured per channel — matches how a physical sensor bar is
    // typically calibrated (sweep the whole bar over white, then black, in one motion).
    uint8_t    channel_mask;

    recipe_t   recipe;

    // Derived-value transform applied after the raw read (see sensor_transform.h). "raw" or
    // "" = passthrough. calib holds type-specific calibration scalars captured on the device
    // and persisted to NVS (colour white reference, IMU gyro bias, distance zero offset, …).
    char       transform[MC_XFORM_LEN];
    // Per-sensor calibration (white reference for colour sensors, gyro bias for IMU, zero
    // offset for distance). Captured via the calibrate command; editable from the web UI for
    // manual fine-tuning (e.g. lowering a colour sensor's white refs brightens its output).
    double     calib[MC_MAX_CALIB];
    int        calib_count;

    // Learnable colour palette (colour sensors only): user-taught references that override /
    // extend the built-in classifier. Empty (colour_count 0) ⇒ built-in defaults only.
    colour_ref_t colours[MC_MAX_COLOURS];
    int          colour_count;

    // Colour reading filtering (colour sensors only; 0 = off, matching prior behaviour).
    // colour_smooth: EMA factor 0..0.95 applied to the raw channels before classification —
    // reduces sensor noise so a single bad sample doesn't skew the reading.
    // colour_debounce: number of consecutive matching classifications required before the
    // reported colour id changes — stops a borderline reading flickering between two ids.
    float      colour_smooth;
    int        colour_debounce;

    // m5_8angle knob reading filter (0 = off, matching prior behaviour). Same EMA scheme as
    // colour_smooth above but scoped to this driver's 8 potentiometer channels — a bare
    // potentiometer wiper is electrically noisy (a few counts of jitter at rest is normal), and
    // unlike a colour sensor's own noise (masked by classification/debounce downstream), a knob's
    // raw ADC count IS the reported value, so that jitter shows up directly on the dashboard/LEGO
    // field with nothing else to absorb it.
    float      knob_smooth;

    // m5_8angle: invert each knob's reported value (raw ADC_MAX - v) so turning the knob toward
    // the physical unit's printed "min"/"max" label end reports 0/max as labelled — the pot's
    // internal wiring reports it backwards from that (see drv_m5_8angle.c). Default true to match
    // the labels on the physical device out of the box.
    bool       knob_invert;

    // Distance sensors (vl53l1x/tof10120/tofi2c): configurable measuring range. dist_mode
    // selects the ranging profile on sensors that support it (0=short/1=long — long sees
    // further but needs a longer integration time, hence a higher minimum poll_ms; enforced
    // in config_store's parse_sensor). dist_min_mm/dist_max_mm are this setup's expected
    // working range; 0/0 = use the mode's native range. The reading is clamped to this range
    // and the web UI's LEGO field editor reads it back to auto-derive scale/offset, so a
    // distance value scales into the emitted LEGO field automatically.
    uint8_t    dist_mode;

    // LED brightness 0-100% (0 = off). Only used by drivers whose device has onboard LEDs:
    // as7341's illumination LED (mapped to a conservative drive-current range in the driver,
    // not the chip's full LED-cooking maximum), and the m5_8angle/m5_step16 units' value-
    // visualisation LEDs. Ignored elsewhere.
    uint8_t    led;
    // Auto-sleep for the visualisation LEDs above (m5_8angle/m5_step16 only): blank the LEDs
    // after this many seconds with no knob/dial movement, wake instantly on movement. -1 =
    // never sleep (always lit while `led` > 0) — same semantics as the display's own
    // sleep_after_s. Ignored by as7341 (its LED illuminates the colour sample being measured;
    // sleeping it would change every reading).
    int        led_sleep_s;
    float      dist_min_mm;
    float      dist_max_mm;

    uint32_t   poll_ms;
    bool       enabled;

    // When true, sensor_read() fabricates plausible random values for this sensor instead of
    // touching the bus at all (no mux select, no I2C/SPI/UART transaction). Lets the dashboard,
    // display, and LEGO emitter be exercised on the same polling schedule as real hardware when
    // a given sensor isn't physically wired up yet.
    bool       simulate;

    bool       show;                        // show this sensor on the display
    int        page;                        // display page/group (paged mode)
    unsigned   value_mask;                   // bit i = monitor value i (dashboard + display)
} sensor_cfg_t;

// ---- Display device config ----
#define MC_CTRL_LEN 12
#define MC_MODE_LEN 10

typedef struct {
    bool       enabled;
    char       controller[MC_CTRL_LEN];     // "st7789" | "ili9341" | "gc9107" (SPI) | "ssd1306" (I2C)
    bus_type_t bus;                         // BUS_SPI | BUS_I2C
    int        cs;                          // SPI CS GPIO (-1 = none)
    int        dc;                          // SPI data/command GPIO
    int        rst;                         // reset GPIO (-1 = none)
    int        bl;                          // backlight GPIO (-1 = none)
    uint8_t    addr;                        // I2C address (e.g. SSD1306 0x3C)
    int        width;
    int        height;
    int        x_gap;
    int        y_gap;
    bool       mirror_x;
    bool       mirror_y;
    bool       invert;
    char       mode[MC_MODE_LEN];           // "summary" | "paged" | "tiles"
    // "tiles" mode only: 0 = auto (fit as many as reasonably legible, sized by aspect ratio),
    // or a fixed 1/2/4/8 tiles-per-screen cap with a matching clean layout (1x1/2x1/2x2/4x2) —
    // extra shown sensors spill onto further BOOT-cycled screens instead of shrinking to fit.
    int        tiles_per_page;
    // "tiles" mode only: group tiles into per-category blocks (Distance/Colour/Motion/
    // Environment/Line-IR/Input/Other — same categories and sensor-type mapping as the web
    // dashboard's own "group by type" toggle, see display.c's category_for_type()) instead of
    // one flat grid of every shown value. Ignored (treated as one auto-fit screen) alongside a
    // fixed tiles_per_page count — slicing a flat sequential tile list by index doesn't compose
    // with grouping by category.
    bool       group_tiles;
    // Auto-sleep: turn the backlight off after this many seconds with no BOOT-button press
    // (saves power/panel life on a status display nobody's actively looking at most of the
    // time). -1 = disabled, screen always on. A press instantly wakes it.
    int        sleep_after_s;
    // Brief device-name splash shown once at boot, before the first real status screen — off
    // (false) skips straight to it for the fastest possible startup.
    bool       show_boot_logo;
    // Backlight brightness 0-100%. Real dimming only where the backlight is PWM-capable (the
    // AtomS3R's LP5562 W channel); plain-GPIO backlights treat it as on/off (>0 = on).
    uint8_t    brightness;
} display_cfg_t;

// ---- LEGO color-sensor emitter config ----
// The board can emulate a LEGO Powered Up Color Sensor and pack selected sensor values into
// its 4×uint16 (64-bit) RGBI payload (LEGO_TARGET_RGBI fields — the hub decodes via
// color.rgbi()), or drive its separate COLOR/REFLT bytes directly (LEGO_TARGET_COLOR/REFLT —
// readable via color()/reflection() without touching RGBI at all). See components/lego_emit
// and docs/lego-emit.md.
#define MC_MAX_LEGO_FIELDS 16
#define MC_LEGO_TOTAL_BITS 64

// Emulated device profile: which LEGO device the emitter pretends to be. The hub binds its
// high-level API and (for the SPIKE app) routes its blocks by the announced LPF2 type byte,
// not by the modes — so receiving writes (e.g. a 3×3 matrix) requires the matrix type.
#define LEGO_PROFILE_COLOR   0   // Color Sensor 0x3D — read-only: packs readings into RGBI
#define LEGO_PROFILE_MATRIX  1   // Technic 3×3 Color Light Matrix 0x40 — receives pixel writes

// Which slot a field writes to. The Color Sensor's other native modes (COLOR/REFLT) are
// separate DATA8 values the hub can request independently of the RGBI payload — a bit-packed
// custom field targeting the RGBI word is invisible to color()/reflection() (they only read
// mode 0/1, not mode 5's RGB I), so a field can instead target one of those directly, skipping
// the bit-packer for it. Only one field of each of these two targets is meaningful at a time
// (there's only one COLOR byte and one REFLT byte); config_store keeps the last one configured.
#define LEGO_TARGET_RGBI   0   // packs into the 64-bit RGBI word (default, legacy behaviour)
#define LEGO_TARGET_COLOR  1   // drives the emulated sensor's COLOR (mode 0) byte directly
#define LEGO_TARGET_REFLT  2   // drives the emulated sensor's REFLT (mode 1) byte directly

// COLOR-target code→colour lookup: the hub interprets the COLOR byte as a colour id and only
// accepts the ids a real sensor reports (0,1,3,4,6,7,9,10) — anything else gets coerced by hub
// firmware (observed: 5 reads back as 6). This optional per-field table translates a small
// code value (a dpad/stick-direction 0..8, a counter, ...) into a chosen supported colour id,
// so e.g. "dpad down" can deliberately read as color 9 (red) on the hub.
#define MC_LEGO_COLOUR_MAP_N 16
#define MC_LEGO_COLOUR_NONE  0xFF   // map entry: send 0xFF ("no colour" — hub color() = none)

typedef struct {
    int     sensor_id;      // source sensor id (matches sensor_cfg_t.id)
    uint8_t value_index;    // which decoded value of that sensor (0..MC_MAX_VALUES-1)
    uint8_t bits;           // field width: 1..16 (LEGO_TARGET_RGBI only; COLOR/REFLT are
                             // always a single byte, this field is unused for them)
    bool    is_signed;      // encode as two's-complement in `bits` (LEGO_TARGET_RGBI only)
    double  scale;          // raw = round((value - offset) / scale). double so clean decimals
    double  offset;         // (e.g. 0.01) round-trip exactly instead of float32 noise
    uint8_t target;         // LEGO_TARGET_* — which slot this field writes to
    bool    use_colour_map; // COLOR target only: translate the scaled code through colour_map
    uint8_t colour_map[MC_LEGO_COLOUR_MAP_N];   // code (0..15) → colour id / MC_LEGO_COLOUR_NONE
    // Optional second-stage output scaling: final = raw * output_scale + output_offset
    // Allows mapping the field's bit range (0..2^bits-1) to a custom LEGO output range
    // e.g., 0-15 field → 48-108 piano notes: output_scale=4, output_offset=48
    double  output_scale;   // 0 = disabled (use raw value as-is)
    double  output_offset;
} lego_field_t;

typedef struct {
    bool        enabled;
    int         profile;                // LEGO_PROFILE_COLOR / LEGO_PROFILE_MATRIX
    bool        debug;                  // verbose: full LPF2 handshake/TX/RX byte trace
    bool        events;                 // simple: just hub mode SELECT / combo / WRITE events
    // Colour passthrough: when >0, a sensor id in "col_full"/"as_full" mode drives the
    // emulated Color Sensor's COLOR/REFLT/RGB channels so the hub's native view + color()/
    // reflection()/rgbi() work. 0 = generic bit-packing (fields below).
    int         colour_source;
    uint8_t     sensor_type;            // LPF2 type byte (0x3D = Color Sensor)
    int         uart_port;              // ESP-IDF UART_NUM_x
    int         tx_gpio;
    int         rx_gpio;
    uint32_t    baud;                   // operational baud after handshake
    int         field_count;
    lego_field_t fields[MC_MAX_LEGO_FIELDS];
} lego_cfg_t;

// ---- Driver vtable ----
typedef struct {
    const char *type;                                                   // matched against cfg->type
    esp_err_t (*probe)(const sensor_cfg_t *cfg);                        // optional, may be NULL
    esp_err_t (*read)(const sensor_cfg_t *cfg, float *out, int max, int *out_count);
    int       (*describe)(const sensor_cfg_t *cfg, const char *names[], int max); // value names
} sensor_driver_t;

// Register all built-in drivers (generic + named). Call once at startup.
void sensor_drivers_register(void);

// Read one sensor: looks up cfg->type in the registry, falls back to the generic
// recipe engine when the type is "generic" or unknown.
esp_err_t sensor_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count);

// Value names for a sensor (for the dashboard). Returns count written.
int sensor_describe(const sensor_cfg_t *cfg, const char *names[], int max);

// The fastest poll_ms this sensor's driver can actually deliver a fresh reading at (hardware
// integration/conversion time + typical I2C/SMUX overhead for that driver, with a safety
// margin) — not an arbitrary global default. config_store clamps poll_ms to this on save; the
// web UI mirrors these same numbers (see pollMsFloor() in web/src/types.ts) so the dropdown
// only offers rates the hardware can really hit instead of a number that just runs slower than
// configured with no indication why.
uint32_t sensor_poll_floor_ms(const sensor_cfg_t *cfg);

// Native (mode-specific) full-scale distance ceiling for a sensor when dist_max_mm isn't
// explicitly configured — MUST match whatever the sensor's own driver falls back to for its
// out-of-range clamp (drv_vl53l1x.c's k_native_range table, drv_vl53l0x.c's unclamped 8190
// sentinel), so a "sensor maxed out / no target" reading lands exactly at this ceiling
// wherever it's used — e.g. the display's distance-bar fill fraction. Before this existed, the
// display used one hardcoded 2000mm fallback for every distance sensor regardless of type/mode;
// a VL53L1X in short-range mode (native ceiling 1300mm) reporting its own max-out value then
// only filled the bar to 1300/2000 = 65%, never reaching full even when genuinely maxed out.
float sensor_dist_native_max_mm(const sensor_cfg_t *cfg);

// Verbose sensor debug logging (default off): gates the throttled distance-sensor range
// diagnostics (drv_vl53l1x.c/drv_vl53l0x.c) and the BLE-HID controller's first-report hex
// dumps (hid_host.c). config_store owns persistence and the BLE "set_verbose_debug" command;
// this is the live flag drivers actually check (config_store can't be called from here — see
// the comment above s_verbose_debug in sensor.c for why).
bool sensor_get_verbose_debug(void);
void sensor_set_verbose_debug(bool on);

// Registry access used by the dispatcher and tests.
const sensor_driver_t *sensor_registry_find(const char *type);
esp_err_t sensor_registry_add(const sensor_driver_t *drv);

// Fabricate plausible values for `cfg` (no bus access) instead of calling `drv->read`. Ranges
// are picked from `drv->describe`'s channel names (falling back to the recipe's names/count),
// and each channel does a smoothed random walk within its range so repeated reads look like a
// live sensor rather than pure noise. Used by sensor_read() when cfg->simulate is set.
esp_err_t sensor_simulate_read(const sensor_driver_t *drv, const sensor_cfg_t *cfg,
                                float *out, int max, int *out_count);

#ifdef __cplusplus
}
#endif
