#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gateway_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t status_codec_parse_packet(const uint8_t *data, size_t len, gateway_status_t *out);

#ifdef __cplusplus
}
#endif
