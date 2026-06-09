#include "gateway_id.h"

#include <stdio.h>
#include "esp_mac.h"

esp_err_t gateway_id_generate(char *out, size_t out_len)
{
    if (out == NULL || out_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }

    int written = snprintf(out, out_len, "GW-%02X%02X%02X%02X%02X%02X",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
