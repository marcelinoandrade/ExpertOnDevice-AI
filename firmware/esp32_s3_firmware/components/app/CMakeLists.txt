idf_component_register(
    SRCS "src/app.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos bsp gui esp_common esp_http_client json mbedtls
)
