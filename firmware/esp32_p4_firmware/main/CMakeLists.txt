idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app gui bsp nvs_flash esp_wifi esp_wifi_remote esp_event esp_netif espressif__esp_hosted
)
