// board_atoms3r.h — pin map for the M5Stack AtomS3R (ESP32-S3).
// Included only from board_config.h when BOARD_ATOMS3R is defined there — don't include
// directly, and don't add an include guard beyond board_config.h's own #pragma once (this file
// is textually spliced into exactly one translation of board_config.h per build).
//
// 0.85" GC9107/ST7735S IPS LCD (128x128 visible, controller memory larger — see below). The
// LCD's SPI bus is wired internally to the display only — there are no spare chip-selects for
// external SPI sensors (BOARD_SPI_CS_COUNT 0, inherited from the shared Atom-family snippet).
// The display itself still runs over SPI (BOARD_TFT_*), just not through the shared BOARD_SPI_*
// bus/CS list that external SPI sensors would use. Pins/init sequence/backlight cross-checked
// against M5Stack's own M5GFX AtomS3R bring-up (M5GFX.cpp's autodetect block) since this exact
// panel/board pairing has no public datasheet.

#define BOARD_NAME "M5Stack AtomS3R"

// Peripheral power enable, DA pins, Motion Base I2C, user button, aux UART (Port B), LEGO LPF2
// UART (Port C), and the "no SPI on this family" defaults all come from the shared Atom-family
// snippet — see board_m5_atom_motion_base.h for the wiring rationale on each. This board
// overrides the aux UART right below (not currently wired/supported on this revision).
#include "board_m5_atom_motion_base.h"

// ---- UART (auxiliary port) override ----
// Disabled for now (-1/-1): not currently supported/wired on this board, so it's turned off at
// the config level (has_uart reports false, hiding it from scan/UI) rather than left "available"
// with nothing usable behind it. Restore GPIO_NUM_7/GPIO_NUM_8 (the shared snippet's defaults,
// same as AtomS3 Lite's Port B) to re-enable once this revision supports it.
#undef BOARD_UART_TX_GPIO
#undef BOARD_UART_RX_GPIO
#define BOARD_UART_TX_GPIO   -1
#define BOARD_UART_RX_GPIO   -1

// ---- I2C (Port.A Grove connector) ----
// SDA/SCL were swapped (SDA=1/SCL=2) — M5Stack's AtomS3 family convention is consistently
// SCL=G2/SDA=G1 across the whole lineup (confirmed against AtomS3 Lite, which already had this
// right, and empirically: a scan found devices when compiled for AtomS3 Lite on this same
// physical board/wiring, but not for AtomS3R, on nothing but this swap).
#define BOARD_I2C_PORT       0
#define BOARD_I2C_SDA_GPIO   GPIO_NUM_2
#define BOARD_I2C_SCL_GPIO   GPIO_NUM_1
#define BOARD_I2C_FREQ_HZ    400000

// ---- Onboard 0.85" LCD (128x128 visible), internal-only SPI ----
// These pins are dedicated to the display and never shared with BOARD_SPI_*/BOARD_SPI_CS_GPIOS.
// CS/DC were previously swapped (CS=42/DC=14) — matched against M5Stack's own AtomS3R pinout
// (CS=14/DC=42); with them swapped the panel IC never even sees valid command/data framing, so
// nothing lit up regardless of init sequence.
//
// Panel controller: this board's actual unit reports as ST7735S, not GC9107 — despite earlier
// assumptions based on M5Stack's own AtomS3R using either chip depending on hardware revision
// (see panel_gc9107.c's own comment/driver, still available and selectable from the web UI's
// Display tab if a future revision needs it again). Geometry below (x_gap=2/y_gap=1/invert=true)
// matches ST7735S's 132x132 memory with a 2-row/1-col RAM offset, not GC9107's 128x160.
#define BOARD_HAS_DISPLAY    1
#define BOARD_TFT_CONTROLLER "st7735s"   // NOT st7789-compatible — see panel_st7735s.c
#define BOARD_TFT_HOST       SPI2_HOST
#define BOARD_TFT_SCLK_GPIO  GPIO_NUM_15
#define BOARD_TFT_MOSI_GPIO  GPIO_NUM_21
#define BOARD_TFT_MISO_GPIO  -1   // display-only bus; never reads
#define BOARD_TFT_CS_GPIO    GPIO_NUM_14
#define BOARD_TFT_DC_GPIO    GPIO_NUM_42
#define BOARD_TFT_RST_GPIO   GPIO_NUM_48
// The backlight is NOT a plain GPIO on this board — per M5Stack's own official AtomS3R pinout
// diagram, an LP5562 4-channel I2C RGB+White LED driver sits on a *dedicated internal* I2C bus
// (SCL=G0, SDA=G45 — same bus as the onboard BMI270 IMU/BMM150 magnetometer, "BL: G0 G45" per
// that diagram), separate from BOARD_I2C_SDA_GPIO/SCL_GPIO above (G1/G2 — the external Port.A
// connector user sensors plug into). An earlier revision of this code mistakenly assumed the
// backlight was on the *shared* Port.A bus (misreading a schematic net-name coincidence) —
// don't repeat that; it made bus scans unreliable by putting boot-time backlight I2C traffic
// on the same bus as user sensors. BOARD_TFT_BL_GPIO stays -1 so display.c's plain-GPIO
// backlight path is skipped; BOARD_TFT_BL_I2C_* below drives it instead (see display.c's
// init_i2c_backlight()) — LP5562 has a single fixed I2C address, no address-select pins.
#define BOARD_TFT_BL_GPIO        -1
#define BOARD_TFT_BL_I2C_PORT     1
#define BOARD_TFT_BL_I2C_SDA_GPIO GPIO_NUM_45
#define BOARD_TFT_BL_I2C_SCL_GPIO GPIO_NUM_0
#define BOARD_TFT_BL_I2C_ADDR     0x30
#define BOARD_TFT_WIDTH      128
#define BOARD_TFT_HEIGHT     128
// ST7735S controller memory is 132x132; the visible 128x128 area sits at M5GFX's documented
// offset_x=2/offset_y=1 — but display.c drives every panel with swap_xy=true, which transposes
// the axes, so their offset_x lands in OUR y_gap and their offset_y in OUR x_gap: x_gap=1/
// y_gap=2 below IS M5GFX's own config, viewed through the axis swap (verified pixel-exact on
// real hardware — x_gap=2 wrapped the rightmost column round to the left edge). mirror_y
// compensates for M5GFX's offset_rotation=2 (an LGFX MADCTL-table concept this driver doesn't
// replicate).
#define BOARD_TFT_X_GAP      1
#define BOARD_TFT_Y_GAP      2
#define BOARD_TFT_MIRROR_X   false
#define BOARD_TFT_MIRROR_Y   true
#define BOARD_TFT_INVERT     true
