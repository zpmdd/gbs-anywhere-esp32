#include "hub_http.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub_ble.h"
#include "hub_api_v2.h"
#include "hub_cloud.h"
#include "hub_cloud_websocket.h"
#include "hub_command_coordinator.h"
#include "hub_config.h"
#include "hub_control.h"
#include "hub_event_log.h"
#include "hub_identity.h"
#include "hub_routes.h"
#include "hub_runtime_resources.h"
#include "hub_ota.h"
#ifdef HUB_GRINDER
#include "hub_grinder.h"
#endif
#ifdef HUB_TOUCHSCREEN
#include "gbs_display.h"
#endif
#include "hub_security.h"
#include "hub_state.h"
#include "hub_state_keeper.h"
#include "hub_version.h"
#include "hub_wifi.h"

static const char *TAG = "hub_http";

/*
 * Static Gate 0 marker: the protected v2 route table is owned by the pinned
 * HTTPS listener. Runtime availability remains fail-closed behind the device
 * security and persistent identity gates.
 */
#define HUB_V2_HTTPS_ROUTE_ACTIVE 1

#define HUB_OTA_RECV_CHUNK_BYTES 4096
#define HUB_BLE_FULL_READ_BODY_BYTES 12288
#define HUB_BLE_NOTIFY_PROBE_BODY_BYTES 24576
#define HUB_BLE_PROTOCOL_PROBE_BODY_BYTES 12288
#define HUB_BLE_DIAGNOSTIC_WORKER_STACK_BYTES 8192

static StaticSemaphore_t full_read_response_mutex_storage;
static SemaphoreHandle_t full_read_response_mutex;
static StaticSemaphore_t notify_probe_response_mutex_storage;
static SemaphoreHandle_t notify_probe_response_mutex;
static StaticSemaphore_t protocol_probe_response_mutex_storage;
static SemaphoreHandle_t protocol_probe_response_mutex;
static StaticSemaphore_t gatt_read_probe_response_mutex_storage;
static SemaphoreHandle_t gatt_read_probe_response_mutex;
static StaticSemaphore_t route_dispatch_mutex_storage;
static SemaphoreHandle_t route_dispatch_mutex;
static hub_http_transport_t s_legacy_transport = HUB_HTTP_TRANSPORT_LEGACY;
static hub_http_transport_t s_https_transport = HUB_HTTP_TRANSPORT_HTTPS_V2;

typedef hub_ble_worker_job_fn ble_diagnostic_handler_fn;

typedef struct {
    httpd_req_t *request;
    ble_diagnostic_handler_fn handler;
} ble_diagnostic_request_t;

static QueueHandle_t ble_diagnostic_request_queue;
static SemaphoreHandle_t ble_diagnostic_worker_ready;
static TaskHandle_t ble_diagnostic_worker_handle;

static uint32_t machine_state_revision(const char *body)
{
    uint32_t revision = 2166136261U;
    if (body == NULL) {
        return revision;
    }
    for (const unsigned char *cursor = (const unsigned char *)body; *cursor != '\0'; cursor++) {
        revision ^= *cursor;
        revision *= 16777619U;
    }
    return revision;
}

static void discard_async_request_body(httpd_req_t *req)
{
    if (req == NULL) {
        return;
    }
    char scratch[128];
    while (true) {
        int received = httpd_req_recv(req, scratch, sizeof(scratch));
        if (received == 0) {
            return;
        }
        if (received < 0) {
            /* Do not reuse a keep-alive socket with an incomplete body. */
            (void)httpd_resp_set_hdr(req, "Connection", "close");
            return;
        }
    }
}

static void ble_diagnostic_worker_task(void *parameter)
{
    (void)parameter;
    while (true) {
        xSemaphoreGive(ble_diagnostic_worker_ready);
        ble_diagnostic_request_t item = {0};
        if (xQueueReceive(ble_diagnostic_request_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (hub_command_interactive_pending()) {
            discard_async_request_body(item.request);
            (void)hub_routes_send_service_unavailable(
                item.request,
                "Daily interactive control has priority over BLE diagnostics"
            );
        } else {
            if (hub_ble_run_worker_job(
                    item.handler,
                    item.request,
                    NULL
                ) != ESP_OK) {
                (void)hub_routes_send_service_unavailable(
                    item.request,
                    "BLE worker is not ready for diagnostics"
                );
            }
        }
        /* Async completion does not purge unread request bytes as sync handlers do. */
        discard_async_request_body(item.request);
        if (httpd_req_async_handler_complete(item.request) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to complete asynchronous BLE diagnostic request");
        }
    }
}

static esp_err_t start_ble_diagnostic_worker(void)
{
    if (ble_diagnostic_worker_handle != NULL) {
        return ESP_OK;
    }
    if (ble_diagnostic_worker_ready == NULL) {
        /* Queueing is safe as soon as task creation succeeds; the queue owns the request. */
        ble_diagnostic_worker_ready = xSemaphoreCreateCounting(1, 1);
    }
    if (ble_diagnostic_request_queue == NULL) {
        ble_diagnostic_request_queue = xQueueCreate(1, sizeof(ble_diagnostic_request_t));
    }
    if (ble_diagnostic_worker_ready == NULL || ble_diagnostic_request_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* This task only queues/waits; BLE/NVS/NimBLE frames run on the internal BLE worker. */
    if (xTaskCreateWithCaps(
            ble_diagnostic_worker_task,
            "hub_ble_diag",
            HUB_BLE_DIAGNOSTIC_WORKER_STACK_BYTES,
            NULL,
            3,
            &ble_diagnostic_worker_handle,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        ) != pdPASS) {
        ble_diagnostic_worker_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t queue_ble_diagnostic_request(
    httpd_req_t *req,
    ble_diagnostic_handler_fn handler
)
{
    if (hub_command_interactive_pending()) {
        return hub_routes_send_service_unavailable(
            req,
            "Daily interactive control has priority over BLE diagnostics"
        );
    }
    if (xSemaphoreTake(ble_diagnostic_worker_ready, 0) != pdTRUE) {
        return hub_routes_send_service_unavailable(req, "A BLE diagnostic is already running");
    }

    httpd_req_t *request_copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &request_copy);
    if (err != ESP_OK) {
        xSemaphoreGive(ble_diagnostic_worker_ready);
        return hub_routes_send_internal_error(req, "Failed to schedule BLE diagnostic");
    }
    ble_diagnostic_request_t item = {
        .request = request_copy,
        .handler = handler,
    };
    if (xQueueSend(ble_diagnostic_request_queue, &item, 0) != pdTRUE) {
        discard_async_request_body(request_copy);
        (void)hub_routes_send_service_unavailable(
            request_copy,
            "BLE diagnostic queue is busy"
        );
        (void)httpd_req_async_handler_complete(request_copy);
        xSemaphoreGive(ble_diagnostic_worker_ready);
        return ESP_OK;
    }
    return ESP_OK;
}

static bool setup_wifi_requires_softap(void)
{
    return strcmp(hub_wifi_mode(), "softap") == 0;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool decode_form_value(char *value)
{
    char *read = value;
    char *write = value;
    while (*read != '\0') {
        if (*read == '+') {
            *write++ = ' ';
            ++read;
            continue;
        }
        if (*read == '%') {
            int high = hex_value(read[1]);
            int low = hex_value(read[2]);
            if (high < 0 || low < 0) {
                return false;
            }
            *write++ = (char)((high << 4) | low);
            read += 3;
            continue;
        }
        *write++ = *read++;
    }
    *write = '\0';
    return true;
}

static esp_err_t read_form_body(httpd_req_t *req, char *body, size_t body_len)
{
    if (req->content_len == 0 || req->content_len >= body_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t remaining = req->content_len;
    size_t offset = 0;
    while (remaining > 0) {
        int read_len = httpd_req_recv(req, body + offset, remaining);
        if (read_len <= 0) {
            return ESP_FAIL;
        }
        offset += read_len;
        remaining -= read_len;
    }
    body[offset] = '\0';
    return ESP_OK;
}

static esp_err_t read_json_body(httpd_req_t *req, char *body, size_t body_len)
{
    return read_form_body(req, body, body_len);
}

static bool request_header_matches(httpd_req_t *req, const char *name, const char *expected)
{
    char value[64] = {0};
    if (httpd_req_get_hdr_value_str(req, name, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    return strcmp(value, expected) == 0;
}

void *hub_http_server_context(hub_http_transport_t transport)
{
    return transport == HUB_HTTP_TRANSPORT_HTTPS_V2
        ? (void *)&s_https_transport
        : (void *)&s_legacy_transport;
}

bool hub_http_request_is_secure(httpd_req_t *req)
{
    if (req == NULL || req->handle == NULL) {
        return false;
    }
    const hub_http_transport_t *transport =
        (const hub_http_transport_t *)httpd_get_global_user_ctx(req->handle);
    return transport != NULL && *transport == HUB_HTTP_TRANSPORT_HTTPS_V2;
}

size_t hub_http_max_uri_handlers(void)
{
    return HUB_HTTP_MAX_URI_HANDLERS;
}

static esp_err_t require_api_transport(httpd_req_t *req)
{
    bool v2_header = request_header_matches(req, HUB_API_V2_HEADER_NAME, "2");
    if (!hub_http_request_is_secure(req)) {
        if (v2_header) {
            (void)hub_routes_send_upgrade_required(req);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (!v2_header) {
        (void)hub_routes_send_bad_request(req, "HTTPS endpoint requires API version 2");
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(req, HUB_API_V2_HEADER_NAME, "2");
    return ESP_OK;
}

static esp_err_t require_authorized(httpd_req_t *req)
{
    if (require_api_transport(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (hub_security_is_authorized(req)) {
        return ESP_OK;
    }
    hub_routes_send_unauthorized(req);
    return ESP_FAIL;
}

static esp_err_t send_versioned_json(httpd_req_t *req, const char *body)
{
    if (!hub_http_request_is_secure(req)) {
        return hub_routes_send_json(req, body);
    }
    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return hub_routes_send_internal_error(req, "invalid v2 response");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, "apiVersion");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "hubId");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "architectureMode");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "architectureEpoch");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "readOwners");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "cloudCommandReadiness");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "actionReadiness");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "sourceSummary");

    const char *cloud_readiness = hub_cloud_command_readiness();
    const char *ble_readiness = !hub_ble_configured()
        ? "unconfigured"
        : (hub_ble_reads_validated() ? "ready" : "warming");
    const char *context_action_readiness = strcmp(cloud_readiness, "ready") == 0
        ? "ready"
        : (strcmp(cloud_readiness, "unconfigured") == 0
            ? "unconfigured"
            : (strcmp(cloud_readiness, "degraded") == 0
                ? "degraded"
                : "contextRefreshRequired"));

    cJSON_AddNumberToObject(root, "apiVersion", HUB_API_V2_VERSION);
    cJSON_AddStringToObject(root, "hubId", hub_identity_hub_id());
    cJSON_AddStringToObject(root, "architectureMode", "legacy");
    cJSON_AddNumberToObject(root, "architectureEpoch", 0);
    cJSON *read_owners = cJSON_AddObjectToObject(root, "readOwners");
    cJSON *action_readiness = cJSON_AddObjectToObject(root, "actionReadiness");
    cJSON *source_summary = cJSON_AddObjectToObject(root, "sourceSummary");
    if (read_owners == NULL || action_readiness == NULL || source_summary == NULL) {
        cJSON_Delete(root);
        return hub_routes_send_internal_error(req, "failed to build v2 response");
    }
    cJSON_AddStringToObject(read_owners, "dashboard", "hub");
    cJSON_AddStringToObject(read_owners, "statistics", "hub");
    cJSON_AddStringToObject(read_owners, "schedule", "hub");
    cJSON_AddStringToObject(read_owners, "firmware", "hub");
    cJSON_AddStringToObject(root, "cloudCommandReadiness", cloud_readiness);
    cJSON_AddStringToObject(action_readiness, "bleControls", ble_readiness);
    cJSON_AddStringToObject(
        action_readiness,
        "quickWeight",
        strcmp(ble_readiness, "ready") == 0
            ? context_action_readiness
            : ble_readiness
    );
    cJSON_AddStringToObject(action_readiness, "prebrewMode", ble_readiness);
    cJSON_AddStringToObject(
        action_readiness,
        "prebrewTimes",
        context_action_readiness
    );
    cJSON_AddStringToObject(action_readiness, "backflush", ble_readiness);
    cJSON_AddNullToObject(source_summary, "bleObservedAt");
    cJSON_AddNullToObject(source_summary, "commandObservedAt");
    cJSON_AddNullToObject(source_summary, "cloudCommandContextObservedAt");
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == NULL) {
        return hub_routes_send_internal_error(req, "failed to render v2 response");
    }
    esp_err_t err = hub_routes_send_json(req, rendered);
    free(rendered);
    return err;
}

static bool request_content_type_contains(httpd_req_t *req, const char *expected)
{
    char value[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    return strstr(value, expected) != NULL;
}

static bool form_optional_value(const char *body, const char *key, char *value, size_t value_len)
{
    value[0] = '\0';
    if (httpd_query_key_value(body, key, value, value_len) != ESP_OK) {
        return false;
    }
    return decode_form_value(value) && value[0] != '\0';
}

static bool form_has_key(const char *body, const char *key)
{
    char value[1] = {0};
    esp_err_t err = httpd_query_key_value(body, key, value, sizeof(value));
    return err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC;
}

static bool form_value(const char *body, const char *key, char *value, size_t value_len)
{
    return form_optional_value(body, key, value, value_len);
}

static void preserve_existing_machine_ble_auth_token_if_missing(
    bool ble_auth_token_missing,
    const char *serial_number,
    char *ble_auth_token,
    size_t ble_auth_token_len
)
{
    if (
        !ble_auth_token_missing
        || serial_number == NULL
        || serial_number[0] == '\0'
        || ble_auth_token == NULL
        || ble_auth_token_len == 0
    ) {
        return;
    }

    hub_machine_selection_t existing_selection = {0};
    if (hub_config_get_machine_selection(&existing_selection) == ESP_OK
        && strcmp(existing_selection.serial_number, serial_number) == 0
        && existing_selection.ble_auth_token[0] != '\0') {
        snprintf(ble_auth_token, ble_auth_token_len, "%s", existing_selection.ble_auth_token);
    }
    memset(&existing_selection, 0, sizeof(existing_selection));
}

static void preserve_existing_machine_ble_identity_if_missing(
    bool ble_device_name_missing,
    bool ble_device_address_missing,
    const char *serial_number,
    char *ble_device_name,
    size_t ble_device_name_len,
    char *ble_device_address,
    size_t ble_device_address_len
)
{
    if (
        serial_number == NULL
        || serial_number[0] == '\0'
        || (!ble_device_name_missing && !ble_device_address_missing)
    ) {
        return;
    }

    hub_machine_selection_t existing_selection = {0};
    if (hub_config_get_machine_selection(&existing_selection) != ESP_OK
        || strcmp(existing_selection.serial_number, serial_number) != 0) {
        memset(&existing_selection, 0, sizeof(existing_selection));
        return;
    }

    if (ble_device_name_missing
        && ble_device_name != NULL
        && ble_device_name_len > 0
        && existing_selection.ble_device_name[0] != '\0') {
        snprintf(ble_device_name, ble_device_name_len, "%s", existing_selection.ble_device_name);
    }
    if (ble_device_address_missing
        && ble_device_address != NULL
        && ble_device_address_len > 0
        && existing_selection.ble_device_address[0] != '\0') {
        snprintf(ble_device_address, ble_device_address_len, "%s", existing_selection.ble_device_address);
    }
    memset(&existing_selection, 0, sizeof(existing_selection));
}

static bool json_string_value(cJSON *root, const char *key, char *value, size_t value_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return false;
    }
    snprintf(value, value_len, "%s", item->valuestring);
    return true;
}

static bool json_int_value(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    int parsed = (int)item->valuedouble;
    if ((double)parsed != item->valuedouble) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool json_double_value(cJSON *root, const char *key, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static bool json_bool_value(cJSON *root, const char *key, bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *value = cJSON_IsTrue(item);
    return true;
}

static bool json_control_value(cJSON *root, const char *key, char *value, size_t value_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
        snprintf(value, value_len, "%s", item->valuestring);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        snprintf(value, value_len, "%.1f", item->valuedouble);
        return true;
    }
    return false;
}

static bool json_optional_request_id(cJSON *root, char *request_id, size_t request_id_len)
{
    if (root == NULL || request_id == NULL || request_id_len < HUB_COMMAND_REQUEST_ID_CAPACITY) {
        return false;
    }
    request_id[0] = '\0';
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsString(item)
        || item->valuestring == NULL
        || !hub_command_request_id_valid(item->valuestring)) {
        return false;
    }
    snprintf(request_id, request_id_len, "%s", item->valuestring);
    return true;
}

static bool parse_v2_client_surface(
    const cJSON *root,
    hub_api_v2_client_surface_t *surface
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "clientSurface");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    if (strcmp(item->valuestring, "iphoneApp") == 0) {
        *surface = HUB_API_V2_SURFACE_IPHONE_APP;
    } else if (strcmp(item->valuestring, "iphoneWidget") == 0) {
        *surface = HUB_API_V2_SURFACE_IPHONE_WIDGET;
    } else if (strcmp(item->valuestring, "siri") == 0) {
        *surface = HUB_API_V2_SURFACE_SIRI;
    } else if (strcmp(item->valuestring, "watchApp") == 0) {
        *surface = HUB_API_V2_SURFACE_WATCH_APP;
    } else if (strcmp(item->valuestring, "watchWidget") == 0) {
        *surface = HUB_API_V2_SURFACE_WATCH_WIDGET;
    } else if (strcmp(item->valuestring, "diagnostic") == 0) {
        *surface = HUB_API_V2_SURFACE_DIAGNOSTIC;
    } else {
        return false;
    }
    return true;
}

static bool apply_v2_command_metadata(
    httpd_req_t *req,
    const cJSON *root,
    hub_command_request_t *command
)
{
    if (!hub_http_request_is_secure(req)) {
        return true;
    }
    if (root == NULL || command == NULL
        || !request_content_type_contains(req, "application/json")
        || !hub_command_request_id_valid(command->request_id)
        || !parse_v2_client_surface(root, &command->client_surface)) {
        return false;
    }

    const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(root, "clientTtlMs");
    bool ttl_present = ttl != NULL;
    int ttl_value = 0;
    if (ttl_present) {
        if (!cJSON_IsNumber(ttl)) {
            return false;
        }
        ttl_value = (int)ttl->valuedouble;
        if ((double)ttl_value != ttl->valuedouble
            || ttl_value < 1
            || ttl_value > (int)HUB_COMMAND_DEADLINE_MAX_TTL_MS) {
            return false;
        }
    }

    bool deadline_command = command->kind == HUB_COMMAND_QUICK_WEIGHT
        || command->kind == HUB_COMMAND_PREBREW
        || (command->kind == HUB_COMMAND_BBW_MODE
            && command->value.bbw_mode.has_preset_grams);
    if (!deadline_command && ttl_present) {
        return false;
    }
    if (deadline_command) {
        command->deadline.enforced = true;
        command->deadline.client_ttl_present = ttl_present;
        command->deadline.client_ttl_ms = ttl_present ? (uint32_t)ttl_value : 0;
    }
    return true;
}

static int query_int_value(httpd_req_t *req, const char *key, int default_value, int min_value, int max_value)
{
    char query[128] = {0};
    char value[24] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return default_value;
    }
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return default_value;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return default_value;
    }
    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return (int)parsed;
}

static bool query_string_value(httpd_req_t *req, const char *key, char *value, size_t value_len)
{
    char query[128] = {0};
    if (value == NULL || value_len == 0) {
        return false;
    }
    value[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, value, value_len) == ESP_OK;
}

static void set_control_http_status(
    httpd_req_t *req,
    esp_err_t err,
    const hub_control_result_t *result
)
{
    int status = result != NULL ? result->response_http_status : 0;
    switch (status) {
    case 202:
        httpd_resp_set_status(req, "202 Accepted");
        return;
    case 400:
        httpd_resp_set_status(req, "400 Bad Request");
        return;
    case 408:
        httpd_resp_set_status(req, "408 Request Timeout");
        return;
    case 409:
        httpd_resp_set_status(req, "409 Conflict");
        return;
    case 503:
        httpd_resp_set_status(req, "503 Service Unavailable");
        return;
    default:
        break;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "400 Bad Request");
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
}

static void apply_control_completion_state_effect(
    esp_err_t err,
    const hub_control_result_t *result
)
{
    bool command_rejected = result != NULL
        && (result->response_http_status == 408 || result->response_http_status == 409);
    if (err == ESP_OK) {
        hub_state_keeper_touch();
    } else if (err != ESP_ERR_INVALID_ARG && !command_rejected) {
        if (hub_cloud_has_cached_snapshot()) {
            hub_state_keeper_touch();
        } else {
            hub_state_keeper_request_recovery();
        }
    }
}

static esp_err_t send_control_response_without_state_effect(
    httpd_req_t *req,
    esp_err_t err,
    const hub_control_result_t *result
)
{
    char body[2048] = {0};
    hub_control_result_json(result, body, sizeof(body));
    set_control_http_status(req, err, result);
    return hub_routes_send_json(req, body);
}

static void command_state_completion(
    const char *request_id,
    esp_err_t err,
    const hub_control_result_t *result,
    bool deduplicated,
    void *raw_context
)
{
    (void)request_id;
    (void)deduplicated;
    (void)raw_context;
    apply_control_completion_state_effect(err, result);
}

static esp_err_t submit_control_command(httpd_req_t *req, const hub_command_request_t *command)
{
    if (command == NULL) {
        return hub_routes_send_bad_request(req, "missing command");
    }
    if (!hub_http_request_is_secure(req)
        && request_header_matches(
            req,
            HUB_API_V2_HEADER_NAME,
            "2"
        )) {
        return hub_routes_send_upgrade_required(req);
    }

    hub_command_request_t scoped_command = *command;
    const char *machine_serial_header = HUB_API_V2_MACHINE_SERIAL_HEADER_NAME;
    size_t machine_serial_len = httpd_req_get_hdr_value_len(req, machine_serial_header);
    if (machine_serial_len > HUB_CONFIG_MAX_MACHINE_SERIAL_LEN) {
        return hub_routes_send_bad_request(req, "invalid machine serial header");
    }
    if (hub_http_request_is_secure(req) && machine_serial_len == 0) {
        return hub_routes_send_bad_request(req, "missing machine serial header");
    }
    if (machine_serial_len > 0
        && (httpd_req_get_hdr_value_str(
                req,
                machine_serial_header,
                scoped_command.expected_serial_number,
                sizeof(scoped_command.expected_serial_number)
            ) != ESP_OK
            || scoped_command.expected_serial_number[0] == '\0')) {
        return hub_routes_send_bad_request(req, "invalid machine serial header");
    }

    hub_command_submission_t submission = {0};
    hub_command_submit_status_t status = hub_command_submit(
        &scoped_command,
        command_state_completion,
        NULL,
        &submission
    );
    if (status == HUB_COMMAND_ACCEPTED || status == HUB_COMMAND_DUPLICATE_INFLIGHT) {
        submission.result.response_http_status = 202;
    } else if (status == HUB_COMMAND_INVALID) {
        submission.result.response_http_status = 400;
    } else if (status == HUB_COMMAND_QUEUE_FULL
        || status == HUB_COMMAND_DEADLINE_CAPACITY) {
        submission.result.response_http_status = 503;
    }
    return send_control_response_without_state_effect(
        req,
        submission.error,
        &submission.result
    );
}

static const char *reset_reason_string(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "powerOn";
    case ESP_RST_EXT: return "externalPin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interruptWatchdog";
    case ESP_RST_TASK_WDT: return "taskWatchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepSleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "powerGlitch";
    case ESP_RST_CPU_LOCKUP: return "cpuLockup";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
    }
}

static esp_err_t health_handler(httpd_req_t *req)
{
    if ((hub_http_request_is_secure(req)
            || request_header_matches(req, HUB_API_V2_HEADER_NAME, "2"))
        && require_api_transport(req) != ESP_OK) {
        return ESP_OK;
    }
#if CONFIG_APP_RETRIEVE_LEN_ELF_SHA != 8
#error "LamaBuddy firmwareBuildId contract requires an 8-character ELF SHA-256 prefix"
#endif
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    uint32_t flash_bytes=0;
    esp_flash_get_size(NULL,&flash_bytes);
    hub_runtime_resources_t resources;
    hub_runtime_resources_snapshot(&resources);
    const char *ble_status = hub_ble_reads_validated()
        ? "cached"
        : (hub_ble_configured() ? "configured" : "unconfigured");
    esp_reset_reason_t reset_reason = esp_reset_reason();

    char body[1024] = {0};
    snprintf(
        body,
        sizeof(body),
        "{"
        "\"status\":\"ok\","
        "\"deviceModel\":\"%s\","
        "\"firmwareVersion\":\"%s\","
        "\"firmwareBuildId\":\"%s\","
        "\"firmwareBuildIdAlgorithm\":\"%s\","
        "\"chip\":\"" LAMABUDDY_CHIP_NAME "\","
        "\"cores\":%d,"
        "\"flashMB\":%lu,"
        "\"psramMB\":%lu,"
        "\"psramInitialized\":%s,"
        "\"psramBytes\":%lu,"
        "\"psramFreeBytes\":%lu,"
        "\"internalFreeHeap\":%lu,"
        "\"internalLargestFreeBlock\":%lu,"
        "\"freeHeap\":%lu,"
        "\"resetReason\":\"%s\","
        "\"resetReasonCode\":%d,"
        "\"bleDiagnosticStackHighWaterMark\":%lu,"
        "\"uptimeSeconds\":%lld,"
        "\"wifiMode\":\"%s\","
        "\"localIp\":\"%s\","
        "\"setupApSsid\":\"%s\","
        "\"cloud\":\"%s\","
        "\"ble\":\"%s\""
        "}",
        LAMABUDDY_DEVICE_MODEL,
        LAMABUDDY_FIRMWARE_VERSION,
        esp_app_get_elf_sha256_str(),
        LAMABUDDY_FIRMWARE_BUILD_ID_ALGORITHM,
        chip_info.cores,
        (unsigned long)(flash_bytes/(1024U*1024U)),
        (unsigned long)(resources.psram_bytes / (1024U * 1024U)),
        resources.psram_initialized ? "true" : "false",
        (unsigned long)resources.psram_bytes,
        (unsigned long)resources.psram_free_bytes,
        (unsigned long)resources.internal_free_bytes,
        (unsigned long)resources.internal_largest_block_bytes,
        (unsigned long)esp_get_free_heap_size(),
        reset_reason_string(reset_reason),
        (int)reset_reason,
        (unsigned long)hub_ble_worker_stack_high_water_mark(),
        (long long)(esp_timer_get_time() / 1000000),
        hub_wifi_mode(),
        hub_wifi_ip_address(),
        hub_wifi_ap_ssid(),
        hub_cloud_status_string(),
        ble_status
    );
    return send_versioned_json(req, body);
}

static esp_err_t setup_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(
        req,
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>LamaBuddy Hub Setup</title></head><body>"
        "<h1>LamaBuddy Hub</h1>"
        "<form method=\"post\" action=\"/setup/wifi\">"
        "<label>Wi-Fi SSID <input name=\"ssid\"></label><br>"
        "<label>Password <input name=\"password\" type=\"password\"></label><br>"
        "<button type=\"submit\">Save Wi-Fi</button>"
        "</form></body></html>"
    );
}

static esp_err_t setup_wifi_handler(httpd_req_t *req)
{
    if (!setup_wifi_requires_softap()) {
        return hub_routes_send_forbidden(req, "wifi setup is only available in setup mode");
    }

    char body[256] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing setup body");
    }

    char ssid[HUB_CONFIG_MAX_SSID_LEN + 1] = {0};
    char password[HUB_CONFIG_MAX_PASSWORD_LEN + 1] = {0};
    if (!form_value(body, "ssid", ssid, sizeof(ssid))) {
        return hub_routes_send_bad_request(req, "missing ssid");
    }
    form_optional_value(body, "password", password, sizeof(password));

    esp_err_t err = hub_config_save_wifi_credentials(ssid, password);
    if (err != ESP_OK) {
        return hub_routes_send_internal_error(req, "failed to save Wi-Fi credentials");
    }

    return hub_routes_send_json(req, "{\"ok\":true,\"restartRequired\":true}");
}

static esp_err_t setup_cloud_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[2048] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing cloud setup body");
    }

    char username[HUB_CONFIG_MAX_CLOUD_USERNAME_LEN + 1] = {0};
    char password[HUB_CONFIG_MAX_CLOUD_PASSWORD_LEN + 1] = {0};
    char refresh_token[HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1] = {0};
    char installation_key[HUB_CONFIG_MAX_INSTALLATION_KEY_LEN + 1] = {0};
    char cloud_serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};

    if (!form_value(body, "username", username, sizeof(username))) {
        return hub_routes_send_bad_request(req, "missing cloud username");
    }
    form_optional_value(body, "password", password, sizeof(password));
    form_optional_value(body, "refreshToken", refresh_token, sizeof(refresh_token));
    if (password[0] == '\0' && refresh_token[0] == '\0') {
        return hub_routes_send_bad_request(req, "missing cloud password or refresh token");
    }
    form_optional_value(body, "installationKey", installation_key, sizeof(installation_key));
    if (installation_key[0] == '\0') {
        return hub_routes_send_bad_request(req, "missing cloud installation key");
    }
    form_optional_value(body, "cloudSerialNumber", cloud_serial_number, sizeof(cloud_serial_number));

    esp_err_t err = hub_config_save_cloud_settings(
        username,
        password,
        refresh_token,
        installation_key,
        cloud_serial_number
    );
    if (err != ESP_OK) {
        return hub_routes_send_internal_error(req, "failed to save cloud settings");
    }

    return hub_routes_send_json(req, "{\"ok\":true,\"cloudConfigured\":true}");
}

static esp_err_t setup_machine_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[768] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing machine setup body");
    }

    char serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};
    char machine_name[HUB_CONFIG_MAX_MACHINE_NAME_LEN + 1] = {0};
    char ble_auth_token[HUB_CONFIG_MAX_BLE_TOKEN_LEN + 1] = {0};
    char ble_device_name[HUB_CONFIG_MAX_BLE_DEVICE_NAME_LEN + 1] = {0};
    char ble_device_address[HUB_CONFIG_MAX_BLE_DEVICE_ADDRESS_LEN + 1] = {0};

    if (!form_value(body, "serialNumber", serial_number, sizeof(serial_number))) {
        return hub_routes_send_bad_request(req, "missing machine serial number");
    }
    form_optional_value(body, "machineName", machine_name, sizeof(machine_name));
    bool ble_auth_token_missing = !form_has_key(body, "bleAuthToken");
    bool ble_device_name_has_key = form_has_key(body, "bleDeviceName");
    bool ble_device_address_has_key = form_has_key(body, "bleDeviceAddress");
    bool ble_auth_token_provided = form_optional_value(body, "bleAuthToken", ble_auth_token, sizeof(ble_auth_token));
    if (!ble_auth_token_provided && !ble_auth_token_missing) {
        ble_auth_token[0] = '\0';
    }
    form_optional_value(body, "bleDeviceName", ble_device_name, sizeof(ble_device_name));
    form_optional_value(body, "bleDeviceAddress", ble_device_address, sizeof(ble_device_address));
    bool ble_device_name_missing = !ble_device_name_has_key || ble_device_name[0] == '\0';
    bool ble_device_address_missing = !ble_device_address_has_key || ble_device_address[0] == '\0';
    preserve_existing_machine_ble_auth_token_if_missing(
        ble_auth_token_missing,
        serial_number,
        ble_auth_token,
        sizeof(ble_auth_token)
    );
    preserve_existing_machine_ble_identity_if_missing(
        ble_device_name_missing,
        ble_device_address_missing,
        serial_number,
        ble_device_name,
        sizeof(ble_device_name),
        ble_device_address,
        sizeof(ble_device_address)
    );

    esp_err_t err = hub_config_save_machine_selection(
        serial_number,
        machine_name,
        ble_auth_token,
        ble_device_name,
        ble_device_address
    );
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_status(req, "409 Conflict");
            return hub_routes_send_json(
                req,
                "{\"ok\":false,\"errorCode\":\"machineSelectionBusy\","
                "\"message\":\"A machine command is currently executing.\"}"
            );
        }
        return hub_routes_send_internal_error(req, "failed to save machine selection");
    }

    (void)hub_cloud_request_refresh();

    char response[96] = {0};
    snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"machineSelected\":true,\"bleAuthTokenCached\":%s}",
        ble_auth_token[0] != '\0' ? "true" : "false"
    );
    return hub_routes_send_json(req, response);
}

static esp_err_t bbw_selection_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) return ESP_OK;
    char body[320] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK)
        return hub_routes_send_bad_request(req, "missing selection");
    cJSON *root = cJSON_Parse(body);
    const cJSON *serial = cJSON_GetObjectItemCaseSensitive(root, "serialNumber");
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *confirmed_at = cJSON_GetObjectItemCaseSensitive(root, "confirmedAtUnixSeconds");
    hub_machine_selection_identity_t target = {0};
    bool valid = cJSON_IsString(serial) && strlen(serial->valuestring) < sizeof(target.serial_number)
        && cJSON_IsNumber(generation) && generation->valuedouble >= 1
        && generation->valuedouble <= UINT32_MAX && floor(generation->valuedouble) == generation->valuedouble
        && cJSON_IsString(mode) && cJSON_IsNumber(confirmed_at);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid) {
        strcpy(target.serial_number, serial->valuestring);
        target.generation = (uint32_t)generation->valuedouble;
        // Display acknowledgement only; this route never dispatches a machine command.
        err = hub_config_save_bbw_selection(&target, mode->valuestring, confirmed_at->valuedouble);
    }
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG) return hub_routes_send_bad_request(req, "invalid selection");
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return hub_routes_send_json(req, "{\"ok\":false}");
    }
    if (err != ESP_OK) return hub_routes_send_service_unavailable(req, "selection storage unavailable");
    return hub_routes_send_json(req, "{\"ok\":true}");
}

static esp_err_t machine_state_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    const char *body = hub_state_machine_state_json();
    uint32_t revision = machine_state_revision(body);
    char etag[24] = {0};
    char revision_text[16] = {0};
    snprintf(etag, sizeof(etag), "\"lm-%08" PRIx32 "\"", revision);
    snprintf(revision_text, sizeof(revision_text), "%08" PRIx32, revision);
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "X-LamaBuddy-State-Revision", revision_text);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char if_none_match[48] = {0};
    if (httpd_req_get_hdr_value_str(
            req,
            "If-None-Match",
            if_none_match,
            sizeof(if_none_match)
        ) == ESP_OK
        && strcmp(if_none_match, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    return send_versioned_json(req, body);
}

static esp_err_t capabilities_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return send_versioned_json(req, hub_state_capabilities_json());
}

static esp_err_t machine_binding_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return hub_routes_send_json(req, hub_state_machine_binding_json());
}

static esp_err_t transports_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_transports_json());
}

static esp_err_t recent_commands_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_recent_commands_json());
}

static esp_err_t command_diagnostics_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    char request_id[HUB_COMMAND_REQUEST_ID_CAPACITY] = {0};
    bool has_request_id = query_string_value(
        req,
        "id",
        request_id,
        sizeof(request_id)
    );
    if (has_request_id && !hub_command_request_id_valid(request_id)) {
        return hub_routes_send_bad_request(req, "invalid command id");
    }

    static EXT_RAM_BSS_ATTR char body[8192];
    hub_command_diagnostics_json(has_request_id ? request_id : NULL, body, sizeof(body));
    if (has_request_id) {
        hub_command_lookup_status_t status = hub_command_lookup(request_id, NULL, NULL);
        if (status == HUB_COMMAND_LOOKUP_INFLIGHT) {
            httpd_resp_set_status(req, "202 Accepted");
        } else if (status == HUB_COMMAND_LOOKUP_NOT_FOUND) {
            httpd_resp_set_status(req, "404 Not Found");
        }
    }
    return hub_routes_send_json(req, body);
}

static esp_err_t setup_events_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_event_log_json());
}

static esp_err_t slow_cloud_reads_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_slow_cloud_reads_json());
}

static esp_err_t slow_cloud_reads_refresh_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    char kind[16] = "statistics";
    query_string_value(req, "kind", kind, sizeof(kind));
    int days = query_int_value(req, "days", 7, 1, 30);
    return hub_routes_send_json(req, hub_state_start_slow_cloud_read_json(kind, days));
}

static esp_err_t statistics_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    int days = query_int_value(req, "days", 7, 1, 30);
    hub_state_slow_read_result_t result = hub_state_statistics_read(days);
    if (result.stale) {
        httpd_resp_set_hdr(req, "X-LamaBuddy-Stale", "1");
    }
    if (result.state == HUB_STATE_SLOW_READ_REFRESHING) {
        httpd_resp_set_status(req, "202 Accepted");
    } else if (result.state == HUB_STATE_SLOW_READ_UNAVAILABLE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    return hub_routes_send_json(req, result.body);
}

static esp_err_t schedule_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    hub_state_slow_read_result_t result = hub_state_schedule_read();
    if (result.stale) {
        httpd_resp_set_hdr(req, "X-LamaBuddy-Stale", "1");
    }
    if (result.state == HUB_STATE_SLOW_READ_REFRESHING) {
        httpd_resp_set_status(req, "202 Accepted");
    } else if (result.state == HUB_STATE_SLOW_READ_UNAVAILABLE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    return hub_routes_send_json(req, result.body);
}

static esp_err_t firmware_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    hub_state_slow_read_result_t result = hub_state_firmware_read();
    if (result.stale) {
        httpd_resp_set_hdr(req, "X-LamaBuddy-Stale", "1");
    }
    if (result.state == HUB_STATE_SLOW_READ_REFRESHING) {
        httpd_resp_set_status(req, "202 Accepted");
    } else if (result.state == HUB_STATE_SLOW_READ_UNAVAILABLE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    return hub_routes_send_json(req, result.body);
}

static esp_err_t live_state_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_live_state_json());
}

static esp_err_t ble_state_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_ble_state_json());
}

static esp_err_t ble_diagnostics_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_ble_diagnostics_json());
}

static bool ble_diagnostic_yields_to_interactive_control(httpd_req_t *req)
{
    if (!hub_command_interactive_pending()) {
        return false;
    }
    (void)hub_routes_send_service_unavailable(
        req,
        "Daily interactive control has priority over BLE diagnostics"
    );
    return true;
}

static esp_err_t ble_validate_read_work_handler(void *context)
{
    httpd_req_t *req = context;
    if (ble_diagnostic_yields_to_interactive_control(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    hub_ble_read_result_t result = {0};
    esp_err_t err = hub_ble_validate_read(&result);
    hub_state_record_command("bleValidateRead", "ble", result.ok, result.duration_ms, result.message);

    char body[256] = {0};
    snprintf(
        body,
        sizeof(body),
        "{\"ok\":%s,\"transport\":\"ble\",\"durationMs\":%d,\"message\":\"%s\"}",
        result.ok ? "true" : "false",
        result.duration_ms,
        result.message
    );
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return hub_routes_send_json(req, body);
    }
    return hub_routes_send_json(req, body);
}

static esp_err_t ble_full_read_work_handler(void *context)
{
    httpd_req_t *req = context;
    if (ble_diagnostic_yields_to_interactive_control(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (full_read_response_mutex == NULL) {
        full_read_response_mutex = xSemaphoreCreateMutexStatic(&full_read_response_mutex_storage);
    }
    if (full_read_response_mutex == NULL
        || xSemaphoreTake(full_read_response_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return hub_routes_send_service_unavailable(req, "BLE full-read response buffer is busy");
    }

    char *full_read_response_body = heap_caps_calloc(1, HUB_BLE_FULL_READ_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (full_read_response_body == NULL) {
        full_read_response_body = heap_caps_calloc(1, HUB_BLE_FULL_READ_BODY_BYTES, MALLOC_CAP_8BIT);
    }
    if (full_read_response_body == NULL) {
        xSemaphoreGive(full_read_response_mutex);
        return hub_routes_send_internal_error(req, "BLE full-read response buffer allocation failed");
    }

    int64_t started = esp_timer_get_time() / 1000;
    esp_err_t err = hub_ble_full_read_probe_json(full_read_response_body, HUB_BLE_FULL_READ_BODY_BYTES);
    int duration_ms = (int)(esp_timer_get_time() / 1000 - started);
    hub_state_record_command(
        "bleFullReadProbe",
        "ble",
        err == ESP_OK,
        duration_ms,
        err == ESP_OK ? "BLE full read probe completed." : hub_ble_last_error()
    );

    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    esp_err_t send_err = hub_routes_send_json(req, full_read_response_body);
    free(full_read_response_body);
    xSemaphoreGive(full_read_response_mutex);
    return send_err;
}

static esp_err_t ble_notify_probe_work_handler(void *context)
{
    httpd_req_t *req = context;
    if (ble_diagnostic_yields_to_interactive_control(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    int duration_ms = 30000;
    if (req->content_len > 0) {
        char request_body[160] = {0};
        if (read_json_body(req, request_body, sizeof(request_body)) != ESP_OK) {
            return hub_routes_send_bad_request(req, "invalid BLE notify probe body");
        }
        cJSON *root = cJSON_Parse(request_body);
        if (root == NULL) {
            return hub_routes_send_bad_request(req, "invalid BLE notify probe JSON");
        }
        const cJSON *duration_ms_item = cJSON_GetObjectItemCaseSensitive(root, "durationMs");
        const cJSON *duration_seconds_item = cJSON_GetObjectItemCaseSensitive(root, "durationSeconds");
        if (cJSON_IsNumber(duration_ms_item)) {
            duration_ms = duration_ms_item->valueint;
        } else if (cJSON_IsNumber(duration_seconds_item)) {
            duration_ms = duration_seconds_item->valueint * 1000;
        }
        cJSON_Delete(root);
    }

    if (notify_probe_response_mutex == NULL) {
        notify_probe_response_mutex = xSemaphoreCreateMutexStatic(&notify_probe_response_mutex_storage);
    }
    if (notify_probe_response_mutex == NULL
        || xSemaphoreTake(notify_probe_response_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return hub_routes_send_service_unavailable(req, "BLE notify probe response buffer is busy");
    }

    char *notify_probe_response_body = heap_caps_calloc(1, HUB_BLE_NOTIFY_PROBE_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (notify_probe_response_body == NULL) {
        notify_probe_response_body = heap_caps_calloc(1, HUB_BLE_NOTIFY_PROBE_BODY_BYTES, MALLOC_CAP_8BIT);
    }
    if (notify_probe_response_body == NULL) {
        xSemaphoreGive(notify_probe_response_mutex);
        return hub_routes_send_internal_error(req, "BLE notify probe response buffer allocation failed");
    }

    int64_t started = esp_timer_get_time() / 1000;
    esp_err_t err = hub_ble_notify_probe_json(duration_ms, notify_probe_response_body, HUB_BLE_NOTIFY_PROBE_BODY_BYTES);
    int elapsed_ms = (int)(esp_timer_get_time() / 1000 - started);
    hub_state_record_command(
        "bleNotifyProbe",
        "ble",
        err == ESP_OK,
        elapsed_ms,
        err == ESP_OK ? "BLE notify probe completed." : hub_ble_last_error()
    );

    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    esp_err_t send_err = hub_routes_send_json(req, notify_probe_response_body);
    free(notify_probe_response_body);
    xSemaphoreGive(notify_probe_response_mutex);
    return send_err;
}

static esp_err_t ble_protocol_probe_work_handler(void *context)
{
    httpd_req_t *req = context;
    if (ble_diagnostic_yields_to_interactive_control(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    char profile[16] = "fast";
    int duration_ms = 30000;
    int interval_ms = 300;
    if (req->content_len > 0) {
        char request_body[192] = {0};
        if (read_json_body(req, request_body, sizeof(request_body)) != ESP_OK) {
            return hub_routes_send_bad_request(req, "invalid BLE protocol probe body");
        }
        cJSON *root = cJSON_Parse(request_body);
        if (root == NULL) {
            return hub_routes_send_bad_request(req, "invalid BLE protocol probe JSON");
        }
        json_string_value(root, "profile", profile, sizeof(profile));
        const cJSON *duration_ms_item = cJSON_GetObjectItemCaseSensitive(root, "durationMs");
        const cJSON *duration_seconds_item = cJSON_GetObjectItemCaseSensitive(root, "durationSeconds");
        const cJSON *interval_ms_item = cJSON_GetObjectItemCaseSensitive(root, "intervalMs");
        if (cJSON_IsNumber(duration_ms_item)) {
            duration_ms = duration_ms_item->valueint;
        } else if (cJSON_IsNumber(duration_seconds_item)) {
            duration_ms = duration_seconds_item->valueint * 1000;
        }
        if (cJSON_IsNumber(interval_ms_item)) {
            interval_ms = interval_ms_item->valueint;
        }
        cJSON_Delete(root);
    }

    if (protocol_probe_response_mutex == NULL) {
        protocol_probe_response_mutex = xSemaphoreCreateMutexStatic(&protocol_probe_response_mutex_storage);
    }
    if (protocol_probe_response_mutex == NULL
        || xSemaphoreTake(protocol_probe_response_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return hub_routes_send_service_unavailable(req, "BLE protocol probe response buffer is busy");
    }

    char *protocol_probe_response_body = heap_caps_calloc(1, HUB_BLE_PROTOCOL_PROBE_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (protocol_probe_response_body == NULL) {
        protocol_probe_response_body = heap_caps_calloc(1, HUB_BLE_PROTOCOL_PROBE_BODY_BYTES, MALLOC_CAP_8BIT);
    }
    if (protocol_probe_response_body == NULL) {
        xSemaphoreGive(protocol_probe_response_mutex);
        return hub_routes_send_internal_error(req, "BLE protocol probe response buffer allocation failed");
    }

    int64_t started = esp_timer_get_time() / 1000;
    esp_err_t err = hub_ble_protocol_probe_json(
        profile,
        duration_ms,
        interval_ms,
        protocol_probe_response_body,
        HUB_BLE_PROTOCOL_PROBE_BODY_BYTES
    );
    int elapsed_ms = (int)(esp_timer_get_time() / 1000 - started);
    hub_state_record_command(
        "bleProtocolProbe",
        "ble",
        err == ESP_OK,
        elapsed_ms,
        err == ESP_OK ? "BLE protocol probe completed." : hub_ble_last_error()
    );

    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    esp_err_t send_err = hub_routes_send_json(req, protocol_probe_response_body);
    free(protocol_probe_response_body);
    xSemaphoreGive(protocol_probe_response_mutex);
    return send_err;
}

static esp_err_t ble_gatt_read_probe_work_handler(void *context)
{
    httpd_req_t *req = context;
    if (ble_diagnostic_yields_to_interactive_control(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    int duration_ms = 30000;
    int interval_ms = 300;
    char profile[16] = "all";
    if (req->content_len > 0) {
        char request_body[160] = {0};
        if (read_json_body(req, request_body, sizeof(request_body)) != ESP_OK) {
            return hub_routes_send_bad_request(req, "invalid BLE GATT read probe body");
        }
        cJSON *root = cJSON_Parse(request_body);
        if (root == NULL) {
            return hub_routes_send_bad_request(req, "invalid BLE GATT read probe JSON");
        }
        const cJSON *duration_ms_item = cJSON_GetObjectItemCaseSensitive(root, "durationMs");
        const cJSON *duration_seconds_item = cJSON_GetObjectItemCaseSensitive(root, "durationSeconds");
        const cJSON *interval_ms_item = cJSON_GetObjectItemCaseSensitive(root, "intervalMs");
        json_string_value(root, "profile", profile, sizeof(profile));
        if (cJSON_IsNumber(duration_ms_item)) {
            duration_ms = duration_ms_item->valueint;
        } else if (cJSON_IsNumber(duration_seconds_item)) {
            duration_ms = duration_seconds_item->valueint * 1000;
        }
        if (cJSON_IsNumber(interval_ms_item)) {
            interval_ms = interval_ms_item->valueint;
        }
        cJSON_Delete(root);
    }

    if (gatt_read_probe_response_mutex == NULL) {
        gatt_read_probe_response_mutex = xSemaphoreCreateMutexStatic(&gatt_read_probe_response_mutex_storage);
    }
    if (gatt_read_probe_response_mutex == NULL
        || xSemaphoreTake(gatt_read_probe_response_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return hub_routes_send_service_unavailable(req, "BLE GATT read probe response buffer is busy");
    }

    char *gatt_read_probe_response_body = heap_caps_calloc(1, HUB_BLE_PROTOCOL_PROBE_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gatt_read_probe_response_body == NULL) {
        gatt_read_probe_response_body = heap_caps_calloc(1, HUB_BLE_PROTOCOL_PROBE_BODY_BYTES, MALLOC_CAP_8BIT);
    }
    if (gatt_read_probe_response_body == NULL) {
        xSemaphoreGive(gatt_read_probe_response_mutex);
        return hub_routes_send_internal_error(req, "BLE GATT read probe response buffer allocation failed");
    }

    int64_t started = esp_timer_get_time() / 1000;
    esp_err_t err = hub_ble_gatt_read_probe_json(
        profile,
        duration_ms,
        interval_ms,
        gatt_read_probe_response_body,
        HUB_BLE_PROTOCOL_PROBE_BODY_BYTES
    );
    int elapsed_ms = (int)(esp_timer_get_time() / 1000 - started);
    hub_state_record_command(
        "bleGattReadProbe",
        "ble",
        err == ESP_OK,
        elapsed_ms,
        err == ESP_OK ? "BLE GATT read probe completed." : hub_ble_last_error()
    );

    if (err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    esp_err_t send_err = hub_routes_send_json(req, gatt_read_probe_response_body);
    free(gatt_read_probe_response_body);
    xSemaphoreGive(gatt_read_probe_response_mutex);
    return send_err;
}

static esp_err_t ble_validate_noop_write_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char request_body[256] = {0};
    if (read_json_body(req, request_body, sizeof(request_body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing BLE no-op validation body");
    }

    cJSON *root = cJSON_Parse(request_body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }

    bool confirm = false;
    bool include_power = false;
    bool has_confirm = json_bool_value(root, "confirm", &confirm);
    json_bool_value(root, "includePower", &include_power);
    cJSON_Delete(root);

    if (!has_confirm) {
        return hub_routes_send_bad_request(req, "BLE no-op write validation requires confirm=true.");
    }

    hub_ble_write_validation_result_t result = {0};
    esp_err_t err = hub_ble_validate_noop_write(confirm, include_power, &result);
    hub_state_record_command("bleValidateNoopWrite", "ble", result.ok, result.duration_ms, result.message);

    char body[768] = {0};
    snprintf(
        body,
        sizeof(body),
        "{\"ok\":%s,\"transport\":\"ble\",\"durationMs\":%d,\"commands\":[%s],\"message\":\"%s\"}",
        result.ok ? "true" : "false",
        result.duration_ms,
        result.commands_json,
        result.message
    );
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "400 Bad Request");
        return hub_routes_send_json(req, body);
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return hub_routes_send_json(req, body);
    }
    return hub_routes_send_json(req, body);
}

static esp_err_t ble_scan_work_handler(void *context)
{
    httpd_req_t *req = context;
    if (ble_diagnostic_yields_to_interactive_control(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    char request_body[256] = {0};
    if (read_json_body(req, request_body, sizeof(request_body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing BLE scan diagnostic body");
    }

    cJSON *root = cJSON_Parse(request_body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }

    bool confirmed = false;
    bool has_confirm = json_bool_value(root, "confirm", &confirmed);
    cJSON *duration_item = cJSON_GetObjectItemCaseSensitive(root, "durationSeconds");
    double duration_seconds = cJSON_IsNumber(duration_item) ? duration_item->valuedouble : 20.0;
    cJSON_Delete(root);

    if (!has_confirm || !confirmed) {
        return hub_routes_send_bad_request(req, "BLE scan diagnostics requires confirm=true");
    }
    if (duration_seconds < 1.0 || duration_seconds > 30.0) {
        return hub_routes_send_bad_request(req, "durationSeconds must be between 1 and 30");
    }

    char response_body[768] = {0};
    esp_err_t err = hub_ble_scan_diagnostics((int)(duration_seconds * 1000.0), response_body, sizeof(response_body));
    hub_state_record_command("bleScan", "ble", err == ESP_OK, 0, hub_ble_last_error());
    if (err != ESP_OK && response_body[0] == '\0') {
        return hub_routes_send_service_unavailable(req, hub_ble_last_error());
    }
    return hub_routes_send_json(req, response_body);
}

static esp_err_t ble_validate_read_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return queue_ble_diagnostic_request(req, ble_validate_read_work_handler);
}

static esp_err_t ble_full_read_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return queue_ble_diagnostic_request(req, ble_full_read_work_handler);
}

static esp_err_t ble_notify_probe_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return queue_ble_diagnostic_request(req, ble_notify_probe_work_handler);
}

static esp_err_t ble_protocol_probe_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return queue_ble_diagnostic_request(req, ble_protocol_probe_work_handler);
}

static esp_err_t ble_gatt_read_probe_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return queue_ble_diagnostic_request(req, ble_gatt_read_probe_work_handler);
}

static esp_err_t ble_scan_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return queue_ble_diagnostic_request(req, ble_scan_work_handler);
}

static void restart_task(void *param)
{
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    if (xTaskCreate(restart_task, "hub_restart", 2048, NULL, 5, NULL) != pdPASS) {
        return hub_routes_send_internal_error(req, "failed to schedule restart");
    }
    return hub_routes_send_json(req, "{\"ok\":true,\"restartScheduled\":true,\"delayMs\":500}");
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[512] = {0};
    esp_err_t err = hub_ota_status_json(body, sizeof(body));
    if (err != ESP_OK) {
        return hub_routes_send_internal_error(req, "failed to build OTA status");
    }
    return hub_routes_send_json(req, body);
}

static esp_err_t send_ota_update_error(httpd_req_t *req, esp_err_t err, const hub_ota_update_t *update)
{
    const char *message = update != NULL && update->message[0] != '\0'
        ? update->message
        : esp_err_to_name(err);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NOT_FOUND) {
        return hub_routes_send_bad_request(req, message);
    }
    return hub_routes_send_internal_error(req, message);
}

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    if (!request_header_matches(req, "X-LamaBuddy-OTA-Confirm", "true")) {
        return hub_routes_send_bad_request(req, "OTA update requires X-LamaBuddy-OTA-Confirm: true");
    }
    if (!request_content_type_contains(req, "application/octet-stream")) {
        return hub_routes_send_bad_request(req, "OTA update requires application/octet-stream");
    }
    if (req->content_len == 0) {
        return hub_routes_send_bad_request(req, "OTA update body is empty");
    }

    hub_ota_update_t update = {0};
    esp_err_t err = hub_ota_begin_update(req->content_len, &update);
    if (err != ESP_OK) {
        return send_ota_update_error(req, err, &update);
    }

    char *buffer = heap_caps_malloc(HUB_OTA_RECV_CHUNK_BYTES, MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        hub_ota_abort(&update);
        return hub_routes_send_internal_error(req, "failed to allocate OTA receive buffer");
    }

    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t to_read = remaining > HUB_OTA_RECV_CHUNK_BYTES ? HUB_OTA_RECV_CHUNK_BYTES : remaining;
        int read_len = httpd_req_recv(req, buffer, to_read);
        if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (read_len <= 0) {
            free(buffer);
            hub_ota_abort(&update);
            return hub_routes_send_internal_error(req, "failed to receive OTA body");
        }

        err = hub_ota_write(&update, buffer, (size_t)read_len);
        if (err != ESP_OK) {
            free(buffer);
            hub_ota_abort(&update);
            return send_ota_update_error(req, err, &update);
        }
        remaining -= (size_t)read_len;
    }
    free(buffer);

    err = hub_ota_finish(&update);
    if (err != ESP_OK) {
        return send_ota_update_error(req, err, &update);
    }

    char body[384] = {0};
    snprintf(
        body,
        sizeof(body),
        "{\"ok\":true,\"bytesWritten\":%lu,\"partition\":\"%s\",\"restartRequired\":true,\"message\":\"%s\"}",
        (unsigned long)update.written,
        update.partition != NULL ? update.partition->label : "",
        update.message
    );
    hub_state_record_command("otaUpdate", "wifi", true, 0, "OTA image staged");
    return hub_routes_send_json(req, body);
}

static esp_err_t cloud_diagnostics_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_cloud_diagnostics_json());
}

static esp_err_t cloud_refresh_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    esp_err_t err = hub_cloud_request_refresh();
    if (err == ESP_OK || hub_cloud_refresh_pending()) {
        httpd_resp_set_status(req, "202 Accepted");
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    return hub_routes_send_json(req, hub_state_cloud_diagnostics_json());
}

static esp_err_t cloud_machines_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    return hub_routes_send_json(req, hub_state_cloud_machines_json());
}

static esp_err_t cloud_websocket_diagnostics_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return hub_routes_send_json(req, hub_cloud_websocket_diagnostics_json());
}

static esp_err_t cloud_websocket_config_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[256] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing Cloud WebSocket config body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }

    bool confirm = false;
    bool enabled = false;
    bool merge_enabled = false;
    bool has_confirm = json_bool_value(root, "confirm", &confirm);
    bool has_enabled = json_bool_value(root, "enabled", &enabled);
    cJSON *merge_item = cJSON_GetObjectItemCaseSensitive(root, "mergeEnabled");
    bool has_merge = merge_item != NULL;
    bool merge_valid = !has_merge || json_bool_value(root, "mergeEnabled", &merge_enabled);
    cJSON *mask_item = cJSON_GetObjectItemCaseSensitive(root, "fieldMask");
    uint32_t field_mask = UINT32_MAX;
    bool mask_valid = true;
    if (mask_item != NULL) {
        if (!cJSON_IsNumber(mask_item)
            || !(mask_item->valuedouble >= 0.0 && mask_item->valuedouble <= (double)UINT32_MAX)) {
            mask_valid = false;
        } else {
            field_mask = (uint32_t)mask_item->valuedouble;
            mask_valid = (double)field_mask == mask_item->valuedouble;
        }
    }
    cJSON_Delete(root);

    if (!has_confirm || !confirm) {
        return hub_routes_send_bad_request(req, "Cloud WebSocket config requires confirm=true");
    }
    if (!has_enabled || !merge_valid || !mask_valid) {
        return hub_routes_send_bad_request(req, "invalid enabled, mergeEnabled, or fieldMask");
    }
    if (merge_enabled) {
        httpd_resp_set_status(req, "409 Conflict");
        return hub_routes_send_json(
            req,
            "{\"ok\":false,\"errorCode\":\"websocketMergeGateClosed\","
            "\"message\":\"Observe-only hardware validation must pass before merge can be enabled.\"}"
        );
    }

    hub_cloud_websocket_config_t config = {
        .enabled = enabled,
        .merge_enabled = false,
        .field_mask = field_mask,
    };
    esp_err_t err = hub_cloud_websocket_apply_config(&config);
    if (err != ESP_OK) {
        return hub_routes_send_internal_error(req, "failed to save Cloud WebSocket config");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return hub_routes_send_json(req, hub_cloud_websocket_diagnostics_json());
}

static esp_err_t bbw_dose_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[256] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing Brew-by-Weight command body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }

    hub_command_request_t command = {.kind = HUB_COMMAND_BBW_DOSE};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_string_value(
            root,
            "dose",
            command.value.bbw_dose.dose,
            sizeof(command.value.bbw_dose.dose)
        )
        && json_int_value(root, "grams", &command.value.bbw_dose.grams);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing dose or grams");
    }
    return submit_control_command(req, &command);
}

static esp_err_t quick_weight_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[256] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing quick-weight command body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }

    hub_command_request_t command = {.kind = HUB_COMMAND_QUICK_WEIGHT};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_int_value(root, "grams", &command.value.quick_weight.grams);
    const cJSON *require_no_match = cJSON_GetObjectItemCaseSensitive(root, "requireNoStoredMatch");
    bool preset_ok = require_no_match == NULL || cJSON_IsBool(require_no_match);
    command.value.quick_weight.require_no_stored_match = cJSON_IsTrue(require_no_match);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing grams");
    }
    if (!preset_ok) {
        return hub_routes_send_bad_request(req, "requireNoStoredMatch must be a boolean");
    }
    return submit_control_command(req, &command);
}

static esp_err_t bbw_mode_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[256] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing Brew-by-Weight mode command body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }

    hub_command_request_t command = {.kind = HUB_COMMAND_BBW_MODE};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_string_value(
        root,
        "mode",
        command.value.bbw_mode.mode,
        sizeof(command.value.bbw_mode.mode)
    );
    const cJSON *preset_grams = cJSON_GetObjectItemCaseSensitive(root, "presetGrams");
    bool preset_ok = true;
    if (preset_grams != NULL) {
        preset_ok = cJSON_IsNumber(preset_grams)
            && preset_grams->valuedouble >= 0.0
            && preset_grams->valuedouble <= 200.0
            && (strcmp(command.value.bbw_mode.mode, "Dose1") == 0
                || strcmp(command.value.bbw_mode.mode, "Dose2") == 0);
        if (preset_ok) {
            command.value.bbw_mode.preset_grams = (int)preset_grams->valuedouble;
            preset_ok = (double)command.value.bbw_mode.preset_grams == preset_grams->valuedouble;
            command.value.bbw_mode.has_preset_grams = preset_ok;
        }
    }
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing mode");
    }
    if (!preset_ok) {
        return hub_routes_send_bad_request(req, "presetGrams requires Dose1 or Dose2 and an integer from 0 to 200");
    }
    return submit_control_command(req, &command);
}

static esp_err_t schedule_control_handler(httpd_req_t *req)
{
    if(require_authorized(req)!=ESP_OK) return ESP_OK;
    char body[1536]={0};
    if(read_json_body(req,body,sizeof(body))!=ESP_OK) return hub_routes_send_bad_request(req,"missing schedule command body");
    cJSON *root=cJSON_ParseWithOpts(body,NULL,true);
    if(!root) return hub_routes_send_bad_request(req,"invalid JSON body");
    hub_command_request_t command={.kind=HUB_COMMAND_SCHEDULE};
    bool ok=json_optional_request_id(root,command.request_id,sizeof(command.request_id))
        && command.request_id[0] && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"confirm"))
        && hub_schedule_parse(cJSON_GetObjectItemCaseSensitive(root,"expected"),&command.value.schedule.expected)
        && hub_schedule_parse(cJSON_GetObjectItemCaseSensitive(root,"schedule"),&command.value.schedule.requested)
        && !strcmp(command.value.schedule.expected.id,command.value.schedule.requested.id)
        && (!command.value.schedule.requested.enabled||(command.value.schedule.requested.days
            && command.value.schedule.requested.on_minutes!=command.value.schedule.requested.off_minutes));
    command.value.schedule.confirm=true;
    bool metadata_ok=apply_v2_command_metadata(req,root,&command);
    cJSON_Delete(root);
    if(!ok||!metadata_ok) return hub_routes_send_bad_request(req,"invalid schedule, expected values, confirmation or v2 metadata");
    return submit_control_command(req,&command);
}

static esp_err_t prebrew_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[256] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing prebrew command body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }

    hub_command_request_t command = {.kind = HUB_COMMAND_PREBREW};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_string_value(
            root,
            "field",
            command.value.prebrew.field,
            sizeof(command.value.prebrew.field)
        )
        && json_control_value(
            root,
            "value",
            command.value.prebrew.value,
            sizeof(command.value.prebrew.value)
        );
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing field or value");
    }
    return submit_control_command(req, &command);
}

static esp_err_t coffee_temperature_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing coffee temperature command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_COFFEE_TEMPERATURE};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_double_value(root, "temperature", &command.value.temperature.temperature);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing temperature");
    }
    return submit_control_command(req, &command);
}

static esp_err_t steam_level_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing steam level command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_STEAM_LEVEL};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_int_value(root, "level", &command.value.steam_level.level);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing level");
    }
    return submit_control_command(req, &command);
}

static esp_err_t steam_enabled_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing steam enabled command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_STEAM_ENABLED};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_bool_value(root, "enabled", &command.value.enabled.enabled);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing enabled");
    }
    return submit_control_command(req, &command);
}

static esp_err_t steam_temperature_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing steam temperature command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_STEAM_TEMPERATURE};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_double_value(root, "temperature", &command.value.temperature.temperature);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing temperature");
    }
    return submit_control_command(req, &command);
}

static esp_err_t smart_standby_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[256] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing smart standby command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_SMART_STANDBY};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_bool_value(root, "enabled", &command.value.smart_standby.enabled)
        && json_int_value(root, "minutes", &command.value.smart_standby.minutes)
        && json_string_value(
            root,
            "mode",
            command.value.smart_standby.mode,
            sizeof(command.value.smart_standby.mode)
    );
    json_bool_value(root, "confirm", &command.value.smart_standby.confirm);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing enabled, minutes, or mode");
    }
    return submit_control_command(req, &command);
}

static esp_err_t power_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing power command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_POWER};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    bool ok = json_bool_value(root, "enabled", &command.value.power.enabled);
    json_bool_value(root, "confirm", &command.value.power.confirm);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    if (!ok) {
        return hub_routes_send_bad_request(req, "missing enabled");
    }
    return submit_control_command(req, &command);
}

static esp_err_t backflush_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing backflush command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_BACKFLUSH};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    json_bool_value(root, "confirm", &command.value.confirmation.confirm);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    return submit_control_command(req, &command);
}

static esp_err_t machine_firmware_update_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) {
        return hub_routes_send_bad_request(req, "missing machine firmware update command body");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return hub_routes_send_bad_request(req, "invalid JSON body");
    }
    hub_command_request_t command = {.kind = HUB_COMMAND_MACHINE_FIRMWARE_UPDATE};
    bool request_id_ok = json_optional_request_id(
        root,
        command.request_id,
        sizeof(command.request_id)
    );
    json_bool_value(root, "confirm", &command.value.confirmation.confirm);
    bool metadata_ok = apply_v2_command_metadata(req, root, &command);
    cJSON_Delete(root);
    if (!request_id_ok) {
        return hub_routes_send_bad_request(req, "invalid requestId");
    }
    if (!metadata_ok) {
        return hub_routes_send_bad_request(req, "invalid v2 command metadata");
    }
    return submit_control_command(req, &command);
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) {
        return ESP_OK;
    }

    esp_err_t err = hub_config_factory_reset();
    if (err != ESP_OK) {
        return hub_routes_send_internal_error(req, "failed to factory reset");
    }
    return hub_routes_send_json(req, "{\"ok\":true,\"restartRequired\":true}");
}

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    bool legacy_only;
} hub_http_route_t;

#ifdef HUB_GRINDER
static esp_err_t grinder_state_handler(httpd_req_t *req)
{
    if(require_authorized(req)!=ESP_OK) return ESP_OK;
    char *body=heap_caps_malloc(6144,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!body) return hub_routes_send_service_unavailable(req,"snapshot unavailable");
    bool ready=hub_grinder_state(body,6144);
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t err=ready?hub_routes_send_json(req,body):hub_routes_send_service_unavailable(req,"snapshot busy");
    free(body); return err;
}
static esp_err_t grinder_mode_handler(httpd_req_t *req)
{
    if(require_authorized(req)!=ESP_OK) return ESP_OK;
    char body[96]={0};
    if(read_json_body(req,body,sizeof(body))!=ESP_OK) return hub_routes_send_bad_request(req,"invalid mode body");
    cJSON *j=cJSON_Parse(body); bool active=false,confirm=false;
    bool valid=json_bool_value(j,"active",&active)&&json_bool_value(j,"confirm",&confirm)&&confirm;
    cJSON_Delete(j);
    if(!valid) return hub_routes_send_bad_request(req,"active and confirm are required booleans");
    if(!hub_grinder_set_active(active)) return hub_routes_send_service_unavailable(req,"grinder not configured or maintenance active");
    return hub_routes_send_json(req,"{\"accepted\":true,\"confirmationPending\":true}");
}
static esp_err_t grinder_recipe_config_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) return ESP_OK;
    char body[256] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) return hub_routes_send_bad_request(req, "invalid recipe configuration");
    cJSON *j = cJSON_Parse(body); bool enabled = false;
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(j, "ownerPassword");
    bool valid = json_bool_value(j, "enabled", &enabled) && (!password ||
        (cJSON_IsString(password) && password->valuestring[0] && strlen(password->valuestring) <= 64));
    bool ok = valid && hub_grinder_configure_recipe(enabled, password ? password->valuestring : NULL);
    if (cJSON_IsString(password)) memset(password->valuestring, 0, strlen(password->valuestring));
    cJSON_Delete(j); memset(body, 0, sizeof(body));
    if (!valid) return hub_routes_send_bad_request(req, "enabled and optional ownerPassword required");
    if (!ok) return hub_routes_send_service_unavailable(req, "recipe configuration unavailable or grinder busy");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return hub_routes_send_json(req, "{\"saved\":true}");
}
static esp_err_t grinder_recipe_probe_handler(httpd_req_t *req)
{
    if (require_authorized(req) != ESP_OK) return ESP_OK;
    char body[96] = {0};
    if (read_json_body(req, body, sizeof(body)) != ESP_OK) return hub_routes_send_bad_request(req, "invalid probe body");
    cJSON *j = cJSON_Parse(body);
    const cJSON *uuid = cJSON_GetObjectItemCaseSensitive(j, "grindUuid");
    bool valid = cJSON_IsString(uuid) && hub_command_request_id_valid(uuid->valuestring);
    bool ok = valid && hub_grinder_probe_recipe(uuid->valuestring);
    cJSON_Delete(j);
    if (!valid) return hub_routes_send_bad_request(req, "grindUuid required");
    if (!ok) return hub_routes_send_service_unavailable(req, "grinder is busy");
    return hub_routes_send_json(req, "{\"accepted\":true,\"readOnly\":true}");
}
#endif

#ifdef HUB_TOUCHSCREEN
static esp_err_t display_diagnostics_handler(httpd_req_t *req)
{
    if(require_authorized(req)!=ESP_OK) return ESP_OK;
    char body[1536];
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return gbs_display_diagnostics_json(body,sizeof(body))?hub_routes_send_json(req,body):
        hub_routes_send_service_unavailable(req,"display diagnostics unavailable");
}

static esp_err_t display_frame_handler(httpd_req_t *req)
{
    if(require_authorized(req)!=ESP_OK) return ESP_OK;
    void *frame=heap_caps_malloc(GBS_DISPLAY_FRAME_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!frame) return hub_routes_send_service_unavailable(req,"display frame unavailable");
    if(!gbs_display_copy_frame(frame,GBS_DISPLAY_FRAME_BYTES)) {
        free(frame); return hub_routes_send_service_unavailable(req,"display is not ready");
    }
    httpd_resp_set_type(req,"application/octet-stream");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Display-Format","RGB565LE");
    char width[8],height[8];
    snprintf(width,sizeof(width),"%u",(unsigned)GBS_DISPLAY_WIDTH);
    snprintf(height,sizeof(height),"%u",(unsigned)GBS_DISPLAY_HEIGHT);
    httpd_resp_set_hdr(req,"X-Display-Width",width);
    httpd_resp_set_hdr(req,"X-Display-Height",height);
    esp_err_t err=httpd_resp_send(req,frame,GBS_DISPLAY_FRAME_BYTES);
    free(frame); return err;
}
#endif

static const hub_http_route_t HUB_HTTP_ROUTES[] = {
#ifdef HUB_TOUCHSCREEN
    {"/diagnostics/display", HTTP_GET, display_diagnostics_handler, false},
    {"/diagnostics/display/frame", HTTP_GET, display_frame_handler, false},
#endif
#ifdef HUB_GRINDER
    {"/diagnostics/grinder", HTTP_GET, grinder_state_handler, false},
    {"/controls/grinder-mode", HTTP_POST, grinder_mode_handler, false},
    {"/controls/grinder-recipe", HTTP_POST, grinder_recipe_config_handler, false},
    {"/diagnostics/grinder/recipe-probe", HTTP_POST, grinder_recipe_probe_handler, false},
#endif
    {"/", HTTP_GET, setup_page_handler, true},
    {"/setup/wifi", HTTP_POST, setup_wifi_handler, true},
    {"/setup/cloud", HTTP_POST, setup_cloud_handler, false},
    {"/setup/machine", HTTP_POST, setup_machine_handler, false},
    {"/health", HTTP_GET, health_handler, false},
    {"/machine/state", HTTP_GET, machine_state_handler, false},
    {"/machine/bbw-selection", HTTP_POST, bbw_selection_handler, false},
    {"/machine/binding", HTTP_GET, machine_binding_handler, false},
    {"/machine/capabilities", HTTP_GET, capabilities_handler, false},
    {"/machine/statistics", HTTP_GET, statistics_handler, false},
    {"/machine/schedule", HTTP_GET, schedule_handler, false},
    {"/machine/live-state", HTTP_GET, live_state_handler, false},
    {"/machine/ble-state", HTTP_GET, ble_state_handler, false},
    {"/diagnostics/transports", HTTP_GET, transports_handler, false},
    {"/diagnostics/recent-commands", HTTP_GET, recent_commands_handler, false},
    {"/diagnostics/commands", HTTP_GET, command_diagnostics_handler, false},
    {"/diagnostics/setup-events", HTTP_GET, setup_events_handler, false},
    {"/diagnostics/slow-cloud-reads", HTTP_GET, slow_cloud_reads_handler, false},
    {"/diagnostics/slow-cloud-reads/refresh", HTTP_POST, slow_cloud_reads_refresh_handler, false},
    {"/diagnostics/ble", HTTP_GET, ble_diagnostics_handler, false},
    {"/diagnostics/ble/validate-read", HTTP_POST, ble_validate_read_handler, false},
    {"/diagnostics/ble/full-read", HTTP_POST, ble_full_read_handler, false},
    {"/diagnostics/ble/notify-probe", HTTP_POST, ble_notify_probe_handler, false},
    {"/diagnostics/ble/protocol-probe", HTTP_POST, ble_protocol_probe_handler, false},
    {"/diagnostics/ble/gatt-read-probe", HTTP_POST, ble_gatt_read_probe_handler, false},
    {"/diagnostics/ble/validate-noop-write", HTTP_POST, ble_validate_noop_write_handler, false},
    {"/diagnostics/ble/scan", HTTP_POST, ble_scan_handler, false},
    {"/diagnostics/firmware", HTTP_GET, firmware_handler, false},
    {"/diagnostics/cloud", HTTP_GET, cloud_diagnostics_handler, false},
    {"/diagnostics/cloud/refresh", HTTP_POST, cloud_refresh_handler, false},
    {"/diagnostics/cloud/machines", HTTP_GET, cloud_machines_handler, false},
    {"/diagnostics/cloud-websocket", HTTP_GET, cloud_websocket_diagnostics_handler, false},
    {"/diagnostics/cloud-websocket/config", HTTP_POST, cloud_websocket_config_handler, false},
    {"/diagnostics/restart", HTTP_POST, restart_handler, false},
    {"/diagnostics/ota", HTTP_GET, ota_status_handler, false},
    {"/diagnostics/ota/update", HTTP_POST, ota_update_handler, false},
    {"/diagnostics/factory-reset", HTTP_POST, factory_reset_handler, false},
    {"/controls/quick-weight", HTTP_POST, quick_weight_handler, false},
    {"/controls/bbw/dose", HTTP_POST, bbw_dose_handler, false},
    {"/controls/bbw/mode", HTTP_POST, bbw_mode_handler, false},
    {"/controls/coffee-temperature", HTTP_POST, coffee_temperature_handler, false},
    {"/controls/steam-level", HTTP_POST, steam_level_handler, false},
    {"/controls/steam/enabled", HTTP_POST, steam_enabled_handler, false},
    {"/controls/steam-temperature", HTTP_POST, steam_temperature_handler, false},
    {"/controls/smart-standby", HTTP_POST, smart_standby_handler, false},
    {"/controls/power", HTTP_POST, power_handler, false},
    {"/controls/backflush", HTTP_POST, backflush_handler, false},
    {"/controls/machine-firmware/update", HTTP_POST, machine_firmware_update_handler, false},
    {"/controls/prebrew", HTTP_POST, prebrew_handler, false},
    {"/controls/schedule", HTTP_POST, schedule_control_handler, false},
};

bool hub_http_local_lock(void)
{
    return route_dispatch_mutex && xSemaphoreTake(route_dispatch_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

void hub_http_local_unlock(void)
{
    xSemaphoreGive(route_dispatch_mutex);
}

static esp_err_t dispatch_route(httpd_req_t *req)
{
    const hub_http_route_t *route = (const hub_http_route_t *)req->user_ctx;
    if (route == NULL || route->handler == NULL) {
        return hub_routes_send_internal_error(req, "route dispatch unavailable");
    }
    if (!hub_http_request_is_secure(req)
        && request_header_matches(req, HUB_API_V2_HEADER_NAME, "2")) {
        return hub_routes_send_upgrade_required(req);
    }
#ifdef HUB_TOUCHSCREEN
    // Frames own their buffer; a slow viewer must not block local UI state reads.
    if (route->handler == display_frame_handler) return route->handler(req);
#endif
    if (xSemaphoreTake(route_dispatch_mutex, portMAX_DELAY) != pdTRUE) {
        return hub_routes_send_service_unavailable(req, "route dispatch busy");
    }
    esp_err_t err = route->handler(req);
    xSemaphoreGive(route_dispatch_mutex);
    return err;
}

esp_err_t hub_http_register_routes(
    httpd_handle_t server,
    hub_http_transport_t transport
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (route_dispatch_mutex == NULL) {
        route_dispatch_mutex = xSemaphoreCreateMutexStatic(&route_dispatch_mutex_storage);
    }
    if (route_dispatch_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if ((sizeof(HUB_HTTP_ROUTES) / sizeof(HUB_HTTP_ROUTES[0]))
        > HUB_HTTP_MAX_URI_HANDLERS) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t index = 0; index < sizeof(HUB_HTTP_ROUTES) / sizeof(HUB_HTTP_ROUTES[0]); index++) {
        const hub_http_route_t *route = &HUB_HTTP_ROUTES[index];
        if (transport == HUB_HTTP_TRANSPORT_HTTPS_V2 && route->legacy_only) {
            continue;
        }
        httpd_uri_t config = {
            .uri = route->uri,
            .method = route->method,
            .handler = dispatch_route,
            .user_ctx = (void *)route,
        };
        esp_err_t err = httpd_register_uri_handler(server, &config);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t hub_http_start(void)
{
    ESP_RETURN_ON_ERROR(
        start_ble_diagnostic_worker(),
        TAG,
        "BLE diagnostic worker failed"
    );
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = LAMABUDDY_HTTP_PORT;
    config.max_uri_handlers = HUB_HTTP_MAX_URI_HANDLERS;
    config.max_open_sockets = HUB_HTTP_LEGACY_MAX_OPEN_SOCKETS;
    config.stack_size = 16384;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.global_user_ctx = hub_http_server_context(HUB_HTTP_TRANSPORT_LEGACY);

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server failed");
    esp_err_t route_err = hub_http_register_routes(server, HUB_HTTP_TRANSPORT_LEGACY);
    if (route_err != ESP_OK) {
        (void)httpd_stop(server);
        return route_err;
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", LAMABUDDY_HTTP_PORT);
    return ESP_OK;
}
