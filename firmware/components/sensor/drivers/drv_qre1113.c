// drv_qre1113.c — QRE1113 analogue reflectance sensor (line-following black/white detection),
// read through one or more channels of an MCP3208 SPI ADC (see mcp3208.h). cs_index selects the
// MCP3208 chip; cfg->channel_mask (bit i = channel i) selects a group of channels read as one
// sensor (a line-sensor bar), or cfg->port selects a single legacy channel when channel_mask is 0.
//
// Raw output is the ADC count per channel; the "line_reflect" transform (sensor_transform.c)
// maps each channel to 0.0 (white) .. 1.0 (black) + a digital detected bit, using a two-point
// calibration (calib[0]=white ref, calib[1]=black ref) shared across the whole group, captured
// via the calibrate command's "point" field ("white"/"black").
#include "mcp3208.h"
#include "sensor.h"
#include "esp_log.h"

static const char *TAG = "drv_qre1113";

static esp_err_t qre1113_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;

    if (cfg->channel_mask == 0) {                        // legacy single-channel mode
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

    int n = 0;
    for (int ch = 0; ch < 8 && n < max; ch++) {
        if (!(cfg->channel_mask & (1u << ch))) continue;
        uint16_t raw = 0;
        esp_err_t err = mcp3208_read_raw(cfg->cs_index, ch, &raw);
        if (err != ESP_OK) return err;
        out[n++] = (float)raw;
    }
    *out_count = n;
    return ESP_OK;
}

static int qre1113_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    static const char *ch_names[8] = { "ch0", "ch1", "ch2", "ch3", "ch4", "ch5", "ch6", "ch7" };
    if (cfg->channel_mask == 0) {
        if (max < 1) return 0;
        names[0] = "counts";
        return 1;
    }
    int n = 0;
    for (int ch = 0; ch < 8 && n < max; ch++)
        if (cfg->channel_mask & (1u << ch)) names[n++] = ch_names[ch];
    return n;
}

const sensor_driver_t drv_qre1113 = { .type = "qre1113", .probe = NULL, .read = qre1113_read, .describe = qre1113_describe };
