// panel_gc9107.c — esp_lcd panel driver for the GC9107 128x160-memory IPS LCD controller (the
// M5Stack AtomS3R's onboard 0.85" display). Structured the same way as the esp_lcd_ili9341
// managed component (see ../../managed_components/espressif__esp_lcd_ili9341) since esp-idf and
// the component registry have no GC9107 driver at all — this board's display previously ran the
// generic st7789 driver's init sequence against it, which the chip doesn't understand (different
// register map/proprietary "unlock" commands), so it never lit up.
//
// The 0xFE/0xEF unlock + 0xB0.. vendor register block below is taken verbatim from M5Stack's own
// M5GFX (src/lgfx/v1/panel/Panel_GC9A01.hpp, struct Panel_GC9107) — the only public, known-good
// reference for this exact chip, since GC9107 has no public datasheet. MADCTL/COLMOD are sent
// explicitly here (M5GFX's own list relies on the chip's power-on default for both, which this
// esp_lcd-style driver doesn't want to assume).
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

#include "panel_gc9107.h"

static const char *TAG = "gc9107";

static esp_err_t panel_gc9107_del(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9107_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9107_init(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9107_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_gc9107_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_gc9107_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_gc9107_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_gc9107_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_gc9107_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;   // current LCD_CMD_MADCTL value
} gc9107_panel_t;

esp_err_t esp_lcd_new_panel_gc9107(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    gc9107_panel_t *gc9107 = NULL;
    gpio_config_t io_conf = { 0 };

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    gc9107 = (gc9107_panel_t *)calloc(1, sizeof(gc9107_panel_t));
    ESP_GOTO_ON_FALSE(gc9107, ESP_ERR_NO_MEM, err, TAG, "no mem for gc9107 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        gc9107->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        gc9107->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb element order");
        break;
    }

    ESP_GOTO_ON_FALSE(panel_dev_config->bits_per_pixel == 16, ESP_ERR_NOT_SUPPORTED, err, TAG, "gc9107 only supports 16bpp (RGB565)");
    gc9107->fb_bits_per_pixel = 16;

    gc9107->io = io;
    gc9107->reset_gpio_num = panel_dev_config->reset_gpio_num;
    gc9107->reset_level = panel_dev_config->flags.reset_active_high;
    gc9107->base.del = panel_gc9107_del;
    gc9107->base.reset = panel_gc9107_reset;
    gc9107->base.init = panel_gc9107_init;
    gc9107->base.draw_bitmap = panel_gc9107_draw_bitmap;
    gc9107->base.invert_color = panel_gc9107_invert_color;
    gc9107->base.set_gap = panel_gc9107_set_gap;
    gc9107->base.mirror = panel_gc9107_mirror;
    gc9107->base.swap_xy = panel_gc9107_swap_xy;
    gc9107->base.disp_on_off = panel_gc9107_disp_on_off;
    *ret_panel = &(gc9107->base);
    ESP_LOGD(TAG, "new gc9107 panel @%p", gc9107);

    return ESP_OK;

err:
    if (gc9107) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(gc9107);
    }
    return ret;
}

static esp_err_t panel_gc9107_del(esp_lcd_panel_t *panel)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    if (gc9107->reset_gpio_num >= 0) {
        gpio_reset_pin(gc9107->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del gc9107 panel @%p", gc9107);
    free(gc9107);
    return ESP_OK;
}

static esp_err_t panel_gc9107_reset(esp_lcd_panel_t *panel)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;

    if (gc9107->reset_gpio_num >= 0) {
        gpio_set_level(gc9107->reset_gpio_num, gc9107->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9107->reset_gpio_num, !gc9107->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));   // GC9107 needs longer than the usual 10ms post-reset settle
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

// {cmd, data..., data_bytes} — verbatim from M5GFX's Panel_GC9107::getInitCommands(), minus the
// 0xFE/0xEF init-delay markers (handled below) and the trailing SLPOUT/DISPON (sent explicitly
// after MADCTL/COLMOD so those two take effect before the panel leaves sleep).
typedef struct { uint8_t cmd; uint8_t data[14]; uint8_t data_bytes; } gc9107_cmd_t;
static const gc9107_cmd_t s_unlock_and_vendor_init[] = {
    {0xB0, {0xC0}, 1},
    {0xB2, {0x2F}, 1},
    {0xB3, {0x03}, 1},
    {0xB6, {0x19}, 1},
    {0xB7, {0x01}, 1},
    {0xAC, {0xCB}, 1},
    {0xAB, {0x0E}, 1},
    {0xB4, {0x04}, 1},
    {0xA8, {0x19}, 1},
    {0xB8, {0x08}, 1},
    {0xE8, {0x24}, 1},
    {0xE9, {0x48}, 1},
    {0xEA, {0x22}, 1},
    {0xC6, {0x30}, 1},
    {0xC7, {0x18}, 1},
    {0xF0, {0x01,0x2b,0x23,0x3c,0xb7,0x12,0x17,0x60,0x00,0x06,0x0c,0x17,0x12,0x1f}, 14},
    {0xF1, {0x05,0x2e,0x2d,0x44,0xd6,0x15,0x17,0xa0,0x02,0x0d,0x0d,0x1a,0x18,0x1f}, 14},
};

static esp_err_t panel_gc9107_init(esp_lcd_panel_t *panel)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;

    // Command-set unlock — required before the 0xB0../0xF1 proprietary registers below are
    // recognised at all. 5ms settle after each, per M5GFX.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xFE, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xEF, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(5));

    for (size_t i = 0; i < sizeof(s_unlock_and_vendor_init) / sizeof(s_unlock_and_vendor_init[0]); i++) {
        const gc9107_cmd_t *c = &s_unlock_and_vendor_init[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, c->cmd, c->data, c->data_bytes), TAG, "send command failed");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9107->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        0x55,   // RGB565
    }, 1), TAG, "send command failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0), TAG, "send command failed");

    ESP_LOGD(TAG, "send init commands success");
    return ESP_OK;
}

static esp_err_t panel_gc9107_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = gc9107->io;

    x_start += gc9107->x_gap;
    x_end += gc9107->x_gap;
    y_start += gc9107->y_gap;
    y_end += gc9107->y_gap;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF, x_start & 0xFF, ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF, y_start & 0xFF, ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    size_t len = (x_end - x_start) * (y_end - y_start) * gc9107->fb_bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);
    return ESP_OK;
}

static esp_err_t panel_gc9107_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9107_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;
    if (mirror_x) gc9107->madctl_val |= LCD_CMD_MX_BIT; else gc9107->madctl_val &= ~LCD_CMD_MX_BIT;
    if (mirror_y) gc9107->madctl_val |= LCD_CMD_MY_BIT; else gc9107->madctl_val &= ~LCD_CMD_MY_BIT;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9107->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9107_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;
    if (swap_axes) gc9107->madctl_val |= LCD_CMD_MV_BIT; else gc9107->madctl_val &= ~LCD_CMD_MV_BIT;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9107->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9107_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    gc9107->x_gap = x_gap;
    gc9107->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_gc9107_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
