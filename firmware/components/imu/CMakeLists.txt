idf_component_register(
    SRCS
        "imu.c"
        "vendor/bmi2.c"
        "vendor/bmi270.c"
        "vendor/bmm150.c"
    INCLUDE_DIRS "include" "vendor"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES bus_i2c2
)

# Bosch's BMM150 API returns raw fixed-point mag data unless this is defined, in which case
# bmm150_aux_mag_data() applies its own trim-register compensation and returns compensated
# microtesla floats directly — exactly what imu_read_mag() wants (see bmm150_defs.h).
target_compile_definitions(${COMPONENT_LIB} PUBLIC BMM150_USE_FLOATING_POINT)

# The vendored Bosch sources are written for generic embedded C across many toolchains and trip
# a few warnings under ESP-IDF's default (fairly strict) settings — silence rather than edit
# third-party code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable -Wno-sign-compare)
