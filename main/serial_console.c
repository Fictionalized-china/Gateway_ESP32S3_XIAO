#include "serial_console.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_core.h"

static const char *TAG = "serial";

static void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || isspace((unsigned char)line[len - 1]))) {
        line[--len] = '\0';
    }
    char *start = line;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != line) {
        memmove(line, start, strlen(start) + 1);
    }
}

static void handle_command(const char *cmd)
{
    if (cmd[0] == '\0') {
        return;
    }
    if (strcmp(cmd, "status") == 0) {
        gateway_core_print_status();
    } else if (strcmp(cmd, "upload") == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gateway_core_upload_now());
    } else if (strcmp(cmd, "poll") == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gateway_core_poll_now());
    } else if (strcmp(cmd, "reconnect") == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gateway_core_reconnect());
    } else if (strcmp(cmd, "devices") == 0) {
        gateway_core_print_devices();
    } else {
        ESP_LOGW(TAG, "unknown command '%s'. Commands: status, upload, poll, reconnect, devices", cmd);
    }
}

static void serial_task(void *arg)
{
    (void)arg;
    char line[96] = {0};
    size_t len = 0;
    ESP_LOGI(TAG, "serial commands ready: status, upload, poll, reconnect, devices");

    while (true) {
        uint8_t ch = 0;
        int read = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(200));
        if (read <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            trim_line(line);
            handle_command(line);
            len = 0;
            line[0] = '\0';
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        }
    }
}

esp_err_t serial_console_start(void)
{
    esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "uart driver ready on UART%d", CONFIG_ESP_CONSOLE_UART_NUM);

    BaseType_t ok = xTaskCreate(serial_task, "serial_console", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
