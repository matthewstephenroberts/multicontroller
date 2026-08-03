#include "i2c_mux.h"
#include "bus_i2c.h"

// Last channel mask successfully written to each mux, so back-to-back reads behind the same
// channel skip the (redundant) select write. Small win per read (~50us at 400kHz), but the
// scheduler orders due jobs by (mux, channel) precisely so runs of same-channel reads happen —
// this cache is what turns that ordering into actually fewer bus transactions.
//
// PCA954x muxes live at 0x70-0x77; index by the low 3 address bits. A cache entry only ever
// updates on a *successful* write, and i2c_mux_invalidate() drops all entries — called by the
// bus scanner (which drives the muxes directly, including disabling them) and available for
// any path that can't be sure the muxes still hold what we last wrote (e.g. after a power
// glitch on the peripheral rail).
#define MUX_SLOTS 8
static int16_t s_cur_mask[MUX_SLOTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };  // -1 = unknown

void i2c_mux_invalidate(void)
{
    for (int i = 0; i < MUX_SLOTS; i++) s_cur_mask[i] = -1;
}

// All muxes share the same upstream SDA/SCL — a TCA9548A holds its channel selection until
// explicitly changed, so with more than one mux present, switching to a channel on mux B while
// mux A still has a channel enabled from a previous select bridges BOTH downstream subtrees
// onto the shared bus at once. Whatever gets read next can then contend with a second device
// answering at the same time, corrupting the transaction (observed as e.g. a colour sensor's ID
// register reading back 0x00 instead of its real value). Only one mux's channel may be active
// on the shared bus at a time, so deselect every *other* mux with a non-zero cached mask before
// applying a new selection (including selecting no mux at all, for a direct/unmuxed sensor).
static void deselect_other_muxes(int keep_slot)
{
    for (int i = 0; i < MUX_SLOTS; i++) {
        if (i == keep_slot || s_cur_mask[i] <= 0) continue;
        uint8_t off = 0x00;
        uint8_t addr = (uint8_t)(0x70 | i);
        esp_err_t err = bus_i2c_write(addr, &off, 1);
        s_cur_mask[i] = (err == ESP_OK) ? 0 : -1;
    }
}

esp_err_t i2c_mux_select(uint8_t mux_addr, int8_t channel)
{
    if (mux_addr == 0) {                              // no mux: direct sensor on the main bus
        deselect_other_muxes(-1);                      // but another mux's channel may still be live
        return ESP_OK;
    }
    uint8_t mask = (channel < 0) ? 0x00 : (uint8_t)(1u << channel);
    int slot = mux_addr & 0x07;
    deselect_other_muxes(slot);
    if (s_cur_mask[slot] == (int16_t)mask) return ESP_OK;   // already routed there
    esp_err_t err = bus_i2c_write(mux_addr, &mask, 1);
    s_cur_mask[slot] = (err == ESP_OK) ? (int16_t)mask : -1;
    return err;
}

esp_err_t i2c_mux_route(uint8_t mux_addr, int8_t channel)
{
    return i2c_mux_select(mux_addr, channel);
}
