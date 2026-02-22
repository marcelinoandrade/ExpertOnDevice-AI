idf_component_register(
    SRCS "src/bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos driver lvgl esp_psram esp_codec_dev esp32_p4_eye esp_event esp_netif esp_wifi esp_wifi_remote
)
