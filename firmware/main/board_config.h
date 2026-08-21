// board_config.h — selects and loads the physical pin map for the MultiController board.
//
// Pick exactly one BOARD_* define below, then rebuild. See docs/wiring.md.
//
// Per-board pin maps live one-per-file under boards/ (firmware/main/boards/) rather than as
// #elif blocks in this file — each board's macros, and the "why" comments behind non-obvious
// pin choices, stay self-contained and diffable on their own, and a new board is "add one file
// + one line here" instead of a growing #if/#elif ladder. Boards that share hardware (e.g. every
// M5Stack Atom-family board dockable on an Atomic Motion Base) share a common snippet header —
// see boards/board_m5_atom_motion_base.h — included by each board file and overridden where that
// specific board's wiring differs, so a future Atom-family board with its own battery-capable
// Motion Base variant only needs to add its display/Port.A specifics, not re-derive the whole
// bottom-header pin assignment again.
#pragma once

#include "driver/gpio.h"

// ================== BOARD SELECTION ==================
//#define BOARD_ESP32_ZERO     // ← Change this for different boards
//#define BOARD_ESP32_TFT      // ← Change this for different boards
//#define BOARD_ATOMS3_LITE      // ← Change this for different boards
#define BOARD_ATOMS3R         // ← Change this for different boards

#if defined(BOARD_ESP32_ZERO)
#include "boards/board_esp32_zero.h"
#elif defined(BOARD_ESP32_TFT)
#include "boards/board_esp32_tft.h"
#elif defined(BOARD_ATOMS3_LITE)
#include "boards/board_atoms3_lite.h"
#elif defined(BOARD_ATOMS3R)
#include "boards/board_atoms3r.h"
#else
    #error "Unknown board!"
#endif

// Boards with no onboard display (BOARD_HAS_DISPLAY 0) or a plain external ST7789 never set
// BOARD_TFT_CONTROLLER themselves (only board_atoms3r.h does, since it's the one board whose
// onboard panel isn't st7789-compatible) — this is the single shared fallback both
// config_store.c (default display config) and bus_scan.c (reported onboard-display type) read,
// so the two can never disagree about which controller a board without its own override uses.
#ifndef BOARD_TFT_CONTROLLER
#define BOARD_TFT_CONTROLLER "st7789"
#endif

// TCA9548A mux address range to scan. A real mux is verified before use (verify_mux() in
// bus_scan.c), so widening this range never misclassifies a sensor that merely shares the
// 0x70..0x77 range (e.g. a BMP280 at 0x76) as a mux — it only costs a little extra scan time
// probing addresses with nothing wired. Covers every address an M5Stack I2C Hub (1-6, or the
// standard 1-8 Hub) can be set to via its address-select pads/DIP switches, and any other
// TCA9548A/PCA9548A-based mux — all of which live somewhere in 0x70..0x77 (the full 3-pin
// address range). Multiple muxes at different addresses on the same bus are scanned together.
#define BOARD_I2C_MUX_ADDR_LO 0x70
#define BOARD_I2C_MUX_ADDR_HI 0x77

// ---- BLE ----
// Fits MC_DEVICE_NAME_LEN (20, config_store.h) comfortably; renamable from the web header.
#define BOARD_BLE_NAME       "MultiController"
