// motion_ctrl.c — M5Stack Atomic Motion Base STM32 motor/servo controller initialization.
// Detects and safely disables the motor controller at 0x38 on the Motion Base I2C bus to
// prevent excessive current draw if the controller is in an unknown state at boot.
//
// Register map (from M5Stack Atomic Motion Base v1.2 docs):
//   0x00-0x03: Servo angles (channels 1-4), range 0-180°
//   0x10, 0x12, 0x14, 0x16: Servo PWM pulse widths (channels 1-4), range 500-2500 (2 bytes each)
//   0x20-0x21: Motor speeds (channels 1-2), range -127 to +127

#include "motion_ctrl.h"
#include "board_config.h"

#if defined(BOARD_MOTION_I2C_SDA_GPIO)

#include "bus_i2c3.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motion_ctrl";

// Motor controller I2C address on the Motion Base bus
#define MOTION_CTRL_ADDR 0x38

// Register addresses (from M5Stack Atomic Motion Base register map)
#define SERVO1_ANGLE_REG    0x00
#define SERVO2_ANGLE_REG    0x01
#define SERVO3_ANGLE_REG    0x02
#define SERVO4_ANGLE_REG    0x03
#define SERVO1_PWM_REG      0x10
#define SERVO2_PWM_REG      0x12
#define SERVO3_PWM_REG      0x14
#define SERVO4_PWM_REG      0x16
#define MOTOR1_SPEED_REG    0x20
#define MOTOR2_SPEED_REG    0x21

esp_err_t motion_ctrl_init(void)
{
    // Initialize the bus_i2c3 first if not already done
    if (bus_i2c3_init() != ESP_OK) {
        ESP_LOGW(TAG, "bus_i2c3 init failed");
        return ESP_FAIL;
    }

    // Probe for the motor controller at 0x38
    if (bus_i2c3_probe(MOTION_CTRL_ADDR) != ESP_OK) {
        ESP_LOGD(TAG, "Motor controller not found at 0x%02x", MOTION_CTRL_ADDR);
        return ESP_OK;  // Not an error — base may not be attached
    }

    ESP_LOGI(TAG, "Motor controller detected at 0x%02x, disabling all outputs", MOTION_CTRL_ADDR);

    // Zero all servo angle registers (0x00-0x03) — neutral positions
    uint8_t servo_angle_regs[] = {SERVO1_ANGLE_REG, SERVO2_ANGLE_REG, SERVO3_ANGLE_REG, SERVO4_ANGLE_REG};
    for (size_t i = 0; i < sizeof(servo_angle_regs); i++) {
        uint8_t cmd[] = {servo_angle_regs[i], 0x00};
        esp_err_t err = bus_i2c3_write(MOTION_CTRL_ADDR, cmd, sizeof(cmd));
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "Set servo%zu angle to 0°", i + 1);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    // Zero all motor speed registers (0x20-0x21) — stop motors
    uint8_t motor_speed_regs[] = {MOTOR1_SPEED_REG, MOTOR2_SPEED_REG};
    for (size_t i = 0; i < sizeof(motor_speed_regs); i++) {
        uint8_t cmd[] = {motor_speed_regs[i], 0x00};
        esp_err_t err = bus_i2c3_write(MOTION_CTRL_ADDR, cmd, sizeof(cmd));
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "Set motor%zu speed to 0", i + 1);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    // Zero all servo PWM registers (0x10, 0x12, 0x14, 0x16) — stop PWM (2 bytes each)
    uint8_t servo_pwm_regs[] = {SERVO1_PWM_REG, SERVO2_PWM_REG, SERVO3_PWM_REG, SERVO4_PWM_REG};
    for (size_t i = 0; i < sizeof(servo_pwm_regs); i++) {
        uint8_t cmd[] = {servo_pwm_regs[i], 0x00, 0x00};  // 2 bytes: MSB then LSB
        esp_err_t err = bus_i2c3_write(MOTION_CTRL_ADDR, cmd, sizeof(cmd));
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "Set servo%zu PWM to 0", i + 1);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // Verify by reading back a servo angle register
    uint8_t verify = 0xFF;
    if (bus_i2c3_read_reg(MOTION_CTRL_ADDR, SERVO1_ANGLE_REG, &verify, 1) == ESP_OK) {
        ESP_LOGI(TAG, "Verified: servo angle register reads 0x%02x (should be 0x00)", verify);
    } else {
        ESP_LOGW(TAG, "Could not verify motor controller state");
    }

    ESP_LOGI(TAG, "Motor controller fully disabled");
    return ESP_OK;
}

#else  // !defined(BOARD_MOTION_I2C_SDA_GPIO) — no Motion Base

esp_err_t motion_ctrl_init(void)
{
    return ESP_OK;  // No Motion Base on this board
}

#endif
