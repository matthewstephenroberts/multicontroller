// panel_st7735s.h — esp_lcd panel driver for the ST7735S 132x132-memory IPS controller. The
// M5Stack AtomS3R ships with one of two different onboard panel ICs depending on hardware
// revision (GC9107 or this one) — see panel_gc9107.c for the other. If gc9107 reports success
// but nothing renders, the unit is very likely this variant instead.
#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
