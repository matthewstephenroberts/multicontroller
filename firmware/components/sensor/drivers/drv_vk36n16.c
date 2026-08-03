// drv_vk36n16.c — Vinka VK36N16I 16-channel capacitive touch keypad (I2C).
//
// The chip senses 16 touch pads (TP0-TP15 — a typical 4x4 keypad legend: 0-9, *, #, A-D) and
// reports the touched set as a bitmap; there is no init sequence, and its INT pin (unused
// here — we poll) is simply low while any pad is touched. Protocol per the VK36N family "I"
// (I2C) datasheets: read the key-state register, one bit per pad, 1 = touched.
//
// Empirically (a VK36N16I keypad read through the generic recipe engine before this driver
// existed): a 2-byte burst read from register 0x00 returns the full 16-key state, first byte =
// high bank (big-endian decode gives bit N = TPN). The VK36N8I datasheet documents 0x02 for
// the 8-key part, but 0x00 is what this chip demonstrably answers on. Both the register and
// the byte order are overridable from the UI recipe if a board revision differs: recipe.reg
// (0 = default 0x00), recipe.byte_order ("be" — the recipe default — matches observed data).
//
// Values:
//   key    — lowest-numbered touched pad (0-15), or -1 when nothing is touched
//   bitmap — the raw 16-bit touched mask (bit N = TPN), for multi-touch / LEGO bit-packing
//   count  — how many pads are currently touched
#include "sensor.h"
#include "bus_i2c.h"
#include "i2c_mux.h"
#include "esp_timer.h"

#define VK36N16_ADDR_DEFAULT 0x65
#define VK36N16_REG_DEFAULT  0x00

// How long a seen touch keeps being reported after the chip stops showing it. A quick tap can
// span a single poll, i.e. exactly one reading carries the press — and the BLE notify throttle
// (ble_svc.c, 50Hz per sensor) may drop precisely that one reading if it lands within the
// throttle window, making taps register only when sub-20ms timing happens to line up
// ("intermittent" misses). Stretching a touch across several polls guarantees multiple
// consecutive readings carry it, so no single dropped notification can lose a press. Slightly
// delays the observed release; irrelevant at keypad speeds.
#define VK36N16_HOLD_US (150 * 1000)

// Per-sensor stretch state, matched by id (ids are arbitrary, not a compact index).
typedef struct { int id; bool used; uint16_t bitmap; int64_t ts_us; } hold_state_t;
static hold_state_t s_hold[MC_MAX_SENSORS];

static uint16_t hold_apply(int id, uint16_t bitmap)
{
    hold_state_t *slot = NULL;
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_hold[i].used && s_hold[i].id == id) { slot = &s_hold[i]; break; }
    if (!slot)
        for (int i = 0; i < MC_MAX_SENSORS; i++)
            if (!s_hold[i].used) { slot = &s_hold[i]; *slot = (hold_state_t){ .id = id, .used = true }; break; }
    if (!slot) return bitmap;               // table full (shouldn't happen) — pass through

    int64_t now = esp_timer_get_time();
    if (bitmap) {
        slot->bitmap = bitmap;
        slot->ts_us = now;
        return bitmap;
    }
    if (slot->bitmap && now - slot->ts_us < VK36N16_HOLD_US) return slot->bitmap;
    slot->bitmap = 0;
    return 0;
}

static esp_err_t vk36n16_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 3) return ESP_ERR_INVALID_SIZE;
    uint8_t addr = cfg->addr ? cfg->addr : VK36N16_ADDR_DEFAULT;
    uint8_t reg  = cfg->recipe.reg ? (uint8_t)cfg->recipe.reg : VK36N16_REG_DEFAULT;

    esp_err_t err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel);
    if (err != ESP_OK) return err;

    // One 2-byte burst read — exactly the transaction the generic recipe engine did when this
    // keypad was (accidentally) read through it, which demonstrably returned valid key state.
    // Retried once: capacitive chips periodically self-recalibrate and can NACK a read landing
    // mid-cycle; without the retry that whole poll fails and a tap during it is lost.
    uint8_t d[2];
    if ((err = bus_i2c_read_reg(addr, reg, d, 2)) != ESP_OK)
        err = bus_i2c_read_reg(addr, reg, d, 2);
    if (err != ESP_OK) return err;

    // Big-endian decode (the recipe default, and what the observed generic-engine values used:
    // legend "5" → 0x2000 with d[0]=0x20) — bit N of the decoded word = TPN. "le" swaps the
    // bytes for a board revision that orders its banks the other way around.
    uint16_t bitmap = cfg->recipe.big_endian
        ? (uint16_t)((d[0] << 8) | d[1])
        : (uint16_t)((d[1] << 8) | d[0]);

    bitmap = hold_apply(cfg->id, bitmap);     // stretch taps across polls (see VK36N16_HOLD_US)

    int key = -1, count = 0;
    for (int i = 0; i < 16; i++) {
        if (bitmap & (1u << i)) {
            if (key < 0) key = i;
            count++;
        }
    }

    out[0] = (float)key;
    out[1] = (float)bitmap;
    out[2] = (float)count;
    *out_count = 3;
    return ESP_OK;
}

static int vk36n16_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    if (max < 3) return 0;
    names[0] = "key";
    names[1] = "bitmap";
    names[2] = "count";
    return 3;
}

const sensor_driver_t drv_vk36n16 = {
    .type = "vk36n16",
    .probe = NULL,
    .read = vk36n16_read,
    .describe = vk36n16_describe,
};
