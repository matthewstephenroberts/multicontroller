idf_component_register(
    SRCS "ble_svc.c" "ble_protocol.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
    REQUIRES bt config_store bus_scan scheduler lego_emit hid_host esp_timer
)
