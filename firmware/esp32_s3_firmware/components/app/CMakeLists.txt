idf_component_register(
    SRCS "src/app.c" "src/app_storage.c" "src/config_manager.c" "src/captive_portal.c" "src/audio_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos bsp gui esp_common esp_http_client esp_http_server json mbedtls lwip vfs fatfs nvs_flash
)
