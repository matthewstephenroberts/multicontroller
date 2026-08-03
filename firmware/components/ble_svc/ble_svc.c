// ble_svc.c — NimBLE peripheral: Nordic-UART-style GATT service + framed JSON transport.
#include "ble_svc.h"
#include "ble_internal.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "config_store.h"

static const char *TAG = "ble_svc";

// Max reassembled frame payload. Sized for the worst realistic set_config: MC_MAX_SENSORS
// sensors each carrying a full 16-colour taught palette (10 ref floats per colour) plus
// recipes/calib — that comfortably exceeded the old 8KB (observed as "rx overflow, reset" and
// a timed-out save once a few palettes were fully taught). The web also rounds ref/calib
// floats before saving to keep the payload well under this.
#define RX_BUF_MAX   24576
#define FRAME_HDR    4             // big-endian uint32 length prefix

// --- Nordic UART Service UUIDs (bytes are little-endian / LSB-first) ---
// Service 6e400001-b5a3-f393-e0a9-e50e24dcca9e, RX ...0002, TX ...0003
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);

static uint16_t s_tx_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type;
static bool     s_subscribed;
static bool     s_ble_enabled = false;  // disabled at boot; button hold enables it
static int64_t  s_notify_min_us = 20 * 1000;   // 50Hz cap by default

// RX reassembly buffer (single connection).
static uint8_t  s_rx[RX_BUF_MAX];
static size_t   s_rx_len;

// Commands are processed off the BLE host task: the GATT callback only reassembles
// frames and queues the JSON payload; a worker task runs the (potentially slow) handler
// so a bus scan never blocks the NimBLE stack.
static QueueHandle_t s_cmd_q;          // holds malloc'd char* JSON payloads

// Serialises send_framed() so a multi-chunk message is notified atomically. Without it a
// streaming `reading` event (scheduler task) can interleave its chunks with a command
// response (cmd_task), desyncing the host's length-prefixed reassembly and making saves
// intermittently fail.
static SemaphoreHandle_t s_tx_lock;

static void advertise(void);

// ---- subscription state (called from ble_protocol.c) ----
void ble_svc_set_subscribed(bool on) { s_subscribed = on; }
bool ble_svc_is_subscribed(void)     { return s_subscribed; }
bool ble_svc_is_connected(void)      { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

bool ble_svc_is_enabled(void)        { return s_ble_enabled; }

void ble_svc_set_notify_min_us(int64_t us) { s_notify_min_us = (us > 0) ? us : (20 * 1000); }
int64_t ble_svc_get_notify_min_us(void) { return s_notify_min_us; }

// ---- TX: frame a JSON string and notify in MTU-sized chunks ----
static void send_framed(const char *json)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !json) return;
    size_t jlen = strlen(json);
    size_t total = FRAME_HDR + jlen;

    uint8_t *frame = malloc(total);
    if (!frame) return;
    frame[0] = (jlen >> 24) & 0xFF;
    frame[1] = (jlen >> 16) & 0xFF;
    frame[2] = (jlen >> 8) & 0xFF;
    frame[3] = jlen & 0xFF;
    memcpy(frame + FRAME_HDR, json, jlen);

    uint16_t mtu = ble_att_mtu(s_conn);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;     // ATT notify overhead = 3
    size_t n_chunks = (total + chunk - 1) / chunk;
    int64_t t0 = esp_timer_get_time();
    ESP_LOGD(TAG, "send_framed: start len=%u chunks=%u", (unsigned)total, (unsigned)n_chunks);

    // Hold the TX lock for the whole message so its chunks are not interleaved with
    // another task's message (command responses vs. streamed reading events). Bounded, not
    // portMAX_DELAY: cmd_task is a single worker draining s_cmd_q sequentially, so if whoever
    // holds this lock is itself stuck deep inside a NimBLE call that never returns (observed on
    // real hardware as this connection's ATT server hitting BLE_HS_ENOMEM under heavy combined
    // reading-notify + command traffic, then going silent — no further "cmd=..." ever logged
    // again), an unbounded wait here means every future command silently stops being processed
    // forever, indistinguishable from a full device lockup. Giving up and skipping this one
    // send instead keeps cmd_task alive to drain whatever comes next.
    bool got_lock = !s_tx_lock || xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(3000)) == pdTRUE;
    if (!got_lock) {
        ESP_LOGW(TAG, "send_framed: TX lock busy 3s — dropping this message rather than blocking forever");
        free(frame);
        return;
    }
    for (size_t off = 0; off < total; off += chunk) {
        size_t n = (total - off < chunk) ? (total - off) : chunk;
        // A multi-chunk message queued back-to-back can outrun how fast the link can actually
        // drain it — each chunk only leaves over the air on its own connection event, but this
        // loop was previously firing all of them within single-digit milliseconds of each other
        // (observed: 7 chunks queued in ~5ms, 8th then hits BLE_HS_ENOMEM and stays stuck for
        // 8+ seconds — nowhere near "transient", just genuinely outrunning the drain rate).
        // Pacing every chunk gives the queue a chance to empty as we go, so exhaustion becomes
        // the exception instead of the routine case for any response past a couple of chunks.
        if (off > 0) vTaskDelay(pdMS_TO_TICKS(4));

        // Retry loop is still needed as a backstop for whatever exhaustion the pacing above
        // doesn't fully avoid (reading events + the LEGO UART emitter also compete for the same
        // pool) — a non-transient error (bad connection, etc.) still bails out immediately.
        // ble_hs_mbuf_from_flat() failing (NULL, out of mbufs to even build the request) is just
        // as much a pool-exhaustion signal as notify_custom() returning ENOMEM/EBUSY, and needs
        // the same retry/backoff treatment — a prior version of this loop bailed on the very
        // first failed allocation without ever retrying, which is why raising the pool size
        // alone didn't change anything: this path never actually re-attempted.
        int rc = -1;
        for (int attempt = 0; attempt < 15; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(frame + off, n);   // consumed by notify_custom either way
            if (om) {
                rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
                if (rc == 0 || (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY)) break;
            } else {
                rc = -1;
            }
            int backoff_ms = 10 * (attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms > 100 ? 100 : backoff_ms));
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "notify rc=%d (chunk %u/%u) after %lld ms — giving up on this message",
                     rc, (unsigned)off, (unsigned)total, (long long)((esp_timer_get_time() - t0) / 1000));
            if (s_tx_lock) xSemaphoreGive(s_tx_lock);
            free(frame);
            return;
        }
    }
    if (s_tx_lock) xSemaphoreGive(s_tx_lock);
    free(frame);
    ESP_LOGD(TAG, "send_framed: done len=%u chunks=%u in %lld ms",
             (unsigned)total, (unsigned)n_chunks, (long long)((esp_timer_get_time() - t0) / 1000));
}

// Reading events are throttled to this rate regardless of the sensor's own poll_ms — several
// sensors can be configured down to a 5-10ms poll floor (mcp3208/qre1113/gpio/adc), which is a
// fine internal sampling rate for the scheduler/LEGO emitter, but far exceeds what a single BLE
// connection can actually notify (realistically tens of notifies/sec at typical connection
// intervals/MTU). Streaming every single poll at those rates floods ble_gatts_notify_custom()
// faster than the link can drain, which starves the same mbuf pool the incoming command
// characteristic needs to receive anything at all — observed on real hardware as `ble_att_svr_pkt
// rc=6` (BLE_HS_ENOMEM) and a `stop`/`unsubscribe` write that never even reaches cmd_task (no
// `cmd=stop` ever logged), because the mbuf pool notify sends are hogging has nothing left to
// receive it with — followed by a supervision-timeout disconnect once the link is congested long
// enough. 50Hz is already far faster than a human can perceive on the dashboard, so nothing is
// lost in practice. This default can be lowered per-device via the web UI's polling cap control.
//
// The cap below is GLOBAL, not per-sensor: the web UI's own "polling cap" control presents a
// single Hz figure for the whole stream, and a per-sensor version of this (each sensor
// independently allowed up to the cap) multiplies actual BLE throughput by however many sensors
// are enabled — 5 sensors at the 50Hz default is 250 notifies/sec, right back in flood territory
// and exactly what starves the command channel above. Capping the aggregate stream instead keeps
// total notify traffic bounded no matter how many sensors are enabled, which is what actually
// protects the mbuf pool a `stop` command needs to land.
//
// A single shared "last send" timestamp alone is fair only between sensors polling at similar
// rates — first-attempt-after-cooldown-wins means a sensor polling every 10ms gets roughly 10x
// the attempts (and, statistically, 10x the wins) of one polling every 100ms in the same window,
// so a slow-polling sensor sharing the link with several fast ones could lose the race for the
// shared slot almost every time — not just "updates less often than the cap", but effectively
// never getting through at all. Observed as: a gamepad sensor at poll_ms=10 looks responsive,
// the same sensor at poll_ms=100 looks frozen, with nothing else about it changed.
//
// Fixed with a small per-sensor pending table: every reading always overwrites its sensor's own
// slot (only the latest value matters for a live dashboard), and whenever the global cooldown
// has elapsed, the slot that has gone LONGEST since it last actually sent — not whichever
// sensor's poll happened to call in at that instant — gets this turn. A rarely-polling sensor's
// slot ages further behind every time a faster sensor wins a turn, so it necessarily rises to
// the front and gets served within a bounded number of turns instead of being starved indefinitely.
typedef struct { int id; bool used; bool pending; int64_t last_sent_us; reading_t data; } pending_reading_t;
static pending_reading_t s_pending[MC_MAX_SENSORS];
static int64_t s_last_notify_us;

static pending_reading_t *pending_slot(int id)
{
    pending_reading_t *free_slot = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++) {
        if (s_pending[i].used && s_pending[i].id == id) return &s_pending[i];
        if (!s_pending[i].used && !free_slot) free_slot = &s_pending[i];
    }
    if (free_slot) *free_slot = (pending_reading_t){ .id = id, .used = true };
    return free_slot;   // NULL only if MC_MAX_SENSORS slots are already all in use — shouldn't happen
}

void ble_svc_on_reading(const reading_t *r)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) return;

    pending_reading_t *slot = pending_slot(r->id);
    if (slot) { slot->data = *r; slot->pending = true; }

    int64_t now = esp_timer_get_time();
    if (s_last_notify_us != 0 && now - s_last_notify_us < s_notify_min_us) return;   // global cap still closed

    // Among every sensor with a fresh, unsent reading, serve whichever has gone longest since
    // its own last actual send (never-sent sorts first via last_sent_us == 0).
    pending_reading_t *pick = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++) {
        if (!s_pending[i].used || !s_pending[i].pending) continue;
        if (!pick || s_pending[i].last_sent_us < pick->last_sent_us) pick = &s_pending[i];
    }
    // No tracked slot had anything pending — either this call's own slot allocation failed
    // (table full; shouldn't happen in practice, see pending_slot()'s comment) or genuinely
    // nothing is due. Either way, don't silently drop THIS reading: send it directly rather
    // than blocking on table exhaustion, matching the "table full — don't block on it"
    // fallback the old per-sensor version of this throttle already used.
    if (!pick) {
        char *ev = protocol_reading_event(r);
        if (ev) { send_framed(ev); free(ev); }
        s_last_notify_us = now;
        return;
    }

    char *ev = protocol_reading_event(&pick->data);
    if (ev) { send_framed(ev); free(ev); }
    pick->pending = false;
    pick->last_sent_us = now;
    s_last_notify_us = now;
}

void ble_svc_on_matrix(const uint16_t cells[9])
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    char *ev = protocol_matrix_event(cells);
    if (ev) { send_framed(ev); free(ev); }
}

void ble_svc_on_hid(bool connected, const char *name)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    char *ev = protocol_hid_event(connected, name);
    if (ev) { send_framed(ev); free(ev); }
}

// ---- RX: accumulate bytes, extract complete frames, dispatch ----
static void rx_reset(void) { s_rx_len = 0; }

static void rx_feed(const uint8_t *data, size_t len)
{
    if (s_rx_len + len > RX_BUF_MAX) { ESP_LOGW(TAG, "rx overflow, reset"); rx_reset(); return; }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;

    // Parse as many complete frames as are buffered.
    for (;;) {
        if (s_rx_len < FRAME_HDR) return;
        uint32_t plen = ((uint32_t)s_rx[0] << 24) | ((uint32_t)s_rx[1] << 16) |
                        ((uint32_t)s_rx[2] << 8)  | s_rx[3];
        if (plen > RX_BUF_MAX - FRAME_HDR) { ESP_LOGW(TAG, "bad len, reset"); rx_reset(); return; }
        if (s_rx_len < FRAME_HDR + plen) return;            // wait for the rest

        char *payload = malloc(plen + 1);
        if (payload) {
            memcpy(payload, s_rx + FRAME_HDR, plen);
            payload[plen] = '\0';
            // Hand off to the worker task; never run handlers on the host task.
            if (!s_cmd_q || xQueueSend(s_cmd_q, &payload, 0) != pdTRUE) {
                ESP_LOGW(TAG, "cmd queue full, dropping");
                free(payload);
            }
        }
        // Shift any trailing bytes (next frame) to the front.
        size_t consumed = FRAME_HDR + plen;
        memmove(s_rx, s_rx + consumed, s_rx_len - consumed);
        s_rx_len -= consumed;
    }
}

// Worker task: process queued commands off the BLE host task.
static void cmd_task(void *arg)
{
    (void)arg;
    char *payload;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &payload, portMAX_DELAY) != pdTRUE) continue;
        char *resp = protocol_handle(payload);
        free(payload);
        if (resp) { send_framed(resp); free(resp); }
    }
}

// ---- GATT access callbacks ----
static int gatt_rx_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t tmp[256];
        while (len > 0) {
            uint16_t n = len > sizeof(tmp) ? sizeof(tmp) : len;
            uint16_t got = 0;
            ble_hs_mbuf_to_flat(ctxt->om, tmp, n, &got);
            rx_feed(tmp, got);
            os_mbuf_adj(ctxt->om, got);
            len -= got;
            if (got == 0) break;
        }
    }
    return 0;
}

static int gatt_tx_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;   // notify-only
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &rx_uuid.u,
                .access_cb = gatt_rx_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &tx_uuid.u,
                .access_cb = gatt_tx_cb,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

// ---- GAP ----
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            rx_reset();
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn, &desc) == 0) {
                ESP_LOGI(TAG, "connected (conn=%d) interval=%ums latency=%u timeout=%ums",
                         s_conn, (unsigned)(desc.conn_itvl * 5 / 4), (unsigned)desc.conn_latency,
                         (unsigned)(desc.supervision_timeout * 10));
            } else {
                ESP_LOGI(TAG, "connected (conn=%d)", s_conn);
            }
            // Whatever interval the central proposed at connect time is often tuned for its own
            // power/latency defaults, not for this app's live sensor-streaming use case — some
            // stacks default to intervals far slower than the ~20ms our notify cap assumes,
            // which structurally caps how many notifies/sec can ever leave regardless of how
            // fast the app tries to send them (each notify only actually goes out once per
            // connection event). That looks identical to a resource leak from this side —
            // mbufs pile up waiting for a connection event that isn't coming often enough to
            // drain them — even though our own pacing/cap is well within budget. Request a fast
            // interval explicitly rather than hoping the default happens to be fast enough;
            // 15-30ms is broadly accepted by desktop/mobile/ChromeOS Bluetooth stacks alike.
            struct ble_gap_upd_params fast = {
                .itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400,
                .min_ce_len = 0, .max_ce_len = 0,
            };
            int rc = ble_gap_update_params(s_conn, &fast);
            if (rc != 0) ESP_LOGW(TAG, "connection interval update request failed rc=%d", rc);
        } else {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (event->conn_update.status == 0 && ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "connection interval updated: %ums latency=%u timeout=%ums",
                     (unsigned)(desc.conn_itvl * 5 / 4), (unsigned)desc.conn_latency,
                     (unsigned)(desc.supervision_timeout * 10));
        } else {
            ESP_LOGW(TAG, "connection interval update failed/rejected (status=%d)", event->conn_update.status);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason=%d, conn=%d)", event->disconnect.reason,
                 event->disconnect.conn.conn_handle);
        // A browser refresh/crash tears down its GATT connection without our peripheral ever
        // seeing an explicit teardown from the app — the physical link's own disconnect can
        // arrive well after the user has already reconnected (a fresh CONNECT event bumping
        // s_conn to the new handle). Without this check, that stale/delayed DISCONNECT for the
        // *old* connection unconditionally wiped s_conn back to NONE, so every subsequent
        // ble_svc_on_reading() call silently no-ops on `s_conn == BLE_HS_CONN_HANDLE_NONE` —
        // the new connection stays alive and answers commands fine (writes are handled per
        // connection regardless of s_conn), but never receives a single reading notification.
        // Only reset state when the disconnect actually belongs to the connection we're tracking.
        if (event->disconnect.conn.conn_handle == s_conn) {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_subscribed = false;
            rx_reset();
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "cccd: notify=%d", event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    if (!s_ble_enabled) return;

    // Read fresh every call (not cached) so a rename via config_store_set_device_name() takes
    // effect the next time advertising restarts — no separate "reload" path needed here.
    const char *name = config_store_get_device_name();

    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)name;
    adv.name_len = strlen(name);
    adv.name_is_complete = 1;
    adv.tx_pwr_lvl_is_present = 1;
    adv.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    if (ble_gap_adv_set_fields(&adv) != 0) ESP_LOGE(TAG, "adv_set_fields failed");

    // 128-bit service UUID goes in the scan response (won't fit alongside the name).
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGE(TAG, "rsp_set_fields failed");

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start rc=%d", rc);
    else ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

void ble_svc_set_enabled(bool enable)
{
    if (s_ble_enabled == enable) return;
    s_ble_enabled = enable;
    if (enable) {
        ESP_LOGI(TAG, "BLE enabled");
        advertise();
    } else {
        ESP_LOGI(TAG, "BLE disabled");
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_subscribed = false;
            rx_reset();
        }
        ble_gap_adv_stop();
    }
}

// Re-applies the current config_store device name to the GAP device-name characteristic —
// called right after a rename (ble_protocol.c's "set_device_name" command) so it's visible
// immediately to anything reading that characteristic, without waiting for a reconnect. The
// *advertised* name (what shows up in a scan) picks up the change on its own the next time
// advertise() runs, since it always reads the current name fresh (see above) rather than a
// value cached at ble_svc_init() time.
void ble_svc_refresh_device_name(void)
{
    ble_svc_gap_device_name_set(config_store_get_device_name());
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "nimble reset, reason=%d", reason); }

static void host_task(void *param)
{
    nimble_port_run();                 // returns on nimble_port_stop()
    nimble_port_freertos_deinit();
}

esp_err_t ble_svc_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) return ESP_ERR_NO_MEM;

    // Command worker: depth 4, payload pointers. Large stack — get_config/set_config and
    // the bus scan build full-config JSON with cJSON (recursive parse/print) and then notify
    // through NimBLE on this task; 6 KB overflowed once the lego config grew the payload.
    s_cmd_q = xQueueCreate(4, sizeof(char *));
    if (!s_cmd_q) return ESP_ERR_NO_MEM;
    if (xTaskCreate(cmd_task, "ble_cmd", 12288, NULL, 4, NULL) != pdPASS) return ESP_FAIL;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err)); return err; }

    // The NimBLE host itself logs every GATT procedure (e.g. "GATT procedure initiated: notify;
    // att_handle=18") at ESP_LOGI — harmless, but on a live-polling connection this fires once
    // per notify (tens of times/sec), drowning out this app's own log lines with no on/off
    // switch of its own. Not the same knob as sensor_get_verbose_debug() (that's this app's own
    // toggle for its own driver logs) — this is IDF/NimBLE's internal tag, silenced unconditionally
    // the same way bus_scan.c already silences "i2c.master" during a scan.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return ESP_FAIL; }

    ble_svc_refresh_device_name();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE started (advertising disabled until button hold)");
    return ESP_OK;
}
