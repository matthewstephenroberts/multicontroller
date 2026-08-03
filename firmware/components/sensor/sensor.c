// sensor.c — driver registry + read/describe dispatch.
#include "sensor.h"
#include "sensor_transform.h"
#include <string.h>
#include "esp_log.h"
#include "bus_i2c.h"
#include "debug_flag.h"

static const char *TAG = "sensor";
// Was 16, but sensor_drivers_register() below already registers 18 named drivers (silently —
// sensor_registry_add()'s ESP_ERR_NO_MEM return is discarded there) before this change: the last
// two registered, drv_ir_ball and drv_vk36n16, never actually made it into the registry, and
// silently fell back to the generic recipe engine for their type strings. Sized with headroom
// above what's registered today so the next driver added doesn't repeat this silently.
#define MAX_DRIVERS 24

static const sensor_driver_t *s_drivers[MAX_DRIVERS];
static int s_driver_n;

// Thin wrappers over debug_flag (see sensor.h for why the flag itself lives in that separate
// leaf component rather than here or in config_store).
bool sensor_get_verbose_debug(void) { return debug_flag_get(); }
void sensor_set_verbose_debug(bool on) { debug_flag_set(on); }

// Provided by the generic recipe engine and the named drivers.
extern const sensor_driver_t sensor_generic_driver;
extern const sensor_driver_t drv_bme280;
extern const sensor_driver_t drv_bmp280;
extern const sensor_driver_t drv_qmi8658;
extern const sensor_driver_t drv_tcs34725;
extern const sensor_driver_t drv_as7341;
extern const sensor_driver_t drv_vl53l1x;
extern const sensor_driver_t drv_vl53l0x;
extern const sensor_driver_t drv_tof10120;
extern const sensor_driver_t drv_tofi2c;
extern const sensor_driver_t drv_gamepad;
extern const sensor_driver_t drv_gpio;
extern const sensor_driver_t drv_adc;
extern const sensor_driver_t drv_mcp3208;
extern const sensor_driver_t drv_qre1113;
extern const sensor_driver_t drv_ir_ball;
extern const sensor_driver_t drv_vk36n16;
extern const sensor_driver_t drv_m5_8angle;
extern const sensor_driver_t drv_m5_step16;
extern const sensor_driver_t drv_bmi270_bmm150;
extern const sensor_driver_t drv_ina226;
extern const sensor_driver_t drv_motor_ctrl_status;

esp_err_t sensor_registry_add(const sensor_driver_t *drv)
{
    if (!drv || !drv->type || !drv->read) return ESP_ERR_INVALID_ARG;
    if (s_driver_n >= MAX_DRIVERS) {
        // Registration callers below don't check this return value, so a silent drop here
        // previously meant a driver's type string quietly fell back to the generic recipe
        // engine with no error anywhere — log loudly instead of failing quietly again.
        ESP_LOGE(TAG, "driver registry full (MAX_DRIVERS=%d) — \"%s\" not registered, will fall back to generic", MAX_DRIVERS, drv->type);
        return ESP_ERR_NO_MEM;
    }
    s_drivers[s_driver_n++] = drv;
    return ESP_OK;
}

const sensor_driver_t *sensor_registry_find(const char *type)
{
    if (!type) return NULL;
    for (int i = 0; i < s_driver_n; i++)
        if (!strcmp(s_drivers[i]->type, type)) return s_drivers[i];
    return NULL;
}

void sensor_drivers_register(void)
{
    sensor_registry_add(&sensor_generic_driver);   // type "generic"
    sensor_registry_add(&drv_bme280);              // type "bme280"
    sensor_registry_add(&drv_bmp280);              // type "bmp280"
    sensor_registry_add(&drv_qmi8658);             // type "qmi8658"
    sensor_registry_add(&drv_tcs34725);            // type "tcs34725"
    sensor_registry_add(&drv_as7341);              // type "as7341"
    sensor_registry_add(&drv_vl53l1x);             // type "vl53l1x" (bare ToF chip)
    sensor_registry_add(&drv_vl53l0x);             // type "vl53l0x" (older ToF chip, same 0x29)
    sensor_registry_add(&drv_tof10120);            // type "tof10120"
    sensor_registry_add(&drv_tofi2c);              // type "tofi2c" (TOFxxxC/F modules)
    sensor_registry_add(&drv_gamepad);             // type "gamepad" (BLE-HID controller)
    sensor_registry_add(&drv_gpio);                // type "gpio" (DA pin digital input)
    sensor_registry_add(&drv_adc);                 // type "adc"  (DA pin analog input)
    sensor_registry_add(&drv_mcp3208);             // type "mcp3208" (8ch SPI ADC)
    sensor_registry_add(&drv_qre1113);             // type "qre1113" (line reflectance, via mcp3208)
    sensor_registry_add(&drv_ir_ball);             // type "tssp_ir" (IR ball, via mcp3208)
    sensor_registry_add(&drv_vk36n16);             // type "vk36n16" (16-key I2C touch keypad)
    sensor_registry_add(&drv_m5_8angle);           // type "m5_8angle" (M5Stack 8Angle unit, I2C)
    sensor_registry_add(&drv_m5_step16);           // type "m5_step16" (M5Stack Unit Step16, I2C)
    sensor_registry_add(&drv_bmi270_bmm150);       // type "bmi270_bmm150" (onboard IMU + mag, e.g. AtomS3R)
    sensor_registry_add(&drv_ina226);              // type "ina226" — tries Port.A first, falls back to the
                                                    // Atomic Motion Base's own fixed-address bus (bus_i2c3)
    sensor_registry_add(&drv_motor_ctrl_status);   // type "motor_ctrl_status" — diagnostic sensor for
                                                    // Atomic Motion Base STM32 motor controller (bus_i2c3)
    ESP_LOGI(TAG, "%d drivers registered", s_driver_n);
}

// Resolve the driver for a config: named match, else the generic engine.
static const sensor_driver_t *resolve(const sensor_cfg_t *cfg)
{
    const sensor_driver_t *d = sensor_registry_find(cfg->type);
    return d ? d : &sensor_generic_driver;
}

esp_err_t sensor_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    const sensor_driver_t *d = resolve(cfg);

    // Simulated sensors never touch any bus — no mux select, no I2C/SPI/UART transaction —
    // so they're usable without the hardware being wired up at all, on the same polling
    // schedule as a real sensor (the scheduler calls sensor_read() either way).
    if (cfg->simulate) return sensor_simulate_read(d, cfg, out, max, out_count);

    // Every I2C driver does mux-select-then-transact; hold the bus lock across the whole
    // read so a concurrent poll/scan can't re-route the mux in between (it's recursive,
    // so the driver's own bus_i2c_* calls nest inside this fine).
    bus_i2c_lock();
    esp_err_t err = d->read(cfg, out, max, out_count);
    bus_i2c_unlock();
    return err;
}

// Per-driver realistic poll floors — see sensor.h. Derived from each driver's own hardware
// integration/conversion time plus its typical I2C transaction overhead (not a single arbitrary
// default), so a sensor can't be configured to poll faster than it can actually deliver fresh
// data. Mirrored in web/src/types.ts's pollMsFloor() for the dashboard's dropdown/validation.
uint32_t sensor_poll_floor_ms(const sensor_cfg_t *cfg)
{
    if (!strcmp(cfg->type, "vl53l1x"))
        // ST distance-mode integration budget (drv_vl53l1x.c): 20ms short / 50ms long, plus
        // I2C/mux overhead margin.
        return cfg->dist_mode ? 140 : 60;
    if (!strcmp(cfg->type, "as7341"))
        // Overlapped read (drv_as7341.c): a poll visit only harvests a finished SMUX pass and
        // starts the next (~2ms of I2C), while the ~27.8ms integration (ATIME=9/ASTEP=999)
        // runs on-chip between visits. Floor = one integration + margin, so a pass is done by
        // the next visit; a *fresh* 10-channel frame completes every two visits.
        return 35;
    if (!strcmp(cfg->type, "tcs34725"))
        // Integration time is auto-derived from poll_ms down to the chip's ~2.4ms minimum
        // (drv_tcs34725.c); floor is just I2C overhead margin, not a fixed integration time.
        return 15;
    if (!strcmp(cfg->type, "bme280") || !strcmp(cfg->type, "bmp280"))
        return 20;      // forced-mode conversion (~10ms, drv_bme280.c/drv_bmp280.c) + I2C margin
    if (!strcmp(cfg->type, "qmi8658"))
        return 10;      // free-running at ~235Hz ODR; floor is I2C read overhead, not the IMU
    if (!strcmp(cfg->type, "vl53l0x"))
        return 40;      // default ~33ms ranging budget (drv_vl53l0x.c) + I2C margin
    if (!strcmp(cfg->type, "tof10120"))
        return 40;      // typical module measurement cycle (~30Hz max per datasheet)
    if (!strcmp(cfg->type, "gpio") || !strcmp(cfg->type, "adc") || !strcmp(cfg->type, "gamepad"))
        return 10;      // direct MCU read / cached HID state — no bus conversion to wait on
    if (!strcmp(cfg->type, "vk36n16"))
        return 20;      // one 2-byte I2C register read; chip's touch scan is continuous internally
    if (!strcmp(cfg->type, "m5_8angle"))
        // Cheap STM32-based M5Stack "Unit" I2C peripheral, not a dedicated ASIC like
        // vk36n16/tcs34725/etc — its I2C slave firmware can wedge the whole bus (not just stop
        // responding itself) if hit with transactions faster than it can service; a burst read
        // across all 8 channels previously did exactly that (see drv_m5_8angle.c), now fixed to
        // 8 separate single-channel reads matching the vendor library — but that's 8 transactions
        // per poll instead of 1, so this floor is higher than m5_step16's. No documented minimum
        // interval exists for this unit; raise it further from the UI's poll-rate dropdown if it
        // still drops off after some time polling.
        return 100;
    if (!strcmp(cfg->type, "m5_step16"))
        return 50;      // single register write + 1-byte read per poll — see drv_m5_step16.c
    if (!strcmp(cfg->type, "mcp3208") || !strcmp(cfg->type, "qre1113")) {
        // One SPI transaction per selected channel (drv_qre1113.c) — a few µs of conversion plus
        // SPI overhead each; a grouped sensor (channel_mask set) does one per channel in the
        // group instead of just one.
        int channels = 1;
        if (cfg->channel_mask) { channels = 0; for (int c = 0; c < 8; c++) if (cfg->channel_mask & (1u << c)) channels++; }
        return 4 + channels;
    }
    if (!strcmp(cfg->type, "bmi270_bmm150"))
        return 20;      // combined accel+gyro+mag read — floor set by the mag's forced-mode
                        // regular-preset conversion (slower than the accel/gyro's own ~100Hz ODR)
    if (!strcmp(cfg->type, "ina226"))
        return 20;      // ~1.1ms+1.1ms conversion time per the driver's continuous-mode config;
                        // floor is really just I2C read overhead for 3 register reads per poll —
                        // battery voltage doesn't need to be read fast
    if (!strcmp(cfg->type, "tssp_ir"))
        // Burst read (drv_ir_ball.c) holds the SPI bus for a few ms — a grouped sensor
        // (channel_mask set) splits a fixed total sample budget across its channels, so this
        // stays roughly the same regardless of group size rather than scaling with channel count.
        return 30;
    return 50;          // generic recipe + tofi2c (module timing varies by board) — safe default
}

float sensor_dist_native_max_mm(const sensor_cfg_t *cfg)
{
    // Keep in sync with each driver's own out-of-range fallback (see sensor.h for why this
    // must match): drv_vl53l1x.c's k_native_range table (short/long mode ceilings from the ST
    // datasheet), drv_vl53l0x.c's unclamped 8190 "infinity" sentinel.
    if (!strcmp(cfg->type, "vl53l1x")) return cfg->dist_mode ? 4000.0f : 1300.0f;
    if (!strcmp(cfg->type, "vl53l0x")) return 8190.0f;
    return 2000.0f;    // tof10120/tofi2c/generic: no native ceiling documented, keep the old default
}

int sensor_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    // A transform changes the value set (e.g. ax..gz → roll/pitch/yaw); its names win.
    int tn = sensor_transform_names(cfg, names, max);
    if (tn > 0) return tn;

    const sensor_driver_t *d = resolve(cfg);
    if (d->describe) return d->describe(cfg, names, max);

    int n = cfg->recipe.value_count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) names[i] = cfg->recipe.value_names[i];
    return n;
}
