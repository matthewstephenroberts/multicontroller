# GPIO / ADC pin sensors (the 5 DA pins)

The board exposes five general-purpose **"DIGITAL ANALOGUE"** pins that can be read as sensors —
each value streams to the dashboard/display and can be packed into the [LEGO emitter](lego-emit.md)
like any other sensor.

| Pin | GPIO | ADC channel |
|-----|------|-------------|
| DA0 | 18 | ADC2_CH7 |
| DA1 | 17 | ADC2_CH6 |
| DA2 | 16 | ADC2_CH5 |
| DA3 | 15 | ADC2_CH4 |
| DA4 | 14 | ADC2_CH3 |

(Defined by `BOARD_DA_GPIOS` in `firmware/main/board_config.h`; the web pin dropdown mirrors it.)

## Adding a pin sensor

In the sensor list, **+ add** a sensor and set its **type**:

- **`gpio`** — digital input. Pick the **pin** (DA0–DA4) and a **pull** (none / up / down).
  Reads `state` = 0 or 1.
- **`adc`** — analog input. Pick the **pin**. Reads `counts` 0–4095; switch the **convert**
  dropdown to **volts** (`adc_volts`) for ~0–3.3 V.

The driver only accepts the five DA pins (it rejects any other GPIO so a stray config can't
drive an I2C/SPI/UART/display pin). Internally the chosen GPIO is stored in the sensor's `port`
field and the digital pull-mode in `recipe.reg` — no special config, so they persist like any
sensor.

## Analog (ADC) note

GPIO14–18 are on **ADC2**. ADC2 is the radio-shared ADC; on this **BLE-only** board BLE does
not claim it (unlike Wi-Fi), so analog reads work — but ADC2 is second-class: a read can
occasionally return *busy* if it coincides with a radio operation (the driver returns an error
for that sample and the next poll retries). For rock-solid analog, prefer a slower `poll_ms`.

Full-scale is ~3.3 V at 12 dB attenuation, 12-bit (0–4095). The `adc_volts` conversion is a
fixed ratio (no per-chip calibration); it's good to a few %.

## Sending a pin to the LEGO hub

In the LEGO emitter card, add a field for the pin sensor:

- a **`gpio` state** → a **1-bit** field (one RGBI bit);
- an **`adc` counts** value → up to a **12-bit** field (set min/max 0–4095, or 0–3.3 with the
  volts convert).

The generated hub decoder reconstructs it from `color_sensor.rgbi()` like any other field.
