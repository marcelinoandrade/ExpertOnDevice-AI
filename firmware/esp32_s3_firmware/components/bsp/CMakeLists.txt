idf_component_register(
    SRCS "src/bsp.c" "src/bsp_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_timer freertos driver esp_event esp_netif esp_wifi nvs_flash esp_lcd lvgl esp_lcd_touch_cst816s esp_psram fatfs sdmmc vfs lwip esp_adc
)
