idf_component_register(
    SRCS "lpf2.cpp" "lego_emit.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_driver_uart esp_driver_gpio esp_timer config_store sensor scheduler display
)
