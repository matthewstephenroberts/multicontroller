// panel_st7735s.c — esp_lcd panel driver for the ST7735S 132x132-memory IPS LCD controller.
// The M5Stack AtomS3R ships with one of two different panel ICs depending on hardware revision
// (this one, or GC9107 — see panel_gc9107.c) with no visible external marking to tell them
// apart; M5Stack's own firmware auto-detects which one is present by reading a panel-ID
// register over a 3-wire SPI read this codebase's display bus doesn't support (MISO isn't
// wired — BOARD_TFT_MISO_GPIO is -1, display-only). Structured the same way as panel_gc9107.c
// (modeled on the esp_lcd_ili9341 managed component); init command table taken verbatim from
// M5GFX (src/lgfx/v1/panel/Panel_ST7735.hpp, struct Panel_ST7735S combined with its Panel_ST7735
// base class's shared command IDs) — the ST7735S has a real public datasheet, unlike GC9107,
// but this table is still the one M5Stack's own AtomS3R support actually uses.
#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "panel_st7735s.h"

static const char *TAG = "st7735s";

static esp_err_t panel_st7735s_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_st7735s_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7735s_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7735s_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7735s_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7735s_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
} st7735s_panel_t;

esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st7735s_panel_t *p = NULL;
    gpio_config_t io_conf = { 0 };

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    p = (st7735s_panel_t *)calloc(1, sizeof(st7735s_panel_t));
    ESP_GOTO_ON_FALSE(p, ESP_ERR_NO_MEM, err, TAG, "no mem for st7735s panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        p->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        p->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb element order");
        break;
    }

    ESP_GOTO_ON_FALSE(panel_dev_config->bits_per_pixel == 16, ESP_ERR_NOT_SUPPORTED, err, TAG, "st7735s only supports 16bpp (RGB565)");
    p->fb_bits_per_pixel = 16;

    p->io = io;
    p->reset_gpio_num = panel_dev_config->reset_gpio_num;
    p->reset_level = panel_dev_config->flags.reset_active_high;
    p->base.del = panel_st7735s_del;
    p->base.reset = panel_st7735s_reset;
    p->base.init = panel_st7735s_init;
    p->base.draw_bitmap = panel_st7735s_draw_bitmap;
    p->base.invert_color = panel_st7735s_invert_color;
    p->base.set_gap = panel_st7735s_set_gap;
    p->base.mirror = panel_st7735s_mirror;
    p->base.swap_xy = panel_st7735s_swap_xy;
    p->base.disp_on_off = panel_st7735s_disp_on_off;
    *ret_panel = &(p->base);
    ESP_LOGD(TAG, "new st7735s panel @%p", p);

    return ESP_OK;

err:
    if (p) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(p);
    }
    return ret;
}

static esp_err_t panel_st7735s_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7735s panel @%p", p);
    free(p);
    return ESP_OK;
}

static esp_err_t panel_st7735s_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(150));   // matches the init table's own post-SWRESET delay below
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return ESP_OK;
}

// {cmd, data..., data_bytes, delay_ms} — verbatim from M5GFX's Panel_ST7735S::getInitCommands()
// (Rcmd1 + Rcmd2 concatenated; both lists always run together, so there's no reason to keep
// them separate here). A 255ms delay in the source list is lgfx's own "500ms" sentinel — spelled
// out directly below instead of replicating that convention.
typedef struct { uint8_t cmd; uint8_t data[16]; uint8_t data_bytes; uint16_t delay_ms; } st7735s_cmd_t;
static const st7735s_cmd_t s_init_cmds[] = {
    { 0xB1, {0x01,0x2C,0x2D}, 3, 0 },      // FRMCTR1
    { 0xB2, {0x01,0x2C,0x2D}, 3, 0 },      // FRMCTR2
    { 0xB3, {0x01,0x2C,0x2D,0x01,0x2C,0x2D}, 6, 0 },   // FRMCTR3
    { 0xB4, {0x07}, 1, 0 },                // INVCTR
    { 0xC0, {0xA2,0x02,0x84}, 3, 0 },      // PWCTR1
    { 0xC1, {0xC5}, 1, 0 },                // PWCTR2
    { 0xC2, {0x0A,0x00}, 2, 0 },           // PWCTR3
    { 0xC3, {0x8A,0x2A}, 2, 0 },           // PWCTR4
    { 0xC4, {0x8A,0xEE}, 2, 0 },           // PWCTR5
    { 0xC5, {0x0E}, 1, 0 },                // VMCTR1
    { 0xE0, {0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10}, 16, 0 },  // GMCTRP1
    { 0xE1, {0x03,0x1d,0x07,0x06,0x2E,0x2C,0x29,0x2D,0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10}, 16, 0 },  // GMCTRN1
};

static esp_err_t panel_st7735s_init(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(500));

    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const st7735s_cmd_t *c = &s_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, c->cmd, c->data, c->data_bytes), TAG, "send command failed");
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        p->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        0x55,   // RGB565
    }, 1), TAG, "send command failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_NORON, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGD(TAG, "send init commands success");
    return ESP_OK;
}

static esp_err_t panel_st7735s_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = p->io;

    x_start += p->x_gap;
    x_end += p->x_gap;
    y_start += p->y_gap;
    y_end += p->y_gap;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF, x_start & 0xFF, ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF, y_start & 0xFF, ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    size_t len = (x_end - x_start) * (y_end - y_start) * p->fb_bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);
    return ESP_OK;
}

static esp_err_t panel_st7735s_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7735s_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    if (mirror_x) p->madctl_val |= LCD_CMD_MX_BIT; else p->madctl_val &= ~LCD_CMD_MX_BIT;
    if (mirror_y) p->madctl_val |= LCD_CMD_MY_BIT; else p->madctl_val &= ~LCD_CMD_MY_BIT;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        p->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7735s_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    if (swap_axes) p->madctl_val |= LCD_CMD_MV_BIT; else p->madctl_val &= ~LCD_CMD_MV_BIT;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        p->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7735s_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7735s_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7735s_panel_t *p = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
