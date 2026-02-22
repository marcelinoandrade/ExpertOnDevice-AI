#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_hosted.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "app.h"
#include "bsp.h"
#include "gui.h"

static const char *TAG = "main";

#if CONFIG_APP_MINIMAL_HOSTED_LINK_TEST
static void run_minimal_hosted_link_test(void)
{
    ESP_LOGW(TAG, "Running minimal ESP-Hosted link test mode");

    int ret = esp_hosted_init();
    if (ret) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %d", ret);
        return;
    }

    ret = esp_hosted_connect_to_slave();
    if (ret) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed: %d", ret);
        (void)esp_hosted_deinit();
        return;
    }

    esp_hosted_coprocessor_fwver_t ver = {0};
    ret = esp_hosted_get_coprocessor_fwversion(&ver);
    if (ret) {
        ESP_LOGE(TAG, "esp_hosted_get_coprocessor_fwversion failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "C6 firmware version: %u.%u.%u", ver.major1, ver.minor1, ver.patch1);
    }

    (void)esp_hosted_deinit();
}
#endif

#if CONFIG_APP_MINIMAL_HOSTED_WIFI_TEST
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_remote_connect();
    } else if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_APP_TEST_WIFI_MAX_RETRY) {
            esp_wifi_remote_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to AP (%d/%d)", s_retry_num, CONFIG_APP_TEST_WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void run_minimal_hosted_wifi_test(void)
{
    ESP_LOGW(TAG, "Running minimal ESP-Hosted Wi-Fi test mode");
    s_wifi_event_group = xEventGroupCreate();

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_remote_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_APP_TEST_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_APP_TEST_WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_remote_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_remote_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "minimal hosted test: connected to SSID '%s'", CONFIG_APP_TEST_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "minimal hosted test: failed to connect to SSID '%s'", CONFIG_APP_TEST_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "minimal hosted test: timed out waiting Wi-Fi event");
    }
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if CONFIG_APP_MINIMAL_HOSTED_LINK_TEST
    run_minimal_hosted_link_test();
    return;
#endif

#if CONFIG_APP_MINIMAL_HOSTED_WIFI_TEST
    run_minimal_hosted_wifi_test();
    return;
#endif

    ESP_ERROR_CHECK(bsp_init());
    ESP_ERROR_CHECK(gui_init());
    ESP_ERROR_CHECK(app_init());

    app_start();

    ESP_LOGI(TAG, "assistant_esp32 started");
}
