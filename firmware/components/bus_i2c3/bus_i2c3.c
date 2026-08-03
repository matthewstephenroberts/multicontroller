#include "bus_i2c3.h"
#include "board_config.h"

#if defined(BOARD_MOTION_I2C_SDA_GPIO)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "bus_i2c3";

// ~100kHz: I2C standard-mode timing minimums (T_LOW/T_HIGH ~4-4.7us) are comfortably satisfied
// by a 5us half-period.
#define HALF_PERIOD_US 5
// Bounded wait for a slave stretching SCL low — most simple I2C slaves (INA226 included) don't
// stretch at all, so this only matters as a hang guard, not a real feature.
#define CLOCK_STRETCH_LOOPS 1000

static SemaphoreHandle_t s_lock;
static bool s_ready;

static inline void sda_release(void) { gpio_set_level(BOARD_MOTION_I2C_SDA_GPIO, 1); }
static inline void sda_low(void)     { gpio_set_level(BOARD_MOTION_I2C_SDA_GPIO, 0); }
static inline int  sda_read(void)    { return gpio_get_level(BOARD_MOTION_I2C_SDA_GPIO); }

static inline void scl_release(void)
{
    gpio_set_level(BOARD_MOTION_I2C_SCL_GPIO, 1);
    int n = CLOCK_STRETCH_LOOPS;
    while (!gpio_get_level(BOARD_MOTION_I2C_SCL_GPIO) && n--) esp_rom_delay_us(1);
}
static inline void scl_low(void) { gpio_set_level(BOARD_MOTION_I2C_SCL_GPIO, 0); }

esp_err_t bus_i2c3_init(void)
{
    if (s_ready) return ESP_OK;

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_MOTION_I2C_SDA_GPIO) | (1ULL << BOARD_MOTION_I2C_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,   // open-drain: level 1 = released (pulled up), 0 = driven low
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }
    sda_release();
    scl_release();
    s_ready = true;
    ESP_LOGI(TAG, "I2C3 (Motion Base, bit-banged) up on SDA=%d SCL=%d", BOARD_MOTION_I2C_SDA_GPIO, BOARD_MOTION_I2C_SCL_GPIO);
    return ESP_OK;
}

static void i2c_start(void)
{
    sda_release(); scl_release(); esp_rom_delay_us(HALF_PERIOD_US);
    sda_low();      esp_rom_delay_us(HALF_PERIOD_US);
    scl_low();      esp_rom_delay_us(HALF_PERIOD_US);
}

// Repeated start: same shape as i2c_start(), kept separate for readability at call sites.
static void i2c_rstart(void) { i2c_start(); }

static void i2c_stop(void)
{
    sda_low();     esp_rom_delay_us(HALF_PERIOD_US);
    scl_release(); esp_rom_delay_us(HALF_PERIOD_US);
    sda_release(); esp_rom_delay_us(HALF_PERIOD_US);
}

static void i2c_write_bit(int bit)
{
    if (bit) sda_release(); else sda_low();
    esp_rom_delay_us(HALF_PERIOD_US);
    scl_release(); esp_rom_delay_us(HALF_PERIOD_US);
    scl_low();
}

static int i2c_read_bit(void)
{
    sda_release(); esp_rom_delay_us(HALF_PERIOD_US);
    scl_release(); esp_rom_delay_us(HALF_PERIOD_US);
    int bit = sda_read();
    scl_low();
    return bit;
}

// Returns true on ACK.
static bool i2c_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) i2c_write_bit((b >> i) & 1);
    return i2c_read_bit() == 0;   // slave pulls SDA low for ACK
}

static uint8_t i2c_read_byte(bool ack)
{
    uint8_t b = 0;
    for (int i = 7; i >= 0; i--) b = (b << 1) | i2c_read_bit();
    i2c_write_bit(ack ? 0 : 1);   // master ACKs all but the final byte
    return b;
}

esp_err_t bus_i2c3_probe(uint8_t addr)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    i2c_start();
    bool acked = i2c_write_byte((addr << 1) | 0);
    i2c_stop();
    xSemaphoreGiveRecursive(s_lock);
    return acked ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t bus_i2c3_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    i2c_start();
    if (!i2c_write_byte((addr << 1) | 0)) { err = ESP_ERR_NOT_FOUND; goto done; }
    for (size_t i = 0; i < len; i++)
        if (!i2c_write_byte(data[i])) { err = ESP_FAIL; goto done; }
done:
    i2c_stop();
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t bus_i2c3_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    i2c_start();
    if (!i2c_write_byte((addr << 1) | 0)) { err = ESP_ERR_NOT_FOUND; goto done; }
    if (!i2c_write_byte(reg)) { err = ESP_FAIL; goto done; }
    i2c_rstart();
    if (!i2c_write_byte((addr << 1) | 1)) { err = ESP_ERR_NOT_FOUND; goto done; }
    for (size_t i = 0; i < len; i++) buf[i] = i2c_read_byte(i + 1 < len);
done:
    i2c_stop();
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t bus_i2c3_read(uint8_t addr, uint8_t *buf, size_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    i2c_start();
    if (!i2c_write_byte((addr << 1) | 1)) { err = ESP_ERR_NOT_FOUND; goto done; }
    for (size_t i = 0; i < len; i++) buf[i] = i2c_read_byte(i + 1 < len);
done:
    i2c_stop();
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

void bus_i2c3_lock(void)   { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void bus_i2c3_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

#else // !defined(BOARD_MOTION_I2C_SDA_GPIO) — board with no Motion Base bottom header at all

esp_err_t bus_i2c3_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bus_i2c3_write(uint8_t addr, const uint8_t *data, size_t len) { (void)addr; (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bus_i2c3_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) { (void)addr; (void)reg; (void)buf; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bus_i2c3_read(uint8_t addr, uint8_t *buf, size_t len) { (void)addr; (void)buf; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bus_i2c3_probe(uint8_t addr) { (void)addr; return ESP_ERR_NOT_SUPPORTED; }
void bus_i2c3_lock(void) {}
void bus_i2c3_unlock(void) {}

#endif
