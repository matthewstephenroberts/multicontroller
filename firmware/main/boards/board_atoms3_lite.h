// board_atoms3_lite.h — pin map for the M5Stack AtomS3 Lite (ESP32-S3FN8).
// Included only from board_config.h when BOARD_ATOMS3_LITE is defined there — don't include
// directly, and don't add an include guard beyond board_config.h's own #pragma once (this file
// is textually spliced into exactly one translation of board_config.h per build).
//
// No display, no exposed SPI — this board has no chip-select lines wired out, so the web UI's
// SPI sensor config is moot (BOARD_SPI_CS_COUNT 0, inherited below).

#define BOARD_NAME "M5Stack AtomS3 Lite"

// Peripheral power enable, DA pins, Motion Base I2C, user button, aux UART (Port B), LEGO LPF2
// UART (Port C), and the "no SPI on this family" defaults all come from the shared Atom-family
// snippet — see board_m5_atom_motion_base.h for the wiring rationale on each.
#include "board_m5_atom_motion_base.h"

// ---- UART (auxiliary port) override ----
// Disabled for now (-1/-1): no sensor support available yet on this board, so hide UART config
// from the UI. Restore GPIO_NUM_7/GPIO_NUM_8 (the shared snippet's defaults) to re-enable once
// sensor support is added.
#undef BOARD_UART_TX_GPIO
#undef BOARD_UART_RX_GPIO
#define BOARD_UART_TX_GPIO   -1
#define BOARD_UART_RX_GPIO   -1

// ---- I2C (Grove/HY2.0-4P port, "Port.A") ----
#define BOARD_I2C_PORT       0
#define BOARD_I2C_SDA_GPIO   GPIO_NUM_2
#define BOARD_I2C_SCL_GPIO   GPIO_NUM_1
#define BOARD_I2C_FREQ_HZ    400000

// ---- Onboard display ----
// AtomS3 Lite has no screen at all.
#define BOARD_HAS_DISPLAY    0
#define BOARD_TFT_BL_GPIO        -1
#define BOARD_TFT_BL_I2C_PORT     1
#define BOARD_TFT_BL_I2C_SDA_GPIO -1
#define BOARD_TFT_BL_I2C_SCL_GPIO -1
#define BOARD_TFT_HOST       SPI2_HOST
#define BOARD_TFT_SCLK_GPIO  -1
#define BOARD_TFT_MOSI_GPIO  -1
#define BOARD_TFT_MISO_GPIO  -1
#define BOARD_TFT_CS_GPIO    -1
#define BOARD_TFT_DC_GPIO    -1
#define BOARD_TFT_RST_GPIO   -1
#define BOARD_TFT_BL_GPIO    -1
#define BOARD_TFT_WIDTH      0
#define BOARD_TFT_HEIGHT     0
#define BOARD_TFT_X_GAP      0
#define BOARD_TFT_Y_GAP      0
#define BOARD_TFT_MIRROR_X   false
#define BOARD_TFT_MIRROR_Y   false
#define BOARD_TFT_INVERT     false
