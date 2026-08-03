idf_component_register(
    SRCS "motion_ctrl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES bus_i2c3
)
