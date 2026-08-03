// hid_host.h — BLE-HID (HOGP) host for a Bluetooth LE game controller (Xbox Series).
//
// Runs as a NimBLE *central* alongside ble_svc's peripheral role (shared host). It scans for
// a BLE-HID device (service 0x1812), bonds (LE Secure Connections, persisted to NVS), and
// subscribes to the input-report notifications, decoding them into a gamepad_state_t that the
// "gamepad" virtual sensor (drv_gamepad) exposes to the scheduler → LEGO emitter pipeline.
//
//   main.c:  ble_svc_init()  ->  hid_host_init()
//   web:     {cmd:"hid_scan"} / {cmd:"hid_forget"}  ->  hid_host_scan() / hid_host_forget()
//
// Also supports a *virtual* controller: the web app can drive an on-screen Xbox layout with no
// physical pad connected. {cmd:"hid_virtual",enabled} toggles it; while enabled,
// {cmd:"hid_set_state",...} pushes a full gamepad_state_t snapshot that hid_host_get_state()
// serves instead of the real BLE report (so drv_gamepad / the scheduler / LEGO emitter see it
// exactly like a real pad — no separate code path). Real HID reports keep updating s_state
// underneath so unchecking "virtual" falls straight back to the actual controller.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decoded controller state. Axes are centred signed (-32768..32767); triggers 0..1023;
// dpad is the raw 8-way hat (0 = released, 1 = up, clockwise to 8 = up-left). `buttons` is a
// normalised bitmask (see HID_BTN_* below) that also folds the dpad into 4 direction bits.
typedef struct {
    uint16_t buttons;
    int16_t  lx, ly, rx, ry;
    uint16_t lt, rt;
    uint8_t  dpad;
    bool     connected;
} gamepad_state_t;

// Normalised button bit positions (stable regardless of the raw report layout).
#define HID_BTN_A      (1u << 0)
#define HID_BTN_B      (1u << 1)
#define HID_BTN_X      (1u << 2)
#define HID_BTN_Y      (1u << 3)
#define HID_BTN_LB     (1u << 4)
#define HID_BTN_RB     (1u << 5)
#define HID_BTN_VIEW   (1u << 6)
#define HID_BTN_MENU   (1u << 7)
#define HID_BTN_LS     (1u << 8)
#define HID_BTN_RS     (1u << 9)
#define HID_BTN_XBOX   (1u << 10)
#define HID_BTN_SHARE  (1u << 11)
#define HID_BTN_DUP    (1u << 12)
#define HID_BTN_DDOWN  (1u << 13)
#define HID_BTN_DLEFT  (1u << 14)
#define HID_BTN_DRIGHT (1u << 15)

// Initialise the HID-host central (SM/bonding config + NVS-backed bond store). Call once
// after ble_svc_init(). If a bond already exists it auto-scans/reconnects in the background.
esp_err_t hid_host_init(void);

// Start scanning to pair a controller (put the controller in pairing mode first).
void hid_host_scan(void);

// Erase the stored bond(s) and disconnect, so the controller must be re-paired.
void hid_host_forget(void);

// Copy the latest controller state (the virtual state, if virtual mode is enabled — see
// hid_host_set_virtual_enabled). Returns false if never connected and virtual mode is off.
bool hid_host_get_state(gamepad_state_t *out);

// True while a controller is connected and subscribed (real HID, not virtual).
bool hid_host_is_connected(void);

// Last-seen controller name ("controller" until one has advertised a name).
const char *hid_host_name(void);

// Enable/disable the virtual controller. While enabled, hid_host_get_state() returns the last
// state pushed via hid_host_set_virtual_state() (as "connected") instead of the real HID state.
void hid_host_set_virtual_enabled(bool enabled);

// True if the virtual controller is currently overriding the real HID state.
bool hid_host_virtual_enabled(void);

// Push a full state snapshot for the virtual controller (only takes effect while virtual mode
// is enabled — see hid_host_set_virtual_enabled).
void hid_host_set_virtual_state(const gamepad_state_t *state);

// Connection-status callback (connected flag + device name), for BLE status events. NULL clears.
void hid_host_set_status_cb(void (*cb)(bool connected, const char *name));

#ifdef __cplusplus
}
#endif
