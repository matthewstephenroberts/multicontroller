// scheduler.h — FreeRTOS polling task + latest-reading table.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     id;
    int64_t ts_ms;                  // device time of the reading
    int     count;
    float   values[MC_MAX_VALUES];
    const char *status;             // "ok" | "error" | "timeout"
} reading_t;

// Called from the scheduler task whenever a sensor produces a fresh reading.
typedef void (*reading_cb_t)(const reading_t *r);

// Create the polling task (suspended until scheduler_start) and reading table.
esp_err_t scheduler_init(void);

// Register a sink for fresh readings (BLE notify). Pass NULL to clear.
void scheduler_set_reading_cb(reading_cb_t cb);

// Reload the sensor list from config_store and reset the poll schedule atomically.
void scheduler_rebuild(void);

// Start / stop polling.
void scheduler_start(void);
void scheduler_stop(void);

// Copy the latest reading for sensor `id` into *out. Returns false if none yet.
bool scheduler_get_reading(int id, reading_t *out);

// Capture a calibration blob for sensor `id` from its latest raw reading (per its transform
// mode), persist it to NVS, and rebuild. Board should be held still / a white tile presented
// as appropriate. `point` selects which point a multi-point mode captures (e.g. "line_reflect"'s
// "white"/"black"); NULL for single-point modes. Returns ESP_ERR_INVALID_ARG if there's no
// reading or the mode has no calibration.
esp_err_t scheduler_calibrate(int id, const char *point);

// Teach colour sensor `id`: capture its latest reading as the reference for colour `name`
// (reporting `out_id`), upsert into the learnable palette, persist + rebuild.
esp_err_t scheduler_learn_colour(int id, const char *name, int out_id);

// Reset colour `name` on sensor `id`: remove its learned/custom palette entry, persist + rebuild.
esp_err_t scheduler_reset_colour(int id, const char *name);

#ifdef __cplusplus
}
#endif
