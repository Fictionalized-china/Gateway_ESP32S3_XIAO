#include "gateway_core.h"

#include <string.h>
#include "app_config.h"
#include "ble_smartreef.h"
#include "cloud_client.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_id.h"
#include "wifi_manager.h"

static const char *TAG = "core";

typedef struct {
    char gateway_id[GATEWAY_ID_MAX_LEN];
    TaskHandle_t task;
    int64_t last_upload_ms;
    int64_t last_poll_ms;
    int64_t last_wifi_reconnect_ms;
    bool initialized;
} gateway_core_state_t;

static gateway_core_state_t s_core;

static bool is_supported_cmd(const char *cmd)
{
    static const char *const supported[] = {
        "light_on",
        "light_off",
        "pump_on",
        "pump_off",
        "heater_on",
        "heater_off",
        "mode_auto",
        "mode_manual",
        "alarm_enable",
        "alarm_disable",
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        if (strcmp(cmd, supported[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void on_identity(const gateway_device_t *device, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "device identity ready gateway_id=%s device_id=%s reg_code=%s type=%s",
             s_core.gateway_id, device->device_id, device->reg_code, device->device_type);
}

static void on_status(const gateway_device_t *device, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "device status update device_id=%s seq=%u heap=%lu",
             device->device_id, device->status.seq, (unsigned long)esp_get_free_heap_size());
}

static bool get_ready_device(gateway_device_t *device)
{
    if (ble_smartreef_get_device(device) != ESP_OK) {
        return false;
    }
    return device->discovered && device->device_id[0] != '\0';
}

static bool get_connected_device(gateway_device_t *device)
{
    return get_ready_device(device) && device->connected;
}

esp_err_t gateway_core_upload_now(void)
{
    gateway_device_t device = {0};
    if (!get_connected_device(&device)) {
        ESP_LOGW(TAG, "upload skipped: no connected device");
        return ESP_ERR_INVALID_STATE;
    }
    if (!device.status.valid) {
        ESP_LOGW(TAG, "upload skipped: no valid status");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "upload skipped: WiFi disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    int http_status = 0;
    esp_err_t err = cloud_client_upload_status(s_core.gateway_id, &device, &http_status);
    ESP_LOGI(TAG, "status upload result err=%s http=%d", gateway_err_name(err), http_status);
    return err;
}

esp_err_t gateway_core_poll_now(void)
{
    gateway_device_t device = {0};
    if (!get_connected_device(&device)) {
        ESP_LOGW(TAG, "poll skipped: no connected device");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "poll skipped: WiFi disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    int http_status = 0;
    gateway_command_t command = {0};
    esp_err_t err = cloud_client_poll_command(s_core.gateway_id, &device, &command, &http_status);
    ESP_LOGI(TAG, "command poll result err=%s http=%d has_command=%d",
             gateway_err_name(err), http_status, command.has_command);
    if (err != ESP_OK || !command.has_command) {
        return err;
    }

    ESP_LOGI(TAG, "received cmd_id=%s cmd=%s", command.cmd_id, command.cmd);
    bool success = false;
    const char *message = "Command write complete";
    if (!is_supported_cmd(command.cmd)) {
        message = "Unsupported command";
    } else {
        esp_err_t write_err = ble_smartreef_write_command(command.cmd);
        success = write_err == ESP_OK;
        if (!success) {
            message = "BLE write failed";
        }
        ESP_LOGI(TAG, "local hardware execution cmd_id=%s cmd=%s result=%s",
                 command.cmd_id, command.cmd, success ? "success" : "failed");
    }

    int result_http = 0;
    esp_err_t report_err = cloud_client_report_command_result(command.cmd_id, success, message, &result_http);
    ESP_LOGI(TAG, "command result report cmd_id=%s err=%s http=%d",
             command.cmd_id, gateway_err_name(report_err), result_http);
    return report_err;
}

static void gateway_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        gateway_device_t device = {0};
        bool has_device = get_connected_device(&device);
        bool wifi_connected = wifi_manager_is_connected();

        if (!wifi_connected && now_ms - s_core.last_wifi_reconnect_ms >= 10000) {
            s_core.last_wifi_reconnect_ms = now_ms;
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
            ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_manager_reconnect());
        }

        if (wifi_connected && has_device && now_ms - s_core.last_poll_ms >= GATEWAY_CMD_POLL_INTERVAL_MS) {
            s_core.last_poll_ms = now_ms;
            esp_err_t err = gateway_core_poll_now();
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "periodic poll failed: %s", gateway_err_name(err));
            }
        }

        if (wifi_connected && has_device && device.status.valid &&
            now_ms - s_core.last_upload_ms >= GATEWAY_UPLOAD_INTERVAL_MS) {
            s_core.last_upload_ms = now_ms;
            esp_err_t err = gateway_core_upload_now();
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "periodic upload failed: %s", gateway_err_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t gateway_core_init(void)
{
    if (s_core.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gateway_id_generate(s_core.gateway_id, sizeof(s_core.gateway_id)),
                        TAG, "generate gateway_id failed");
    ESP_LOGI(TAG, "gateway_id=%s", s_core.gateway_id);

    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(ble_smartreef_init(on_identity, on_status, NULL), TAG, "ble init failed");
    s_core.initialized = true;
    return ESP_OK;
}

esp_err_t gateway_core_start(void)
{
    ESP_RETURN_ON_ERROR(gateway_core_init(), TAG, "core init failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_manager_connect());
    ESP_RETURN_ON_ERROR(ble_smartreef_start(), TAG, "ble start failed");

    if (s_core.task == NULL) {
        BaseType_t ok = xTaskCreate(gateway_task, "gateway_core", 8192, NULL, 5, &s_core.task);
        if (ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void gateway_core_print_status(void)
{
    gateway_device_t device = {0};
    ble_smartreef_get_device(&device);
    ESP_LOGI(TAG, "status gateway_id=%s wifi=%d ble=%d heap=%lu",
             s_core.gateway_id, wifi_manager_is_connected(), ble_smartreef_is_connected(),
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "device discovered=%d connected=%d id=%s reg=%s type=%s status_valid=%d seq=%u",
             device.discovered, device.connected, device.device_id, device.reg_code,
             device.device_type, device.status.valid, device.status.seq);
}

void gateway_core_print_devices(void)
{
    ble_smartreef_print_devices();
}

esp_err_t gateway_core_reconnect(void)
{
    ESP_LOGI(TAG, "manual reconnect requested");
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_manager_reconnect());
    return ble_smartreef_reconnect();
}

const char *gateway_core_get_gateway_id(void)
{
    return s_core.gateway_id;
}
