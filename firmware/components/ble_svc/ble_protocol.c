// ble_protocol.c — JSON command dispatch (see docs/ble-protocol.md).
#include "ble_internal.h"
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "config_store.h"
#include "bus_scan.h"
#include "scheduler.h"
#include "lego_emit.h"
#include "hid_host.h"
#include "board_config.h"

static const char *TAG = "ble_proto";

static char *finish(cJSON *resp)
{
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out;
}

static char *err_resp(int id, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", id);
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    return finish(r);
}

// Minimal JSON string escaping for a value coming from outside our own config (a paired
// BLE-HID controller's advertised name) — used only for get_config's spliced-together response
// below, where there's no cJSON tree to lean on for this.
static void json_escape(const char *s, char *dst, size_t dstsize)
{
    size_t di = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && di + 2 < dstsize; p++) {
        if (*p == '"' || *p == '\\') { dst[di++] = '\\'; dst[di++] = (char)*p; }
        else if (*p >= 0x20) dst[di++] = (char)*p;
    }
    dst[di] = '\0';
}

// Hands the sensor's freshly-captured calib/colours back in a calibrate/learn_colour/
// reset_colour/reset_sensor response, so the web app can patch just that one sensor's data into
// its local state instead of doing a full get_config refetch after every single Teach/Calibrate
// click — that refetch (not the flash write itself, which is already async) was the actual
// source of the per-action delay. cJSON_AddRawToObject() copies the string internally, so it's
// safe to free our own buffers right after.
static void add_calib_colours(cJSON *r, int sensor_id)
{
    char *calib_json = NULL, *colours_json = NULL;
    config_store_get_calib_colours_json(sensor_id, &calib_json, &colours_json);
    if (calib_json) { cJSON_AddRawToObject(r, "calib", calib_json); free(calib_json); }
    if (colours_json) { cJSON_AddRawToObject(r, "colours", colours_json); free(colours_json); }
}

char *protocol_handle(const char *json_in)
{
    cJSON *root = cJSON_Parse(json_in);
    if (!root) return err_resp(0, "bad json");

    const cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *id_j  = cJSON_GetObjectItemCaseSensitive(root, "id");
    int id = cJSON_IsNumber(id_j) ? id_j->valueint : 0;
    // Copied into a local buffer, not just a `const char *` into root's own string storage: the
    // set_config branch below frees `root` early (to cut peak heap during the heavier work that
    // follows) and every branch is matched with plain strcmp(cmd, ...) throughout this function,
    // so cmd must stay valid after that free — including in the trailing ESP_LOGI below, which
    // was reading already-freed (and by then reused) memory for every set_config call, printing
    // garbage bytes instead of "set_config" in the serial log. That log line is diagnostic-only —
    // it doesn't affect the actual response, which is built from `id`/`ver` (never the freed
    // string) — but it's still a genuine use-after-free worth fixing outright.
    char cmd_buf[32];
    if (cJSON_IsString(cmd_j)) { strncpy(cmd_buf, cmd_j->valuestring, sizeof(cmd_buf) - 1); cmd_buf[sizeof(cmd_buf) - 1] = '\0'; }
    else cmd_buf[0] = '\0';
    const char *cmd = cmd_buf;

    char *out = NULL;

    if (!strcmp(cmd, "scan")) {
        // Same fix as get_config below: bus_scan_run_json() already returns a plain JSON array
        // string ("[...]") — splice it onto our own head instead of cJSON_Parse()-ing it back
        // into a tree purely to re-embed it in a new object that then gets printed again. A big
        // mux fan-out (multiple 8-channel muxes across both I2C buses) is nowhere near the size
        // that made this fail for set_config/get_config's much larger multi-sensor payloads, but
        // it's the exact same unnecessary round trip, so worth closing off for the same reason.
        char *devs = bus_scan_run_json();
        if (!devs) {
            out = err_resp(id, "scan failed (out of memory)");
        } else {
            char head[32];
            int head_len = snprintf(head, sizeof head, "{\"id\":%d,\"ok\":true,\"devices\":", id);
            size_t devs_len = strlen(devs);   // devs is "[...]"
            out = malloc((size_t)head_len + devs_len + 2);   // + closing '}' + NUL
            if (out) {
                memcpy(out, head, (size_t)head_len);
                memcpy(out + head_len, devs, devs_len);
                out[head_len + devs_len] = '}';
                out[head_len + devs_len + 1] = '\0';
            }
            free(devs);
        }

    } else if (!strcmp(cmd, "get_config")) {
        // config_store_to_json() already returns exactly {"version":...,"display":{...},
        // "lego":{...},"sensors":[...],...} — this used to get cJSON_Parse()'d right back into
        // a second tree purely to detach its pieces into a new response object, then printed
        // yet again by finish(). That reparse-rebuild-print round trip is the exact same
        // tree-node memory amplification config_store_to_json() itself used to suffer from
        // (see its own history) — for a large multi-sensor config it could fail outright and
        // silently produce an empty/near-empty response (fell back to `sensors: []`, reading as
        // "no sensors" in the web app even though the store still has all of them). Splice our
        // own extra fields onto cfg's own JSON text by hand instead: cfg is always
        // `{"version":...}` with a single object at top level, so replacing its opening `{`
        // with our own `{...our fields...,` produces one valid combined object with no parsing
        // of it needed at all.
        char *cfg = config_store_to_json();
        if (!cfg) {
            out = err_resp(id, "config too large to read back right now (out of memory) — try again shortly");
        } else {
            char name_esc[64];
            json_escape(hid_host_name(), name_esc, sizeof name_esc);
            char board_name_esc[64];
            json_escape(BOARD_NAME, board_name_esc, sizeof board_name_esc);
            // has_uart: derived from pin validity rather than a new opt-in board macro, same
            // spirit as BOARD_SPI_CS_COUNT already being 0 on boards with no CS lines wired —
            // every board today defines real BOARD_UART_TX/RX_GPIO, so this is true everywhere
            // right now, but a future board setting either to -1 (no aux UART broken out) is
            // automatically reported correctly with no other change needed.
            const char *has_uart = (BOARD_UART_TX_GPIO >= 0 && BOARD_UART_RX_GPIO >= 0) ? "true" : "false";
            char head[512];
            int head_len = snprintf(head, sizeof head,
                "{\"id\":%d,\"ok\":true,\"hid\":{\"connected\":%s,\"name\":\"%s\"},\"persist_ok\":%s,"
                "\"board\":{\"name\":\"%s\",\"spi_cs_count\":%d,\"has_uart\":%s,\"has_display\":%s,\"tft_controller\":\"%s\","
                "\"tft_cs\":%d,\"tft_dc\":%d,\"tft_rst\":%d,\"tft_bl\":%d},",
                id, hid_host_is_connected() ? "true" : "false", name_esc,
                config_store_last_persist_ok() ? "true" : "false",
                board_name_esc, BOARD_SPI_CS_COUNT, has_uart, BOARD_HAS_DISPLAY ? "true" : "false",
                BOARD_TFT_CONTROLLER, BOARD_TFT_CS_GPIO, BOARD_TFT_DC_GPIO, BOARD_TFT_RST_GPIO, BOARD_TFT_BL_GPIO);
            size_t cfg_len = strlen(cfg);   // cfg[0] is '{' — skipped below, our own head supplies it
            out = malloc((size_t)head_len + cfg_len);
            if (out) {
                memcpy(out, head, (size_t)head_len);
                memcpy(out + head_len, cfg + 1, cfg_len);   // cfg_len bytes: (cfg_len-1) body + cfg's NUL
            }
            free(cfg);
        }

    } else if (!strcmp(cmd, "set_config")) {
        cJSON *sensors = cJSON_GetObjectItemCaseSensitive(root, "sensors");
        if (!cJSON_IsArray(sensors)) {
            out = err_resp(id, "sensors must be an array");
        } else {
            // config_store_set_json() only ever reads the "sensors"/"display"/"lego" keys by
            // name — the extra "cmd"/"id" keys in json_in are harmless noise to it — so it can
            // parse json_in directly instead of this handler re-wrapping/re-serialising a
            // {sensors, display, lego} subset into a fresh string first. That round trip used to
            // hold `root` (the full parsed request) AND a full re-print of it in a second buffer
            // alive at the same time, right before config_store_set_json() parses yet a THIRD
            // copy internally — for a fully-taught multi-sensor config that's tens of KB, three
            // copies alive briefly at once was enough to make cJSON_PrintUnformatted() itself run
            // out of memory (observed as a misleading "invalid config", when nothing about the
            // config was actually invalid). Dropping the middle copy roughly halves that peak.
            //
            // json_in is owned by the caller (ble_svc.c's cmd_task frees it only after this
            // function returns), so it's safe to use here even after root (parsed *from* it) is
            // freed below — freeing root doesn't touch the separate json_in string at all.
            cJSON_Delete(root);
            root = NULL;
            uint32_t ver = 0;
            esp_err_t e = config_store_set_json(json_in, &ver);
            if (e == ESP_ERR_NO_MEM) {
                // The config parsed and was applied to RAM, but serialising it for the flash
                // write ran out of memory — it will NOT survive a reboot. Distinct from
                // "invalid config", which used to mask this failure mode.
                out = err_resp(id, "config applied but NOT saved to flash (out of memory) — it will be lost on reboot; retry, or reduce the config size");
            } else if (e != ESP_OK) {
                out = err_resp(id, "invalid config");
            } else {
                scheduler_rebuild();
                lego_emit_apply();      // restart/stop the emitter if its config changed
                cJSON *r = cJSON_CreateObject();
                cJSON_AddNumberToObject(r, "id", id);
                cJSON_AddBoolToObject(r, "ok", true);
                cJSON_AddNumberToObject(r, "version", ver);
                out = finish(r);
            }
        }

    } else if (!strcmp(cmd, "calibrate")) {
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(root, "sensor_id");
        const cJSON *pj = cJSON_GetObjectItemCaseSensitive(root, "point");
        int sid = cJSON_IsNumber(sj) ? sj->valueint : -1;
        const char *point = cJSON_IsString(pj) ? pj->valuestring : NULL;
        esp_err_t e = scheduler_calibrate(sid, point);
        if (e != ESP_OK) {
            out = err_resp(id, "calibrate failed — no reading yet, mode has no calibration, or the capture was saturated (a channel at full scale): lower LED brightness / move the target further away and retry");
        } else {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "id", id);
            cJSON_AddBoolToObject(r, "ok", true);
            cJSON_AddNumberToObject(r, "version", config_store_version());
            add_calib_colours(r, sid);
            out = finish(r);
        }

    } else if (!strcmp(cmd, "start") || !strcmp(cmd, "stop")) {
        if (cmd[1] == 't') scheduler_start(); else scheduler_stop();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        out = finish(r);

    } else if (!strcmp(cmd, "subscribe") || !strcmp(cmd, "unsubscribe")) {
        ble_svc_set_subscribed(cmd[0] == 's');
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        out = finish(r);

    } else if (!strcmp(cmd, "set_polling_cap")) {
        const cJSON *hz_j = cJSON_GetObjectItemCaseSensitive(root, "cap_hz");
        double cap_hz = cJSON_IsNumber(hz_j) ? hz_j->valuedouble : 0;
        int64_t cap_us = (cap_hz > 0) ? (int64_t)(1000000.0 / cap_hz) : (20 * 1000);  // 0 or invalid defaults to 50Hz
        config_store_set_polling_cap_us(cap_us);
        ble_svc_set_notify_min_us(cap_us);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddNumberToObject(r, "version", config_store_version());
        out = finish(r);

    } else if (!strcmp(cmd, "set_verbose_debug")) {
        const cJSON *ej = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        config_store_set_verbose_debug(cJSON_IsTrue(ej));
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "verbose_debug", config_store_get_verbose_debug());
        cJSON_AddNumberToObject(r, "version", config_store_version());
        out = finish(r);

    } else if (!strcmp(cmd, "set_lego_debug")) {
        // Same instant-apply shape as set_verbose_debug above — the LEGO emitter's two log
        // toggles used to only take effect via the full LEGO tab "Save to device" flow, unlike
        // every other device debug toggle. lego_emit_apply() re-reads config_store's LEGO
        // config itself, so the running emitter picks up the new verbosity immediately.
        const cJSON *evj = cJSON_GetObjectItemCaseSensitive(root, "events");
        const cJSON *dgj = cJSON_GetObjectItemCaseSensitive(root, "debug");
        config_store_set_lego_debug(cJSON_IsTrue(evj), cJSON_IsTrue(dgj));
        lego_emit_apply();
        lego_cfg_t lego;
        config_store_get_lego(&lego);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "events", lego.events);
        cJSON_AddBoolToObject(r, "debug", lego.debug);
        cJSON_AddNumberToObject(r, "version", config_store_version());
        out = finish(r);

    } else if (!strcmp(cmd, "learn_colour")) {
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(root, "sensor_id");
        const cJSON *nj = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *oj = cJSON_GetObjectItemCaseSensitive(root, "out_id");
        int sid = cJSON_IsNumber(sj) ? sj->valueint : -1;
        const char *name = cJSON_IsString(nj) ? nj->valuestring : "";
        int out_id = cJSON_IsNumber(oj) ? oj->valueint : -1;
        esp_err_t e = scheduler_learn_colour(sid, name, out_id);
        if (e != ESP_OK) out = err_resp(id, "learn_colour failed — no reading, not a colour sensor, palette full, capture saturated (lower LED / more distance), or white calibration not done yet (Calibrate before Teach)");
        else {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "id", id);
            cJSON_AddBoolToObject(r, "ok", true);
            cJSON_AddNumberToObject(r, "version", config_store_version());
            add_calib_colours(r, sid);
            out = finish(r);
        }

    } else if (!strcmp(cmd, "reset_colour")) {
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(root, "sensor_id");
        const cJSON *nj = cJSON_GetObjectItemCaseSensitive(root, "name");
        int sid = cJSON_IsNumber(sj) ? sj->valueint : -1;
        const char *name = cJSON_IsString(nj) ? nj->valuestring : "";
        esp_err_t e = scheduler_reset_colour(sid, name);
        if (e != ESP_OK) out = err_resp(id, "reset_colour failed");
        else {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "id", id);
            cJSON_AddBoolToObject(r, "ok", true);
            cJSON_AddNumberToObject(r, "version", config_store_version());
            add_calib_colours(r, sid);
            out = finish(r);
        }

    } else if (!strcmp(cmd, "reset_sensor")) {
        // Per-sensor factory reset: wipe this sensor's calibration + taught colour palette
        // (its type/bus/pins/transform config stays — use set_config or Remove for those).
        // Cheap alternative to a full set_config round-trip just to clear captured data.
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(root, "sensor_id");
        int sid = cJSON_IsNumber(sj) ? sj->valueint : -1;
        double none = 0;
        esp_err_t e = config_store_set_calib(sid, &none, 0);
        if (e == ESP_OK) e = config_store_set_colours(sid, NULL, 0);
        if (e != ESP_OK) {
            out = err_resp(id, "reset_sensor failed — unknown sensor id");
        } else {
            scheduler_rebuild();
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "id", id);
            cJSON_AddBoolToObject(r, "ok", true);
            cJSON_AddNumberToObject(r, "version", config_store_version());
            add_calib_colours(r, sid);
            out = finish(r);
        }

    } else if (!strcmp(cmd, "factory_reset")) {
        config_store_factory_reset();
        ble_svc_refresh_device_name();
        scheduler_rebuild();
        lego_emit_apply();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddNumberToObject(r, "version", config_store_version());
        out = finish(r);

    } else if (!strcmp(cmd, "set_device_name")) {
        const cJSON *nj = cJSON_GetObjectItemCaseSensitive(root, "name");
        const char *name = cJSON_IsString(nj) ? nj->valuestring : "";
        esp_err_t e = config_store_set_device_name(name);
        if (e != ESP_OK) {
            out = err_resp(id, "invalid name (must be 1-19 characters)");
        } else {
            ble_svc_refresh_device_name();
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "id", id);
            cJSON_AddBoolToObject(r, "ok", true);
            cJSON_AddStringToObject(r, "device_name", config_store_get_device_name());
            cJSON_AddNumberToObject(r, "version", config_store_version());
            out = finish(r);
        }

    } else if (!strcmp(cmd, "hid_scan") || !strcmp(cmd, "hid_forget")) {
        if (cmd[4] == 's') hid_host_scan(); else hid_host_forget();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "connected", hid_host_is_connected());
        out = finish(r);

    } else if (!strcmp(cmd, "hid_virtual")) {
        const cJSON *ej = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        hid_host_set_virtual_enabled(cJSON_IsTrue(ej));
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "virtual", hid_host_virtual_enabled());
        out = finish(r);

    } else if (!strcmp(cmd, "hid_set_state")) {
        gamepad_state_t st = {0};
        const cJSON *j;
        j = cJSON_GetObjectItemCaseSensitive(root, "buttons"); st.buttons = cJSON_IsNumber(j) ? (uint16_t)j->valueint : 0;
        j = cJSON_GetObjectItemCaseSensitive(root, "lx");      st.lx      = cJSON_IsNumber(j) ? (int16_t)j->valueint  : 0;
        j = cJSON_GetObjectItemCaseSensitive(root, "ly");      st.ly      = cJSON_IsNumber(j) ? (int16_t)j->valueint  : 0;
        j = cJSON_GetObjectItemCaseSensitive(root, "rx");      st.rx      = cJSON_IsNumber(j) ? (int16_t)j->valueint  : 0;
        j = cJSON_GetObjectItemCaseSensitive(root, "ry");      st.ry      = cJSON_IsNumber(j) ? (int16_t)j->valueint  : 0;
        j = cJSON_GetObjectItemCaseSensitive(root, "lt");      st.lt      = cJSON_IsNumber(j) ? (uint16_t)j->valueint : 0;
        j = cJSON_GetObjectItemCaseSensitive(root, "rt");      st.rt      = cJSON_IsNumber(j) ? (uint16_t)j->valueint : 0;
        j = cJSON_GetObjectItemCaseSensitive(root, "dpad");    st.dpad    = cJSON_IsNumber(j) ? (uint8_t)j->valueint  : 0;
        hid_host_set_virtual_state(&st);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", id);
        cJSON_AddBoolToObject(r, "ok", true);
        out = finish(r);

    } else {
        out = err_resp(id, "unknown cmd");
    }

    ESP_LOGI(TAG, "cmd=%s id=%d", cmd, id);
    cJSON_Delete(root);
    return out;
}

char *protocol_reading_event(const reading_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "reading");
    cJSON_AddNumberToObject(o, "sensor", r->id);
    cJSON_AddNumberToObject(o, "ts", (double)r->ts_ms);
    cJSON *vals = cJSON_AddArrayToObject(o, "values");
    for (int i = 0; i < r->count; i++)
        cJSON_AddItemToArray(vals, cJSON_CreateNumber(r->values[i]));
    cJSON_AddStringToObject(o, "status", r->status ? r->status : "ok");
    return finish(o);
}

// 3×3 Light Matrix pixels pushed by the hub → frontend virtual grid. Each cell is RGB565;
// emit as "#rrggbb" so the web side can render directly.
char *protocol_matrix_event(const uint16_t cells[9])
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "lego_matrix");
    cJSON *px = cJSON_AddArrayToObject(o, "pixels");
    for (int i = 0; i < 9; i++) {
        uint16_t c = cells[i];
        unsigned r = ((c >> 11) & 0x1F) * 255 / 31;
        unsigned g = ((c >> 5)  & 0x3F) * 255 / 63;
        unsigned b = (c & 0x1F) * 255 / 31;
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
        cJSON_AddItemToArray(px, cJSON_CreateString(hex));
    }
    return finish(o);
}

// BLE-HID controller connection status → frontend (Gamepad card).
char *protocol_hid_event(bool connected, const char *name)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "hid");
    cJSON_AddBoolToObject(o, "connected", connected);
    cJSON_AddStringToObject(o, "name", name ? name : "");
    return finish(o);
}
