#include "bus_uart.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "board_config.h"

static const char *TAG = "bus_uart";
#define UART_RX_BUF 1024

static bool s_inited;

esp_err_t bus_uart_init(int baud)
{
    if (s_inited) return ESP_OK;

    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BOARD_UART_PORT, UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(BOARD_UART_PORT, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(BOARD_UART_PORT, BOARD_UART_TX_GPIO, BOARD_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    s_inited = true;
    ESP_LOGI(TAG, "UART%d up TX=%d RX=%d @%d baud",
             BOARD_UART_PORT, BOARD_UART_TX_GPIO, BOARD_UART_RX_GPIO, baud);
    return ESP_OK;
}

esp_err_t bus_uart_write(const uint8_t *data, size_t len)
{
    int n = uart_write_bytes(BOARD_UART_PORT, (const char *)data, len);
    return (n == (int)len) ? ESP_OK : ESP_FAIL;
}

int bus_uart_read(uint8_t *buf, size_t max, int timeout_ms)
{
    return uart_read_bytes(BOARD_UART_PORT, buf, max, pdMS_TO_TICKS(timeout_ms));
}

void bus_uart_flush(void)
{
    uart_flush_input(BOARD_UART_PORT);
}
