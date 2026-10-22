#include "hub_provisioning_ble.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "hub_config.h"
#include "hub_event_log.h"
#include "hub_identity.h"
#include "hub_security.h"
#include "hub_setup_mode.h"
#include "hub_version.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "hub_provisioning_ble";

static const char *PROVISIONING_SERVICE_UUID_TEXT = "5b7d0001-6d4f-4c8f-9f4f-000000000001";
static const char *WIFI_CREDENTIALS_UUID_TEXT = "5b7d0002-6d4f-4c8f-9f4f-000000000001";
static const char *PROVISIONING_STATUS_UUID_TEXT = "5b7d0003-6d4f-4c8f-9f4f-000000000001";
static const char *SETUP_INFO_UUID_TEXT = "5b7d0004-6d4f-4c8f-9f4f-000000000001";
static const char *LOCAL_TOKEN_UUID_TEXT = "5b7d0005-6d4f-4c8f-9f4f-000000000001";
static const char *WIFI_CREDENTIALS_CHARACTERISTIC = "wifiCredentials";
static const char *PROVISIONING_STATUS_CHARACTERISTIC = "provisioningStatus";
static const char *SETUP_INFO_CHARACTERISTIC = "setupInfo";
static const char *LOCAL_TOKEN_CHARACTERISTIC = "localToken";

#define HUB_SETUP_INFO_JSON_CAPACITY 320

static const ble_uuid128_t PROVISIONING_SERVICE_UUID =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x9f, 0x8f, 0x4c, 0x4f, 0x6d, 0x01, 0x00, 0x7d, 0x5b);
static const ble_uuid128_t WIFI_CREDENTIALS_UUID =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x9f, 0x8f, 0x4c, 0x4f, 0x6d, 0x02, 0x00, 0x7d, 0x5b);
static const ble_uuid128_t PROVISIONING_STATUS_UUID =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x9f, 0x8f, 0x4c, 0x4f, 0x6d, 0x03, 0x00, 0x7d, 0x5b);
static const ble_uuid128_t SETUP_INFO_UUID =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x9f, 0x8f, 0x4c, 0x4f, 0x6d, 0x04, 0x00, 0x7d, 0x5b);
static const ble_uuid128_t LOCAL_TOKEN_UUID =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x9f, 0x8f, 0x4c, 0x4f, 0x6d, 0x05, 0x00, 0x7d, 0x5b);

static bool s_initialized;
static bool s_started;
static bool s_synced;
static bool s_wifi_accepted;
static uint8_t s_own_addr_type;
static uint16_t s_status_value_handle;
static uint16_t s_conn_handle = UINT16_MAX;
static uint16_t s_authorized_token_conn_handle = UINT16_MAX;
static bool s_token_consumed;
static bool s_restart_scheduled;
static SemaphoreHandle_t s_sync_sem;

typedef esp_err_t (*provisioning_write_handler_t)(uint16_t conn_handle, const char *json);
typedef esp_err_t (*provisioning_read_handler_t)(uint16_t conn_handle, char *value, size_t value_len);

static int provisioning_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_chr_def provisioning_characteristics[] = {
    {
        .uuid = &WIFI_CREDENTIALS_UUID.u,
        .access_cb = provisioning_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &PROVISIONING_STATUS_UUID.u,
        .access_cb = provisioning_gatt_access_cb,
        .val_handle = &s_status_value_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &SETUP_INFO_UUID.u,
        .access_cb = provisioning_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &LOCAL_TOKEN_UUID.u,
        .access_cb = provisioning_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {0},
};

static const struct ble_gatt_svc_def provisioning_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &PROVISIONING_SERVICE_UUID.u,
        .characteristics = provisioning_characteristics,
    },
    {0},
};

static esp_err_t build_redacted_status(char *body, size_t body_len, const char *state, esp_err_t status)
{
    if (body == NULL || body_len == 0 || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(
        body,
        body_len,
        "{\"ok\":%s,\"state\":\"%s\",\"wifiAccepted\":%s,\"restartRequired\":%s}",
        status == ESP_OK ? "true" : "false",
        state,
        s_wifi_accepted ? "true" : "false",
        s_wifi_accepted ? "true" : "false"
    );
    return ESP_OK;
}

static esp_err_t build_setup_info(char *body, size_t body_len)
{
    if (body == NULL || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK) {
        return err;
    }

    const char *hub_id = hub_identity_hub_id();
    const char *spki_sha256 = hub_identity_spki_sha256();
    bool identity_complete = hub_identity_ready()
        && strlen(hub_id) == HUB_IDENTITY_HUB_ID_CAPACITY - 1
        && strlen(spki_sha256) == HUB_IDENTITY_SPKI_SHA256_HEX_CAPACITY - 1;
    int written;
    if (identity_complete) {
        written = snprintf(
            body,
            body_len,
            "{\"model\":\"%s\",\"firmwareVersion\":\"%s\",\"setupState\":\"%s\","
            "\"deviceId\":\"%02X%02X%02X\",\"hubId\":\"%s\",\"spkiSha256\":\"%s\","
            "\"credentialVersion\":%d}",
            LAMABUDDY_DEVICE_MODEL,
            LAMABUDDY_FIRMWARE_VERSION,
            hub_setup_mode_window_active() ? "active" : "inactive",
            mac[3],
            mac[4],
            mac[5],
            hub_id,
            spki_sha256,
            HUB_IDENTITY_CREDENTIAL_VERSION
        );
    } else {
        written = snprintf(
            body,
            body_len,
            "{\"model\":\"%s\",\"firmwareVersion\":\"%s\",\"setupState\":\"%s\","
            "\"deviceId\":\"%02X%02X%02X\"}",
            LAMABUDDY_DEVICE_MODEL,
            LAMABUDDY_FIRMWARE_VERSION,
            hub_setup_mode_window_active() ? "active" : "inactive",
            mac[3],
            mac[4],
            mac[5]
        );
    }
    if (written < 0 || (size_t)written >= body_len) {
        body[0] = '\0';
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t read_setup_session_token(uint16_t conn_handle, char *value, size_t value_len)
{
    if (!s_wifi_accepted
        || s_token_consumed
        || conn_handle == UINT16_MAX
        || conn_handle != s_authorized_token_conn_handle
        || !hub_setup_mode_window_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = hub_security_get_setup_token(value, value_len);
    return err;
}

static bool copy_json_string(cJSON *root, const char *key, char *value, size_t value_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return false;
    }
    snprintf(value, value_len, "%s", item->valuestring);
    return true;
}

static esp_err_t save_wifi_credentials_json(uint16_t conn_handle, const char *json)
{
    if (json == NULL || !hub_setup_mode_window_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char ssid[HUB_CONFIG_MAX_SSID_LEN + 1] = {0};
    char password[HUB_CONFIG_MAX_PASSWORD_LEN + 1] = {0};
    bool ok = copy_json_string(root, "ssid", ssid, sizeof(ssid))
        && copy_json_string(root, "password", password, sizeof(password));
    cJSON_Delete(root);

    if (!ok) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = hub_config_save_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
        s_wifi_accepted = true;
        s_authorized_token_conn_handle = conn_handle;
        s_token_consumed = false;
        hub_event_log_record("setupBLE", "wifiAccepted", true, "Wi-Fi accepted");
    }
    memset(password, 0, sizeof(password));
    return err;
}

static int append_string(struct os_mbuf *om, const char *value)
{
    int rc = os_mbuf_append(om, value, (uint16_t)strlen(value));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static esp_err_t notify_status(const char *state, esp_err_t status)
{
    if (s_conn_handle == UINT16_MAX || s_status_value_handle == 0) {
        return ESP_OK;
    }

    char body[128] = {0};
    esp_err_t err = build_redacted_status(body, sizeof(body), state, status);
    if (err != ESP_OK) {
        return err;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(body, strlen(body));
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_status_value_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static int write_wifi_credentials(uint16_t conn_handle, struct os_mbuf *om)
{
    char json[192] = {0};
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(om, json, sizeof(json) - 1, &len);
    if (rc != 0 || len == 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    json[len] = '\0';

    esp_err_t err = save_wifi_credentials_json(conn_handle, json);
    memset(json, 0, sizeof(json));
    if (err != ESP_OK) {
        (void)notify_status("failed", err);
        return BLE_ATT_ERR_UNLIKELY;
    }

    (void)notify_status("wifiAccepted", ESP_OK);
    return 0;
}

static int read_status(struct os_mbuf *om)
{
    char body[128] = {0};
    esp_err_t err = build_redacted_status(body, sizeof(body), s_wifi_accepted ? "wifiAccepted" : "advertising", ESP_OK);
    if (err != ESP_OK) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return append_string(om, body);
}

static int read_setup_info(struct os_mbuf *om)
{
    char body[HUB_SETUP_INFO_JSON_CAPACITY] = {0};
    esp_err_t err = build_setup_info(body, sizeof(body));
    if (err != ESP_OK) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return append_string(om, body);
}

static void restart_after_provisioning_task(void *param)
{
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

static void schedule_restart_after_provisioning(void)
{
    if (s_restart_scheduled) {
        return;
    }

    s_restart_scheduled = true;
    hub_event_log_record("setupBLE", "restartScheduled", true, "restart scheduled after provisioning");
    if (xTaskCreate(restart_after_provisioning_task, "prov_restart", 2048, NULL, 5, NULL) != pdPASS) {
        s_restart_scheduled = false;
        ESP_LOGW(TAG, "setup provisioning restart scheduling failed");
    }
}

static int read_local_token(uint16_t conn_handle, struct os_mbuf *om)
{
    char token[HUB_CONFIG_TOKEN_LEN] = {0};
    esp_err_t err = read_setup_session_token(conn_handle, token, sizeof(token));
    if (err != ESP_OK) {
        memset(token, 0, sizeof(token));
        hub_event_log_record("setupBLE", "localTokenRead", false, "local pairing material read denied");
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    int rc = append_string(om, token);
    if (rc == 0) {
        hub_event_log_record("setupBLE", "localTokenRead", true, "local pairing material read accepted");
        s_token_consumed = true;
        s_authorized_token_conn_handle = UINT16_MAX;
        hub_setup_mode_mark_completed();
        hub_provisioning_ble_stop();
        schedule_restart_after_provisioning();
    }
    memset(token, 0, sizeof(token));
    return rc;
}

static int provisioning_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    const ble_uuid_t *uuid = ctxt->chr->uuid;
    if (ble_uuid_cmp(uuid, &WIFI_CREDENTIALS_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return write_wifi_credentials(conn_handle, ctxt->om);
    }
    if (ble_uuid_cmp(uuid, &PROVISIONING_STATUS_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return read_status(ctxt->om);
    }
    if (ble_uuid_cmp(uuid, &SETUP_INFO_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return read_setup_info(ctxt->om);
    }
    if (ble_uuid_cmp(uuid, &LOCAL_TOKEN_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return read_local_token(conn_handle, ctxt->om);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static esp_err_t start_advertising(void);

static int provisioning_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "setup provisioning client connected");
            hub_event_log_record("setupBLE", "clientConnected", true, "setup client connected");
        } else if (s_started) {
            (void)start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_authorized_token_conn_handle) {
            s_authorized_token_conn_handle = UINT16_MAX;
        }
        s_conn_handle = UINT16_MAX;
        ESP_LOGI(TAG, "setup provisioning client disconnected");
        hub_event_log_record("setupBLE", "clientDisconnected", true, "setup client disconnected");
        if (s_started && hub_setup_mode_window_active()) {
            (void)start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_started && hub_setup_mode_window_active()) {
            (void)start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "setup provisioning status subscription changed");
        return 0;
    default:
        return 0;
    }
}

static esp_err_t start_advertising(void)
{
    if (!hub_setup_mode_window_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    char device_name[32] = {0};
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(device_name, sizeof(device_name), "LamaBuddy Setup %02X%02X%02X", mac[3], mac[4], mac[5]);
    ble_svc_gap_device_name_set(device_name);

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&PROVISIONING_SERVICE_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "setup provisioning advertising fields failed: rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_hs_adv_fields scan_response_fields;
    memset(&scan_response_fields, 0, sizeof(scan_response_fields));
    scan_response_fields.name = (uint8_t *)device_name;
    scan_response_fields.name_len = (uint8_t)strlen(device_name);
    scan_response_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&scan_response_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "setup provisioning scan response fields failed: rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, provisioning_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "setup provisioning advertising start failed: rc=%d", rc);
        hub_event_log_record("setupBLE", "advertisingStartFailed", false, "advertising start failed");
        return ESP_FAIL;
    }

    hub_event_log_record("setupBLE", "advertisingStarted", true, "setup advertising started");
    return ESP_OK;
}

static void provisioning_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGW(TAG, "setup provisioning address inference failed: rc=%d", rc);
        return;
    }
    s_synced = true;
    if (s_sync_sem != NULL) {
        xSemaphoreGive(s_sync_sem);
    }
}

static void provisioning_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void bind_provisioning_handlers(void)
{
    volatile provisioning_write_handler_t wifi_write = save_wifi_credentials_json;
    volatile provisioning_read_handler_t setup_token_read = read_setup_session_token;
    (void)wifi_write;
    (void)setup_token_read;
}

esp_err_t hub_provisioning_ble_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_sync_sem = xSemaphoreCreateBinary();
    if (s_sync_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGW(TAG, "setup provisioning NimBLE init failed: rc=%d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("LamaBuddy Setup");

    rc = ble_gatts_count_cfg(provisioning_services);
    if (rc != 0) {
        ESP_LOGW(TAG, "setup provisioning GATT count failed: rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(provisioning_services);
    if (rc != 0) {
        ESP_LOGW(TAG, "setup provisioning GATT add failed: rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = provisioning_on_sync;
    nimble_port_freertos_init(provisioning_host_task);

    s_initialized = true;
    s_started = false;
    s_synced = false;
    s_wifi_accepted = false;
    s_conn_handle = UINT16_MAX;
    s_authorized_token_conn_handle = UINT16_MAX;
    s_token_consumed = false;
    s_restart_scheduled = false;

    char setup_info[HUB_SETUP_INFO_JSON_CAPACITY] = {0};
    (void)build_setup_info(setup_info, sizeof(setup_info));
    bind_provisioning_handlers();
    (void)PROVISIONING_SERVICE_UUID_TEXT;
    (void)WIFI_CREDENTIALS_UUID_TEXT;
    (void)PROVISIONING_STATUS_UUID_TEXT;
    (void)SETUP_INFO_UUID_TEXT;
    (void)LOCAL_TOKEN_UUID_TEXT;
    (void)WIFI_CREDENTIALS_CHARACTERISTIC;
    (void)PROVISIONING_STATUS_CHARACTERISTIC;
    (void)SETUP_INFO_CHARACTERISTIC;
    (void)LOCAL_TOKEN_CHARACTERISTIC;
    ESP_LOGI(TAG, "setup provisioning shell initialized");
    hub_event_log_record("setupBLE", "initialized", true, "provisioning shell initialized");
    return ESP_OK;
}

esp_err_t hub_provisioning_ble_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hub_setup_mode_window_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_synced && xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_started = true;
    s_authorized_token_conn_handle = UINT16_MAX;
    s_token_consumed = false;
    char status[128] = {0};
    (void)build_redacted_status(status, sizeof(status), "advertising", ESP_OK);
    esp_err_t err = start_advertising();
    if (err != ESP_OK) {
        s_started = false;
        return err;
    }
    ESP_LOGI(TAG, "setup provisioning advertising started");
    return ESP_OK;
}

void hub_provisioning_ble_stop(void)
{
    if (!s_started) {
        return;
    }

    s_started = false;
    s_authorized_token_conn_handle = UINT16_MAX;
    (void)ble_gap_adv_stop();
    ESP_LOGI(TAG, "setup provisioning advertising stopped");
}
