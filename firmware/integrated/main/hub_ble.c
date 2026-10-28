#include "hub_ble.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "hub_config.h"
#include "hub_command_coordinator.h"
#include "hub_cloud.h"
#include "hub_brew_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "hub_ble";

static const char *LM_READ_UUID_TEXT = "0a0b7847-e12b-09a8-b04b-8e0922a9abab";
static const char *LM_WRITE_UUID_TEXT = "0b0b7847-e12b-09a8-b04b-8e0922a9abab";
static const char *LM_AUTH_UUID_TEXT = "0d0b7847-e12b-09a8-b04b-8e0922a9abab";
static const int HUB_BLE_SCAN_NAME_TIMEOUT_MS = 8000;
static const int HUB_BLE_SCAN_FALLBACK_TIMEOUT_MS = 6000;
static const int HUB_BLE_CONNECT_TIMEOUT_MS = 5000;
static const int HUB_BLE_DIRECT_CONNECT_TIMEOUT_MS = 2500;
static const int HUB_BLE_CONNECT_CANCEL_TIMEOUT_MS = 1500;
static const int HUB_BLE_CONNECT_CANCEL_POLL_MS = 50;
static const int HUB_BLE_SCAN_CANCEL_TIMEOUT_MS = 1500;
static const int HUB_BLE_SCAN_CANCEL_POLL_MS = 50;
static const int HUB_BLE_DISCOVERY_TIMEOUT_MS = 5000;
static const int HUB_BLE_WRITE_TIMEOUT_MS = 5000;
static const int HUB_BLE_READ_TIMEOUT_MS = 5000;
static const int HUB_BLE_READ_RESPONSE_RETRY_COUNT = 4;
static const int HUB_BLE_READ_RESPONSE_RETRY_DELAY_MS = 250;
static const int HUB_BLE_OPERATION_MUTEX_TIMEOUT_MS = 20000;
static const int HUB_BLE_COMMAND_SYNC_TIMEOUT_MS = 250;
/*
 * A scheduled realtime cache read normally owns the BLE mutex for roughly
 * 350-450 ms.  Let an interactive command wait through that bounded overlap,
 * while the absolute 1500 ms command deadline still caps the whole path.
 */
static const int HUB_BLE_COMMAND_MUTEX_TIMEOUT_MS = 650;
static const int HUB_BLE_COMMAND_DEADLINE_MS = 1500;
/*
 * A direct ATT read itself occupies roughly 150-250 ms on the real Mini R.
 * Keep only a short cooperative gap after a mismatching observation; the
 * read latency supplies the remainder without weakening target validation.
 */
static const int HUB_BLE_STATE_CONFIRMATION_POLL_MS = 25;
static const int HUB_BLE_FULL_READ_MUTEX_TIMEOUT_MS = 1000;
static const int HUB_BLE_FULL_READ_GATT_TIMEOUT_MS = 1200;
static const int HUB_BLE_WORKER_STACK_BYTES = 12288;
static const int HUB_BLE_WORKER_BOOT_DELAY_MS = 10000;
static const int HUB_BLE_WORKER_POLL_MS = 30000;
static const int HUB_BLE_WORKER_PRIORITY_RECHECK_MS = 1000;
static const int HUB_BLE_WORKER_RETRY_DELAYS_MS[] = {60000, 120000, 300000};
static const int HUB_BLE_STATE_CACHE_TTL_MS = 45000;
static const int HUB_BLE_BREW_TIMER_MUTEX_TIMEOUT_MS = 0;
static const int HUB_BLE_NOTIFY_PROBE_MUTEX_TIMEOUT_MS = 1000;
static const int HUB_BLE_NOTIFY_PROBE_MIN_MS = 1000;
static const int HUB_BLE_NOTIFY_PROBE_MAX_MS = 60000;
static const int HUB_BLE_NOTIFY_PROBE_DEFAULT_MS = 30000;
static const int HUB_BLE_NOTIFY_PROBE_SUBSCRIBE_SETTLE_MS = 1000;
static const int HUB_BLE_PROTOCOL_PROBE_MUTEX_TIMEOUT_MS = 1000;
static const int HUB_BLE_PROTOCOL_PROBE_MIN_MS = 1000;
static const int HUB_BLE_PROTOCOL_PROBE_MAX_MS = 120000;
static const int HUB_BLE_PROTOCOL_PROBE_DEFAULT_MS = 30000;
static const int HUB_BLE_PROTOCOL_PROBE_MIN_INTERVAL_MS = 0;
static const int HUB_BLE_PROTOCOL_PROBE_MAX_INTERVAL_MS = 5000;
static const int HUB_BLE_PROTOCOL_PROBE_DEFAULT_INTERVAL_MS = 300;
static const int HUB_BLE_GATT_READ_PROBE_MUTEX_TIMEOUT_MS = 1000;
static const int HUB_BLE_GATT_READ_PROBE_MIN_MS = 1000;
static const int HUB_BLE_GATT_READ_PROBE_MAX_MS = 120000;
static const int HUB_BLE_GATT_READ_PROBE_DEFAULT_MS = 30000;
static const int HUB_BLE_GATT_READ_PROBE_DEFAULT_INTERVAL_MS = 300;
#define HUB_BLE_PROTOCOL_PROBE_MAX_SAMPLES 64
#define HUB_BLE_PROTOCOL_PROBE_PAYLOAD_MAX_BYTES 160
#define HUB_BLE_PROTOCOL_PROBE_RAW_BYTES 128
#define HUB_BLE_GATT_READ_PROBE_MAX_SAMPLES 56
#define HUB_BLE_GATT_READ_PROBE_HEX_CHARS 32

static int ble_worker_retry_delay_ms(int consecutive_failures)
{
    size_t count = sizeof(HUB_BLE_WORKER_RETRY_DELAYS_MS) / sizeof(HUB_BLE_WORKER_RETRY_DELAYS_MS[0]);
    size_t index = consecutive_failures > 0 ? (size_t)(consecutive_failures - 1) : 0;
    if (index >= count) {
        index = count - 1;
    }
    return HUB_BLE_WORKER_RETRY_DELAYS_MS[index];
}
#define HUB_BLE_GATT_READ_PROBE_TEXT_BYTES 220
#define HUB_BLE_NOTIFY_PROBE_MAX_SAMPLES 32
#define HUB_BLE_NOTIFY_PROBE_SAMPLE_HEX_CHARS 96
#define HUB_BLE_GATT_PROBE_MAX_SERVICES 16
#define HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS 64

static const ble_uuid_t *LM_READ_UUID =
    BLE_UUID128_DECLARE(0xab, 0xab, 0xa9, 0x22, 0x09, 0x8e, 0x4b, 0xb0,
                        0xa8, 0x09, 0x2b, 0xe1, 0x47, 0x78, 0x0b, 0x0a);
static const ble_uuid_t *LM_WRITE_UUID =
    BLE_UUID128_DECLARE(0xab, 0xab, 0xa9, 0x22, 0x09, 0x8e, 0x4b, 0xb0,
                        0xa8, 0x09, 0x2b, 0xe1, 0x47, 0x78, 0x0b, 0x0b);
static const ble_uuid_t *LM_AUTH_UUID =
    BLE_UUID128_DECLARE(0xab, 0xab, 0xa9, 0x22, 0x09, 0x8e, 0x4b, 0xb0,
                        0xa8, 0x09, 0x2b, 0xe1, 0x47, 0x78, 0x0b, 0x0d);

typedef struct {
    bool configured;
    bool reads_validated;
    bool writes_validated;
    bool refresh_pending;
    bool connected;
    bool has_cached_state;
    bool manual_intervention_recommended;
    bool worker_started;
    int consecutive_failures;
    int rssi;
    int64_t last_seen_ms;
    int64_t last_read_ms;
    int64_t cached_at_ms;
    uint32_t cache_sequence;
    uint32_t selection_generation;
    char status[24];
    char device_name[64];
    char device_address[32];
    char match_reason[32];
    char machine_mode[32];
    char selection_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    char last_error[128];
    char recovery_hint[48];
    char cache_origin[24];
    char state_json[1536];
} hub_ble_state_t;

typedef struct {
    hub_machine_selection_t selection;
    hub_machine_selection_identity_t selection_identity;
    ble_addr_t target_addr;
    bool target_found;
    bool connected;
    bool reused_session;
    uint16_t conn_handle;
    uint32_t connection_epoch;
    uint16_t auth_handle;
    uint16_t read_handle;
    uint16_t write_handle;
    const ble_uuid_t *discover_uuid;
    uint16_t *discover_handle;
    int rc;
    int rssi;
    int scan_seen_count;
    int scan_named_count;
    char target_name[64];
    char target_address[32];
    char match_reason[32];
    char last_scan_name[64];
    char last_scan_address[32];
    char read_response[1024];
    size_t read_response_len;
} hub_ble_operation_t;

typedef struct {
    bool has_coffee_target;
    bool has_steam_target;
    bool has_steam_enabled;
    double coffee_target;
    double steam_target;
    bool steam_enabled;
} hub_ble_noop_targets_t;

typedef enum {
    HUB_BLE_CONFIRM_NONE = 0,
    HUB_BLE_CONFIRM_COFFEE_TARGET,
    HUB_BLE_CONFIRM_STEAM_TARGET,
    HUB_BLE_CONFIRM_STEAM_ENABLED,
} hub_ble_confirmation_kind_t;

typedef struct {
    hub_ble_confirmation_kind_t kind;
    double target;
    bool enabled;
} hub_ble_confirmation_target_t;

typedef struct {
    bool indication;
    uint16_t attr_handle;
    int elapsed_ms;
    int length;
    bool truncated;
    char hex[HUB_BLE_NOTIFY_PROBE_SAMPLE_HEX_CHARS + 1];
} hub_ble_notify_sample_t;

typedef struct {
    bool active;
    uint16_t conn_handle;
    int64_t started_ms;
    int notification_count;
    int indication_count;
    int sample_count;
    hub_ble_notify_sample_t samples[HUB_BLE_NOTIFY_PROBE_MAX_SAMPLES];
} hub_ble_notify_probe_state_t;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    char uuid[BLE_UUID_STR_LEN];
} hub_ble_gatt_service_t;

typedef struct {
    const char *name;
    const char *payload;
} hub_ble_protocol_candidate_t;

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint8_t properties;
    char uuid[BLE_UUID_STR_LEN];
} hub_ble_gatt_characteristic_t;

typedef struct {
    int rc;
    int service_count;
    int characteristic_count;
    bool services_truncated;
    bool characteristics_truncated;
    hub_ble_gatt_service_t services[HUB_BLE_GATT_PROBE_MAX_SERVICES];
    hub_ble_gatt_characteristic_t characteristics[HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS];
} hub_ble_gatt_probe_t;

typedef struct {
    hub_ble_worker_job_fn function;
    void *context;
} hub_ble_worker_job_t;

typedef struct {
    bool boilers;
    uint16_t conn_handle;
    uint32_t connection_epoch;
    uint16_t read_handle;
    uint16_t write_handle;
    uint32_t selection_generation;
    uint32_t observation_sequence;
    uint64_t auth_fingerprint;
    char selection_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
} hub_ble_read_selector_proof_t;

static hub_ble_state_t ble_state = {
    .status = "unavailable",
    .state_json = "{\"connected\":false,\"deviceName\":null,\"deviceAddress\":null,\"rssi\":null,\"machineMode\":null,\"tankStatus\":null,\"boilers\":[],\"smartStandby\":null,\"lastReadAt\":null,\"source\":\"ble\"}",
};

static SemaphoreHandle_t ble_op_mutex;
static SemaphoreHandle_t ble_sync_sem;
static SemaphoreHandle_t ble_scan_sem;
static SemaphoreHandle_t ble_connect_sem;
static SemaphoreHandle_t ble_discover_sem;
static SemaphoreHandle_t ble_mtu_sem;
static SemaphoreHandle_t ble_write_sem;
static SemaphoreHandle_t ble_read_sem;
static bool ble_initialized;
static bool ble_synced;
static bool worker_started;
static TaskHandle_t ble_worker_handle;
static QueueHandle_t ble_worker_job_queue;
static SemaphoreHandle_t ble_worker_job_mutex;
static SemaphoreHandle_t ble_worker_job_complete;
static esp_err_t ble_worker_job_result;
static volatile UBaseType_t ble_worker_stack_high_water_mark;
static volatile bool ble_worker_job_active;
static volatile bool ble_worker_job_yielded;
static TaskHandle_t volatile ble_worker_job_owner;
static volatile bool ble_scan_teardown_pending;
static hub_ble_operation_t *active_op;
static uint16_t cached_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint32_t cached_connection_epoch;
static uint32_t connection_epoch_counter;
static uint16_t cached_auth_handle;
static uint16_t cached_read_handle;
static uint16_t cached_write_handle;
static hub_ble_read_selector_proof_t cached_read_selector_proof;
static int cached_rssi;
static char cached_target_name[64];
static char cached_target_address[32];
static char cached_match_reason[32];
static hub_machine_selection_identity_t cached_selection_identity;
static uint64_t cached_ble_auth_fingerprint;
static hub_ble_notify_probe_state_t notify_probe_state;

static int hub_ble_gap_event(struct ble_gap_event *event, void *arg);
static esp_err_t validate_read_internal(hub_ble_read_result_t *result, bool realtime_only);
static bool cached_session_matches_operation(const hub_ble_operation_t *op);
static bool realtime_boilers_payload_is_valid(const char *boilers_raw);
static uint64_t ble_auth_fingerprint(const char *token);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void drain_sem(SemaphoreHandle_t sem)
{
    if (sem == NULL) {
        return;
    }
    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

static bool wait_sem(SemaphoreHandle_t sem, uint32_t timeout_ms)
{
    return sem != NULL && xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void copy_safe(char *dest, size_t dest_len, const char *source)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }
    dest[0] = '\0';
    if (source == NULL) {
        return;
    }
    snprintf(dest, dest_len, "%s", source);
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *dest, size_t dest_len)
{
    static const char hex[] = "0123456789abcdef";
    if (dest == NULL || dest_len == 0) {
        return;
    }
    dest[0] = '\0';
    if (bytes == NULL || len == 0) {
        return;
    }

    size_t out = 0;
    for (size_t index = 0; index < len && out + 2 < dest_len; index++) {
        dest[out++] = hex[(bytes[index] >> 4) & 0x0f];
        dest[out++] = hex[bytes[index] & 0x0f];
    }
    dest[out] = '\0';
}

static uint32_t fnv1a32(const uint8_t *bytes, size_t len)
{
    uint32_t hash = 2166136261u;
    if (bytes == NULL) {
        return hash;
    }
    for (size_t index = 0; index < len; index++) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

static void set_last_error(const char *message)
{
    copy_safe(ble_state.last_error, sizeof(ble_state.last_error), message);
}

static bool ble_diagnostic_should_yield(void)
{
    if (!ble_worker_job_active
        || xTaskGetCurrentTaskHandle() != ble_worker_job_owner
        || !hub_command_interactive_pending()) {
        return false;
    }
    ble_worker_job_yielded = true;
    set_last_error("BLE diagnostic yielded to an interactive control.");
    return true;
}

static bool ble_diagnostic_yielded_on_current_task(void)
{
    return ble_worker_job_active
        && xTaskGetCurrentTaskHandle() == ble_worker_job_owner
        && ble_worker_job_yielded;
}

static bool ble_diagnostic_delay(int delay_ms)
{
    int remaining_ms = delay_ms;
    while (remaining_ms > 0) {
        if (ble_diagnostic_should_yield()) {
            return false;
        }
        int slice_ms = remaining_ms > 100 ? 100 : remaining_ms;
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
        remaining_ms -= slice_ms;
    }
    return true;
}

static void set_status(const char *status)
{
    copy_safe(ble_state.status, sizeof(ble_state.status), status);
}

static bool equal_fold(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool starts_with_fold(const char *value, const char *prefix)
{
    if (value == NULL || prefix == NULL) {
        return false;
    }
    while (*prefix != '\0') {
        if (*value == '\0' || tolower((unsigned char)*value) != tolower((unsigned char)*prefix)) {
            return false;
        }
        value++;
        prefix++;
    }
    return true;
}

static bool contains_fold(const char *value, const char *needle)
{
    if (value == NULL || needle == NULL) {
        return false;
    }
    size_t value_len = strlen(value);
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > value_len) {
        return false;
    }
    for (size_t index = 0; index <= value_len - needle_len; index++) {
        if (strncasecmp(value + index, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_lamarzocco_name(const char *name)
{
    return starts_with_fold(name, "LINEAR_")
        || starts_with_fold(name, "MICRA")
        || starts_with_fold(name, "MINI")
        || starts_with_fold(name, "GS3");
}

static void format_ble_addr(const ble_addr_t *addr, char *dest, size_t dest_len)
{
    snprintf(
        dest,
        dest_len,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        addr->val[5],
        addr->val[4],
        addr->val[3],
        addr->val[2],
        addr->val[1],
        addr->val[0]
    );
}

static bool parse_configured_ble_address(const char *source, ble_addr_t *addr)
{
    if (source == NULL || addr == NULL) {
        return false;
    }

    unsigned int byte0 = 0;
    unsigned int byte1 = 0;
    unsigned int byte2 = 0;
    unsigned int byte3 = 0;
    unsigned int byte4 = 0;
    unsigned int byte5 = 0;
    int consumed = 0;
    int parsed = sscanf(
        source,
        "%2x:%2x:%2x:%2x:%2x:%2x%n",
        &byte0,
        &byte1,
        &byte2,
        &byte3,
        &byte4,
        &byte5,
        &consumed
    );
    if (parsed != 6 || source[consumed] != '\0'
        || byte0 > 0xff || byte1 > 0xff || byte2 > 0xff
        || byte3 > 0xff || byte4 > 0xff || byte5 > 0xff) {
        return false;
    }

    addr->type = BLE_ADDR_PUBLIC;
    addr->val[0] = (uint8_t)byte5;
    addr->val[1] = (uint8_t)byte4;
    addr->val[2] = (uint8_t)byte3;
    addr->val[3] = (uint8_t)byte2;
    addr->val[4] = (uint8_t)byte1;
    addr->val[5] = (uint8_t)byte0;
    return true;
}

static bool use_configured_address_direct(hub_ble_operation_t *op)
{
    if (op->selection.ble_device_address[0] == '\0'
        || !parse_configured_ble_address(op->selection.ble_device_address, &op->target_addr)) {
        return false;
    }

    op->target_found = true;
    op->rssi = 0;
    copy_safe(op->target_name, sizeof(op->target_name), op->selection.ble_device_name[0] != '\0'
        ? op->selection.ble_device_name
        : "configured BLE address");
    copy_safe(op->target_address, sizeof(op->target_address), op->selection.ble_device_address);
    copy_safe(op->match_reason, sizeof(op->match_reason), "configured_address_direct");
    ble_state.last_seen_ms = now_ms();
    return true;
}

static bool use_configured_address_random_fallback(hub_ble_operation_t *op)
{
    if (op->selection.ble_device_address[0] == '\0'
        || !parse_configured_ble_address(op->selection.ble_device_address, &op->target_addr)) {
        return false;
    }

    op->target_addr.type = BLE_ADDR_RANDOM;
    op->target_found = true;
    op->rssi = 0;
    copy_safe(op->target_name, sizeof(op->target_name), op->selection.ble_device_name[0] != '\0'
        ? op->selection.ble_device_name
        : "configured BLE random address");
    copy_safe(op->target_address, sizeof(op->target_address), op->selection.ble_device_address);
    copy_safe(op->match_reason, sizeof(op->match_reason), "configured_address_random_fallback");
    ble_state.last_seen_ms = now_ms();
    return true;
}

static void persist_discovered_ble_address(hub_ble_operation_t *op)
{
    if (op == NULL || op->target_address[0] == '\0') {
        return;
    }
    if (op->selection.ble_device_address[0] != '\0'
        && equal_fold(op->selection.ble_device_address, op->target_address)) {
        return;
    }

    esp_err_t err = hub_config_save_ble_device_address_for_serial(
        op->selection.serial_number,
        op->target_address
    );
    if (err == ESP_OK) {
        copy_safe(op->selection.ble_device_address, sizeof(op->selection.ble_device_address), op->target_address);
        ESP_LOGI(TAG, "Persisted BLE device address %s after successful BLE exchange", op->target_address);
    } else {
        ESP_LOGW(TAG, "Failed to persist BLE device address %s: %s", op->target_address, esp_err_to_name(err));
    }
}

static void json_escape_copy(char *dest, size_t dest_len, const char *source)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }
    dest[0] = '\0';
    if (source == NULL) {
        return;
    }

    size_t write = 0;
    for (const char *read = source; *read != '\0' && write + 1 < dest_len; read++) {
        char value = *read;
        if (value == '"' || value == '\\') {
            value = '\'';
        } else if ((unsigned char)value < 0x20) {
            value = ' ';
        }
        dest[write++] = value;
    }
    dest[write] = '\0';
}

static const char *bool_text(bool value)
{
    return value ? "true" : "false";
}

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}

static bool json_number(cJSON *object, const char *key, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static bool json_number_any(cJSON *object, const char *a, const char *b, const char *c, double *value)
{
    return json_number(object, a, value) || json_number(object, b, value) || json_number(object, c, value);
}

static bool json_bool_any(cJSON *object, const char *a, const char *b, const char *c, bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, a);
    if (!cJSON_IsBool(item)) {
        item = cJSON_GetObjectItemCaseSensitive(object, b);
    }
    if (!cJSON_IsBool(item)) {
        item = cJSON_GetObjectItemCaseSensitive(object, c);
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *value = cJSON_IsTrue(item);
    return true;
}

static void append_boiler_json(char *dest, size_t dest_len, size_t *offset, cJSON *boiler, bool first)
{
    char id[48] = {0};
    char current_text[32] = "null";
    char target_text[32] = "null";
    char enabled_text[8] = "null";
    json_escape_copy(id, sizeof(id), json_string(boiler, "id"));

    double current = 0;
    double target = 0;
    bool enabled = false;
    bool has_current = json_number_any(boiler, "currentTemperature", "current_temperature", "current", &current);
    bool has_target = json_number_any(boiler, "targetTemperature", "target_temperature", "target", &target);
    bool has_enabled = json_bool_any(boiler, "enabled", "isEnabled", "is_enabled", &enabled);

    if (has_current) {
        snprintf(current_text, sizeof(current_text), "%.1f", current);
    }
    if (has_target) {
        snprintf(target_text, sizeof(target_text), "%.1f", target);
    }
    if (has_enabled) {
        snprintf(enabled_text, sizeof(enabled_text), "%s", enabled ? "true" : "false");
    }

    *offset += snprintf(
        dest + *offset,
        dest_len - *offset,
        "%s{\"id\":\"%s\",\"currentTemperature\":%s,\"targetTemperature\":%s,\"enabled\":%s}",
        first ? "" : ",",
        id[0] != '\0' ? id : "unknown",
        current_text,
        target_text,
        enabled_text
    );
}

static void build_boilers_json(const char *raw, char *dest, size_t dest_len)
{
    snprintf(dest, dest_len, "[]");
    cJSON *root = cJSON_Parse(raw);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    size_t offset = 0;
    offset += snprintf(dest + offset, dest_len - offset, "[");
    int index = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        append_boiler_json(dest, dest_len, &offset, item, index == 0);
        index++;
        if (offset >= dest_len) {
            break;
        }
    }
    snprintf(dest + offset, dest_len - offset, "]");
    cJSON_Delete(root);
}

static void parse_json_string_value(const char *raw, char *dest, size_t dest_len)
{
    dest[0] = '\0';
    cJSON *root = cJSON_Parse(raw);
    if (cJSON_IsString(root) && root->valuestring != NULL) {
        json_escape_copy(dest, dest_len, root->valuestring);
    }
    cJSON_Delete(root);
}

static bool parse_json_bool_value(const char *raw, bool *value)
{
    cJSON *root = cJSON_Parse(raw);
    bool ok = cJSON_IsBool(root);
    if (ok) {
        *value = cJSON_IsTrue(root);
    }
    cJSON_Delete(root);
    return ok;
}

static void build_smart_standby_json(const char *raw, char *dest, size_t dest_len)
{
    char enabled_text[8] = "null";
    char minutes_text[16] = "null";
    char mode_text[64] = "null";
    snprintf(dest, dest_len, "null");
    cJSON *root = cJSON_Parse(raw);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *minutes_item = cJSON_GetObjectItemCaseSensitive(root, "minutes");
    const char *mode = json_string(root, "mode");
    char mode_safe[48] = {0};
    json_escape_copy(mode_safe, sizeof(mode_safe), mode);
    if (cJSON_IsBool(enabled_item)) {
        snprintf(enabled_text, sizeof(enabled_text), "%s", cJSON_IsTrue(enabled_item) ? "true" : "false");
    }
    if (cJSON_IsNumber(minutes_item)) {
        snprintf(minutes_text, sizeof(minutes_text), "%d", minutes_item->valueint);
    }
    if (mode_safe[0] != '\0') {
        snprintf(mode_text, sizeof(mode_text), "\"%s\"", mode_safe);
    }
    snprintf(dest, dest_len, "{\"enabled\":%s,\"minutes\":%s,\"mode\":%s}", enabled_text, minutes_text, mode_text);
    cJSON_Delete(root);
}

static bool ble_cache_identity_matches_selection(void)
{
    hub_machine_selection_identity_t identity = {0};
    return ble_state.has_cached_state
        && ble_state.selection_serial[0] != '\0'
        && hub_config_get_machine_selection_identity(&identity) == ESP_OK
        && ble_state.selection_generation == identity.generation
        && strcmp(ble_state.selection_serial, identity.serial_number) == 0;
}

static bool ble_cache_is_fresh(void)
{
    int64_t current_ms = now_ms();
    return ble_cache_identity_matches_selection()
        && ble_state.cached_at_ms > 0
        && current_ms >= ble_state.cached_at_ms
        && current_ms - ble_state.cached_at_ms <= HUB_BLE_STATE_CACHE_TTL_MS;
}

static void reset_read_selector_proof(void)
{
    memset(&cached_read_selector_proof, 0, sizeof(cached_read_selector_proof));
    cached_read_selector_proof.conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static uint32_t next_connection_epoch(void)
{
    connection_epoch_counter++;
    if (connection_epoch_counter == 0) {
        connection_epoch_counter = 1;
    }
    return connection_epoch_counter;
}

static bool read_selector_proof_matches_operation(
    const hub_ble_read_selector_proof_t *proof,
    const hub_ble_operation_t *op
)
{
    return proof != NULL
        && op != NULL
        && proof->boilers
        && op->conn_handle != BLE_HS_CONN_HANDLE_NONE
        && proof->conn_handle == op->conn_handle
        && op->connection_epoch != 0
        && proof->connection_epoch == op->connection_epoch
        && proof->read_handle != 0
        && proof->read_handle == op->read_handle
        && proof->write_handle != 0
        && proof->write_handle != proof->read_handle
        && op->write_handle == proof->write_handle
        && proof->selection_generation == op->selection_identity.generation
        && proof->selection_serial[0] != '\0'
        && strcmp(
            proof->selection_serial,
            op->selection_identity.serial_number
        ) == 0
        && proof->auth_fingerprint
            == ble_auth_fingerprint(op->selection.ble_auth_token)
        && hub_config_machine_selection_matches(&op->selection_identity);
}

static bool read_selector_proof_belongs_to_operation(const hub_ble_operation_t *op)
{
    return read_selector_proof_matches_operation(&cached_read_selector_proof, op);
}

static bool prepare_boilers_selector_proof_candidate(
    const hub_ble_operation_t *op,
    uint32_t observation_sequence,
    hub_ble_read_selector_proof_t *candidate
)
{
    if (candidate == NULL) {
        return false;
    }
    memset(candidate, 0, sizeof(*candidate));
    candidate->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (op == NULL
        || op->conn_handle == BLE_HS_CONN_HANDLE_NONE
        || op->connection_epoch == 0
        || op->read_handle == 0
        || op->write_handle == 0
        || op->write_handle == op->read_handle
        || observation_sequence == 0
        || op->selection_identity.serial_number[0] == '\0'
        || !hub_config_machine_selection_matches(&op->selection_identity)) {
        return false;
    }
    candidate->boilers = true;
    candidate->conn_handle = op->conn_handle;
    candidate->connection_epoch = op->connection_epoch;
    candidate->read_handle = op->read_handle;
    candidate->write_handle = op->write_handle;
    candidate->selection_generation = op->selection_identity.generation;
    candidate->observation_sequence = observation_sequence;
    candidate->auth_fingerprint = ble_auth_fingerprint(op->selection.ble_auth_token);
    copy_safe(
        candidate->selection_serial,
        sizeof(candidate->selection_serial),
        op->selection_identity.serial_number
    );
    return true;
}

static bool commit_boilers_selector_proof_candidate(
    const hub_ble_operation_t *op,
    const hub_ble_read_selector_proof_t *candidate,
    const char *required_cache_origin
)
{
    if (required_cache_origin == NULL
        || strcmp(ble_state.cache_origin, required_cache_origin) != 0
        || !ble_cache_is_fresh()
        || candidate == NULL
        || candidate->observation_sequence == 0
        || candidate->observation_sequence != ble_state.cache_sequence
        || !read_selector_proof_matches_operation(candidate, op)
        || !cached_session_matches_operation(op)
        || op->conn_handle != cached_conn_handle
        || op->connection_epoch != cached_connection_epoch
        || op->read_handle != cached_read_handle
        || op->write_handle == 0
        || op->write_handle == op->read_handle
        || op->write_handle != cached_write_handle
        || cached_write_handle == 0
        || cached_write_handle == cached_read_handle) {
        return false;
    }
    cached_read_selector_proof = *candidate;
    return true;
}

static void consume_read_selector_proof_before_setting_write(void)
{
    /* Fixed-size, lock-free while ble_op_mutex is owned by the caller. */
    cached_read_selector_proof.observation_sequence = 0;
}

static bool command_confirmation_direct_read_ready(const hub_ble_operation_t *op)
{
    return ble_cache_is_fresh()
        && read_selector_proof_belongs_to_operation(op)
        && cached_session_matches_operation(op)
        && op->conn_handle == cached_conn_handle
        && op->connection_epoch == cached_connection_epoch
        && op->read_handle == cached_read_handle
        && op->write_handle != 0
        && op->write_handle != op->read_handle
        && op->write_handle == cached_write_handle
        && cached_read_selector_proof.write_handle == cached_write_handle
        && cached_read_selector_proof.observation_sequence != 0
        && cached_read_selector_proof.observation_sequence == ble_state.cache_sequence;
}

static int worker_delay_after_recent_command_confirmation(void)
{
    if (!ble_cache_identity_matches_selection()
        || strcmp(ble_state.status, "cached") != 0
        || strcmp(ble_state.cache_origin, "commandConfirmation") != 0) {
        return 0;
    }

    int64_t current_ms = now_ms();
    if (ble_state.cached_at_ms <= 0 || current_ms < ble_state.cached_at_ms) {
        return 0;
    }
    int64_t age_ms = current_ms - ble_state.cached_at_ms;
    if (age_ms >= HUB_BLE_WORKER_POLL_MS) {
        return 0;
    }

    int remaining_ms = HUB_BLE_WORKER_POLL_MS - (int)age_ms;
    return remaining_ms < HUB_BLE_WORKER_PRIORITY_RECHECK_MS
        ? HUB_BLE_WORKER_PRIORITY_RECHECK_MS
        : remaining_ms;
}

static void record_cache_observation(const char *origin)
{
    ble_state.cache_sequence++;
    if (ble_state.cache_sequence == 0) {
        ble_state.cache_sequence = 1;
    }
    copy_safe(
        ble_state.cache_origin,
        sizeof(ble_state.cache_origin),
        origin != NULL ? origin : "unknown"
    );
}

static esp_err_t build_state_json(hub_ble_operation_t *op, const char *boilers_raw, const char *mode_raw, const char *tank_raw, const char *smart_raw)
{
    if (op == NULL
        || op->selection_identity.serial_number[0] == '\0'
        || strcmp(op->selection_identity.serial_number, op->selection.serial_number) != 0
        || !hub_config_machine_selection_matches(&op->selection_identity)) {
        return ESP_ERR_INVALID_STATE;
    }
    char boilers_json[768] = {0};
    char mode[48] = {0};
    char smart_json[192] = {0};
    bool tank = false;
    bool has_tank = false;

    build_boilers_json(boilers_raw, boilers_json, sizeof(boilers_json));
    parse_json_string_value(mode_raw, mode, sizeof(mode));
    has_tank = parse_json_bool_value(tank_raw, &tank);
    build_smart_standby_json(smart_raw, smart_json, sizeof(smart_json));

    char next_state_json[sizeof(ble_state.state_json)] = {0};
    snprintf(
        next_state_json,
        sizeof(next_state_json),
        "{"
        "\"connected\":true,"
        "\"machineSerial\":\"%s\","
        "\"deviceName\":\"%s\","
        "\"deviceAddress\":\"%s\","
        "\"rssi\":%d,"
        "\"machineMode\":%s%s%s,"
        "\"tankStatus\":%s,"
        "\"boilers\":%s,"
        "\"smartStandby\":%s,"
        "\"lastReadAt\":\"1970-01-01T00:00:00Z\","
        "\"source\":\"ble\""
        "}",
        op->selection.serial_number,
        op->target_name,
        op->target_address,
        op->rssi,
        mode[0] != '\0' ? "\"" : "null",
        mode[0] != '\0' ? mode : "",
        mode[0] != '\0' ? "\"" : "",
        has_tank ? (tank ? "true" : "false") : "null",
        boilers_json,
        smart_json
    );

    if (!hub_config_machine_selection_matches(&op->selection_identity)) {
        return ESP_ERR_INVALID_STATE;
    }

    copy_safe(ble_state.state_json, sizeof(ble_state.state_json), next_state_json);
    ble_state.connected = true;
    ble_state.has_cached_state = true;
    ble_state.reads_validated = true;
    ble_state.rssi = op->rssi;
    ble_state.last_read_ms = now_ms();
    ble_state.cached_at_ms = ble_state.last_read_ms;
    ble_state.selection_generation = op->selection_identity.generation;
    copy_safe(
        ble_state.selection_serial,
        sizeof(ble_state.selection_serial),
        op->selection_identity.serial_number
    );
    copy_safe(ble_state.device_name, sizeof(ble_state.device_name), op->target_name);
    copy_safe(ble_state.device_address, sizeof(ble_state.device_address), op->target_address);
    copy_safe(ble_state.match_reason, sizeof(ble_state.match_reason), op->match_reason);
    copy_safe(ble_state.machine_mode, sizeof(ble_state.machine_mode), mode);
    record_cache_observation("fullRead");
    return ESP_OK;
}

static bool replace_json_item(cJSON *root, const char *key, cJSON *item)
{
    if (root == NULL || key == NULL || item == NULL) {
        cJSON_Delete(item);
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    if (!cJSON_AddItemToObject(root, key, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static esp_err_t update_realtime_state_json(
    hub_ble_operation_t *op,
    const char *boilers_raw,
    const char *cache_origin
)
{
    if (op == NULL
        || op->selection_identity.serial_number[0] == '\0'
        || strcmp(op->selection_identity.serial_number, op->selection.serial_number) != 0
        || !hub_config_machine_selection_matches(&op->selection_identity)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!realtime_boilers_payload_is_valid(boilers_raw)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char boilers_json[768] = {0};
    build_boilers_json(boilers_raw, boilers_json, sizeof(boilers_json));
    cJSON *boilers = cJSON_Parse(boilers_json);
    cJSON *root = ble_cache_identity_matches_selection()
        ? cJSON_Parse(ble_state.state_json)
        : cJSON_CreateObject();
    if (root == NULL || !cJSON_IsArray(boilers)) {
        cJSON_Delete(root);
        cJSON_Delete(boilers);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool ok = replace_json_item(root, "connected", cJSON_CreateBool(true))
        && replace_json_item(root, "machineSerial", cJSON_CreateString(op->selection.serial_number))
        && replace_json_item(root, "deviceName", cJSON_CreateString(op->target_name))
        && replace_json_item(root, "deviceAddress", cJSON_CreateString(op->target_address))
        && replace_json_item(root, "rssi", cJSON_CreateNumber(op->rssi));
    if (ok) {
        ok = replace_json_item(root, "boilers", boilers);
        if (ok) {
            boilers = NULL;
        }
    }
    if (ok) {
        ok = replace_json_item(root, "lastReadAt", cJSON_CreateString("1970-01-01T00:00:00Z"))
            && replace_json_item(root, "source", cJSON_CreateString("ble"));
    }
    if (ok && cJSON_GetObjectItemCaseSensitive(root, "machineMode") == NULL) {
        ok = cJSON_AddNullToObject(root, "machineMode") != NULL;
    }
    if (ok && cJSON_GetObjectItemCaseSensitive(root, "tankStatus") == NULL) {
        ok = cJSON_AddNullToObject(root, "tankStatus") != NULL;
    }
    if (ok && cJSON_GetObjectItemCaseSensitive(root, "smartStandby") == NULL) {
        ok = cJSON_AddNullToObject(root, "smartStandby") != NULL;
    }
    char *printed = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    cJSON_Delete(boilers);
    if (printed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!hub_config_machine_selection_matches(&op->selection_identity)) {
        cJSON_free(printed);
        return ESP_ERR_INVALID_STATE;
    }

    copy_safe(ble_state.state_json, sizeof(ble_state.state_json), printed);
    cJSON_free(printed);
    ble_state.connected = true;
    ble_state.has_cached_state = true;
    ble_state.reads_validated = true;
    ble_state.rssi = op->rssi;
    ble_state.last_read_ms = now_ms();
    ble_state.cached_at_ms = ble_state.last_read_ms;
    ble_state.selection_generation = op->selection_identity.generation;
    copy_safe(
        ble_state.selection_serial,
        sizeof(ble_state.selection_serial),
        op->selection_identity.serial_number
    );
    copy_safe(ble_state.device_name, sizeof(ble_state.device_name), op->target_name);
    copy_safe(ble_state.device_address, sizeof(ble_state.device_address), op->target_address);
    copy_safe(ble_state.match_reason, sizeof(ble_state.match_reason), op->match_reason);
    record_cache_observation(cache_origin);
    return ESP_OK;
}

static bool extract_noop_targets(const char *state_json, hub_ble_noop_targets_t *targets)
{
    if (state_json == NULL || targets == NULL) {
        return false;
    }
    memset(targets, 0, sizeof(*targets));

    cJSON *root = cJSON_Parse(state_json);
    cJSON *boilers = cJSON_GetObjectItemCaseSensitive(root, "boilers");
    if (!cJSON_IsArray(boilers)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *boiler = NULL;
    cJSON_ArrayForEach(boiler, boilers) {
        if (!cJSON_IsObject(boiler)) {
            continue;
        }
        const char *id = json_string(boiler, "id");
        cJSON *target = cJSON_GetObjectItemCaseSensitive(boiler, "targetTemperature");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(boiler, "enabled");

        if (strcmp(id, "CoffeeBoiler1") == 0 && cJSON_IsNumber(target)) {
            targets->coffee_target = target->valuedouble;
            targets->has_coffee_target = true;
        } else if (strcmp(id, "SteamBoiler") == 0) {
            if (cJSON_IsNumber(target)) {
                targets->steam_target = target->valuedouble;
                targets->has_steam_target = true;
            }
            if (cJSON_IsBool(enabled)) {
                targets->steam_enabled = cJSON_IsTrue(enabled);
                targets->has_steam_enabled = true;
            }
        }
    }

    cJSON_Delete(root);
    return targets->has_coffee_target && targets->has_steam_target && targets->has_steam_enabled;
}

static bool same_temperature(double a, double b)
{
    double delta = a - b;
    if (delta < 0) {
        delta = -delta;
    }
    return delta < 0.05;
}

static hub_ble_confirmation_target_t confirmation_target_from_command(const char *command_json)
{
    hub_ble_confirmation_target_t target = {0};
    cJSON *root = cJSON_Parse(command_json);
    const char *name = json_string(root, "name");
    cJSON *parameter = cJSON_GetObjectItemCaseSensitive(root, "parameter");
    const char *identifier = json_string(parameter, "identifier");
    if (strcmp(name, "SettingBoilerTarget") == 0) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(parameter, "value");
        if (cJSON_IsNumber(value) && strcmp(identifier, "CoffeeBoiler1") == 0) {
            target.kind = HUB_BLE_CONFIRM_COFFEE_TARGET;
            target.target = value->valuedouble;
        } else if (cJSON_IsNumber(value) && strcmp(identifier, "SteamBoiler") == 0) {
            target.kind = HUB_BLE_CONFIRM_STEAM_TARGET;
            target.target = value->valuedouble;
        }
    } else if (strcmp(name, "SettingBoilerEnable") == 0
        && strcmp(identifier, "SteamBoiler") == 0) {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(parameter, "state");
        if (cJSON_IsBool(state)) {
            target.kind = HUB_BLE_CONFIRM_STEAM_ENABLED;
            target.enabled = cJSON_IsTrue(state);
        }
    }
    cJSON_Delete(root);
    return target;
}

static bool confirmation_target_matches(
    const hub_ble_confirmation_target_t *target,
    const char *state_json
)
{
    if (target == NULL || target->kind == HUB_BLE_CONFIRM_NONE) {
        return true;
    }
    hub_ble_noop_targets_t observed = {0};
    (void)extract_noop_targets(state_json, &observed);
    switch (target->kind) {
    case HUB_BLE_CONFIRM_COFFEE_TARGET:
        return observed.has_coffee_target && same_temperature(observed.coffee_target, target->target);
    case HUB_BLE_CONFIRM_STEAM_TARGET:
        return observed.has_steam_target && same_temperature(observed.steam_target, target->target);
    case HUB_BLE_CONFIRM_STEAM_ENABLED:
        return observed.has_steam_enabled && observed.steam_enabled == target->enabled;
    case HUB_BLE_CONFIRM_NONE:
        return true;
    }
    return false;
}

static bool realtime_boilers_payload_is_valid(const char *boilers_raw)
{
    if (boilers_raw == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(boilers_raw);
    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) <= 0) {
        cJSON_Delete(root);
        return false;
    }

    bool coffee_valid = false;
    bool steam_valid = false;
    cJSON *boiler = NULL;
    cJSON_ArrayForEach(boiler, root) {
        if (!cJSON_IsObject(boiler)) {
            continue;
        }
        double target_temperature = 0;
        bool enabled = false;
        bool shape_valid = json_number_any(
            boiler,
            "targetTemperature",
            "target_temperature",
            "target",
            &target_temperature
        ) && json_bool_any(boiler, "enabled", "isEnabled", "is_enabled", &enabled);
        if (!shape_valid) {
            continue;
        }
        const char *id = json_string(boiler, "id");
        if (strcmp(id, "CoffeeBoiler1") == 0) {
            coffee_valid = true;
        } else if (strcmp(id, "SteamBoiler") == 0) {
            steam_valid = true;
        }
    }

    cJSON_Delete(root);
    return coffee_valid && steam_valid;
}

static bool confirmation_boilers_payload_is_valid(
    const char *boilers_raw,
    const hub_ble_confirmation_target_t *target
)
{
    return target != NULL
        && target->kind != HUB_BLE_CONFIRM_NONE
        && realtime_boilers_payload_is_valid(boilers_raw);
}

static bool load_selection(hub_machine_selection_t *selection)
{
    if (hub_config_get_machine_selection(selection) != ESP_OK) {
        return false;
    }
    return selection->ble_auth_token[0] != '\0' && selection->serial_number[0] != '\0';
}

static void parse_adv_name_data(const uint8_t *data, uint8_t data_len, char *name, size_t name_len)
{
    name[0] = '\0';
    struct ble_hs_adv_fields fields = {0};
    if (ble_hs_adv_parse_fields(&fields, data, data_len) != 0) {
        return;
    }
    if (fields.name != NULL && fields.name_len > 0) {
        size_t copy_len = fields.name_len < name_len - 1 ? fields.name_len : name_len - 1;
        memcpy(name, fields.name, copy_len);
        name[copy_len] = '\0';
    }
}

static void record_advertisement_sample(
    hub_ble_operation_t *op,
    const uint8_t *data,
    uint8_t data_len,
    const ble_addr_t *addr,
    int rssi
)
{
    if (op == NULL || data == NULL || addr == NULL) {
        return;
    }

    char name[64] = {0};
    char address[32] = {0};
    parse_adv_name_data(data, data_len, name, sizeof(name));
    format_ble_addr(addr, address, sizeof(address));

    op->scan_seen_count++;
    if (name[0] != '\0') {
        op->scan_named_count++;
        copy_safe(op->last_scan_name, sizeof(op->last_scan_name), name);
        copy_safe(op->last_scan_address, sizeof(op->last_scan_address), address);
    } else if (op->last_scan_address[0] == '\0') {
        copy_safe(op->last_scan_name, sizeof(op->last_scan_name), "unnamed");
        copy_safe(op->last_scan_address, sizeof(op->last_scan_address), address);
    }

    if (name[0] != '\0' || op->scan_seen_count <= 3) {
        ESP_LOGI(TAG,
                 "BLE scan sample count=%d name=%s address=%s rssi=%d type=%d",
                 op->scan_seen_count,
                 name[0] != '\0' ? name : "unnamed",
                 address,
                 rssi,
                 addr->type);
    }
}

static void record_disc_sample(hub_ble_operation_t *op, const struct ble_gap_disc_desc *disc)
{
    record_advertisement_sample(op, disc->data, disc->length_data, &disc->addr, disc->rssi);
}

#if MYNEWT_VAL(BLE_EXT_ADV)
static void record_ext_disc_sample(hub_ble_operation_t *op, const struct ble_gap_ext_disc_desc *disc)
{
    record_advertisement_sample(op, disc->data, disc->length_data, &disc->addr, disc->rssi);
}
#endif

static bool match_advertisement(
    hub_ble_operation_t *op,
    const uint8_t *data,
    uint8_t data_len,
    const ble_addr_t *addr,
    int rssi
)
{
    char name[64] = {0};
    char address[32] = {0};
    parse_adv_name_data(data, data_len, name, sizeof(name));
    format_ble_addr(addr, address, sizeof(address));

    const char *reason = NULL;
    if (op->selection.ble_device_name[0] != '\0' && equal_fold(name, op->selection.ble_device_name)) {
        reason = "configured_name";
    } else if (op->selection.ble_device_address[0] != '\0' && equal_fold(address, op->selection.ble_device_address)) {
        reason = "configured_address";
    } else if (op->selection.serial_number[0] != '\0' && contains_fold(name, op->selection.serial_number)) {
        reason = "configured_serial";
    } else if (is_lamarzocco_name(name)) {
        reason = "lamarzocco_name";
    }

    if (reason == NULL) {
        return false;
    }

    op->target_addr = *addr;
    op->target_found = true;
    op->rssi = rssi;
    copy_safe(op->target_name, sizeof(op->target_name), name[0] != '\0' ? name : "unknown");
    copy_safe(op->target_address, sizeof(op->target_address), address);
    copy_safe(op->match_reason, sizeof(op->match_reason), reason);
    return true;
}

static bool match_disc(hub_ble_operation_t *op, const struct ble_gap_disc_desc *disc)
{
    return match_advertisement(op, disc->data, disc->length_data, &disc->addr, disc->rssi);
}

#if MYNEWT_VAL(BLE_EXT_ADV)
static bool match_ext_disc(hub_ble_operation_t *op, const struct ble_gap_ext_disc_desc *disc)
{
    return match_advertisement(op, disc->data, disc->length_data, &disc->addr, disc->rssi);
}
#endif

static int discover_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    hub_ble_operation_t *op = arg;
    if (error->status == 0 && chr != NULL) {
        if (op->discover_uuid != NULL && ble_uuid_cmp(&chr->uuid.u, op->discover_uuid) == 0) {
            *op->discover_handle = chr->val_handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        op->rc = 0;
        xSemaphoreGive(ble_discover_sem);
        return 0;
    }

    op->rc = error->status;
    xSemaphoreGive(ble_discover_sem);
    return error->status;
}

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    hub_ble_operation_t *op = arg;
    (void)conn_handle;
    op->rc = error != NULL ? error->status : BLE_HS_EAPP;
    if (op->rc == 0) {
        ESP_LOGI(TAG, "BLE MTU ready: %u", (unsigned)mtu);
    }
    xSemaphoreGive(ble_mtu_sem);
    return 0;
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    hub_ble_operation_t *op = arg;
    op->rc = error->status;
    xSemaphoreGive(ble_write_sem);
    return 0;
}

static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    hub_ble_operation_t *op = arg;
    int status = error != NULL ? error->status : BLE_HS_EAPP;
    (void)conn_handle;

    if (status == 0 && attr != NULL && attr->om != NULL) {
        size_t chunk_len = OS_MBUF_PKTLEN(attr->om);
        size_t remaining = sizeof(op->read_response) - 1 - op->read_response_len;
        if (chunk_len > remaining) {
            chunk_len = remaining;
        }
        if (chunk_len > 0 && os_mbuf_copydata(attr->om, 0, chunk_len, op->read_response + op->read_response_len) == 0) {
            op->read_response_len += chunk_len;
            op->read_response[op->read_response_len] = '\0';
        }
        op->rc = 0;
        xSemaphoreGive(ble_read_sem);
        return 0;
    }

    op->rc = status;
    xSemaphoreGive(ble_read_sem);
    return 0;
}

static void record_notify_probe_event(const struct ble_gap_event *event)
{
    if (event == NULL || !notify_probe_state.active) {
        return;
    }
    if (event->notify_rx.conn_handle != notify_probe_state.conn_handle) {
        return;
    }

    int length = event->notify_rx.om != NULL ? (int)OS_MBUF_PKTLEN(event->notify_rx.om) : 0;
    if (event->notify_rx.indication) {
        notify_probe_state.indication_count++;
    } else {
        notify_probe_state.notification_count++;
    }

    if (notify_probe_state.sample_count >= HUB_BLE_NOTIFY_PROBE_MAX_SAMPLES) {
        return;
    }

    hub_ble_notify_sample_t *sample = &notify_probe_state.samples[notify_probe_state.sample_count++];
    memset(sample, 0, sizeof(*sample));
    sample->indication = event->notify_rx.indication;
    sample->attr_handle = event->notify_rx.attr_handle;
    sample->elapsed_ms = (int)(now_ms() - notify_probe_state.started_ms);
    sample->length = length;

    uint8_t bytes[HUB_BLE_NOTIFY_PROBE_SAMPLE_HEX_CHARS / 2] = {0};
    size_t copy_len = length > (int)sizeof(bytes) ? sizeof(bytes) : (size_t)length;
    sample->truncated = length > (int)copy_len;
    if (copy_len > 0 && event->notify_rx.om != NULL
        && os_mbuf_copydata(event->notify_rx.om, 0, copy_len, bytes) == 0) {
        bytes_to_hex(bytes, copy_len, sample->hex, sizeof(sample->hex));
    }
}

static int discover_all_services_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg)
{
    hub_ble_gatt_probe_t *probe = arg;
    (void)conn_handle;
    if (probe == NULL || error == NULL) {
        return BLE_HS_EAPP;
    }

    if (error->status == BLE_HS_EDONE) {
        probe->rc = 0;
        xSemaphoreGive(ble_discover_sem);
        return 0;
    }
    if (error->status != 0) {
        probe->rc = error->status;
        xSemaphoreGive(ble_discover_sem);
        return error->status;
    }
    if (service == NULL) {
        return 0;
    }

    if (probe->service_count >= HUB_BLE_GATT_PROBE_MAX_SERVICES) {
        probe->services_truncated = true;
        return 0;
    }

    hub_ble_gatt_service_t *entry = &probe->services[probe->service_count++];
    memset(entry, 0, sizeof(*entry));
    entry->start_handle = service->start_handle;
    entry->end_handle = service->end_handle;
    ble_uuid_to_str(&service->uuid.u, entry->uuid);
    return 0;
}

static int discover_all_characteristics_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    hub_ble_gatt_probe_t *probe = arg;
    (void)conn_handle;
    if (probe == NULL || error == NULL) {
        return BLE_HS_EAPP;
    }

    if (error->status == BLE_HS_EDONE) {
        probe->rc = 0;
        xSemaphoreGive(ble_discover_sem);
        return 0;
    }
    if (error->status != 0) {
        probe->rc = error->status;
        xSemaphoreGive(ble_discover_sem);
        return error->status;
    }
    if (chr == NULL) {
        return 0;
    }

    if (probe->characteristic_count >= HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS) {
        probe->characteristics_truncated = true;
        return 0;
    }

    hub_ble_gatt_characteristic_t *entry = &probe->characteristics[probe->characteristic_count++];
    memset(entry, 0, sizeof(*entry));
    entry->def_handle = chr->def_handle;
    entry->val_handle = chr->val_handle;
    entry->properties = chr->properties;
    ble_uuid_to_str(&chr->uuid.u, entry->uuid);
    return 0;
}

static esp_err_t discover_all_gatt_for_probe(hub_ble_operation_t *op, hub_ble_gatt_probe_t *probe)
{
    if (op == NULL || probe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(probe, 0, sizeof(*probe));

    probe->rc = 0;
    drain_sem(ble_discover_sem);
    int rc = ble_gattc_disc_all_svcs(op->conn_handle, discover_all_services_cb, probe);
    if (rc != 0) {
        return ESP_FAIL;
    }
    if (!wait_sem(ble_discover_sem, HUB_BLE_DISCOVERY_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (probe->rc != 0) {
        return ESP_FAIL;
    }

    int service_count = probe->service_count;
    for (int index = 0; index < service_count; index++) {
        probe->rc = 0;
        drain_sem(ble_discover_sem);
        rc = ble_gattc_disc_all_chrs(
            op->conn_handle,
            probe->services[index].start_handle,
            probe->services[index].end_handle,
            discover_all_characteristics_cb,
            probe
        );
        if (rc != 0) {
            return ESP_FAIL;
        }
        if (!wait_sem(ble_discover_sem, HUB_BLE_DISCOVERY_TIMEOUT_MS)) {
            return ESP_ERR_TIMEOUT;
        }
        if (probe->rc != 0) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t wait_status(SemaphoreHandle_t sem, uint32_t timeout_ms, int *rc, const char *timeout_message)
{
    if (!wait_sem(sem, timeout_ms)) {
        set_last_error(timeout_message);
        return ESP_ERR_TIMEOUT;
    }
    if (*rc != 0) {
        char message[128] = {0};
        snprintf(message, sizeof(message), "%s: rc=%d", timeout_message, *rc);
        set_last_error(message);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t discover_handle(hub_ble_operation_t *op, const ble_uuid_t *uuid, uint16_t *handle)
{
    *handle = 0;
    op->discover_uuid = uuid;
    op->discover_handle = handle;
    op->rc = 0;
    drain_sem(ble_discover_sem);
    int rc = ble_gattc_disc_chrs_by_uuid(op->conn_handle, 1, 0xffff, uuid, discover_chr_cb, op);
    if (rc != 0) {
        return ESP_FAIL;
    }
    esp_err_t err = wait_status(ble_discover_sem, HUB_BLE_DISCOVERY_TIMEOUT_MS, &op->rc, "BLE characteristic discovery timeout");
    if (err != ESP_OK) {
        return err;
    }
    if (*handle == 0) {
        set_last_error("BLE required characteristic not found.");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t exchange_mtu(hub_ble_operation_t *op)
{
    op->rc = 0;
    drain_sem(ble_mtu_sem);
    int rc = ble_gattc_exchange_mtu(op->conn_handle, mtu_cb, op);
    if (rc != 0) {
        set_last_error("BLE MTU exchange could not start.");
        return ESP_FAIL;
    }
    return wait_status(ble_mtu_sem, 5000, &op->rc, "BLE MTU exchange timeout");
}

static void begin_read_selector_write(const hub_ble_operation_t *op, uint16_t handle)
{
    if (op != NULL && handle == op->read_handle) {
        reset_read_selector_proof();
    }
}

static esp_err_t write_gatt(hub_ble_operation_t *op, uint16_t handle, const void *data, size_t len, const char *label)
{
    op->rc = 0;
    drain_sem(ble_write_sem);
    begin_read_selector_write(op, handle);
    int rc = ble_gattc_write_flat(op->conn_handle, handle, data, (uint16_t)len, write_cb, op);
    if (rc != 0) {
        return ESP_FAIL;
    }
    char timeout[96] = {0};
    snprintf(timeout, sizeof(timeout), "BLE write timeout at %s", label);
    return wait_status(ble_write_sem, HUB_BLE_WRITE_TIMEOUT_MS, &op->rc, timeout);
}

static esp_err_t write_gatt_with_timeout(
    hub_ble_operation_t *op,
    uint16_t handle,
    const void *data,
    size_t len,
    const char *label,
    uint32_t timeout_ms
)
{
    op->rc = 0;
    drain_sem(ble_write_sem);
    begin_read_selector_write(op, handle);
    int rc = ble_gattc_write_flat(op->conn_handle, handle, data, (uint16_t)len, write_cb, op);
    if (rc != 0) {
        return ESP_FAIL;
    }
    char timeout[96] = {0};
    snprintf(timeout, sizeof(timeout), "BLE write timeout at %s", label);
    return wait_status(ble_write_sem, timeout_ms, &op->rc, timeout);
}

static esp_err_t read_gatt_once_with_timeout(
    hub_ble_operation_t *op,
    uint16_t handle,
    char *dest,
    size_t dest_len,
    const char *label,
    uint32_t timeout_ms
)
{
    op->rc = 0;
    op->read_response[0] = '\0';
    op->read_response_len = 0;
    drain_sem(ble_read_sem);
    int rc = ble_gattc_read(op->conn_handle, handle, read_cb, op);
    if (rc != 0) {
        return ESP_FAIL;
    }
    char timeout[96] = {0};
    snprintf(timeout, sizeof(timeout), "BLE read timeout at %s", label);
    esp_err_t err = wait_status(ble_read_sem, timeout_ms, &op->rc, timeout);
    if (err != ESP_OK) {
        return err;
    }
    if (op->read_response[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_safe(dest, dest_len, op->read_response);
    return ESP_OK;
}

static esp_err_t read_gatt_raw_once_with_timeout(
    hub_ble_operation_t *op,
    uint16_t handle,
    const char *label,
    uint32_t timeout_ms
)
{
    op->rc = 0;
    op->read_response[0] = '\0';
    op->read_response_len = 0;
    drain_sem(ble_read_sem);
    int rc = ble_gattc_read(op->conn_handle, handle, read_cb, op);
    if (rc != 0) {
        return ESP_FAIL;
    }
    char timeout[96] = {0};
    snprintf(timeout, sizeof(timeout), "BLE read timeout at %s", label);
    esp_err_t err = wait_status(ble_read_sem, timeout_ms, &op->rc, timeout);
    if (err != ESP_OK) {
        return err;
    }
    if (op->read_response_len == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t read_gatt_once(hub_ble_operation_t *op, uint16_t handle, char *dest, size_t dest_len, const char *label)
{
    return read_gatt_once_with_timeout(op, handle, dest, dest_len, label, HUB_BLE_READ_TIMEOUT_MS);
}

static esp_err_t read_gatt(hub_ble_operation_t *op, uint16_t handle, char *dest, size_t dest_len, const char *label)
{
    esp_err_t last_err = ESP_FAIL;
    for (int attempt = 0; attempt <= HUB_BLE_READ_RESPONSE_RETRY_COUNT; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(HUB_BLE_READ_RESPONSE_RETRY_DELAY_MS));
        }
        last_err = read_gatt_once(op, handle, dest, dest_len, label);
        if (last_err == ESP_OK) {
            return ESP_OK;
        }
        if (last_err != ESP_ERR_INVALID_RESPONSE) {
            return last_err;
        }
    }
    set_last_error("BLE read returned an empty response.");
    return last_err;
}

static esp_err_t read_setting(hub_ble_operation_t *op, const char *setting, char *dest, size_t dest_len)
{
    char command[32] = {0};
    snprintf(command, sizeof(command), "%s", setting);
    esp_err_t err = write_gatt(op, op->read_handle, command, strlen(command) + 1, setting);
    if (err != ESP_OK) {
        return err;
    }
    return read_gatt(op, op->read_handle, dest, dest_len, setting);
}

static uint32_t deadline_remaining_ms(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us < 1000) {
        return 0;
    }
    return (uint32_t)(remaining_us / 1000);
}

static esp_err_t read_setting_until_deadline(
    hub_ble_operation_t *op,
    const char *setting,
    char *dest,
    size_t dest_len,
    int64_t deadline_us
)
{
    char command[32] = {0};
    snprintf(command, sizeof(command), "%s", setting);
    uint32_t remaining_ms = deadline_remaining_ms(deadline_us);
    if (remaining_ms == 0) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = write_gatt_with_timeout(
        op,
        op->read_handle,
        command,
        strlen(command) + 1,
        setting,
        remaining_ms
    );
    if (err != ESP_OK) {
        return err;
    }
    remaining_ms = deadline_remaining_ms(deadline_us);
    if (remaining_ms == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return read_gatt_once_with_timeout(
        op,
        op->read_handle,
        dest,
        dest_len,
        setting,
        remaining_ms
    );
}

static esp_err_t read_setting_probe(hub_ble_operation_t *op, const char *setting, char *dest, size_t dest_len)
{
    char command[32] = {0};
    snprintf(command, sizeof(command), "%s", setting);
    esp_err_t err = write_gatt_with_timeout(
        op,
        op->read_handle,
        command,
        strlen(command) + 1,
        setting,
        HUB_BLE_FULL_READ_GATT_TIMEOUT_MS
    );
    if (err != ESP_OK) {
        return err;
    }
    return read_gatt_once_with_timeout(
        op,
        op->read_handle,
        dest,
        dest_len,
        setting,
        HUB_BLE_FULL_READ_GATT_TIMEOUT_MS
    );
}

static esp_err_t read_payload_probe(hub_ble_operation_t *op, const char *payload, char *dest, size_t dest_len)
{
    if (op == NULL || payload == NULL || payload[0] == '\0' || dest == NULL || dest_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t payload_len = strlen(payload);
    if (payload_len >= HUB_BLE_PROTOCOL_PROBE_PAYLOAD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = write_gatt_with_timeout(
        op,
        op->read_handle,
        payload,
        payload_len + 1,
        payload,
        HUB_BLE_FULL_READ_GATT_TIMEOUT_MS
    );
    if (err != ESP_OK) {
        return err;
    }
    return read_gatt_once_with_timeout(
        op,
        op->read_handle,
        dest,
        dest_len,
        payload,
        HUB_BLE_FULL_READ_GATT_TIMEOUT_MS
    );
}

static bool raw_echoes_payload(const char *payload, const char *raw)
{
    if (payload == NULL || raw == NULL) {
        return false;
    }
    if (strcmp(payload, raw) == 0) {
        return true;
    }

    char quoted[HUB_BLE_PROTOCOL_PROBE_PAYLOAD_MAX_BYTES + 4] = {0};
    snprintf(quoted, sizeof(quoted), "\"%s\"", payload);
    return strcmp(quoted, raw) == 0;
}

static esp_err_t read_all_state(hub_ble_operation_t *op)
{
    char boilers[768] = {0};
    char mode[128] = {0};
    char tank[128] = {0};
    char smart[384] = {0};

    set_status("reading");
    esp_err_t err = read_setting(op, "boilers", boilers, sizeof(boilers));
    if (err == ESP_OK && ble_diagnostic_should_yield()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err = read_setting(op, "machineMode", mode, sizeof(mode));
    }
    if (err == ESP_OK && ble_diagnostic_should_yield()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err = read_setting(op, "tankStatus", tank, sizeof(tank));
    }
    if (err == ESP_OK && ble_diagnostic_should_yield()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err = read_setting(op, "smartStandBy", smart, sizeof(smart));
    }
    if (err == ESP_OK) {
        err = build_state_json(op, boilers, mode, tank, smart);
    }
    if (err == ESP_OK) {
        hub_brew_timer_update_mode(ble_state.machine_mode, esp_timer_get_time());
        persist_discovered_ble_address(op);
    }
    return err;
}

static esp_err_t read_realtime_state(hub_ble_operation_t *op)
{
    char boilers[768] = {0};

    set_status("reading");
    esp_err_t err = read_setting(op, "boilers", boilers, sizeof(boilers));
    if (err == ESP_OK) {
        err = update_realtime_state_json(op, boilers, "workerRealtime");
    }
    if (err == ESP_OK) {
        persist_discovered_ble_address(op);
    }
    return err;
}

static esp_err_t read_realtime_state_until_deadline(
    hub_ble_operation_t *op,
    const hub_ble_confirmation_target_t *target,
    int64_t deadline_us
)
{
    char boilers[768] = {0};

    set_status("confirming");
    esp_err_t err = read_setting_until_deadline(
        op,
        "boilers",
        boilers,
        sizeof(boilers),
        deadline_us
    );
    if (err == ESP_OK && !confirmation_boilers_payload_is_valid(boilers, target)) {
        set_last_error("BLE selector confirmation returned an invalid boilers payload.");
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        err = update_realtime_state_json(op, boilers, "commandConfirmation");
    }
    if (err == ESP_OK) {
        persist_discovered_ble_address(op);
    }
    return err;
}

static esp_err_t read_realtime_state_direct_until_deadline(
    hub_ble_operation_t *op,
    const hub_ble_confirmation_target_t *target,
    int64_t deadline_us
)
{
    char boilers[768] = {0};
    uint32_t remaining_ms = deadline_remaining_ms(deadline_us);
    if (remaining_ms == 0) {
        return ESP_ERR_TIMEOUT;
    }

    set_status("confirming");
    esp_err_t err = read_gatt_once_with_timeout(
        op,
        op->read_handle,
        boilers,
        sizeof(boilers),
        "boilers-direct-confirmation",
        remaining_ms
    );
    if (err != ESP_OK) {
        return err;
    }
    if (!confirmation_boilers_payload_is_valid(boilers, target)) {
        set_last_error("BLE direct confirmation returned an invalid boilers payload.");
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = update_realtime_state_json(op, boilers, "commandConfirmation");
    if (err == ESP_OK) {
        persist_discovered_ble_address(op);
    }
    return err;
}

static esp_err_t confirm_command_target_until_deadline(
    hub_ble_operation_t *op,
    const hub_ble_confirmation_target_t *target,
    bool direct_first,
    int64_t deadline_us,
    int *confirmation_ms,
    uint32_t *observation_sequence,
    bool *connection_healthy,
    int *read_count,
    int *selector_write_count,
    int *direct_read_count,
    int *reprime_count,
    hub_ble_read_selector_proof_t *proof_candidate
)
{
    int64_t started_us = esp_timer_get_time();
    if (confirmation_ms != NULL) {
        *confirmation_ms = 0;
    }
    if (observation_sequence != NULL) {
        *observation_sequence = 0;
    }
    if (connection_healthy != NULL) {
        *connection_healthy = true;
    }
    if (proof_candidate != NULL) {
        memset(proof_candidate, 0, sizeof(*proof_candidate));
        proof_candidate->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    int reads = 0;
    int selector_writes = 0;
    int direct_reads = 0;
    int reprimes = 0;
    bool selector_primed = direct_first;
    bool boilers_selector_evidence = direct_first;
    bool reprime_used = false;

    while (deadline_remaining_ms(deadline_us) > 0) {
        bool direct = selector_primed;
        reads++;
        if (direct) {
            direct_reads++;
        } else {
            selector_writes++;
            if (reprime_used) {
                reprimes++;
            }
        }
        esp_err_t read_err = direct
            ? read_realtime_state_direct_until_deadline(op, target, deadline_us)
            : read_realtime_state_until_deadline(op, target, deadline_us);
        if (read_err == ESP_OK) {
            if (!direct) {
                boilers_selector_evidence = true;
            }
            selector_primed = true;
            if (observation_sequence != NULL) {
                *observation_sequence = ble_state.cache_sequence;
            }
            if (confirmation_target_matches(target, ble_state.state_json)) {
                if (confirmation_ms != NULL) {
                    *confirmation_ms = (int)((esp_timer_get_time() - started_us) / 1000);
                }
                if (read_count != NULL) {
                    *read_count = reads;
                }
                if (selector_write_count != NULL) {
                    *selector_write_count = selector_writes;
                }
                if (direct_read_count != NULL) {
                    *direct_read_count = direct_reads;
                }
                if (reprime_count != NULL) {
                    *reprime_count = reprimes;
                }
                if (boilers_selector_evidence) {
                    (void)prepare_boilers_selector_proof_candidate(
                        op,
                        ble_state.cache_sequence,
                        proof_candidate
                    );
                }
                return ESP_OK;
            }
            if (connection_healthy != NULL) {
                *connection_healthy = true;
            }
        } else {
            if (connection_healthy != NULL) {
                *connection_healthy = false;
            }
            if (direct && read_err == ESP_ERR_INVALID_RESPONSE && !reprime_used) {
                reprime_used = true;
                selector_primed = false;
                boilers_selector_evidence = false;
                continue;
            }
            break;
        }

        uint32_t remaining_ms = deadline_remaining_ms(deadline_us);
        if (remaining_ms == 0) {
            break;
        }
        uint32_t delay_ms = remaining_ms < (uint32_t)HUB_BLE_STATE_CONFIRMATION_POLL_MS
            ? remaining_ms
            : (uint32_t)HUB_BLE_STATE_CONFIRMATION_POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    if (confirmation_ms != NULL) {
        *confirmation_ms = (int)((esp_timer_get_time() - started_us) / 1000);
    }
    if (read_count != NULL) {
        *read_count = reads;
    }
    if (selector_write_count != NULL) {
        *selector_write_count = selector_writes;
    }
    if (direct_read_count != NULL) {
        *direct_read_count = direct_reads;
    }
    if (reprime_count != NULL) {
        *reprime_count = reprimes;
    }
    set_last_error("BLE write was accepted but device state was not confirmed before the deadline.");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_brew_timer_mode(hub_ble_operation_t *op)
{
    char mode_raw[128] = {0};
    char mode[128] = {0};

    set_status("reading");
    esp_err_t err = read_setting(op, "machineMode", mode_raw, sizeof(mode_raw));
    if (err == ESP_OK) {
        parse_json_string_value(mode_raw, mode, sizeof(mode));
        hub_brew_timer_update_mode(mode, esp_timer_get_time());
    }
    return err;
}

static bool write_status_is_success(const char *status)
{
    cJSON *root = cJSON_Parse(status);
    cJSON *status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
    bool succeeded = cJSON_IsString(status_item)
        && status_item->valuestring != NULL
        && strcasecmp(status_item->valuestring, "success") == 0;
    cJSON_Delete(root);
    return succeeded;
}

static esp_err_t write_json_command(hub_ble_operation_t *op, const char *json, char *status, size_t status_len, const char *label)
{
    consume_read_selector_proof_before_setting_write();
    esp_err_t err = write_gatt(op, op->write_handle, json, strlen(json) + 1, label);
    if (err != ESP_OK) {
        return err;
    }
    err = read_gatt(op, op->write_handle, status, status_len, label);
    if (err != ESP_OK) {
        return err;
    }
    if (!write_status_is_success(status)) {
        char message[128] = {0};
        snprintf(
            message,
            sizeof(message),
            "BLE command failed at %s: %.80s",
            label,
            status[0] != '\0' ? status : "empty status"
        );
        set_last_error(message);
        return ESP_FAIL;
    }
    persist_discovered_ble_address(op);
    return ESP_OK;
}

static esp_err_t write_json_command_until_deadline(
    hub_ble_operation_t *op,
    const char *json,
    char *status,
    size_t status_len,
    const char *label,
    int64_t deadline_us
)
{
    uint32_t remaining_ms = deadline_remaining_ms(deadline_us);
    if (remaining_ms == 0) {
        set_last_error("BLE command deadline expired before write.");
        return ESP_ERR_TIMEOUT;
    }
    consume_read_selector_proof_before_setting_write();
    esp_err_t err = write_gatt_with_timeout(
        op,
        op->write_handle,
        json,
        strlen(json) + 1,
        label,
        remaining_ms
    );
    if (err != ESP_OK) {
        return err;
    }

    while ((remaining_ms = deadline_remaining_ms(deadline_us)) > 0) {
        err = read_gatt_once_with_timeout(
            op,
            op->write_handle,
            status,
            status_len,
            label,
            remaining_ms
        );
        if (err == ESP_OK) {
            break;
        }
        if (err == ESP_ERR_TIMEOUT) {
            return err;
        }
        remaining_ms = deadline_remaining_ms(deadline_us);
        if (remaining_ms == 0) {
            return ESP_ERR_TIMEOUT;
        }
        uint32_t delay_ms = remaining_ms < (uint32_t)HUB_BLE_STATE_CONFIRMATION_POLL_MS
            ? remaining_ms
            : (uint32_t)HUB_BLE_STATE_CONFIRMATION_POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    if (err != ESP_OK) {
        set_last_error("BLE command status was not available before the deadline.");
        return ESP_ERR_TIMEOUT;
    }
    if (!write_status_is_success(status)) {
        char message[128] = {0};
        snprintf(
            message,
            sizeof(message),
            "BLE command failed at %s: %.80s",
            label,
            status[0] != '\0' ? status : "empty status"
        );
        set_last_error(message);
        return ESP_FAIL;
    }
    persist_discovered_ble_address(op);
    return ESP_OK;
}

static esp_err_t prepare_authenticated_connection(hub_ble_operation_t *op, bool discover_write_handle);
static void finish_active_connection(hub_ble_operation_t *op, bool keep_alive);
static bool restore_cached_session(hub_ble_operation_t *op, bool require_write_handle);

static void set_ble_command_result(
    hub_ble_command_result_t *result,
    bool ok,
    int64_t started_us,
    const char *message
)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->ok = ok;
    result->duration_ms = (int)((esp_timer_get_time() - started_us) / 1000);
    copy_safe(result->message, sizeof(result->message), message);
}

static void set_ble_command_attempt(hub_ble_command_result_t *result, bool request_started)
{
    if (result == NULL) {
        return;
    }
    result->request_started = request_started;
    result->attempts = request_started ? 1 : 0;
}

static esp_err_t execute_ble_write_json(
    const char *command_json,
    const char *label,
    hub_ble_command_result_t *result
)
{
    int64_t started_us = esp_timer_get_time();
    int64_t command_deadline_us = started_us
        + (int64_t)HUB_BLE_COMMAND_DEADLINE_MS * 1000;
    if (command_json == NULL || command_json[0] == '\0') {
        set_ble_command_result(result, false, started_us, "BLE command body is empty.");
        return ESP_ERR_INVALID_ARG;
    }
    hub_machine_selection_identity_t target = {0};
    if (!hub_command_target_guard_get(&target)
        || !hub_config_machine_selection_matches(&target)) {
        set_ble_command_result(result, false, started_us, "BLE command target is no longer active.");
        if (result != NULL) {
            result->guard_rejected = true;
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!hub_ble_configured() || !hub_cloud_ble_token_cached()) {
        set_ble_command_result(result, false, started_us, "BLE is not configured or BLE auth token is not cached.");
        return ESP_ERR_INVALID_STATE;
    }
    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        set_ble_command_result(result, false, started_us, ble_state.last_error);
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(selection.serial_number, target.serial_number) != 0) {
        set_last_error("BLE command target does not match the selected machine.");
        set_ble_command_result(result, false, started_us, ble_state.last_error);
        if (result != NULL) {
            result->guard_rejected = true;
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_initialized) {
        set_last_error("BLE stack is not initialized.");
        set_ble_command_result(result, false, started_us, ble_state.last_error);
        return ESP_ERR_INVALID_STATE;
    }
    if (ble_state.manual_intervention_recommended && !ble_cache_is_fresh()) {
        set_ble_command_result(
            result,
            false,
            started_us,
            "BLE recovery backoff is active; use Cloud fallback."
        );
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t remaining_ms = deadline_remaining_ms(command_deadline_us);
    uint32_t sync_timeout_ms = remaining_ms < (uint32_t)HUB_BLE_COMMAND_SYNC_TIMEOUT_MS
        ? remaining_ms
        : (uint32_t)HUB_BLE_COMMAND_SYNC_TIMEOUT_MS;
    if (!ble_synced && (sync_timeout_ms == 0 || !wait_sem(ble_sync_sem, sync_timeout_ms))) {
        set_last_error("BLE host sync timeout.");
        set_ble_command_result(result, false, started_us, ble_state.last_error);
        return ESP_ERR_TIMEOUT;
    }
    remaining_ms = deadline_remaining_ms(command_deadline_us);
    uint32_t mutex_timeout_ms = remaining_ms < (uint32_t)HUB_BLE_COMMAND_MUTEX_TIMEOUT_MS
        ? remaining_ms
        : (uint32_t)HUB_BLE_COMMAND_MUTEX_TIMEOUT_MS;
    if (mutex_timeout_ms == 0
        || xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(mutex_timeout_ms)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        set_ble_command_result(result, false, started_us, ble_state.last_error);
        return ESP_ERR_TIMEOUT;
    }

    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;
    op.selection_identity = target;
    if (!restore_cached_session(&op, false) || op.write_handle == 0) {
        set_last_error("BLE control session is not prewarmed; use Cloud fallback.");
        set_ble_command_result(result, false, started_us, ble_state.last_error);
        if (result != NULL) {
            result->pre_write_ms = (int)((esp_timer_get_time() - started_us) / 1000);
        }
        set_ble_command_attempt(result, false);
        xSemaphoreGive(ble_op_mutex);
        if (ble_worker_handle != NULL) {
            xTaskNotifyGive(ble_worker_handle);
        }
        return ESP_ERR_NOT_FOUND;
    }

    ble_state.configured = true;
    ble_state.refresh_pending = true;
    hub_ble_confirmation_target_t confirmation_target = confirmation_target_from_command(command_json);
    bool confirmation_can_read_direct_first = confirmation_target.kind != HUB_BLE_CONFIRM_NONE
        && command_confirmation_direct_read_ready(&op);
    esp_err_t err = ESP_OK;
    bool request_started = false;
    bool guard_rejected = false;
    bool write_accepted = false;
    bool state_confirmed = false;
    bool confirmation_pending = false;
    bool connection_healthy = true;
    int pre_write_ms = 0;
    int write_status_ms = 0;
    int state_confirmation_ms = 0;
    int confirmation_read_count = 0;
    int confirmation_selector_write_count = 0;
    int confirmation_direct_read_count = 0;
    int confirmation_reprime_count = 0;
    uint32_t observation_sequence = 0;
    hub_ble_read_selector_proof_t confirmation_proof_candidate = {0};
    confirmation_proof_candidate.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    char status[128] = {0};
    if (err == ESP_OK) {
        if (!hub_config_machine_selection_matches(&target)
            || strcmp(op.selection.serial_number, target.serial_number) != 0) {
            set_last_error("BLE command target changed before write.");
            guard_rejected = true;
            err = ESP_ERR_INVALID_STATE;
        } else {
            set_status("writing");
            if (deadline_remaining_ms(command_deadline_us) == 0) {
                set_last_error("BLE command deadline expired before write.");
                err = ESP_ERR_TIMEOUT;
            } else {
                pre_write_ms = (int)((esp_timer_get_time() - started_us) / 1000);
                request_started = true;
                int64_t write_status_started_us = esp_timer_get_time();
                err = write_json_command_until_deadline(
                    &op,
                    command_json,
                    status,
                    sizeof(status),
                    label,
                    command_deadline_us
                );
                write_status_ms = (int)((esp_timer_get_time() - write_status_started_us) / 1000);
            }
            write_accepted = err == ESP_OK;
            if (write_accepted && confirmation_target.kind != HUB_BLE_CONFIRM_NONE) {
                err = confirm_command_target_until_deadline(
                    &op,
                    &confirmation_target,
                    confirmation_can_read_direct_first,
                    command_deadline_us,
                    &state_confirmation_ms,
                    &observation_sequence,
                    &connection_healthy,
                    &confirmation_read_count,
                    &confirmation_selector_write_count,
                    &confirmation_direct_read_count,
                    &confirmation_reprime_count,
                    &confirmation_proof_candidate
                );
                state_confirmed = err == ESP_OK;
                confirmation_pending = !state_confirmed;
            }
        }
    }
    ble_state.refresh_pending = false;
    if (err == ESP_OK) {
        set_status("cached");
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        ble_state.last_error[0] = '\0';
        set_ble_command_result(
            result,
            true,
            started_us,
            confirmation_target.kind == HUB_BLE_CONFIRM_NONE
                ? "BLE command completed."
                : "BLE command completed and target state was confirmed."
        );
    } else if (confirmation_pending && write_accepted) {
        set_status("confirmation_pending");
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        set_ble_command_result(
            result,
            false,
            started_us,
            "BLE write was accepted but device state confirmation timed out."
        );
    } else {
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
        ble_state.consecutive_failures++;
        ble_state.manual_intervention_recommended = true;
        copy_safe(ble_state.recovery_hint, sizeof(ble_state.recovery_hint), "restart_machine");
        set_ble_command_result(
            result,
            false,
            started_us,
            ble_state.last_error[0] != '\0' ? ble_state.last_error : "BLE command failed."
        );
    }
    set_ble_command_attempt(result, request_started);
    if (result != NULL) {
        result->guard_rejected = guard_rejected;
        result->write_accepted = write_accepted;
        result->state_confirmed = state_confirmed;
        result->state_confirmation_pending = confirmation_pending;
        /*
         * Once the setting GATT write starts, a transport/status failure can
         * no longer prove that the machine stayed unchanged.  Keep that
         * ambiguity explicit even when no Success status was observed.
         */
        result->late_side_effect_risk = request_started && err != ESP_OK;
        result->pre_write_ms = pre_write_ms;
        result->write_status_ms = write_status_ms;
        result->state_confirmation_ms = state_confirmation_ms;
        result->confirmation_read_count = confirmation_read_count;
        result->confirmation_selector_write_count = confirmation_selector_write_count;
        result->confirmation_direct_read_count = confirmation_direct_read_count;
        result->confirmation_direct_first = confirmation_can_read_direct_first
            && confirmation_read_count > 0;
        result->confirmation_reprime_count = confirmation_reprime_count;
        result->observation_sequence = observation_sequence;
        if (confirmation_pending && write_accepted) {
            copy_safe(result->error_code, sizeof(result->error_code), "stateConfirmationTimeout");
        } else if (result->late_side_effect_risk) {
            copy_safe(result->error_code, sizeof(result->error_code), "unknownOutcome");
        }
    }
    finish_active_connection(&op, write_accepted && connection_healthy);
    if (state_confirmed) {
        /*
         * Reissue only after keep-alive stored the authenticated session.
         * Failure is intentionally best-effort: the completed command remains
         * confirmed and the next command safely falls back to selector-first.
         */
        (void)commit_boilers_selector_proof_candidate(
            &op,
            &confirmation_proof_candidate,
            "commandConfirmation"
        );
    }
    xSemaphoreGive(ble_op_mutex);
    if (request_started && err != ESP_OK && ble_worker_handle != NULL) {
        xTaskNotifyGive(ble_worker_handle);
    }
    return err;
}

static bool smart_standby_mode_valid(const char *mode)
{
    return mode != NULL && (strcmp(mode, "LastBrewing") == 0 || strcmp(mode, "PowerOn") == 0);
}

static bool bbw_dose_is_valid(const char *dose)
{
    return dose != NULL && (strcmp(dose, "dose1") == 0 || strcmp(dose, "dose2") == 0);
}

static const char *bbw_ble_dose_key(const char *dose)
{
    return dose != NULL && strcmp(dose, "dose2") == 0 ? "Dose2" : "Dose1";
}

static bool bbw_mode_is_valid(const char *mode)
{
    return mode != NULL
        && (strcmp(mode, "Continuous") == 0
            || strcmp(mode, "Dose1") == 0
            || strcmp(mode, "Dose2") == 0
            || strcmp(mode, "PulsesType") == 0);
}

static bool prebrew_mode_is_valid(const char *value)
{
    return value != NULL
        && (strcmp(value, "Disabled") == 0
            || strcmp(value, "PreBrewing") == 0
            || strcmp(value, "PreInfusion") == 0);
}

static bool parse_prebrew_time_value(const char *value, double *number)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.0 || parsed > 10.0) {
        return false;
    }
    *number = parsed;
    return true;
}

static esp_err_t ble_attempt_cloud_fallback_required(const char *label, hub_ble_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    char message[128] = {0};
    snprintf(message, sizeof(message), "BLE attempt recorded for %.48s; Cloud fallback required.", label);
    set_ble_command_result(result, false, started_us, message);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t hub_ble_set_bbw_dose(const char *dose, int grams, hub_ble_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (!bbw_dose_is_valid(dose)) {
        set_ble_command_result(result, false, started_us, "Invalid Brew-by-Weight dose key.");
        return ESP_ERR_INVALID_ARG;
    }
    if (grams < 0 || grams > 200) {
        set_ble_command_result(result, false, started_us, "Brew-by-Weight dose must be between 0 and 200 grams.");
        return ESP_ERR_INVALID_ARG;
    }
    char command[192] = {0};
    snprintf(
        command,
        sizeof(command),
        "{\"name\":\"BrewByWeightSettingDose\",\"parameter\":{\"dose\":\"%s\",\"value\":%d}}",
        bbw_ble_dose_key(dose),
        grams
    );
    return execute_ble_write_json(command, "brew-by-weight dose", result);
}

esp_err_t hub_ble_set_bbw_mode(const char *mode, hub_ble_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (!bbw_mode_is_valid(mode)) {
        set_ble_command_result(result, false, started_us, "Invalid Brew-by-Weight mode.");
        return ESP_ERR_INVALID_ARG;
    }
    char command[160] = {0};
    snprintf(command, sizeof(command), "{\"name\":\"BrewByWeightChangeMode\",\"parameter\":{\"mode\":\"%s\"}}", mode);
    return execute_ble_write_json(command, "brew-by-weight mode", result);
}

esp_err_t hub_ble_set_prebrew(const char *field, const char *value, hub_ble_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (field == NULL || value == NULL || value[0] == '\0') {
        set_ble_command_result(result, false, started_us, "Prebrew field and value are required.");
        return ESP_ERR_INVALID_ARG;
    }

    char command[192] = {0};
    if (strcmp(field, "mode") == 0) {
        if (!prebrew_mode_is_valid(value)) {
            set_ble_command_result(result, false, started_us, "Invalid prebrew mode.");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(command, sizeof(command), "{\"name\":\"PreBrewingChangeMode\",\"parameter\":{\"mode\":\"%s\"}}", value);
    } else if (strcmp(field, "onTime") == 0 || strcmp(field, "offTime") == 0) {
        double seconds = 0.0;
        if (!parse_prebrew_time_value(value, &seconds)) {
            set_ble_command_result(result, false, started_us, "Prebrew time must be between 0 and 10 seconds.");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(
            command,
            sizeof(command),
            "{\"name\":\"PreBrewingSettingTime\",\"parameter\":{\"field\":\"%s\",\"value\":%.1f}}",
            strcmp(field, "onTime") == 0 ? "In" : "Out",
            seconds
        );
    } else {
        set_ble_command_result(result, false, started_us, "Invalid prebrew field.");
        return ESP_ERR_INVALID_ARG;
    }
    return execute_ble_write_json(command, "prebrew", result);
}

esp_err_t hub_ble_set_coffee_temperature(double temperature, hub_ble_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (temperature < 80.0 || temperature > 100.0) {
        set_ble_command_result(result, false, started_us, "Coffee temperature must be between 80.0 and 100.0 C.");
        return ESP_ERR_INVALID_ARG;
    }
    char command[192] = {0};
    snprintf(command, sizeof(command), "{\"name\":\"SettingBoilerTarget\",\"parameter\":{\"identifier\":\"CoffeeBoiler1\",\"value\":%.1f}}", temperature);
    return execute_ble_write_json(command, "coffee temperature", result);
}

esp_err_t hub_ble_set_steam_temperature(double temperature, hub_ble_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (temperature < 126.0 || temperature > 131.0) {
        set_ble_command_result(result, false, started_us, "Steam temperature must be between 126.0 and 131.0 C.");
        return ESP_ERR_INVALID_ARG;
    }
    char command[192] = {0};
    snprintf(command, sizeof(command), "{\"name\":\"SettingBoilerTarget\",\"parameter\":{\"identifier\":\"SteamBoiler\",\"value\":%.1f}}", temperature);
    return execute_ble_write_json(command, "steam temperature", result);
}

esp_err_t hub_ble_set_steam_enabled(bool enabled, hub_ble_command_result_t *result)
{
    char command[192] = {0};
    snprintf(command, sizeof(command), "{\"name\":\"SettingBoilerEnable\",\"parameter\":{\"identifier\":\"SteamBoiler\",\"state\":%s}}", enabled ? "true" : "false");
    return execute_ble_write_json(command, "steam enabled", result);
}

esp_err_t hub_ble_set_power
(
    bool enabled,
    hub_ble_command_result_t *result
)
{
    char command[160] = {0};
    snprintf(command, sizeof(command), "{\"name\":\"MachineChangeMode\",\"parameter\":{\"mode\":\"%s\"}}", enabled ? "BrewingMode" : "StandBy");
    return execute_ble_write_json(command, "power", result);
}

esp_err_t hub_ble_set_smart_standby(bool enabled, int minutes, const char *mode, hub_ble_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (minutes < 1 || minutes > 240) {
        set_ble_command_result(result, false, started_us, "Smart standby minutes must be between 1 and 240.");
        return ESP_ERR_INVALID_ARG;
    }
    if (!smart_standby_mode_valid(mode)) {
        set_ble_command_result(result, false, started_us, "Smart standby mode must be LastBrewing or PowerOn.");
        return ESP_ERR_INVALID_ARG;
    }
    char command[192] = {0};
    snprintf(command, sizeof(command), "{\"name\":\"SettingSmartStandby\",\"parameter\":{\"minutes\":%d,\"mode\":\"%s\",\"enabled\":%s}}", minutes, mode, enabled ? "true" : "false");
    return execute_ble_write_json(command, "smart standby", result);
}

esp_err_t hub_ble_start_backflush(hub_ble_command_result_t *result)
{
    return execute_ble_write_json(
        "{\"name\":\"BackFlushStartCleaning\",\"parameter\":{\"enabled\":true}}",
        "backflush",
        result
    );
}

esp_err_t hub_ble_update_machine_firmware(hub_ble_command_result_t *result)
{
    return ble_attempt_cloud_fallback_required("machine firmware update", result);
}

static void append_command_json(hub_ble_write_validation_result_t *result, int started_ms, const char *message)
{
    if (result == NULL || result->command_count >= 6) {
        return;
    }
    char item[128] = {0};
    snprintf(
        item,
        sizeof(item),
        "%s{\"ok\":true,\"transport\":\"ble\",\"durationMs\":%d,\"message\":\"%s\"}",
        result->command_count == 0 ? "" : ",",
        (int)(now_ms() - started_ms),
        message
    );
    size_t used = strlen(result->commands_json);
    if (used < sizeof(result->commands_json)) {
        snprintf(result->commands_json + used, sizeof(result->commands_json) - used, "%s", item);
    }
    result->command_count++;
}

static esp_err_t write_noop_targets(hub_ble_operation_t *op, const hub_ble_noop_targets_t *targets, hub_ble_write_validation_result_t *result)
{
    char command[192] = {0};
    char status[128] = {0};

    int command_started = (int)now_ms();
    snprintf(
        command,
        sizeof(command),
        "{\"name\":\"SettingBoilerTarget\",\"parameter\":{\"identifier\":\"CoffeeBoiler1\",\"value\":%.1f}}",
        targets->coffee_target
    );
    esp_err_t err = write_json_command(op, command, status, sizeof(status), "coffee temperature no-op");
    if (err != ESP_OK) {
        return err;
    }
    append_command_json(result, command_started, "coffee temperature no-op");

    command_started = (int)now_ms();
    snprintf(
        command,
        sizeof(command),
        "{\"name\":\"SettingBoilerTarget\",\"parameter\":{\"identifier\":\"SteamBoiler\",\"value\":%.1f}}",
        targets->steam_target
    );
    err = write_json_command(op, command, status, sizeof(status), "steam temperature no-op");
    if (err != ESP_OK) {
        return err;
    }
    append_command_json(result, command_started, "steam temperature no-op");

    command_started = (int)now_ms();
    snprintf(
        command,
        sizeof(command),
        "{\"name\":\"SettingBoilerEnable\",\"parameter\":{\"identifier\":\"SteamBoiler\",\"state\":%s}}",
        targets->steam_enabled ? "true" : "false"
    );
    err = write_json_command(op, command, status, sizeof(status), "steam enabled no-op");
    if (err != ESP_OK) {
        return err;
    }
    append_command_json(result, command_started, "steam enabled no-op");

    return ESP_OK;
}

static bool cancel_pending_connect(void)
{
    if (!ble_gap_conn_active()) {
        return true;
    }

    drain_sem(ble_connect_sem);
    int rc = ble_gap_conn_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "BLE connect cancellation failed: rc=%d", rc);
        return false;
    }

    int64_t deadline_ms = now_ms() + HUB_BLE_CONNECT_CANCEL_TIMEOUT_MS;
    while (ble_gap_conn_active() && now_ms() < deadline_ms) {
        int64_t remaining_ms = deadline_ms - now_ms();
        uint32_t wait_ms = remaining_ms > HUB_BLE_CONNECT_CANCEL_POLL_MS
            ? HUB_BLE_CONNECT_CANCEL_POLL_MS
            : (uint32_t)remaining_ms;
        (void)wait_sem(ble_connect_sem, wait_ms);
    }
    if (ble_gap_conn_active()) {
        ESP_LOGW(TAG, "BLE connect cancellation did not become idle");
        return false;
    }
    return true;
}

static bool cancel_pending_scan(void)
{
    if (!ble_scan_teardown_pending && !ble_gap_disc_active()) {
        return true;
    }

    drain_sem(ble_scan_sem);
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "BLE scan cancellation failed: rc=%d", rc);
        return false;
    }

    int64_t deadline_ms = now_ms() + HUB_BLE_SCAN_CANCEL_TIMEOUT_MS;
    while (ble_gap_disc_active() && now_ms() < deadline_ms) {
        int64_t remaining_ms = deadline_ms - now_ms();
        uint32_t wait_ms = remaining_ms > HUB_BLE_SCAN_CANCEL_POLL_MS
            ? HUB_BLE_SCAN_CANCEL_POLL_MS
            : (uint32_t)remaining_ms;
        (void)wait_sem(ble_scan_sem, wait_ms);
    }
    if (ble_gap_disc_active()) {
        ESP_LOGW(TAG, "BLE scan cancellation did not become idle");
        return false;
    }

    /* The host can become idle before delivering DISC_COMPLETE. */
    ble_scan_teardown_pending = false;
    return true;
}

static esp_err_t scan_for_target(hub_ble_operation_t *op, uint8_t own_addr_type, int duration_ms)
{
    if (!cancel_pending_scan()) {
        set_last_error("Previous BLE scan teardown is still pending; use Cloud fallback.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!cancel_pending_connect()) {
        set_last_error("Previous BLE connect teardown is still pending; use Cloud fallback.");
        return ESP_ERR_INVALID_STATE;
    }
    struct ble_gap_disc_params disc_params = {0};
    disc_params.filter_duplicates = 0;
    disc_params.passive = 0;
    disc_params.itvl = 0x0040;
    disc_params.window = 0x0040;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    op->target_found = false;
    op->scan_seen_count = 0;
    op->scan_named_count = 0;
    op->last_scan_name[0] = '\0';
    op->last_scan_address[0] = '\0';
    drain_sem(ble_scan_sem);
    set_status("scanning");
    ble_state.refresh_pending = true;

    ble_scan_teardown_pending = true;
    int rc = ble_gap_disc(own_addr_type, duration_ms, &disc_params, hub_ble_gap_event, NULL);
    if (rc != 0) {
        ble_scan_teardown_pending = false;
        set_last_error("BLE scan could not start.");
        return ESP_FAIL;
    }
    int64_t scan_deadline_ms = now_ms() + duration_ms + 500;
    while (now_ms() < scan_deadline_ms) {
        if (ble_diagnostic_should_yield()) {
            (void)cancel_pending_scan();
            return ESP_ERR_INVALID_STATE;
        }
        int64_t remaining_ms = scan_deadline_ms - now_ms();
        uint32_t wait_ms = remaining_ms > 100 ? 100 : (uint32_t)remaining_ms;
        if (wait_sem(ble_scan_sem, wait_ms)) {
            break;
        }
    }
    if (ble_scan_teardown_pending) {
        if (!cancel_pending_scan()) {
            set_last_error("BLE scan completion was not confirmed; use Cloud fallback.");
            return ESP_ERR_TIMEOUT;
        }
    }
    if (!op->target_found) {
        char message[128] = {0};
        snprintf(message,
                 sizeof(message),
                 "BLE scan failed: LINEAR not visible (seen=%d, named=%d, last=%.24s/%.17s).",
                 op->scan_seen_count,
                 op->scan_named_count,
                 op->last_scan_name[0] != '\0' ? op->last_scan_name : "-",
                 op->last_scan_address[0] != '\0' ? op->last_scan_address : "-");
        set_last_error(message);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static esp_err_t connect_target(hub_ble_operation_t *op, uint8_t own_addr_type, int timeout_ms)
{
    if (!cancel_pending_connect()) {
        set_last_error("Previous BLE connect teardown is still pending; use Cloud fallback.");
        return ESP_ERR_INVALID_STATE;
    }
    drain_sem(ble_connect_sem);
    set_status("connecting");
    int rc = ble_gap_connect(own_addr_type, &op->target_addr, timeout_ms, NULL, hub_ble_gap_event, NULL);
    if (rc != 0) {
        set_last_error("BLE connect could not start.");
        return ESP_FAIL;
    }
    bool completed = wait_sem(ble_connect_sem, (uint32_t)timeout_ms + 500);
    if (completed && op->connected) {
        return ESP_OK;
    }

    bool cancelled = cancel_pending_connect();
    if (op->connected) {
        return ESP_OK;
    }
    if (!cancelled) {
        set_last_error("BLE connect teardown timed out; use Cloud fallback.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!completed) {
        set_last_error("BLE connect timeout.");
        return ESP_ERR_TIMEOUT;
    }
    set_last_error("BLE connect failed.");
    return ESP_FAIL;
}

static void reset_cached_session(void)
{
    cached_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    cached_connection_epoch = 0;
    cached_auth_handle = 0;
    cached_read_handle = 0;
    cached_write_handle = 0;
    reset_read_selector_proof();
    cached_rssi = 0;
    cached_target_name[0] = '\0';
    cached_target_address[0] = '\0';
    cached_match_reason[0] = '\0';
    memset(&cached_selection_identity, 0, sizeof(cached_selection_identity));
    cached_ble_auth_fingerprint = 0;
}

static uint64_t ble_auth_fingerprint(const char *token)
{
    uint64_t value = 1469598103934665603ULL;
    if (token == NULL) {
        return value;
    }
    for (const unsigned char *cursor = (const unsigned char *)token; *cursor != '\0'; cursor++) {
        value ^= *cursor;
        value *= 1099511628211ULL;
    }
    return value;
}

static bool ensure_operation_selection_identity(hub_ble_operation_t *op)
{
    if (op == NULL || op->selection.serial_number[0] == '\0') {
        return false;
    }
    if (op->selection_identity.serial_number[0] == '\0') {
        if (hub_config_get_machine_selection_identity(&op->selection_identity) != ESP_OK
            || strcmp(op->selection_identity.serial_number, op->selection.serial_number) != 0) {
            memset(&op->selection_identity, 0, sizeof(op->selection_identity));
            return false;
        }
    }
    return strcmp(op->selection_identity.serial_number, op->selection.serial_number) == 0
        && hub_config_machine_selection_matches(&op->selection_identity);
}

static bool cached_session_alive(void)
{
    if (cached_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    struct ble_gap_conn_desc desc = {0};
    int rc = ble_gap_conn_find(cached_conn_handle, &desc);
    if (rc != 0) {
        reset_cached_session();
        return false;
    }
    return true;
}

static bool store_cached_session(const hub_ble_operation_t *op)
{
    if (op == NULL
        || op->conn_handle == BLE_HS_CONN_HANDLE_NONE
        || op->connection_epoch == 0
        || op->selection_identity.serial_number[0] == '\0'
        || strcmp(op->selection_identity.serial_number, op->selection.serial_number) != 0
        || !hub_config_machine_selection_matches(&op->selection_identity)) {
        return false;
    }
    if (!read_selector_proof_belongs_to_operation(op)) {
        reset_read_selector_proof();
    }
    cached_conn_handle = op->conn_handle;
    cached_connection_epoch = op->connection_epoch;
    cached_auth_handle = op->auth_handle;
    cached_read_handle = op->read_handle;
    cached_write_handle = op->write_handle;
    cached_rssi = op->rssi;
    copy_safe(cached_target_name, sizeof(cached_target_name), op->target_name);
    copy_safe(cached_target_address, sizeof(cached_target_address), op->target_address);
    copy_safe(cached_match_reason, sizeof(cached_match_reason), op->match_reason);
    cached_selection_identity = op->selection_identity;
    cached_ble_auth_fingerprint = ble_auth_fingerprint(op->selection.ble_auth_token);
    return true;
}

static void terminate_cached_session(void)
{
    uint16_t conn_handle = cached_conn_handle;
    reset_cached_session();
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static bool cached_session_matches_operation(const hub_ble_operation_t *op)
{
    if (op == NULL
        || cached_connection_epoch == 0
        || op->connection_epoch != cached_connection_epoch
        || cached_selection_identity.serial_number[0] == '\0'
        || op->selection_identity.serial_number[0] == '\0') {
        return false;
    }
    if (cached_selection_identity.generation != op->selection_identity.generation
        || strcmp(cached_selection_identity.serial_number, op->selection_identity.serial_number) != 0
        || cached_ble_auth_fingerprint != ble_auth_fingerprint(op->selection.ble_auth_token)) {
        return false;
    }
    return op->selection.ble_device_address[0] == '\0'
        || cached_target_address[0] == '\0'
        || strcasecmp(op->selection.ble_device_address, cached_target_address) == 0;
}

static bool restore_cached_session(hub_ble_operation_t *op, bool require_write_handle)
{
    if (!ensure_operation_selection_identity(op)) {
        return false;
    }
    if (!cached_session_alive() || cached_read_handle == 0) {
        return false;
    }
    op->connection_epoch = cached_connection_epoch;
    if (!cached_session_matches_operation(op)) {
        ESP_LOGI(TAG, "Discarding BLE GATT session because machine selection changed");
        terminate_cached_session();
        return false;
    }

    op->reused_session = true;
    op->target_found = true;
    op->connected = true;
    op->conn_handle = cached_conn_handle;
    op->connection_epoch = cached_connection_epoch;
    op->auth_handle = cached_auth_handle;
    op->read_handle = cached_read_handle;
    op->write_handle = cached_write_handle;
    op->rssi = cached_rssi;
    copy_safe(op->target_name, sizeof(op->target_name), cached_target_name[0] != '\0' ? cached_target_name : "cached BLE session");
    copy_safe(op->target_address, sizeof(op->target_address), cached_target_address);
    copy_safe(op->match_reason, sizeof(op->match_reason), "cached_session");

    if (require_write_handle && op->write_handle == 0) {
        esp_err_t err = discover_handle(op, LM_WRITE_UUID, &op->write_handle);
        if (err != ESP_OK) {
            reset_cached_session();
            return false;
        }
        cached_write_handle = op->write_handle;
    }

    set_status("connected");
    ESP_LOGI(TAG, "Reusing cached BLE GATT session");
    return true;
}

static void finish_active_connection(hub_ble_operation_t *op, bool keep_alive)
{
    if (op != NULL && op->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        if (keep_alive) {
            if (store_cached_session(op)) {
                active_op = NULL;
                return;
            }
            keep_alive = false;
        }
        if (cached_read_selector_proof.boilers
            && cached_read_selector_proof.conn_handle == op->conn_handle
            && cached_read_selector_proof.connection_epoch == op->connection_epoch) {
            reset_read_selector_proof();
        }
        if (cached_conn_handle == op->conn_handle
            && cached_connection_epoch == op->connection_epoch) {
            reset_cached_session();
        }
        ble_gap_terminate(op->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        op->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        op->connected = false;
    }
    active_op = NULL;
}

static esp_err_t prepare_authenticated_connection(hub_ble_operation_t *op, bool discover_write_handle)
{
    if (ble_scan_teardown_pending) {
        set_last_error("Previous BLE scan teardown is still pending; use Cloud fallback.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ensure_operation_selection_identity(op)) {
        set_last_error("BLE operation target is no longer the selected machine.");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        set_last_error("BLE host address is not ready.");
        return ESP_FAIL;
    }

    if (restore_cached_session(op, discover_write_handle)) {
        return ESP_OK;
    }

    active_op = op;
    bool direct_target = false;
    esp_err_t err = ESP_OK;

    if (op->selection.ble_device_address[0] != '\0') {
        ESP_LOGI(TAG, "Configured BLE address direct fast path enabled");
        direct_target = use_configured_address_direct(op);
        err = direct_target ? ESP_OK : scan_for_target(op, own_addr_type, HUB_BLE_SCAN_FALLBACK_TIMEOUT_MS);
    } else if (op->selection.ble_device_name[0] != '\0') {
        err = scan_for_target(op, own_addr_type, HUB_BLE_SCAN_NAME_TIMEOUT_MS);
        if (err != ESP_OK && ble_diagnostic_yielded_on_current_task()) {
            active_op = NULL;
            return err;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Configured BLE name scan failed, trying configured address");
            direct_target = use_configured_address_direct(op);
            if (!direct_target) {
                active_op = NULL;
                return err;
            }
            err = ESP_OK;
        }
    } else {
        err = scan_for_target(op, own_addr_type, HUB_BLE_SCAN_FALLBACK_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        active_op = NULL;
        return err;
    }
    if (ble_diagnostic_should_yield()) {
        active_op = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    err = connect_target(
        op,
        own_addr_type,
        direct_target ? HUB_BLE_DIRECT_CONNECT_TIMEOUT_MS : HUB_BLE_CONNECT_TIMEOUT_MS
    );
    if (ble_diagnostic_should_yield() || ble_diagnostic_yielded_on_current_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK && direct_target) {
        ESP_LOGW(TAG, "Direct BLE public address connect failed, trying random address type");
        op->target_found = false;
        op->connected = false;
        op->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (use_configured_address_random_fallback(op)) {
            err = connect_target(op, own_addr_type, HUB_BLE_DIRECT_CONNECT_TIMEOUT_MS);
        }
    }
    if (ble_diagnostic_should_yield() || ble_diagnostic_yielded_on_current_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK && direct_target) {
        ESP_LOGW(TAG, "Direct BLE address connect failed, falling back to active scan");
        op->target_found = false;
        op->connected = false;
        op->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        err = scan_for_target(op, own_addr_type, HUB_BLE_SCAN_FALLBACK_TIMEOUT_MS);
        if (err == ESP_OK) {
            err = connect_target(op, own_addr_type, HUB_BLE_CONNECT_TIMEOUT_MS);
        }
    }
    if (err != ESP_OK) {
        active_op = NULL;
        return err;
    }

    err = exchange_mtu(op);
    if (err != ESP_OK) {
        active_op = NULL;
        return err;
    }
    if (ble_diagnostic_should_yield()) {
        return ESP_ERR_INVALID_STATE;
    }

    err = discover_handle(op, LM_AUTH_UUID, &op->auth_handle);
    if (err == ESP_OK && ble_diagnostic_should_yield()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err = discover_handle(op, LM_READ_UUID, &op->read_handle);
    }
    if (err == ESP_OK && ble_diagnostic_should_yield()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK && discover_write_handle) {
        err = discover_handle(op, LM_WRITE_UUID, &op->write_handle);
    }
    if (err == ESP_OK) {
        set_status("authenticating");
        err = write_gatt(op, op->auth_handle, op->selection.ble_auth_token, strlen(op->selection.ble_auth_token), "auth");
    }
    return err;
}

static void prewarm_control_write_handle(hub_ble_operation_t *op)
{
    if (op == NULL || op->write_handle != 0) {
        return;
    }
    /*
     * Control readiness is prepared only by a read-only/background path,
     * outside the 1500 ms setting-command deadline.  Discovery failure must
     * not invalidate the already successful state read.
     */
    esp_err_t prewarm_err = discover_handle(op, LM_WRITE_UUID, &op->write_handle);
    if (prewarm_err != ESP_OK) {
        op->write_handle = 0;
        ESP_LOGW(TAG, "BLE control WRITE characteristic prewarm failed: %s", esp_err_to_name(prewarm_err));
    }
}

static esp_err_t run_ble_read(hub_ble_operation_t *op)
{
    esp_err_t err = prepare_authenticated_connection(op, false);
    if (err == ESP_OK && ble_diagnostic_should_yield()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err = read_all_state(op);
    }
    if (err == ESP_OK) {
        prewarm_control_write_handle(op);
    }
    finish_active_connection(op, err == ESP_OK);
    return err;
}

static esp_err_t run_ble_realtime_read(hub_ble_operation_t *op)
{
    hub_ble_read_selector_proof_t proof_candidate = {0};
    proof_candidate.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    bool proof_candidate_ready = false;
    esp_err_t err = prepare_authenticated_connection(op, false);
    if (err == ESP_OK && ble_diagnostic_should_yield()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err = read_realtime_state(op);
    }
    if (err == ESP_OK) {
        prewarm_control_write_handle(op);
        proof_candidate_ready = strcmp(ble_state.cache_origin, "workerRealtime") == 0
            && prepare_boilers_selector_proof_candidate(
                op,
                ble_state.cache_sequence,
                &proof_candidate
            );
    }
    finish_active_connection(op, err == ESP_OK);
    if (err == ESP_OK && proof_candidate_ready) {
        (void)commit_boilers_selector_proof_candidate(
            op,
            &proof_candidate,
            "workerRealtime"
        );
    }
    return err;
}

static esp_err_t run_ble_brew_timer_mode_read(hub_ble_operation_t *op)
{
    if (!restore_cached_session(op, false)) {
        hub_brew_timer_update_source_stale(esp_timer_get_time());
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = read_brew_timer_mode(op);
    finish_active_connection(op, err == ESP_OK);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        hub_brew_timer_update_source_stale(esp_timer_get_time());
    }
    if (err == ESP_OK) {
        set_status("cached");
        ble_state.last_error[0] = '\0';
    }
    return err;
}

static esp_err_t run_ble_noop_write(hub_ble_operation_t *op, hub_ble_write_validation_result_t *result)
{
    esp_err_t err = prepare_authenticated_connection(op, true);
    if (err != ESP_OK) {
        finish_active_connection(op, false);
        return err;
    }

    hub_ble_noop_targets_t before = {0};
    hub_ble_noop_targets_t after = {0};
    err = read_realtime_state(op);
    if (err == ESP_OK && !extract_noop_targets(ble_state.state_json, &before)) {
        set_last_error("BLE no-op targets are incomplete.");
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && !hub_config_machine_selection_matches(&op->selection_identity)) {
        set_last_error("BLE no-op target changed before write.");
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        set_status("writing");
        err = write_noop_targets(op, &before, result);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!hub_config_machine_selection_matches(&op->selection_identity)) {
            set_last_error("BLE no-op target changed after write.");
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = read_realtime_state(op);
        }
    }
    if (err == ESP_OK && !extract_noop_targets(ble_state.state_json, &after)) {
        set_last_error("BLE no-op targets are incomplete after write.");
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK
        && (!same_temperature(before.coffee_target, after.coffee_target)
            || !same_temperature(before.steam_target, after.steam_target)
            || before.steam_enabled != after.steam_enabled)) {
        set_last_error("BLE no-op write changed the target state.");
        err = ESP_FAIL;
    }

    finish_active_connection(op, err == ESP_OK);
    return err;
}

static int hub_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    hub_ble_operation_t *op = active_op;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (op != NULL) {
            record_disc_sample(op, &event->disc);
            if (!op->target_found && match_disc(op, &event->disc)) {
                ble_state.last_seen_ms = now_ms();
                ble_gap_disc_cancel();
            }
        }
        return 0;
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC:
        if (op != NULL) {
            record_ext_disc_sample(op, &event->ext_disc);
            if (!op->target_found && match_ext_disc(op, &event->ext_disc)) {
                ble_state.last_seen_ms = now_ms();
                ble_gap_disc_cancel();
            }
        }
        return 0;
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ble_scan_teardown_pending = false;
        xSemaphoreGive(ble_scan_sem);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (op != NULL && event->connect.status == 0) {
            reset_read_selector_proof();
            op->conn_handle = event->connect.conn_handle;
            op->connection_epoch = next_connection_epoch();
            op->connected = true;
        }
        xSemaphoreGive(ble_connect_sem);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_state.connected = false;
        if (cached_read_selector_proof.boilers
            && cached_read_selector_proof.conn_handle == event->disconnect.conn.conn_handle) {
            reset_read_selector_proof();
        }
        if (cached_conn_handle == event->disconnect.conn.conn_handle) {
            reset_cached_session();
            hub_brew_timer_update_source_stale(esp_timer_get_time());
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        record_notify_probe_event(event);
        return 0;
    default:
        return 0;
    }
}

static void hub_ble_on_sync(void)
{
    ble_synced = true;
    xSemaphoreGive(ble_sync_sem);
}

static void hub_ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t hub_ble_init(void)
{
    if (ble_initialized) {
        return ESP_OK;
    }

    ble_op_mutex = xSemaphoreCreateMutex();
    ble_sync_sem = xSemaphoreCreateBinary();
    ble_scan_sem = xSemaphoreCreateBinary();
    ble_connect_sem = xSemaphoreCreateBinary();
    ble_discover_sem = xSemaphoreCreateBinary();
    ble_mtu_sem = xSemaphoreCreateBinary();
    ble_write_sem = xSemaphoreCreateBinary();
    ble_read_sem = xSemaphoreCreateBinary();
    ble_worker_job_queue = xQueueCreate(1, sizeof(hub_ble_worker_job_t));
    ble_worker_job_mutex = xSemaphoreCreateMutex();
    ble_worker_job_complete = xSemaphoreCreateBinary();
    if (ble_op_mutex == NULL || ble_sync_sem == NULL || ble_scan_sem == NULL || ble_connect_sem == NULL
        || ble_discover_sem == NULL || ble_mtu_sem == NULL || ble_write_sem == NULL || ble_read_sem == NULL
        || ble_worker_job_queue == NULL || ble_worker_job_mutex == NULL || ble_worker_job_complete == NULL) {
        set_last_error("BLE synchronization primitives could not be created.");
        return ESP_ERR_NO_MEM;
    }

    int rc = nimble_port_init();
    if (rc != 0) {
        set_status("unavailable");
        set_last_error("NimBLE init failed.");
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("LamaBuddy ESP32-S3 Hub");
    ble_hs_cfg.sync_cb = hub_ble_on_sync;
    nimble_port_freertos_init(hub_ble_host_task);
    ble_initialized = true;
    set_status("idle");
    ESP_LOGI(TAG, "NimBLE initialized for Gate F read/no-op diagnostics");
    ESP_LOGD(TAG, "La Marzocco BLE read uuid=%s write uuid=%s auth uuid=%s", LM_READ_UUID_TEXT, LM_WRITE_UUID_TEXT, LM_AUTH_UUID_TEXT);
    return ESP_OK;
}

static void record_ble_worker_stack_high_water_mark(void)
{
    UBaseType_t high_water_mark = uxTaskGetStackHighWaterMark(NULL);
    if (ble_worker_stack_high_water_mark == 0
        || high_water_mark < ble_worker_stack_high_water_mark) {
        ble_worker_stack_high_water_mark = high_water_mark;
    }
}

uint32_t hub_ble_worker_stack_high_water_mark(void)
{
    return (uint32_t)ble_worker_stack_high_water_mark;
}

esp_err_t hub_ble_run_worker_job(
    hub_ble_worker_job_fn job,
    void *context,
    esp_err_t *job_result
)
{
    if (job == NULL || !worker_started || ble_worker_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(ble_worker_job_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    drain_sem(ble_worker_job_complete);
    hub_ble_worker_job_t item = {
        .function = job,
        .context = context,
    };
    if (xQueueSend(ble_worker_job_queue, &item, 0) != pdTRUE) {
        xSemaphoreGive(ble_worker_job_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(ble_worker_handle);
    if (xSemaphoreTake(ble_worker_job_complete, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(ble_worker_job_mutex);
        return ESP_FAIL;
    }
    if (job_result != NULL) {
        *job_result = ble_worker_job_result;
    }
    xSemaphoreGive(ble_worker_job_mutex);
    return ESP_OK;
}

static void hub_ble_worker_task(void *param)
{
    int full_read_delay_ms = HUB_BLE_WORKER_BOOT_DELAY_MS;

    // machineMode did not prove shot lifecycle in real-machine probes. Keep automatic
    // BLE ownership with the state refresh and command-confirmation paths only.
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(full_read_delay_ms));

        hub_ble_worker_job_t job = {0};
        if (xQueueReceive(ble_worker_job_queue, &job, 0) == pdTRUE) {
            uint32_t cache_sequence_before_job = ble_state.cache_sequence;
            ble_worker_job_yielded = false;
            ble_worker_job_owner = xTaskGetCurrentTaskHandle();
            ble_worker_job_active = true;
            ble_worker_job_result = job.function(job.context);
            ble_worker_job_active = false;
            ble_worker_job_owner = NULL;
            record_ble_worker_stack_high_water_mark();
            /* HTTP job success alone does not prove a fresh BLE observation. */
            if (!ble_worker_job_yielded
                && ble_state.cache_sequence != cache_sequence_before_job
                && ble_state.consecutive_failures == 0
                && ble_cache_is_fresh()) {
                full_read_delay_ms = HUB_BLE_WORKER_POLL_MS;
            }
            ble_worker_job_yielded = false;
            xSemaphoreGive(ble_worker_job_complete);
            continue;
        }

        if (!hub_ble_configured()) {
            hub_brew_timer_update_source_stale(esp_timer_get_time());
            full_read_delay_ms = ble_worker_retry_delay_ms(ble_state.consecutive_failures);
            continue;
        }

        if (hub_command_interactive_pending()) {
            full_read_delay_ms = HUB_BLE_WORKER_PRIORITY_RECHECK_MS;
            continue;
        }

        int command_cache_delay_ms = worker_delay_after_recent_command_confirmation();
        if (command_cache_delay_ms > 0) {
            full_read_delay_ms = command_cache_delay_ms;
            continue;
        }

        hub_ble_read_result_t ignored = {0};
        esp_err_t err = validate_read_internal(&ignored, true);
        record_ble_worker_stack_high_water_mark();
        full_read_delay_ms = (err == ESP_OK)
            ? HUB_BLE_WORKER_POLL_MS
            : ble_worker_retry_delay_ms(ble_state.consecutive_failures);
    }
}

esp_err_t hub_ble_start_worker(void)
{
    if (worker_started) {
        return ESP_OK;
    }
    worker_started = true;
    ble_state.worker_started = true;
    if (xTaskCreate(
            hub_ble_worker_task,
            "hub_ble_worker",
            HUB_BLE_WORKER_STACK_BYTES,
            NULL,
            4,
            &ble_worker_handle
        ) != pdPASS) {
        worker_started = false;
        ble_state.worker_started = false;
        ble_worker_handle = NULL;
        set_last_error("BLE worker task could not be created.");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool hub_ble_configured(void)
{
    hub_machine_selection_t selection = {0};
    return load_selection(&selection);
}

esp_err_t hub_ble_poll_brew_timer_mode(void)
{
    if (!hub_ble_configured() || !ble_initialized) {
        hub_brew_timer_update_source_stale(esp_timer_get_time());
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_BREW_TIMER_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        hub_brew_timer_update_source_stale(esp_timer_get_time());
        xSemaphoreGive(ble_op_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ble_state.configured = true;
    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;
    esp_err_t err = run_ble_brew_timer_mode_read(&op);
    xSemaphoreGive(ble_op_mutex);
    return err;
}

bool hub_ble_reads_validated(void)
{
    return ble_state.reads_validated && ble_cache_identity_matches_selection();
}

bool hub_ble_writes_validated(void)
{
    return ble_state.writes_validated && ble_cache_identity_matches_selection();
}

bool hub_ble_has_cached_state(void)
{
    return ble_cache_is_fresh();
}

bool hub_ble_refresh_pending(void)
{
    return ble_state.refresh_pending;
}

static esp_err_t validate_read_internal(hub_ble_read_result_t *result, bool realtime_only)
{
    int64_t started = now_ms();
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        hub_brew_timer_update_source_stale(esp_timer_get_time());
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (!ble_initialized) {
        hub_brew_timer_update_source_stale(esp_timer_get_time());
        set_last_error("BLE stack is not initialized.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (!ble_synced && !wait_sem(ble_sync_sem, 5000)) {
        set_last_error("BLE host sync timeout.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_OPERATION_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (ble_diagnostic_should_yield()) {
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        xSemaphoreGive(ble_op_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ble_state.configured = true;
    ble_state.refresh_pending = true;
    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;
    esp_err_t err = realtime_only ? run_ble_realtime_read(&op) : run_ble_read(&op);
    ble_state.refresh_pending = false;
    if (err == ESP_OK) {
        set_status("cached");
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        ble_state.last_error[0] = '\0';
        if (result != NULL) {
            result->ok = true;
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(
                result->message,
                sizeof(result->message),
                realtime_only
                    ? "BLE realtime read validated and cached."
                    : "BLE read validated and cached."
            );
        }
    } else if (ble_worker_job_yielded) {
        set_status(ble_state.has_cached_state ? "cached" : "idle");
        if (result != NULL) {
            result->ok = false;
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
    } else {
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            hub_brew_timer_update_source_stale(esp_timer_get_time());
        }
        bool target_not_visible = strstr(ble_state.last_error, "LINEAR not visible") != NULL;
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
        ble_state.consecutive_failures++;
        ble_state.manual_intervention_recommended = ble_state.has_cached_state || target_not_visible;
        copy_safe(
            ble_state.recovery_hint,
            sizeof(ble_state.recovery_hint),
            target_not_visible ? "check_antenna_or_wake_ble" : (ble_state.has_cached_state ? "restart_machine" : "")
        );
        if (result != NULL) {
            result->ok = false;
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
    }

    xSemaphoreGive(ble_op_mutex);
    return err;
}

esp_err_t hub_ble_validate_read(hub_ble_read_result_t *result)
{
    return validate_read_internal(result, false);
}

esp_err_t hub_ble_full_read_probe_json(char *dest, size_t dest_len)
{
    int64_t started = now_ms();
    if (dest == NULL || dest_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dest[0] = '\0';

    const char *candidates[] = {
        "boilers",
        "machineMode",
        "tankStatus",
        "shotTimer",
        "shotTime",
        "extractionSeconds",
        "extractionTime",
        "groupState",
        "brewState",
        "pumpState",
        "flowMeter",
        "brewWeight",
        "scaleWeight",
        "targetWeight",
        "brewTimer",
        "brewTime",
        "smartStandBy",
        "brewByWeightMode",
        "brewByWeightDose",
        "brewByWeightDose1",
        "brewByWeightDose2",
        "prebrewMode",
        "prebrew",
        "coffeeTemperature",
        "steamLevel",
        "steamTemperature",
    };
    const size_t candidate_count = sizeof(candidates) / sizeof(candidates[0]);
    const int full_read_budget_ms = 12000;

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"readCount\":0,\"successCount\":0,\"failureCount\":0,\"fields\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_initialized) {
        set_last_error("BLE stack is not initialized.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"readCount\":0,\"successCount\":0,\"failureCount\":0,\"fields\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_synced && !wait_sem(ble_sync_sem, 5000)) {
        set_last_error("BLE host sync timeout.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"readCount\":0,\"successCount\":0,\"failureCount\":0,\"fields\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_FULL_READ_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"readCount\":0,\"successCount\":0,\"failureCount\":0,\"fields\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (ble_diagnostic_should_yield()) {
        xSemaphoreGive(ble_op_mutex);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"readCount\":0,\"successCount\":0,\"failureCount\":0,\"fields\":[],\"error\":\"interactive control pending\"}",
            (int)(now_ms() - started)
        );
        return ESP_ERR_INVALID_STATE;
    }

    ble_state.configured = true;
    ble_state.refresh_pending = true;
    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;

    cJSON *root = cJSON_CreateObject();
    cJSON *fields = root != NULL ? cJSON_AddArrayToObject(root, "fields") : NULL;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (root != NULL && fields != NULL) {
        err = restore_cached_session(&op, false) ? ESP_OK : prepare_authenticated_connection(&op, false);
    }
    int success_count = 0;
    int failure_count = 0;
    int attempted_count = 0;
    bool stopped_early = false;
    bool connection_unhealthy = false;

    if (err == ESP_OK) {
        set_status("reading");
        for (size_t index = 0; index < candidate_count; index++) {
            if (ble_diagnostic_should_yield()) {
                err = ESP_ERR_INVALID_STATE;
                stopped_early = true;
                break;
            }
            if ((int)(now_ms() - started) >= full_read_budget_ms) {
                stopped_early = true;
                break;
            }
            int64_t read_started = now_ms();
            char raw[256] = {0};
            esp_err_t read_err = read_setting_probe(&op, candidates[index], raw, sizeof(raw));
            attempted_count++;
            cJSON *field = cJSON_CreateObject();
            if (field == NULL) {
                failure_count++;
                connection_unhealthy = true;
                continue;
            }

            cJSON_AddStringToObject(field, "name", candidates[index]);
            cJSON_AddBoolToObject(field, "ok", read_err == ESP_OK);
            cJSON_AddNumberToObject(field, "durationMs", (int)(now_ms() - read_started));
            if (read_err == ESP_OK) {
                success_count++;
                cJSON_AddStringToObject(field, "raw", raw);
                cJSON_AddBoolToObject(field, "truncated", strlen(op.read_response) >= sizeof(raw) - 1);
            } else {
                failure_count++;
                if (read_err == ESP_ERR_TIMEOUT) {
                    connection_unhealthy = true;
                }
                cJSON_AddStringToObject(field, "error", esp_err_to_name(read_err));
                cJSON_AddStringToObject(field, "message", ble_state.last_error[0] != '\0' ? ble_state.last_error : esp_err_to_name(read_err));
            }
            cJSON_AddItemToArray(fields, field);
        }
    }

    finish_active_connection(&op, err == ESP_OK && success_count > 0 && !connection_unhealthy);
    ble_state.refresh_pending = false;
    if (err == ESP_OK && success_count > 0) {
        persist_discovered_ble_address(&op);
        set_status("cached");
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        ble_state.last_error[0] = '\0';
    } else if (ble_worker_job_yielded) {
        set_status(ble_state.has_cached_state ? "cached" : "idle");
    } else if (err != ESP_OK) {
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
        ble_state.consecutive_failures++;
        ble_state.manual_intervention_recommended = ble_state.has_cached_state;
    }
    xSemaphoreGive(ble_op_mutex);

    if (root == NULL || fields == NULL) {
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"readCount\":0,\"successCount\":0,\"failureCount\":0,\"fields\":[],\"error\":\"no memory\"}",
            (int)(now_ms() - started)
        );
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", err == ESP_OK && success_count > 0);
    cJSON_AddStringToObject(root, "transport", "ble");
    cJSON_AddNumberToObject(root, "durationMs", (int)(now_ms() - started));
    cJSON_AddNumberToObject(root, "candidateCount", (int)candidate_count);
    cJSON_AddNumberToObject(root, "readCount", attempted_count);
    cJSON_AddNumberToObject(root, "successCount", success_count);
    cJSON_AddNumberToObject(root, "failureCount", failure_count);
    cJSON_AddBoolToObject(root, "partial", err == ESP_OK && (failure_count > 0 || stopped_early));
    cJSON_AddBoolToObject(root, "complete", err == ESP_OK && !stopped_early && attempted_count == (int)candidate_count);
    cJSON_AddBoolToObject(root, "stoppedEarly", stopped_early);
    if (stopped_early) {
        cJSON_AddStringToObject(
            root,
            "stopReason",
            ble_worker_job_yielded ? "interactiveControl" : "timeBudgetExceeded"
        );
    } else {
        cJSON_AddNullToObject(root, "stopReason");
    }
    cJSON_AddStringToObject(root, "deviceName", op.target_name);
    cJSON_AddStringToObject(root, "deviceAddress", op.target_address);
    cJSON_AddNumberToObject(root, "rssi", op.rssi);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        cJSON_AddStringToObject(root, "message", ble_state.last_error[0] != '\0' ? ble_state.last_error : esp_err_to_name(err));
    } else if (success_count == 0) {
        cJSON_AddStringToObject(root, "error", "no fields read");
        cJSON_AddStringToObject(root, "message", "BLE full read probe completed without a successful field.");
    } else {
        cJSON_AddNullToObject(root, "error");
        cJSON_AddStringToObject(
            root,
            "message",
            (failure_count > 0 || stopped_early)
                ? "BLE full read probe completed with partial field results."
                : "BLE full read probe completed."
        );
    }

    if (!cJSON_PrintPreallocated(root, dest, dest_len, false)) {
        cJSON_Delete(root);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"readCount\":0,\"successCount\":0,\"failureCount\":0,\"fields\":[],\"error\":\"no memory\"}",
            (int)(now_ms() - started)
        );
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);
    return err;
}

static bool is_notify_candidate(const hub_ble_gatt_characteristic_t *chr)
{
    if (chr == NULL) {
        return false;
    }
    return (chr->properties & (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE)) != 0;
}

static int gatt_notify_candidate_count(const hub_ble_gatt_probe_t *probe)
{
    if (probe == NULL) {
        return 0;
    }
    int count = 0;
    for (int index = 0; index < probe->characteristic_count; index++) {
        if (is_notify_candidate(&probe->characteristics[index])) {
            count++;
        }
    }
    return count;
}

static void add_subscribe_result(cJSON *array, const char *name, uint16_t handle, int rc)
{
    if (array == NULL || name == NULL) {
        return;
    }
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL) {
        return;
    }
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddNumberToObject(entry, "handle", handle);
    cJSON_AddNumberToObject(entry, "rc", rc);
    cJSON_AddBoolToObject(entry, "started", rc == 0);
    cJSON_AddItemToArray(array, entry);
}

static void add_gatt_probe_json(cJSON *root, const hub_ble_gatt_probe_t *probe, esp_err_t err)
{
    if (root == NULL || probe == NULL) {
        return;
    }

    cJSON_AddBoolToObject(root, "gattDiscoveryOk", err == ESP_OK);
    cJSON_AddNumberToObject(root, "gattDiscoveryError", err == ESP_OK ? 0 : (int)err);
    cJSON_AddNumberToObject(root, "gattServiceCount", probe->service_count);
    cJSON_AddNumberToObject(root, "gattCharacteristicCount", probe->characteristic_count);
    cJSON_AddNumberToObject(root, "gattNotifyCandidateCount", gatt_notify_candidate_count(probe));
    cJSON_AddBoolToObject(root, "gattServicesTruncated", probe->services_truncated);
    cJSON_AddBoolToObject(root, "gattCharacteristicsTruncated", probe->characteristics_truncated);

    cJSON *services = cJSON_AddArrayToObject(root, "gattServices");
    if (services != NULL) {
        for (int index = 0; index < probe->service_count; index++) {
            const hub_ble_gatt_service_t *svc = &probe->services[index];
            cJSON *entry = cJSON_CreateObject();
            if (entry == NULL) {
                continue;
            }
            cJSON_AddNumberToObject(entry, "startHandle", svc->start_handle);
            cJSON_AddNumberToObject(entry, "endHandle", svc->end_handle);
            cJSON_AddStringToObject(entry, "uuid", svc->uuid);
            cJSON_AddItemToArray(services, entry);
        }
    }

    cJSON *characteristics = cJSON_AddArrayToObject(root, "gattCharacteristics");
    if (characteristics != NULL) {
        for (int index = 0; index < probe->characteristic_count; index++) {
            const hub_ble_gatt_characteristic_t *chr = &probe->characteristics[index];
            cJSON *entry = cJSON_CreateObject();
            if (entry == NULL) {
                continue;
            }
            cJSON_AddNumberToObject(entry, "defHandle", chr->def_handle);
            cJSON_AddNumberToObject(entry, "valueHandle", chr->val_handle);
            cJSON_AddNumberToObject(entry, "properties", chr->properties);
            cJSON_AddBoolToObject(entry, "notify", (chr->properties & BLE_GATT_CHR_F_NOTIFY) != 0);
            cJSON_AddBoolToObject(entry, "indicate", (chr->properties & BLE_GATT_CHR_F_INDICATE) != 0);
            cJSON_AddStringToObject(entry, "uuid", chr->uuid);
            cJSON_AddItemToArray(characteristics, entry);
        }
    }
}

static bool is_known_notify_handle(const hub_ble_operation_t *op, uint16_t handle)
{
    if (op == NULL || handle == 0) {
        return false;
    }
    return handle == op->read_handle || handle == op->auth_handle || handle == op->write_handle;
}

static void subscribe_gatt_notify_candidates(const hub_ble_operation_t *op, const hub_ble_gatt_probe_t *probe, cJSON *subscribe_results)
{
    if (op == NULL || probe == NULL || subscribe_results == NULL) {
        return;
    }

    for (int index = 0; index < probe->characteristic_count; index++) {
        const hub_ble_gatt_characteristic_t *chr = &probe->characteristics[index];
        if (!is_notify_candidate(chr) || is_known_notify_handle(op, chr->val_handle)) {
            continue;
        }

        char name[40] = {0};
        snprintf(name, sizeof(name), "gatt-%u", (unsigned)chr->val_handle);
        int rc = ble_gattc_register_for_notification(op->conn_handle, chr->val_handle);
        add_subscribe_result(subscribe_results, name, chr->val_handle, rc);
        vTaskDelay(pdMS_TO_TICKS(HUB_BLE_NOTIFY_PROBE_SUBSCRIBE_SETTLE_MS));
    }
}

static bool gatt_chr_uuid_starts(const hub_ble_gatt_characteristic_t *chr, const char *prefix)
{
    return chr != NULL && starts_with_fold(chr->uuid, prefix);
}

static bool is_gatt_read_probe_candidate(const hub_ble_gatt_characteristic_t *chr, const char *profile)
{
    if (chr == NULL || (chr->properties & BLE_GATT_CHR_F_READ) == 0) {
        return false;
    }
    if (strcmp(chr->uuid, "0x2a00") == 0
        || strcmp(chr->uuid, "0x2a01") == 0
        || strcmp(chr->uuid, "0x2a05") == 0
        || strcmp(chr->uuid, "0x2a24") == 0
        || strcmp(chr->uuid, "0x2a25") == 0
        || strcmp(chr->uuid, "0x2a26") == 0
        || strcmp(chr->uuid, "0x2a27") == 0
        || strcmp(chr->uuid, "0x2a28") == 0
        || strcmp(chr->uuid, "0x2a29") == 0
        || strcmp(chr->uuid, "0x2b29") == 0
        || strcmp(chr->uuid, "0x2b3a") == 0) {
        return false;
    }
    if (profile != NULL && equal_fold(profile, "state")) {
        return gatt_chr_uuid_starts(chr, "050b7847") || gatt_chr_uuid_starts(chr, "0a0b7847");
    }
    if (profile != NULL && equal_fold(profile, "flags")) {
        return gatt_chr_uuid_starts(chr, "030b7847")
            || gatt_chr_uuid_starts(chr, "040b7847")
            || gatt_chr_uuid_starts(chr, "090b7847")
            || gatt_chr_uuid_starts(chr, "0b0b7847")
            || gatt_chr_uuid_starts(chr, "0d0b7847");
    }
    return true;
}

static int collect_gatt_read_probe_candidates(
    const hub_ble_gatt_probe_t *probe,
    const char *profile,
    const hub_ble_gatt_characteristic_t **candidates,
    int max_candidates
)
{
    if (probe == NULL || candidates == NULL || max_candidates <= 0) {
        return 0;
    }
    int count = 0;
    for (int index = 0; index < probe->characteristic_count && count < max_candidates; index++) {
        const hub_ble_gatt_characteristic_t *chr = &probe->characteristics[index];
        if (is_gatt_read_probe_candidate(chr, profile)) {
            candidates[count++] = chr;
        }
    }
    return count;
}

static bool update_gatt_probe_checksum(
    uint16_t handle,
    uint32_t checksum,
    uint16_t *handles,
    uint32_t *checksums,
    int *count,
    int max_count
)
{
    if (handles == NULL || checksums == NULL || count == NULL || max_count <= 0) {
        return true;
    }
    for (int index = 0; index < *count; index++) {
        if (handles[index] != handle) {
            continue;
        }
        bool changed = checksums[index] != checksum;
        checksums[index] = checksum;
        return changed;
    }
    if (*count < max_count) {
        handles[*count] = handle;
        checksums[*count] = checksum;
        (*count)++;
    }
    return true;
}

static bool gatt_probe_response_is_printable(const char *bytes, size_t len)
{
    if (bytes == NULL || len == 0 || len > HUB_BLE_GATT_READ_PROBE_TEXT_BYTES) {
        return false;
    }
    for (size_t index = 0; index < len; index++) {
        unsigned char value = (unsigned char)bytes[index];
        if (value == '\0' || value < 0x20 || value > 0x7e) {
            return false;
        }
    }
    return true;
}

esp_err_t hub_ble_gatt_read_probe_json(const char *profile, int duration_ms, int interval_ms, char *dest, size_t dest_len)
{
    int64_t started = now_ms();
    if (dest == NULL || dest_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dest[0] = '\0';

    if (duration_ms <= 0) {
        duration_ms = HUB_BLE_GATT_READ_PROBE_DEFAULT_MS;
    }
    if (duration_ms < HUB_BLE_GATT_READ_PROBE_MIN_MS) {
        duration_ms = HUB_BLE_GATT_READ_PROBE_MIN_MS;
    }
    if (duration_ms > HUB_BLE_GATT_READ_PROBE_MAX_MS) {
        duration_ms = HUB_BLE_GATT_READ_PROBE_MAX_MS;
    }
    if (interval_ms < HUB_BLE_PROTOCOL_PROBE_MIN_INTERVAL_MS) {
        interval_ms = HUB_BLE_GATT_READ_PROBE_DEFAULT_INTERVAL_MS;
    }
    if (interval_ms > HUB_BLE_PROTOCOL_PROBE_MAX_INTERVAL_MS) {
        interval_ms = HUB_BLE_PROTOCOL_PROBE_MAX_INTERVAL_MS;
    }

    const char *profile_name = "all";
    if (profile != NULL && equal_fold(profile, "state")) {
        profile_name = "state";
    } else if (profile != NULL && equal_fold(profile, "flags")) {
        profile_name = "flags";
    }

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_initialized) {
        set_last_error("BLE stack is not initialized.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_synced && !wait_sem(ble_sync_sem, 5000)) {
        set_last_error("BLE host sync timeout.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_GATT_READ_PROBE_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (ble_diagnostic_should_yield()) {
        xSemaphoreGive(ble_op_mutex);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"interactive control pending\"}",
            profile_name,
            (int)(now_ms() - started)
        );
        return ESP_ERR_INVALID_STATE;
    }

    ble_state.configured = true;
    ble_state.refresh_pending = true;
    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;

    cJSON *root = cJSON_CreateObject();
    cJSON *samples = root != NULL ? cJSON_AddArrayToObject(root, "samples") : NULL;
    hub_ble_gatt_probe_t gatt_probe = {0};
    const hub_ble_gatt_characteristic_t *read_candidates[HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS] = {0};
    esp_err_t err = ESP_ERR_NO_MEM;
    esp_err_t gatt_discovery_err = ESP_ERR_INVALID_STATE;
    int candidate_count = 0;
    int64_t listen_started = now_ms();
    int sample_count = 0;
    int success_count = 0;
    int failure_count = 0;
    bool sample_limit_reached = false;
    bool connection_unhealthy = false;
    int next_candidate = 0;
    uint16_t checksum_handles[HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS] = {0};
    uint32_t checksum_values[HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS] = {0};
    int checksum_count = 0;

    if (root != NULL && samples != NULL) {
        err = restore_cached_session(&op, false) ? ESP_OK : prepare_authenticated_connection(&op, false);
    }

    if (err == ESP_OK) {
        set_status("discovering");
        gatt_discovery_err = discover_all_gatt_for_probe(&op, &gatt_probe);
        if (gatt_discovery_err == ESP_OK) {
            candidate_count = collect_gatt_read_probe_candidates(
                &gatt_probe,
                profile_name,
                read_candidates,
                HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS
            );
        } else {
            err = gatt_discovery_err;
        }
    }

    if (err == ESP_OK && candidate_count == 0) {
        err = ESP_ERR_NOT_FOUND;
        set_last_error("No readable custom BLE characteristics found.");
    }

    if (err == ESP_OK) {
        set_status("gattReadProbe");
        listen_started = now_ms();
        while ((int)(now_ms() - listen_started) < duration_ms) {
            if (ble_diagnostic_should_yield()) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            if (sample_count >= HUB_BLE_GATT_READ_PROBE_MAX_SAMPLES) {
                sample_limit_reached = true;
                break;
            }

            const hub_ble_gatt_characteristic_t *candidate = read_candidates[next_candidate];
            next_candidate = (next_candidate + 1) % candidate_count;

            int64_t sample_started = now_ms();
            char label[32] = {0};
            snprintf(label, sizeof(label), "handle-%u", (unsigned)candidate->val_handle);
            esp_err_t read_err = read_gatt_raw_once_with_timeout(
                &op,
                candidate->val_handle,
                label,
                HUB_BLE_FULL_READ_GATT_TIMEOUT_MS
            );
            int sample_duration_ms = (int)(now_ms() - sample_started);

            cJSON *sample = cJSON_CreateObject();
            if (sample != NULL) {
                cJSON_AddNumberToObject(sample, "elapsedMs", (int)(sample_started - listen_started));
                cJSON_AddNumberToObject(sample, "handle", candidate->val_handle);
                cJSON_AddStringToObject(sample, "uuid", candidate->uuid);
                cJSON_AddBoolToObject(sample, "ok", read_err == ESP_OK);
                cJSON_AddNumberToObject(sample, "durationMs", sample_duration_ms);
                if (read_err == ESP_OK) {
                    char hex[HUB_BLE_GATT_READ_PROBE_HEX_CHARS + 1] = {0};
                    size_t copy_len = op.read_response_len;
                    if (copy_len > HUB_BLE_GATT_READ_PROBE_HEX_CHARS / 2) {
                        copy_len = HUB_BLE_GATT_READ_PROBE_HEX_CHARS / 2;
                    }
                    bytes_to_hex((const uint8_t *)op.read_response, copy_len, hex, sizeof(hex));
                    uint32_t checksum = fnv1a32((const uint8_t *)op.read_response, op.read_response_len);
                    char checksum_text[9] = {0};
                    snprintf(checksum_text, sizeof(checksum_text), "%08lx", (unsigned long)checksum);
                    bool changed = update_gatt_probe_checksum(
                        candidate->val_handle,
                        checksum,
                        checksum_handles,
                        checksum_values,
                        &checksum_count,
                        HUB_BLE_GATT_PROBE_MAX_CHARACTERISTICS
                    );
                    cJSON_AddNumberToObject(sample, "length", (int)op.read_response_len);
                    cJSON_AddBoolToObject(sample, "truncated", op.read_response_len > copy_len);
                    cJSON_AddStringToObject(sample, "checksum", checksum_text);
                    cJSON_AddBoolToObject(sample, "changed", changed);
                    if (changed) {
                        cJSON_AddStringToObject(sample, "hex", hex);
                    }
                    if (changed && gatt_probe_response_is_printable(op.read_response, op.read_response_len)) {
                        cJSON_AddStringToObject(sample, "text", op.read_response);
                    }
                } else {
                    cJSON_AddStringToObject(sample, "error", esp_err_to_name(read_err));
                    cJSON_AddStringToObject(sample, "message", ble_state.last_error[0] != '\0' ? ble_state.last_error : esp_err_to_name(read_err));
                }
                cJSON_AddItemToArray(samples, sample);
            }

            sample_count++;
            if (read_err == ESP_OK) {
                success_count++;
            } else {
                failure_count++;
                if (read_err == ESP_ERR_TIMEOUT) {
                    connection_unhealthy = true;
                }
            }

            int delay_ms = interval_ms - sample_duration_ms;
            if (delay_ms > 0 && (int)(now_ms() - listen_started) + delay_ms < duration_ms) {
                if (!ble_diagnostic_delay(delay_ms)) {
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }
            }
        }
    }

    finish_active_connection(&op, err == ESP_OK && success_count > 0 && !connection_unhealthy);
    ble_state.refresh_pending = false;
    if (err == ESP_OK && success_count > 0) {
        persist_discovered_ble_address(&op);
        set_status("cached");
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        ble_state.last_error[0] = '\0';
    } else if (ble_worker_job_yielded) {
        set_status(ble_state.has_cached_state ? "cached" : "idle");
    } else if (err != ESP_OK) {
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
        ble_state.consecutive_failures++;
        ble_state.manual_intervention_recommended = ble_state.has_cached_state;
    }
    xSemaphoreGive(ble_op_mutex);

    if (root == NULL || samples == NULL) {
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"no memory\"}",
            profile_name,
            (int)(now_ms() - started)
        );
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", err == ESP_OK && success_count > 0);
    cJSON_AddStringToObject(root, "transport", "ble");
    cJSON_AddStringToObject(root, "profile", profile_name);
    cJSON_AddNumberToObject(root, "durationMs", (int)(now_ms() - started));
    cJSON_AddNumberToObject(root, "requestedDurationMs", duration_ms);
    cJSON_AddNumberToObject(root, "listenElapsedMs", (int)(now_ms() - listen_started));
    cJSON_AddNumberToObject(root, "intervalMs", interval_ms);
    cJSON_AddNumberToObject(root, "candidateCount", candidate_count);
    cJSON_AddNumberToObject(root, "sampleCount", sample_count);
    cJSON_AddNumberToObject(root, "successCount", success_count);
    cJSON_AddNumberToObject(root, "failureCount", failure_count);
    cJSON_AddNumberToObject(root, "sampleLimit", HUB_BLE_GATT_READ_PROBE_MAX_SAMPLES);
    cJSON_AddBoolToObject(root, "sampleLimitReached", sample_limit_reached);
    cJSON_AddStringToObject(root, "deviceName", op.target_name);
    cJSON_AddStringToObject(root, "deviceAddress", op.target_address);
    cJSON_AddNumberToObject(root, "rssi", op.rssi);
    cJSON_AddBoolToObject(root, "gattDiscoveryOk", gatt_discovery_err == ESP_OK);
    cJSON_AddNumberToObject(root, "gattCharacteristicCount", gatt_probe.characteristic_count);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        cJSON_AddStringToObject(root, "message", ble_state.last_error[0] != '\0' ? ble_state.last_error : esp_err_to_name(err));
    } else if (success_count == 0) {
        cJSON_AddStringToObject(root, "error", "no handles read");
        cJSON_AddStringToObject(root, "message", "BLE GATT read probe completed without a successful characteristic read.");
    } else {
        cJSON_AddNullToObject(root, "error");
        cJSON_AddStringToObject(root, "message", "BLE GATT read probe completed.");
    }

    if (!cJSON_PrintPreallocated(root, dest, dest_len, false)) {
        cJSON_Delete(root);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"no memory\"}",
            profile_name,
            (int)(now_ms() - started)
        );
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);
    if (err == ESP_OK && success_count == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

esp_err_t hub_ble_notify_probe_json(int duration_ms, char *dest, size_t dest_len)
{
    int64_t started = now_ms();
    if (dest == NULL || dest_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dest[0] = '\0';

    if (duration_ms <= 0) {
        duration_ms = HUB_BLE_NOTIFY_PROBE_DEFAULT_MS;
    }
    if (duration_ms < HUB_BLE_NOTIFY_PROBE_MIN_MS) {
        duration_ms = HUB_BLE_NOTIFY_PROBE_MIN_MS;
    }
    if (duration_ms > HUB_BLE_NOTIFY_PROBE_MAX_MS) {
        duration_ms = HUB_BLE_NOTIFY_PROBE_MAX_MS;
    }

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"listenMs\":%d,\"notificationCount\":0,\"indicationCount\":0,\"samples\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            duration_ms,
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_initialized) {
        set_last_error("BLE stack is not initialized.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"listenMs\":%d,\"notificationCount\":0,\"indicationCount\":0,\"samples\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            duration_ms,
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_synced && !wait_sem(ble_sync_sem, 5000)) {
        set_last_error("BLE host sync timeout.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"listenMs\":%d,\"notificationCount\":0,\"indicationCount\":0,\"samples\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            duration_ms,
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_NOTIFY_PROBE_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"listenMs\":%d,\"notificationCount\":0,\"indicationCount\":0,\"samples\":[],\"error\":\"%s\"}",
            (int)(now_ms() - started),
            duration_ms,
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (ble_diagnostic_should_yield()) {
        xSemaphoreGive(ble_op_mutex);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"listenMs\":%d,\"notificationCount\":0,\"indicationCount\":0,\"samples\":[],\"error\":\"interactive control pending\"}",
            (int)(now_ms() - started),
            duration_ms
        );
        return ESP_ERR_INVALID_STATE;
    }

    ble_state.configured = true;
    ble_state.refresh_pending = true;
    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;

    cJSON *root = cJSON_CreateObject();
    cJSON *subscribe_results = root != NULL ? cJSON_AddArrayToObject(root, "subscribeResults") : NULL;
    cJSON *samples = root != NULL ? cJSON_AddArrayToObject(root, "samples") : NULL;
    esp_err_t err = ESP_ERR_NO_MEM;
    int read_subscribe_rc = BLE_HS_EAPP;
    int auth_subscribe_rc = BLE_HS_EAPP;
    int write_subscribe_rc = BLE_HS_EAPP;
    hub_ble_gatt_probe_t gatt_probe = {0};
    esp_err_t gatt_discovery_err = ESP_ERR_INVALID_STATE;

    if (root != NULL && subscribe_results != NULL && samples != NULL) {
        err = restore_cached_session(&op, true) ? ESP_OK : prepare_authenticated_connection(&op, true);
    }

    if (err == ESP_OK) {
        set_status("discovering");
        gatt_discovery_err = discover_all_gatt_for_probe(&op, &gatt_probe);

        memset(&notify_probe_state, 0, sizeof(notify_probe_state));
        notify_probe_state.active = true;
        notify_probe_state.conn_handle = op.conn_handle;
        notify_probe_state.started_ms = now_ms();

        set_status("listening");
        read_subscribe_rc = ble_gattc_register_for_notification(op.conn_handle, op.read_handle);
        if (!ble_diagnostic_delay(HUB_BLE_NOTIFY_PROBE_SUBSCRIBE_SETTLE_MS)) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            auth_subscribe_rc = ble_gattc_register_for_notification(op.conn_handle, op.auth_handle);
            if (!ble_diagnostic_delay(HUB_BLE_NOTIFY_PROBE_SUBSCRIBE_SETTLE_MS)) {
                err = ESP_ERR_INVALID_STATE;
            }
        }
        if (err == ESP_OK) {
            write_subscribe_rc = op.write_handle != 0
                ? ble_gattc_register_for_notification(op.conn_handle, op.write_handle)
                : BLE_HS_ENOTSUP;
            if (!ble_diagnostic_delay(HUB_BLE_NOTIFY_PROBE_SUBSCRIBE_SETTLE_MS)) {
                err = ESP_ERR_INVALID_STATE;
            }
        }
        if (err == ESP_OK && gatt_discovery_err == ESP_OK) {
            subscribe_gatt_notify_candidates(&op, &gatt_probe, subscribe_results);
        }

        while (err == ESP_OK && (int)(now_ms() - notify_probe_state.started_ms) < duration_ms) {
            if (ble_diagnostic_should_yield()) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        notify_probe_state.active = false;
    }

    int notification_count = notify_probe_state.notification_count;
    int indication_count = notify_probe_state.indication_count;
    int sample_count = notify_probe_state.sample_count;

    finish_active_connection(&op, false);
    ble_state.refresh_pending = false;
    if (err == ESP_OK) {
        persist_discovered_ble_address(&op);
        set_status("cached");
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        ble_state.last_error[0] = '\0';
    } else if (ble_worker_job_yielded) {
        set_status(ble_state.has_cached_state ? "cached" : "idle");
    } else {
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
        ble_state.consecutive_failures++;
        ble_state.manual_intervention_recommended = ble_state.has_cached_state;
    }
    xSemaphoreGive(ble_op_mutex);

    if (root == NULL || subscribe_results == NULL || samples == NULL) {
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"listenMs\":%d,\"notificationCount\":0,\"indicationCount\":0,\"samples\":[],\"error\":\"no memory\"}",
            (int)(now_ms() - started),
            duration_ms
        );
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    const struct {
        const char *name;
        uint16_t handle;
        int rc;
    } subscriptions[] = {
        {"read", op.read_handle, read_subscribe_rc},
        {"auth", op.auth_handle, auth_subscribe_rc},
        {"write", op.write_handle, write_subscribe_rc},
    };
    for (size_t index = 0; index < sizeof(subscriptions) / sizeof(subscriptions[0]); index++) {
        add_subscribe_result(subscribe_results, subscriptions[index].name, subscriptions[index].handle, subscriptions[index].rc);
    }

    for (int index = 0; index < sample_count; index++) {
        const hub_ble_notify_sample_t *sample = &notify_probe_state.samples[index];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(item, "elapsedMs", sample->elapsed_ms);
        cJSON_AddNumberToObject(item, "attrHandle", sample->attr_handle);
        cJSON_AddBoolToObject(item, "indication", sample->indication);
        cJSON_AddNumberToObject(item, "length", sample->length);
        cJSON_AddBoolToObject(item, "truncated", sample->truncated);
        cJSON_AddStringToObject(item, "hex", sample->hex);
        cJSON_AddItemToArray(samples, item);
    }

    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "transport", "ble");
    cJSON_AddNumberToObject(root, "durationMs", (int)(now_ms() - started));
    cJSON_AddNumberToObject(root, "listenMs", duration_ms);
    cJSON_AddStringToObject(root, "deviceName", op.target_name);
    cJSON_AddStringToObject(root, "deviceAddress", op.target_address);
    cJSON_AddNumberToObject(root, "rssi", op.rssi);
    add_gatt_probe_json(root, &gatt_probe, gatt_discovery_err);
    cJSON_AddNumberToObject(root, "notificationCount", notification_count);
    cJSON_AddNumberToObject(root, "indicationCount", indication_count);
    cJSON_AddNumberToObject(root, "sampleCount", sample_count);
    cJSON_AddNumberToObject(root, "sampleLimit", HUB_BLE_NOTIFY_PROBE_MAX_SAMPLES);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        cJSON_AddStringToObject(root, "message", ble_state.last_error[0] != '\0' ? ble_state.last_error : esp_err_to_name(err));
    } else {
        cJSON_AddNullToObject(root, "error");
        cJSON_AddStringToObject(
            root,
            "message",
            notification_count + indication_count > 0
                ? "BLE notify probe received notification data."
                : "BLE notify probe completed without notification data."
        );
    }

    if (!cJSON_PrintPreallocated(root, dest, dest_len, false)) {
        cJSON_Delete(root);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"durationMs\":%d,\"listenMs\":%d,\"notificationCount\":0,\"indicationCount\":0,\"samples\":[],\"error\":\"no memory\"}",
            (int)(now_ms() - started),
            duration_ms
        );
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t hub_ble_protocol_probe_json(const char *profile, int duration_ms, int interval_ms, char *dest, size_t dest_len)
{
    static const hub_ble_protocol_candidate_t mode_candidates[] = {
        {"machineMode", "machineMode"},
    };
    static const hub_ble_protocol_candidate_t fast_candidates[] = {
        {"machineMode", "machineMode"},
        {"shotTimer", "shotTimer"},
        {"shotTime", "shotTime"},
        {"extractionSeconds", "extractionSeconds"},
        {"extractionTime", "extractionTime"},
        {"brewTimer", "brewTimer"},
        {"brewTime", "brewTime"},
        {"groupState", "groupState"},
        {"brewState", "brewState"},
        {"shotState", "shotState"},
        {"pumpState", "pumpState"},
        {"flowMeter", "flowMeter"},
        {"brewWeight", "brewWeight"},
        {"scaleWeight", "scaleWeight"},
    };
    static const hub_ble_protocol_candidate_t timer_candidates[] = {
        {"shotTimer", "shotTimer"},
        {"shotTime", "shotTime"},
        {"extractionSeconds", "extractionSeconds"},
        {"extractionTime", "extractionTime"},
        {"brewTimer", "brewTimer"},
        {"brewTime", "brewTime"},
        {"currentShot", "currentShot"},
        {"machineMode", "machineMode"},
    };
    static const hub_ble_protocol_candidate_t broad_candidates[] = {
        {"machineMode", "machineMode"},
        {"shotTimer", "shotTimer"},
        {"shotTime", "shotTime"},
        {"extractionSeconds", "extractionSeconds"},
        {"extractionTime", "extractionTime"},
        {"brewTimer", "brewTimer"},
        {"brewTime", "brewTime"},
        {"groupState", "groupState"},
        {"brewState", "brewState"},
        {"shotState", "shotState"},
        {"shotStatus", "shotStatus"},
        {"brewStatus", "brewStatus"},
        {"extraction", "extraction"},
        {"extractionStatus", "extractionStatus"},
        {"pumpState", "pumpState"},
        {"flowMeter", "flowMeter"},
        {"currentShot", "currentShot"},
        {"brewWeight", "brewWeight"},
        {"scaleWeight", "scaleWeight"},
        {"targetWeight", "targetWeight"},
        {"scale", "scale"},
        {"scaleConnected", "scaleConnected"},
        {"scaleBatteryLevel", "scaleBatteryLevel"},
        {"scaleName", "scaleName"},
        {"status", "status"},
        {"machineStatus", "machineStatus"},
        {"json-name-shotTimer", "{\"name\":\"shotTimer\"}"},
        {"json-name-extractionSeconds", "{\"name\":\"extractionSeconds\"}"},
        {"json-name-machineMode", "{\"name\":\"machineMode\"}"},
        {"json-command-shotTimer", "{\"command\":\"shotTimer\"}"},
        {"json-command-machineStatus", "{\"command\":\"machineStatus\"}"},
        {"json-command-brewStatus", "{\"command\":\"brewStatus\"}"},
    };

    int64_t started = now_ms();
    if (dest == NULL || dest_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dest[0] = '\0';

    if (duration_ms <= 0) {
        duration_ms = HUB_BLE_PROTOCOL_PROBE_DEFAULT_MS;
    }
    if (duration_ms < HUB_BLE_PROTOCOL_PROBE_MIN_MS) {
        duration_ms = HUB_BLE_PROTOCOL_PROBE_MIN_MS;
    }
    if (duration_ms > HUB_BLE_PROTOCOL_PROBE_MAX_MS) {
        duration_ms = HUB_BLE_PROTOCOL_PROBE_MAX_MS;
    }
    if (interval_ms < HUB_BLE_PROTOCOL_PROBE_MIN_INTERVAL_MS) {
        interval_ms = HUB_BLE_PROTOCOL_PROBE_DEFAULT_INTERVAL_MS;
    }
    if (interval_ms > HUB_BLE_PROTOCOL_PROBE_MAX_INTERVAL_MS) {
        interval_ms = HUB_BLE_PROTOCOL_PROBE_MAX_INTERVAL_MS;
    }

    const char *profile_name = "fast";
    const hub_ble_protocol_candidate_t *candidates = fast_candidates;
    size_t candidate_count = sizeof(fast_candidates) / sizeof(fast_candidates[0]);
    if (profile != NULL && equal_fold(profile, "mode")) {
        profile_name = "mode";
        candidates = mode_candidates;
        candidate_count = sizeof(mode_candidates) / sizeof(mode_candidates[0]);
    } else if (profile != NULL && equal_fold(profile, "timer")) {
        profile_name = "timer";
        candidates = timer_candidates;
        candidate_count = sizeof(timer_candidates) / sizeof(timer_candidates[0]);
    } else if (profile != NULL && equal_fold(profile, "broad")) {
        profile_name = "broad";
        candidates = broad_candidates;
        candidate_count = sizeof(broad_candidates) / sizeof(broad_candidates[0]);
    }

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_initialized) {
        set_last_error("BLE stack is not initialized.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_synced && !wait_sem(ble_sync_sem, 5000)) {
        set_last_error("BLE host sync timeout.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_PROTOCOL_PROBE_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"%s\"}",
            profile_name,
            (int)(now_ms() - started),
            ble_state.last_error
        );
        return ESP_ERR_TIMEOUT;
    }
    if (ble_diagnostic_should_yield()) {
        xSemaphoreGive(ble_op_mutex);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"interactive control pending\"}",
            profile_name,
            (int)(now_ms() - started)
        );
        return ESP_ERR_INVALID_STATE;
    }

    ble_state.configured = true;
    ble_state.refresh_pending = true;
    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;

    cJSON *root = cJSON_CreateObject();
    cJSON *samples = root != NULL ? cJSON_AddArrayToObject(root, "samples") : NULL;
    esp_err_t err = ESP_ERR_NO_MEM;
    int64_t listen_started = now_ms();
    int sample_count = 0;
    int success_count = 0;
    int failure_count = 0;
    int useful_count = 0;
    bool sample_limit_reached = false;
    bool connection_unhealthy = false;
    size_t next_candidate = 0;

    if (root != NULL && samples != NULL) {
        err = restore_cached_session(&op, false) ? ESP_OK : prepare_authenticated_connection(&op, false);
    }

    if (err == ESP_OK) {
        set_status("protocolProbe");
        listen_started = now_ms();
        while ((int)(now_ms() - listen_started) < duration_ms) {
            if (ble_diagnostic_should_yield()) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            if (sample_count >= HUB_BLE_PROTOCOL_PROBE_MAX_SAMPLES) {
                sample_limit_reached = true;
                break;
            }

            const hub_ble_protocol_candidate_t *candidate = &candidates[next_candidate];
            next_candidate = (next_candidate + 1) % candidate_count;

            int64_t sample_started = now_ms();
            char raw[HUB_BLE_PROTOCOL_PROBE_RAW_BYTES] = {0};
            esp_err_t read_err = read_payload_probe(&op, candidate->payload, raw, sizeof(raw));
            int sample_duration_ms = (int)(now_ms() - sample_started);
            bool echoed = read_err == ESP_OK && raw_echoes_payload(candidate->payload, raw);

            cJSON *sample = cJSON_CreateObject();
            if (sample != NULL) {
                cJSON_AddNumberToObject(sample, "elapsedMs", (int)(sample_started - listen_started));
                cJSON_AddStringToObject(sample, "candidate", candidate->name);
                cJSON_AddBoolToObject(sample, "ok", read_err == ESP_OK);
                cJSON_AddNumberToObject(sample, "durationMs", sample_duration_ms);
                if (read_err == ESP_OK) {
                    cJSON_AddStringToObject(sample, "raw", raw);
                    cJSON_AddNumberToObject(sample, "responseBytes", (int)op.read_response_len);
                    cJSON_AddBoolToObject(sample, "truncated", op.read_response_len >= sizeof(raw) - 1);
                    cJSON_AddBoolToObject(sample, "echoed", echoed);
                } else {
                    cJSON_AddStringToObject(sample, "error", esp_err_to_name(read_err));
                    cJSON_AddStringToObject(sample, "message", ble_state.last_error[0] != '\0' ? ble_state.last_error : esp_err_to_name(read_err));
                }
                cJSON_AddItemToArray(samples, sample);
            }

            sample_count++;
            if (read_err == ESP_OK) {
                success_count++;
                if (!echoed) {
                    useful_count++;
                }
            } else {
                failure_count++;
                if (read_err == ESP_ERR_TIMEOUT) {
                    connection_unhealthy = true;
                }
            }

            int delay_ms = interval_ms - sample_duration_ms;
            if (delay_ms > 0 && (int)(now_ms() - listen_started) + delay_ms < duration_ms) {
                if (!ble_diagnostic_delay(delay_ms)) {
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }
            }
        }
    }

    finish_active_connection(&op, err == ESP_OK && success_count > 0 && !connection_unhealthy);
    ble_state.refresh_pending = false;
    if (err == ESP_OK && success_count > 0) {
        persist_discovered_ble_address(&op);
        set_status("cached");
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        ble_state.last_error[0] = '\0';
    } else if (ble_worker_job_yielded) {
        set_status(ble_state.has_cached_state ? "cached" : "idle");
    } else if (err != ESP_OK) {
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
        ble_state.consecutive_failures++;
        ble_state.manual_intervention_recommended = ble_state.has_cached_state;
    }
    xSemaphoreGive(ble_op_mutex);

    if (root == NULL || samples == NULL) {
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"no memory\"}",
            profile_name,
            (int)(now_ms() - started)
        );
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", err == ESP_OK && success_count > 0);
    cJSON_AddStringToObject(root, "transport", "ble");
    cJSON_AddStringToObject(root, "profile", profile_name);
    cJSON_AddNumberToObject(root, "durationMs", (int)(now_ms() - started));
    cJSON_AddNumberToObject(root, "requestedDurationMs", duration_ms);
    cJSON_AddNumberToObject(root, "listenElapsedMs", (int)(now_ms() - listen_started));
    cJSON_AddNumberToObject(root, "intervalMs", interval_ms);
    cJSON_AddNumberToObject(root, "candidateCount", (int)candidate_count);
    cJSON_AddNumberToObject(root, "sampleCount", sample_count);
    cJSON_AddNumberToObject(root, "successCount", success_count);
    cJSON_AddNumberToObject(root, "failureCount", failure_count);
    cJSON_AddNumberToObject(root, "usefulCount", useful_count);
    cJSON_AddNumberToObject(root, "sampleLimit", HUB_BLE_PROTOCOL_PROBE_MAX_SAMPLES);
    cJSON_AddBoolToObject(root, "sampleLimitReached", sample_limit_reached);
    cJSON_AddStringToObject(root, "deviceName", op.target_name);
    cJSON_AddStringToObject(root, "deviceAddress", op.target_address);
    cJSON_AddNumberToObject(root, "rssi", op.rssi);

    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        cJSON_AddStringToObject(root, "message", ble_state.last_error[0] != '\0' ? ble_state.last_error : esp_err_to_name(err));
    } else if (success_count == 0) {
        cJSON_AddStringToObject(root, "error", "no fields read");
        cJSON_AddStringToObject(root, "message", "BLE protocol probe completed without a successful field.");
    } else {
        cJSON_AddNullToObject(root, "error");
        cJSON_AddStringToObject(
            root,
            "message",
            useful_count > 0
                ? "BLE protocol probe found at least one non-echo response."
                : "BLE protocol probe completed; all successful responses were echoes."
        );
    }

    if (!cJSON_PrintPreallocated(root, dest, dest_len, false)) {
        cJSON_Delete(root);
        snprintf(
            dest,
            dest_len,
            "{\"ok\":false,\"transport\":\"ble\",\"profile\":\"%s\",\"durationMs\":%d,\"sampleCount\":0,\"successCount\":0,\"failureCount\":0,\"samples\":[],\"error\":\"no memory\"}",
            profile_name,
            (int)(now_ms() - started)
        );
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);
    if (err == ESP_OK && success_count == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

esp_err_t hub_ble_validate_noop_write(bool confirm, bool include_power, hub_ble_write_validation_result_t *result)
{
    int64_t started = now_ms();
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    if (!confirm) {
        set_last_error("BLE no-op write validation requires confirm=true.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (include_power) {
        set_last_error("Power write validation is not a no-op because current power state is unknown.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_INVALID_ARG;
    }

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (!ble_initialized) {
        set_last_error("BLE stack is not initialized.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (!ble_synced && !wait_sem(ble_sync_sem, 5000)) {
        set_last_error("BLE host sync timeout.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_OPERATION_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        if (result != NULL) {
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
        return ESP_ERR_TIMEOUT;
    }

    ble_state.configured = true;
    ble_state.refresh_pending = true;
    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;
    esp_err_t err = run_ble_noop_write(&op, result);
    ble_state.refresh_pending = false;
    if (err == ESP_OK) {
        set_status("cached");
        ble_state.reads_validated = true;
        ble_state.writes_validated = true;
        ble_state.consecutive_failures = 0;
        ble_state.manual_intervention_recommended = false;
        ble_state.recovery_hint[0] = '\0';
        ble_state.last_error[0] = '\0';
        if (result != NULL) {
            result->ok = true;
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), "BLE no-op write validation completed.");
        }
    } else {
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
        ble_state.writes_validated = false;
        ble_state.consecutive_failures++;
        ble_state.manual_intervention_recommended = true;
        copy_safe(ble_state.recovery_hint, sizeof(ble_state.recovery_hint), "restart_machine");
        if (result != NULL) {
            result->ok = false;
            result->duration_ms = (int)(now_ms() - started);
            copy_safe(result->message, sizeof(result->message), ble_state.last_error);
        }
    }

    xSemaphoreGive(ble_op_mutex);
    return err;
}

esp_err_t hub_ble_scan_diagnostics(int duration_ms, char *body, size_t body_len)
{
    int64_t started = now_ms();
    if (body == NULL || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms < 1000 || duration_ms > 30000) {
        set_last_error("BLE scan duration must be between 1 and 30 seconds.");
        return ESP_ERR_INVALID_ARG;
    }

    hub_machine_selection_t selection = {0};
    if (!load_selection(&selection)) {
        set_status("unconfigured");
        set_last_error("BLE token or machine serial is missing.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_initialized) {
        set_last_error("BLE stack is not initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ble_synced && !wait_sem(ble_sync_sem, 5000)) {
        set_last_error("BLE host sync timeout.");
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(ble_op_mutex, pdMS_TO_TICKS(HUB_BLE_OPERATION_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        set_last_error("BLE operation already running.");
        return ESP_ERR_TIMEOUT;
    }
    if (ble_diagnostic_should_yield()) {
        xSemaphoreGive(ble_op_mutex);
        snprintf(
            body,
            body_len,
            "{\"ok\":false,\"durationMs\":%d,\"targetFound\":false,\"error\":\"interactive control pending\"}",
            (int)(now_ms() - started)
        );
        return ESP_ERR_INVALID_STATE;
    }
    if (ble_scan_teardown_pending) {
        xSemaphoreGive(ble_op_mutex);
        set_last_error("Previous BLE scan teardown is still pending; use Cloud fallback.");
        snprintf(
            body,
            body_len,
            "{\"ok\":false,\"durationMs\":%d,\"targetFound\":false,\"error\":\"scan teardown pending\"}",
            (int)(now_ms() - started)
        );
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        xSemaphoreGive(ble_op_mutex);
        set_last_error("BLE host address is not ready.");
        return ESP_FAIL;
    }

    hub_ble_operation_t op = {0};
    op.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    op.selection = selection;
    active_op = &op;
    ble_state.configured = true;
    ble_state.refresh_pending = true;
    esp_err_t scan_err = scan_for_target(&op, own_addr_type, duration_ms);
    active_op = NULL;
    ble_state.refresh_pending = false;
    if (ble_worker_job_yielded) {
        set_status(ble_state.has_cached_state ? "cached" : "idle");
    } else if (op.target_found) {
        set_status("idle");
        ble_state.last_error[0] = '\0';
    } else {
        set_status(ble_state.has_cached_state ? "backoff" : "disconnected");
    }
    xSemaphoreGive(ble_op_mutex);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool scan_completed = scan_err == ESP_OK || scan_err == ESP_ERR_NOT_FOUND;
    cJSON_AddBoolToObject(root, "ok", scan_completed);
    cJSON_AddNumberToObject(root, "durationMs", (int)(now_ms() - started));
    cJSON_AddNumberToObject(root, "scanSeconds", duration_ms / 1000.0);
    cJSON_AddNumberToObject(root, "deviceCount", op.scan_seen_count);
    cJSON_AddNumberToObject(root, "namedDeviceCount", op.scan_named_count);
    cJSON_AddBoolToObject(root, "targetFound", op.target_found);

    if (op.target_found) {
        cJSON *candidate = cJSON_AddObjectToObject(root, "candidate");
        cJSON_AddStringToObject(candidate, "name", op.target_name);
        cJSON_AddStringToObject(candidate, "address", op.target_address);
        cJSON_AddNumberToObject(candidate, "rssi", op.rssi);
        cJSON_AddStringToObject(candidate, "matchReason", op.match_reason);
    } else {
        cJSON_AddNullToObject(root, "candidate");
    }

    if (op.last_scan_address[0] != '\0') {
        cJSON *last_sample = cJSON_AddObjectToObject(root, "lastSample");
        cJSON_AddStringToObject(last_sample, "name", op.last_scan_name[0] != '\0' ? op.last_scan_name : "unnamed");
        cJSON_AddStringToObject(last_sample, "address", op.last_scan_address);
    } else {
        cJSON_AddNullToObject(root, "lastSample");
    }

    if (ble_state.last_error[0] != '\0') {
        cJSON_AddStringToObject(root, "error", ble_state.last_error);
    } else {
        cJSON_AddNullToObject(root, "error");
    }

    char *printed = cJSON_PrintUnformatted(root);
    if (printed == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, body_len, "%s", printed);
    cJSON_free(printed);
    cJSON_Delete(root);
    return scan_err == ESP_ERR_NOT_FOUND ? ESP_OK : scan_err;
}

const char *hub_ble_state_json(void)
{
    if (!ble_cache_identity_matches_selection()) {
        return "{\"connected\":false,\"machineSerial\":null,\"deviceName\":null,"
            "\"deviceAddress\":null,\"rssi\":null,\"machineMode\":null,"
            "\"tankStatus\":null,\"boilers\":[],\"smartStandby\":null,"
            "\"lastReadAt\":null,\"source\":\"ble\"}";
    }
    return ble_state.state_json;
}

const char *hub_ble_diagnostics_json(void)
{
    static char body[1280];
    hub_machine_selection_t selection = {0};
    bool has_selection = hub_config_get_machine_selection(&selection) == ESP_OK;
    bool token_configured = has_selection && selection.ble_auth_token[0] != '\0';
    bool serial_configured = has_selection && selection.serial_number[0] != '\0';
    bool configured = token_configured && serial_configured;
    bool device_address_configured = has_selection && selection.ble_device_address[0] != '\0';
    bool device_name_configured = has_selection && selection.ble_device_name[0] != '\0';
    bool has_ble_target = device_address_configured || device_name_configured;
    bool ble_first_enabled = configured && has_ble_target;
    bool write_route_open = ble_first_enabled;
    bool preferred = write_route_open;
    bool cache_identity_matches = ble_cache_identity_matches_selection();
    bool cache_fresh = ble_cache_is_fresh();
    hub_machine_selection_identity_t selection_identity = {0};
    bool has_selection_identity = hub_config_get_machine_selection_identity(&selection_identity) == ESP_OK;
    bool control_session_ready = cache_identity_matches
        && has_selection
        && has_selection_identity
        && ble_state.connected
        && cached_conn_handle != BLE_HS_CONN_HANDLE_NONE
        && cached_auth_handle != 0
        && cached_read_handle != 0
        && cached_write_handle != 0
        && cached_selection_identity.generation == selection_identity.generation
        && strcmp(cached_selection_identity.serial_number, selection_identity.serial_number) == 0
        && cached_ble_auth_fingerprint == ble_auth_fingerprint(selection.ble_auth_token)
        && (selection.ble_device_address[0] == '\0'
            || cached_target_address[0] == '\0'
            || strcasecmp(selection.ble_device_address, cached_target_address) == 0);
    char rssi_text[16] = "null";
    char cache_age_text[16] = "null";
    char device_name[80] = {0};
    char machine_mode[48] = {0};
    char recovery_hint[64] = {0};
    char last_error[160] = {0};
    char cache_origin[40] = {0};
    char backoff_seconds[16] = "null";
    int cache_age_ms = -1;
    if (cache_identity_matches && ble_state.cached_at_ms > 0) {
        cache_age_ms = (int)(now_ms() - ble_state.cached_at_ms);
    }
    if (ble_state.rssi != 0) {
        snprintf(rssi_text, sizeof(rssi_text), "%d", ble_state.rssi);
    }
    if (cache_age_ms >= 0) {
        snprintf(cache_age_text, sizeof(cache_age_text), "%d", cache_age_ms);
    }
    if (ble_state.consecutive_failures > 0) {
        snprintf(
            backoff_seconds,
            sizeof(backoff_seconds),
            "%d",
            ble_worker_retry_delay_ms(ble_state.consecutive_failures) / 1000
        );
    }
    json_escape_copy(device_name, sizeof(device_name), ble_state.device_name);
    json_escape_copy(machine_mode, sizeof(machine_mode), ble_state.machine_mode);
    json_escape_copy(recovery_hint, sizeof(recovery_hint), ble_state.recovery_hint);
    json_escape_copy(last_error, sizeof(last_error), ble_state.last_error);
    if (cache_identity_matches) {
        json_escape_copy(cache_origin, sizeof(cache_origin), ble_state.cache_origin);
    }

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"configured\":%s,"
        "\"tokenConfigured\":%s,"
        "\"cloudTokenAvailable\":%s,"
        "\"deviceAddressConfigured\":%s,"
        "\"deviceNameConfigured\":%s,"
        "\"bleFirstEnabled\":%s,"
        "\"writeRouteOpen\":%s,"
        "\"controlSessionReady\":%s,"
        "\"readsValidated\":%s,"
        "\"writesValidated\":%s,"
        "\"preferred\":%s,"
        "\"workerEnabled\":%s,"
        "\"workerStatus\":\"%s\","
        "\"workerConnected\":%s,"
        "\"cachedConnected\":%s,"
        "\"cachedDeviceName\":%s%s%s,"
        "\"cachedMachineMode\":%s%s%s,"
        "\"signalRssi\":%s,"
        "\"cacheStatus\":\"%s\","
        "\"lastSeenAt\":%s,"
        "\"lastReadAt\":%s,"
        "\"cacheAgeMs\":%s,"
        "\"cacheTtlSeconds\":45,"
        "\"cacheSequence\":%lu,"
        "\"cacheOrigin\":%s%s%s,"
        "\"consecutiveFailures\":%d,"
        "\"backoffSeconds\":%s,"
        "\"manualInterventionRecommended\":%s,"
        "\"recoveryHint\":%s%s%s,"
        "\"lastError\":%s%s%s"
        "}",
        bool_text(configured),
        bool_text(token_configured),
        bool_text(token_configured),
        bool_text(device_address_configured),
        bool_text(device_name_configured),
        bool_text(ble_first_enabled),
        bool_text(write_route_open),
        bool_text(control_session_ready),
        bool_text(ble_state.reads_validated),
        bool_text(ble_state.writes_validated),
        bool_text(preferred),
        bool_text(worker_started),
        ble_state.status,
        bool_text(ble_state.connected),
        bool_text(cache_fresh),
        device_name[0] != '\0' ? "\"" : "null",
        device_name[0] != '\0' ? device_name : "",
        device_name[0] != '\0' ? "\"" : "",
        machine_mode[0] != '\0' ? "\"" : "null",
        machine_mode[0] != '\0' ? machine_mode : "",
        machine_mode[0] != '\0' ? "\"" : "",
        rssi_text,
        cache_fresh ? "fresh" : (cache_identity_matches ? "stale" : "missing"),
        ble_state.last_seen_ms > 0 ? "\"1970-01-01T00:00:00Z\"" : "null",
        ble_state.last_read_ms > 0 ? "\"1970-01-01T00:00:00Z\"" : "null",
        cache_age_text,
        (unsigned long)(cache_identity_matches ? ble_state.cache_sequence : 0),
        cache_origin[0] != '\0' ? "\"" : "null",
        cache_origin[0] != '\0' ? cache_origin : "",
        cache_origin[0] != '\0' ? "\"" : "",
        ble_state.consecutive_failures,
        backoff_seconds,
        bool_text(ble_state.manual_intervention_recommended),
        recovery_hint[0] != '\0' ? "\"" : "null",
        recovery_hint[0] != '\0' ? recovery_hint : "",
        recovery_hint[0] != '\0' ? "\"" : "",
        last_error[0] != '\0' ? "\"" : "null",
        last_error[0] != '\0' ? last_error : "",
        last_error[0] != '\0' ? "\"" : ""
    );
    return body;
}

const char *hub_ble_last_error(void)
{
    return ble_state.last_error;
}
