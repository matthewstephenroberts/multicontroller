idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        config_store
        bus_i2c
        bus_spi
        bus_uart
        i2c_mux
        motion_ctrl
        button_ctrl
        sensor
        bus_scan
        ble_svc
        scheduler
        display
        lego_emit
        hid_host
)
