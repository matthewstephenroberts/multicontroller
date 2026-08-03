idf_component_register(
    SRCS "scheduler.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor config_store esp_timer
)
