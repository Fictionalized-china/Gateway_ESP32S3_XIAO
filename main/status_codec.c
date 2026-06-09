#include "status_codec.h"

#include <string.h>
#include "esp_timer.h"

#define SMARTREEF_STATUS_LEN 11
#define SMARTREEF_STATUS_MAGIC 0x53
#define SMARTREEF_STATUS_VERSION 1

static int16_t read_i16_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

esp_err_t status_codec_parse_packet(const uint8_t *data, size_t len, gateway_status_t *out)
{
    if (data == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != SMARTREEF_STATUS_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (data[0] != SMARTREEF_STATUS_MAGIC || data[1] != SMARTREEF_STATUS_VERSION) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    gateway_status_t parsed = {0};
    parsed.temp_x10 = read_i16_le(&data[2]);
    parsed.ph_x100 = read_u16_le(&data[4]);
    parsed.water_ok = data[6] == 1;
    parsed.light = (data[7] & 0x01) != 0;
    parsed.heater = (data[7] & 0x02) != 0;
    parsed.pump = (data[7] & 0x04) != 0;
    parsed.ctrl_mode_auto = data[8] == 1;
    parsed.alarm_enable = data[9] == 1;
    parsed.seq = data[10];
    parsed.updated_at_ms = esp_timer_get_time() / 1000;
    parsed.valid = true;

    memcpy(out, &parsed, sizeof(parsed));
    return ESP_OK;
}
