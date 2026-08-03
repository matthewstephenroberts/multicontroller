idf_component_register(
    SRCS "bus_i2c3.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_driver_gpio esp_rom
)
