// sensor_transform.h — derived-value transforms applied after a raw sensor read.
//
// A sensor's cfg->transform selects a mode that maps the driver's raw values into standard
// measurements (IMU → orientation, colour → hue / 0-255 RGB / LEGO colour index, distance →
// mm/cm), optionally corrected by the per-sensor calibration blob (cfg->calib). The scheduler
// applies this so the dashboard, display, and LEGO emitter all see the derived values.
//
// Modes (dispatched on the mode string; the input layout is implied):
//   "raw" / ""    passthrough (driver values unchanged)
//   "imu_orient"  in: ax,ay,az,gx,gy,gz (qmi8658) → roll,pitch,yaw (deg, Madgwick fusion)
//   "imu_tilt"    in: ax,ay,az,…              → pitch,roll (deg, accel only, no drift)
//   "col_rgb255"  in: clear,red,green,blue    → r,g,b (0-255, white-balanced)
//   "col_hue"     in: clear,red,green,blue    → hue(0-359),sat(0-100),val(0-100)
//   "col_lego"    in: clear,red,green,blue    → colour (LEGO colour id 0-15)
//   "dist_mm"     in: distance(mm)            → dist (mm, zero-offset corrected)
//   "dist_cm"     in: distance(mm)            → dist (cm)
//   "line_reflect" in: counts (qre1113)       → reflect (0.0 white .. 1.0 black, 2-point calib),
//                                                detected (0/1, reflect > 0.5); repeats per
//                                                channel when cfg->channel_mask groups several
//                                                MCP3208 channels into one sensor
//   "ir_ball"     in: min counts (tssp_ir)    → strength (0-1), detected (0/1); repeats per
//                                                channel when cfg->channel_mask groups several
//                                                MCP3208 channels into one sensor
#pragma once

#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// Map raw values to derived values per cfg->transform. Returns the derived count (≤ out_max).
// For "raw"/unknown modes, copies the input through unchanged.
int sensor_transform_apply(const sensor_cfg_t *cfg, const float *in, int in_n,
                           float *out, int out_max);

// Derived value names for cfg->transform. Returns count written, or 0 for "raw" (the caller
// then falls back to the driver's own names).
int sensor_transform_names(const sensor_cfg_t *cfg, const char *names[], int max);

// Compute a calibration blob from current raw values for `mode` (board at rest / white tile
// in front, as appropriate). `calib`/`calib_n` is the sensor's *current* calibration (used as
// the starting point so a mode that captures its points one at a time, like "line_reflect"'s
// white/black, doesn't lose the other point) — most modes ignore it and overwrite everything.
// `point` selects which point a multi-point mode captures ("white"/"black" for "line_reflect");
// NULL/"" means "the only point" for single-point modes. Writes up to `max` scalars into
// `calib` in place, returns the new count, or -1 if the mode has no calibration.
int sensor_transform_calibrate(const char *mode, const char *point,
                               const double *calib_in, int calib_n,
                               const float *raw, int raw_n,
                               double *calib, int max);

// Capture the current raw reading as a colour reference vector for the learnable palette
// (white-balanced RGB for TCS34725, post-whitecal spectrum for AS7341). Writes into ref_out
// (length ≥ MC_COL_CH) and returns the channel count (3 or 10), or 0 if not a colour sensor.
int sensor_transform_capture_colour(const sensor_cfg_t *cfg, const float *raw, int raw_n,
                                    float *ref_out);

// Number of built-in demo/reference colours shared by the TCS34725 and AS7341 classifiers.
#define SENSOR_TRANSFORM_NUM_DEMO_COLOURS 12

// Fabricate a plausible *raw* reading for demo colour `idx` (0..NUM_DEMO_COLOURS-1) in the
// given sensor's raw channel layout: n_ch 4 = TCS34725 (clear,red,green,blue), 10 = AS7341
// (F1-F8,Clear,NIR). Used by sensor_simulate_read() so a simulated colour sensor actually
// classifies to a real, changing colour — independent per-channel noise essentially never
// resembles a real spectrum, so it either matches nothing (AS7341's tight chromaticity
// threshold) or produces channel ratios a real sensor could never see (e.g. a colour channel
// reading brighter than the broadband "clear" channel). Returns false for an unsupported n_ch.
bool sensor_transform_demo_colour(int idx, int n_ch, float *out);

// RGB565 swatch colour for a reported LEGO colour id (0..NUM_DEMO_COLOURS-1), using the same
// reference palette the classifier itself matches against — so the swatch shown for an id (e.g.
// on the onboard display) is the same colour that id's name/hex implies elsewhere in the UI.
// Returns 0 (black) for an out-of-range id, including -1 ("no colour").
uint16_t sensor_transform_colour_rgb565(int id);

// Standard name for a reported LEGO colour id (0..NUM_DEMO_COLOURS-1) — "black", "violet", …
// (mirrors SPIKE_COLOURS in web/src/types.ts). Returns "none" for id < 0 ("no colour") or an
// out-of-range id. Doesn't know about a sensor's taught/custom colours — check cfg->colours[]
// for a learned entry matching the id first if you want the user's own name to take priority.
const char *sensor_transform_colour_name(int id);

// RGB565 swatch for a *taught* colour reference (a sensor's colour_ref_t.ref) — same treatment
// as sensor_transform_colour_rgb565() but for a captured reference instead of the built-in
// palette. `type` selects how to read `ref`: "as7341" combines the 10-band spectrum into an
// approximate RGB the same way as_full does (F7+F8/F4+F5/F2+F3, own-max normalised — a hue
// indicator, not a measurement); anything else (TCS34725) reads ref[0..2] directly as RGB.
// Mirrors swatchFromRef() in web/src/types.ts.
uint16_t sensor_transform_ref_rgb565(const char *type, const float *ref);

// Gamma-encode a single 0-255 *linear* channel (proportional to reflected light intensity, e.g.
// a raw/white-balanced sensor count) into the perceptual domain a display expects. Only for
// turning a physical reading into a swatch a human looks at — never apply this before
// classification/distance matching (colour_match/as_dist_sq need the original linear values),
// and never to values emitted to the LEGO hub (a real sensor's wire values are linear too, so
// gamma-encoding them would misrepresent what's being emulated). Mirrors gammaEncode() in
// web/src/types.ts.
uint8_t sensor_transform_gamma_u8(uint8_t linear);

#ifdef __cplusplus
}
#endif
