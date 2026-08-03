// drv_motor_ctrl_status.c — M5Stack Atomic Motion Base motor controller diagnostic sensor.
// Reads the current state of all motor and servo control registers from the STM32 at 0x38
// to verify initialization and monitor active outputs.
//
// Outputs (8 floats):
//   [0-3]: Servo angles (channels 1-4), degrees 0-180
//   [4-5]: Motor speeds (channels 1-2), -127 to +127
//   [6-7]: Servo PWM (channel 1-2 low word), 500-2500 (divided by 100 for display)

#include "sensor.h"
#include "board_config.h"

#if defined(BOARD_MOTION_I2C_SDA_GPIO)

#include "bus_i2c3.h"
#include "esp_log.h"

static const char *TAG = "drv_motor_ctrl";

#define MOTOR_CTRL_ADDR 0x38

// Register addresses
#define SERVO_ANGLE_START       0x00  // 0x00-0x03 (4 registers)
#define MOTOR_SPEED_START       0x20  // 0x20-0x21 (2 registers)
#define SERVO_PWM_REGS          4     // Number of servo PWM registers
#define SERVO_PWM_ADDR(ch)      (0x10 + (ch) * 2)  // 0x10, 0x12, 0x14, 0x16

static esp_err_t motor_ctrl_read(const sensor_cfg_t *cfg, float *out, int max, int *out_count)
{
    (void)cfg;

    if (max < 8) return ESP_ERR_INVALID_SIZE;

    // Initialize bus_i2c3 if needed
    if (bus_i2c3_init() != ESP_OK) {
        ESP_LOGW(TAG, "bus_i2c3 not available");
        return ESP_FAIL;
    }

    // Probe for motor controller
    if (bus_i2c3_probe(MOTOR_CTRL_ADDR) != ESP_OK) {
        ESP_LOGD(TAG, "Motor controller not found at 0x%02x", MOTOR_CTRL_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    // Read servo angles (0x00-0x03)
    for (int i = 0; i < 4; i++) {
        uint8_t angle = 0;
        if (bus_i2c3_read_reg(MOTOR_CTRL_ADDR, SERVO_ANGLE_START + i, &angle, 1) == ESP_OK) {
            out[i] = (float)angle;  // 0-180 degrees
        } else {
            out[i] = -1.0f;  // Error indicator
        }
    }

    // Read motor speeds (0x20-0x21)
    for (int i = 0; i < 2; i++) {
        int8_t speed = 0;
        if (bus_i2c3_read_reg(MOTOR_CTRL_ADDR, MOTOR_SPEED_START + i, (uint8_t *)&speed, 1) == ESP_OK) {
            out[4 + i] = (float)speed;  // -127 to +127
        } else {
            out[4 + i] = -128.0f;  // Error indicator
        }
    }

    // Read servo PWM values (0x10, 0x12, 0x14, 0x16) — 2 bytes each, MSB first
    // Return combined PWM values (divide by 100 for compact display, e.g., 500 → 5.00)
    for (int i = 0; i < 2; i++) {  // Return first 2 servo PWM values
        uint8_t pwm_bytes[2] = {0, 0};
        uint16_t pwm_value = 0;
        if (bus_i2c3_read_reg(MOTOR_CTRL_ADDR, SERVO_PWM_ADDR(i), pwm_bytes, 2) == ESP_OK) {
            pwm_value = ((uint16_t)pwm_bytes[0] << 8) | pwm_bytes[1];
            out[6 + i] = (float)pwm_value / 100.0f;  // Scale down for readability
        } else {
            out[6 + i] = -1.0f;  // Error indicator
        }
    }

    *out_count = 8;
    return ESP_OK;
}

static int motor_ctrl_describe(const sensor_cfg_t *cfg, const char *names[], int max)
{
    (void)cfg;
    static const char *n[] = {
        "servo1_angle_deg",
        "servo2_angle_deg",
        "servo3_angle_deg",
        "servo4_angle_deg",
        "motor1_speed",
        "motor2_speed",
        "servo1_pwm_x100",
        "servo2_pwm_x100",
    };
    int c = max < 8 ? max : 8;
    for (int i = 0; i < c; i++) names[i] = n[i];
    return c;
}

const sensor_driver_t drv_motor_ctrl_status = {
    .type = "motor_ctrl_status",
    .probe = NULL,
    .read = motor_ctrl_read,
    .describe = motor_ctrl_describe,
};

#else  // !defined(BOARD_MOTION_I2C_SDA_GPIO)

const sensor_driver_t drv_motor_ctrl_status = {
    .type = "motor_ctrl_status",
    .probe = NULL,
    .read = NULL,
    .describe = NULL,
};

#endif
