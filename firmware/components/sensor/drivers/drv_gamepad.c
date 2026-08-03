// drv_gamepad.c — virtual sensor exposing a BLE-HID game controller (via hid_host).
//
// Not a bus device: read() ignores cfg->bus/addr and just copies the latest controller state
// from hid_host into the value array, so the scheduler → transform → LEGO-emitter pipeline can
// pack button/axis state into the colour-sensor's RGBI payload like any other sensor.
//
// Values: [buttons, lx, ly, rx, ry, lt, rt, dpad]
//   buttons — 16-bit normalised bitmask (HID_BTN_*; map a 16-bit LEGO field to it)
//   lx/ly/rx/ry — sticks, centred -32768..32767
//   lt/rt — triggers 0..1023
//   dpad — 8-way hat 0..8

#include "sensor.h"
#include "hid_host.h"

static esp_err_t gamepad_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    (void)cfg;
    gamepad_state_t st;
    hid_host_get_state(&st);                 // zeroed when no controller is connected
    int n = 0;
    if (max > n) out[n++] = (float)st.buttons;
    if (max > n) out[n++] = (float)st.lx;
    if (max > n) out[n++] = (float)st.ly;
    if (max > n) out[n++] = (float)st.rx;
    if (max > n) out[n++] = (float)st.ry;
    if (max > n) out[n++] = (float)st.lt;
    if (max > n) out[n++] = (float)st.rt;
    if (max > n) out[n++] = (float)st.dpad;
    *out_count = n;
    return ESP_OK;
}

static int gamepad_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    static const char *n[] = { "buttons", "lx", "ly", "rx", "ry", "lt", "rt", "dpad" };
    int c = (int)(sizeof(n) / sizeof(n[0]));
    if (c > max) c = max;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_gamepad = {
    .type = "gamepad", .probe = NULL, .read = gamepad_read, .describe = gamepad_describe
};
