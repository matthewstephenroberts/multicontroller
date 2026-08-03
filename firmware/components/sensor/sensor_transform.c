// sensor_transform.c — derived-value transforms + per-sensor calibration. See header.
#include "sensor_transform.h"
#include <string.h>
#include <math.h>
#include "esp_timer.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG (180.0f / (float)M_PI)
#define RAD ((float)M_PI / 180.0f)

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// LEGO Powered Up colour ids (the classifier's internal palette). Exactly 12 categories —
// aligned 1:1 with TCS_DEF below so both sensor types classify into the identical id set.
enum { LC_BLACK = 0, LC_MAGENTA = 1, LC_PURPLE = 2, LC_BLUE = 3, LC_AZURE = 4, LC_TEAL = 5,
       LC_GREEN = 6, LC_YELLOW = 7, LC_ORANGE = 8, LC_RED = 9, LC_WHITE = 10, LC_SILVER = 11,
       LC_NONE = 15 };

// Map the internal palette → SPIKE-family colour ids: the 8 the SPIKE app's color() block
// names (Black 0, Violet 1, Blue 3, Light Blue 4, Green 6, Yellow 7, Red 9, White 10), plus
// the classic LPF2 Color & Distance Sensor's extra ids that fill the hue gaps between them —
// Purple 2, Cyan/Teal 5, Orange 8, Silver 11. The extra ids are still emitted/matchable, but
// the SPIKE app's native color() block may not print a name for them (treat like a custom id).
static int spike_color_id(int c)
{
    switch (c) {
        case LC_BLACK:   return 0;
        case LC_MAGENTA: return 1;   // violet
        case LC_PURPLE:  return 2;
        case LC_BLUE:    return 3;
        case LC_AZURE:   return 4;   // light blue
        case LC_TEAL:    return 5;   // cyan
        case LC_GREEN:   return 6;
        case LC_YELLOW:  return 7;
        case LC_ORANGE:  return 8;
        case LC_RED:     return 9;
        case LC_WHITE:   return 10;
        case LC_SILVER:  return 11;
        default:         return -1;  // none / unknown
    }
}

// ── AS7341 spectral classifier (10 channels: F1-F8, Clear, NIR) ─────────────
// Reference spectra + matcher ported from the source project (post-whitecal space,
// max of F1..F8 ≈ 1000). The lowest weighted chromaticity distance wins. Exactly the same
// 12 categories as TCS_DEF (below) — LIME (redundant with GREEN) and PINK (redundant with
// MAGENTA/violet) were dropped so both sensor types share one classification palette.
#define AS_CH 10
#define NUM_REF_COLOURS 12
// Raw full scale: the ADC ceiling of 10000 counts ((ATIME+1)*(ASTEP+1)) times the driver's
// ×4 count scaling (drv_as7341.c runs AGAIN=64x and rescales to the 256x-equivalent scale) —
// a channel at/above 99% of this clipped during capture.
#define AS_RAW_FULL 40000.0f
#define AS_RAW_CLIP (AS_RAW_FULL * 0.99f)
static const float AS_REFS[NUM_REF_COLOURS][AS_CH] = {
    {897,939,947,893,926,931,999,782,2970,532},     // BLACK
    {71,339,514,590,612,691,871,665,1000,194},      // WHITE
    {927,996,960,987,976,1000,975,947,2962,902},    // SILVER
    {400,172,139,162,167,545,1000,951,1830,772},    // RED
    {15,22,37,140,330,440,520,480,1000,325},        // ORANGE
    {1000,928,965,946,993,969,964,958,2263,1220},   // YELLOW
    {350,182,487,1000,525,223,201,236,1751,456},    // GREEN
    {23,100,340,420,310,90,30,22,1000,300},         // TEAL
    {585,885,1000,903,589,361,354,312,1973,427},    // AZURE
    {471,996,1000,650,316,294,267,323,2335,512},    // BLUE
    {440,308,196,143,138,375,918,1000,1331,540},    // PURPLE
    {38,130,90,50,45,90,220,180,1000,420},          // MAGENTA (violet)
};
static const int AS_REF_STATE[NUM_REF_COLOURS] = {
    LC_BLACK, LC_WHITE, LC_SILVER, LC_RED, LC_ORANGE, LC_YELLOW,
    LC_GREEN, LC_TEAL, LC_AZURE, LC_BLUE, LC_PURPLE, LC_MAGENTA,
};

// White-reference calibration: divide each channel by the captured white response, then
// rescale so max of ch[0..7] is 1000 (device-independent spectrum). calib = 10 white refs.
static void as_whitecal(const float *in, const double *calib, int calib_n, float *out)
{
    if (calib_n < AS_CH) { for (int i = 0; i < AS_CH; i++) out[i] = in[i]; return; }
    float gained[AS_CH], gmax = 0;
    for (int i = 0; i < AS_CH; i++) {
        float w = (float)calib[i];
        gained[i] = (w > 0) ? in[i] / w : 0;
        if (i < 8 && gained[i] > gmax) gmax = gained[i];
    }
    if (gmax < 1e-6f) { for (int i = 0; i < AS_CH; i++) out[i] = in[i]; return; }
    float s = 1000.0f / gmax;
    for (int i = 0; i < AS_CH; i++) out[i] = clampf(gained[i] * s, 0, 65535);
}

// Chromaticity distance: normalise by max(ch[0..7]); Clear dropped, NIR weak, cyan/green/
// yellow bands (3..5) up-weighted.
static float as_dist_sq(const float *sample, const float *ref)
{
    float sMax = 1, rMax = 1;
    for (int i = 0; i < 8; i++) { if (sample[i] > sMax) sMax = sample[i]; if (ref[i] > rMax) rMax = ref[i]; }
    if (sMax < 50 || rMax < 50) return 1e9f;
    float d = 0;
    for (int i = 0; i < AS_CH; i++) {
        float diff = sample[i] / sMax - ref[i] / rMax;
        float w = (i == 8) ? 0.0f : (i == 9) ? 0.1f : (i >= 3 && i <= 5) ? 1.6f : 1.0f;
        d += w * diff * diff;
    }
    return d;
}

static int has_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

// ── Colour helpers (TCS34725: clear, red, green, blue raw counts) ───────────

// Normalise raw r/g/b to 0-255. White reference (calib clear,red,green,blue) makes a white
// target read ~255; without calibration we divide by the clear channel.
static void rgb_to_255(const sensor_cfg_t *cfg, const float *in, float *r, float *g, float *b)
{
    float clear = in[0], red = in[1], grn = in[2], blu = in[3];
    if (cfg->calib_count >= 4 && cfg->calib[1] > 0 && cfg->calib[2] > 0 && cfg->calib[3] > 0) {
        *r = red * 255.0f / (float)cfg->calib[1];
        *g = grn * 255.0f / (float)cfg->calib[2];
        *b = blu * 255.0f / (float)cfg->calib[3];
    } else {
        float base = clear > 1 ? clear : 1;
        *r = red * 255.0f / base;
        *g = grn * 255.0f / base;
        *b = blu * 255.0f / base;
    }
    *r = clampf(*r, 0, 255);
    *g = clampf(*g, 0, 255);
    *b = clampf(*b, 0, 255);
}

// Colour-correction matrix + bias, applied to white-balanced RGB (0-255) before classification
// or any output — corrects the TCS34725's photodiode spectral crosstalk (e.g. its "blue"
// channel picking up some green/yellow energy, which reads a saturated yellow as brownish).
// A single white-point gain (rgb_to_255 above) can't fix this: it only corrects each channel's
// overall sensitivity, not one channel bleeding into another for a specific hue.
//
// Fitted (weighted least-squares, one sample per colour) against the 12 TCS_DEF reference
// colours, weighting LEGO's 8 officially-published references higher than the 4 approximate
// ids (purple/cyan/orange/silver — no published reference to fit against). Brings nearest-
// neighbour misclassification on the fitting set from 3/12 (uncalibrated) to 2/12 — the 2
// remaining are purple->violet and cyan->blue, both against an approximate (guessed) target,
// not a real reference; all 8 published colours (including red, which misclassified as violet
// before this) now match correctly.
//
// This was fit from ONE specific sensor unit under ONE specific lighting setup (see
// docs/colour-calibration.md's capture worksheet) — it is not guaranteed to generalise to a
// different sensor or lighting. Re-fit (see the worksheet) if accuracy still looks off for your
// setup, or use Teach for colours you need to be precise regardless.
static const float TCS_CCM[3][3] = {
    {  2.00588f, -1.63575f,  0.72529f },
    { -0.69537f,  2.17888f, -0.41740f },
    { -0.11402f, -0.72319f,  1.86334f },
};
static const float TCS_CCM_BIAS[3] = { -28.72926f, -2.39113f, -19.55043f };

static void apply_ccm(float *r, float *g, float *b)
{
    float in[3] = { *r, *g, *b }, out[3];
    for (int i = 0; i < 3; i++)
        out[i] = TCS_CCM[i][0] * in[0] + TCS_CCM[i][1] * in[1] + TCS_CCM[i][2] * in[2] + TCS_CCM_BIAS[i];
    *r = clampf(out[0], 0, 255);
    *g = clampf(out[1], 0, 255);
    *b = clampf(out[2], 0, 255);
}

// RGB (0-255) → HSV: hue 0-359, sat 0-100, val 0-100. Ported from the source project.
static void rgb_to_hsv(float r, float g, float b, float *h, float *s, float *v)
{
    float maxc = fmaxf(r, fmaxf(g, b));
    float minc = fminf(r, fminf(g, b));
    *v = maxc * 100.0f / 255.0f;
    if (maxc <= 0) { *s = 0; *h = 0; return; }
    *s = (maxc - minc) * 100.0f / maxc;
    float hh;
    if (maxc == minc)      hh = 0;
    else if (maxc == r)  { hh = 60.0f * (g - b) / (maxc - minc); if (hh < 0) hh += 360; }
    else if (maxc == g)    hh = 120.0f + 60.0f * (b - r) / (maxc - minc);
    else                   hh = 240.0f + 60.0f * (r - g) / (maxc - minc);
    *h = fmodf(hh, 360.0f);
}

// ── Learnable colour palette (per-sensor) ───────────────────────────────────
// Default TCS34725 references in white-balanced RGB (0-255) space, each mapped to its SPIKE
// output id. Used when the sensor has no learned palette; learned entries override by out_id.
// The 8 SPIKE-named colours (black/violet/blue/azure/green/yellow/red/white) use LEGO's own
// published SPIKE Color Sensor reference values. purple/cyan/orange/silver have no published
// reference (LEGO's spec only documents 8 colours) — these 4 are still approximate.
static const float TCS_DEF[][MC_COL_CH] = {
    {  0,   0,   0},   // black
    {144,  31, 118},   // violet (LEGO: "bright reddish violet")
    {140,   0, 140},   // purple (approximate — no published reference)
    { 30,  90, 168},   // blue
    {104, 195, 226},   // azure / light blue (LEGO: "medium azur")
    {  0, 200, 200},   // cyan / teal (approximate — no published reference)
    {  0, 133,  43},   // green
    {250, 200,  10},   // yellow
    {255, 140,   0},   // orange (approximate — no published reference)
    {180,   0,   0},   // red
    {244, 244, 244},   // white
    {192, 192, 192},   // silver (approximate — no published reference)
};
static const int TCS_DEF_ID[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
#define TCS_DEF_N ((int)(sizeof(TCS_DEF_ID)/sizeof(TCS_DEF_ID[0])))
// Below this raw clear count, nothing reflective is close enough for a confident reading —
// report -1 ("no colour"), matching a real LEGO colour sensor when it sees no target. Used only
// as a fallback before a white calibration exists (see tcs_none_clear_threshold() below) — a
// fixed absolute count doesn't scale with LED brightness/distance/gain, so it was previously
// used unconditionally and could reject a genuinely taught BLACK sample as "no target": black
// reflects very little light, so its raw clear count sits naturally low too (observed: ~129
// against a calib clear of 866 — under this 200 fixed floor, misreported as "none" no matter how
// well black was taught).
#define TCS_NONE_CLEAR 200.0f
// Once calibrated, scale the "no target" floor to the white reference's own raw clear count
// instead of a fixed number, so it moves with whatever LED brightness/distance/gain this sensor
// unit and setup actually produce. 5% comfortably sits below a taught black's typical raw clear
// (roughly 10-20% of white, per the TCS_DEF black reference's own proportions) while still
// rejecting genuinely nothing-in-range (near-zero raw counts, well under even that).
#define TCS_NONE_CLEAR_FRAC 0.05f

static float tcs_none_clear_threshold(const sensor_cfg_t *cfg)
{
    if (cfg->calib_count >= 4 && cfg->calib[0] > 0)
        return (float)cfg->calib[0] * TCS_NONE_CLEAR_FRAC;
    return TCS_NONE_CLEAR;                          // no calibration yet — no scale to derive from
}

bool sensor_transform_demo_colour(int idx, int n_ch, float *out)
{
    if (idx < 0 || idx >= TCS_DEF_N) return false;
    if (n_ch == AS_CH) {                       // AS7341: AS_REFS is already raw-shaped
        memcpy(out, AS_REFS[idx], sizeof(float) * AS_CH);
        return true;
    }
    if (n_ch == 4) {                           // TCS34725: reconstruct raw counts from the
        // 0-255 white-balanced reference so clear ends up >= the colour channels (as on real
        // hardware) and red/green/blue keep the reference's proportions.
        float r = TCS_DEF[idx][0], g = TCS_DEF[idx][1], b = TCS_DEF[idx][2];
        const float scale = 8.0f, dark = 60.0f;    // 0-255 -> raw-count-ish + dark-current floor
        out[1] = r * scale + dark;
        out[2] = g * scale + dark;
        out[3] = b * scale + dark;
        out[0] = out[1] + out[2] + out[3] + dark;  // clear >= sum of filtered channels
        return true;
    }
    return false;
}

uint8_t sensor_transform_gamma_u8(uint8_t linear)
{
    return (uint8_t)roundf(powf((float)linear / 255.0f, 1.0f / 2.2f) * 255.0f);
}

uint16_t sensor_transform_colour_rgb565(int id)
{
    if (id < 0 || id >= TCS_DEF_N) return 0;
    uint8_t r = sensor_transform_gamma_u8((uint8_t)clampf(TCS_DEF[id][0], 0, 255));
    uint8_t g = sensor_transform_gamma_u8((uint8_t)clampf(TCS_DEF[id][1], 0, 255));
    uint8_t b = sensor_transform_gamma_u8((uint8_t)clampf(TCS_DEF[id][2], 0, 255));
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint16_t sensor_transform_ref_rgb565(const char *type, const float *ref)
{
    float r, g, b;
    if (!strcmp(type, "as7341")) {
        r = ref[6] + ref[7]; g = ref[3] + ref[4]; b = ref[1] + ref[2];   // F7+F8/F4+F5/F2+F3
        float mx = fmaxf(r, fmaxf(g, b)); if (mx < 1.0f) mx = 1.0f;
        r = r / mx * 255.0f; g = g / mx * 255.0f; b = b / mx * 255.0f;
    } else {
        r = ref[0]; g = ref[1]; b = ref[2];
    }
    uint8_t rc = sensor_transform_gamma_u8((uint8_t)clampf(r, 0, 255));
    uint8_t gc = sensor_transform_gamma_u8((uint8_t)clampf(g, 0, 255));
    uint8_t bc = sensor_transform_gamma_u8((uint8_t)clampf(b, 0, 255));
    return (uint16_t)(((rc >> 3) << 11) | ((gc >> 2) << 5) | (bc >> 3));
}

const char *sensor_transform_colour_name(int id)
{
    // Order matches TCS_DEF above / SPIKE_COLOURS in web/src/types.ts — keep in sync.
    static const char *names[TCS_DEF_N] = {
        "black", "violet", "purple", "blue", "light blue", "cyan",
        "green", "yellow", "orange", "red", "white", "silver",
    };
    if (id < 0) return "none";
    if (id >= TCS_DEF_N) return "unknown";
    return names[id];
}

// Distance: chromaticity (as_dist_sq) for 10-channel spectral, plain euclidean for ≤3-channel.
static float col_dist(const float *a, const float *b, int n)
{
    if (n >= AS_CH) return as_dist_sq(a, b);
    float d = 0; for (int i = 0; i < n; i++) { float diff = a[i] - b[i]; d += diff * diff; }
    return d;
}

// Nearest-match over the sensor's learned palette + built-in defaults (defaults whose out_id a
// learned entry reuses are overridden). Returns the matched entry's out_id, or -1 if the best
// distance exceeds `threshold` (= "no match"). `sample`/refs are length `n_ch`.
static int colour_match(const sensor_cfg_t *cfg, const float *sample, int n_ch,
                        const float (*def)[MC_COL_CH], const int *def_id, int n_def,
                        float threshold)
{
    float best = 1e18f; int best_id = -1;
    int learned_ids[MC_MAX_COLOURS]; int nl = 0;
    for (int i = 0; i < cfg->colour_count; i++) {
        const colour_ref_t *c = &cfg->colours[i];
        if (!c->learned) continue;                       // only captured entries match
        learned_ids[nl++] = c->out_id;
        float d = col_dist(sample, c->ref, n_ch);
        if (d < best) { best = d; best_id = c->out_id; }
    }
    for (int j = 0; j < n_def; j++) {
        bool overridden = false;
        for (int k = 0; k < nl; k++) if (learned_ids[k] == def_id[j]) { overridden = true; break; }
        if (overridden) continue;
        float d = col_dist(sample, def[j], n_ch);
        if (d < best) { best = d; best_id = def_id[j]; }
    }
    return (best > threshold) ? -1 : best_id;
}

// Black/silver/white are all "flat" (achromatic) spectra — they differ from each other almost
// entirely in reflected intensity, not shape. But whitecal normalises every capture's shape to
// peak 1000, which DISCARDS intensity: a taught black and a taught white end up with nearly
// identical stored spectra (observed on real hardware: black's post-whitecal Clear 1669 vs
// silver's 1644 — inseparable). The one intensity-preserving quantity available is the RAW
// Clear count, so sensor_transform_capture_colour() stores it in ref[8] — a slot the chromatic
// matcher explicitly weights at zero (see as_dist_sq), i.e. free for this. Match the live raw
// Clear (chRaw[8]) against taught raw Clears among black/white/silver. Returns -1 if none of
// the three has a taught reference, or if the taught refs predate the raw-Clear format (their
// ref[8] is a whitecal value ~peak-relative, not a raw count — re-Teach to upgrade).
static int as_achromatic_match(const sensor_cfg_t *cfg, const float *chRaw)
{
    static const int ACHROMATIC_IDS[3] = { LC_BLACK, LC_WHITE, LC_SILVER };
    float best = 1e18f; int best_id = -1;
    for (int i = 0; i < cfg->colour_count; i++) {
        const colour_ref_t *c = &cfg->colours[i];
        if (!c->learned) continue;
        bool is_achromatic = false;
        for (int k = 0; k < 3; k++) if (c->out_id == ACHROMATIC_IDS[k]) { is_achromatic = true; break; }
        if (!is_achromatic) continue;
        float diff = chRaw[8] - c->ref[8];
        float d = diff * diff;
        if (d < best) { best = d; best_id = c->out_id; }
    }
    return best_id;
}

// TCS34725 counterpart: unlike AS7341's chromaticity-only match, colour_match's plain-Euclidean
// distance on (r,g,b) *does* already carry brightness (these aren't normalised to a unit
// vector) — but that's not enough on its own: TCS_DEF's approximate (unpublished) silver
// {192,192,192} sits almost exactly as close to azure {104,195,226} as it does to white
// {244,244,244}, so sensor noise alone can flip a genuinely-grey/silver reading to "light blue".
// Same fix as the AS7341 side: once a sample looks achromatic, prefer the taught black/white/
// silver nearest by brightness (average of r,g,b — no separate "clear" channel exists or is
// needed for TCS, since intensity already lives in r/g/b's magnitude) over the generic
// chromatic-colour matcher. Returns -1 if none of the three has been taught.
static int tcs_achromatic_match(const sensor_cfg_t *cfg, float r, float g, float b)
{
    static const int ACHROMATIC_IDS[3] = { LC_BLACK, LC_WHITE, LC_SILVER };
    float sample_avg = (r + g + b) / 3.0f;
    float best = 1e18f; int best_id = -1;
    for (int i = 0; i < cfg->colour_count; i++) {
        const colour_ref_t *c = &cfg->colours[i];
        if (!c->learned) continue;
        bool is_achromatic = false;
        for (int k = 0; k < 3; k++) if (c->out_id == ACHROMATIC_IDS[k]) { is_achromatic = true; break; }
        if (!is_achromatic) continue;
        float ref_avg = (c->ref[0] + c->ref[1] + c->ref[2]) / 3.0f;
        float diff = sample_avg - ref_avg;
        float d = diff * diff;
        if (d < best) { best = d; best_id = c->out_id; }
    }
    return best_id;
}

// AS7341 colour id: keep the spectral pre-checks (none/black/white/silver), then palette match.
static int as_colour_id(const sensor_cfg_t *cfg, const float *ch, const float *chRaw)
{
    if (chRaw[8] < 200) return -1;                       // no target (Clear too low)
    float rawSum = 0; for (int i = 0; i < 8; i++) rawSum += chRaw[i];
    float calMin = ch[0], calMax = ch[0];
    for (int i = 1; i < 8; i++) { if (ch[i] < calMin) calMin = ch[i]; if (ch[i] > calMax) calMax = ch[i]; }
    // 1.20 flatness gate (was 1.05): real neutral targets measured post-whitecal show F-band
    // spreads up to ~1.18 (sensor noise + imperfect white reference tint), so the tighter gate
    // routed genuinely-grey samples to the chromatic matcher, where black/white/silver shapes
    // are all near-identical anyway. Chromatic colours have far larger spreads (2x-4x).
    bool achromatic = rawSum < 4000 || calMax <= calMin * 1.20f;

    if (achromatic) {
        int taught_id = as_achromatic_match(cfg, chRaw);
        if (taught_id >= 0) return taught_id;
        // No taught black/silver/white — fall back to a 3-way split by raw reflected
        // intensity (unnormalised sensor counts, so it doesn't inherit the whitecal-space
        // quirk where a dim black target and a bright silver one can end up needing similar
        // gain). Approximate defaults — Teach on a real sample for a better match.
        // Splits tuned for unclipped raw Clear (the driver's 64x gain gives Clear headroom to
        // AS_RAW_FULL=40000): a lit white card typically reads well above 12000, silver/grey
        // roughly half that, black a fraction. Approximate — Teach beats any fixed split.
        if (rawSum < 4000)    return LC_BLACK;
        if (chRaw[8] < 4000)  return LC_BLACK;
        if (chRaw[8] < 12000) return LC_SILVER;
        return LC_WHITE;
    }

    int def_id[NUM_REF_COLOURS];
    for (int c = 0; c < NUM_REF_COLOURS; c++) def_id[c] = spike_color_id(AS_REF_STATE[c]);
    // Acceptance bound 0.120 (was 0.040). The nearest reference always wins regardless of the
    // bound — it only decides when to give up and report -1 ("none") instead. 0.040 was tuned
    // for taught colours (captured through this unit's own whitecal/LED/geometry, which land
    // well inside it), but the built-in AS_REFS spectra were captured on a *different* sensor
    // unit and setup, so an untaught colour on real hardware routinely sits just outside 0.040
    // while still being unambiguously nearest one reference (observed: a clearly-red target
    // reporting "none"). 0.120 absorbs that unit-to-unit variation while a genuinely
    // out-of-palette target still reports -1, like a real LEGO sensor seeing nothing it knows.
    return colour_match(cfg, ch, AS_CH, AS_REFS, def_id, NUM_REF_COLOURS, 0.120f);
}

// ── Colour reading filtering (noise smoothing + id debounce) ────────────────
// Smoothing: an exponential moving average over the raw channels, applied before white-cal/
// classification, so a single noisy sample doesn't swing the reading. cfg->colour_smooth is
// the EMA weight given to the *old* value (0 = off/immediate, up to 0.95 = heavy smoothing).
typedef struct { int id; bool used; bool init; float buf[AS_CH]; } colour_smooth_t;
static colour_smooth_t s_csmooth[MC_MAX_SENSORS];

static const float *colour_smooth_apply(int sensor_id, float alpha, const float *raw, int n)
{
    if (alpha <= 0.0f || n > AS_CH) return raw;
    colour_smooth_t *st = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++) if (s_csmooth[i].used && s_csmooth[i].id == sensor_id) { st = &s_csmooth[i]; break; }
    if (!st) for (int i = 0; i < MC_MAX_SENSORS; i++) if (!s_csmooth[i].used) { st = &s_csmooth[i]; st->used = true; st->id = sensor_id; st->init = false; break; }
    if (!st) return raw;                                  // table full (shouldn't happen)
    if (!st->init) { for (int i = 0; i < n; i++) st->buf[i] = raw[i]; st->init = true; }
    else           { for (int i = 0; i < n; i++) st->buf[i] = st->buf[i] * alpha + raw[i] * (1.0f - alpha); }
    return st->buf;
}

// Debounce: only change the reported colour id after `n` consecutive classifications agree,
// so a borderline reading that flickers between two ids reports the last *stable* one instead.
// n <= 0 disables debouncing (returns raw_id unchanged, matching prior behaviour).
typedef struct { int id; bool used; int stable_id, candidate_id, candidate_count; } colour_debounce_t;
static colour_debounce_t s_cdebounce[MC_MAX_SENSORS];

static int colour_debounce_apply(int sensor_id, int raw_id, int n)
{
    if (n <= 0) return raw_id;
    colour_debounce_t *st = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++) if (s_cdebounce[i].used && s_cdebounce[i].id == sensor_id) { st = &s_cdebounce[i]; break; }
    if (!st) for (int i = 0; i < MC_MAX_SENSORS; i++) if (!s_cdebounce[i].used) {
        st = &s_cdebounce[i]; st->used = true; st->id = sensor_id;
        st->stable_id = raw_id; st->candidate_id = raw_id; st->candidate_count = 0;
        break;
    }
    if (!st) return raw_id;                                // table full (shouldn't happen)
    if (raw_id == st->stable_id) { st->candidate_count = 0; return st->stable_id; }
    if (raw_id == st->candidate_id) st->candidate_count++;
    else { st->candidate_id = raw_id; st->candidate_count = 1; }
    if (st->candidate_count >= n) { st->stable_id = st->candidate_id; st->candidate_count = 0; }
    return st->stable_id;
}

// Deadband hold for "knob_digital" (below) — per sensor, per channel (8 knobs max). Reports the
// raw value essentially unchanged while a knob is actually being turned (full analogue
// resolution, not bucketed into coarse steps), but HOLDS the last-reported value rock-steady
// once movement stops, only releasing the hold when the raw count drifts more than `deadband`
// away from what's currently being held. A knob sitting still reads as a perfectly flat line;
// turning it tracks the real position immediately, no lag, no coarsening. An earlier version of
// this quantized into a small fixed number of big steps (0-20) — that flattened jitter too, but
// also flattened genuine fine movement into the same handful of buckets, which read as "clean"
// but not actually close to the potentiometer's real range. This keeps the range; it just
// refuses to move on noise alone.
typedef struct { int id; bool used; bool init[8]; float held[8]; } knob_digital_t;
static knob_digital_t s_kdigital[MC_MAX_SENSORS];

static float knob_digital_hold(int sensor_id, int ch, float raw, float deadband)
{
    knob_digital_t *st = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++) if (s_kdigital[i].used && s_kdigital[i].id == sensor_id) { st = &s_kdigital[i]; break; }
    if (!st) for (int i = 0; i < MC_MAX_SENSORS; i++) if (!s_kdigital[i].used) { st = &s_kdigital[i]; st->used = true; st->id = sensor_id; break; }
    if (!st) return raw;                                    // table full (shouldn't happen)
    if (!st->init[ch]) { st->held[ch] = raw; st->init[ch] = true; return raw; }
    if (fabsf(raw - st->held[ch]) > deadband) st->held[ch] = raw;
    return st->held[ch];
}

// ── IMU Madgwick fusion (accel + gyro, no magnetometer) ─────────────────────
typedef struct { int id; bool used; float q0, q1, q2, q3; int64_t last_us; } imu_state_t;
static imu_state_t s_imu[MC_MAX_SENSORS];

static imu_state_t *imu_state(int id)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_imu[i].used && s_imu[i].id == id) return &s_imu[i];
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (!s_imu[i].used) {
            s_imu[i] = (imu_state_t){ .id = id, .used = true, .q0 = 1, .last_us = 0 };
            return &s_imu[i];
        }
    return &s_imu[0];
}

// Standard Madgwick IMU-only update. gyro in rad/s, accel in any unit (normalised here).
static void madgwick(imu_state_t *st, float gx, float gy, float gz,
                     float ax, float ay, float az, float dt)
{
    const float beta = 0.1f;
    float q0 = st->q0, q1 = st->q1, q2 = st->q2, q3 = st->q3;

    float qDot1 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
    float qDot2 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy);
    float qDot3 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx);
    float qDot4 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx);

    float n = sqrtf(ax * ax + ay * ay + az * az);
    if (n > 1e-6f) {
        ax /= n; ay /= n; az /= n;
        float _2q0 = 2*q0, _2q1 = 2*q1, _2q2 = 2*q2, _2q3 = 2*q3;
        float _4q0 = 4*q0, _4q1 = 4*q1, _4q2 = 4*q2;
        float _8q1 = 8*q1, _8q2 = 8*q2;
        float q0q0 = q0*q0, q1q1 = q1*q1, q2q2 = q2*q2, q3q3 = q3*q3;
        float s0 = _4q0*q2q2 + _2q2*ax + _4q0*q1q1 - _2q1*ay;
        float s1 = _4q1*q3q3 - _2q3*ax + 4*q0q0*q1 - _2q0*ay - _4q1 + _8q1*q1q1 + _8q1*q2q2 + _4q1*az;
        float s2 = 4*q0q0*q2 + _2q0*ax + _4q2*q3q3 - _2q3*ay - _4q2 + _8q2*q1q1 + _8q2*q2q2 + _4q2*az;
        float s3 = 4*q1q1*q3 - _2q1*ax + 4*q2q2*q3 - _2q2*ay;
        float sn = sqrtf(s0*s0 + s1*s1 + s2*s2 + s3*s3);
        if (sn > 1e-6f) {
            s0 /= sn; s1 /= sn; s2 /= sn; s3 /= sn;
            qDot1 -= beta * s0; qDot2 -= beta * s1; qDot3 -= beta * s2; qDot4 -= beta * s3;
        }
    }

    q0 += qDot1 * dt; q1 += qDot2 * dt; q2 += qDot3 * dt; q3 += qDot4 * dt;
    float qn = sqrtf(q0*q0 + q1*q1 + q2*q2 + q3*q3);
    if (qn > 1e-6f) { q0 /= qn; q1 /= qn; q2 /= qn; q3 /= qn; }
    st->q0 = q0; st->q1 = q1; st->q2 = q2; st->q3 = q3;
}

// ── IMU 9-axis fusion (accel + gyro + magnetometer, "MARG") ─────────────────
// A Mahony-style complementary filter: same quaternion-integration skeleton as the accel-only
// Madgwick filter above, but correction comes from a cross-product error between the
// estimated and measured gravity/magnetic-field directions rather than Madgwick's gradient
// descent — simpler to get numerically right for the 9-axis case, and this is what actually
// fixes yaw: accel+gyro alone has no reference for heading, so yaw is pure gyro integration and
// drifts; the magnetometer gives an absolute heading (like a compass) the filter can correct
// against, the same way the accelerometer already corrects pitch/roll drift against gravity.
static void madgwick_marg(imu_state_t *st, float gx, float gy, float gz,
                          float ax, float ay, float az,
                          float mx, float my, float mz, float dt)
{
    const float beta = 0.1f;
    float q0 = st->q0, q1 = st->q1, q2 = st->q2, q3 = st->q3;

    float qDot1 = 0.5f * (-q1*gx - q2*gy - q3*gz);
    float qDot2 = 0.5f * ( q0*gx + q2*gz - q3*gy);
    float qDot3 = 0.5f * ( q0*gy - q1*gz + q3*gx);
    float qDot4 = 0.5f * ( q0*gz + q1*gy - q2*gx);

    float an = sqrtf(ax*ax + ay*ay + az*az);
    float mn = sqrtf(mx*mx + my*my + mz*mz);
    if (an > 1e-6f && mn > 1e-6f) {
        ax /= an; ay /= an; az /= an;
        mx /= mn; my /= mn; mz /= mn;

        float q0q0 = q0*q0, q0q1 = q0*q1, q0q2 = q0*q2, q0q3 = q0*q3;
        float q1q1 = q1*q1, q1q2 = q1*q2, q1q3 = q1*q3;
        float q2q2 = q2*q2, q2q3 = q2*q3, q3q3 = q3*q3;

        // Reference direction of Earth's magnetic field, rotated into the earth frame by the
        // current attitude estimate, then flattened to a horizontal (2D) field + vertical
        // component — this is what lets heading (yaw) be corrected without also being upset by
        // the sensor's tilt.
        float hx = 2*mx*(0.5f - q2q2 - q3q3) + 2*my*(q1q2 - q0q3)       + 2*mz*(q1q3 + q0q2);
        float hy = 2*mx*(q1q2 + q0q3)       + 2*my*(0.5f - q1q1 - q3q3) + 2*mz*(q2q3 - q0q1);
        float hz = 2*mx*(q1q3 - q0q2)       + 2*my*(q2q3 + q0q1)       + 2*mz*(0.5f - q1q1 - q2q2);
        float bx = sqrtf(hx*hx + hy*hy);
        float bz = hz;

        float halfvx = q1q3 - q0q2;
        float halfvy = q0q1 + q2q3;
        float halfvz = q0q0 - 0.5f + q3q3;
        float halfwx = bx*(0.5f - q2q2 - q3q3) + bz*(q1q3 - q0q2);
        float halfwy = bx*(q1q2 - q0q3)       + bz*(q0q1 + q2q3);
        float halfwz = bx*(q0q2 + q1q3)       + bz*(0.5f - q1q1 - q2q2);

        // Error is the cross product between the estimated and measured field directions
        // (gravity from accel, magnetic field from mag) — this is a Mahony-style error term used
        // as the gradient-descent step here for numerical simplicity/stability, feeding the same
        // quaternion integration as the accel-only filter above.
        float ex = (ay*halfvz - az*halfvy) + (my*halfwz - mz*halfwy);
        float ey = (az*halfvx - ax*halfvz) + (mz*halfwx - mx*halfwz);
        float ez = (ax*halfvy - ay*halfvx) + (mx*halfwy - my*halfwx);

        qDot1 += -beta * (q1*ex + q2*ey + q3*ez) * 0.5f;
        qDot2 +=  beta * (q0*ex + q2*ez - q3*ey) * 0.5f;
        qDot3 +=  beta * (q0*ey - q1*ez + q3*ex) * 0.5f;
        qDot4 +=  beta * (q0*ez + q1*ey - q2*ex) * 0.5f;
    }

    q0 += qDot1 * dt; q1 += qDot2 * dt; q2 += qDot3 * dt; q3 += qDot4 * dt;
    float qn = sqrtf(q0*q0 + q1*q1 + q2*q2 + q3*q3);
    if (qn > 1e-6f) { q0 /= qn; q1 /= qn; q2 /= qn; q3 /= qn; }
    st->q0 = q0; st->q1 = q1; st->q2 = q2; st->q3 = q3;
}

// ── Public API ──────────────────────────────────────────────────────────────
int sensor_transform_apply(const sensor_cfg_t *cfg, const float *in, int in_n,
                           float *out, int out_max)
{
    const char *m = cfg->transform;
    if (!m[0] || !strcmp(m, "raw")) {                      // passthrough
        int n = in_n < out_max ? in_n : out_max;
        memcpy(out, in, sizeof(float) * n);
        return n;
    }

    if (has_prefix(m, "imu_")) {
        if (!strcmp(m, "imu_orient9")) {
            // Needs ax,ay,az,gx,gy,gz,mx,my,mz — the combined bmi270 driver's first 9 values
            // (see drv_bmi270_bmm150.c); anything shorter (e.g. a plain 6-axis qmi8658) can't do 9-axis
            // fusion at all, so fall back to passthrough rather than silently reading garbage.
            if (in_n < 9) { int n = in_n < out_max ? in_n : out_max; memcpy(out, in, sizeof(float)*n); return n; }
            float ax = in[0], ay = in[1], az = in[2];
            float gx = in[3], gy = in[4], gz = in[5];
            float mx = in[6], my = in[7], mz = in[8];
            if (cfg->calib_count >= 3) { gx -= cfg->calib[0]; gy -= cfg->calib[1]; gz -= cfg->calib[2]; }
            imu_state_t *st = imu_state(cfg->id);
            int64_t now = esp_timer_get_time();
            float dt = (st->last_us && now > st->last_us) ? (now - st->last_us) / 1e6f : (cfg->poll_ms / 1000.0f);
            st->last_us = now;
            if (dt <= 0 || dt > 1.0f) dt = 0.02f;
            // A dropped/glitched mag sample (drv_bmi270_bmm150.c zeroes mx/my/mz rather than failing the
            // whole reading) falls back to accel+gyro-only fusion for that one tick instead of
            // feeding the filter a bogus all-zero field direction.
            if (mx*mx + my*my + mz*mz > 1e-6f)
                madgwick_marg(st, gx*RAD, gy*RAD, gz*RAD, ax, ay, az, mx, my, mz, dt);
            else
                madgwick(st, gx*RAD, gy*RAD, gz*RAD, ax, ay, az, dt);
            float q0 = st->q0, q1 = st->q1, q2 = st->q2, q3 = st->q3;
            if (out_max < 3) return 0;
            out[0] = atan2f(2*(q0*q1 + q2*q3), 1 - 2*(q1*q1 + q2*q2)) * DEG;   // roll
            out[1] = asinf(fmaxf(-1.0f, fminf(1.0f, 2*(q0*q2 - q3*q1)))) * DEG; // pitch
            out[2] = atan2f(2*(q0*q3 + q1*q2), 1 - 2*(q2*q2 + q3*q3)) * DEG;   // yaw
            return 3;
        }
        if (in_n < 6) { int n = in_n < out_max ? in_n : out_max; memcpy(out, in, sizeof(float)*n); return n; }
        float ax = in[0], ay = in[1], az = in[2];
        float gx = in[3], gy = in[4], gz = in[5];
        if (cfg->calib_count >= 3) { gx -= cfg->calib[0]; gy -= cfg->calib[1]; gz -= cfg->calib[2]; }
        if (!strcmp(m, "imu_tilt")) {
            if (out_max < 2) return 0;
            out[0] = atan2f(-ax, sqrtf(ay*ay + az*az)) * DEG;   // pitch
            out[1] = atan2f(ay, az) * DEG;                      // roll
            return 2;
        }
        // imu_orient — Madgwick fusion (accel + gyro only, yaw will drift — see imu_orient9)
        imu_state_t *st = imu_state(cfg->id);
        int64_t now = esp_timer_get_time();
        float dt = (st->last_us && now > st->last_us) ? (now - st->last_us) / 1e6f : (cfg->poll_ms / 1000.0f);
        st->last_us = now;
        if (dt <= 0 || dt > 1.0f) dt = 0.02f;
        madgwick(st, gx*RAD, gy*RAD, gz*RAD, ax, ay, az, dt);
        float q0 = st->q0, q1 = st->q1, q2 = st->q2, q3 = st->q3;
        if (out_max < 3) return 0;
        out[0] = atan2f(2*(q0*q1 + q2*q3), 1 - 2*(q1*q1 + q2*q2)) * DEG;   // roll
        out[1] = asinf(fmaxf(-1.0f, fminf(1.0f, 2*(q0*q2 - q3*q1)))) * DEG; // pitch
        out[2] = atan2f(2*(q0*q3 + q1*q2), 1 - 2*(q2*q2 + q3*q3)) * DEG;   // yaw
        return 3;
    }

    if (has_prefix(m, "col_")) {
        if (in_n < 4) { int n = in_n < out_max ? in_n : out_max; memcpy(out, in, sizeof(float)*n); return n; }
        in = colour_smooth_apply(cfg->id, cfg->colour_smooth, in, 4);   // noise filter, if enabled
        float r, g, b;
        rgb_to_255(cfg, in, &r, &g, &b);
        apply_ccm(&r, &g, &b);
        if (!strcmp(m, "col_rgb255")) {
            if (out_max < 3) return 0;
            out[0] = roundf(r); out[1] = roundf(g); out[2] = roundf(b);
            return 3;
        }
        float h, s, v;
        rgb_to_hsv(r, g, b, &h, &s, &v);
        if (!strcmp(m, "col_hue")) {
            if (out_max < 3) return 0;
            out[0] = h; out[1] = s; out[2] = v; return 3;
        }
        float crgb[3] = { r, g, b };
        int tcs_id;
        if (in[0] < tcs_none_clear_threshold(cfg)) {
            // No target in range (like a real LEGO colour sensor's -1 "no colour"): clear is
            // too low for a confident reading — nothing reflective close enough, or ambient dark.
            tcs_id = -1;
        } else if (s < 12.0f) {
            // Achromatic (near-zero saturation, matching the web UI's wheel threshold):
            // black/silver/white differ almost entirely by brightness, and TCS_DEF's
            // approximate (unpublished) silver {192,192,192} sits nearly as close to azure
            // {104,195,226} as it does to white {244,244,244} in plain RGB distance — prefer
            // whichever of the three has actually been taught, nearest by brightness (no
            // separate "clear" value needed or captured for TCS; intensity already lives in
            // r/g/b's magnitude). Falls back to a brightness split only if none is taught.
            tcs_id = tcs_achromatic_match(cfg, r, g, b);
            if (tcs_id < 0) {
                float avg = (r + g + b) / 3.0f;
                tcs_id = (avg < 40.0f) ? LC_BLACK : (avg < 220.0f) ? LC_SILVER : LC_WHITE;
            }
        } else {
            tcs_id = colour_match(cfg, crgb, 3, TCS_DEF, TCS_DEF_ID, TCS_DEF_N, 1e18f);
        }
        tcs_id = colour_debounce_apply(cfg->id, tcs_id, cfg->colour_debounce);   // id debounce, if enabled
        if (!strcmp(m, "col_lego")) {
            if (out_max < 1) return 0;
            out[0] = (float)tcs_id; return 1;
        }
        if (!strcmp(m, "col_full")) {            // colour id + reflect% + RGB (0-1024) + raw clear, for passthrough
            if (out_max < 5) return 0;
            float clear = in[0];
            float wref = (cfg->calib_count >= 1 && cfg->calib[0] > 0) ? (float)cfg->calib[0] : 65535.0f;
            out[0] = (float)tcs_id;
            out[1] = roundf(clampf(clear / wref * 100.0f, 0, 100));   // whole % — REFLT is an int8 on the wire
            out[2] = roundf(clampf(r * 1024.0f / 255.0f, 0, 1024));   // ints — RGB I is int16 on the wire
            out[3] = roundf(clampf(g * 1024.0f / 255.0f, 0, 1024));
            out[4] = roundf(clampf(b * 1024.0f / 255.0f, 0, 1024));
            // 6th value: the raw, unscaled clear/intensity count (not part of what passthrough
            // sends to the hub — colour_source's cache read only ever looks at the first 5 — but
            // gives LEGO fields somewhere to get the uncalibrated count without a second sensor
            // entry in "raw" mode).
            if (out_max < 6) return 5;
            out[5] = clear;
            return 6;
        }
    }

    if (has_prefix(m, "as_")) {                 // AS7341 spectral (in: F1-F8,Clear,NIR)
        if (in_n < AS_CH) { int n = in_n < out_max ? in_n : out_max; memcpy(out, in, sizeof(float)*n); return n; }
        in = colour_smooth_apply(cfg->id, cfg->colour_smooth, in, AS_CH);   // noise filter, if enabled
        float ch[AS_CH];
        as_whitecal(in, cfg->calib, cfg->calib_count, ch);
        int as_id = colour_debounce_apply(cfg->id, as_colour_id(cfg, ch, in), cfg->colour_debounce);
        if (!strcmp(m, "as_lego")) {
            if (out_max < 1) return 0;
            out[0] = (float)as_id;                // chRaw = in (pre-calibration)
            return 1;
        }
        if (!strcmp(m, "as_full")) {             // colour id + reflect% + approx RGB (0-1024)
            if (out_max < 5) return 0;
            float wref = (cfg->calib_count >= 9 && cfg->calib[8] > 0) ? (float)cfg->calib[8] : 65535.0f;
            // White reference per output channel (F7+F8/F4+F5/F2+F3), same layout as the
            // combine below. Uncalibrated fallback normalizes against this sample's own clear
            // channel (like TCS's rgb_to_255 fallback) — *not* a fixed 65535, which assumes
            // near-full-scale 16-bit raw counts and crushes real-world raw counts (hundreds to
            // low thousands) down to a few percent of the 0-1024 output range.
            float wr = (cfg->calib_count >= 9) ? (float)cfg->calib[6] + (float)cfg->calib[7] : 0.0f;
            float wg = (cfg->calib_count >= 9) ? (float)cfg->calib[3] + (float)cfg->calib[4] : 0.0f;
            float wb = (cfg->calib_count >= 9) ? (float)cfg->calib[1] + (float)cfg->calib[2] : 0.0f;
            float r_raw = in[6] + in[7], g_raw = in[3] + in[4], b_raw = in[1] + in[2];   // F7+F8/F4+F5/F2+F3 (raw)
            float clear_base = in[8] > 1.0f ? in[8] : 1.0f;
            out[0] = (float)as_id;
            out[1] = roundf(clampf(in[8] / wref * 100.0f, 0, 100));   // whole % — REFLT is an int8 on the wire
            // Scaled against each channel's own white reference (brightness-preserving) — like
            // TCS's col_full — not renormalized to the sample's own max, which erased brightness
            // and always pinned the strongest channel near 1024 regardless of actual light level.
            out[2] = roundf(clampf((wr > 0 ? r_raw / wr : r_raw / clear_base) * 1024.0f, 0, 1024));   // ints — RGB I is int16 on the wire
            out[3] = roundf(clampf((wg > 0 ? g_raw / wg : g_raw / clear_base) * 1024.0f, 0, 1024));
            out[4] = roundf(clampf((wb > 0 ? b_raw / wb : b_raw / clear_base) * 1024.0f, 0, 1024));
            // 6th value: the raw, unscaled Clear channel count — see col_full's identical 6th
            // value above for why (not sent by passthrough, just available for LEGO fields).
            if (out_max < 6) return 5;
            out[5] = in[8];
            return 6;
        }
        if (!strcmp(m, "as_dist")) {             // match score (0-100) to each official colour
            // reference rows in AS_REFS (12-row table): black,white,red,yellow,green,azure,blue,violet
            static const int idx[8] = {0, 1, 3, 5, 6, 8, 9, 11};
            int n = out_max < 8 ? out_max : 8;
            for (int i = 0; i < n; i++) {
                float d = as_dist_sq(ch, AS_REFS[idx[i]]);
                out[i] = clampf((1.0f - d / 0.08f) * 100.0f, 0, 100);   // 100 = closest match
            }
            return n;
        }
    }

    if (has_prefix(m, "dist_")) {
        if (in_n < 1 || out_max < 1) return 0;
        float d = in[0] - (cfg->calib_count >= 1 ? (float)cfg->calib[0] : 0.0f);
        if (d < 0) d = 0;
        out[0] = !strcmp(m, "dist_cm") ? d / 10.0f : d;
        return 1;
    }

    if (!strcmp(m, "pad_digital")) {            // gamepad: sticks/triggers quantized to small ints
        // Digital-friendly gamepad values that pack into tiny LEGO fields with identity scale
        // (like dpad, but with usable resolution): sticks quantize to -7..+7 (15 levels — a
        // signed 4-bit field) with a 10% centre deadzone so a resting stick is exactly 0
        // rather than jittering ±1; triggers quantize to 0..15 (4 bits). buttons/dpad pass
        // through unchanged. Input layout = the driver's raw [buttons,lx,ly,rx,ry,lt,rt,dpad].
        //
        // ldir/rdir (values 8/9): each stick's x+y folded into ONE 8-way compass code using
        // the dpad's own encoding (0 = centred, 1 = up, clockwise to 8 = up-left) — a whole
        // stick in a single unsigned 4-bit field when only direction matters, decoded hub-side
        // exactly like dpad. A 30% radial deadzone keeps a resting stick at 0 and stops the
        // code flickering between neighbouring sectors near the centre.
        if (in_n < 8 || out_max < 8) return 0;
        const float DZ = 3277.0f;               // 10% of full scale (per-axis levels)
        out[0] = in[0];                          // buttons bitmask (map a 16-bit field)
        for (int i = 1; i <= 4; i++) {
            float a = fabsf(in[i]);
            float lvl = (a <= DZ) ? 0.0f : roundf((a - DZ) / (32767.0f - DZ) * 7.0f);
            if (lvl > 7.0f) lvl = 7.0f;
            out[i] = (in[i] < 0) ? -lvl : lvl;
        }
        out[5] = roundf(clampf(in[5], 0, 1023) / 1023.0f * 15.0f);
        out[6] = roundf(clampf(in[6], 0, 1023) / 1023.0f * 15.0f);
        out[7] = in[7];                          // dpad hat code 0-8
        if (out_max < 10) return 8;
        const float DIR_DZ = 9830.0f;           // 30% radial deadzone
        for (int s = 0; s < 2; s++) {            // 0: left stick (in[1],in[2]) — 1: right (in[3],in[4])
            float x = in[1 + 2 * s], y = in[2 + 2 * s];
            if (sqrtf(x * x + y * y) < DIR_DZ) { out[8 + s] = 0; continue; }
            // HID sticks: negative y = up. atan2(x, -y) gives 0° = up, clockwise positive.
            float ang = atan2f(x, -y) * (180.0f / (float)M_PI);
            if (ang < 0) ang += 360.0f;
            out[8 + s] = (float)(((int)roundf(ang / 45.0f) % 8) + 1);
        }
        return 10;
    }

    if (!strcmp(m, "knob_digital")) {           // m5_8angle: 8 knobs, full range, held steady at rest
        // A bare potentiometer's raw 12-bit count wanders by design (even a rock-steady physical
        // position still has a few LSBs of real electrical noise) — EMA smoothing (knob_smooth)
        // reduces that, but any analogue value read often enough will still show SOME motion on
        // a chart. Deadband-hold (see knob_digital_hold above) keeps the full 0-4095 range and
        // near-instant tracking while a knob is actually moving, but freezes the reported value
        // the moment it stops, so a knob sitting still reads as a flat line instead of a jittery
        // one. Independent of knob_smooth — this needs no smoothing config to already stop
        // reporting new values from noise alone at rest.
        if (in_n < 9 || out_max < 9) return 0;
        const float DEADBAND = 24.0f;            // ~0.6% of full scale — covers typical pot/ADC noise at rest
        for (int ch = 0; ch < 8; ch++)
            out[ch] = knob_digital_hold(cfg->id, ch, in[ch], DEADBAND);
        out[8] = in[8];                          // switch passes through unchanged
        return 9;
    }

    if (!strcmp(m, "adc_volts")) {              // ADC counts (0-4095) → volts (12-bit, ~3.3V FS)
        // Applies per channel: a grouped qre1113/tssp_ir sensor (channel_mask set) has in_n > 1,
        // one count per channel — a single-channel sensor is just the in_n == 1 case of the
        // same loop.
        int n = in_n < out_max ? in_n : out_max;
        for (int i = 0; i < n; i++) out[i] = in[i] * 3.3f / 4095.0f;
        return n;
    }

    if (!strcmp(m, "line_reflect")) {           // QRE1113: counts → reflect (0 white-1 black) + detected
        // channel_mask set ⇒ in[] is one raw count per selected channel (drv_qre1113.c); each
        // channel gets its own reflect/detected pair, using the *same* white/black calibration
        // for all of them (calibrated by sweeping the whole bar over white then black in one
        // motion, like a real line-sensor array — not one calibration per channel).
        if (in_n < 1 || out_max < 2) return 0;
        int n = in_n < (out_max / 2) ? in_n : (out_max / 2);
        if (n < 1) n = 1;
        for (int i = 0; i < n; i++) {
            float reflect;
            if (cfg->calib_count < 2) {
                // Uncalibrated: assume a full-scale 12-bit ADC span (0-4095) instead of the raw
                // count so there's still a plausible 0-1 reading (and a live "detected" line)
                // before the user calibrates — a bare passthrough would swamp the 0-1 range the
                // field/dashboard expects.
                reflect = clampf(in[i] / 4095.0f, 0.0f, 1.0f);
            } else {
                float white = (float)cfg->calib[0], black = (float)cfg->calib[1];
                float span = black - white;
                reflect = (fabsf(span) < 1e-6f) ? 0.0f : clampf((in[i] - white) / span, 0.0f, 1.0f);
            }
            out[2 * i] = reflect;
            out[2 * i + 1] = (reflect > 0.5f) ? 1.0f : 0.0f;
        }
        return 2 * n;
    }

    if (!strcmp(m, "ir_ball")) {                // TSSP4038/TSOP34840: min burst counts → strength/detected
        // channel_mask set ⇒ in[] is one channel's burst-minimum per selected channel
        // (drv_ir_ball.c); every channel shares one idle-baseline calibration (captured with no
        // object anywhere near the whole ring, not per channel).
        if (in_n < 1 || out_max < 2) return 0;
        int n = in_n < (out_max / 2) ? in_n : (out_max / 2);
        if (n < 1) n = 1;
        // Uncalibrated: assume the receiver idles near full-scale (idles high, dips low on
        // detection — see drv_ir_ball.c) instead of using this very sample as its own baseline,
        // which would make every dip trivially zero and "detected" permanently false.
        float baseline = (cfg->calib_count >= 1) ? (float)cfg->calib[0] : 4095.0f;
        for (int i = 0; i < n; i++) {
            float dip = baseline - in[i];
            float strength = (baseline > 1.0f) ? clampf(dip / baseline, 0.0f, 1.0f) : 0.0f;
            out[2 * i] = strength;
            out[2 * i + 1] = (strength > 0.08f) ? 1.0f : 0.0f;
        }
        return 2 * n;
    }

    // Unknown mode — passthrough.
    int n = in_n < out_max ? in_n : out_max;
    memcpy(out, in, sizeof(float) * n);
    return n;
}

int sensor_transform_names(const sensor_cfg_t *cfg, const char *names[], int max)
{
    const char *m = cfg->transform;

    // Grouped qre1113/tssp_ir (channel_mask set): one name (adc_volts) or pair of names
    // (line_reflect/ir_object) per selected channel, e.g. "ch2_reflect"/"ch2_detected" — lets the
    // dashboard/display value_mask and LEGO field picker address one specific channel, the same
    // way they already address any other sensor's named values.
    if (cfg->channel_mask != 0 &&
        (!strcmp(m, "line_reflect") || !strcmp(m, "ir_ball") || !strcmp(m, "adc_volts"))) {
        static const char *CH_REFLECT[8]  = { "ch0_reflect",  "ch1_reflect",  "ch2_reflect",  "ch3_reflect",
                                               "ch4_reflect",  "ch5_reflect",  "ch6_reflect",  "ch7_reflect" };
        static const char *CH_STRENGTH[8] = { "ch0_strength", "ch1_strength", "ch2_strength", "ch3_strength",
                                               "ch4_strength", "ch5_strength", "ch6_strength", "ch7_strength" };
        static const char *CH_DETECTED[8] = { "ch0_detected", "ch1_detected", "ch2_detected", "ch3_detected",
                                               "ch4_detected", "ch5_detected", "ch6_detected", "ch7_detected" };
        static const char *CH_VOLTS[8]    = { "ch0_volts",    "ch1_volts",    "ch2_volts",    "ch3_volts",
                                               "ch4_volts",    "ch5_volts",    "ch6_volts",    "ch7_volts" };
        int c = 0;
        for (int ch = 0; ch < 8 && c < max; ch++) {
            if (!(cfg->channel_mask & (1u << ch))) continue;
            if (!strcmp(m, "adc_volts")) {
                names[c++] = CH_VOLTS[ch];
            } else {
                names[c++] = !strcmp(m, "line_reflect") ? CH_REFLECT[ch] : CH_STRENGTH[ch];
                if (c < max) names[c++] = CH_DETECTED[ch];
            }
        }
        return c;
    }

    const char *src[4] = {0};
    int n = 0;
    if (!m[0] || !strcmp(m, "raw"))        { return 0; }
    else if (!strcmp(m, "imu_orient") || !strcmp(m, "imu_orient9")) { src[0]="roll"; src[1]="pitch"; src[2]="yaw"; n=3; }
    else if (!strcmp(m, "imu_tilt"))       { src[0]="pitch"; src[1]="roll"; n=2; }
    else if (!strcmp(m, "col_rgb255"))     { src[0]="r"; src[1]="g"; src[2]="b"; n=3; }
    else if (!strcmp(m, "col_hue"))        { src[0]="hue"; src[1]="sat"; src[2]="val"; n=3; }
    else if (!strcmp(m, "col_lego"))       { src[0]="colour"; n=1; }
    else if (!strcmp(m, "as_lego"))        { src[0]="colour"; n=1; }
    else if (has_prefix(m, "dist_"))       { src[0]="dist"; n=1; }
    else if (!strcmp(m, "adc_volts"))      { src[0]="volts"; n=1; }
    else if (!strcmp(m, "line_reflect"))   { src[0]="reflect"; src[1]="detected"; n=2; }
    else if (!strcmp(m, "ir_ball"))        { src[0]="strength"; src[1]="detected"; n=2; }
    int c = n < max ? n : max;
    for (int i = 0; i < c; i++) names[i] = src[i];

    static const char *PAD[10] = {"buttons", "lx7", "ly7", "rx7", "ry7", "lt15", "rt15", "dpad", "ldir", "rdir"};
    if (!strcmp(m, "pad_digital")) {
        c = max < 10 ? max : 10;
        for (int i = 0; i < c; i++) names[i] = PAD[i];
        return c;
    }

    static const char *KNOBS[9] = {"k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "switch"};
    if (!strcmp(m, "knob_digital")) {
        c = max < 9 ? max : 9;
        for (int i = 0; i < c; i++) names[i] = KNOBS[i];
        return c;
    }

    // wider value sets (built directly into names[])
    static const char *FULL[6] = {"colour", "reflect", "r", "g", "b", "clear"};
    static const char *DIST[8] = {"black", "white", "red", "yellow", "green", "lblue", "blue", "violet"};
    if (!strcmp(m, "col_full") || !strcmp(m, "as_full")) {
        c = max < 6 ? max : 6;
        for (int i = 0; i < c; i++) names[i] = FULL[i];
        return c;
    }
    if (!strcmp(m, "as_dist")) {
        c = max < 8 ? max : 8;
        for (int i = 0; i < c; i++) names[i] = DIST[i];
        return c;
    }
    return c;
}

int sensor_transform_calibrate(const char *mode, const char *point,
                               const double *calib_in, int calib_n,
                               const float *raw, int raw_n,
                               double *calib, int max)
{
    if (has_prefix(mode, "col_")) {            // white reference: clear,red,green,blue
        if (raw_n < 4 || max < 4) return -1;
        // Reject a saturated capture: a channel pinned at the TCS34725's 16-bit full scale has
        // clipped, and a clipped white reference under-corrects that channel forever after.
        for (int i = 0; i < 4; i++)
            if (raw[i] >= 65535.0f * 0.99f) return -1;
        for (int i = 0; i < 4; i++) calib[i] = raw[i];
        return 4;
    }
    if (has_prefix(mode, "as_")) {             // white reference: 10 spectral channels
        int n = raw_n < AS_CH ? raw_n : AS_CH;
        if (n > max) n = max;
        // Reject a saturated capture (>= AS_RAW_CLIP, the ADC ceiling times the driver's count
        // scaling): a white reference with clipped F-channels flattens the very spectral shape
        // the classifier matches on — silently accepting one is how a whole palette of taught
        // colours ends up near-indistinguishable. Turn the LED down / move the white target
        // further away and recapture instead. Only F1-F8 (indices 0-7) gate the capture: Clear
        // aggregates the whole spectrum (brightest channel by far), and the matcher weights
        // Clear/NIR at ~zero — blocking on them made white calibration near-impossible. The
        // web CalibrationSummary warns when Clear clipped instead.
        int n_gate = n < 8 ? n : 8;
        for (int i = 0; i < n_gate; i++)
            if (raw[i] >= AS_RAW_CLIP) return -1;
        for (int i = 0; i < n; i++) calib[i] = raw[i];
        return n;
    }
    if (has_prefix(mode, "imu_")) {            // gyro bias (board held still)
        if (raw_n < 6 || max < 3) return -1;
        for (int i = 0; i < 3; i++) calib[i] = raw[3 + i];
        return 3;
    }
    if (has_prefix(mode, "dist_")) {           // zero offset
        if (raw_n < 1 || max < 1) return -1;
        calib[0] = raw[0];
        return 1;
    }
    if (!strcmp(mode, "line_reflect")) {       // two-point: white (calib[0]) / black (calib[1])
        if (raw_n < 1 || max < 2) return -1;
        double white = (calib_n >= 1) ? calib_in[0] : 0.0;
        double black = (calib_n >= 2) ? calib_in[1] : 0.0;
        // Grouped (channel_mask set): raw[] is one count per channel. One shared calibration
        // point for the whole group — average across the channels captured in this sample
        // (sweep the whole bar over white, then black, in one motion) rather than one channel's
        // count on its own.
        double avg = 0; for (int i = 0; i < raw_n; i++) avg += raw[i]; avg /= raw_n;
        if (point && !strcmp(point, "black")) black = avg; else white = avg;
        calib[0] = white; calib[1] = black;
        return 2;
    }
    if (!strcmp(mode, "ir_ball")) {            // idle baseline (no ball present)
        if (raw_n < 1 || max < 1) return -1;
        // Grouped: one shared baseline averaged across every channel's burst-minimum, captured
        // with no ball anywhere near the whole ring.
        double avg = 0; for (int i = 0; i < raw_n; i++) avg += raw[i]; avg /= raw_n;
        calib[0] = avg;
        return 1;
    }
    return -1;
}

int sensor_transform_capture_colour(const sensor_cfg_t *cfg, const float *raw, int raw_n,
                                    float *ref_out)
{
    if (!strcmp(cfg->type, "as7341")) {        // post-whitecal spectrum (10 channels)
        if (raw_n < AS_CH) return 0;
        // Refuse to teach from a clipped sample (>= AS_RAW_CLIP): clipping flattens the
        // spectrum's shape, and a flat-topped taught reference is near-indistinguishable from
        // every other flat-topped one. Only F1-F8 (indices 0-7) gate the capture — Clear is
        // the brightest channel by far (whole-spectrum photodiode), so gating on it made
        // bright colours unteachable; a clipped Clear only degrades black/white/silver
        // separation, which the web CalibrationSummary warns about instead. Also refuse
        // teaching before a white calibration exists — the ref would be stored in raw units,
        // in a different space from every post-whitecal live reading and later-taught colour.
        for (int i = 0; i < 8; i++)
            if (raw[i] >= AS_RAW_CLIP) return 0;
        if (cfg->calib_count < AS_CH) return 0;
        // Same smoothing live classification applies, so a taught reference matches what
        // as_colour_id() actually compares against later (not a single noisy raw sample).
        raw = colour_smooth_apply(cfg->id, cfg->colour_smooth, raw, AS_CH);
        float ch[AS_CH];
        as_whitecal(raw, cfg->calib, cfg->calib_count, ch);
        for (int i = 0; i < AS_CH; i++) ref_out[i] = ch[i];
        // ref[8] carries the RAW Clear count, not the whitecal one: whitecal normalises shape
        // to peak 1000 and discards intensity — the only thing separating black/white/silver.
        // The chromatic matcher weights index 8 at zero (as_dist_sq), so this slot is free;
        // as_achromatic_match() compares it against the live raw Clear. Requires teaching and
        // classifying under the same LED setting/geometry (already the documented practice).
        ref_out[8] = raw[8];
        return AS_CH;
    }
    // TCS34725-style: white-balanced RGB (0-255)
    if (raw_n < 4) return 0;
    // Same reasoning as the AS7341 gate above: without a white calibration, rgb_to_255() falls
    // back to a crude "divide by clear channel" normalisation (see its calib_count<4 branch) —
    // a different, inconsistent space from every reference (TCS_DEF defaults and any other
    // taught colour) that WAS captured post-whitecal. Teaching in that mismatched space used to
    // silently "succeed" and store a reference that then just never matched anything live,
    // classifying as "none" with no indication why. Refuse it here instead, same as AS7341.
    if (cfg->calib_count < 4) return 0;
    raw = colour_smooth_apply(cfg->id, cfg->colour_smooth, raw, 4);
    float r, g, b;
    rgb_to_255(cfg, raw, &r, &g, &b);
    apply_ccm(&r, &g, &b);   // taught reference must live in the same corrected space as live reads
    ref_out[0] = r; ref_out[1] = g; ref_out[2] = b;
    return 3;
}
