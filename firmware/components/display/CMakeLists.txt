idf_component_register(
    SRCS "display.c" "panel_gc9107.c" "panel_st7735s.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main" "."
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer config_store scheduler sensor ble_svc bus_i2c bus_spi bus_i2c2
)
