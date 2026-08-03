// drv_mcp3208.c — MCP3208 8-channel 12-bit SPI ADC.
//
//   type "mcp3208" — raw channel read: out[0] = 0..4095 counts (use the existing "adc_volts"
//                    transform for volts, same as the onboard "adc" driver).
//
// cs_index selects the chip on the shared SPI bus (bus_spi.h); cfg->port selects the MCP3208
// channel 0..7 (reusing the field UART sensors use for their port — see sensor.h). Also exposes
// mcp3208_read_raw() for the "qre1113" and "tssp_ir" drivers, which are just this chip's channel
// reading plus sensor-specific math, not separately-addressed devices.
#include "mcp3208.h"
#include "sensor.h"
#include "bus_spi.h"
#include "esp_log.h"

static const char *TAG = "drv_mcp3208";

esp_err_t mcp3208_read_raw(int cs_index, int channel, uint16_t *out)
{
    if (!out || channel < 0 || channel > 7) return ESP_ERR_INVALID_ARG;

    // Single-ended conversion command: start bit, SGL/DIFF=1, then the 3-bit channel number,
    // followed by enough clocked-in bytes to shift the 12-bit result out on MISO.
    uint8_t tx[3] = {
        (uint8_t)(0x06 | (channel >> 2)),
        (uint8_t)((channel & 0x03) << 6),
        0x00,
    };
    uint8_t rx[3] = {0};
    esp_err_t err = bus_spi_transfer(cs_index, tx, rx, sizeof(tx));
    if (err != ESP_OK) return err;

    *out = (uint16_t)(((rx[1] & 0x0F) << 8) | rx[2]);
    return ESP_OK;
}

static esp_err_t mcp3208_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;
    if (cfg->port < 0 || cfg->port > 7) {
        ESP_LOGW(TAG, "channel %d out of range 0..7", cfg->port);
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw = 0;
    esp_err_t err = mcp3208_read_raw(cfg->cs_index, cfg->port, &raw);
    if (err != ESP_OK) return err;
    out[0] = (float)raw;
    *out_count = 1;
    return ESP_OK;
}

static int mcp3208_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg; if (max < 1) return 0; names[0] = "counts"; return 1;
}

const sensor_driver_t drv_mcp3208 = { .type = "mcp3208", .probe = NULL, .read = mcp3208_read, .describe = mcp3208_describe };
