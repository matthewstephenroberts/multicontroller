// sensor_simulate.c — fabricate plausible sensor data with no bus access, so a sensor can be
// configured, viewed on the dashboard/display, and emitted to LEGO before hardware is wired up.
#include "sensor.h"
#include "sensor_transform.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Per-channel plausible range, looked up by the driver's describe() name. Values not found
// here (custom "generic" recipe names) fall back to k_default_range.
typedef struct {
    const char *name;
    float       lo, hi;
} sim_range_t;

static const sim_range_t k_ranges[] = {
    { "temp",     18.0f,   28.0f },     // °C
    { "pressure", 980.0f, 1030.0f },    // hPa
    { "humidity", 30.0f,   70.0f },     // %RH
    { "clear",    200.0f, 4000.0f },    // colour raw counts
    { "red",      100.0f, 3000.0f },
    { "green",    100.0f, 3000.0f },
    { "blue",     100.0f, 3000.0f },
    { "F1",        50.0f, 3000.0f },    // AS7341 spectral channels
    { "F2",        50.0f, 3000.0f },
    { "F3",        50.0f, 3000.0f },
    { "F4",        50.0f, 3000.0f },
    { "F5",        50.0f, 3000.0f },
    { "F6",        50.0f, 3000.0f },
    { "F7",        50.0f, 3000.0f },
    { "F8",        50.0f, 3000.0f },
    { "nir",       50.0f, 3000.0f },
    { "ax",       -1.0f,   1.0f },      // IMU accel, g
    { "ay",       -1.0f,   1.0f },
    { "az",        0.5f,   1.0f },      // resting near +1g
    { "gx",      -50.0f,  50.0f },      // IMU gyro, deg/s
    { "gy",      -50.0f,  50.0f },
    { "gz",      -50.0f,  50.0f },
    { "dist",     50.0f, 2000.0f },     // ToF, mm
    { "co2",     400.0f, 1500.0f },     // ppm
    { "state",     0.0f,   1.0f },      // gpio digital in
    { "counts",    0.0f, 4095.0f },     // adc raw
};
static const sim_range_t k_default_range = { NULL, 0.0f, 100.0f };

static const sim_range_t *find_range(const char *name)
{
    if (name)
        for (size_t i = 0; i < sizeof(k_ranges) / sizeof(k_ranges[0]); i++)
            if (!strcmp(k_ranges[i].name, name)) return &k_ranges[i];
    return &k_default_range;
}

// Per-(sensor id, channel) walk state so consecutive reads drift smoothly instead of jumping
// randomly every poll — looks like a live sensor on the dashboard's live view/graphs.
typedef struct {
    int   sensor_id;
    int   channel;
    bool  has_value;
    float value;
} sim_state_t;

#define SIM_SLOTS (MC_MAX_SENSORS * MC_MAX_VALUES)
static sim_state_t s_state[SIM_SLOTS];

static sim_state_t *state_for(int sensor_id, int channel)
{
    sim_state_t *free_slot = NULL;
    for (int i = 0; i < SIM_SLOTS; i++) {
        sim_state_t *s = &s_state[i];
        if (s->has_value && s->sensor_id == sensor_id && s->channel == channel) return s;
        if (!free_slot && !s->has_value) free_slot = s;
    }
    if (!free_slot) free_slot = &s_state[(unsigned)(sensor_id * 31 + channel) % SIM_SLOTS];
    free_slot->sensor_id = sensor_id;
    free_slot->channel = channel;
    return free_slot;
}

static float randf_unit(void) { return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f; }

// Colour sensors (TCS34725/AS7341) can't use independent per-channel noise like other sensor
// types: their channels are correlated (a real target's clear/red/green/blue or F1-F8/Clear/NIR
// follow one reflectance spectrum), and AS7341's classifier only accepts samples within a tight
// chromaticity distance of a real reference — pure noise essentially never matches, so it would
// permanently report "no colour" while TCS (whose match threshold is unbounded) always finds
// some nearest colour. Instead, hold a "demo colour" target for a few polls, ease toward it, and
// add a little noise — both sensors then report a real, changing, classifiable colour.
typedef struct {
    int  sensor_id;
    bool used;
    int  target;
    int  ticks_left;
} colour_target_t;
static colour_target_t s_colour[MC_MAX_SENSORS];

static colour_target_t *colour_state_for(int sensor_id)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_colour[i].used && s_colour[i].sensor_id == sensor_id) return &s_colour[i];
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (!s_colour[i].used) {
            s_colour[i] = (colour_target_t){ .sensor_id = sensor_id, .used = true, .target = -1, .ticks_left = 0 };
            return &s_colour[i];
        }
    return &s_colour[0];
}

static bool simulate_colour(const sensor_cfg_t *cfg, int n, float *out)
{
    if (n != 4 && n != 10) return false;      // not a colour-shaped read (TCS34725 / AS7341)
    float target[MC_MAX_VALUES];

    colour_target_t *ct = colour_state_for(cfg->id);
    if (ct->ticks_left <= 0) {
        int next = rand() % SENSOR_TRANSFORM_NUM_DEMO_COLOURS;
        if (next == ct->target) next = (next + 1) % SENSOR_TRANSFORM_NUM_DEMO_COLOURS;
        ct->target = next;
        ct->ticks_left = 6 + rand() % 6;       // hold each demo colour ~6-11 polls before easing to the next
    }
    ct->ticks_left--;
    sensor_transform_demo_colour(ct->target, n, target);

    for (int i = 0; i < n; i++) {
        sim_state_t *st = state_for(cfg->id, i);
        float v = st->has_value ? st->value : target[i];
        v += (target[i] - v) * 0.35f;                                // ease toward the target colour
        v += randf_unit() * fmaxf(target[i], 50.0f) * 0.03f;         // small live-looking noise
        if (v < 0) v = 0;
        st->value = v;
        st->has_value = true;
        out[i] = v;
    }
    return true;
}

esp_err_t sensor_simulate_read(const sensor_driver_t *drv, const sensor_cfg_t *cfg,
                                float *out, int max, int *out_count)
{
    const char *names[MC_MAX_VALUES];
    int n = (drv && drv->describe) ? drv->describe(cfg, names, max) : 0;
    if (n <= 0) {
        n = cfg->recipe.value_count > 0 ? cfg->recipe.value_count : 1;
        if (n > max) n = max;
        for (int i = 0; i < n; i++)
            names[i] = (i < cfg->recipe.value_count) ? cfg->recipe.value_names[i] : "value";
    }

    bool is_colour = n > 0 && names[0] && (!strcmp(names[0], "clear") || !strcmp(names[0], "F1"));
    if (is_colour && simulate_colour(cfg, n, out)) {
        *out_count = n;
        return ESP_OK;
    }

    for (int i = 0; i < n; i++) {
        const sim_range_t *r = find_range(names[i]);
        float span = r->hi - r->lo;
        sim_state_t *st = state_for(cfg->id, i);
        float v = st->has_value ? st->value : (r->lo + span * 0.5f);
        v += randf_unit() * span * 0.05f;          // ~5% of range per sample, smoothed drift
        if (v < r->lo) v = r->lo;
        if (v > r->hi) v = r->hi;
        st->value = v;
        st->has_value = true;
        out[i] = v;
    }

    *out_count = n;
    return ESP_OK;
}
