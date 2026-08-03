// drv_ir_ball.c — TSSP4038 / TSOP34840 demodulating IR receiver for 40kHz modulated IR
// object detection, read through one or more channels of an MCP3208 SPI ADC (see mcp3208.h).
// cs_index selects the MCP3208 chip; cfg->channel_mask (bit i = channel i) selects a group of
// channels read as one sensor (an IR-receiver ring), or cfg->port selects a single legacy
// channel when channel_mask is 0.
//
// Both parts pass ~40kHz modulated IR (TSSP4038 fixed-gain @38kHz, TSOP34840 AGC @40kHz).
// Each part's internal filter turns the carrier into a slower envelope that dips (idles high,
// pulled low on detection) when it sees a burst, dipping further as the signal strengthens
// (closer object). read() bursts many back-to-back ADC samples per channel — a single slow
// sample could miss a short burst entirely — and hands the sensor pipeline just the deepest
// dip seen per channel (the burst itself never leaves this driver; the shared read()/transform
// buffers are sized for decoded values, not raw samples).
//
// The "ir_ball" transform (sensor_transform.c) turns each channel's minimum into a detected
// flag + a normalised strength against the sensor's idle baseline; calib[0] is that baseline
// (no object present, shared across the whole group), captured via the one-shot calibrate
// command like the dist_* zero-offset.
#include "mcp3208.h"
#include "sensor.h"
#include "esp_log.h"

static const char *TAG = "drv_ir_ball";

// Single-channel burst: enough samples to almost certainly catch a burst within the sample
// window regardless of poll phase, without holding the shared SPI bus for too long.
#define IR_BALL_BURST_SAMPLES 160

// Grouped mode splits a fixed total sample budget across the selected channels instead of
// bursting the full single-channel count on *each* one — an 8-channel ring at the single-channel
// burst size would be 8×160 = 1280 SPI transactions per poll, however many ms that adds up to.
// Capping the total keeps one poll's SPI/CPU cost roughly constant regardless of group size (at
// the cost of a shorter, slightly less certain burst window per channel as the group grows).
#define IR_BALL_GROUP_BUDGET   320
#define IR_BALL_GROUP_MIN      40      // per-channel floor even for a large group

static uint16_t burst_min(int8_t cs_index, int channel, int samples)
{
    uint16_t min_raw = UINT16_MAX;
    for (int i = 0; i < samples; i++) {
        uint16_t raw = 0;
        if (mcp3208_read_raw(cs_index, channel, &raw) != ESP_OK) continue;
        if (raw < min_raw) min_raw = raw;
    }
    return min_raw;
}

static esp_err_t ir_ball_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    if (max < 1) return ESP_ERR_INVALID_SIZE;

    if (cfg->channel_mask == 0) {                         // legacy single-channel mode
        if (cfg->port < 0 || cfg->port > 7) {
            ESP_LOGW(TAG, "channel %d out of range 0..7", cfg->port);
            return ESP_ERR_INVALID_ARG;
        }
        out[0] = (float)burst_min(cfg->cs_index, cfg->port, IR_BALL_BURST_SAMPLES);
        *out_count = 1;
        return ESP_OK;
    }

    int chan_count = 0;
    for (int ch = 0; ch < 8; ch++) if (cfg->channel_mask & (1u << ch)) chan_count++;
    if (chan_count == 0) return ESP_ERR_INVALID_ARG;
    int per_channel = IR_BALL_GROUP_BUDGET / chan_count;
    if (per_channel < IR_BALL_GROUP_MIN) per_channel = IR_BALL_GROUP_MIN;

    int n = 0;
    for (int ch = 0; ch < 8 && n < max; ch++) {
        if (!(cfg->channel_mask & (1u << ch))) continue;
        out[n++] = (float)burst_min(cfg->cs_index, ch, per_channel);
    }
    *out_count = n;
    return ESP_OK;
}

static int ir_ball_describe(const sensor_cfg_t *cfg, const char *names[], int max)
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

const sensor_driver_t drv_ir_ball = { .type = "tssp_ir", .probe = NULL, .read = ir_ball_read, .describe = ir_ball_describe };
