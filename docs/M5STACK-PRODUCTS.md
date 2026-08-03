# M5Stack Products Used in MultiController

Complete reference of all M5Stack products mentioned in the Hackster.io article and user manual, with part numbers and sensor specifications.

## Core Hardware

### Microcontroller Boards
- **M5Stack AtomS3R** (SKU C126)
  - Tiny ESP32-S3 with 1.14" built-in display
  - Recommended for education (students can see sensor readings on device)
  - Features: 240×135 LCD, Bluetooth LE, USB-C

- **M5Stack AtomS3 Lite** (SKU C124)
  - Tiny ESP32-S3 with no display
  - Entry-level, most compact option
  - Features: No screen, Bluetooth LE, USB-C

### Power & Expansion
- **M5Stack Atomic Motion Base v1.2** (SKU A090-V12)
  - Backpack board for AtomS3 with battery power
  - Essential for portability and extended operation
  - Features: Battery slot, Grove I2C connector, UART/SPI pins, power management

## Sensors (M5Stack Unit Line)

### Color/Light Sensing
- **M5Stack Colour Unit** (SKU U009)
  - Sensor: TCS34725
  - Measures: Red, Green, Blue, Lux (brightness)
  - Interface: I2C (address 0x29)
  - Max polling: 50 Hz

- **M5Stack Color Sensor Unit** (SKU U172-AS7341)
  - Sensor: AS7341 (spectral sensor)
  - Measures: 11 different light wavelengths for spectral analysis
  - Interface: I2C
  - Resolution: Advanced colour classification

### Distance Sensing
- **M5Stack Time-of-Flight Distance Unit** (SKU U172)
  - Sensor: VL53L1X (ToF laser rangefinder)
  - Measures: Distance up to 4 meters
  - Interface: I2C (address 0x29 default)
  - Max polling: 50 Hz
  - Accuracy: ±100mm typical

### Rotation/Potentiometer
- **M5Stack 8-Angle Unit** (SKU U154)
  - Sensor: 8 rotary potentiometers with RGB lighting
  - Measures: 8 independent rotary knobs, 0-360° each
  - Interface: I2C (selectable addresses 0x36 or others)
  - Features: Individual RGB LED indicators per knob
  - Max polling: 50 Hz

### Encoder/Step Counter
- **M5Stack Step16 Unit** (SKU U198)
  - Type: 16-bit rotary encoder with LED ring
  - Measures: 16 click-stops position + counts
  - Interface: I2C
  - Features: Light-up LED ring for visual feedback
  - Max polling: 50 Hz

## Connectivity & Expansion

### I2C Multiplexing
- **M5Stack Unit PaHub v2.1** (SKU U076)
  - Type: I2C multiplexer (TCA9548A-based)
  - Channels: 8 independent I2C channels
  - Supports: Up to 6 sensors per PaHub
  - Chainable: Yes, with different I2C addresses (0x70-0x77)
  - Use case: Connect multiple sensors with same address, or 6+ sensors total

- **M5Stack Grove Y Cable**
  - Type: Simple 1→2 splitter
  - Limitation: Both sensors must have different I2C addresses
  - Use case: Simple 2-sensor setups

### Motor Control
- **Atomic Motion Base v1.2** includes motor output connectors
  - 2× DC motor ports with PWM control capability
  - Integrated power management and battery monitoring

## Display
- **M5Stack AtomS3R Onboard Display**
  - Type: 1.14" TFT LCD (ST7789)
  - Resolution: 135×240 pixels
  - Shows: Boot splash, sensor readings, tile view, gamepad input
  - Power managed by firmware

## Accessories

### Connectivity
- **Grove I2C Connector** (QWIIC-compatible)
  - Standard on: Atomic Motion Base, all Unit sensors
  - Supports: Daisy-chaining via cables
  - Voltage: 3.3V

- **LPF2 Cable** (LEGO connector)
  - Type: Custom 6-pin connector
  - Used for: Wiring to LEGO SPIKE/Powered Up hubs
  - User-sourced: Not included, must obtain separately

- **USB-C Cable**
  - Purpose: Power and firmware flashing
  - Usually included with Motion Base

## I2C Address Reference

| Product | Default Address | Alternative | Notes |
|---------|-----------------|-------------|-------|
| Colour Unit (TCS34725) | 0x29 | - | |
| Distance Unit (VL53L1X) | 0x29 | - | |
| 8-Angle Unit (Potentiometers) | 0x36 | Configurable | 8 channels on one address |
| Unit PaHub v2.1 | 0x70-0x77 | Multiple | Chainable multiplexer |
| Step16 Unit | 0x44 | - | 16-bit encoder |

## Connector Standards

- **Grove**: 4-pin connector (GND, VCC, SDA, SCL for I2C)
- **STEMMA/QWIIC**: Compatible with Grove I2C
- **LPF2**: 6-pin LEGO Powered Up connector (custom wiring required)

## Shopping Cart Summary

**Essential:**
1. M5Stack AtomS3R or AtomS3 Lite
2. M5Stack Atomic Motion Base v1.2

**At least one sensor:**
- M5Stack Colour Unit
- M5Stack Time-of-Flight Distance Unit
- M5Stack 8-Angle Unit
- M5Stack Step16 Unit
- M5Stack Color Sensor Unit (AS7341)

**Optional expansion:**
- M5Stack Unit PaHub v2.1
- M5Stack Grove Y Cable

**Other:**
- LEGO hub (already own)
- LPF2 cable (DIY or sourced separately)
- USB-C cable (often included with Motion Base)

For current pricing, visit [M5Stack Shop](https://shop.m5stack.com)

## Documentation

- Full user manual: `docs/assets/user-manual.pdf`
- Hackster.io article: `docs/hackster-article.md`
- Technical specifications: See M5Stack official documentation
- Wiring details: `docs/wiring.md`
- BLE protocol: `docs/ble-protocol.md`

---

*Last updated: August 3, 2026*
*All product information from M5Stack official documentation*
