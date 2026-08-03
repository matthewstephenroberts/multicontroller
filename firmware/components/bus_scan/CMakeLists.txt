idf_component_register(
    SRCS "bus_scan.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES bus_i2c bus_i2c3 bus_spi i2c_mux
)
