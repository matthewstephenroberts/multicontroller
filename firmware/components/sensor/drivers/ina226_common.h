// ina226_common.h — shared register map/constants/conversions for the TI INA226, used by both
// drv_ina226.c (a user-wired INA226 on the external Port.A bus) and
// drv_atom_motion_battery.c (the Atomic Motion Base v1.2's fixed-address INA226 on its own
// bottom-header bus, bus_i2c3) — same chip, same math, different physical bus/addressing, so
// the register/conversion logic lives here once instead of being duplicated by hand.
#pragma once

#include <stdint.h>

#define INA226_REG_CONFIG       0x00
#define INA226_REG_BUSVOLTAGE   0x02
#define INA226_REG_POWER        0x03
#define INA226_REG_CURRENT      0x04
#define INA226_REG_CALIBRATION  0x05
#define INA226_REG_MANUFACTURER 0xFE   // reads 0x5449 ("TI") — no equivalent on the similarly-
#define INA226_REG_DIE_ID       0xFF   // addressed INA219, so this pair reliably tells them apart.

#define INA226_MANUFACTURER_ID  0x5449
#define INA226_DIE_ID           0x2260

// AVERAGES_16 (0b010) | BUS_CONV_TIME_1100US (0b100) | SHUNT_CONV_TIME_1100US (0b100) |
// MODE_SHUNT_BUS_CONT (0b111) — same continuous-conversion config M5Stack's own library sets.
#define INA226_CONFIG_VALUE 0x0527

// calibrationValue = 0.00512 / (currentLSB * rShunt), currentLSB = 0.0003 A/bit for
// rShunt=0.02Ω/iMaxExpected=8.192A (M5Stack's own calibrate(0.02, 8.192) call, both here and on
// the Motion Base's INA226 — reproduced as a constant since both drivers target that one shunt
// value rather than an arbitrary one).
#define INA226_CALIB_VALUE  853
#define INA226_CURRENT_LSB  0.0003f    // A per bit, from the calibration above
#define INA226_POWER_LSB    0.0075f    // W per bit = 25 * CURRENT_LSB (INA226 datasheet relation)
#define INA226_BUS_LSB      0.00125f   // V per bit, fixed by the chip (not calibration-dependent)

// Rough single-cell Li-ion/LiPo state-of-charge from open-circuit-ish voltage — a commonly used
// generic discharge-curve approximation (piecewise-linear), not specific to any one board's cell
// chemistry/age, so treat it as a ballpark ("low/medium/full") rather than a precise percentage
// — a real gauge IC would use coulomb counting, which this chip doesn't do.
static inline float ina226_batt_pct_from_voltage(float v)
{
    static const float V[] = { 3.27f, 3.61f, 3.69f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f,
                                3.82f, 3.84f, 3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.08f,
                                4.11f, 4.15f, 4.20f };
    static const float P[] = { 0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80,
                                85, 90, 95, 100 };
    const int n = sizeof(V) / sizeof(V[0]);
    if (v <= V[0]) return 0.0f;
    if (v >= V[n - 1]) return 100.0f;
    for (int i = 1; i < n; i++) {
        if (v <= V[i]) {
            float t = (v - V[i - 1]) / (V[i] - V[i - 1]);
            return P[i - 1] + t * (P[i] - P[i - 1]);
        }
    }
    return 100.0f;
}
