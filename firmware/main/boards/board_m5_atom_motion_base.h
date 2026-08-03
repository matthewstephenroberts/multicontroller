// board_m5_atom_motion_base.h — shared pin map for any M5Stack "Atom"-family board (AtomS3,
// AtomS3 Lite, AtomS3R, and future family members) docked on an M5Stack Atomic Motion Base
// (v1.2). Not a standalone board target — #include this from a specific board's header (e.g.
// board_atoms3_lite.h) *before* that board defines its own Port.A I2C / SPI / display config,
// then override any of these macros afterward if that particular board's wiring differs
// (board_atoms3r.h does this for BOARD_UART_TX_GPIO/RX_GPIO — see there for why).
//
// Rationale for pulling this out of the per-board files: every board in this family shares the
// exact same bottom pogo-pin header (G1, G2, G5, G6, G7, G8, G38, G39) and, once a Motion Base
// is docked, the exact same fixed assignment of that header — Motion Base I2C on G38/G39, Port B
// UART on G7/G8, Port C UART on G5/G6 (see comments below for the "why" on each, verified against
// M5Stack's own Atomic Motion Base Arduino examples). A new Atom-family board with a battery-
// capable Motion Base variant should be able to pick this up with a one-line #include rather than
// re-deriving and re-verifying the same pin assignments again.

// ---- Peripheral power enable ----
// No separate TFT_I2C_POWER-style gate on this family — Port.A and the Motion Base bus are
// always powered once the Atom itself is powered.
#define BOARD_PERIP_PWR_GPIO -1

// ---- DIGITAL ANALOGUE sensors ----
// Of the 6 bottom-header GPIOs (G5, G6, G7, G8, G38, G39), G5/G6 (Port C) and G7/G8 (Port B)
// are reserved below for the LEGO emitter UART and the aux UART respectively, matching the
// Atomic Motion Base v1.2's own Port C / Port B breakouts. G38/G39 were previously assumed free
// for general-purpose DA use — WRONG: confirmed against M5Stack's own Atomic Motion Base
// Arduino example (its INA226.ino explicitly sets sda=38/scl=39 for the AtomS3/AtomS3R/AtomS3
// Lite family), the base's own I2C (STM32 motor/servo driver at 0x38, INA226 power monitor at
// 0x40 — see BOARD_MOTION_I2C_* below) runs over G38/G39 too, not the external Port.A bus
// (G1/G2). All 6 bottom-header pins are therefore spoken for once a Motion Base might be
// attached, leaving no general-purpose DA pins on this board family — BOARD_DA_COUNT 0.
#define BOARD_DA0_GPIO   -1
#define BOARD_DA1_GPIO   -1
#define BOARD_DA2_GPIO   -1
#define BOARD_DA3_GPIO   -1
#define BOARD_DA4_GPIO   -1
#define BOARD_DA_GPIOS   { }
#define BOARD_DA_COUNT   0

// ---- I2C (Atomic Motion Base v1.2, bottom pogo-pin header — G38/G39, NOT Port.A) ----
// A third, separate I2C bus: the base's own STM32 motor/servo controller (0x38) and INA226
// power monitor (0x40) — see components/sensor/drivers/drv_ina226.c. Only meaningful if a base
// is actually docked underneath; bus_scan.c probes it like any other detachable accessory
// rather than assuming it's always present. This is also the bus a future board's own onboard
// battery-monitor IC would most naturally reuse, if that board follows the same Motion Base
// docking convention.
// Bit-banged (bus_i2c3.c), not a hardware I2C peripheral: the ESP32-S3 has only two I2C
// controllers, both already committed elsewhere in every board that includes this file (Port.A
// on BOARD_I2C_PORT 0, and — on boards with an onboard display — the backlight driver on
// BOARD_TFT_BL_I2C_PORT 1). No BOARD_MOTION_I2C_PORT exists because there's no third controller
// to assign one to.
#define BOARD_MOTION_I2C_SDA_GPIO GPIO_NUM_38
#define BOARD_MOTION_I2C_SCL_GPIO GPIO_NUM_39

// ---- User button ----
#define BOARD_BUTTON_GPIO    GPIO_NUM_41   // active-low, onboard pull-up. -1 = none

// ---- UART (auxiliary port; UART0 is the console) ----
// Port B (G7/G8) on an Atomic Motion Base v1.2, or the bare G7/G8 header pins with no base
// attached. Doesn't conflict with I2C (G1/G2) or the LEGO UART below (Port C, G5/G6). A board
// whose specific hardware revision doesn't support this port should #undef/redefine
// BOARD_UART_TX_GPIO/BOARD_UART_RX_GPIO to -1/-1 after including this file (see board_atoms3r.h)
// rather than removing the port outright, so re-enabling it later is a one-line change.
#define BOARD_UART_PORT      1
#define BOARD_UART_TX_GPIO   GPIO_NUM_7
#define BOARD_UART_RX_GPIO   GPIO_NUM_8
#define BOARD_UART_DEFAULT_BAUD 9600

// ---- LEGO color-sensor emitter (LPF2 UART to a SPIKE Prime / Powered Up hub) ----
// Port C (G5/G6) on an Atomic Motion Base v1.2 — the Motion Base's own I2C devices (STM32
// motor/servo driver + INA226 power monitor) are on G1/G2 only, so wiring the LEGO hub to
// Port C instead leaves I2C free for other sensors and doesn't collide with the base's own
// use of the bus. Use Port C's cable/header, not the shared I2C Grove port, for the LEGO hub.
#define BOARD_LEGO_UART_PORT  2
#define BOARD_LEGO_TX_GPIO    5
#define BOARD_LEGO_RX_GPIO    6
#define BOARD_LEGO_TYPE       0x3D    // LPF2 type byte: LEGO Color Sensor
#define BOARD_LEGO_BAUD       115200  // operational baud after the 2400-baud handshake

// ---- SPI sensors ----
// No Atom-family board in this line breaks out external SPI chip-selects — the bottom header is
// fully committed to I2C/UART as above, and any onboard display (where present) is wired
// internally on its own dedicated SPI, never on a shared external bus/CS list. A board that
// really does expose SPI CS lines should override BOARD_SPI_CS_COUNT/BOARD_SPI_CS_GPIOS after
// including this file rather than assume this default applies.
#define BOARD_SPI_HOST       SPI2_HOST
#define BOARD_SPI_SCLK_GPIO  -1
#define BOARD_SPI_MOSI_GPIO  -1
#define BOARD_SPI_MISO_GPIO  -1
#define BOARD_SPI_CS_COUNT     0
#define BOARD_SPI_CS_GPIOS     { }
#define BOARD_DISPLAY_CS_INDEX -1
