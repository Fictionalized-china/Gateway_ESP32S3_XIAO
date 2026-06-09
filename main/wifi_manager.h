#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(void);
esp_err_t wifi_manager_reconnect(void);
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
