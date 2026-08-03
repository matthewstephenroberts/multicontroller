// hid_host.c — NimBLE central / HID-over-GATT host for a BLE game controller (Xbox Series).
//
// Flow: scan → connect → bond (LESC, persisted) → discover the HID service (0x1812) →
// set Report protocol mode → enable notifications on every CCCD → decode input reports into
// a gamepad_state_t. Shares the NimBLE host started by ble_svc_init(); uses its own GAP/GATT
// callbacks and conn handle, independent of the peripheral (web-app) link.

#include "hid_host.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "debug_flag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"

extern void ble_store_config_init(void);   // NVS-backed bond store (CONFIG_BT_NIMBLE_NVS_PERSIST)

static const char *TAG = "hid_host";

// HOGP / GATT UUIDs (16-bit).
#define UUID_HID_SVC       0x1812
#define UUID_REPORT        0x2A4D
#define UUID_PROTO_MODE    0x2A4E
#define UUID_CCCD          0x2902

static uint8_t  s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool     s_auto;                       // keep (re)scanning to reconnect a known pad
static bool     s_subscribed;
static uint16_t s_proto_mode_handle;
static uint16_t s_hid_start_handle, s_hid_end_handle;   // HID service's own handle range
static char     s_name[32];
static int      s_report_dumps;               // hex-dump budget for the first reports after connect

static gamepad_state_t s_state;
static gamepad_state_t s_virtual_state;
static bool            s_virtual_enabled;
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;
static void (*s_status_cb)(bool, const char *);

static void start_scan(void);

// ── State publish ───────────────────────────────────────────────────────────
static void publish(const gamepad_state_t *st)
{
    portENTER_CRITICAL(&s_mux);
    s_state = *st;
    portEXIT_CRITICAL(&s_mux);
}

bool hid_host_get_state(gamepad_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    bool virt = s_virtual_enabled;
    *out = virt ? s_virtual_state : s_state;
    portEXIT_CRITICAL(&s_mux);
    return virt || out->connected || s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void hid_host_set_virtual_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_mux);
    s_virtual_enabled = enabled;
    if (enabled) s_virtual_state.connected = true;
    portEXIT_CRITICAL(&s_mux);
}

bool hid_host_virtual_enabled(void)
{
    portENTER_CRITICAL(&s_mux);
    bool v = s_virtual_enabled;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void hid_host_set_virtual_state(const gamepad_state_t *state)
{
    portENTER_CRITICAL(&s_mux);
    s_virtual_state = *state;
    s_virtual_state.connected = true;
    portEXIT_CRITICAL(&s_mux);
}

bool hid_host_is_connected(void) { return s_subscribed; }
const char *hid_host_name(void) { return s_name[0] ? s_name : "controller"; }
void hid_host_set_status_cb(void (*cb)(bool, const char *)) { s_status_cb = cb; }

static void notify_status(bool connected)
{
    if (s_status_cb) s_status_cb(connected, s_name[0] ? s_name : "controller");
}

// ── Xbox Series BLE input report (16 bytes) → normalised state ───────────────
// Layout per the xpadneo / Linux hid-microsoft mapping; tweak here if a controller's report
// differs (use the web live-viz to confirm bit positions).
static void parse_report(const uint8_t *d, int n)
{
    if (n < 15) return;
    gamepad_state_t st = {0};
    st.connected = true;
    st.lx = (int16_t)((uint16_t)(d[0] | (d[1] << 8)) - 32768);
    st.ly = (int16_t)((uint16_t)(d[2] | (d[3] << 8)) - 32768);
    st.rx = (int16_t)((uint16_t)(d[4] | (d[5] << 8)) - 32768);
    st.ry = (int16_t)((uint16_t)(d[6] | (d[7] << 8)) - 32768);
    st.lt = (uint16_t)(d[8]  | (d[9]  << 8)) & 0x03FF;
    st.rt = (uint16_t)(d[10] | (d[11] << 8)) & 0x03FF;
    st.dpad = d[12];

    uint16_t b = 0;
    uint8_t b0 = d[13], b1 = (n > 14) ? d[14] : 0, b2 = (n > 15) ? d[15] : 0;
    if (b0 & 0x01) b |= HID_BTN_A;
    if (b0 & 0x02) b |= HID_BTN_B;
    if (b0 & 0x08) b |= HID_BTN_X;
    if (b0 & 0x10) b |= HID_BTN_Y;
    if (b0 & 0x40) b |= HID_BTN_LB;
    if (b0 & 0x80) b |= HID_BTN_RB;
    if (b1 & 0x04) b |= HID_BTN_VIEW;
    if (b1 & 0x08) b |= HID_BTN_MENU;
    if (b1 & 0x20) b |= HID_BTN_LS;
    if (b1 & 0x40) b |= HID_BTN_RS;
    if (b2 & 0x01) b |= HID_BTN_XBOX;
    if (b2 & 0x02) b |= HID_BTN_SHARE;
    // Fold the 8-way hat into direction bits.
    switch (st.dpad) {
        case 1: b |= HID_BTN_DUP; break;
        case 2: b |= HID_BTN_DUP | HID_BTN_DRIGHT; break;
        case 3: b |= HID_BTN_DRIGHT; break;
        case 4: b |= HID_BTN_DDOWN | HID_BTN_DRIGHT; break;
        case 5: b |= HID_BTN_DDOWN; break;
        case 6: b |= HID_BTN_DDOWN | HID_BTN_DLEFT; break;
        case 7: b |= HID_BTN_DLEFT; break;
        case 8: b |= HID_BTN_DUP | HID_BTN_DLEFT; break;
        default: break;
    }
    st.buttons = b;
    publish(&st);
}

// ── GATT discovery ───────────────────────────────────────────────────────────
static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_def_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0 && dsc &&
        ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) {
        uint8_t val[2] = {0x01, 0x00};                      // enable notifications
        ble_gattc_write_flat(conn, dsc->handle, val, sizeof(val), NULL, NULL);
        s_subscribed = true;
    }
    if (err->status == BLE_HS_EDONE && s_subscribed) {
        ESP_LOGI(TAG, "subscribed to HID notifications");
        notify_status(true);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        uint16_t u = ble_uuid_u16(&chr->uuid.u);
        if (u == UUID_PROTO_MODE) s_proto_mode_handle = chr->val_handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (s_proto_mode_handle) {                          // force Report protocol (not Boot)
            uint8_t report = 0x01;
            ble_gattc_write_flat(conn, s_proto_mode_handle, &report, 1, NULL, NULL);
        }
        // Enable notifications on every CCCD within the HID service's own handle range (input
        // report, etc.) — scoped the same way characteristic discovery above was (see on_svc),
        // not the whole GATT database.
        ble_gattc_disc_all_dscs(conn, s_hid_start_handle, s_hid_end_handle, on_dsc, NULL);
    }
    return 0;
}

// Look up the HID service's own start/end handle before discovering anything inside it — a
// real gamepad's GATT database commonly also carries Device Information, Battery, and
// vendor-specific services alongside HID, and discovering characteristics/descriptors across
// the ENTIRE handle range (1-0xffff, as this used to) means paying for round trips through all
// of those unrelated services too. Scoping both discovery passes to just the HID service's own
// range cuts that overhead — a real, measurable slice of "why does pairing take a while".
static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        s_hid_start_handle = svc->start_handle;
        s_hid_end_handle = svc->end_handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (s_hid_start_handle && s_hid_end_handle) {
            ble_gattc_disc_all_chrs(conn, s_hid_start_handle, s_hid_end_handle, on_chr, NULL);
        } else {
            // adv_is_gamepad() already required the HID service UUID in the advertisement
            // before we ever connected — landing here means the advertised UUID didn't actually
            // resolve to a real service in this device's GATT database (a malformed/unusual
            // advertiser). Disconnect so DISCONNECT fires and auto-reconnect gets a real retry,
            // matching the encryption-failure handling below, rather than stranding a connected-
            // but-useless link with no HID data and no retry.
            ESP_LOGW(TAG, "HID service not found in GATT database — disconnecting to retry");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

// ── GAP ──────────────────────────────────────────────────────────────────────

// BLE GAP appearance values (Bluetooth SIG assigned numbers, HID category).
#define APPEARANCE_HID_JOYSTICK 0x03C3
#define APPEARANCE_HID_GAMEPAD  0x03C4

// A gamepad, specifically — not just any HID advertiser. TV remotes (e.g. a Sky Q remote),
// keyboards and mice all advertise the same HID service UUID, and matching on the service
// alone made the host latch onto whichever HID device happened to advertise first. Gamepads
// declare themselves via the GAP appearance field (0x03C4, joystick 0x03C3 — Xbox controllers
// advertise 0x03C4); require that when present, and fall back to a name check for adverts
// that omit appearance rather than rejecting them outright.
//
// Fields for one device are commonly split across TWO separate reports — the primary
// advertisement and its scan response — each a distinct BLE_GAP_EVENT_DISC with only part of
// what the controller broadcasts (e.g. the HID-service UUID in one, the appearance in the
// other). Judging either packet in isolation meant whichever one happened to be delivered
// first for a given scan decided whether the pad was recognised at all — this is the
// "sometimes connects, sometimes doesn't" behaviour. Accumulate fields per address across
// reports instead, so a decision only fires once we've actually seen enough to make one either
// way (this same address's next report, not a whole new scan, gives it another chance).
static ble_addr_t s_pend_addr;
static bool       s_pend_valid;
static bool       s_pend_has_hid;
static bool       s_pend_appearance_present;
static uint16_t   s_pend_appearance;
static char       s_pend_name[32];

static bool addr_eq(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, sizeof a->val) == 0;
}

static bool adv_is_gamepad(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return false;

    if (!s_pend_valid || !addr_eq(&s_pend_addr, &d->addr)) {
        s_pend_addr = d->addr;
        s_pend_valid = true;
        s_pend_has_hid = false;
        s_pend_appearance_present = false;
        s_pend_name[0] = 0;
    }

    for (int i = 0; i < f.num_uuids16; i++)
        if (ble_uuid_u16(&f.uuids16[i].u) == UUID_HID_SVC) { s_pend_has_hid = true; break; }
    if (f.appearance_is_present) { s_pend_appearance_present = true; s_pend_appearance = f.appearance; }
    if (f.name && f.name_len && !s_pend_name[0]) {
        int n = f.name_len < (int)sizeof(s_pend_name) - 1 ? f.name_len : (int)sizeof(s_pend_name) - 1;
        memcpy(s_pend_name, f.name, n);
    }

    if (!s_pend_has_hid) return false;   // no evidence of a HID device from this address yet

    bool is_pad;
    if (s_pend_appearance_present) {
        is_pad = s_pend_appearance == APPEARANCE_HID_GAMEPAD || s_pend_appearance == APPEARANCE_HID_JOYSTICK;
    } else {
        // No appearance seen from either packet yet — accept by name so a controller whose
        // adverts omit the field still pairs, without reopening the door to every remote/
        // keyboard nearby. Not decided until a name has actually arrived (returning false lets
        // the next report for this same address try again, rather than rejecting outright).
        if (!s_pend_name[0]) return false;
        is_pad = strstr(s_pend_name, "Xbox") != NULL || strstr(s_pend_name, "xbox") != NULL;
    }

    if (!is_pad) {
        ESP_LOGI(TAG, "ignoring non-gamepad HID device '%s'%s", s_pend_name[0] ? s_pend_name : "?",
                 s_pend_appearance_present ? "" : " (no appearance field)");
        return false;
    }

    if (s_pend_name[0]) { strncpy(s_name, s_pend_name, sizeof(s_name) - 1); s_name[sizeof(s_name) - 1] = 0; }
    return true;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (adv_is_gamepad(&event->disc)) {
            ESP_LOGI(TAG, "found gamepad '%s' — connecting", s_name[0] ? s_name : "?");
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 10000, NULL, gap_event, NULL);
            // If the connect attempt couldn't even start (host busy right after cancelling the
            // scan is a plausible transient), scanning is already stopped and nothing else
            // would ever resume it — a silent dead end needing a manual re-press. Restart
            // scanning immediately instead so auto-reconnect actually gets another try.
            if (rc != 0) {
                ESP_LOGW(TAG, "connect start failed (%d) — resuming scan", rc);
                if (s_auto) start_scan();
            }
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_report_dumps = 30;
            ESP_LOGI(TAG, "connected — securing");
            ble_gap_security_initiate(s_conn);              // pair/encrypt (uses bond if present)
        } else {
            ESP_LOGW(TAG, "connect failed (%d)", event->connect.status);
            if (s_auto) start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "encrypted — discovering HID service");
            s_subscribed = false; s_proto_mode_handle = 0;
            s_hid_start_handle = 0; s_hid_end_handle = 0;
            ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(UUID_HID_SVC), on_svc, NULL);
        } else {
            // Previously just logged and left the connection sitting there: GATT discovery
            // (and everything after it) only ever starts on encryption SUCCESS, so a failure
            // here silently stranded a connected-but-useless link forever — no gamepad data,
            // no retry, nothing until the user noticed and manually forgot/re-paired.
            // Terminate it instead so DISCONNECT fires and auto-reconnect gets a real retry.
            ESP_LOGW(TAG, "encryption failed (%d) — disconnecting to retry (forgetting bond may help if this persists)",
                     event->enc_change.status);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != s_conn) return 0;
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len >= 2 && len <= 32) {
            uint8_t buf[32];
            uint16_t got = 0;
            ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &got);
            // First few reports after each connect get hex-dumped: parse_report only decodes
            // the Xbox Series layout, so this is how a new controller's report format gets
            // captured (move one control at a time, read the changing bytes off the monitor)
            // to add a mapping for it — without a HID report-descriptor parser on board. Gated
            // behind the web's Settings > verbose sensor debug toggle (was unconditional).
            if (debug_flag_get() && s_report_dumps > 0) {
                s_report_dumps--;
                char hex[3 * 32 + 1];
                for (int i = 0; i < got; i++) sprintf(&hex[i * 3], "%02x ", buf[i]);
                ESP_LOGI(TAG, "report len=%u: %s", got, hex);
            }
            parse_report(buf, got);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "controller disconnected (reason %d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        { gamepad_state_t z = {0}; publish(&z); }
        notify_status(false);
        if (s_auto) start_scan();
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (!ble_hs_synced()) return;             // host not ready yet (deferred by boot task)
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    // filter_duplicates=0: adv_is_gamepad() now accumulates fields ACROSS a device's separate
    // primary-advertisement and scan-response reports before deciding — with duplicate
    // filtering on, it's not guaranteed both packet types (or their repeats) actually reach us
    // if the controller's dedup logic collapses them by address rather than by packet type,
    // which would starve that accumulation of the second half it needs.
    struct ble_gap_disc_params p = { .itvl = 0, .window = 0, .filter_duplicates = 0, .passive = 0 };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGW(TAG, "scan start rc=%d", rc);
}

// Wait for the NimBLE host to sync (ble_svc owns on_sync), then kick off auto-reconnect.
static void boot_scan_task(void *arg)
{
    (void)arg;
    while (!ble_hs_synced()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));           // let the peripheral start advertising first
    if (s_auto) start_scan();
    vTaskDelete(NULL);
}

// ── Public API ───────────────────────────────────────────────────────────────
void hid_host_scan(void)
{
    s_auto = true;
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    start_scan();
}

void hid_host_forget(void)
{
    s_auto = false;
    ble_gap_disc_cancel();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    // Erase all bonds (peer + our security material).
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "forget: store_clear rc=%d", rc);
    s_name[0] = 0;
}

esp_err_t hid_host_init(void)
{
    // Security: bond with LE Secure Connections, Just-Works (no display/keyboard).
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();                 // NVS-backed bond persistence

    // If a bond already exists, auto-reconnect once the host syncs (deferred off this call).
    s_auto = true;
    xTaskCreate(boot_scan_task, "hid_boot", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "HID host ready (will scan for a paired controller)");
    return ESP_OK;
}
