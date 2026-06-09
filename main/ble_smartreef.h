#pragma once

#include "esp_err.h"
#include "gateway_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_smartreef_init(gateway_identity_cb_t identity_cb, gateway_status_cb_t status_cb, void *cb_ctx);
esp_err_t ble_smartreef_start(void);
esp_err_t ble_smartreef_reconnect(void);
esp_err_t ble_smartreef_write_command(const char *cmd);
bool ble_smartreef_is_connected(void);
esp_err_t ble_smartreef_get_device(gateway_device_t *out);
void ble_smartreef_print_devices(void);

#ifdef __cplusplus
}
#endif
