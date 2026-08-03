// bus_scan.c — enumerate attached devices for the web UI's "Scan sensors" action.
#include "bus_scan.h"
#include <stdbool.h>
#include "cJSON.h"
#include "esp_log.h"
#include "bus_i2c.h"
#include "bus_i2c3.h"
#include "bus_spi.h"
#include "i2c_mux.h"
#include "board_config.h"

static const char *TAG = "bus_scan";

#define MUX_ADDR_LO BOARD_I2C_MUX_ADDR_LO   // mux scan range (board_config.h)
#define MUX_ADDR_HI BOARD_I2C_MUX_ADDR_HI
#define ADDR_LO     0x08                     // full I2C 7-bit sensor probe range
#define ADDR_HI     0x77

// 0x29 is shared by three very different chips (TCS34725 colour, VL53L0X ToF, VL53L1X ToF) —
// an address alone can't tell them apart, and each needs a different driver. Their ID
// registers can: harmless single-register reads, done while the scan still has the device's
// mux channel routed.
static const char *guess_0x29(void)
{
    uint8_t v = 0;
    bus_i2c_lock();
    const char *guess = "tcs34725";                       // prior behaviour as the fallback
    if (bus_i2c_read_reg(0x29, 0xC0, &v, 1) == ESP_OK && v == 0xEE) {
        guess = "vl53l0x";                                // VL53L0X model id
    } else {
        uint8_t idx[2] = { 0x01, 0x0F };                  // VL53L1X: 16-bit index 0x010F = 0xEA
        if (bus_i2c_write(0x29, idx, 2) == ESP_OK && bus_i2c_read(0x29, &v, 1) == ESP_OK && v == 0xEA)
            guess = "vl53l1x";
        else if (bus_i2c_read_reg(0x29, 0x80 | 0x12, &v, 1) == ESP_OK &&
                 (v == 0x44 || v == 0x4D || v == 0x10 || v == 0x12))
            guess = "tcs34725";                           // TCS3472x family ID variants
    }
    bus_i2c_unlock();
    return guess;
}

// 0x39 is shared by the AS7341 spectral sensor and the APDS-9960 gesture sensor (among
// others) — their ID registers tell them apart. AS7341: WHOAMI 0x92 carries the part number
// in bits 7:2 (0x09); APDS-9960's 0x92 reads 0xAB.
static const char *guess_0x39(void)
{
    uint8_t v = 0;
    if (bus_i2c_read_reg(0x39, 0x92, &v, 1) == ESP_OK && ((v & 0xFC) >> 2) == 0x09)
        return "as7341";
    return "unknown";
}

// 0x48 is shared by the ADS1115 ADC and M5Stack's Unit Step16 — their firmware-version register
// tells them apart: Step16's 0xFE reads back a small plausible version byte, while an ADS1115
// (only 4 registers, 0-3) reading register pointer 0xFE is very unlikely to coincidentally land
// in that same range. Heuristic, not a real ID register — pick the right type manually if wrong.
static const char *guess_0x48(void)
{
    uint8_t v = 0;
    if (bus_i2c_read_reg(0x48, 0xFE, &v, 1) == ESP_OK && v > 0 && v < 0x80)
        return "m5_step16";
    return "ads1115";
}

// 0x40 is shared by the INA219/INA226/INA260 family of power monitors (and a few unrelated
// chips) — e.g. M5Stack's Atomic Motion Base v1.2 (Atom Motion Base) puts an INA226 here to
// monitor its 18350 Li-ion cell (confirmed against M5Stack's own M5Atomic-Motion Arduino
// library and the INA226 datasheet). Its manufacturer/die-ID registers (0xFE/0xFF, absent on
// INA219) tell it apart from the other 0x40 chips this driver doesn't support.
static const char *guess_0x40(void)
{
    uint8_t d[2] = {0};
    if (bus_i2c_read_reg(0x40, 0xFE, d, 2) == ESP_OK && d[0] == 0x54 && d[1] == 0x49 &&
        bus_i2c_read_reg(0x40, 0xFF, d, 2) == ESP_OK && d[0] == 0x22 && d[1] == 0x60)
        return "ina226";
    return "unknown";
}

// Best-effort type hint from a known I2C address.
static const char *guess_i2c(uint8_t addr)
{
    switch (addr) {
    case 0x76: case 0x77: return "bmp280";   // also BME280; pick the right type when adding
    case 0x53:            return "adxl345";
    case 0x68: case 0x69: return "mpu6050";
    case 0x6a: case 0x6b: return "qmi8658";
    case 0x29:            return guess_0x29();
    case 0x39:            return guess_0x39();
    case 0x65:            return "vk36n16";
    case 0x48:            return guess_0x48();
    case 0x43:            return "m5_8angle";
    case 0x38:            return "m5_motor_ctrl";  // STM32 on Atomic Motion Base
    case 0x40:            return guess_0x40();
    case 0x3c:            return "ssd1306";
    default:              return "unknown";
    }
}

// Distinguish a real TCA9548A from a sensor that merely shares the 0x70..0x77 range
// (e.g. a QMI8658 at 0x77 or a BMP280 at 0x76). A mux echoes its control register, so
// writing a value and reading it back returns the same value; a sensor returns register
// contents instead. Leaves all channels deselected.
static bool verify_mux(uint8_t addr)
{
    static const uint8_t patterns[] = {0x00, 0x05};
    uint8_t off = 0x00, v;
    bus_i2c_lock();                                   // write+readback must not interleave
    for (size_t i = 0; i < sizeof(patterns); i++) {
        if (bus_i2c_write(addr, &patterns[i], 1) != ESP_OK) { bus_i2c_unlock(); return false; }
        if (bus_i2c_read(addr, &v, 1) != ESP_OK || v != patterns[i]) {
            bus_i2c_write(addr, &off, 1);             // best-effort deselect
            bus_i2c_unlock();
            return false;
        }
    }
    bus_i2c_write(addr, &off, 1);                     // deselect all channels
    bus_i2c_unlock();
    return true;
}

static void add_i2c(cJSON *arr, uint8_t addr, uint8_t mux_addr, int channel)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "bus", "i2c");
    cJSON_AddNumberToObject(o, "addr", addr);
    cJSON_AddNumberToObject(o, "mux_addr", mux_addr);
    cJSON_AddNumberToObject(o, "channel", channel);
    cJSON_AddStringToObject(o, "guess", guess_i2c(addr));
    cJSON_AddItemToArray(arr, o);
}

// A floating bus (e.g. an empty mux channel) yields intermittent phantom ACKs, so require
// several consecutive ACKs before believing a device is really there. Returns fast on the
// common no-device case (first probe NACKs).
#define PROBE_CONFIRMS 4
static bool probe_stable(uint8_t a)
{
    for (int i = 0; i < PROBE_CONFIRMS; i++)
        if (bus_i2c_probe(a) != ESP_OK) return false;
    return true;
}

// Probe ADDR_LO..ADDR_HI.
//  `is_mux` — addresses that are *detected* muxes (reported separately as kind:"mux").
//             Only these are skipped, so a real device at 0x70..0x77 (e.g. a BMP280 at
//             0x76) is still found instead of being hidden by the mux address range.
//  `skip`   — upstream/direct-bus addresses to ignore on a channel (the TCA9548A does not
//             isolate the upstream bus, so onboard sensors would otherwise appear on every
//             channel). `record` (may be NULL) is set for each address found.
static int probe_range(cJSON *arr, uint8_t mux_addr, int channel,
                       const bool *is_mux, const bool *skip, bool *record)
{
    int found = 0;
    for (uint8_t a = ADDR_LO; a <= ADDR_HI; a++) {
        if (is_mux && is_mux[a]) continue;            // a detected mux, not a sensor
        if (skip && skip[a]) continue;                // upstream device, not on this channel
        if (probe_stable(a)) {
            add_i2c(arr, a, mux_addr, channel);
            if (record) record[a] = true;
            found++;
        }
    }
    return found;
}

char *bus_scan_run_json(void)
{
    // A full scan deliberately probes every address in ADDR_LO..ADDR_HI (~112 addresses, times
    // however many mux channels are present) expecting most to have nothing there — that's the
    // whole point of a scan, and probe_stable()/bus_i2c_probe() already treat a no-response
    // probe as an ordinary, expected outcome, not a real error. But esp-idf's i2c_master driver
    // itself logs "probe device timeout" at ESP_LOGE for every one of those non-responses,
    // completely independent of our own error handling — flooding the serial console with
    // dozens of scary-looking E() lines for a scan that's working exactly as intended. Drop that
    // tag's level for the duration of the scan only, so a genuine I2C error during normal sensor
    // polling (outside a scan) still logs normally.
    esp_log_level_t prev_i2c_log = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);   // ESP_LOG_WARN would still let ERROR through

    cJSON *arr = cJSON_CreateArray();

    int total = 0, n_mux = 0;

    // 1) Detect present muxes. A mux ACKs at 0x70..0x77; only the addresses that actually
    //    respond are treated as muxes, so a sensor sharing that range (e.g. BMP280 @ 0x76)
    //    is still reported as a normal device.
    bool mux_present[MUX_ADDR_HI - MUX_ADDR_LO + 1] = {0};
    bool is_mux[ADDR_HI + 1] = {0};
    for (uint8_t m = MUX_ADDR_LO; m <= MUX_ADDR_HI; m++) {
        if (bus_i2c_probe(m) == ESP_OK && verify_mux(m)) {
            mux_present[m - MUX_ADDR_LO] = true;
            is_mux[m] = true;
            n_mux++;
            ESP_LOGI(TAG, "mux detected at 0x%02x", m);
        }
    }

    // 1b) Report each detected mux itself (kind:"mux") so the UI can confirm it is wired,
    //     even before any sensors are attached behind it. Not selectable as a sensor.
    for (uint8_t m = MUX_ADDR_LO; m <= MUX_ADDR_HI; m++) {
        if (!mux_present[m - MUX_ADDR_LO]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "bus", "i2c");
        cJSON_AddStringToObject(o, "kind", "mux");
        cJSON_AddNumberToObject(o, "addr", m);
        cJSON_AddNumberToObject(o, "channels", 8);
        cJSON_AddStringToObject(o, "guess", "tca9548a");
        cJSON_AddItemToArray(arr, o);
    }

    // 2) Direct bus: deselect all muxes, probe, and remember which addresses are upstream.
    // Locked so no other task can select a mux channel while the direct bus is probed.
    bool upstream[ADDR_HI + 1] = {0};
    bus_i2c_lock();
    for (uint8_t m = MUX_ADDR_LO; m <= MUX_ADDR_HI; m++)
        if (mux_present[m - MUX_ADDR_LO]) i2c_mux_select(m, -1);
    total += probe_range(arr, 0, -1, is_mux, NULL, upstream);
    bus_i2c_unlock();

    // 3) Each mux channel, skipping upstream devices so they aren't reported per-channel.
    // select+probe+deselect is one locked critical section so a concurrent poll task can't
    // re-route the mux mid-scan (which would otherwise duplicate a device across channels).
    for (uint8_t m = MUX_ADDR_LO; m <= MUX_ADDR_HI; m++) {
        if (!mux_present[m - MUX_ADDR_LO]) continue;
        for (int ch = 0; ch < 8; ch++) {
            bus_i2c_lock();
            i2c_mux_select(m, ch);
            total += probe_range(arr, m, ch, is_mux, upstream, NULL);
            i2c_mux_select(m, -1);
            bus_i2c_unlock();
        }
    }

    // 4) SPI: report every wired CS line (cannot enumerate without a known device).
    for (int cs = 0; cs < BOARD_SPI_CS_COUNT; cs++) {
        if (!bus_spi_cs_valid(cs)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "bus", "spi");
        cJSON_AddNumberToObject(o, "cs_index", cs);
        cJSON_AddStringToObject(o, "guess", "unknown");
        cJSON_AddItemToArray(arr, o);
    }

    // 5) UART: report the configured aux port — only if this board actually has one wired
    // (mirrors the SPI loop above self-guarding via BOARD_SPI_CS_COUNT==0; every board today
    // defines real UART pins, so this is unconditional in practice, but a future board setting
    // either to -1 is handled correctly with no other change needed).
#if BOARD_UART_TX_GPIO >= 0 && BOARD_UART_RX_GPIO >= 0
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "bus", "uart");
        cJSON_AddNumberToObject(o, "port", BOARD_UART_PORT);
        cJSON_AddStringToObject(o, "guess", "unknown");
        cJSON_AddItemToArray(arr, o);
    }
#endif

    // 6) Onboard display (SPI panels can't be auto-detected; report the board's built-in one
    //    so the UI can enable it. External SPI displays are added manually; an I2C SSD1306
    //    surfaces in the direct-bus scan above at 0x3C).
#if BOARD_HAS_DISPLAY
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "bus", "spi");
        cJSON_AddStringToObject(o, "kind", "display");
        cJSON_AddBoolToObject(o, "builtin", true);
        cJSON_AddStringToObject(o, "controller", BOARD_TFT_CONTROLLER);
        cJSON_AddNumberToObject(o, "cs_index", BOARD_DISPLAY_CS_INDEX);
        // Actual wired pins for the *onboard* panel, straight from board_config.h — a saved
        // display config in NVS predating a pin fix (e.g. this board's earlier CS/DC swap) has
        // no other way to self-correct; the web UI's "Use as display" applies these directly
        // instead of just the controller+geometry, so it fully resets to what's really wired.
        cJSON_AddNumberToObject(o, "cs", BOARD_TFT_CS_GPIO);
        cJSON_AddNumberToObject(o, "dc", BOARD_TFT_DC_GPIO);
        cJSON_AddNumberToObject(o, "rst", BOARD_TFT_RST_GPIO);
        cJSON_AddNumberToObject(o, "bl", BOARD_TFT_BL_GPIO);
        cJSON_AddStringToObject(o, "guess", BOARD_TFT_CONTROLLER);
        cJSON_AddItemToArray(arr, o);
    }
#endif

    // 7) Onboard IMU (BMI270 + BMM150, see the imu component) — on a second, internal I2C bus
    //    that isn't reachable by the normal bus scan at all, so unlike every other sensor here
    //    this can never actually be *discovered*, only reported as always-present so it can be
    //    added the normal way instead of requiring "+ Blank sensor" + picking the type by hand.
    //    No real address/mux — the `bmi270_bmm150` driver ignores that config entirely. One
    //    entry, not two: BMM150 is only reachable through BMI270's aux port (see
    //    drv_bmi270_bmm150.c), so the two chips are exposed to the user as a single combined
    //    sensor, not two separate ones.
#if defined(BOARD_TFT_BL_I2C_ADDR)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "bus", "i2c");
        cJSON_AddBoolToObject(o, "builtin", true);
        cJSON_AddStringToObject(o, "guess", "bmi270_bmm150");
        cJSON_AddItemToArray(arr, o);
    }
#endif

    // 8) Atomic Motion Base v1.2 (bottom pogo-pin header — G38/G39, a third I2C bus entirely
    //    separate from Port.A — see board_config.h's BOARD_MOTION_I2C_* and bus_i2c3.h). Unlike
    //    the onboard IMU above, this is a detachable external accessory, so it's genuinely
    //    probed rather than assumed present. Two devices on this bus: the STM32 motor/servo
    //    controller (0x38) and the INA226 power monitor (0x40). Both are surfaced for diagnostics,
    //    though the motor controller isn't a MultiController sensor (motion_ctrl.c handles its
    //    initialization/disabling at boot). Same manufacturer/die-ID check as guess_0x40() above,
    //    just against bus_i2c3 instead of the external bus.
#if defined(BOARD_MOTION_I2C_SDA_GPIO)
    if (bus_i2c3_init() == ESP_OK) {
        // Report STM32 motor/servo controller at 0x38 for diagnostics
        if (bus_i2c3_probe(0x38) == ESP_OK) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "bus", "i2c");
            cJSON_AddNumberToObject(o, "addr", 0x38);
            cJSON_AddStringToObject(o, "guess", "m5_motor_ctrl");
            cJSON_AddStringToObject(o, "kind", "motor_controller");
            cJSON_AddItemToArray(arr, o);
            total++;
        }

        // Report INA226 power monitor at 0x40
        uint8_t d[2] = {0};
        if (bus_i2c3_read_reg(0x40, 0xFE, d, 2) == ESP_OK && d[0] == 0x54 && d[1] == 0x49 &&
            bus_i2c3_read_reg(0x40, 0xFF, d, 2) == ESP_OK && d[0] == 0x22 && d[1] == 0x60) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "bus", "i2c");
            cJSON_AddNumberToObject(o, "addr", 0x40);
            cJSON_AddStringToObject(o, "guess", "ina226");
            cJSON_AddItemToArray(arr, o);
            total++;
        }
    }
#endif

    esp_log_level_set("i2c.master", prev_i2c_log);

    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "scan complete: %d mux, %d I2C device(s)", n_mux, total);
    return out;
}
