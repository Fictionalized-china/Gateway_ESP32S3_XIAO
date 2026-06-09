#pragma once

#include "esp_err.h"
#include "gateway_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cloud_client_upload_status(const char *gateway_id, const gateway_device_t *device, int *http_status);
esp_err_t cloud_client_poll_command(const char *gateway_id, const gateway_device_t *device,
                                    gateway_command_t *out, int *http_status);
esp_err_t cloud_client_report_command_result(const char *cmd_id, bool success,
                                             const char *message, int *http_status);

#ifdef __cplusplus
}
#endif
