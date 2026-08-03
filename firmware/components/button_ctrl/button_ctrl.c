// button_ctrl.c — Standalone button controller for boards with BOARD_BUTTON_GPIO.
// Detects button holds and triggers actions (e.g., toggle BLE after 3-second press).
// Works independently of display component, so button works on headless boards too.

#include "button_ctrl.h"
#include "board_config.h"

#if defined(BOARD_BUTTON_GPIO) && BOARD_BUTTON_GPIO >= 0

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_svc.h"

static const char *TAG = "button_ctrl";

// Button state tracking
static int64_t button_press_time_ms = 0;      // When button went down, 0 if not held
static bool button_hold_3s_fired = false;     // Did we already fire action for this hold?
static bool last_button_high = true;          // Last debounced GPIO level (active-low, so high=released)

// Debouncing: require button to be stable for this many ms before considering it pressed
#define DEBOUNCE_MS 50

// Debounce state tracking
static bool raw_button_high = true;           // Raw GPIO level (may bounce)
static int64_t debounce_time = 0;             // When current raw state started

static void button_task(void *arg)
{
    (void)arg;
    const int poll_ms = 20;  // Poll button every 20ms (faster for debouncing)

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));

        int64_t now = esp_timer_get_time() / 1000;  // Convert to milliseconds
        bool raw_high = gpio_get_level(BOARD_BUTTON_GPIO);

        // Debounce: track raw state changes
        if (raw_high != raw_button_high) {
            raw_button_high = raw_high;
            debounce_time = now;
        }

        // Wait for debounce period to elapse
        bool held = !raw_button_high;  // active-low: GPIO low = button held
        bool debounced = (now - debounce_time >= DEBOUNCE_MS);

        // Only process button if debounced AND has stabilized
        if (!debounced) continue;

        // Detect button press (falling edge, active-low) — only on transition, not every loop
        if (last_button_high && held && button_press_time_ms == 0) {
            button_press_time_ms = now;
            button_hold_3s_fired = false;
        }

        // Detect 3-second hold and toggle BLE
        if (held && button_press_time_ms > 0 && !button_hold_3s_fired) {
            int64_t hold_time = now - button_press_time_ms;
            if (hold_time >= 3000) {
                bool was_enabled = ble_svc_is_enabled();
                ble_svc_set_enabled(!was_enabled);
                ESP_LOGI(TAG, "Button: BLE toggled (%s → %s)",
                         was_enabled ? "on" : "off", was_enabled ? "off" : "on");
                button_hold_3s_fired = true;
            }
        }

        // Detect button release (rising edge)
        if (!held && button_press_time_ms > 0) {
            button_press_time_ms = 0;
        }

        last_button_high = held;  // Update debounced state for next iteration
    }
}

esp_err_t button_ctrl_init(void)
{
    // Configure GPIO as input with pull-up
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create button polling task
    if (xTaskCreate(button_task, "button_ctrl", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Button controller initialized (GPIO %d)", BOARD_BUTTON_GPIO);
    return ESP_OK;
}

#else  // No button GPIO defined

esp_err_t button_ctrl_init(void)
{
    return ESP_OK;  // No-op on boards without a button
}

#endif
