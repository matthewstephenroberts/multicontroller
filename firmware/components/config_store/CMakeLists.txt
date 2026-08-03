idf_component_register(
    SRCS "config_store.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES sensor nvs_flash
)
