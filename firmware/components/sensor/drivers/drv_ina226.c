// drv_ina226.c — named driver: TI INA226 current/voltage/power monitor. One generic sensor
// type covering both places this chip actually shows up in this project:
//   1. A bare INA226 breakout wired directly to the external Port.A I2C bus (bus_i2c) at
//      whatever address it's strapped to (cfg->addr, default 0x40).
//   2. M5Stack's Atomic Motion Base v1.2's own fixed-address INA226, on a completely separate
//      bus (bus_i2c3, the bottom pogo-pin header's G38/G39 — not Port.A at all).
// cfg->addr/mux/bus can't tell these apart (both commonly sit at 0x40, no mux), so on first
// read this driver tries Port.A at cfg->addr first, and only falls back to probing the Motion
// Base's fixed bus/address if nothing valid answers there — then remembers which bus it found
// the chip on (keyed by cfg->id, not addr/mux, precisely because two different sensor entries
// can share the same address on two different buses) so every later read goes straight there.
// See ina226_common.h for the shared register map/calibration/battery-curve math.
//
// Outputs: voltage (mV), current (mA), power (mW), pct (%, rough Li-ion state-of-charge estimate
// from voltage — see ina226_batt_pct_from_voltage()).
#include "sensor.h"
#include "bus_i2c.h"
#include "bus_i2c3.h"
#include "i2c_mux.h"
#include "ina226_common.h"

#define MOTION_BASE_ADDR 0x40   // Atomic Motion Base's INA226: fixed address, no mux, own bus

typedef enum { INA_BUS_UNKNOWN = 0, INA_BUS_PORTA, INA_BUS_MOTION } ina_bus_t;

typedef struct {
    int id;
    bool used;
    ina_bus_t bus;
} state_t;

static state_t s_state[MC_MAX_SENSORS];

static state_t *state_lookup(int id)
{
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (s_state[i].used && s_state[i].id == id) return &s_state[i];
    for (int i = 0; i < MC_MAX_SENSORS; i++)
        if (!s_state[i].used) { s_state[i] = (state_t){ .id = id, .used = true, .bus = INA_BUS_UNKNOWN }; return &s_state[i]; }
    return NULL;
}

static esp_err_t write_reg16_porta(uint8_t addr, uint8_t reg, uint16_t val)
{
    uint8_t d[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
    return bus_i2c_write(addr, d, sizeof(d));
}
static esp_err_t read_reg16_porta(uint8_t addr, uint8_t reg, uint16_t *val)
{
    uint8_t d[2];
    esp_err_t err = bus_i2c_read_reg(addr, reg, d, sizeof(d));
    if (err != ESP_OK) return err;
    *val = ((uint16_t)d[0] << 8) | d[1];
    return ESP_OK;
}
static esp_err_t write_reg16_motion(uint8_t reg, uint16_t val)
{
    uint8_t d[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
    return bus_i2c3_write(MOTION_BASE_ADDR, d, sizeof(d));
}
static esp_err_t read_reg16_motion(uint8_t reg, uint16_t *val)
{
    uint8_t d[2];
    esp_err_t err = bus_i2c3_read_reg(MOTION_BASE_ADDR, reg, d, sizeof(d));
    if (err != ESP_OK) return err;
    *val = ((uint16_t)d[0] << 8) | d[1];
    return ESP_OK;
}

// Verify the chip via its manufacturer/die-ID registers (0x5449/0x2260 — no equivalent on the
// similarly-addressed INA219), then apply the shared config/calibration. False on anything not
// answering exactly like a real INA226.
static bool check_and_configure_porta(uint8_t addr)
{
    uint16_t mfg = 0, die = 0;
    if (read_reg16_porta(addr, INA226_REG_MANUFACTURER, &mfg) != ESP_OK) return false;
    if (read_reg16_porta(addr, INA226_REG_DIE_ID, &die) != ESP_OK) return false;
    if (mfg != INA226_MANUFACTURER_ID || die != INA226_DIE_ID) return false;
    write_reg16_porta(addr, INA226_REG_CONFIG, INA226_CONFIG_VALUE);
    write_reg16_porta(addr, INA226_REG_CALIBRATION, INA226_CALIB_VALUE);
    return true;
}

static bool check_and_configure_motion(void)
{
    if (bus_i2c3_init() != ESP_OK) return false;
    uint16_t mfg = 0, die = 0;
    if (read_reg16_motion(INA226_REG_MANUFACTURER, &mfg) != ESP_OK) return false;
    if (read_reg16_motion(INA226_REG_DIE_ID, &die) != ESP_OK) return false;
    if (mfg != INA226_MANUFACTURER_ID || die != INA226_DIE_ID) return false;
    write_reg16_motion(INA226_REG_CONFIG, INA226_CONFIG_VALUE);
    write_reg16_motion(INA226_REG_CALIBRATION, INA226_CALIB_VALUE);
    return true;
}

static esp_err_t ina226_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 4) return ESP_ERR_INVALID_SIZE;

    state_t *st = state_lookup(cfg->id);
    if (!st) return ESP_ERR_NO_MEM;

    if (st->bus == INA_BUS_UNKNOWN) {
        if (i2c_mux_route(cfg->mux_addr, cfg->mux_channel) == ESP_OK && check_and_configure_porta(cfg->addr)) {
            st->bus = INA_BUS_PORTA;
        } else if (check_and_configure_motion()) {
            st->bus = INA_BUS_MOTION;
        } else {
            return ESP_ERR_NOT_FOUND;
        }
    }

    uint16_t rawV, rawI, rawP;
    esp_err_t err;
    if (st->bus == INA_BUS_PORTA) {
        if ((err = i2c_mux_route(cfg->mux_addr, cfg->mux_channel)) != ESP_OK) return err;
        if ((err = read_reg16_porta(cfg->addr, INA226_REG_BUSVOLTAGE, &rawV)) != ESP_OK) return err;
        if ((err = read_reg16_porta(cfg->addr, INA226_REG_CURRENT, &rawI)) != ESP_OK) return err;
        if ((err = read_reg16_porta(cfg->addr, INA226_REG_POWER, &rawP)) != ESP_OK) return err;
    } else {
        if ((err = read_reg16_motion(INA226_REG_BUSVOLTAGE, &rawV)) != ESP_OK) return err;
        if ((err = read_reg16_motion(INA226_REG_CURRENT, &rawI)) != ESP_OK) return err;
        if ((err = read_reg16_motion(INA226_REG_POWER, &rawP)) != ESP_OK) return err;
    }

    float voltage = rawV * INA226_BUS_LSB;
    // All values in milli-units (mV, mA, mW) for better LEGO compatibility (larger whole integers)
    out[0] = voltage * 1000.0f;
    out[1] = (int16_t)rawI * INA226_CURRENT_LSB * 1000.0f;
    out[2] = rawP * INA226_POWER_LSB * 1000.0f;
    out[3] = ina226_batt_pct_from_voltage(voltage);
    *out_count = 4;
    return ESP_OK;
}

static int ina226_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    static const char *n[] = {"voltage_mV", "current_mA", "power_mW", "pct"};
    int c = max < 4 ? max : 4;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_ina226 = {
    .type = "ina226",
    .probe = NULL,
    .read = ina226_read,
    .describe = ina226_describe,
};
