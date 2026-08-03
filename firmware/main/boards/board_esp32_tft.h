// board_esp32_tft.h — pin map for the "ESP32-S3 1.14\" TFT" board target
// (ESP32S3FH4R2 + onboard ST7789, onboard BMP280 + QMI8658C, STEMMA/QWIIC I2C header).
// Included only from board_config.h when BOARD_ESP32_TFT is defined there — don't include
// directly, and don't add an include guard beyond board_config.h's own #pragma once (this file
// is textually spliced into exactly one translation of board_config.h per build).

#define BOARD_NAME "ESP32-S3 1.14\" TFT"

// ---- Peripheral power enable (TFT_I2C_POWER) ----
// On this board GPIO21 gates power to the I2C/STEMMA bus and the display.
// It must be driven HIGH at boot or the onboard sensors read nothing. -1 = none.
#define BOARD_PERIP_PWR_GPIO GPIO_NUM_21

// ---- DIGITAL ANALOGUE sensors ----
// Five general-purpose pins usable as digital inputs or analog (ADC) inputs via the
// "gpio"/"adc" sensor drivers. GPIO14-18 are ADC2 channels. BOARD_DA_GPIOS is the allow-list
// the drivers validate against (and the web pin dropdown mirrors).
#define BOARD_DA0_GPIO   GPIO_NUM_18
#define BOARD_DA1_GPIO   GPIO_NUM_17
#define BOARD_DA2_GPIO   GPIO_NUM_16
#define BOARD_DA3_GPIO   GPIO_NUM_15
#define BOARD_DA4_GPIO   GPIO_NUM_14
#define BOARD_DA_GPIOS   { BOARD_DA0_GPIO, BOARD_DA1_GPIO, BOARD_DA2_GPIO, BOARD_DA3_GPIO, BOARD_DA4_GPIO }
#define BOARD_DA_COUNT   5

// ---- I2C (shared bus; TCA9548A mux lives here; onboard BMP280 + QMI8658C) ----
#define BOARD_I2C_PORT       0
#define BOARD_I2C_SDA_GPIO   GPIO_NUM_42
#define BOARD_I2C_SCL_GPIO   GPIO_NUM_41
#define BOARD_I2C_FREQ_HZ    400000

// ---- SPI sensors (share the onboard TFT's SPI2 bus; up to 5 chip-selects) ----
// SCLK/MOSI/MISO are the FSPI pins shared with the display; each device (the TFT and
// every SPI sensor) has its own CS. The bus is initialised once (by whichever of the
// display / bus_spi runs first) and arbitrated by the SPI driver.
#define BOARD_SPI_HOST       SPI2_HOST
#define BOARD_SPI_SCLK_GPIO  GPIO_NUM_36
#define BOARD_SPI_MOSI_GPIO  GPIO_NUM_35
#define BOARD_SPI_MISO_GPIO  GPIO_NUM_37

// All SPI chip-select lines on the board (sensors + the onboard display). Index 5 is the
// display's CS; bus_spi leaves it alone (esp_lcd drives it). Set an entry to -1 if not wired.
#define BOARD_SPI_CS_COUNT     6
#define BOARD_SPI_CS_GPIOS     { GPIO_NUM_12, GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_7 }
#define BOARD_DISPLAY_CS_INDEX 5    // CS index reserved for the onboard display (-1 = none)

// ---- User button (BOOT) — advances display pages in paged mode ----
#define BOARD_BUTTON_GPIO    GPIO_NUM_0   // active-low, onboard pull-up. -1 = none

// ---- UART (auxiliary port; UART0 is the console) ----
#define BOARD_UART_PORT      1
#define BOARD_UART_TX_GPIO   GPIO_NUM_5
#define BOARD_UART_RX_GPIO   GPIO_NUM_6
#define BOARD_UART_DEFAULT_BAUD 9600

// ---- LEGO color-sensor emitter (LPF2 UART to a SPIKE Prime / Powered Up hub) ----
// Dedicated UART (UART2; UART1 is the aux sensor bus). TX→hub RX, RX←hub TX on the
// LEGO 6-wire connector. These are the defaults seeded into NVS on first boot; the
// pins and enable flag are configurable at runtime over BLE. See docs/lego-emit.md.
#define BOARD_LEGO_UART_PORT  2
#define BOARD_LEGO_TX_GPIO    1
#define BOARD_LEGO_RX_GPIO    2
#define BOARD_LEGO_TYPE       0x3D    // LPF2 type byte: LEGO Color Sensor
#define BOARD_LEGO_BAUD       115200  // operational baud after the 2400-baud handshake

// ---- Onboard TFT (ST7789, 1.14", 135x240) on SPI2 ----
// These are the *defaults* for the onboard display config seeded into NVS on first boot;
// the display is configurable/disable-able at runtime over BLE. Set BOARD_HAS_DISPLAY 0
// on a board with no screen (the firmware then runs display-less).
#define BOARD_HAS_DISPLAY    1
#define BOARD_TFT_HOST       SPI2_HOST
#define BOARD_TFT_SCLK_GPIO  GPIO_NUM_36
#define BOARD_TFT_MOSI_GPIO  GPIO_NUM_35
#define BOARD_TFT_MISO_GPIO  GPIO_NUM_37   // shared-bus MISO (the display itself never reads)
#define BOARD_TFT_CS_GPIO    GPIO_NUM_7
#define BOARD_TFT_DC_GPIO    GPIO_NUM_39
#define BOARD_TFT_RST_GPIO   GPIO_NUM_40
#define BOARD_TFT_BL_GPIO    GPIO_NUM_45
// bus_i2c2 (the AtomS3R-style I2C-driven backlight bus) is compiled into every build
// regardless of board — required transitively via the display/imu components — so its
// BOARD_TFT_BL_I2C_* macros must exist even on a board (like this one) whose backlight is a
// plain GPIO instead. Actually using this bus at runtime additionally requires
// BOARD_TFT_BL_I2C_ADDR to be defined (it isn't here), so these disabled values (-1 SDA/SCL)
// are never dereferenced — see display.c's and imu.c's `#if defined(BOARD_TFT_BL_I2C_ADDR)`
// guards around every call site.
#define BOARD_TFT_BL_I2C_PORT     1
#define BOARD_TFT_BL_I2C_SDA_GPIO -1
#define BOARD_TFT_BL_I2C_SCL_GPIO -1
// Landscape 240x135. The 1.14" panel is offset within the ST7789's 240x320 RAM;
// tweak the gaps/mirror below if the image is shifted or flipped on your unit.
#define BOARD_TFT_WIDTH      240
#define BOARD_TFT_HEIGHT     135
#define BOARD_TFT_X_GAP      40
#define BOARD_TFT_Y_GAP      52
#define BOARD_TFT_MIRROR_X   false
#define BOARD_TFT_MIRROR_Y   true
#define BOARD_TFT_INVERT     true
