#include "config_store.h"
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "board_config.h"

static const char *TAG = "config_store";
#define NVS_NS  "mcfg"
#define NVS_KEY "sensors"
// Previous-generation copy of NVS_KEY, rotated into place just before each new write (see
// persist_task). Lets config_store_init() recover a working config if the primary blob is
// ever corrupt/unparseable (e.g. a write interrupted by power loss mid-flash-erase) instead of
// silently treating "parse failed" the same as "no config yet" and reseeding an empty config
// over top of it.
#define NVS_KEY_BAK "sensors_bak"

static sensor_cfg_t  s_sensors[MC_MAX_SENSORS];
static size_t        s_count;
static uint32_t      s_version = 1;
static display_cfg_t s_display;
static lego_cfg_t    s_lego;
static char          s_device_name[MC_DEVICE_NAME_LEN] = BOARD_BLE_NAME;
static int64_t       s_polling_cap_us = 20 * 1000;  // 50Hz default
static ble_power_cfg_t s_ble_power = {
    .conn_itvl_min = 48,        // 60ms
    .conn_itvl_max = 96,        // 120ms
    .tx_power = 0,              // max power
    .idle_disconnect_s = 0,     // disabled
};
// verbose_debug's live value lives in sensor.c (sensor_get/set_verbose_debug) — see the
// comment there for why (config_store REQUIRES sensor, so the flag can't live here and be
// read back by sensor's drivers without a circular component dependency). Persistence and
// the BLE command still live here, same as every other setting.
static SemaphoreHandle_t s_lock;

// Async NVS persist: nvs_commit() of the ~KB-sized config blob can block for over a second
// (flash erase/write), and every config-mutating command (set_config, calibrate, learn_colour,
// factory_reset, ...) used to call it inline from the BLE command task — starving the BLE host
// task on the same core for that whole time and tripping the link's supervision timeout
// (disconnect reason 520 = HCI Connection Timeout). The in-memory config (s_sensors/s_display/
// s_lego, under s_lock) is updated synchronously as before — only the flash write itself moves
// to persist_task, off the BLE-critical path. Only the *latest* snapshot matters, so a
// still-pending one is superseded rather than queued (a burst of edits then doesn't pile up N
// flash writes — just the last state actually gets committed).
static SemaphoreHandle_t s_persist_lock;
static SemaphoreHandle_t s_persist_sem;
static char             *s_persist_pending;   // owned by whichever side currently holds it

// Whether the last queued persist actually reached flash. set_config's synchronous return only
// reflects the RAM update + successful hand-off to persist_task — the real nvs_commit() happens
// here, off the BLE-command path, so a failure here (heap pressure from a burst of concurrent
// BLE-notify traffic while sensors are polling quickly, a transient NVS error, ...) would
// otherwise be invisible to the client: it already got "ok:true" before this ran. Exposed via
// config_store_last_persist_ok() so get_config can surface it (see ble_protocol.c).
static volatile bool s_last_persist_ok = true;

static void persist_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_persist_sem, portMAX_DELAY);
        xSemaphoreTake(s_persist_lock, portMAX_DELAY);
        char *json = s_persist_pending;
        s_persist_pending = NULL;
        xSemaphoreGive(s_persist_lock);
        if (!json) continue;

        // A transient failure here (most commonly ESP_ERR_NO_MEM from heap pressure while a lot
        // of concurrent BLE-notify traffic is in flight) is usually gone a moment later once
        // that traffic's buffers are freed — retry a few times with a short backoff instead of
        // giving up on the very first attempt and silently leaving the board on stale flash
        // content.
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(200));
            nvs_handle_t h;
            err = nvs_open(NVS_NS, NVS_READWRITE, &h);
            if (err != ESP_OK) continue;
            // Rotate the current primary blob into the backup slot before overwriting it, so a
            // torn/corrupt write below still leaves a recoverable prior generation on flash.
            size_t old_len = 0;
            if (nvs_get_blob(h, NVS_KEY, NULL, &old_len) == ESP_OK && old_len > 0) {
                char *old = malloc(old_len);
                if (old && nvs_get_blob(h, NVS_KEY, old, &old_len) == ESP_OK)
                    nvs_set_blob(h, NVS_KEY_BAK, old, old_len);
                free(old);
            }
            err = nvs_set_blob(h, NVS_KEY, json, strlen(json) + 1);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
            if (err == ESP_OK) break;
        }
        s_last_persist_ok = (err == ESP_OK);
        if (err != ESP_OK) ESP_LOGE(TAG, "async nvs persist failed after retries: %s", esp_err_to_name(err));
        free(json);
    }
}

bool config_store_last_persist_ok(void) { return s_last_persist_ok; }

// ---- JSON helpers ----------------------------------------------------------

static bus_type_t bus_from_str(const char *s)
{
    if (s && !strcmp(s, "spi"))  return BUS_SPI;
    if (s && !strcmp(s, "uart")) return BUS_UART;
    return BUS_I2C;
}
static const char *bus_to_str(bus_type_t b)
{
    switch (b) { case BUS_SPI: return "spi"; case BUS_UART: return "uart"; default: return "i2c"; }
}

static int json_int(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}
static double json_num(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}
static bool json_bool(const cJSON *o, const char *k, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}
static void json_str(const cJSON *o, const char *k, char *dst, size_t n, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    const char *s = cJSON_IsString(v) ? v->valuestring : dflt;
    strncpy(dst, s ? s : "", n - 1);
    dst[n - 1] = '\0';
}

// Parse one sensor object. Returns false on a hard validation failure.
static bool parse_sensor(const cJSON *o, sensor_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id   = json_int(o, "id", 0);
    json_str(o, "name", out->name, sizeof(out->name), "");
    json_str(o, "type", out->type, sizeof(out->type), "generic");
    if (out->type[0] == '\0') strcpy(out->type, "generic");

    const cJSON *busv = cJSON_GetObjectItemCaseSensitive(o, "bus");
    out->bus = bus_from_str(cJSON_IsString(busv) ? busv->valuestring : "i2c");

    out->addr        = (uint8_t)json_int(o, "addr", 0);
    out->mux_addr    = (uint8_t)json_int(o, "mux_addr", 0);
    out->mux_channel = (int8_t) json_int(o, "mux_channel", -1);
    out->cs_index    = (int8_t) json_int(o, "cs_index", 0);
    out->port        =          json_int(o, "port", 1);
    out->channel_mask = (uint8_t)json_int(o, "channel_mask", 0);
    out->poll_ms     = (uint32_t)json_int(o, "poll_ms", 1000);
    out->enabled     = json_bool(o, "enabled", true);
    out->simulate    = json_bool(o, "simulate", false);
    out->show        = json_bool(o, "show", false);
    out->page        =          json_int(o, "page", 0);
    out->value_mask  = (unsigned)json_int(o, "value_mask", 0xFFFF);   // default: all values

    const cJSON *r = cJSON_GetObjectItemCaseSensitive(o, "recipe");
    if (cJSON_IsObject(r)) {
        out->recipe.reg        = json_int(r, "reg", 0);
        out->recipe.length     = json_int(r, "length", 1);
        out->recipe.big_endian = json_bool(r, "byte_order", true) ; // overwritten below
        const cJSON *bo = cJSON_GetObjectItemCaseSensitive(r, "byte_order");
        out->recipe.big_endian = !(cJSON_IsString(bo) && !strcmp(bo->valuestring, "le"));
        out->recipe.is_signed  = json_bool(r, "signed", false);
        out->recipe.scale      = (float)json_num(r, "scale", 1.0);
        out->recipe.offset     = (float)json_num(r, "offset", 0.0);

        const cJSON *names = cJSON_GetObjectItemCaseSensitive(r, "value_names");
        int n = 0;
        if (cJSON_IsArray(names)) {
            const cJSON *nm;
            cJSON_ArrayForEach(nm, names) {
                if (n >= MC_MAX_VALUES) break;
                if (cJSON_IsString(nm)) {
                    strncpy(out->recipe.value_names[n], nm->valuestring, MC_NAME_LEN - 1);
                    n++;
                }
            }
        }
        if (n == 0) { strcpy(out->recipe.value_names[0], "value"); n = 1; }
        out->recipe.value_count = n;
        if (out->recipe.length < 1) out->recipe.length = 1;
    } else {
        out->recipe.length = 1;
        out->recipe.scale = 1.0f;
        out->recipe.value_count = 1;
        strcpy(out->recipe.value_names[0], "value");
    }

    // Derived-value transform + per-sensor calibration blob (see sensor_transform.h).
    json_str(o, "transform", out->transform, sizeof(out->transform), "raw");
    if (out->transform[0] == '\0') strcpy(out->transform, "raw");

    // A grouped qre1113/tssp_ir sensor (channel_mask set) reports up to 2 values per selected
    // channel ("line_reflect"/"ir_ball" — reflect+detected / strength+detected), and every
    // sensor is capped at MC_MAX_VALUES total. Drop the extra channels from the *top* of the
    // mask (rather than silently truncating in the driver, which would leave a mismatch between
    // what's configured and what's actually read) so what's saved is what's actually read.
    if (out->channel_mask != 0 && (!strcmp(out->transform, "line_reflect") || !strcmp(out->transform, "ir_ball"))) {
        int max_channels = MC_MAX_VALUES / 2;
        int kept = 0;
        for (int ch = 0; ch < 8; ch++) {
            if (!(out->channel_mask & (1u << ch))) continue;
            if (kept >= max_channels) out->channel_mask &= ~(1u << ch);
            else kept++;
        }
    }

    const cJSON *cal = cJSON_GetObjectItemCaseSensitive(o, "calib");
    int cn = 0;
    if (cJSON_IsArray(cal)) {
        const cJSON *v;
        cJSON_ArrayForEach(v, cal) {
            if (cn >= MC_MAX_CALIB) break;
            if (cJSON_IsNumber(v)) out->calib[cn++] = v->valuedouble;
        }
    }
    out->calib_count = cn;

    // Learnable colour palette (colour sensors): [{name,out_id,learned,ref:[...]}].
    const cJSON *cols = cJSON_GetObjectItemCaseSensitive(o, "colours");
    int kn = 0;
    if (cJSON_IsArray(cols)) {
        const cJSON *ce;
        cJSON_ArrayForEach(ce, cols) {
            if (kn >= MC_MAX_COLOURS || !cJSON_IsObject(ce)) continue;
            colour_ref_t *c = &out->colours[kn];
            memset(c, 0, sizeof(*c));
            json_str(ce, "name", c->name, sizeof(c->name), "");
            c->out_id  = json_int(ce, "out_id", -1);
            c->learned = json_bool(ce, "learned", false);
            const cJSON *ref = cJSON_GetObjectItemCaseSensitive(ce, "ref");
            int rn = 0;
            if (cJSON_IsArray(ref)) {
                const cJSON *v;
                cJSON_ArrayForEach(v, ref) { if (rn >= MC_COL_CH) break; if (cJSON_IsNumber(v)) c->ref[rn++] = (float)v->valuedouble; }
            }
            kn++;
        }
    }
    out->colour_count = kn;

    out->colour_smooth   = (float)json_num(o, "colour_smooth", 0.0);
    out->colour_debounce = json_int(o, "colour_debounce", 0);
    out->knob_smooth     = (float)json_num(o, "knob_smooth", 0.0);
    out->knob_invert     = json_bool(o, "knob_invert", true);

    out->dist_mode   = (uint8_t)json_int(o, "dist_mode", 0);
    int led = json_int(o, "led", 0);
    out->led = (uint8_t)(led < 0 ? 0 : led > 100 ? 100 : led);
    out->led_sleep_s = json_int(o, "led_sleep_s", -1);
    if (out->led_sleep_s < -1) out->led_sleep_s = -1;
    out->dist_min_mm = (float)json_num(o, "dist_min_mm", 0.0);
    out->dist_max_mm = (float)json_num(o, "dist_max_mm", 0.0);

    // Clamp to this driver's realistic floor (hardware integration time + I2C overhead — see
    // sensor_poll_floor_ms()) so poll_ms can't be set faster than the sensor can actually
    // deliver fresh data.
    uint32_t floor_ms = sensor_poll_floor_ms(out);
    if (out->poll_ms < floor_ms) out->poll_ms = floor_ms;
    return true;
}

// ---- Direct string JSON writer for config_store_to_json() ------------------
// cJSON's tree (a heap node per key/value/array-element, plus a separately malloc'd string per
// string field) costs far more memory than the JSON text it represents — observed on-device: a
// 9-sensor store with taught colour palettes serialised to ~10 KB of JSON text but needed ~60 KB
// of tree nodes to get there, which is exactly the kind of transient spike that fails on a
// heap that's fragmented (plenty of *total* free heap, not enough *contiguous* free heap) rather
// than genuinely full. Writing the string directly, field by field, costs roughly the final text
// size and nothing more — no intermediate tree ever exists. This is only used for the one-way
// store→string direction (the read path still goes through cJSON_Parse(); a full hand-rolled
// parser would be a much larger, riskier change for comparatively little of the same benefit,
// since a request only needs a bounded number of known sensors read once, not the store's own
// full-fidelity round trip).
typedef struct { char *buf; size_t len, cap; } jw_t;

static bool jw_reserve(jw_t *w, size_t extra)
{
    if (!w->buf) return false;
    if (w->len + extra + 1 <= w->cap) return true;
    size_t cap = w->cap ? w->cap : 256;
    while (cap < w->len + extra + 1) cap *= 2;
    char *nb = realloc(w->buf, cap);
    if (!nb) { free(w->buf); w->buf = NULL; return false; }
    w->buf = nb;
    w->cap = cap;
    return true;
}
static void jw_raw(jw_t *w, const char *s)
{
    size_t n = strlen(s);
    if (!jw_reserve(w, n)) return;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}
static void jw_str(jw_t *w, const char *s)
{
    jw_raw(w, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = (char)*p; esc[2] = '\0'; jw_raw(w, esc); }
        else if (*p == '\n') jw_raw(w, "\\n");
        else if (*p < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *p); jw_raw(w, esc); }
        else { esc[0] = (char)*p; esc[1] = '\0'; jw_raw(w, esc); }
    }
    jw_raw(w, "\"");
}
static void jw_num(jw_t *w, double v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%.9g", v);
    jw_raw(w, buf);
}
static void jw_bool(jw_t *w, bool b) { jw_raw(w, b ? "true" : "false"); }

static void jw_sensor(jw_t *w, const sensor_cfg_t *s)
{
    jw_raw(w, "{\"id\":");        jw_num(w, s->id);
    jw_raw(w, ",\"name\":");      jw_str(w, s->name);
    jw_raw(w, ",\"type\":");      jw_str(w, s->type);
    jw_raw(w, ",\"bus\":");       jw_str(w, bus_to_str(s->bus));
    jw_raw(w, ",\"addr\":");      jw_num(w, s->addr);
    jw_raw(w, ",\"mux_addr\":");  jw_num(w, s->mux_addr);
    jw_raw(w, ",\"mux_channel\":"); jw_num(w, s->mux_channel);
    jw_raw(w, ",\"cs_index\":");  jw_num(w, s->cs_index);
    jw_raw(w, ",\"port\":");      jw_num(w, s->port);
    jw_raw(w, ",\"channel_mask\":"); jw_num(w, s->channel_mask);
    jw_raw(w, ",\"poll_ms\":");   jw_num(w, s->poll_ms);
    jw_raw(w, ",\"enabled\":");   jw_bool(w, s->enabled);
    jw_raw(w, ",\"simulate\":");  jw_bool(w, s->simulate);
    jw_raw(w, ",\"show\":");      jw_bool(w, s->show);
    jw_raw(w, ",\"page\":");      jw_num(w, s->page);
    jw_raw(w, ",\"value_mask\":"); jw_num(w, s->value_mask);

    jw_raw(w, ",\"recipe\":{\"reg\":");  jw_num(w, s->recipe.reg);
    jw_raw(w, ",\"length\":");           jw_num(w, s->recipe.length);
    jw_raw(w, ",\"byte_order\":");       jw_str(w, s->recipe.big_endian ? "be" : "le");
    jw_raw(w, ",\"signed\":");           jw_bool(w, s->recipe.is_signed);
    jw_raw(w, ",\"scale\":");            jw_num(w, s->recipe.scale);
    jw_raw(w, ",\"offset\":");           jw_num(w, s->recipe.offset);
    jw_raw(w, ",\"value_names\":[");
    for (int i = 0; i < s->recipe.value_count; i++) {
        if (i) jw_raw(w, ",");
        jw_str(w, s->recipe.value_names[i]);
    }
    jw_raw(w, "]}");

    jw_raw(w, ",\"transform\":"); jw_str(w, s->transform[0] ? s->transform : "raw");
    jw_raw(w, ",\"calib\":[");
    for (int i = 0; i < s->calib_count; i++) {
        if (i) jw_raw(w, ",");
        jw_num(w, s->calib[i]);
    }
    jw_raw(w, "]");

    if (s->colour_count > 0) {
        jw_raw(w, ",\"colours\":[");
        for (int i = 0; i < s->colour_count; i++) {
            if (i) jw_raw(w, ",");
            const colour_ref_t *c = &s->colours[i];
            jw_raw(w, "{\"name\":");  jw_str(w, c->name);
            jw_raw(w, ",\"out_id\":"); jw_num(w, c->out_id);
            jw_raw(w, ",\"learned\":"); jw_bool(w, c->learned);
            jw_raw(w, ",\"ref\":[");
            for (int k = 0; k < MC_COL_CH; k++) {
                if (k) jw_raw(w, ",");
                jw_num(w, c->ref[k]);
            }
            jw_raw(w, "]}");
        }
        jw_raw(w, "]");
    }
    jw_raw(w, ",\"dist_mode\":");       jw_num(w, s->dist_mode);
    jw_raw(w, ",\"led\":");             jw_num(w, s->led);
    jw_raw(w, ",\"led_sleep_s\":");     jw_num(w, s->led_sleep_s);
    jw_raw(w, ",\"dist_min_mm\":");     jw_num(w, s->dist_min_mm);
    jw_raw(w, ",\"dist_max_mm\":");     jw_num(w, s->dist_max_mm);
    jw_raw(w, ",\"colour_smooth\":");   jw_num(w, s->colour_smooth);
    jw_raw(w, ",\"colour_debounce\":"); jw_num(w, s->colour_debounce);
    jw_raw(w, ",\"knob_smooth\":");     jw_num(w, s->knob_smooth);
    jw_raw(w, ",\"knob_invert\":");     jw_bool(w, s->knob_invert);
    jw_raw(w, "}");
}

static void jw_display(jw_t *w, const display_cfg_t *d)
{
    jw_raw(w, "{\"enabled\":");   jw_bool(w, d->enabled);
    jw_raw(w, ",\"controller\":"); jw_str(w, d->controller);
    jw_raw(w, ",\"bus\":");       jw_str(w, bus_to_str(d->bus));
    jw_raw(w, ",\"cs\":");        jw_num(w, d->cs);
    jw_raw(w, ",\"dc\":");        jw_num(w, d->dc);
    jw_raw(w, ",\"rst\":");       jw_num(w, d->rst);
    jw_raw(w, ",\"bl\":");        jw_num(w, d->bl);
    jw_raw(w, ",\"addr\":");      jw_num(w, d->addr);
    jw_raw(w, ",\"width\":");     jw_num(w, d->width);
    jw_raw(w, ",\"height\":");    jw_num(w, d->height);
    jw_raw(w, ",\"x_gap\":");     jw_num(w, d->x_gap);
    jw_raw(w, ",\"y_gap\":");     jw_num(w, d->y_gap);
    jw_raw(w, ",\"mirror_x\":");  jw_bool(w, d->mirror_x);
    jw_raw(w, ",\"mirror_y\":");  jw_bool(w, d->mirror_y);
    jw_raw(w, ",\"invert\":");    jw_bool(w, d->invert);
    jw_raw(w, ",\"mode\":");      jw_str(w, d->mode);
    jw_raw(w, ",\"tiles_per_page\":"); jw_num(w, d->tiles_per_page);
    jw_raw(w, ",\"group_tiles\":");    jw_bool(w, d->group_tiles);
    jw_raw(w, ",\"sleep_after_s\":");  jw_num(w, d->sleep_after_s);
    jw_raw(w, ",\"show_boot_logo\":"); jw_bool(w, d->show_boot_logo);
    jw_raw(w, ",\"brightness\":");     jw_num(w, d->brightness);
    jw_raw(w, "}");
}

static void jw_lego(jw_t *w, const lego_cfg_t *l)
{
    jw_raw(w, "{\"enabled\":");   jw_bool(w, l->enabled);
    jw_raw(w, ",\"profile\":");   jw_num(w, l->profile);
    jw_raw(w, ",\"debug\":");     jw_bool(w, l->debug);
    jw_raw(w, ",\"events\":");    jw_bool(w, l->events);
    jw_raw(w, ",\"colour_source\":"); jw_num(w, l->colour_source);
    jw_raw(w, ",\"sensor_type\":"); jw_num(w, l->sensor_type);
    jw_raw(w, ",\"uart_port\":"); jw_num(w, l->uart_port);
    jw_raw(w, ",\"tx_gpio\":");   jw_num(w, l->tx_gpio);
    jw_raw(w, ",\"rx_gpio\":");   jw_num(w, l->rx_gpio);
    jw_raw(w, ",\"baud\":");      jw_num(w, l->baud);
    jw_raw(w, ",\"fields\":[");
    for (int i = 0; i < l->field_count; i++) {
        if (i) jw_raw(w, ",");
        const lego_field_t *f = &l->fields[i];
        jw_raw(w, "{\"sensor_id\":");   jw_num(w, f->sensor_id);
        jw_raw(w, ",\"value_index\":"); jw_num(w, f->value_index);
        jw_raw(w, ",\"bits\":");        jw_num(w, f->bits);
        jw_raw(w, ",\"signed\":");      jw_bool(w, f->is_signed);
        jw_raw(w, ",\"scale\":");       jw_num(w, f->scale);
        jw_raw(w, ",\"offset\":");      jw_num(w, f->offset);
        jw_raw(w, ",\"target\":");      jw_num(w, f->target);
        if (f->use_colour_map) {
            jw_raw(w, ",\"colour_map\":[");
            for (int k = 0; k < MC_LEGO_COLOUR_MAP_N; k++) {
                if (k) jw_raw(w, ",");
                jw_num(w, f->colour_map[k]);
            }
            jw_raw(w, "]");
        }
        jw_raw(w, "}");
    }
    jw_raw(w, "]}");
}

// Parse a sensors array into a temp buffer. Returns count, or -1 on error.
static int parse_array(const cJSON *arr, sensor_cfg_t *dst)
{
    if (!cJSON_IsArray(arr)) return -1;
    if (cJSON_GetArraySize(arr) > MC_MAX_SENSORS) return -1;
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!parse_sensor(it, &dst[n])) return -1;
        n++;
    }
    return n;
}

// ---- Display ---------------------------------------------------------------

static void seed_default_display(display_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    strcpy(d->controller, BOARD_TFT_CONTROLLER);   // always defined — see board_config.h's fallback
    strcpy(d->mode, "summary");
    d->bus = BUS_SPI;
    d->cs = -1; d->dc = -1; d->rst = -1; d->bl = -1;
    d->addr = 0x3C;
#if BOARD_HAS_DISPLAY
    d->enabled  = true;
    d->cs       = BOARD_TFT_CS_GPIO;
    d->dc       = BOARD_TFT_DC_GPIO;
    d->rst      = BOARD_TFT_RST_GPIO;
    d->bl       = BOARD_TFT_BL_GPIO;
    d->width    = BOARD_TFT_WIDTH;
    d->height   = BOARD_TFT_HEIGHT;
    d->x_gap    = BOARD_TFT_X_GAP;
    d->y_gap    = BOARD_TFT_Y_GAP;
    d->mirror_x = BOARD_TFT_MIRROR_X;
    d->mirror_y = BOARD_TFT_MIRROR_Y;
    d->invert   = BOARD_TFT_INVERT;
#endif
    d->sleep_after_s = -1;   // always on by default
    d->show_boot_logo = true;
    d->brightness = 100;      // full backlight by default (0-100%; PWM-dimmable panels only)
}

// Apply a (possibly partial) display object over the current config; unset fields keep
// their existing values, so a small update doesn't have to resend everything.
static void parse_display(const cJSON *o, display_cfg_t *d)
{
    if (!cJSON_IsObject(o)) return;
    d->enabled = json_bool(o, "enabled", d->enabled);
    json_str(o, "controller", d->controller, sizeof(d->controller), d->controller);
    const cJSON *busv = cJSON_GetObjectItemCaseSensitive(o, "bus");
    if (cJSON_IsString(busv)) d->bus = bus_from_str(busv->valuestring);
    d->cs       = json_int(o, "cs", d->cs);
    d->dc       = json_int(o, "dc", d->dc);
    d->rst      = json_int(o, "rst", d->rst);
    d->bl       = json_int(o, "bl", d->bl);
    d->addr     = (uint8_t)json_int(o, "addr", d->addr);
    d->width    = json_int(o, "width", d->width);
    d->height   = json_int(o, "height", d->height);
    d->x_gap    = json_int(o, "x_gap", d->x_gap);
    d->y_gap    = json_int(o, "y_gap", d->y_gap);
    d->mirror_x = json_bool(o, "mirror_x", d->mirror_x);
    d->mirror_y = json_bool(o, "mirror_y", d->mirror_y);
    d->invert   = json_bool(o, "invert", d->invert);
    json_str(o, "mode", d->mode, sizeof(d->mode), d->mode);
    d->tiles_per_page = json_int(o, "tiles_per_page", d->tiles_per_page);
    if (d->tiles_per_page != 1 && d->tiles_per_page != 2 && d->tiles_per_page != 4 && d->tiles_per_page != 8)
        d->tiles_per_page = 0;   // anything else (including unset/0) = auto
    d->group_tiles = json_bool(o, "group_tiles", d->group_tiles);
    d->sleep_after_s = json_int(o, "sleep_after_s", d->sleep_after_s);
    if (d->sleep_after_s < -1) d->sleep_after_s = -1;
    d->show_boot_logo = json_bool(o, "show_boot_logo", d->show_boot_logo);
    int bright = json_int(o, "brightness", d->brightness);
    d->brightness = (uint8_t)(bright < 0 ? 0 : bright > 100 ? 100 : bright);
}

// ---- LEGO emitter ----------------------------------------------------------

static void seed_default_lego(lego_cfg_t *l)
{
    memset(l, 0, sizeof(*l));
    l->enabled     = false;
    l->debug       = false;
    l->sensor_type = BOARD_LEGO_TYPE;
    l->uart_port   = BOARD_LEGO_UART_PORT;
    l->tx_gpio     = BOARD_LEGO_TX_GPIO;
    l->rx_gpio     = BOARD_LEGO_RX_GPIO;
    l->baud        = BOARD_LEGO_BAUD;
    l->field_count = 0;
}

// Apply a (possibly partial) lego object over the current config. Scalar fields keep
// their value when unset; a present "fields" array replaces the field list wholesale.
static void parse_lego(const cJSON *o, lego_cfg_t *l)
{
    if (!cJSON_IsObject(o)) return;
    l->enabled       = json_bool(o, "enabled", l->enabled);
    l->profile       = json_int(o, "profile", l->profile);
    l->debug         = json_bool(o, "debug", l->debug);
    l->events        = json_bool(o, "events", l->events);
    l->colour_source = json_int(o, "colour_source", l->colour_source);
    l->sensor_type   = (uint8_t)json_int(o, "sensor_type", l->sensor_type);
    l->uart_port   = json_int(o, "uart_port", l->uart_port);
    l->tx_gpio     = json_int(o, "tx_gpio", l->tx_gpio);
    l->rx_gpio     = json_int(o, "rx_gpio", l->rx_gpio);
    l->baud        = (uint32_t)json_int(o, "baud", l->baud);

    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(o, "fields");
    if (cJSON_IsArray(fields)) {
        int n = 0;
        const cJSON *fj;
        cJSON_ArrayForEach(fj, fields) {
            if (n >= MC_MAX_LEGO_FIELDS) break;
            if (!cJSON_IsObject(fj)) continue;
            lego_field_t *f = &l->fields[n];
            f->sensor_id   = json_int(fj, "sensor_id", 0);
            f->value_index = (uint8_t)json_int(fj, "value_index", 0);
            f->bits        = (uint8_t)json_int(fj, "bits", 16);
            f->is_signed   = json_bool(fj, "signed", false);
            f->scale       = json_num(fj, "scale", 1.0);
            f->offset      = json_num(fj, "offset", 0.0);
            f->target      = (uint8_t)json_int(fj, "target", LEGO_TARGET_RGBI);
            if (f->target > LEGO_TARGET_REFLT) f->target = LEGO_TARGET_RGBI;
            if (f->bits < 1 || f->bits > 16) f->bits = 16;   // any 1..16-bit width
            // Optional COLOR-target code→colour lookup: [ints], entry 255 = "no colour".
            f->use_colour_map = false;
            memset(f->colour_map, MC_LEGO_COLOUR_NONE, sizeof(f->colour_map));
            const cJSON *cmap = cJSON_GetObjectItemCaseSensitive(fj, "colour_map");
            if (cJSON_IsArray(cmap)) {
                int mi = 0;
                const cJSON *mv;
                cJSON_ArrayForEach(mv, cmap) {
                    if (mi >= MC_LEGO_COLOUR_MAP_N) break;
                    if (cJSON_IsNumber(mv)) f->colour_map[mi] = (uint8_t)mv->valueint;
                    mi++;
                }
                f->use_colour_map = mi > 0;
            }
            n++;
        }
        l->field_count = n;
    }
}

// ---- NVS -------------------------------------------------------------------

// Serialises the current in-memory config (fast, RAM-only) and hands it to persist_task for the
// actual flash write (slow — see the comment on s_persist_pending above). Returns as soon as the
// snapshot is queued, not once it's actually on flash: config_store_version() already bumps
// before this is called, so a concurrent get_config sees the new state immediately regardless of
// when the flash write lands, and a power-loss in the small window before it lands just means
// that one save is lost (same risk as before, an in-flight nvs_commit() being interrupted) rather
// than a new one introduced.
static esp_err_t nvs_write_current(void)
{
    char *json = config_store_to_json();
    if (!json) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    free(s_persist_pending);      // supersede any not-yet-written snapshot — only the latest matters
    s_persist_pending = json;
    xSemaphoreGive(s_persist_lock);
    xSemaphoreGive(s_persist_sem);
    return ESP_OK;
}

// Read the config JSON from NVS under the given key, accepting either the new blob or a legacy
// string (migration). Returns a malloc'd NUL-terminated buffer (caller frees), or NULL if absent.
static char *nvs_read_config_key(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_blob(h, key, NULL, &len) == ESP_OK && len > 0) {
        char *buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, key, buf, &len) == ESP_OK) { buf[len] = '\0'; return buf; }
        free(buf);
        return NULL;
    }
    if (nvs_get_str(h, key, NULL, &len) == ESP_OK && len > 0) {   // legacy string
        char *buf = malloc(len);
        if (buf && nvs_get_str(h, key, buf, &len) == ESP_OK) return buf;
        free(buf);
    }
    return NULL;
}

esp_err_t config_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_persist_lock = xSemaphoreCreateMutex();
    s_persist_sem = xSemaphoreCreateBinary();
    if (!s_persist_lock || !s_persist_sem) return ESP_ERR_NO_MEM;
    if (xTaskCreate(persist_task, "cfg_persist", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    seed_default_display(&s_display);   // board defaults; NVS (below) overrides
    seed_default_lego(&s_lego);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    bool key_present = false;   // NVS_KEY existed at all (distinguishes "corrupt" from "never written")
    bool loaded = false;       // some generation (primary or backup) parsed successfully
    if (err == ESP_OK) {
        char *buf = nvs_read_config_key(h, NVS_KEY);
        key_present = (buf != NULL);
        const char *source = "primary";
        if (!buf) {
            // Primary absent — fall back to the backup generation in case a prior write left
            // the primary key missing (e.g. very first persist was interrupted before commit).
            buf = nvs_read_config_key(h, NVS_KEY_BAK);
            source = "backup";
        }
        cJSON *root = buf ? cJSON_Parse(buf) : NULL;
        if (buf && !root) {
            // The blob exists but isn't valid JSON — do NOT treat this as "first boot". Log
            // loudly and try the backup generation before giving up.
            ESP_LOGE(TAG, "%s config blob failed to parse (%u bytes) — trying backup",
                     source, (unsigned)strlen(buf));
            free(buf);
            buf = nvs_read_config_key(h, NVS_KEY_BAK);
            source = "backup";
            root = buf ? cJSON_Parse(buf) : NULL;
            if (buf && !root)
                ESP_LOGE(TAG, "backup config blob also failed to parse (%u bytes) — config lost",
                         (unsigned)strlen(buf));
        }
        if (root) {
            s_version = (uint32_t)json_int(root, "version", 1);
            int n = parse_array(cJSON_GetObjectItemCaseSensitive(root, "sensors"), s_sensors);
            s_count = (n > 0) ? (size_t)n : 0;
            parse_display(cJSON_GetObjectItemCaseSensitive(root, "display"), &s_display);
            parse_lego(cJSON_GetObjectItemCaseSensitive(root, "lego"), &s_lego);
            s_polling_cap_us = (int64_t)json_num(root, "polling_cap_us", 20 * 1000);
            sensor_set_verbose_debug(json_bool(root, "verbose_debug", false));
            const cJSON *dn = cJSON_GetObjectItemCaseSensitive(root, "device_name");
            if (cJSON_IsString(dn) && dn->valuestring[0])
                strncpy(s_device_name, dn->valuestring, sizeof(s_device_name) - 1);
            cJSON_Delete(root);
            loaded = true;
            if (strcmp(source, "backup") == 0)
                ESP_LOGW(TAG, "recovered config from backup generation");
            ESP_LOGI(TAG, "loaded %u sensors, display=%s mode=%s (v%u)",
                     (unsigned)s_count, s_display.enabled ? "on" : "off", s_display.mode, (unsigned)s_version);
        }
        free(buf);
        nvs_close(h);
    }

    if (!loaded && !key_present) {
        // Genuinely first boot: neither key has ever been written. Safe to seed.
        ESP_LOGI(TAG, "seeding empty config");
        nvs_write_current();
    } else if (!loaded) {
        // A primary blob existed but neither it nor the backup would parse. Leave the in-RAM
        // config empty for this boot (so the device still works) but deliberately do NOT
        // persist over the corrupt blob — it's left on flash in case manual recovery/inspection
        // is needed, and this boot's empty state won't get written until the user next saves.
        ESP_LOGE(TAG, "config unrecoverable — starting with empty config, NOT overwriting flash");
    }
    return ESP_OK;
}

uint32_t config_store_version(void) { return s_version; }

void config_store_get(const sensor_cfg_t **arr, size_t *count)
{
    if (arr) *arr = s_sensors;
    if (count) *count = s_count;
}

void config_store_get_display(display_cfg_t *out)
{
    if (out) *out = s_display;
}

void config_store_get_lego(lego_cfg_t *out)
{
    if (out) *out = s_lego;
}

const char *config_store_get_device_name(void) { return s_device_name; }

esp_err_t config_store_set_device_name(const char *name)
{
    if (!name || !name[0] || strlen(name) >= sizeof(s_device_name)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(s_device_name, name, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';
    s_version++;
    esp_err_t err = nvs_write_current();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_device_name \"%s\" %s", s_device_name, esp_err_to_name(err));
    return err;
}

esp_err_t config_store_set_calib(int id, const double *calib, int n)
{
    if (!calib || n < 0) return ESP_ERR_INVALID_ARG;
    if (n > MC_MAX_CALIB) n = MC_MAX_CALIB;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < s_count; i++) {
        if (s_sensors[i].id == id) {
            for (int k = 0; k < n; k++) s_sensors[i].calib[k] = calib[k];
            s_sensors[i].calib_count = n;
            s_version++;
            err = nvs_write_current();
            break;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_calib id=%d n=%d %s", id, n, esp_err_to_name(err));
    return err;
}

esp_err_t config_store_set_colours(int id, const colour_ref_t *colours, int n)
{
    if (n < 0) n = 0;
    if (n > MC_MAX_COLOURS) n = MC_MAX_COLOURS;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < s_count; i++) {
        if (s_sensors[i].id == id) {
            for (int k = 0; k < n; k++) s_sensors[i].colours[k] = colours[k];
            s_sensors[i].colour_count = n;
            s_version++;
            err = nvs_write_current();
            break;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_colours id=%d n=%d %s", id, n, esp_err_to_name(err));
    return err;
}

// Snapshot one sensor's calib/colours as JSON array text (e.g. "[1.2,3.4]" / "[{...},...]") —
// used by calibrate/learn_colour/reset_colour/reset_sensor's responses so the web app can patch
// just that one sensor's captured data locally instead of doing a full get_config refetch after
// every single Teach/Calibrate click (that round trip, not the flash write itself — which is
// already async — was the actual source of the per-action delay). Caller frees both with free();
// either can come back NULL on a bad id or OOM, in which case the caller should omit that field
// from its response rather than send a wrong/empty one.
void config_store_get_calib_colours_json(int id, char **calib_json, char **colours_json)
{
    *calib_json = NULL;
    *colours_json = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        if (s_sensors[i].id != id) continue;
        const sensor_cfg_t *s = &s_sensors[i];

        jw_t cw = { .buf = malloc(256), .len = 0 };
        cw.cap = cw.buf ? 256 : 0;
        if (cw.buf) {
            jw_raw(&cw, "[");
            for (int k = 0; k < s->calib_count; k++) {
                if (k) jw_raw(&cw, ",");
                jw_num(&cw, s->calib[k]);
            }
            jw_raw(&cw, "]");
            *calib_json = cw.buf;
        }

        jw_t xw = { .buf = malloc(512), .len = 0 };
        xw.cap = xw.buf ? 512 : 0;
        if (xw.buf) {
            jw_raw(&xw, "[");
            for (int k = 0; k < s->colour_count; k++) {
                if (k) jw_raw(&xw, ",");
                const colour_ref_t *c = &s->colours[k];
                jw_raw(&xw, "{\"name\":");  jw_str(&xw, c->name);
                jw_raw(&xw, ",\"out_id\":"); jw_num(&xw, c->out_id);
                jw_raw(&xw, ",\"learned\":"); jw_bool(&xw, c->learned);
                jw_raw(&xw, ",\"ref\":[");
                for (int j = 0; j < MC_COL_CH; j++) {
                    if (j) jw_raw(&xw, ",");
                    jw_num(&xw, c->ref[j]);
                }
                jw_raw(&xw, "]}");
            }
            jw_raw(&xw, "]");
            *colours_json = xw.buf;
        }
        break;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t config_store_factory_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) { nvs_erase_key(h, NVS_KEY); nvs_commit(h); nvs_close(h); }
    // Reset in-RAM state to board defaults.
    s_count = 0;
    seed_default_display(&s_display);
    seed_default_lego(&s_lego);
    strncpy(s_device_name, BOARD_BLE_NAME, sizeof(s_device_name) - 1);
    s_polling_cap_us = 20 * 1000;  // reset to 50Hz default
    sensor_set_verbose_debug(false);
    s_version++;
    nvs_write_current();             // persist the empty/defaults seed
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "factory reset — config erased, defaults restored (v%u)", (unsigned)s_version);
    return ESP_OK;
}

char *config_store_to_json(void)
{
    // Direct string writer (see jw_* above/jw_sensor/jw_display/jw_lego) — no cJSON tree is ever
    // built for this, so peak memory here is roughly the final JSON text size instead of the
    // ~5-6x a full tree of nodes costs. That's what was actually failing before: cJSON parsing/
    // building a tree for a modest multi-sensor config could cost tens of KB on top of the
    // config's own ~10 KB of text, which a heap that's fragmented (plenty of *total* free heap,
    // not much *contiguous* free heap) has no room for even when it isn't remotely full.
    // Sized close to the observed real requirement (a 9-sensor config with taught AS7341
    // palettes measured at 12400 bytes total, ~1250/sensor) rather than either of the two
    // extremes already tried: 2048 + 1024/sensor (11264 bytes) undershot the *actual* output,
    // forcing a regrow on every single call; overcorrecting to 4096 + 2048/sensor (22528 bytes)
    // then demanded more contiguous space than was even available (largest free block measured
    // at 18432 bytes right after a save, well above the *real* 12400-byte need but well below
    // that overcorrected estimate) — asking for more than necessary just to "be safe" made it
    // fail *more* often on a heap where the constraint is contiguous space, not total free heap.
    // 1536 + 1250/sensor lands just above the measured real need with modest headroom, comfortably
    // under the fragmentation ceiling this session has actually shown, without demanding space
    // that was never going to be used.
    size_t want = 1536 + s_count * 1250;
    jw_t w = { .buf = malloc(want), .len = 0 };
    w.cap = w.buf ? want : 0;
    if (!w.buf) {
        ESP_LOGE(TAG, "to_json: initial malloc(%u) failed — free heap=%u largest_block=%u",
                 (unsigned)want, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return NULL;
    }

    jw_raw(&w, "{\"version\":");        jw_num(&w, s_version);
    jw_raw(&w, ",\"device_name\":");    jw_str(&w, s_device_name);
    jw_raw(&w, ",\"polling_cap_us\":"); jw_num(&w, (double)s_polling_cap_us);
    jw_raw(&w, ",\"verbose_debug\":");  jw_bool(&w, sensor_get_verbose_debug());
    jw_raw(&w, ",\"display\":");        jw_display(&w, &s_display);
    jw_raw(&w, ",\"lego\":");           jw_lego(&w, &s_lego);
    jw_raw(&w, ",\"sensors\":[");
    for (size_t i = 0; i < s_count; i++) {
        if (i) jw_raw(&w, ",");
        jw_sensor(&w, &s_sensors[i]);
    }
    jw_raw(&w, "]}");
    if (!w.buf) {
        ESP_LOGE(TAG, "to_json: grew past initial %u bytes and the regrow failed — free heap=%u largest_block=%u",
                 (unsigned)want, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    return w.buf;   // NULL if any reserve along the way failed (jw_reserve frees on OOM)
}

esp_err_t config_store_set_json(const char *config_json, uint32_t *out_version)
{
    // config_json is the full config object: {"sensors":[...], "display":{...}}.
    cJSON *root = cJSON_Parse(config_json);
    if (!root) return ESP_ERR_INVALID_ARG;

    // Parsing the request into cJSON's own tree already costs far more heap than the source
    // text (observed: a ~10.5 KB request ballooned to ~61 KB of tree nodes) — a heap that's
    // fragmented rather than actually full then has no large-enough contiguous block left for
    // an extra temp copy of the sensor array on top of that, however small. parse_sensor() below
    // never actually fails (it always returns true — the "invalid config" cases are only "not an
    // array" and "too many sensors", both checked here before touching anything), so the
    // heap-allocated tmp array + memcpy this used to go through wasn't buying any real
    // all-or-nothing safety, just an unconditional extra ~14 KB contiguous allocation on the
    // worst possible heap state. Parse straight into s_sensors instead — the lock below already
    // makes this atomic from any reader's point of view (they see either the whole old array or
    // the whole new one, never a partial write).
    cJSON *sensors_j = cJSON_GetObjectItemCaseSensitive(root, "sensors");
    if (!cJSON_IsArray(sensors_j) || cJSON_GetArraySize(sensors_j) > MC_MAX_SENSORS) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // A sensor object that omits "calib"/"colours" keeps whatever's already on the device for
    // that id, instead of parse_sensor() below defaulting it to empty — an ordinary Save from
    // the web (adding a sensor, flipping enabled, changing poll_ms, ...) never touches
    // calibration at all (every Teach/Calibrate/Reset action already writes straight to the
    // device on its own — see the calibrate/learn_colour/reset_colour/reset_sensor commands in
    // ble_protocol.c), so making it re-upload calibration/taught-colour data it already sent
    // moments earlier was pure redundant traffic — for a fully-taught multi-sensor config that's
    // the majority of the payload, and the actual cause of large saves timing out/dropping the
    // BLE connection mid-upload. Splicing in the current values here (reusing the same accessor
    // ble_protocol.c's add_calib_colours() already uses for calibrate/learn_colour responses)
    // means parse_sensor()/parse_array() below need no changes at all — every sensor object has
    // calib/colours one way or another by the time they run. Runs *before* the s_lock below:
    // config_store_get_calib_colours_json() takes that same lock itself, so calling it while
    // already holding it would deadlock.
    cJSON *sj;
    cJSON_ArrayForEach(sj, sensors_j) {
        if (!cJSON_IsObject(sj)) continue;
        bool has_calib = cJSON_HasObjectItem(sj, "calib");
        bool has_colours = cJSON_HasObjectItem(sj, "colours");
        if (has_calib && has_colours) continue;
        int id = json_int(sj, "id", -1);
        char *calib_json = NULL, *colours_json = NULL;
        config_store_get_calib_colours_json(id, &calib_json, &colours_json);
        // NOT cJSON_AddRawToObject() — that creates a cJSON_Raw node (verbatim text, meant only
        // for *printing* — see add_calib_colours() in ble_protocol.c, whose result is printed
        // straight back out and never re-parsed). parse_sensor() below reads this same tree
        // structurally (cJSON_IsArray()/cJSON_ArrayForEach()), which a Raw node fails silently —
        // not an error, just treated as "field absent", so calib_count/colour_count silently
        // came out 0 regardless of what was spliced in here. Re-parsing the text into a real
        // array node before attaching is what actually makes it visible to parse_sensor.
        if (!has_calib && calib_json) {
            cJSON *calib_tree = cJSON_Parse(calib_json);
            if (calib_tree) cJSON_AddItemToObject(sj, "calib", calib_tree);
        }
        if (!has_colours && colours_json) {
            cJSON *colours_tree = cJSON_Parse(colours_json);
            if (colours_tree) cJSON_AddItemToObject(sj, "colours", colours_tree);
        }
        free(calib_json);
        free(colours_json);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = parse_array(sensors_j, s_sensors);   // guaranteed >= 0 — already validated above
    if (n < 0) n = 0;                            // defensive only; parse_array can't actually reach this
    s_count = (size_t)n;
    parse_display(cJSON_GetObjectItemCaseSensitive(root, "display"), &s_display);
    parse_lego(cJSON_GetObjectItemCaseSensitive(root, "lego"), &s_lego);
    // Only touch the polling cap if the request actually included it — an ordinary Sensors/
    // Display/LEGO Save from the web never does (it's set via its own dedicated command), and
    // unconditionally defaulting here silently reset it to 50Hz on every unrelated save.
    if (cJSON_HasObjectItem(root, "polling_cap_us"))
        s_polling_cap_us = (int64_t)json_num(root, "polling_cap_us", 20 * 1000);
    s_version++;
    // Free the parse tree before serialising for NVS: nvs_write_current() builds its own full
    // cJSON tree of the store plus a contiguous printed string — holding the inbound tree alive
    // across that pushed peak heap over the edge for a fully-taught multi-sensor config
    // (observed as set_config applying to RAM but persisting with ESP_ERR_NO_MEM — the save then
    // vanished on reboot). Everything the store needs has already been copied into
    // s_sensors/s_display/s_lego above.
    cJSON_Delete(root);
    esp_err_t err = nvs_write_current();
    if (out_version) *out_version = s_version;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_config: %d sensors, display=%s (v%u) %s",
             n, s_display.enabled ? "on" : "off", (unsigned)s_version, esp_err_to_name(err));
    return err;
}

int64_t config_store_get_polling_cap_us(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t cap = s_polling_cap_us;
    xSemaphoreGive(s_lock);
    return cap;
}

esp_err_t config_store_set_polling_cap_us(int64_t us)
{
    if (us <= 0) us = 20 * 1000;  // default 50Hz
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_polling_cap_us = us;
    s_version++;
    esp_err_t err = nvs_write_current();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_polling_cap_us: %lld us (v%u) %s", (long long)us, (unsigned)s_version, esp_err_to_name(err));
    return err;
}

bool config_store_get_verbose_debug(void)
{
    return sensor_get_verbose_debug();
}

esp_err_t config_store_set_verbose_debug(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sensor_set_verbose_debug(on);
    s_version++;
    esp_err_t err = nvs_write_current();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_verbose_debug: %s (v%u) %s", on ? "on" : "off", (unsigned)s_version, esp_err_to_name(err));
    return err;
}

// Same shape as config_store_set_verbose_debug() above — mutate just these two fields and
// persist on their own, independent of the rest of the LEGO config's Save flow. Doesn't call
// lego_emit_apply() itself: config_store has no dependency on lego_emit (lego_emit depends on
// config_store, not the other way — see each component's CMakeLists.txt REQUIRES), matching how
// the full "set_config" BLE command already applies lego_emit_apply() itself after this store
// updates, rather than config_store doing it internally.
esp_err_t config_store_set_lego_debug(bool events, bool debug)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_lego.events = events;
    s_lego.debug = debug;
    s_version++;
    esp_err_t err = nvs_write_current();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_lego_debug: events=%s debug=%s (v%u) %s",
             events ? "on" : "off", debug ? "on" : "off", (unsigned)s_version, esp_err_to_name(err));
    return err;
}

ble_power_cfg_t config_store_get_ble_power(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ble_power_cfg_t cfg = s_ble_power;
    xSemaphoreGive(s_lock);
    return cfg;
}

esp_err_t config_store_set_ble_power(const ble_power_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ble_power = *cfg;
    s_version++;
    esp_err_t err = nvs_write_current();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "set_ble_power: conn %u-%ums tx %ddBm idle %us (v%u) %s",
             (unsigned)(s_ble_power.conn_itvl_min * 5 / 4),
             (unsigned)(s_ble_power.conn_itvl_max * 5 / 4),
             s_ble_power.tx_power,
             s_ble_power.idle_disconnect_s,
             (unsigned)s_version, esp_err_to_name(err));
    return err;
}
