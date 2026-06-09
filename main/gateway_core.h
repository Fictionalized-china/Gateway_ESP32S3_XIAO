#pragma once

#include "esp_err.h"
#include "gateway_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gateway_core_init(void);
esp_err_t gateway_core_start(void);
void gateway_core_print_status(void);
void gateway_core_print_devices(void);
esp_err_t gateway_core_upload_now(void);
esp_err_t gateway_core_poll_now(void);
esp_err_t gateway_core_reconnect(void);
const char *gateway_core_get_gateway_id(void);

#ifdef __cplusplus
}
#endif
