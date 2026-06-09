#include "ble_smartreef.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "status_codec.h"

static const char *TAG = "ble";

static const ble_uuid128_t SMARTREEF_SERVICE_UUID =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00);
static const ble_uuid128_t SMARTREEF_COMMAND_UUID =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xF1, 0xFF, 0x00, 0x00);
static const ble_uuid128_t SMARTREEF_STATUS_UUID =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xF2, 0xFF, 0x00, 0x00);
static const ble_uuid128_t SMARTREEF_IDENTITY_UUID =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xF3, 0xFF, 0x00, 0x00);

typedef enum {
    DISC_CHAR_COMMAND = 1,
    DISC_CHAR_STATUS,
    DISC_CHAR_IDENTITY,
} disc_char_t;

typedef struct {
    bool initialized;
    bool started;
    bool scanning;
    bool connecting;
    uint8_t own_addr_type;
    uint16_t conn_handle;
    uint16_t svc_start_handle;
    uint16_t svc_end_handle;
    uint16_t command_handle;
    uint16_t status_handle;
    uint16_t identity_handle;
    gateway_device_t device;
    SemaphoreHandle_t lock;
    gateway_identity_cb_t identity_cb;
    gateway_status_cb_t status_cb;
    void *cb_ctx;
} ble_state_t;

static ble_state_t s_ble = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void with_device_update_connected(bool connected)
{
    xSemaphoreTake(s_ble.lock, portMAX_DELAY);
    s_ble.device.connected = connected;
    xSemaphoreGive(s_ble.lock);
}

static bool advertised_name_matches(const struct ble_gap_disc_desc *disc, char *name, size_t name_len)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }

    const uint8_t *raw_name = fields.name;
    uint8_t raw_len = fields.name_len;
    if (raw_name == NULL || raw_len == 0) {
        return false;
    }

    size_t copy_len = raw_len < name_len - 1 ? raw_len : name_len - 1;
    memcpy(name, raw_name, copy_len);
    name[copy_len] = '\0';
    return strncmp(name, GATEWAY_BLE_SCAN_PREFIX, strlen(GATEWAY_BLE_SCAN_PREFIX)) == 0;
}

static void start_scan(void)
{
    if (!s_ble.started) {
        return;
    }

    struct ble_gap_disc_params params = {
        .filter_duplicates = 1,
        .passive = 0,
        .itvl = 0x0010,
        .window = 0x0010,
        .filter_policy = 0,
        .limited = 0,
    };

    int rc = ble_gap_disc(s_ble.own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) {
        s_ble.scanning = true;
        ESP_LOGI(TAG, "Scanning for BLE devices with prefix '%s'", GATEWAY_BLE_SCAN_PREFIX);
    } else {
        ESP_LOGE(TAG, "BLE scan start failed rc=%d", rc);
    }
}

static int subscribe_status_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    ESP_LOGI(TAG, "status subscribe write result status=%d", error->status);
    return 0;
}

static void subscribe_status(void)
{
    if (s_ble.status_handle == 0) {
        return;
    }

    uint16_t cccd_value = 1;
    uint16_t cccd_handle = s_ble.status_handle + 1;
    int rc = ble_gattc_write_flat(s_ble.conn_handle, cccd_handle, &cccd_value, sizeof(cccd_value),
                                  subscribe_status_cb, NULL);
    ESP_LOGI(TAG, "subscribe status handle=%u cccd=%u rc=%d", s_ble.status_handle, cccd_handle, rc);
}

static int read_status_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status != 0 || attr == NULL || attr->om == NULL) {
        ESP_LOGW(TAG, "initial status read failed status=%d", error->status);
        return 0;
    }

    uint8_t data[32] = {0};
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len > sizeof(data)) {
        len = sizeof(data);
    }
    os_mbuf_copydata(attr->om, 0, len, data);

    gateway_status_t status = {0};
    esp_err_t err = status_codec_parse_packet(data, len, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial status parse failed: %s", gateway_err_name(err));
        return 0;
    }

    xSemaphoreTake(s_ble.lock, portMAX_DELAY);
    s_ble.device.status = status;
    gateway_device_t snapshot = s_ble.device;
    xSemaphoreGive(s_ble.lock);

    ESP_LOGI(TAG, "status read temp=%.1f ph=%.2f water=%d light=%d heater=%d pump=%d mode=%s alarm=%d seq=%u",
             (double)status.temp_x10 / 10.0, (double)status.ph_x100 / 100.0,
             status.water_ok, status.light, status.heater, status.pump,
             status.ctrl_mode_auto ? "AUTO" : "MANUAL", status.alarm_enable, status.seq);
    if (s_ble.status_cb != NULL) {
        s_ble.status_cb(&snapshot, s_ble.cb_ctx);
    }
    return 0;
}

static int read_identity_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status != 0 || attr == NULL || attr->om == NULL) {
        ESP_LOGW(TAG, "identity read failed status=%d", error->status);
        return 0;
    }

    char identity[64] = {0};
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len >= sizeof(identity)) {
        len = sizeof(identity) - 1;
    }
    os_mbuf_copydata(attr->om, 0, len, identity);
    identity[len] = '\0';

    char *comma = strchr(identity, ',');
    if (identity[0] != 'I' || comma == NULL) {
        ESP_LOGW(TAG, "invalid identity: %s", identity);
        return 0;
    }

    *comma = '\0';
    const char *device_id = identity + 1;
    const char *reg_code = comma + 1;

    xSemaphoreTake(s_ble.lock, portMAX_DELAY);
    strlcpy(s_ble.device.device_id, device_id, sizeof(s_ble.device.device_id));
    strlcpy(s_ble.device.reg_code, reg_code, sizeof(s_ble.device.reg_code));
    strlcpy(s_ble.device.device_type, GATEWAY_DEVICE_TYPE_SMART_REEF_TANK, sizeof(s_ble.device.device_type));
    s_ble.device.discovered = true;
    s_ble.device.connected = true;
    gateway_device_t snapshot = s_ble.device;
    xSemaphoreGive(s_ble.lock);

    ESP_LOGI(TAG, "identity device_id=%s reg_code=%s device_type=%s",
             snapshot.device_id, snapshot.reg_code, snapshot.device_type);
    if (s_ble.identity_cb != NULL) {
        s_ble.identity_cb(&snapshot, s_ble.cb_ctx);
    }

    if (s_ble.status_handle != 0) {
        ble_gattc_read(s_ble.conn_handle, s_ble.status_handle, read_status_cb, NULL);
        subscribe_status();
    }
    return 0;
}

static void read_identity(void)
{
    if (s_ble.identity_handle == 0) {
        ESP_LOGW(TAG, "identity handle missing");
        return;
    }
    int rc = ble_gattc_read(s_ble.conn_handle, s_ble.identity_handle, read_identity_cb, NULL);
    ESP_LOGI(TAG, "read identity handle=%u rc=%d", s_ble.identity_handle, rc);
}

static void discover_next_characteristic(void);

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    disc_char_t which = (disc_char_t)(intptr_t)arg;
    if (error->status == 0 && chr != NULL) {
        if (which == DISC_CHAR_COMMAND) {
            s_ble.command_handle = chr->val_handle;
            ESP_LOGI(TAG, "command characteristic handle=%u", s_ble.command_handle);
        } else if (which == DISC_CHAR_STATUS) {
            s_ble.status_handle = chr->val_handle;
            ESP_LOGI(TAG, "status characteristic handle=%u", s_ble.status_handle);
        } else if (which == DISC_CHAR_IDENTITY) {
            s_ble.identity_handle = chr->val_handle;
            ESP_LOGI(TAG, "identity characteristic handle=%u", s_ble.identity_handle);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        discover_next_characteristic();
        return 0;
    }

    ESP_LOGW(TAG, "characteristic discovery failed which=%d status=%d", which, error->status);
    discover_next_characteristic();
    return 0;
}

static void discover_characteristic(disc_char_t which, const ble_uuid_t *uuid)
{
    int rc = ble_gattc_disc_chrs_by_uuid(s_ble.conn_handle, s_ble.svc_start_handle, s_ble.svc_end_handle,
                                         uuid, chr_disc_cb, (void *)(intptr_t)which);
    ESP_LOGI(TAG, "discover characteristic which=%d rc=%d", which, rc);
}

static void discover_next_characteristic(void)
{
    if (s_ble.command_handle == 0) {
        discover_characteristic(DISC_CHAR_COMMAND, &SMARTREEF_COMMAND_UUID.u);
    } else if (s_ble.status_handle == 0) {
        discover_characteristic(DISC_CHAR_STATUS, &SMARTREEF_STATUS_UUID.u);
    } else if (s_ble.identity_handle == 0) {
        discover_characteristic(DISC_CHAR_IDENTITY, &SMARTREEF_IDENTITY_UUID.u);
    } else {
        read_identity();
    }
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && service != NULL) {
        s_ble.svc_start_handle = service->start_handle;
        s_ble.svc_end_handle = service->end_handle;
        ESP_LOGI(TAG, "SmartReef service start=%u end=%u", s_ble.svc_start_handle, s_ble.svc_end_handle);
        discover_next_characteristic();
        return 0;
    }

    if (error->status == BLE_HS_EDONE && s_ble.svc_start_handle == 0) {
        ESP_LOGW(TAG, "SmartReef service not found");
    } else if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "service discovery failed status=%d", error->status);
    }
    return 0;
}

static void start_service_discovery(void)
{
    s_ble.svc_start_handle = 0;
    s_ble.svc_end_handle = 0;
    s_ble.command_handle = 0;
    s_ble.status_handle = 0;
    s_ble.identity_handle = 0;
    int rc = ble_gattc_disc_svc_by_uuid(s_ble.conn_handle, &SMARTREEF_SERVICE_UUID.u, svc_disc_cb, NULL);
    ESP_LOGI(TAG, "discover SmartReef service rc=%d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        char name[48] = {0};
        if (!advertised_name_matches(&event->disc, name, sizeof(name))) {
            return 0;
        }
        ESP_LOGI(TAG, "Found SmartReef advertisement name=%s rssi=%d", name, event->disc.rssi);
        s_ble.scanning = false;
        s_ble.connecting = true;
        ble_gap_disc_cancel();
        int rc = ble_gap_connect(s_ble.own_addr_type, &event->disc.addr, 30000, NULL, gap_event, NULL);
        ESP_LOGI(TAG, "connect start rc=%d", rc);
        if (rc != 0) {
            s_ble.connecting = false;
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        s_ble.connecting = false;
        if (event->connect.status == 0) {
            s_ble.conn_handle = event->connect.conn_handle;
            with_device_update_connected(true);
            ESP_LOGI(TAG, "BLE connected conn_handle=%u", s_ble.conn_handle);
            start_service_discovery();
        } else {
            ESP_LOGW(TAG, "BLE connect failed status=%d", event->connect.status);
            s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            with_device_update_connected(false);
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "BLE disconnected reason=%d", event->disconnect.reason);
        s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        with_device_update_connected(false);
        start_scan();
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle != s_ble.status_handle) {
            return 0;
        }
        uint8_t data[32] = {0};
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(data)) {
            len = sizeof(data);
        }
        os_mbuf_copydata(event->notify_rx.om, 0, len, data);

        gateway_status_t status = {0};
        esp_err_t err = status_codec_parse_packet(data, len, &status);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "notify status parse failed: %s len=%u", gateway_err_name(err), len);
            return 0;
        }

        xSemaphoreTake(s_ble.lock, portMAX_DELAY);
        s_ble.device.status = status;
        gateway_device_t snapshot = s_ble.device;
        xSemaphoreGive(s_ble.lock);

        ESP_LOGI(TAG, "notify temp=%.1f ph=%.2f water=%d light=%d heater=%d pump=%d mode=%s alarm=%d seq=%u",
                 (double)status.temp_x10 / 10.0, (double)status.ph_x100 / 100.0,
                 status.water_ok, status.light, status.heater, status.pump,
                 status.ctrl_mode_auto ? "AUTO" : "MANUAL", status.alarm_enable, status.seq);
        if (s_ble.status_cb != NULL) {
            s_ble.status_cb(&snapshot, s_ble.cb_ctx);
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe event attr=%u notify=%d indicate=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify, event->subscribe.cur_indicate);
        return 0;

    default:
        return 0;
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_ble.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return;
    }
    s_ble.started = true;
    start_scan();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_smartreef_init(gateway_identity_cb_t identity_cb, gateway_status_cb_t status_cb, void *cb_ctx)
{
    if (s_ble.initialized) {
        return ESP_OK;
    }

    s_ble.lock = xSemaphoreCreateMutex();
    if (s_ble.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ble.identity_cb = identity_cb;
    s_ble.status_cb = status_cb;
    s_ble.cb_ctx = cb_ctx;
    strlcpy(s_ble.device.device_type, GATEWAY_DEVICE_TYPE_SMART_REEF_TANK, sizeof(s_ble.device.device_type));

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_device_name_set("SmartReef-Gateway");

    nimble_port_freertos_init(ble_host_task);
    s_ble.initialized = true;
    return ESP_OK;
}

esp_err_t ble_smartreef_start(void)
{
    if (!s_ble.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ble.started && !s_ble.scanning && s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        start_scan();
    }
    return ESP_OK;
}

esp_err_t ble_smartreef_reconnect(void)
{
    ESP_LOGI(TAG, "BLE reconnect requested");
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_ble.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        start_scan();
    }
    return ESP_OK;
}

static int command_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    ESP_LOGI(TAG, "command write callback status=%d", error->status);
    return 0;
}

esp_err_t ble_smartreef_write_command(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE || s_ble.command_handle == 0) {
        ESP_LOGW(TAG, "cannot write command, BLE not ready");
        return ESP_ERR_INVALID_STATE;
    }

    char payload[64] = {0};
    int written = snprintf(payload, sizeof(payload), "{\"cmd\":\"%s\"}", cmd);
    if (written < 0 || written >= (int)sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int rc = ble_gattc_write_flat(s_ble.conn_handle, s_ble.command_handle, payload, strlen(payload),
                                  command_write_cb, NULL);
    ESP_LOGI(TAG, "write command handle=%u payload=%s rc=%d", s_ble.command_handle, payload, rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool ble_smartreef_is_connected(void)
{
    return s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

esp_err_t ble_smartreef_get_device(gateway_device_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ble.lock, portMAX_DELAY);
    *out = s_ble.device;
    xSemaphoreGive(s_ble.lock);
    return ESP_OK;
}

void ble_smartreef_print_devices(void)
{
    gateway_device_t device = {0};
    ble_smartreef_get_device(&device);
    ESP_LOGI(TAG, "device discovered=%d connected=%d device_id=%s reg_code=%s type=%s status_valid=%d seq=%u",
             device.discovered, device.connected, device.device_id, device.reg_code,
             device.device_type, device.status.valid, device.status.seq);
}
