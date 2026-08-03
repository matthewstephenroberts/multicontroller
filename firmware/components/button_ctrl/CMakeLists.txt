idf_component_register(
    SRCS "button_ctrl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES ble_svc esp_driver_gpio esp_timer
)
