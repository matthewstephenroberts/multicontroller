idf_component_register(
    SRCS "bus_uart.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_driver_uart esp_driver_gpio
)
