#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define GATEWAY_ID_MAX_LEN 24
#define GATEWAY_DEVICE_ID_MAX_LEN 32
#define GATEWAY_REG_CODE_MAX_LEN 16
#define GATEWAY_DEVICE_TYPE_MAX_LEN 32
#define GATEWAY_CMD_ID_MAX_LEN 64
#define GATEWAY_CMD_NAME_MAX_LEN 32

typedef struct {
    int16_t temp_x10;
    uint16_t ph_x100;
    bool water_ok;
    bool light;
    bool heater;
    bool pump;
    bool ctrl_mode_auto;
    bool alarm_enable;
    uint8_t seq;
    int64_t updated_at_ms;
    bool valid;
} gateway_status_t;

typedef struct {
    char device_id[GATEWAY_DEVICE_ID_MAX_LEN];
    char reg_code[GATEWAY_REG_CODE_MAX_LEN];
    char device_type[GATEWAY_DEVICE_TYPE_MAX_LEN];
    gateway_status_t status;
    bool discovered;
    bool connected;
} gateway_device_t;

typedef struct {
    char cmd_id[GATEWAY_CMD_ID_MAX_LEN];
    char cmd[GATEWAY_CMD_NAME_MAX_LEN];
    bool has_command;
} gateway_command_t;

typedef void (*gateway_status_cb_t)(const gateway_device_t *device, void *ctx);
typedef void (*gateway_identity_cb_t)(const gateway_device_t *device, void *ctx);

static inline const char *gateway_err_name(esp_err_t err)
{
    return err == ESP_OK ? "OK" : esp_err_to_name(err);
}
