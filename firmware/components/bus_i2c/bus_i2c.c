#include "bus_i2c.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "board_config.h"

static const char *TAG = "bus_i2c";
#define I2C_TIMEOUT_MS       100
#define I2C_PROBE_TIMEOUT_MS 20    // keep bus scans snappy on absent/stuck addresses
#define DEV_CACHE_MAX        32

static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_lock;

// Small cache of per-address device handles (the new driver keys transactions on a handle).
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} s_cache[DEV_CACHE_MAX];
static int s_cache_n;

esp_err_t bus_i2c_init(void)
{
    if (s_bus) return ESP_OK;

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_PORT,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C up on SDA=%d SCL=%d @%dHz",
             BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, BOARD_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t bus_i2c_handle(void) { return s_bus; }

// Get (or lazily add) a device handle for a 7-bit address. Caller holds s_lock.
static esp_err_t get_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    for (int i = 0; i < s_cache_n; i++) {
        if (s_cache[i].addr == addr) { *out = s_cache[i].dev; return ESP_OK; }
    }
    if (s_cache_n >= DEV_CACHE_MAX) return ESP_ERR_NO_MEM;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dcfg, &dev);
    if (err != ESP_OK) return err;

    s_cache[s_cache_n].addr = addr;
    s_cache[s_cache_n].dev = dev;
    s_cache_n++;
    *out = dev;
    return ESP_OK;
}

esp_err_t bus_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = get_dev(addr, &dev);
    if (err == ESP_OK) err = i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS);
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t bus_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t dev;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = get_dev(addr, &dev);
    if (err == ESP_OK)
        err = i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t bus_i2c_read(uint8_t addr, uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t dev;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = get_dev(addr, &dev);
    if (err == ESP_OK) err = i2c_master_receive(dev, buf, len, I2C_TIMEOUT_MS);
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t bus_i2c_probe(uint8_t addr)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_probe(s_bus, addr, I2C_PROBE_TIMEOUT_MS);
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

void bus_i2c_lock(void)   { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void bus_i2c_unlock(void) { xSemaphoreGiveRecursive(s_lock); }
