#include "esp_log.h"
#include "nvs_flash.h"
#include "gateway_core.h"
#include "serial_console.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "SmartReef Gateway starting");
    ESP_ERROR_CHECK(serial_console_start());
    ESP_ERROR_CHECK(gateway_core_start());
}
