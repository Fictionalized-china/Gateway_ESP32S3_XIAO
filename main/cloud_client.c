#include "cloud_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cloud";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    response_buf_t *buf = (response_buf_t *)evt->user_data;
    if (buf == NULL) {
        return ESP_OK;
    }

    size_t needed = buf->len + (size_t)evt->data_len + 1;
    if (needed > buf->cap) {
        size_t next_cap = buf->cap == 0 ? 256 : buf->cap * 2;
        while (next_cap < needed) {
            next_cap *= 2;
        }
        char *next = realloc(buf->data, next_cap);
        if (next == NULL) {
            return ESP_ERR_NO_MEM;
        }
        buf->data = next;
        buf->cap = next_cap;
    }

    memcpy(buf->data + buf->len, evt->data, (size_t)evt->data_len);
    buf->len += (size_t)evt->data_len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static char *build_url(const char *path_and_query)
{
    size_t base_len = strlen(GATEWAY_CLOUD_BASE_URL);
    size_t path_len = strlen(path_and_query);
    bool need_slash = base_len > 0 && GATEWAY_CLOUD_BASE_URL[base_len - 1] != '/' && path_and_query[0] != '/';
    char *url = calloc(1, base_len + path_len + (need_slash ? 2 : 1));
    if (url == NULL) {
        return NULL;
    }
    strcpy(url, GATEWAY_CLOUD_BASE_URL);
    if (need_slash) {
        strcat(url, "/");
    }
    strcat(url, path_and_query);
    return url;
}

static esp_err_t perform_request(const char *method, const char *path_and_query, const char *body,
                                 response_buf_t *response, int *http_status)
{
    if (http_status != NULL) {
        *http_status = 0;
    }

    char *url = build_url(path_and_query);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = GATEWAY_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(url);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "X-Gateway-Key", GATEWAY_KEY);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (body != NULL) {
            esp_http_client_set_post_field(client, body, (int)strlen(body));
        }
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (http_status != NULL) {
        *http_status = status;
    }
    ESP_LOGI(TAG, "%s %s -> err=%s http=%d body_len=%d",
             method, path_and_query, gateway_err_name(err), status, esp_http_client_get_content_length(client));

    esp_http_client_cleanup(client);
    free(url);
    return err;
}

static void add_status_json(cJSON *parent, const gateway_status_t *status)
{
    cJSON_AddNumberToObject(parent, "temp_x10", status->temp_x10);
    cJSON_AddNumberToObject(parent, "temp_c", (double)status->temp_x10 / 10.0);
    cJSON_AddNumberToObject(parent, "ph_x100", status->ph_x100);
    cJSON_AddNumberToObject(parent, "ph", (double)status->ph_x100 / 100.0);
    cJSON_AddBoolToObject(parent, "water_ok", status->water_ok);
    cJSON_AddBoolToObject(parent, "light", status->light);
    cJSON_AddBoolToObject(parent, "heater", status->heater);
    cJSON_AddBoolToObject(parent, "pump", status->pump);
    cJSON_AddStringToObject(parent, "ctrl_mode", status->ctrl_mode_auto ? "AUTO" : "MANUAL");
    cJSON_AddBoolToObject(parent, "alarm_enable", status->alarm_enable);
    cJSON_AddNumberToObject(parent, "seq", status->seq);
}

esp_err_t cloud_client_upload_status(const char *gateway_id, const gateway_device_t *device, int *http_status)
{
    if (gateway_id == NULL || device == NULL || !device->status.valid) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "gateway_id", gateway_id);
    cJSON_AddStringToObject(root, "device_id", device->device_id);
    if (device->reg_code[0] != '\0') {
        cJSON_AddStringToObject(root, "reg_code", device->reg_code);
    }
    cJSON_AddStringToObject(root, "device_type", device->device_type);
    cJSON *status = cJSON_AddObjectToObject(root, "status");
    add_status_json(status, &device->status);
    cJSON_AddNumberToObject(root, "ts_ms", esp_timer_get_time() / 1000);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    response_buf_t response = {0};
    esp_err_t err = perform_request("POST", "/gateway/upload", body, &response, http_status);
    ESP_LOGI(TAG, "upload status %s http=%d heap=%lu", gateway_err_name(err),
             http_status ? *http_status : 0, (unsigned long)esp_get_free_heap_size());

    free(response.data);
    free(body);
    return err;
}

esp_err_t cloud_client_poll_command(const char *gateway_id, const gateway_device_t *device,
                                    gateway_command_t *out, int *http_status)
{
    if (gateway_id == NULL || device == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char path[192] = {0};
    snprintf(path, sizeof(path), "/gateway/cmd?gateway_id=%s&device_id=%s", gateway_id, device->device_id);

    response_buf_t response = {0};
    esp_err_t err = perform_request("GET", path, NULL, &response, http_status);
    if (err != ESP_OK) {
        free(response.data);
        return err;
    }
    if (response.data == NULL || response.len == 0) {
        free(response.data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (root == NULL) {
        ESP_LOGW(TAG, "poll command parse failed: %s", response.data);
        free(response.data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cmd == NULL || cJSON_IsNull(cmd)) {
        ESP_LOGI(TAG, "command poll: no command");
        cJSON_Delete(root);
        free(response.data);
        return ESP_OK;
    }

    cJSON *cmd_id = cJSON_GetObjectItem(root, "cmd_id");
    cJSON *cmd_name = cJSON_GetObjectItem(cmd, "cmd");
    if (!cJSON_IsString(cmd_id) || !cJSON_IsString(cmd_name)) {
        cJSON_Delete(root);
        free(response.data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(out->cmd_id, cmd_id->valuestring, sizeof(out->cmd_id));
    strlcpy(out->cmd, cmd_name->valuestring, sizeof(out->cmd));
    out->has_command = true;
    ESP_LOGI(TAG, "command poll: cmd_id=%s cmd=%s", out->cmd_id, out->cmd);

    cJSON_Delete(root);
    free(response.data);
    return ESP_OK;
}

esp_err_t cloud_client_report_command_result(const char *cmd_id, bool success,
                                             const char *message, int *http_status)
{
    if (cmd_id == NULL || cmd_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd_id", cmd_id);
    cJSON_AddStringToObject(root, "status", success ? "success" : "failed");
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON_AddStringToObject(result, "message", message != NULL ? message : "");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    response_buf_t response = {0};
    esp_err_t err = perform_request("POST", "/gateway/cmd/result", body, &response, http_status);
    ESP_LOGI(TAG, "cmd result cmd_id=%s success=%d err=%s http=%d",
             cmd_id, success, gateway_err_name(err), http_status ? *http_status : 0);

    free(response.data);
    free(body);
    return err;
}
