#include "bus_spi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "board_config.h"

static const char *TAG = "bus_spi";
#define SPI_CLOCK_HZ (1 * 1000 * 1000)
#define SPI_RDBUF_MAX 32

#ifndef BOARD_DISPLAY_CS_INDEX
#define BOARD_DISPLAY_CS_INDEX -1   // CS index owned by the display (esp_lcd), not bus_spi
#endif

// An ESP32-S3 SPI host has only 3 hardware CS lines, so to support up to 5 CS we drive
// the CS GPIOs manually and register a single device with software CS (spics_io_num = -1).
static const int s_cs_gpios[BOARD_SPI_CS_COUNT] = BOARD_SPI_CS_GPIOS;
static spi_device_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static bool s_inited;

esp_err_t bus_spi_init(void)
{
    if (s_inited) return ESP_OK;

#if BOARD_SPI_CS_COUNT == 0
    // No SPI sensor CS lines wired on this board (no shared SPI bus at all, or the only
    // SPI device is the display on its own internal bus) — nothing to initialize here.
    // Deliberately leave s_inited false: on boards with a display (e.g. BOARD_ATOMS3R),
    // display.c checks bus_spi_is_initialized() to decide whether it must bring up
    // BOARD_TFT_HOST itself; it must see false so it does that.
    return ESP_OK;
#endif

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SPI_MOSI_GPIO,
        .miso_io_num = BOARD_SPI_MISO_GPIO,
        .sclk_io_num = BOARD_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,   // large enough for the display's line strips (shared bus)
    };
    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    // The TFT shares this host; whichever inits first wins. INVALID_STATE = already up.
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI bus already initialized (shared with TFT)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    // Configure every wired sensor CS GPIO as an output, idle HIGH (CS is active-low).
    // The display's CS index is left alone — esp_lcd drives it.
    uint64_t mask = 0;
    int wired = 0;
    for (int i = 0; i < BOARD_SPI_CS_COUNT; i++) {
        if (s_cs_gpios[i] < 0 || i == BOARD_DISPLAY_CS_INDEX) continue;
        mask |= 1ULL << s_cs_gpios[i];
        wired++;
    }
    if (mask) {
        gpio_config_t cs = { .pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&cs);
        for (int i = 0; i < BOARD_SPI_CS_COUNT; i++)
            if (s_cs_gpios[i] >= 0) gpio_set_level(s_cs_gpios[i], 1);
    }

    // One shared device with software CS; we toggle the right GPIO per transfer.
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(BOARD_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "SPI up: %d CS line(s) wired (software CS)", wired);
    return ESP_OK;
}

bool bus_spi_is_initialized(void)
{
    return s_inited;
}

bool bus_spi_cs_valid(int cs_index)
{
    return cs_index >= 0 && cs_index < BOARD_SPI_CS_COUNT &&
           cs_index != BOARD_DISPLAY_CS_INDEX && s_cs_gpios[cs_index] >= 0;
}

esp_err_t bus_spi_transfer(int cs_index, const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!bus_spi_cs_valid(cs_index)) return ESP_ERR_INVALID_ARG;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Reserve the shared bus for the whole CS-asserted window so the TFT (another device
    // on this host) cannot clock data while this sensor is selected.
    esp_err_t err = spi_device_acquire_bus(s_dev, portMAX_DELAY);
    if (err == ESP_OK) {
        gpio_set_level(s_cs_gpios[cs_index], 0);       // assert CS
        err = spi_device_polling_transmit(s_dev, &t);
        gpio_set_level(s_cs_gpios[cs_index], 1);       // release CS
        spi_device_release_bus(s_dev);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bus_spi_read_reg(int cs_index, uint8_t reg, uint8_t *buf, size_t len)
{
    if (len + 1 > SPI_RDBUF_MAX) return ESP_ERR_INVALID_SIZE;
    uint8_t tx[SPI_RDBUF_MAX] = {0};
    uint8_t rx[SPI_RDBUF_MAX] = {0};
    tx[0] = reg;                                   // caller embeds read/auto-inc bits
    esp_err_t err = bus_spi_transfer(cs_index, tx, rx, len + 1);
    if (err == ESP_OK) memcpy(buf, &rx[1], len);   // first byte clocked out during cmd
    return err;
}
