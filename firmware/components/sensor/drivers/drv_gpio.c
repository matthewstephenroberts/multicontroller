// drv_gpio.c — the five board "DIGITAL ANALOGUE" pins (BOARD_DA0..DA4) as sensors.
//
//   type "gpio" — digital input: out[0] = 0/1. Pull from recipe.reg (0 none, 1 up, 2 down).
//   type "adc"  — analog input:  out[0] = raw ADC counts 0..4095 (use the adc_volts transform
//                 for volts). GPIO14-18 are ADC2 channels.
//
// Both are non-bus drivers: read() ignores cfg->bus/addr and uses cfg->port as the GPIO. The
// pin is validated against the board's DA allow-list so a stray config can't drive an I2C/SPI
// /UART/display pin.

#include "sensor.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "drv_gpio";

static bool pin_allowed(int gpio)
{
    static const int da[] = BOARD_DA_GPIOS;
    for (int i = 0; i < BOARD_DA_COUNT; i++) if (da[i] == gpio) return true;
    return false;
}

// ── Digital ─────────────────────────────────────────────────────────────────
static uint64_t s_gpio_configured;            // bitmask of GPIOs already set up (≤64)

static esp_err_t gpio_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    int pin = cfg->port;
    if (!pin_allowed(pin)) { ESP_LOGW(TAG, "gpio %d not a DA pin", pin); return ESP_ERR_INVALID_ARG; }
    if (max < 1) return ESP_ERR_INVALID_SIZE;

    if (!(s_gpio_configured & (1ULL << pin))) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en   = cfg->recipe.reg == 1 ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
            .pull_down_en = cfg->recipe.reg == 2 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&c) != ESP_OK) return ESP_FAIL;
        s_gpio_configured |= (1ULL << pin);
    }
    out[0] = (float)gpio_get_level(pin);
    *out_count = 1;
    return ESP_OK;
}

static int gpio_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg; if (max < 1) return 0; names[0] = "state"; return 1;
}

// ── Analog (ADC oneshot) ─────────────────────────────────────────────────────
static adc_oneshot_unit_handle_t s_adc[2];    // [0]=ADC1, [1]=ADC2 (created lazily)
static uint64_t s_adc_chan_configured;        // bitmask of GPIOs whose channel is configured

static esp_err_t adc_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    int pin = cfg->port;
    if (!pin_allowed(pin)) { ESP_LOGW(TAG, "adc gpio %d not a DA pin", pin); return ESP_ERR_INVALID_ARG; }
    if (max < 1) return ESP_ERR_INVALID_SIZE;

    adc_unit_t unit; adc_channel_t chan;
    if (adc_oneshot_io_to_channel(pin, &unit, &chan) != ESP_OK) {
        ESP_LOGW(TAG, "gpio %d has no ADC channel", pin); return ESP_ERR_NOT_SUPPORTED;
    }
    int u = (unit == ADC_UNIT_1) ? 0 : 1;
    if (!s_adc[u]) {
        adc_oneshot_unit_init_cfg_t ic = { .unit_id = unit };
        if (adc_oneshot_new_unit(&ic, &s_adc[u]) != ESP_OK) return ESP_FAIL;
    }
    if (!(s_adc_chan_configured & (1ULL << pin))) {
        adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_oneshot_config_channel(s_adc[u], chan, &cc) != ESP_OK) return ESP_FAIL;
        s_adc_chan_configured |= (1ULL << pin);
    }
    int raw = 0;
    esp_err_t e = adc_oneshot_read(s_adc[u], chan, &raw);
    if (e != ESP_OK) return e;                 // ADC2 may return BUSY if the radio is mid-op
    out[0] = (float)raw;
    *out_count = 1;
    return ESP_OK;
}

static int adc_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg; if (max < 1) return 0; names[0] = "counts"; return 1;
}

const sensor_driver_t drv_gpio = { .type = "gpio", .probe = NULL, .read = gpio_read, .describe = gpio_describe };
const sensor_driver_t drv_adc  = { .type = "adc",  .probe = NULL, .read = adc_read,  .describe = adc_describe };
