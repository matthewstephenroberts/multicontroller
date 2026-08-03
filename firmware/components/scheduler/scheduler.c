// scheduler.c — single polling task; per-sensor due times; latest-reading table.
//
// The poll task exclusively owns the active job list. A rebuild stages a fresh config
// that the task adopts at the top of its loop, so bus I/O never runs under the lock.
//
#include "scheduler.h"
#include "sensor_transform.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "config_store.h"

static const char *TAG = "scheduler";

typedef struct {
    sensor_cfg_t cfg;
    int64_t      next_due_ms;
    reading_t    last;                  // transformed (derived) reading
    float        raw_values[MC_MAX_VALUES]; // pre-transform, for calibration
    int          raw_count;
    bool         has_reading;
} job_t;

static job_t       s_jobs[MC_MAX_SENSORS];     // owned by the poll task
static int         s_njobs;

static sensor_cfg_t s_pending[MC_MAX_SENSORS]; // staged by rebuild()
static int          s_npending;
static bool         s_pending_ready;

static SemaphoreHandle_t s_lock;               // guards pending + each job's reading
static volatile bool s_running;
static reading_cb_t  s_cb;
static TaskHandle_t  s_task;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static const char *status_str(esp_err_t err)
{
    if (err == ESP_OK) return "ok";
    if (err == ESP_ERR_TIMEOUT) return "timeout";
    return "error";
}

static void adopt_pending_locked(void)
{
    s_njobs = s_npending;
    for (int i = 0; i < s_njobs; i++) {
        s_jobs[i].cfg = s_pending[i];
        s_jobs[i].next_due_ms = now_ms();      // due immediately
        s_jobs[i].has_reading = false;
    }
    s_pending_ready = false;
    ESP_LOGI(TAG, "adopted %d jobs", s_njobs);
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pending_ready) adopt_pending_locked();
        xSemaphoreGive(s_lock);

        if (!s_running) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        int64_t now = now_ms();
        int64_t soonest = now + 200;

        // Collect this pass's due jobs, then run them grouped by (mux, channel): consecutive
        // reads behind the same mux channel skip the redundant select write entirely
        // (i2c_mux_select caches the current channel), and muxes get switched the minimum
        // number of times per pass instead of ping-ponging in config order. Insertion sort —
        // MC_MAX_SENSORS is 16, and the list is usually already grouped.
        int due[MC_MAX_SENSORS], ndue = 0;
        for (int i = 0; i < s_njobs; i++)
            if (s_jobs[i].cfg.enabled && now >= s_jobs[i].next_due_ms) due[ndue++] = i;
        for (int a = 1; a < ndue; a++) {
            int idx = due[a], b = a;
            const sensor_cfg_t *c = &s_jobs[idx].cfg;
            while (b > 0) {
                const sensor_cfg_t *p = &s_jobs[due[b - 1]].cfg;
                if (p->mux_addr < c->mux_addr ||
                    (p->mux_addr == c->mux_addr && p->mux_channel <= c->mux_channel)) break;
                due[b] = due[b - 1];
                b--;
            }
            due[b] = idx;
        }

        for (int d = 0; d < ndue; d++) {
            job_t *j = &s_jobs[due[d]];
            reading_t r = { .id = j->cfg.id };
            float raw[MC_MAX_VALUES];
            int cnt = 0;
            esp_err_t err = sensor_read(&j->cfg, raw, MC_MAX_VALUES, &cnt);
            // Map raw → derived values per the sensor's transform (passthrough for "raw").
            r.count  = (err == ESP_OK)
                ? sensor_transform_apply(&j->cfg, raw, cnt, r.values, MC_MAX_VALUES) : 0;
            r.ts_ms  = now_ms();
            r.status = status_str(err);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            j->last = r;
            memcpy(j->raw_values, raw, sizeof(raw));
            j->raw_count = (err == ESP_OK) ? cnt : 0;
            j->has_reading = true;
            xSemaphoreGive(s_lock);

            if (s_cb) s_cb(&r);
            j->next_due_ms = now + j->cfg.poll_ms;
        }

        for (int i = 0; i < s_njobs; i++)
            if (s_jobs[i].cfg.enabled && s_jobs[i].next_due_ms < soonest) soonest = s_jobs[i].next_due_ms;

        int64_t wait = soonest - now_ms();
        if (wait < 5)   wait = 5;
        if (wait > 200) wait = 200;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

esp_err_t scheduler_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(poll_task, "poll", 4096, NULL, 5, &s_task) != pdPASS)
        return ESP_FAIL;
    return ESP_OK;
}

void scheduler_set_reading_cb(reading_cb_t cb) { s_cb = cb; }

void scheduler_rebuild(void)
{
    const sensor_cfg_t *arr;
    size_t count;
    config_store_get(&arr, &count);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_npending = (int)count;
    memcpy(s_pending, arr, sizeof(sensor_cfg_t) * count);
    s_pending_ready = true;
    xSemaphoreGive(s_lock);
}

void scheduler_start(void) { s_running = true; }
void scheduler_stop(void)  { s_running = false; }

esp_err_t scheduler_calibrate(int id, const char *point)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    double calib[MC_MAX_CALIB];
    int n = -1;
    char mode[MC_XFORM_LEN] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_njobs; i++) {
        if (s_jobs[i].cfg.id == id && s_jobs[i].has_reading) {
            strncpy(mode, s_jobs[i].cfg.transform, sizeof(mode) - 1);
            // Colour sensors capture the same white reference (their raw channels) in every
            // convert mode — key off the type so Calibrate also works in "raw", the mode the
            // live spectrum view needs. Teach requires a whitecal, so without this a user
            // inspecting the raw spectrum could never teach at all.
            if (!strcmp(s_jobs[i].cfg.type, "as7341"))        strcpy(mode, "as_");
            else if (!strcmp(s_jobs[i].cfg.type, "tcs34725")) strcpy(mode, "col_");
            n = sensor_transform_calibrate(mode, point,
                                           s_jobs[i].cfg.calib, s_jobs[i].cfg.calib_count,
                                           s_jobs[i].raw_values, s_jobs[i].raw_count,
                                           calib, MC_MAX_CALIB);
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (n < 0) return ESP_ERR_INVALID_ARG;          // no reading yet, or mode has no calibration
    esp_err_t err = config_store_set_calib(id, calib, n);
    if (err == ESP_OK) scheduler_rebuild();         // adopt the new calib on the next loop
    return err;
}

// Teach colour `name`→`out_id`: capture the current reading as that colour's reference and
// upsert it (by name) into the sensor's learnable palette. Mirrors scheduler_calibrate.
esp_err_t scheduler_learn_colour(int id, const char *name, int out_id)
{
    if (!s_lock || !name || !name[0]) return ESP_ERR_INVALID_ARG;
    colour_ref_t pal[MC_MAX_COLOURS];
    int count = -1;
    float ref[MC_COL_CH] = {0};
    int rn = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_njobs; i++) {
        if (s_jobs[i].cfg.id == id && s_jobs[i].has_reading) {
            rn = sensor_transform_capture_colour(&s_jobs[i].cfg, s_jobs[i].raw_values,
                                                 s_jobs[i].raw_count, ref);
            count = s_jobs[i].cfg.colour_count;
            memcpy(pal, s_jobs[i].cfg.colours, sizeof(pal));
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (count < 0 || rn <= 0) return ESP_ERR_INVALID_ARG;   // no reading, or not a colour sensor

    int slot = -1;                                  // upsert by name
    for (int i = 0; i < count; i++) if (!strncmp(pal[i].name, name, MC_COL_NAME_LEN)) { slot = i; break; }
    if (slot < 0) {
        if (count >= MC_MAX_COLOURS) return ESP_ERR_NO_MEM;
        slot = count++;
        memset(&pal[slot], 0, sizeof(pal[slot]));
        strncpy(pal[slot].name, name, MC_COL_NAME_LEN - 1);
    }
    pal[slot].out_id = out_id;
    pal[slot].learned = true;
    memset(pal[slot].ref, 0, sizeof(pal[slot].ref));
    for (int i = 0; i < rn && i < MC_COL_CH; i++) pal[slot].ref[i] = ref[i];

    esp_err_t err = config_store_set_colours(id, pal, count);
    if (err == ESP_OK) scheduler_rebuild();
    return err;
}

// Reset colour `name`: remove its palette entry (a learned standard colour falls back to its
// built-in default; a custom colour is removed entirely).
esp_err_t scheduler_reset_colour(int id, const char *name)
{
    if (!s_lock || !name) return ESP_ERR_INVALID_ARG;
    colour_ref_t pal[MC_MAX_COLOURS];
    int count = -1;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_njobs; i++) {
        if (s_jobs[i].cfg.id == id) {
            count = s_jobs[i].cfg.colour_count;
            memcpy(pal, s_jobs[i].cfg.colours, sizeof(pal));
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (count < 0) return ESP_ERR_NOT_FOUND;

    int out = 0;
    for (int i = 0; i < count; i++)
        if (strncmp(pal[i].name, name, MC_COL_NAME_LEN) != 0) pal[out++] = pal[i];

    esp_err_t err = config_store_set_colours(id, pal, out);
    if (err == ESP_OK) scheduler_rebuild();
    return err;
}

bool scheduler_get_reading(int id, reading_t *out)
{
    if (!s_lock) return false;            // may be queried (e.g. by the display) before init
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_njobs; i++) {
        if (s_jobs[i].cfg.id == id && s_jobs[i].has_reading) {
            *out = s_jobs[i].last;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}
