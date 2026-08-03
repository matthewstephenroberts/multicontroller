// panel_gc9107.h — esp_lcd panel driver for the GC9107 (128x160-memory IPS controller used by
// the M5Stack AtomS3R's onboard 0.85" LCD). Not command-compatible with ST7789 — see the
// register-unlock/init table in panel_gc9107.c.
#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_lcd_new_panel_gc9107(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
