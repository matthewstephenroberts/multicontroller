idf_component_register(
    SRCS "bus_spi.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_driver_spi esp_driver_gpio
)
