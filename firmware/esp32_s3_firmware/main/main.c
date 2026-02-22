#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app.h"
#include "bsp.h"
#include "gui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(bsp_init());
    ESP_ERROR_CHECK(gui_init());
    ESP_ERROR_CHECK(app_init());

    app_start();

    ESP_LOGI(TAG, "assistant_esp32 started");
}
