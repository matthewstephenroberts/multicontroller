# Wiring & pin map

GPIO assignments are in `firmware/main/board_config.h` — change them there to match your board,
then rebuild. **The defaults target the ESP32S3-1.14TFT board** (ESP32S3FH4R2 + onboard ST7789
display, onboard BMP280 + QMI8658C, and a STEMMA/QWIIC I2C header).

> ESP32-S3 GPIOs route through the GPIO matrix, but several are used by the onboard display and
> sensors — see the board's silkscreen/pinout. Avoid the in-package flash/PSRAM pins.

## Peripheral power enable (important!)

| Signal           | GPIO    | Notes                                                        |
|------------------|---------|--------------------------------------------------------------|
| TFT_I2C_POWER    | GPIO 21 | **Driven HIGH at boot.** Gates power to the I2C/STEMMA bus and display — if low, the onboard sensors and the QWIIC header are unpowered and a scan finds nothing. |

## I2C (onboard sensors + STEMMA/QWIIC header + mux)

| Signal | GPIO    | Notes                                  |
|--------|---------|----------------------------------------|
| SDA    | GPIO 42 | STEMMA/QWIIC `SDA`; pull-ups onboard   |
| SCL    | GPIO 41 | STEMMA/QWIIC `SCL`; pull-ups onboard   |

Onboard devices already on this bus: **BMP280** (0x76) and **QMI8658C** IMU (0x6A). Plug your
**TCA9548A** mux (default **0x70**) into the QWIIC header; its 8 channels each fan out to sensors.
Same-address sensors can coexist on different channels; sensors may also sit directly on the bus
(`mux_addr: 0`).

```
ESP32-S3 ─ SDA/SCL(42/41) ─┬─ BMP280 (0x76), QMI8658C (0x6A)   [onboard]
                           ├─ TCA9548A(0x70) ─ ch0..ch7 ─ sensors
                           └─ (other direct sensors)
```

## Onboard TFT display (ST7789, 1.14", 135×240) — SPI2

| Signal        | GPIO    |
|---------------|---------|
| SCLK          | GPIO 36 |
| MOSI          | GPIO 35 |
| CS            | GPIO 7  |
| DC            | GPIO 39 |
| RST           | GPIO 40 |
| Backlight     | GPIO 45 |

Driven by the `display` component (`esp_lcd` ST7789). It shows device name, BLE state, sensor
count, and live readings. If the image is shifted/mirrored/inverted on your unit, adjust
`BOARD_TFT_X_GAP` / `BOARD_TFT_Y_GAP` / `BOARD_TFT_MIRROR_*` / `BOARD_TFT_INVERT` in
`board_config.h`.

## SPI sensors (share the TFT's SPI2 bus, up to 5 chip-selects)

External SPI sensors share the **onboard TFT's SPI2 bus** — same SCLK/MOSI/MISO as the display,
each device with its own CS.

| Signal | GPIO    |
|--------|---------|
| SCLK   | GPIO 36 |
| MOSI   | GPIO 35 |
| MISO   | GPIO 37 |
| CS0–4  | GPIO 12, 11, 10, 9, 8 |

A sensor's config `cs_index` (0–4) selects its CS line; set an entry to `-1` in
`board_config.h` if unused. **CS index 5 (GPIO 7) is reserved for the onboard display** —
`esp_lcd` drives it, so leave it out of sensor configs.

## UART (one auxiliary port)

| Signal | GPIO   | Notes          |
|--------|--------|----------------|
| TX     | GPIO 5 | to sensor RX   |
| RX     | GPIO 6 | from sensor TX |

Default baud 9600 (`BOARD_UART_DEFAULT_BAUD`). UART0 is reserved for the USB-serial
console/log; UART2 is used by the LEGO emitter (below).

## LEGO emitter (LPF2 to a SPIKE Prime / Powered Up hub)

| Signal | GPIO   | Notes                |
|--------|--------|----------------------|
| TX     | GPIO 1 | → hub RX             |
| RX     | GPIO 2 | ← hub TX             |

Dedicated UART2; pins and enable are configurable at runtime over BLE. See
[`docs/lego-emit.md`](lego-emit.md).

## BOOT button

GPIO 0 (active-low, onboard pull-up) — advances the display page in paged mode.

## Digital / analog pins (DA0–DA4)

Five general-purpose pins — GPIO 18, 17, 16, 15, 14 — readable as **digital** (0/1) or
**analog** (ADC2) sensors via the `gpio` / `adc` sensor types. See
[`docs/gpio-sensors.md`](gpio-sensors.md).

## Example bring-up sensors

| Bus  | Example part   | Type       | Addressing                              |
|------|----------------|------------|-----------------------------------------|
| I2C  | BMP280 (onboard) | `generic`/`bme280` | addr 0x76 (BMP280 has no humidity) |
| I2C  | any sensor     | varies     | behind TCA9548A channel on the QWIIC header |
| SPI  | ADXL345        | `generic`  | `cs_index` 0, register recipe           |
| UART | (your sensor)  | named/uart | `port` 1                                |

Addressing and recipes live in config, not firmware — swap parts freely.
