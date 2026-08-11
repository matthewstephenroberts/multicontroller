// main.c — MultiController boot flow.
//
//   NVS -> config -> buses -> sensor drivers -> scheduler -> BLE -> start polling
//
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "board_config.h"
#include "config_store.h"
#include "bus_i2c.h"
#include "bus_spi.h"
#include "bus_uart.h"
#include "i2c_mux.h"
#include "motion_ctrl.h"
#include "button_ctrl.h"
#include "sensor.h"
#include "scheduler.h"
#include "ble_svc.h"
#include "display.h"
#include "lego_emit.h"
#include "hid_host.h"

static const char *TAG = "main";

// Fan-out for fresh readings: stream over BLE and feed the LEGO emitter's value cache.
static void on_reading(const reading_t *r)
{
    ble_svc_on_reading(r);
    lego_emit_on_reading(r);
}

// Drive the peripheral power-enable pin (TFT_I2C_POWER) HIGH so the I2C/STEMMA bus
// and the display are powered. Without this the onboard sensors read nothing.
static void board_power_on(void)
{
    if (BOARD_PERIP_PWR_GPIO < 0) {
        ESP_LOGI(TAG, "peripheral power pin not used");
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (BOARD_PERIP_PWR_GPIO >= 0) ? (1ULL << BOARD_PERIP_PWR_GPIO) : 0ULL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_PERIP_PWR_GPIO, 1);
    ESP_LOGI(TAG, "peripheral power enabled (GPIO%d)", BOARD_PERIP_PWR_GPIO);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "MultiController starting");

    board_power_on();                 // power the I2C/STEMMA bus + display first

    init_nvs();
    ESP_ERROR_CHECK(config_store_init());

    // Buses come up before drivers so probes during scan/poll work immediately.
    // Non-fatal: a problem on one bus must not prevent the others or BLE from running.
    if (bus_i2c_init() != ESP_OK)                        ESP_LOGW(TAG, "I2C init failed");
    // Muxes keep their channel selection across a reset — clear them before anything probes
    // the bus, or a channel left live from the previous boot bridges two subtrees together.
    i2c_mux_reset_all();
    if (bus_spi_init() != ESP_OK)                        ESP_LOGW(TAG, "SPI init failed");
    if (bus_uart_init(BOARD_UART_DEFAULT_BAUD) != ESP_OK) ESP_LOGW(TAG, "UART init failed");

    // Safely disable Motion Base motor controller at startup to prevent excessive current draw.
    // Non-fatal: absence of a base or an init failure must not take down the main stack.
    if (motion_ctrl_init() != ESP_OK) ESP_LOGW(TAG, "motion_ctrl init failed");

    sensor_drivers_register();

    ESP_ERROR_CHECK(scheduler_init());
    scheduler_set_reading_cb(on_reading);

    ESP_ERROR_CHECK(ble_svc_init());
    ble_svc_set_notify_min_us(config_store_get_polling_cap_us());

    // Button controller — handles button presses independently of display.
    // Non-fatal: boards without a button GPIO skip initialization.
    if (button_ctrl_init() != ESP_OK) ESP_LOGW(TAG, "button_ctrl init failed");

    // LEGO color-sensor emitter — idle until enabled in its config over BLE.
    // Non-fatal: a problem here must not take down the sensor/BLE stack.
    if (lego_emit_init() != ESP_OK) ESP_LOGW(TAG, "lego_emit init failed — continuing");
    lego_emit_set_matrix_cb(ble_svc_on_matrix);   // 3×3 matrix pixels → frontend grid

    // BLE-HID game controller host (NimBLE central, shares the host started by ble_svc).
    // Non-fatal: a problem here must not take down the sensor/BLE stack.
    if (hid_host_init() != ESP_OK) ESP_LOGW(TAG, "hid_host init failed — continuing");
    hid_host_set_status_cb(ble_svc_on_hid);       // controller connect/disconnect → frontend

    // Configurable status display — after the buses/scheduler/BLE it reads from.
    // Non-fatal: a display problem must not take down the sensor/BLE stack.
    if (display_init() != ESP_OK) ESP_LOGW(TAG, "display init failed — continuing without screen");

    // Apply the loaded config and begin polling.
    scheduler_rebuild();
    scheduler_start();

    // config_store_get_device_name(), not BOARD_BLE_NAME — the compile-time default only
    // applies until the board is renamed (or on a fresh/factory-reset boot); logging the
    // constant here always showed the original default even after a rename, contradicting
    // ble_svc's own "advertising as" log line right above it.
    ESP_LOGI(TAG, "ready (config v%u, advertising as \"%s\")",
             (unsigned)config_store_version(), config_store_get_device_name());
}
