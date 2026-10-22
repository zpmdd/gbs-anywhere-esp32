#include "hub_cloud.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub_config.h"
#include "hub_command_coordinator.h"
#include "hub_http_response.h"
#include "hub_state_keeper.h"
#include "miniz.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

static const char *TAG = "hub_cloud";
static const char *CUSTOMER_APP_URL = "https://lion.lamarzocco.io/api/customer-app";
static const int64_t ACCESS_TOKEN_CACHE_TTL_MS = 45LL * 60LL * 1000LL;
static const int64_t ACCESS_TOKEN_PROACTIVE_RENEWAL_MS = 40LL * 60LL * 1000LL;
static const int64_t TOKEN_RENEWAL_RETRY_DELAY_MS = 60000;
static const int CLOUD_COMMAND_HTTP_TIMEOUT_MS = 12000;
static const int CLOUD_COMMAND_CONNECT_RETRY_DELAY_MS = 500;
static const int CLOUD_READONLY_HTTP_TIMEOUT_MS = 4000;
static const int CLOUD_STATISTICS_HTTP_TIMEOUT_MS = 6500;
static const int CLOUD_STATISTICS_RETRY_BASE_DELAY_MS = 400;
static const int CLOUD_STATISTICS_ENDPOINT_PAUSE_MS = 300;
/*
 * sdkconfig routes ordinary allocations up to 4 KB to scarce internal RAM.
 * esp_http_client allocates its RX/TX buffers with malloc(), so the previous
 * 1 KB buffers consumed and fragmented internal-only headroom on every TLS
 * connection. An 8 KB buffer is intentionally above that threshold and is
 * therefore backed by PSRAM under CONFIG_SPIRAM_USE_MALLOC.
 */
static const int CLOUD_HTTP_CLIENT_BUFFER_BYTES = 8192;
static const int64_t CLOUD_COMMAND_AFTER_IO_COOLDOWN_MS = 500;
static const uint32_t CLOUD_COMMAND_HEAP_SETTLE_MAX_MS = 3000;
static const uint32_t CLOUD_COMMAND_HEAP_SETTLE_POLL_MS = 100;
static const size_t CLOUD_COMMAND_MIN_INTERNAL_BLOCK_BYTES = 6144;
// A foreground command cannot safely preempt esp_http_client_perform() midway
// through a TLS response. Keep its lock wait above the longest single
// statistics request so the command takes ownership at the next safe boundary
// instead of failing locally before any upstream write is attempted.
static const uint32_t CLOUD_COMMAND_REFRESH_WAIT_MS = 12000;
static const uint32_t CLOUD_READONLY_IO_WAIT_MS = 3000;
static const uint32_t CLOUD_READONLY_HEAP_SETTLE_MAX_MS = 5000;
static const uint32_t CLOUD_READONLY_HEAP_SETTLE_POLL_MS = 100;
static const size_t CLOUD_READONLY_MIN_INTERNAL_BLOCK_BYTES = 4096;
static const uint32_t CLOUD_TOKEN_MUTEX_WAIT_MS = 1000;

typedef struct {
    char installation_id[40];
    char private_key_b64[256];
    unsigned char secret[32];
} installation_key_t;

typedef hub_http_response_t http_response_t;

typedef enum {
    READONLY_DIAG_THINGS = 0,
    READONLY_DIAG_DASHBOARD,
    READONLY_DIAG_COUNTER,
    READONLY_DIAG_RECENT,
    READONLY_DIAG_TREND,
    READONLY_DIAG_SCHEDULE,
    READONLY_DIAG_FIRMWARE,
    READONLY_DIAG_COUNT,
} readonly_diag_kind_t;

typedef struct {
    esp_err_t err;
    int status_code;
    int duration_ms;
    size_t response_bytes;
    uint32_t first_bytes;
    uint32_t free_heap;
    uint32_t internal_largest;
    uint32_t attempts;
} readonly_http_diag_t;

typedef struct {
    char access_token[HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1];
    char refresh_token[HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1];
} cloud_tokens_t;

typedef struct {
    char serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    uint32_t selection_generation;
    bool connected;
    bool coffee_temperature_set;
    double coffee_temperature;
    bool steam_level_set;
    int steam_level;
    char coffee_boiler_ready_time[32];
    char steam_boiler_ready_time[32];
    bool total_coffee_set;
    int total_coffee;
    bool total_flush_set;
    int total_flush;
    bool dose1_set;
    double dose1;
    bool dose2_set;
    double dose2;
    char bbw_mode[32];
    bool bbw_scale_connected_set;
    bool bbw_scale_connected;
    bool thing_scale_connected_set;
    bool thing_scale_connected;
    bool scale_battery_level_set;
    double scale_battery_level;
    int64_t scale_observed_ms;
    bool scale_calibration_required_set;
    bool scale_calibration_required;
    char scale_name[64];
    char prebrew_mode[32];
    bool prebrew_on_set;
    double prebrew_on;
    bool prebrew_off_set;
    double prebrew_off;
    char last_updated[32];
} cloud_snapshot_t;

typedef struct {
    bool dose1_set;
    double dose1;
    bool dose2_set;
    double dose2;
    char mode[32];
} bbw_snapshot_fields_t;

static hub_cloud_status_t status = HUB_CLOUD_STATUS_UNCONFIGURED;
static char machine_name[HUB_CONFIG_MAX_MACHINE_NAME_LEN + 1] = "";
static char machine_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = "";
static char last_error[192] = "Cloud is not configured on ESP32-S3.";
static int64_t last_refresh_ms = 0;
static bool machine_selected = false;
static bool ble_token_cached = false;
static bool cached_snapshot = false;
static bool sntp_initialized = false;
static bool command_refresh_pending = false;
static uint32_t command_refresh_generation = 0;
static bool refresh_running = false;
static SemaphoreHandle_t refresh_mutex = NULL;
static portMUX_TYPE refresh_mutex_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t token_mutex = NULL;
static portMUX_TYPE token_mutex_lock = portMUX_INITIALIZER_UNLOCKED;
static bool cloud_command_waiting = false;
static int64_t last_cloud_io_completed_ms = 0;
static portMUX_TYPE cloud_io_priority_lock = portMUX_INITIALIZER_UNLOCKED;
static cloud_snapshot_t snapshot = {0};
static portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static cloud_tokens_t cached_tokens = {0};
static int64_t cached_tokens_ms = 0;
static int64_t token_renewal_retry_after_ms = 0;
static char last_auth_step[24] = "";
static esp_err_t last_auth_err = ESP_OK;
static int last_auth_status_code = 0;
static char installation_identity_sha256[65] = "";
static uint32_t auth_refresh_success_count = 0;
static uint32_t auth_refresh_failure_count = 0;
static int64_t auth_refresh_last_success_uptime_ms = 0;
static portMUX_TYPE auth_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static readonly_http_diag_t readonly_http_diags[READONLY_DIAG_COUNT] = {0};
static portMUX_TYPE readonly_http_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static bool cached_snapshot_matches_selection(void);

static void copy_snapshot(cloud_snapshot_t *out, bool *has_cached_snapshot)
{
    portENTER_CRITICAL(&snapshot_lock);
    if (out != NULL) {
        *out = snapshot;
    }
    if (has_cached_snapshot != NULL) {
        *has_cached_snapshot = cached_snapshot;
    }
    portEXIT_CRITICAL(&snapshot_lock);
}

static const char *readonly_diag_name(readonly_diag_kind_t kind)
{
    switch (kind) {
    case READONLY_DIAG_THINGS:
        return "things";
    case READONLY_DIAG_DASHBOARD:
        return "dashboard";
    case READONLY_DIAG_COUNTER:
        return "counter";
    case READONLY_DIAG_RECENT:
        return "recent";
    case READONLY_DIAG_TREND:
        return "trend";
    case READONLY_DIAG_SCHEDULE:
        return "schedule";
    case READONLY_DIAG_FIRMWARE:
        return "firmware";
    case READONLY_DIAG_COUNT:
        break;
    }
    return "unknown";
}

static void record_readonly_http_diag(
    readonly_diag_kind_t kind,
    esp_err_t err,
    int status_code,
    int duration_ms,
    size_t response_bytes,
    uint32_t first_bytes
)
{
    if ((int)kind < 0 || kind >= READONLY_DIAG_COUNT) {
        return;
    }

    /*
     * Heap inspection takes the allocator lock. Never call it while holding
     * the cross-core diagnostics spinlock: the HTTP diagnostics task may be
     * allocating at the same time, which can otherwise deadlock one core and
     * leave a statistics refresh permanently running.
     */
    uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
    uint32_t internal_largest =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    portENTER_CRITICAL(&readonly_http_diag_lock);
    readonly_http_diags[kind].err = err;
    readonly_http_diags[kind].status_code = status_code;
    readonly_http_diags[kind].duration_ms = duration_ms;
    readonly_http_diags[kind].response_bytes = response_bytes;
    readonly_http_diags[kind].first_bytes = first_bytes;
    readonly_http_diags[kind].free_heap = free_heap;
    readonly_http_diags[kind].internal_largest = internal_largest;
    readonly_http_diags[kind].attempts++;
    portEXIT_CRITICAL(&readonly_http_diag_lock);
}

static void clear_cached_fields(void)
{
    machine_name[0] = '\0';
    machine_serial[0] = '\0';
    machine_selected = false;
    ble_token_cached = false;
    portENTER_CRITICAL(&snapshot_lock);
    cached_snapshot = false;
    memset(&snapshot, 0, sizeof(snapshot));
    portEXIT_CRITICAL(&snapshot_lock);
}

static esp_err_t ensure_refresh_mutex(void)
{
    if (refresh_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&refresh_mutex_lock);
    if (refresh_mutex == NULL) {
        refresh_mutex = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&refresh_mutex_lock);

    if (created != NULL) {
        vSemaphoreDelete(created);
    }
    return ESP_OK;
}

static esp_err_t ensure_token_mutex(void)
{
    if (token_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&token_mutex_lock);
    if (token_mutex == NULL) {
        token_mutex = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&token_mutex_lock);

    if (created != NULL) {
        vSemaphoreDelete(created);
    }
    return ESP_OK;
}

static void set_cloud_command_waiting(bool waiting)
{
    portENTER_CRITICAL(&cloud_io_priority_lock);
    cloud_command_waiting = waiting;
    portEXIT_CRITICAL(&cloud_io_priority_lock);
}

static bool cloud_command_is_waiting(void)
{
    bool waiting = false;
    portENTER_CRITICAL(&cloud_io_priority_lock);
    waiting = cloud_command_waiting;
    portEXIT_CRITICAL(&cloud_io_priority_lock);
    return waiting;
}

static void mark_cloud_io_completed(void)
{
    portENTER_CRITICAL(&cloud_io_priority_lock);
    last_cloud_io_completed_ms = esp_timer_get_time() / 1000;
    portEXIT_CRITICAL(&cloud_io_priority_lock);
}

static int64_t cloud_command_io_cooldown_remaining_ms(void)
{
    int64_t last_completed = 0;
    portENTER_CRITICAL(&cloud_io_priority_lock);
    last_completed = last_cloud_io_completed_ms;
    portEXIT_CRITICAL(&cloud_io_priority_lock);
    if (last_completed <= 0) {
        return 0;
    }
    int64_t elapsed = esp_timer_get_time() / 1000 - last_completed;
    return elapsed < CLOUD_COMMAND_AFTER_IO_COOLDOWN_MS
        ? CLOUD_COMMAND_AFTER_IO_COOLDOWN_MS - elapsed
        : 0;
}

static esp_err_t cloud_command_wait_for_internal_heap(
    const hub_command_deadline_t *deadline,
    uint32_t required_after_ms
)
{
    uint32_t waited_ms = 0;
    while (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        < CLOUD_COMMAND_MIN_INTERNAL_BLOCK_BYTES) {
        if (waited_ms >= CLOUD_COMMAND_HEAP_SETTLE_MAX_MS) {
            return ESP_ERR_NO_MEM;
        }
        int64_t after_poll_ms = esp_timer_get_time() / 1000
            + CLOUD_COMMAND_HEAP_SETTLE_POLL_MS;
        if (!hub_command_deadline_can_start_step(
                deadline,
                after_poll_ms,
                required_after_ms
            )) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(CLOUD_COMMAND_HEAP_SETTLE_POLL_MS));
        waited_ms += CLOUD_COMMAND_HEAP_SETTLE_POLL_MS;
    }
    return ESP_OK;
}

static void cloud_command_io_cancel_lock(void)
{
    xSemaphoreGive(refresh_mutex);
    set_cloud_command_waiting(false);
}

static esp_err_t cloud_command_io_lock(
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms
)
{
    ESP_RETURN_ON_ERROR(ensure_refresh_mutex(), TAG, "Cloud command mutex unavailable");
    uint32_t required_after_ms = reserve_after_ms
        + HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS;
    int lock_wait_ms = hub_command_deadline_timeout_ms(
        deadline,
        esp_timer_get_time() / 1000,
        CLOUD_COMMAND_REFRESH_WAIT_MS,
        required_after_ms
    );
    if (lock_wait_ms <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    set_cloud_command_waiting(true);
    BaseType_t taken = xSemaphoreTake(
        refresh_mutex,
        pdMS_TO_TICKS((uint32_t)lock_wait_ms)
    );
    if (taken != pdTRUE) {
        set_cloud_command_waiting(false);
        return ESP_ERR_TIMEOUT;
    }
    int64_t cooldown_ms = cloud_command_io_cooldown_remaining_ms();
    if (cooldown_ms > 0) {
        if (!hub_command_deadline_can_start_step(
                deadline,
                esp_timer_get_time() / 1000 + cooldown_ms,
                required_after_ms
            )) {
            cloud_command_io_cancel_lock();
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)cooldown_ms));
    }
    esp_err_t settle_err = cloud_command_wait_for_internal_heap(
        deadline,
        required_after_ms
    );
    if (settle_err != ESP_OK) {
        cloud_command_io_cancel_lock();
        return settle_err;
    }
    if (!hub_command_deadline_can_start_step(
            deadline,
            esp_timer_get_time() / 1000,
            required_after_ms
        )) {
        cloud_command_io_cancel_lock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void cloud_command_io_unlock(void)
{
    mark_cloud_io_completed();
    xSemaphoreGive(refresh_mutex);
    set_cloud_command_waiting(false);
}

static esp_err_t cloud_readonly_io_lock(void)
{
    if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ensure_refresh_mutex(), TAG, "Cloud read-only mutex unavailable");
    if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(refresh_mutex, pdMS_TO_TICKS(CLOUD_READONLY_IO_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
        xSemaphoreGive(refresh_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void cloud_readonly_io_unlock(void)
{
    mark_cloud_io_completed();
    xSemaphoreGive(refresh_mutex);
}

esp_err_t hub_cloud_background_io_begin(void)
{
    return cloud_readonly_io_lock();
}

void hub_cloud_background_io_end(void)
{
    cloud_readonly_io_unlock();
}

/*
 * Closing an ESP HTTPS client releases most TLS memory immediately, while
 * lwIP can return its small internal-only blocks a little later. Starting the
 * next statistics connection while the largest internal block is below 4 KB
 * caused repeatable ESP_ERR_HTTP_EAGAIN/FETCH_HEADER failures even with more
 * than 8 MB of PSRAM free. Wait outside the Cloud IO mutex so a foreground
 * command can still take ownership immediately. This is a bounded yield, not
 * another endpoint retry.
 */
static esp_err_t cloud_readonly_wait_for_internal_heap(void)
{
    uint32_t waited_ms = 0;
    while (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        < CLOUD_READONLY_MIN_INTERNAL_BLOCK_BYTES) {
        if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (waited_ms >= CLOUD_READONLY_HEAP_SETTLE_MAX_MS) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(CLOUD_READONLY_HEAP_SETTLE_POLL_MS));
        waited_ms += CLOUD_READONLY_HEAP_SETTLE_POLL_MS;
    }
    return ESP_OK;
}

static void set_refresh_running(bool running)
{
    portENTER_CRITICAL(&refresh_mutex_lock);
    refresh_running = running;
    portEXIT_CRITICAL(&refresh_mutex_lock);
}

static void set_error(const char *message)
{
    snprintf(last_error, sizeof(last_error), "%s", message);
}

static void clear_auth_error(void)
{
    last_auth_step[0] = '\0';
    last_auth_err = ESP_OK;
    last_auth_status_code = 0;
}

static void record_auth_error(const char *step, esp_err_t err, int status_code)
{
    snprintf(last_auth_step, sizeof(last_auth_step), "%s", step != NULL ? step : "unknown");
    last_auth_err = err;
    last_auth_status_code = status_code;
}

static void set_authentication_failed_error(void)
{
    if (last_auth_step[0] == '\0') {
        set_error("Cloud authentication failed.");
        return;
    }
    snprintf(
        last_error,
        sizeof(last_error),
        "Cloud authentication failed at %s: %s, HTTP %d.",
        last_auth_step,
        esp_err_to_name(last_auth_err),
        last_auth_status_code
    );
}

static bool json_string_to_buffer(cJSON *parent, const char *key, char *buffer, size_t buffer_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return false;
    }
    snprintf(buffer, buffer_len, "%s", item->valuestring);
    return true;
}

static bool parse_double_string(const char *text, double *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    double parsed = strtod(text, &end);
    if (end == text || errno == ERANGE) {
        return false;
    }
    while (end != NULL && isspace((unsigned char)*end)) {
        end++;
    }
    if (end == NULL || *end != '\0') {
        return false;
    }

    *value = parsed;
    return true;
}

static bool json_item_to_double(cJSON *item, double *value)
{
    if (value == NULL) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        *value = item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return parse_double_string(item->valuestring, value);
    }
    return false;
}

static bool json_number_to_double(cJSON *parent, const char *key, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    return json_item_to_double(item, value);
}

static esp_err_t base64_decode_exact(const char *input, unsigned char *output, size_t output_len)
{
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(output, output_len, &decoded_len, (const unsigned char *)input, strlen(input));
    if (ret != 0 || decoded_len != output_len) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t base64_decode_alloc(const char *input, unsigned char **output, size_t *output_len)
{
    size_t required_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &required_len, (const unsigned char *)input, strlen(input));
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char *buffer = calloc(1, required_len);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_decode(buffer, required_len, output_len, (const unsigned char *)input, strlen(input));
    if (ret != 0) {
        free(buffer);
        return ESP_ERR_INVALID_ARG;
    }

    *output = buffer;
    return ESP_OK;
}

static esp_err_t base64_encode_alloc(const unsigned char *input, size_t input_len, char *output, size_t output_len)
{
    size_t written_len = 0;
    int ret = mbedtls_base64_encode((unsigned char *)output, output_len, &written_len, input, input_len);
    if (ret != 0 || written_len >= output_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    output[written_len] = '\0';
    return ESP_OK;
}

static esp_err_t copy_trimmed_alloc(const char *value, char **trimmed_out)
{
    if (value == NULL || trimmed_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *trimmed_out = NULL;

    const char *start = value;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    size_t len = (size_t)(end - start);
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *trimmed = calloc(1, len + 1);
    if (trimmed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(trimmed, start, len);
    *trimmed_out = trimmed;
    return ESP_OK;
}

static esp_err_t installation_key_json_from_config(const char *config_value, char **json_out)
{
    if (json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;

    char *trimmed = NULL;
    esp_err_t err = copy_trimmed_alloc(config_value, &trimmed);
    if (err != ESP_OK) {
        return err;
    }

    if (trimmed[0] == '{') {
        *json_out = trimmed;
        return ESP_OK;
    }

    unsigned char *decoded = NULL;
    size_t decoded_len = 0;
    err = base64_decode_alloc(trimmed, &decoded, &decoded_len);
    free(trimmed);
    if (err != ESP_OK) {
        return err;
    }

    char *json = calloc(1, decoded_len + 1);
    if (json == NULL) {
        free(decoded);
        return ESP_ERR_NO_MEM;
    }
    memcpy(json, decoded, decoded_len);
    free(decoded);
    *json_out = json;
    return ESP_OK;
}

static void cache_installation_identity_digest(const char *installation_id)
{
    static const char identity_domain[] = "lamabuddy-gate0-cloud-installation:v1";
    if (installation_id == NULL || installation_id[0] == '\0') {
        return;
    }

    size_t installation_id_len = strlen(installation_id);
    unsigned char material[sizeof(identity_domain) + sizeof(((installation_key_t *)0)->installation_id)] = {0};
    if (installation_id_len >= sizeof(((installation_key_t *)0)->installation_id)) {
        return;
    }
    memcpy(material, identity_domain, sizeof(identity_domain));
    memcpy(material + sizeof(identity_domain), installation_id, installation_id_len);

    unsigned char digest[32] = {0};
    if (mbedtls_md(
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
            material,
            sizeof(identity_domain) + installation_id_len,
            digest
        ) != 0) {
        return;
    }

    char encoded[65] = {0};
    for (size_t index = 0; index < sizeof(digest); index++) {
        snprintf(encoded + index * 2, sizeof(encoded) - index * 2, "%02x", digest[index]);
    }
    portENTER_CRITICAL(&auth_diag_lock);
    memcpy(installation_identity_sha256, encoded, sizeof(installation_identity_sha256));
    portEXIT_CRITICAL(&auth_diag_lock);
}

static void record_auth_refresh_result(bool succeeded)
{
    portENTER_CRITICAL(&auth_diag_lock);
    if (succeeded) {
        auth_refresh_success_count++;
        auth_refresh_last_success_uptime_ms = esp_timer_get_time() / 1000;
    } else {
        auth_refresh_failure_count++;
    }
    portEXIT_CRITICAL(&auth_diag_lock);
}

static esp_err_t parse_installation_key(const char *config_value, installation_key_t *key)
{
    memset(key, 0, sizeof(*key));

    char *json = NULL;
    ESP_RETURN_ON_ERROR(installation_key_json_from_config(config_value, &json), TAG, "installation key load failed");

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool ok = json_string_to_buffer(root, "installation_id", key->installation_id, sizeof(key->installation_id))
        && json_string_to_buffer(root, "private_key", key->private_key_b64, sizeof(key->private_key_b64));

    char secret_b64[64] = {0};
    ok = ok && json_string_to_buffer(root, "secret", secret_b64, sizeof(secret_b64));
    cJSON_Delete(root);

    if (!ok) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t secret_err = base64_decode_exact(secret_b64, key->secret, sizeof(key->secret));
    if (secret_err == ESP_OK) {
        cache_installation_identity_digest(key->installation_id);
    }
    return secret_err;
}

static esp_err_t generate_request_proof(const char *base_string, const unsigned char secret[32], char *proof, size_t proof_len)
{
    unsigned char work[32] = {0};
    memcpy(work, secret, sizeof(work));

    for (const unsigned char *cursor = (const unsigned char *)base_string; *cursor != '\0'; cursor++) {
        unsigned char byte_val = *cursor;
        int idx = byte_val % 32;
        int shift_idx = (idx + 1) % 32;
        int shift_amount = work[shift_idx] & 7;
        unsigned char xor_result = byte_val ^ work[idx];
        unsigned char rotated = (unsigned char)(((xor_result << shift_amount) | (xor_result >> (8 - shift_amount))) & 0xff);
        work[idx] = rotated;
    }

    unsigned char digest[32] = {0};
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), work, sizeof(work), digest);
    return base64_encode_alloc(digest, sizeof(digest), proof, proof_len);
}

static void make_nonce(char *nonce, size_t nonce_len)
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    uint32_t d = esp_random();
    snprintf(
        nonce,
        nonce_len,
        "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
        (unsigned long)a,
        (unsigned long)((b >> 16) & 0xffff),
        (unsigned long)(b & 0xffff),
        (unsigned long)((c >> 16) & 0xffff),
        (unsigned long)(c & 0xffffffff),
        (unsigned long)(d & 0xffff)
    );
}

static bool time_is_reasonable(void)
{
    time_t now = time(NULL);
    struct tm tm = {0};
    gmtime_r(&now, &tm);
    return tm.tm_year + 1900 >= 2025;
}

static esp_err_t ensure_time_synced(void)
{
    if (time_is_reasonable()) {
        return ESP_OK;
    }

    if (!sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.wait_for_sync = true;
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            return err;
        }
        sntp_initialized = true;
    }

    for (int i = 0; i < 15; i++) {
        if (time_is_reasonable()) {
            return ESP_OK;
        }
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        if (err == ESP_OK && time_is_reasonable()) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t sign_with_installation_key(
    const installation_key_t *key,
    const char *signature_data,
    char *signature_b64,
    size_t signature_b64_len
)
{
    esp_err_t err = ESP_FAIL;
    unsigned char *private_der = NULL;
    size_t private_der_len = 0;
    unsigned char hash[32] = {0};
    unsigned char signature[96] = {0};
    size_t signature_len = 0;

    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    if (base64_decode_alloc(key->private_key_b64, &private_der, &private_der_len) != ESP_OK) {
        goto cleanup;
    }

    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"lamabuddy", 9) != 0) {
        goto cleanup;
    }
    if (mbedtls_pk_parse_key(&pk, private_der, private_der_len, NULL, 0, mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        goto cleanup;
    }

    mbedtls_md(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        (const unsigned char *)signature_data,
        strlen(signature_data),
        hash
    );
    if (mbedtls_pk_sign(
            &pk,
            MBEDTLS_MD_SHA256,
            hash,
            sizeof(hash),
            signature,
            sizeof(signature),
            &signature_len,
            mbedtls_ctr_drbg_random,
            &ctr_drbg
        ) != 0) {
        goto cleanup;
    }

    err = base64_encode_alloc(signature, signature_len, signature_b64, signature_b64_len);

cleanup:
    free(private_der);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return err;
}

static esp_err_t generate_signed_headers(
    const installation_key_t *key,
    char *timestamp,
    size_t timestamp_len,
    char *nonce,
    size_t nonce_len,
    char *signature,
    size_t signature_len
)
{
    time_t now = time(NULL);
    snprintf(timestamp, timestamp_len, "%lld000", (long long)now);
    make_nonce(nonce, nonce_len);

    char proof_input[128] = {0};
    char proof[64] = {0};
    char signature_data[256] = {0};
    snprintf(proof_input, sizeof(proof_input), "%s.%s.%s", key->installation_id, nonce, timestamp);
    ESP_RETURN_ON_ERROR(generate_request_proof(proof_input, key->secret, proof, sizeof(proof)), TAG, "proof failed");
    snprintf(signature_data, sizeof(signature_data), "%s.%s", proof_input, proof);
    return sign_with_installation_key(key, signature_data, signature, signature_len);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    http_response_t *response = (http_response_t *)evt->user_data;
    if (response == NULL) {
        return ESP_OK;
    }

    return hub_http_response_append(response, evt->data, (size_t)evt->data_len);
}

static bool http_response_is_gzip(const http_response_t *response)
{
    const unsigned char *data = (const unsigned char *)response->data;
    return response != NULL
        && response->data != NULL
        && response->len >= 18
        && data[0] == 0x1f
        && data[1] == 0x8b
        && data[2] == 0x08;
}

static uint32_t http_response_first_bytes(const http_response_t *response)
{
    if (response == NULL || response->data == NULL || response->len == 0) {
        return 0;
    }

    const unsigned char *data = (const unsigned char *)response->data;
    uint32_t value = 0;
    size_t count = response->len < 4 ? response->len : 4;
    for (size_t index = 0; index < count; index++) {
        value |= ((uint32_t)data[index]) << (index * 8);
    }
    return value;
}

static bool http_response_is_zlib(const http_response_t *response)
{
    const unsigned char *data = (const unsigned char *)response->data;
    return response != NULL
        && response->data != NULL
        && response->len >= 2
        && data[0] == 0x78;
}

static esp_err_t gzip_deflate_payload(
    const unsigned char *data,
    size_t len,
    const unsigned char **payload,
    size_t *payload_len
)
{
    if (data == NULL || payload == NULL || payload_len == NULL || len < 18) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t pos = 10;
    unsigned char flags = data[3];
    if ((flags & 0x04) != 0) {
        if (pos + 2 > len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t extra_len = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
        pos += 2 + extra_len;
    }
    if ((flags & 0x08) != 0) {
        while (pos < len && data[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if ((flags & 0x10) != 0) {
        while (pos < len && data[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if ((flags & 0x02) != 0) {
        pos += 2;
    }
    if (pos + 8 > len) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *payload = data + pos;
    *payload_len = len - pos - 8;
    return *payload_len > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t replace_response_with_inflated_json(
    http_response_t *response,
    const void *compressed,
    size_t compressed_len,
    int flags
)
{
    size_t inflated_len = 0;
    void *inflated = tinfl_decompress_mem_to_heap(compressed, compressed_len, &inflated_len, flags);
    if (inflated == NULL || inflated_len == 0
        || inflated_len >= HUB_HTTP_RESPONSE_DEFAULT_MAX_CAPACITY) {
        if (inflated != NULL) {
            mz_free(inflated);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    char *json = heap_caps_malloc(inflated_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        json = heap_caps_malloc(inflated_len + 1, MALLOC_CAP_8BIT);
    }
    if (json == NULL) {
        mz_free(inflated);
        return ESP_ERR_NO_MEM;
    }

    memcpy(json, inflated, inflated_len);
    json[inflated_len] = '\0';
    mz_free(inflated);

    hub_http_response_reset(response);
    response->data = json;
    response->len = inflated_len;
    response->capacity = inflated_len + 1;
    return ESP_OK;
}

static esp_err_t inflate_http_response_if_needed(http_response_t *response)
{
    if (response == NULL || response->data == NULL || response->len == 0) {
        return ESP_OK;
    }

    if (http_response_is_gzip(response)) {
        const unsigned char *payload = NULL;
        size_t payload_len = 0;
        ESP_RETURN_ON_ERROR(
            gzip_deflate_payload((const unsigned char *)response->data, response->len, &payload, &payload_len),
            TAG,
            "invalid gzip response"
        );
        return replace_response_with_inflated_json(
            response,
            payload,
            payload_len,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
        );
    }

    if (http_response_is_zlib(response)) {
        return replace_response_with_inflated_json(
            response,
            response->data,
            response->len,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
        );
    }

    return ESP_OK;
}

static bool cloud_http_retryable_error(esp_err_t err)
{
    return err == ESP_ERR_HTTP_CONNECT
        || err == ESP_ERR_HTTP_WRITE_DATA
        || err == ESP_ERR_HTTP_EAGAIN
        || err == ESP_ERR_HTTP_FETCH_HEADER
        || err == ESP_ERR_HTTP_CONNECTION_CLOSED
        || err == ESP_ERR_HTTP_READ_TIMEOUT
        || err == ESP_ERR_HTTP_INCOMPLETE_DATA
        || err == ESP_ERR_TIMEOUT
        || err == ESP_ERR_INVALID_SIZE;
}

static bool cloud_readonly_retryable_error(esp_err_t err)
{
    return cloud_http_retryable_error(err)
        || err == ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t cloud_http_request_once(
    const char *url,
    esp_http_client_method_t method,
    const installation_key_t *key,
    const char *access_token,
    const char *body,
    http_response_t *response,
    int *status_code,
    int timeout_ms
)
{
    char timestamp[24] = {0};
    char nonce[40] = {0};
    char signature[160] = {0};
    ESP_RETURN_ON_ERROR(generate_signed_headers(key, timestamp, sizeof(timestamp), nonce, sizeof(nonce), signature, sizeof(signature)), TAG, "signed headers failed");

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_handler,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = CLOUD_HTTP_CLIENT_BUFFER_BYTES,
        .buffer_size_tx = CLOUD_HTTP_CLIENT_BUFFER_BYTES,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-App-Installation-Id", key->installation_id);
    esp_http_client_set_header(client, "X-Timestamp", timestamp);
    esp_http_client_set_header(client, "X-Nonce", nonce);
    esp_http_client_set_header(client, "X-Request-Signature", signature);
    if (access_token != NULL && access_token[0] != '\0') {
        char auth_header[HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 16] = {0};
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", access_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    if (body != NULL) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int64_t expected_bytes = esp_http_client_get_content_length(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    if (err == ESP_OK) {
        err = hub_http_response_finish(response, expected_bytes, chunked);
    }
    if (err == ESP_OK) {
        err = inflate_http_response_if_needed(response);
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t cloud_http_request_no_retry(
    const char *url,
    esp_http_client_method_t method,
    const installation_key_t *key,
    const char *access_token,
    const char *body,
    http_response_t *response,
    int *status_code,
    int timeout_ms
)
{
    if (status_code != NULL) {
        *status_code = 0;
    }
    if (response != NULL) {
        hub_http_response_reset(response);
    }
    return cloud_http_request_once(
        url,
        method,
        key,
        access_token,
        body,
        response,
        status_code,
        timeout_ms
    );
}

static esp_err_t cloud_http_request(
    const char *url,
    esp_http_client_method_t method,
    const installation_key_t *key,
    const char *access_token,
    const char *body,
    http_response_t *response,
    int *status_code
)
{
    const int max_attempts = 3;
    esp_err_t err = ESP_FAIL;
    if (status_code != NULL) {
        *status_code = 0;
    }

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (response != NULL) {
            hub_http_response_reset(response);
        }

        int attempt_status = 0;
        err = cloud_http_request_once(
            url,
            method,
            key,
            access_token,
            body,
            response,
            &attempt_status,
            CLOUD_READONLY_HTTP_TIMEOUT_MS
        );
        if (status_code != NULL) {
            *status_code = attempt_status;
        }
        if (err == ESP_OK || !cloud_http_retryable_error(err) || attempt == max_attempts - 1) {
            return err;
        }
        if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGW(TAG, "Cloud HTTP request failed on attempt %d/%d: %s, HTTP %d", attempt + 1, max_attempts, esp_err_to_name(err), attempt_status);
        vTaskDelay(pdMS_TO_TICKS(750 * (attempt + 1)));
    }

    return err;
}

static esp_err_t cloud_readonly_http_request(
    const char *url,
    esp_http_client_method_t method,
    const installation_key_t *key,
    const char *access_token,
    const char *body,
    http_response_t *response,
    int *status_code,
    int timeout_ms
)
{
    if (status_code != NULL) {
        *status_code = 0;
    }
    if (response != NULL) {
        hub_http_response_reset(response);
    }
    return cloud_http_request_once(
        url,
        method,
        key,
        access_token,
        body,
        response,
        status_code,
        timeout_ms
    );
}

static int statistics_http_timeout_ms(readonly_diag_kind_t kind)
{
    switch (kind) {
    case READONLY_DIAG_COUNTER:
    case READONLY_DIAG_RECENT:
    case READONLY_DIAG_TREND:
        return CLOUD_STATISTICS_HTTP_TIMEOUT_MS;
    case READONLY_DIAG_THINGS:
    case READONLY_DIAG_DASHBOARD:
    case READONLY_DIAG_SCHEDULE:
    case READONLY_DIAG_FIRMWARE:
    case READONLY_DIAG_COUNT:
        return CLOUD_READONLY_HTTP_TIMEOUT_MS;
    }
    return CLOUD_READONLY_HTTP_TIMEOUT_MS;
}

static int statistics_http_max_attempts(readonly_diag_kind_t kind)
{
    switch (kind) {
    case READONLY_DIAG_COUNTER:
    case READONLY_DIAG_RECENT:
    case READONLY_DIAG_TREND:
        return 3;
    case READONLY_DIAG_THINGS:
    case READONLY_DIAG_DASHBOARD:
    case READONLY_DIAG_SCHEDULE:
    case READONLY_DIAG_FIRMWARE:
    case READONLY_DIAG_COUNT:
        return 3;
    }
    return 1;
}

static esp_err_t cloud_statistics_pause_between_endpoints(void)
{
    const int poll_ms = 100;
    int remaining_ms = CLOUD_STATISTICS_ENDPOINT_PAUSE_MS;
    while (remaining_ms > 0) {
        if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
            return ESP_ERR_INVALID_STATE;
        }
        int delay_ms = remaining_ms < poll_ms ? remaining_ms : poll_ms;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        remaining_ms -= delay_ms;
    }
    return ESP_OK;
}

static esp_err_t cloud_refresh_http_request(
    const char *url,
    esp_http_client_method_t method,
    const installation_key_t *key,
    const char *access_token,
    const char *body,
    http_response_t *response,
    int *status_code
)
{
    /*
     * State keeper owns the retry cadence.  Retrying each snapshot endpoint
     * here duplicated the same read and could monopolize Cloud IO for minutes.
     */
    const int max_attempts = 1;
    esp_err_t err = ESP_FAIL;
    if (status_code != NULL) {
        *status_code = 0;
    }

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (response != NULL) {
            hub_http_response_reset(response);
        }

        int attempt_status = 0;
        err = cloud_http_request_once(
            url,
            method,
            key,
            access_token,
            body,
            response,
            &attempt_status,
            CLOUD_READONLY_HTTP_TIMEOUT_MS
        );
        if (status_code != NULL) {
            *status_code = attempt_status;
        }
        if (err == ESP_OK || !cloud_readonly_retryable_error(err) || attempt == max_attempts - 1) {
            return err;
        }
        if (hub_command_interactive_pending() || cloud_command_is_waiting()) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGW(TAG, "Cloud refresh HTTP request failed on attempt %d/%d: %s, HTTP %d", attempt + 1, max_attempts, esp_err_to_name(err), attempt_status);
        vTaskDelay(pdMS_TO_TICKS(750 * (attempt + 1)));
    }

    return err;
}

static esp_err_t parse_tokens(const char *json, cloud_tokens_t *tokens)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool ok = json_string_to_buffer(root, "accessToken", tokens->access_token, sizeof(tokens->access_token))
        && json_string_to_buffer(root, "refreshToken", tokens->refresh_token, sizeof(tokens->refresh_token));
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t cloud_sign_in(const hub_cloud_settings_t *settings, const installation_key_t *key, cloud_tokens_t *tokens)
{
    char url[128] = {0};
    char body[256] = {0};
    snprintf(url, sizeof(url), "%s/auth/signin", CUSTOMER_APP_URL);
    snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\"}", settings->username, settings->password);

    http_response_t response = {0};
    int status_code = 0;
    esp_err_t err = cloud_http_request(url, HTTP_METHOD_POST, key, NULL, body, &response, &status_code);
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        err = parse_tokens(response.data, tokens);
        if (err != ESP_OK) {
            record_auth_error("signin-parse", err, status_code);
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
        record_auth_error("signin", err, status_code);
    } else {
        record_auth_error("signin", err, status_code);
    }
    hub_http_response_reset(&response);
    return err;
}

static esp_err_t cloud_refresh_token(
    const hub_cloud_settings_t *settings,
    const installation_key_t *key,
    cloud_tokens_t *tokens,
    int *response_status
)
{
    char url[128] = {0};
    char body[1400] = {0};
    snprintf(url, sizeof(url), "%s/auth/refreshtoken", CUSTOMER_APP_URL);
    snprintf(body, sizeof(body), "{\"username\":\"%s\",\"refreshToken\":\"%s\"}", settings->username, settings->refresh_token);

    http_response_t response = {0};
    int status_code = 0;
    esp_err_t err = cloud_http_request(url, HTTP_METHOD_POST, key, NULL, body, &response, &status_code);
    if (response_status != NULL) {
        *response_status = status_code;
    }
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        err = parse_tokens(response.data, tokens);
        if (err != ESP_OK) {
            record_auth_error("refresh-parse", err, status_code);
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
        record_auth_error("refresh-token", err, status_code);
    } else {
        record_auth_error("refresh-token", err, status_code);
    }
    record_auth_refresh_result(err == ESP_OK);
    hub_http_response_reset(&response);
    return err;
}

static esp_err_t get_access_tokens(const hub_cloud_settings_t *settings, const installation_key_t *key, cloud_tokens_t *tokens)
{
    memset(tokens, 0, sizeof(*tokens));
    clear_auth_error();
    if (settings->refresh_token[0] != '\0') {
        int refresh_status = 0;
        esp_err_t refresh_err = cloud_refresh_token(settings, key, tokens, &refresh_status);
        if (refresh_err == ESP_OK) {
            clear_auth_error();
            return ESP_OK;
        }
        if (refresh_status < 400 || refresh_status >= 500) {
            return refresh_err;
        }
    }
    if (settings->password[0] == '\0') {
        record_auth_error("signin", ESP_ERR_INVALID_STATE, 0);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = cloud_sign_in(settings, key, tokens);
    if (err == ESP_OK) {
        clear_auth_error();
    }
    return err;
}

static void set_command_result(hub_cloud_command_result_t *result, bool ok, int64_t started_us, const char *message)
{
    if (result == NULL) {
        return;
    }
    result->ok = ok;
    result->duration_ms = (int)((esp_timer_get_time() - started_us) / 1000);
    snprintf(result->message, sizeof(result->message), "%s", message != NULL ? message : "");
}

static void set_command_attempt(
    hub_cloud_command_result_t *result,
    const hub_cloud_command_attempt_t *attempt
)
{
    if (result == NULL || attempt == NULL) {
        return;
    }
    result->upstream_attempts = attempt->attempts;
    result->http_status = attempt->http_status;
    result->request_started = attempt->request_started;
}

static void cloud_command_failure_message(
    char *dest,
    size_t dest_len,
    const char *label,
    esp_err_t err,
    const hub_cloud_command_attempt_t *attempt
)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }
    const char *resolved_label = label != NULL ? label : "Cloud";
    if (attempt == NULL || !attempt->request_started) {
        snprintf(
            dest,
            dest_len,
            "%s Cloud command failed before upstream request: %s.",
            resolved_label,
            esp_err_to_name(err)
        );
        return;
    }
    snprintf(
        dest,
        dest_len,
        "%s Cloud command failed: %s HTTP %d bytes %lu firstBytes %lu.",
        resolved_label,
        esp_err_to_name(err),
        attempt->http_status,
        (unsigned long)attempt->response_bytes,
        (unsigned long)attempt->first_bytes
    );
}

static bool cached_tokens_valid_locked(int64_t now_ms, int64_t maximum_age_ms)
{
    return cached_tokens.access_token[0] != '\0'
        && cached_tokens.refresh_token[0] != '\0'
        && cached_tokens_ms > 0
        && now_ms - cached_tokens_ms < maximum_age_ms;
}

static bool copy_cached_tokens_if_fresh(cloud_tokens_t *tokens, int64_t maximum_age_ms)
{
    if (tokens == NULL
        || ensure_token_mutex() != ESP_OK
        || xSemaphoreTake(token_mutex, pdMS_TO_TICKS(CLOUD_TOKEN_MUTEX_WAIT_MS)) != pdTRUE) {
        return false;
    }
    bool valid = cached_tokens_valid_locked(esp_timer_get_time() / 1000, maximum_age_ms);
    if (valid) {
        memcpy(tokens, &cached_tokens, sizeof(*tokens));
    }
    xSemaphoreGive(token_mutex);
    return valid;
}

static bool copy_cached_tokens_if_valid(cloud_tokens_t *tokens)
{
    return copy_cached_tokens_if_fresh(tokens, ACCESS_TOKEN_CACHE_TTL_MS);
}

static bool copy_cached_tokens_for_refresh(cloud_tokens_t *tokens)
{
    return copy_cached_tokens_if_fresh(tokens, ACCESS_TOKEN_PROACTIVE_RENEWAL_MS);
}

int64_t hub_cloud_token_renewal_due_ms(void)
{
    if (ensure_token_mutex() != ESP_OK
        || xSemaphoreTake(token_mutex, pdMS_TO_TICKS(CLOUD_TOKEN_MUTEX_WAIT_MS)) != pdTRUE) {
        return 0;
    }
    int64_t due_ms = cached_tokens_ms > 0
        ? cached_tokens_ms + ACCESS_TOKEN_PROACTIVE_RENEWAL_MS
        : 0;
    if (due_ms > 0 && token_renewal_retry_after_ms > due_ms) {
        due_ms = token_renewal_retry_after_ms;
    }
    xSemaphoreGive(token_mutex);
    return due_ms;
}

static void defer_token_renewal_retry(void)
{
    if (ensure_token_mutex() != ESP_OK
        || xSemaphoreTake(token_mutex, pdMS_TO_TICKS(CLOUD_TOKEN_MUTEX_WAIT_MS)) != pdTRUE) {
        return;
    }
    token_renewal_retry_after_ms = esp_timer_get_time() / 1000 + TOKEN_RENEWAL_RETRY_DELAY_MS;
    xSemaphoreGive(token_mutex);
}

static void update_cached_tokens(const cloud_tokens_t *tokens)
{
    if (tokens == NULL
        || tokens->access_token[0] == '\0'
        || ensure_token_mutex() != ESP_OK
        || xSemaphoreTake(token_mutex, pdMS_TO_TICKS(CLOUD_TOKEN_MUTEX_WAIT_MS)) != pdTRUE) {
        return;
    }
    memset(&cached_tokens, 0, sizeof(cached_tokens));
    memcpy(&cached_tokens, tokens, sizeof(cached_tokens));
    cached_tokens_ms = esp_timer_get_time() / 1000;
    token_renewal_retry_after_ms = 0;
    xSemaphoreGive(token_mutex);
}

static void invalidate_cached_tokens(void)
{
    if (ensure_token_mutex() != ESP_OK
        || xSemaphoreTake(token_mutex, pdMS_TO_TICKS(CLOUD_TOKEN_MUTEX_WAIT_MS)) != pdTRUE) {
        return;
    }
    memset(&cached_tokens, 0, sizeof(cached_tokens));
    cached_tokens_ms = 0;
    token_renewal_retry_after_ms = 0;
    xSemaphoreGive(token_mutex);
}

static void free_cloud_context(
    hub_cloud_settings_t *settings,
    installation_key_t *installation_key,
    cloud_tokens_t *tokens
)
{
    if (settings != NULL) {
        memset(settings, 0, sizeof(*settings));
        free(settings);
    }
    if (installation_key != NULL) {
        memset(installation_key, 0, sizeof(*installation_key));
        free(installation_key);
    }
    if (tokens != NULL) {
        memset(tokens, 0, sizeof(*tokens));
        free(tokens);
    }
}

static esp_err_t load_cloud_context(
    hub_cloud_settings_t *settings,
    installation_key_t *installation_key,
    cloud_tokens_t *tokens
)
{
    esp_err_t settings_err = hub_config_get_cloud_settings(settings);
    if (
        settings_err != ESP_OK
        || settings->username[0] == '\0'
        || settings->installation_key[0] == '\0'
        || (settings->password[0] == '\0' && settings->refresh_token[0] == '\0')
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!time_is_reasonable()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_installation_key(settings->installation_key, installation_key), TAG, "Cloud command installation key invalid");
    if (copy_cached_tokens_if_valid(tokens)) {
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

void hub_cloud_clear_websocket_auth(hub_cloud_websocket_auth_t *auth)
{
    if (auth == NULL) {
        return;
    }
    volatile unsigned char *cursor = (volatile unsigned char *)auth;
    for (size_t index = 0; index < sizeof(*auth); index++) {
        cursor[index] = 0;
    }
}

esp_err_t hub_cloud_get_websocket_auth(hub_cloud_websocket_auth_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    hub_cloud_clear_websocket_auth(out);

    hub_cloud_settings_t *settings = heap_caps_calloc(
        1,
        sizeof(*settings),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    installation_key_t *installation_key = heap_caps_calloc(
        1,
        sizeof(*installation_key),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    cloud_tokens_t *tokens = heap_caps_calloc(
        1,
        sizeof(*tokens),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (settings == NULL || installation_key == NULL || tokens == NULL) {
        free_cloud_context(settings, installation_key, tokens);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = load_cloud_context(settings, installation_key, tokens);
    hub_machine_selection_identity_t identity = {0};
    if (err == ESP_OK) {
        err = hub_config_get_machine_selection_identity(&identity);
    }

    char timestamp[24] = {0};
    char nonce[40] = {0};
    char signature[160] = {0};
    if (err == ESP_OK) {
        err = generate_signed_headers(
            installation_key,
            timestamp,
            sizeof(timestamp),
            nonce,
            sizeof(nonce),
            signature,
            sizeof(signature)
        );
    }
    if (err == ESP_OK) {
        int written = snprintf(
            out->handshake_headers,
            sizeof(out->handshake_headers),
            "X-App-Installation-Id: %s\r\n"
            "X-Timestamp: %s\r\n"
            "X-Nonce: %s\r\n"
            "X-Request-Signature: %s\r\n",
            installation_key->installation_id,
            timestamp,
            nonce,
            signature
        );
        if (written < 0 || (size_t)written >= sizeof(out->handshake_headers)) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err == ESP_OK) {
        snprintf(out->access_token, sizeof(out->access_token), "%s", tokens->access_token);
        snprintf(out->machine_serial, sizeof(out->machine_serial), "%s", identity.serial_number);
        out->selection_generation = identity.generation;
    }

    memset(timestamp, 0, sizeof(timestamp));
    memset(nonce, 0, sizeof(nonce));
    memset(signature, 0, sizeof(signature));
    free_cloud_context(settings, installation_key, tokens);
    if (err != ESP_OK) {
        hub_cloud_clear_websocket_auth(out);
    }
    return err;
}

static hub_cloud_command_attempt_t command_attempt_error(esp_err_t err)
{
    hub_cloud_command_attempt_t attempt = {
        .err = err,
    };
    return attempt;
}

static bool cloud_command_connect_failed_before_request(
    const hub_cloud_command_attempt_t *attempt
)
{
    return attempt != NULL
        && attempt->err == ESP_ERR_HTTP_CONNECT
        && attempt->http_status == 0
        && attempt->response_bytes == 0;
}

static hub_cloud_command_attempt_t cloud_execute_url(
    const installation_key_t *key,
    const cloud_tokens_t *tokens,
    const char *url,
    const char *label,
    const char *body,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms
)
{
    hub_cloud_command_attempt_t attempt = {
        .err = ESP_FAIL,
    };
    http_response_t response = {0};
    const int max_connect_attempts = 2;
    for (int connect_attempt = 0; connect_attempt < max_connect_attempts; connect_attempt++) {
        if (!hub_command_side_effect_allowed()) {
            attempt.err = ESP_ERR_INVALID_STATE;
            break;
        }
        int request_timeout_ms = hub_command_deadline_timeout_ms(
            deadline,
            esp_timer_get_time() / 1000,
            CLOUD_COMMAND_HTTP_TIMEOUT_MS,
            reserve_after_ms
        );
        if (request_timeout_ms <= 0) {
            attempt.err = ESP_ERR_TIMEOUT;
            break;
        }
        attempt.http_status = 0;
        hub_http_response_reset(&response);
        attempt.err = cloud_http_request_no_retry(
            url,
            HTTP_METHOD_POST,
            key,
            tokens->access_token,
            body,
            &response,
            &attempt.http_status,
            request_timeout_ms
        );
        attempt.response_bytes = response.len;
        attempt.first_bytes = http_response_first_bytes(&response);
        attempt.attempts = connect_attempt + 1;

        bool failed_before_request = cloud_command_connect_failed_before_request(&attempt);
        attempt.request_started = !failed_before_request;
        if (!failed_before_request || connect_attempt == max_connect_attempts - 1) {
            break;
        }

        /*
         * ESP_ERR_HTTP_CONNECT is raised before the HTTP request is written,
         * so opening one fresh connection does not replay a business POST.
         * All write/header/read failures remain single-attempt because their
         * upstream effect is ambiguous.
         */
        int64_t retry_at_ms = esp_timer_get_time() / 1000
            + CLOUD_COMMAND_CONNECT_RETRY_DELAY_MS;
        if (!hub_command_deadline_can_start_step(
                deadline,
                retry_at_ms,
                HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS
                    + reserve_after_ms
            )) {
            attempt.err = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CLOUD_COMMAND_CONNECT_RETRY_DELAY_MS));
    }
    if (attempt.err == ESP_OK && (attempt.http_status < 200 || attempt.http_status >= 300)) {
        ESP_LOGW(TAG, "Cloud command %s returned HTTP %d", label, attempt.http_status);
        attempt.err = ESP_ERR_INVALID_RESPONSE;
    }
    hub_http_response_reset(&response);
    return attempt;
}

static hub_cloud_command_attempt_t cloud_execute_command(
    const installation_key_t *key,
    const cloud_tokens_t *tokens,
    const char *serial_number,
    const char *command,
    const char *body,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms
)
{
    hub_machine_selection_identity_t target = {0};
    if (hub_command_target_guard_get(&target)) {
        if (!hub_config_machine_selection_matches(&target)) {
            return command_attempt_error(ESP_ERR_INVALID_STATE);
        }
        serial_number = target.serial_number;
    }
    if (serial_number == NULL || serial_number[0] == '\0') {
        return command_attempt_error(ESP_ERR_INVALID_STATE);
    }

    char url[224] = {0};
    snprintf(url, sizeof(url), "%s/things/%s/command/%s", CUSTOMER_APP_URL, serial_number, command);
    return cloud_execute_url(
        key,
        tokens,
        url,
        command,
        body,
        deadline,
        reserve_after_ms
    );
}

esp_err_t hub_cloud_request_refresh(void)
{
    portENTER_CRITICAL(&refresh_mutex_lock);
    command_refresh_generation++;
    if (command_refresh_generation == 0) {
        command_refresh_generation = 1;
    }
    command_refresh_pending = true;
    portEXIT_CRITICAL(&refresh_mutex_lock);
    hub_state_keeper_request_recovery();
    return ESP_OK;
}

uint32_t hub_cloud_refresh_request_generation(void)
{
    portENTER_CRITICAL(&refresh_mutex_lock);
    uint32_t generation = command_refresh_generation;
    portEXIT_CRITICAL(&refresh_mutex_lock);
    return generation;
}

void hub_cloud_complete_refresh_request(uint32_t generation)
{
    portENTER_CRITICAL(&refresh_mutex_lock);
    if (command_refresh_pending && generation == command_refresh_generation) {
        command_refresh_pending = false;
    }
    portEXIT_CRITICAL(&refresh_mutex_lock);
}

bool hub_cloud_refresh_is_running(void)
{
    portENTER_CRITICAL(&refresh_mutex_lock);
    bool running = refresh_running;
    portEXIT_CRITICAL(&refresh_mutex_lock);
    return running;
}

static bool cloud_refresh_pending_snapshot(void)
{
    portENTER_CRITICAL(&refresh_mutex_lock);
    bool pending = command_refresh_pending || refresh_running;
    portEXIT_CRITICAL(&refresh_mutex_lock);
    return pending;
}

static void schedule_refresh_after_command(void)
{
    (void)hub_cloud_request_refresh();
}

static void format_snapshot_timestamp_now(char *destination, size_t destination_len)
{
    if (destination == NULL || destination_len == 0) {
        return;
    }
    destination[0] = '\0';
    time_t now = time(NULL);
    struct tm tm = {0};
    if (gmtime_r(&now, &tm) != NULL) {
        strftime(destination, destination_len, "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
}

static bool active_command_target(hub_machine_selection_identity_t *target)
{
    return target != NULL
        && hub_command_target_guard_get(target)
        && hub_config_machine_selection_matches(target);
}
static void update_snapshot_bbw_dose(double dose1, double dose2)
{
    hub_machine_selection_identity_t target = {0};
    if (!active_command_target(&target)) {
        return;
    }
    portENTER_CRITICAL(&snapshot_lock);
    bool matches = snapshot.selection_generation == target.generation
        && strcmp(snapshot.serial_number, target.serial_number) == 0;
    if (matches) {
        snapshot.dose1 = dose1;
        snapshot.dose1_set = true;
        snapshot.dose2 = dose2;
        snapshot.dose2_set = true;
    }
    portEXIT_CRITICAL(&snapshot_lock);
}

static void update_snapshot_bbw_mode(const char *mode)
{
    hub_machine_selection_identity_t target = {0};
    if (!active_command_target(&target)) {
        return;
    }
    portENTER_CRITICAL(&snapshot_lock);
    if (snapshot.selection_generation == target.generation
        && strcmp(snapshot.serial_number, target.serial_number) == 0) {
        snprintf(snapshot.bbw_mode, sizeof(snapshot.bbw_mode), "%s", mode);
    }
    portEXIT_CRITICAL(&snapshot_lock);
    struct timeval confirmed = {0};
    gettimeofday(&confirmed, NULL);
    if (hub_config_save_bbw_selection(&target, mode,
            (double)confirmed.tv_sec + confirmed.tv_usec / 1000000.0) != ESP_OK)
        ESP_LOGW(TAG, "Could not persist confirmed BBW display selection");
}

static hub_cloud_command_attempt_t execute_command_once_with_auth(
    const char *command,
    const char *body,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms
)
{
    if (!hub_command_deadline_can_start_step(
            deadline,
            esp_timer_get_time() / 1000,
            HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS + reserve_after_ms
        )) {
        return command_attempt_error(ESP_ERR_TIMEOUT);
    }
    hub_cloud_settings_t *settings = calloc(1, sizeof(*settings));
    installation_key_t *installation_key = calloc(1, sizeof(*installation_key));
    cloud_tokens_t *tokens = calloc(1, sizeof(*tokens));
    if (settings == NULL || installation_key == NULL || tokens == NULL) {
        free_cloud_context(settings, installation_key, tokens);
        return command_attempt_error(ESP_ERR_NO_MEM);
    }

    hub_cloud_command_attempt_t attempt = command_attempt_error(ESP_FAIL);
    attempt.err = load_cloud_context(settings, installation_key, tokens);
    if (attempt.err == ESP_OK
        && !hub_command_deadline_can_start_step(
            deadline,
            esp_timer_get_time() / 1000,
            HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS + reserve_after_ms
        )) {
        attempt.err = ESP_ERR_TIMEOUT;
    }
    if (attempt.err == ESP_OK) {
        attempt = cloud_execute_command(
            installation_key,
            tokens,
            machine_serial,
            command,
            body,
            deadline,
            reserve_after_ms
        );
    }
    free_cloud_context(settings, installation_key, tokens);
    return attempt;
}

static hub_cloud_command_attempt_t execute_command_with_auth_locked(
    const char *command,
    const char *body,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms
)
{
    hub_cloud_command_attempt_t attempt = execute_command_once_with_auth(
        command,
        body,
        deadline,
        reserve_after_ms
    );
    if (attempt.http_status == 401 || attempt.http_status == 403) {
        invalidate_cached_tokens();
    }
    return attempt;
}


static void schedule_command_auth_recovery(const hub_cloud_command_attempt_t *attempt)
{
    /*
     * A full dashboard refresh does not repair an ESP transport error. Starting
     * one immediately after WRITE_DATA/EAGAIN instead starves the user's next
     * control retry and creates a failure/refresh/failure feedback loop. Keep
     * the last-good snapshot and let the normal keeper cadence reconcile it.
     * Authentication and missing-context failures still require recovery.
     */
    bool should_refresh_after_failure =
        (!attempt->request_started && attempt->err == ESP_ERR_INVALID_STATE)
        || attempt->http_status == 401
        || attempt->http_status == 403;
    if (should_refresh_after_failure) {
        (void)hub_cloud_request_refresh();
    }
}

static hub_cloud_command_attempt_t execute_command_with_auth(
    const char *command,
    const char *body,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms
)
{
    esp_err_t lock_err = cloud_command_io_lock(deadline, reserve_after_ms);
    if (lock_err != ESP_OK) {
        return command_attempt_error(lock_err);
    }
    hub_cloud_command_attempt_t attempt = execute_command_with_auth_locked(
        command,
        body,
        deadline,
        reserve_after_ms
    );
    cloud_command_io_unlock();
    /*
     * A full dashboard refresh does not repair an ESP transport error. Starting
     * one immediately after WRITE_DATA/EAGAIN instead starves the user's next
     * control retry and creates a failure/refresh/failure feedback loop. Keep
     * the last-good snapshot and let the normal keeper cadence reconcile it.
     * Authentication and missing-context failures still require recovery.
     */
    bool should_refresh_after_failure =
        (!attempt.request_started && attempt.err == ESP_ERR_INVALID_STATE)
        || attempt.http_status == 401
        || attempt.http_status == 403;
    if (should_refresh_after_failure) {
        (void)hub_cloud_request_refresh();
    }
    return attempt;
}

static hub_cloud_command_attempt_t execute_setting_command_with_auth(
    const char *command,
    const char *body,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms
)
{
    return execute_command_with_auth(
        command,
        body,
        deadline,
        reserve_after_ms
    );
}

static hub_cloud_command_attempt_t execute_high_risk_command_with_auth(
    const char *command,
    const char *body
)
{
    return execute_command_with_auth(command, body, NULL, 0);
}

static hub_cloud_command_attempt_t execute_firmware_update_with_auth(void)
{
    esp_err_t lock_err = cloud_command_io_lock(NULL, 0);
    if (lock_err != ESP_OK) {
        return command_attempt_error(lock_err);
    }

    hub_cloud_command_attempt_t attempt = command_attempt_error(ESP_FAIL);
    hub_cloud_settings_t *settings = calloc(1, sizeof(*settings));
    installation_key_t *installation_key = calloc(1, sizeof(*installation_key));
    cloud_tokens_t *tokens = calloc(1, sizeof(*tokens));
    hub_machine_selection_identity_t target = {0};
    if (settings == NULL || installation_key == NULL || tokens == NULL) {
        attempt.err = ESP_ERR_NO_MEM;
    } else if (!hub_command_target_guard_get(&target)
        || !hub_config_machine_selection_matches(&target)) {
        attempt.err = ESP_ERR_INVALID_STATE;
    } else {
        attempt.err = load_cloud_context(settings, installation_key, tokens);
        if (attempt.err == ESP_OK) {
            char url[192] = {0};
            snprintf(url, sizeof(url), "%s/things/%s/update-fw", CUSTOMER_APP_URL, target.serial_number);
            attempt = cloud_execute_url(
                installation_key,
                tokens,
                url,
                "machineFirmwareUpdate",
                NULL,
                NULL,
                0
            );
        }
    }
    bool auth_recovery_required = attempt.http_status == 401 || attempt.http_status == 403;
    if (auth_recovery_required) {
        invalidate_cached_tokens();
    }
    free_cloud_context(settings, installation_key, tokens);
    cloud_command_io_unlock();
    if (auth_recovery_required
        || (!attempt.request_started && attempt.err == ESP_ERR_INVALID_STATE)) {
        (void)hub_cloud_request_refresh();
    }
    return attempt;
}

static const char *json_optional_string(cJSON *parent, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}

static void json_safe_copy(char *dest, size_t dest_len, const char *source)
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

static bool json_optional_bool(cJSON *parent, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    return cJSON_IsTrue(item);
}

static void preserve_existing_machine_ble_identity(hub_machine_selection_t *selection)
{
    if (selection == NULL || selection->serial_number[0] == '\0') {
        return;
    }

    hub_machine_selection_t existing = {0};
    if (hub_config_get_machine_selection(&existing) != ESP_OK
        || strcmp(existing.serial_number, selection->serial_number) != 0) {
        memset(&existing, 0, sizeof(existing));
        return;
    }

    if (selection->ble_device_name[0] == '\0' && existing.ble_device_name[0] != '\0') {
        snprintf(selection->ble_device_name, sizeof(selection->ble_device_name), "%s", existing.ble_device_name);
    }
    if (selection->ble_device_address[0] == '\0' && existing.ble_device_address[0] != '\0') {
        snprintf(selection->ble_device_address, sizeof(selection->ble_device_address), "%s", existing.ble_device_address);
    }
    if (selection->ble_auth_token[0] == '\0' && existing.ble_auth_token[0] != '\0') {
        snprintf(selection->ble_auth_token, sizeof(selection->ble_auth_token), "%s", existing.ble_auth_token);
    }
    if (selection->machine_name[0] == '\0' && existing.machine_name[0] != '\0') {
        snprintf(selection->machine_name, sizeof(selection->machine_name), "%s", existing.machine_name);
    }
    memset(&existing, 0, sizeof(existing));
}

static void ensure_machine_ble_name(hub_machine_selection_t *selection)
{
    if (selection == NULL || selection->ble_device_name[0] != '\0' || selection->serial_number[0] == '\0') {
        return;
    }

    const int suffix_len = (int)(sizeof(selection->ble_device_name) - strlen("LINEAR_") - 1);
    snprintf(selection->ble_device_name, sizeof(selection->ble_device_name), "LINEAR_%.*s", suffix_len, selection->serial_number);
}

static bool selected_machine_serial_for_request(const hub_cloud_settings_t *settings, char *serial, size_t serial_len)
{
    if (serial == NULL || serial_len == 0) {
        return false;
    }
    serial[0] = '\0';

    hub_machine_selection_identity_t identity = {0};
    if (hub_config_get_machine_selection_identity(&identity) == ESP_OK) {
        snprintf(serial, serial_len, "%s", identity.serial_number);
        return true;
    }
    if (settings != NULL && settings->cloud_serial_number[0] != '\0') {
        snprintf(serial, serial_len, "%s", settings->cloud_serial_number);
        return true;
    }

    if (machine_serial[0] != '\0') {
        snprintf(serial, serial_len, "%s", machine_serial);
        return true;
    }
    return false;
}

static cJSON *cloud_response_payload(cJSON *root)
{
    if (root == NULL) {
        return NULL;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data) || cJSON_IsArray(data)) {
        return data;
    }
    return root;
}

static cJSON *cloud_response_output(cJSON *root)
{
    cJSON *payload = cloud_response_payload(root);
    if (payload == NULL) {
        return NULL;
    }
    cJSON *output = cJSON_GetObjectItemCaseSensitive(payload, "output");
    if (cJSON_IsObject(output) || cJSON_IsArray(output)) {
        return output;
    }
    return payload;
}

static esp_err_t cloud_parse_json_text(
    const char *json_text,
    bool use_output,
    cJSON **payload
)
{
    if (json_text == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *payload = NULL;

    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *source = use_output ? cloud_response_output(root) : cloud_response_payload(root);
    if (source != NULL) {
        *payload = cJSON_Duplicate(source, true);
    }
    cJSON_Delete(root);
    return *payload != NULL ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/*
 * Statistics keeps compressed or plain response text in PSRAM until all
 * network IO is finished, then builds cJSON trees. Each endpoint owns a fresh
 * HTTP/TLS client: La Marzocco sometimes closes a keep-alive connection after
 * the counter response, and reusing that handle caused repeatable
 * ESP_ERR_HTTP_FETCH_HEADER failures for recent/trend. Holding only raw PSRAM
 * text between requests avoids the earlier cJSON/internal-heap fragmentation.
 */
static esp_err_t cloud_fetch_json_text(
    const char *url,
    const installation_key_t *key,
    const cloud_tokens_t *tokens,
    readonly_diag_kind_t diag_kind,
    char **json_text
)
{
    if (url == NULL || key == NULL || tokens == NULL || json_text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_text = NULL;

    const int max_attempts = statistics_http_max_attempts(diag_kind);
    esp_err_t final_err = ESP_FAIL;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        esp_err_t settle_err = cloud_readonly_wait_for_internal_heap();
        if (settle_err != ESP_OK) {
            return settle_err;
        }
        http_response_t response = {0};
        int status_code = 0;
        int64_t started_us = esp_timer_get_time();
        esp_err_t lock_err = cloud_readonly_io_lock();
        if (lock_err != ESP_OK) {
            record_readonly_http_diag(
                diag_kind,
                lock_err,
                0,
                (int)((esp_timer_get_time() - started_us) / 1000),
                0,
                0
            );
            return lock_err;
        }
        esp_err_t err = cloud_readonly_http_request(
            url,
            HTTP_METHOD_GET,
            key,
            tokens->access_token,
            NULL,
            &response,
            &status_code,
            statistics_http_timeout_ms(diag_kind)
        );
        cloud_readonly_io_unlock();
        uint32_t first_bytes = http_response_first_bytes(&response);
        int duration_ms = (int)((esp_timer_get_time() - started_us) / 1000);
        final_err = err;

        if (err == ESP_OK
            && status_code >= 200
            && status_code < 300
            && response.data != NULL) {
            size_t response_bytes = response.len;
            *json_text = response.data;
            response.data = NULL;
            response.len = 0;
            response.capacity = 0;
            record_readonly_http_diag(
                diag_kind,
                ESP_OK,
                status_code,
                duration_ms,
                response_bytes,
                first_bytes
            );
            return ESP_OK;
        }

        final_err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        record_readonly_http_diag(
            diag_kind,
            final_err,
            status_code,
            duration_ms,
            response.len,
            first_bytes
        );
        hub_http_response_reset(&response);
        if (!cloud_readonly_retryable_error(final_err)
            || attempt == max_attempts - 1
            || hub_command_interactive_pending()
            || cloud_command_is_waiting()) {
            return final_err;
        }
        /* cloud_readonly_http_request already disposed the failed client. */
        vTaskDelay(pdMS_TO_TICKS(CLOUD_STATISTICS_RETRY_BASE_DELAY_MS * (attempt + 1)));
    }

    return final_err;
}

static esp_err_t cloud_fetch_json_payload(
    const char *url,
    const installation_key_t *key,
    const cloud_tokens_t *tokens,
    bool use_output,
    readonly_diag_kind_t diag_kind,
    cJSON **payload
)
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *payload = NULL;

    const int max_attempts = statistics_http_max_attempts(diag_kind);
    esp_err_t final_err = ESP_FAIL;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        esp_err_t settle_err = cloud_readonly_wait_for_internal_heap();
        if (settle_err != ESP_OK) {
            return settle_err;
        }
        http_response_t response = {0};
        int status_code = 0;
        int64_t started_us = esp_timer_get_time();
        esp_err_t lock_err = cloud_readonly_io_lock();
        if (lock_err != ESP_OK) {
            record_readonly_http_diag(
                diag_kind,
                lock_err,
                0,
                (int)((esp_timer_get_time() - started_us) / 1000),
                0,
                0
            );
            return lock_err;
        }
        esp_err_t err = cloud_readonly_http_request(
            url,
            HTTP_METHOD_GET,
            key,
            tokens->access_token,
            NULL,
            &response,
            &status_code,
            statistics_http_timeout_ms(diag_kind)
        );
        cloud_readonly_io_unlock();
        uint32_t first_bytes = http_response_first_bytes(&response);
        int duration_ms = (int)((esp_timer_get_time() - started_us) / 1000);
        final_err = err;

        if (err != ESP_OK || status_code < 200 || status_code >= 300 || response.data == NULL) {
            final_err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
            record_readonly_http_diag(
                diag_kind,
                final_err,
                status_code,
                duration_ms,
                response.len,
                first_bytes
            );
            hub_http_response_reset(&response);
        } else {
            size_t response_bytes = response.len;
            cJSON *root = cJSON_Parse(response.data);
            hub_http_response_reset(&response);
            if (root == NULL) {
                final_err = ESP_ERR_INVALID_RESPONSE;
                record_readonly_http_diag(
                    diag_kind,
                    final_err,
                    status_code,
                    duration_ms,
                    response_bytes,
                    first_bytes
                );
            } else {
                cJSON *source = use_output ? cloud_response_output(root) : cloud_response_payload(root);
                if (source == NULL) {
                    final_err = ESP_ERR_INVALID_RESPONSE;
                    record_readonly_http_diag(
                        diag_kind,
                        final_err,
                        status_code,
                        duration_ms,
                        response_bytes,
                        first_bytes
                    );
                } else {
                    *payload = cJSON_Duplicate(source, true);
                    final_err = *payload != NULL ? ESP_OK : ESP_ERR_NO_MEM;
                    record_readonly_http_diag(
                        diag_kind,
                        final_err,
                        status_code,
                        duration_ms,
                        response_bytes,
                        first_bytes
                    );
                }
                cJSON_Delete(root);
            }
        }

        if (final_err == ESP_OK
            || !cloud_readonly_retryable_error(final_err)
            || attempt == max_attempts - 1
            || cloud_command_is_waiting()) {
            return final_err;
        }
        vTaskDelay(pdMS_TO_TICKS(750 * (attempt + 1)));
    }

    return final_err;
}

static cJSON *find_key_recursive(cJSON *node, const char *key)
{
    if (node == NULL || key == NULL) {
        return NULL;
    }

    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (child->string != NULL && strcmp(child->string, key) == 0) {
                return child;
            }
            cJSON *found = find_key_recursive(child, key);
            if (found != NULL) {
                return found;
            }
        }
    }
    return NULL;
}

static bool find_number_recursive(cJSON *node, const char *key, double *value)
{
    cJSON *item = find_key_recursive(node, key);
    return json_item_to_double(item, value);
}

static bool is_secret_json_key(const char *key)
{
    if (key == NULL) {
        return false;
    }
    return strcasecmp(key, "bleAuthToken") == 0
        || strcasecmp(key, "installationKey") == 0
        || strcasecmp(key, "refreshToken") == 0
        || strcasecmp(key, "hubToken") == 0
        || strcasecmp(key, "password") == 0;
}

static void redact_secret_fields(cJSON *node)
{
    if (node == NULL || (!cJSON_IsObject(node) && !cJSON_IsArray(node))) {
        return;
    }

    cJSON *child = node->child;
    while (child != NULL) {
        cJSON *next = child->next;
        if (is_secret_json_key(child->string)) {
            cJSON *redacted = cJSON_CreateString("[redacted]");
            if (redacted != NULL) {
                cJSON_ReplaceItemViaPointer(node, child, redacted);
            }
            child = next;
            continue;
        }
        redact_secret_fields(child);
        child = next;
    }
}

static esp_err_t cjson_print_to_buffer(cJSON *root, char *buffer, size_t buffer_len)
{
    if (root == NULL || buffer == NULL || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *printed = cJSON_PrintUnformatted(root);
    if (printed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (strlen(printed) >= buffer_len) {
        cJSON_free(printed);
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(buffer, buffer_len, "%s", printed);
    cJSON_free(printed);
    return ESP_OK;
}

static void add_number_or_null(cJSON *parent, const char *key, cJSON *source)
{
    double value = 0.0;
    if (json_item_to_double(source, &value)) {
        cJSON_AddItemToObject(parent, key, cJSON_CreateNumber(value));
    } else {
        cJSON_AddNullToObject(parent, key);
    }
}

static void add_counter_number_or_null(
    cJSON *parent,
    const char *key,
    cJSON *counter_output,
    bool cached_set,
    int cached_value
)
{
    double value = 0.0;
    if (find_number_recursive(counter_output, key, &value)) {
        cJSON_AddItemToObject(parent, key, cJSON_CreateNumber(value));
    } else if (cached_set) {
        cJSON_AddItemToObject(parent, key, cJSON_CreateNumber(cached_value));
    } else {
        cJSON_AddNullToObject(parent, key);
    }
}

static void add_string_or_null(cJSON *parent, const char *key, cJSON *source)
{
    if (cJSON_IsString(source) && source->valuestring != NULL) {
        cJSON_AddItemToObject(parent, key, cJSON_CreateString(source->valuestring));
    } else {
        cJSON_AddNullToObject(parent, key);
    }
}

static bool millis_to_iso8601(cJSON *source, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return false;
    }
    buffer[0] = '\0';

    double millis = 0.0;
    if (json_item_to_double(source, &millis)) {
        time_t seconds = (time_t)(millis / 1000.0);
        struct tm tm = {0};
        gmtime_r(&seconds, &tm);
        strftime(buffer, buffer_len, "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buffer[0] != '\0';
    }

    if (cJSON_IsString(source) && source->valuestring != NULL && source->valuestring[0] != '\0') {
        snprintf(buffer, buffer_len, "%s", source->valuestring);
        return true;
    }
    return false;
}

static bool millis_number_to_iso8601(cJSON *source, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return false;
    }
    buffer[0] = '\0';

    double millis = 0.0;
    if (!json_item_to_double(source, &millis)) {
        return false;
    }

    time_t seconds = (time_t)(millis / 1000.0);
    struct tm tm = {0};
    gmtime_r(&seconds, &tm);
    strftime(buffer, buffer_len, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer[0] != '\0';
}

static cJSON *statistics_recent_coffees_json(cJSON *recent_output)
{
    cJSON *recent = cJSON_CreateArray();
    if (recent == NULL) {
        return NULL;
    }

    cJSON *last_coffees = cJSON_GetObjectItemCaseSensitive(recent_output, "lastCoffees");
    if (!cJSON_IsArray(last_coffees)) {
        last_coffees = find_key_recursive(recent_output, "lastCoffees");
    }
    if (!cJSON_IsArray(last_coffees) && cJSON_IsArray(recent_output)) {
        last_coffees = recent_output;
    }
    cJSON *row = NULL;
    cJSON_ArrayForEach(row, last_coffees) {
        if (!cJSON_IsObject(row)) {
            continue;
        }

        char iso_time[32] = {0};
        if (!millis_to_iso8601(cJSON_GetObjectItemCaseSensitive(row, "time"), iso_time, sizeof(iso_time))) {
            continue;
        }

        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "time", iso_time);
        add_number_or_null(entry, "extractionSeconds", cJSON_GetObjectItemCaseSensitive(row, "extractionSeconds"));
        add_string_or_null(entry, "doseMode", cJSON_GetObjectItemCaseSensitive(row, "doseMode"));
        add_string_or_null(entry, "doseIndex", cJSON_GetObjectItemCaseSensitive(row, "doseIndex"));
        cJSON *dose_value = cJSON_GetObjectItemCaseSensitive(row, "doseValue");
        if (dose_value == NULL) {
            dose_value = cJSON_GetObjectItemCaseSensitive(row, "doseValueNumerator");
        }
        add_number_or_null(entry, "doseValue", dose_value);
        add_number_or_null(entry, "targetTemperature", cJSON_GetObjectItemCaseSensitive(row, "targetTemperature"));
        cJSON_AddItemToArray(recent, entry);
    }
    return recent;
}

static cJSON *statistics_trend_points_json(cJSON *trend_output, const char *key)
{
    cJSON *points = cJSON_CreateArray();
    if (points == NULL) {
        return NULL;
    }

    cJSON *source_points = cJSON_GetObjectItemCaseSensitive(trend_output, key);
    if (!cJSON_IsArray(source_points)) {
        source_points = find_key_recursive(trend_output, key);
    }
    cJSON *row = NULL;
    cJSON_ArrayForEach(row, source_points) {
        if (!cJSON_IsObject(row)) {
            continue;
        }

        char iso_time[32] = {0};
        if (!millis_to_iso8601(cJSON_GetObjectItemCaseSensitive(row, "timestamp"), iso_time, sizeof(iso_time))) {
            continue;
        }
        double value = 0.0;
        if (!json_item_to_double(cJSON_GetObjectItemCaseSensitive(row, "value"), &value)) {
            continue;
        }

        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "timestamp", iso_time);
        cJSON_AddNumberToObject(entry, "value", value);
        cJSON_AddItemToArray(points, entry);
    }
    return points;
}

static void update_counter_snapshot(const char *serial_number, cJSON *counter_output)
{
    hub_machine_selection_identity_t identity = {0};
    if (serial_number == NULL
        || hub_config_get_machine_selection_identity(&identity) != ESP_OK
        || strcmp(identity.serial_number, serial_number) != 0) {
        return;
    }
    double value = 0.0;
    bool has_total_coffee = find_number_recursive(counter_output, "totalCoffee", &value);
    int total_coffee = (int)value;
    bool has_total_flush = find_number_recursive(counter_output, "totalFlush", &value);
    int total_flush = (int)value;
    portENTER_CRITICAL(&snapshot_lock);
    bool matches = cached_snapshot
        && snapshot.selection_generation == identity.generation
        && strcmp(snapshot.serial_number, identity.serial_number) == 0;
    if (matches && has_total_coffee) {
        snapshot.total_coffee = total_coffee;
        snapshot.total_coffee_set = true;
    }
    if (matches && has_total_flush) {
        snapshot.total_flush = total_flush;
        snapshot.total_flush_set = true;
    }
    portEXIT_CRITICAL(&snapshot_lock);
}

static bool counter_indicates_recent_coffees(cJSON *counter_output)
{
    double value = 0.0;
    if (find_number_recursive(counter_output, "totalCoffee", &value)) {
        return value > 0.0;
    }
    cloud_snapshot_t current = {0};
    bool has_cached = false;
    copy_snapshot(&current, &has_cached);
    hub_machine_selection_identity_t identity = {0};
    return has_cached
        && hub_config_get_machine_selection_identity(&identity) == ESP_OK
        && current.selection_generation == identity.generation
        && strcmp(current.serial_number, identity.serial_number) == 0
        && current.total_coffee_set
        && current.total_coffee > 0;
}

static bool statistics_output_has_counter(cJSON *counter_output)
{
    double value = 0.0;
    return find_number_recursive(counter_output, "totalCoffee", &value)
        || find_number_recursive(counter_output, "totalFlush", &value);
}

static bool statistics_array_has_rows(cJSON *array)
{
    return cJSON_IsArray(array) && cJSON_GetArraySize(array) > 0;
}

static bool statistics_json_has_meaningful_data(
    cJSON *counter_output,
    cJSON *recent,
    cJSON *coffees,
    cJSON *flushes
)
{
    return statistics_output_has_counter(counter_output)
        || statistics_array_has_rows(recent)
        || statistics_array_has_rows(coffees)
        || statistics_array_has_rows(flushes);
}

static bool statistics_missing_required_recent(cJSON *counter_output, cJSON *recent)
{
    return counter_indicates_recent_coffees(counter_output) && !statistics_array_has_rows(recent);
}

static bool statistics_missing_required_trend(cJSON *coffees, cJSON *flushes)
{
    return !statistics_array_has_rows(coffees)
        || !statistics_array_has_rows(flushes);
}

static esp_err_t statistics_best_error_for_missing_recent(esp_err_t recent_err)
{
    return recent_err != ESP_OK ? recent_err : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t statistics_best_error_for_empty_outputs(
    esp_err_t counter_err,
    esp_err_t recent_err,
    esp_err_t trend_err
)
{
    if (counter_err != ESP_OK) {
        return counter_err;
    }
    if (recent_err != ESP_OK) {
        return recent_err;
    }
    if (trend_err != ESP_OK) {
        return trend_err;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t cloud_fetch_things(
    const hub_cloud_settings_t *settings,
    const installation_key_t *key,
    const cloud_tokens_t *tokens,
    hub_machine_selection_t *selection
)
{
    char url[128] = {0};
    snprintf(url, sizeof(url), "%s/things", CUSTOMER_APP_URL);

    http_response_t response = {0};
    int status_code = 0;
    int64_t started_us = esp_timer_get_time();
    esp_err_t err = cloud_refresh_http_request(url, HTTP_METHOD_GET, key, tokens->access_token, NULL, &response, &status_code);
    uint32_t first_bytes = http_response_first_bytes(&response);
    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        record_readonly_http_diag(
            READONLY_DIAG_THINGS,
            err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err,
            status_code,
            (int)((esp_timer_get_time() - started_us) / 1000),
            response.len,
            first_bytes
        );
        hub_http_response_reset(&response);
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }

    cJSON *root = cJSON_Parse(response.data);
    hub_http_response_reset(&response);
    if (!cJSON_IsArray(root)) {
        record_readonly_http_diag(
            READONLY_DIAG_THINGS,
            ESP_ERR_INVALID_RESPONSE,
            status_code,
            (int)((esp_timer_get_time() - started_us) / 1000),
            response.len,
            first_bytes
        );
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *chosen = NULL;
    cJSON *item = NULL;
    char preferred_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};
    hub_machine_selection_t saved_selection = {0};
    if (hub_config_get_machine_selection(&saved_selection) == ESP_OK
        && saved_selection.serial_number[0] != '\0') {
        snprintf(preferred_serial, sizeof(preferred_serial), "%s", saved_selection.serial_number);
    } else if (settings != NULL && settings->cloud_serial_number[0] != '\0') {
        snprintf(preferred_serial, sizeof(preferred_serial), "%s", settings->cloud_serial_number);
    }
    cJSON_ArrayForEach(item, root) {
        const char *serial = json_optional_string(item, "serialNumber");
        if (serial[0] == '\0') {
            continue;
        }
        if (chosen == NULL || (preferred_serial[0] != '\0' && strcmp(serial, preferred_serial) == 0)) {
            chosen = item;
        }
        if (preferred_serial[0] != '\0' && strcmp(serial, preferred_serial) == 0) {
            break;
        }
    }

    if (chosen != NULL
        && preferred_serial[0] != '\0'
        && strcmp(json_optional_string(chosen, "serialNumber"), preferred_serial) != 0) {
        chosen = NULL;
    }

    if (chosen == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    memset(selection, 0, sizeof(*selection));
    snprintf(selection->serial_number, sizeof(selection->serial_number), "%s", json_optional_string(chosen, "serialNumber"));
    snprintf(selection->machine_name, sizeof(selection->machine_name), "%s", json_optional_string(chosen, "name"));
    snprintf(selection->ble_auth_token, sizeof(selection->ble_auth_token), "%s", json_optional_string(chosen, "bleAuthToken"));
    snprintf(selection->ble_device_name, sizeof(selection->ble_device_name), "%s", json_optional_string(chosen, "bleDeviceName"));
    snprintf(selection->ble_device_address, sizeof(selection->ble_device_address), "%s", json_optional_string(chosen, "bleDeviceAddress"));
    preserve_existing_machine_ble_identity(selection);
    ensure_machine_ble_name(selection);

    cJSON_Delete(root);
    record_readonly_http_diag(
        READONLY_DIAG_THINGS,
        ESP_OK,
        status_code,
        (int)((esp_timer_get_time() - started_us) / 1000),
        response.len,
        first_bytes
    );
    return ESP_OK;
}

static bool select_saved_machine_for_refresh(
    const hub_cloud_settings_t *settings,
    hub_machine_selection_t *selection
)
{
    if (selection == NULL) {
        return false;
    }

    memset(selection, 0, sizeof(*selection));
    hub_machine_selection_t saved = {0};
    if (hub_config_get_machine_selection(&saved) == ESP_OK && saved.serial_number[0] != '\0') {
        *selection = saved;
    } else if (settings != NULL && settings->cloud_serial_number[0] != '\0') {
        snprintf(selection->serial_number, sizeof(selection->serial_number), "%s", settings->cloud_serial_number);
    } else {
        return false;
    }

    if (selection->machine_name[0] == '\0') {
        snprintf(selection->machine_name, sizeof(selection->machine_name), "Selected machine");
    }
    ensure_machine_ble_name(selection);

    return true;
}

static void parse_machine_status(cJSON *output, cloud_snapshot_t *parsed)
{
    parsed->connected = strcmp(json_optional_string(output, "status"), "PoweredOn") == 0
        || strcmp(json_optional_string(output, "status"), "Brewing") == 0;
}

static void parse_coffee_boiler(cJSON *output, cloud_snapshot_t *parsed)
{
    double target = 0;
    if (json_number_to_double(output, "targetTemperature", &target)) {
        parsed->coffee_temperature = target;
        parsed->coffee_temperature_set = true;
    }
    (void)millis_number_to_iso8601(
        cJSON_GetObjectItemCaseSensitive(output, "readyStartTime"),
        parsed->coffee_boiler_ready_time,
        sizeof(parsed->coffee_boiler_ready_time)
    );
}

static void parse_steam_level(cJSON *output, cloud_snapshot_t *parsed)
{
    const char *level = json_optional_string(output, "targetLevel");
    if (strcmp(level, "Level1") == 0) {
        parsed->steam_level = 1;
        parsed->steam_level_set = true;
    } else if (strcmp(level, "Level2") == 0) {
        parsed->steam_level = 2;
        parsed->steam_level_set = true;
    } else if (strcmp(level, "Level3") == 0) {
        parsed->steam_level = 3;
        parsed->steam_level_set = true;
    }
    (void)millis_number_to_iso8601(
        cJSON_GetObjectItemCaseSensitive(output, "readyStartTime"),
        parsed->steam_boiler_ready_time,
        sizeof(parsed->steam_boiler_ready_time)
    );
}

static void parse_prebrew(cJSON *output, cloud_snapshot_t *parsed)
{
    snprintf(parsed->prebrew_mode, sizeof(parsed->prebrew_mode), "%s", json_optional_string(output, "mode"));
    cJSON *times = cJSON_GetObjectItemCaseSensitive(output, "times");
    cJSON *prebrewing = cJSON_GetObjectItemCaseSensitive(times, "PreBrewing");
    cJSON *first = cJSON_IsArray(prebrewing) ? cJSON_GetArrayItem(prebrewing, 0) : NULL;
    cJSON *seconds = cJSON_GetObjectItemCaseSensitive(first, "seconds");
    double value = 0;
    if (json_number_to_double(seconds, "In", &value)) {
        parsed->prebrew_on = value;
        parsed->prebrew_on_set = true;
    }
    if (json_number_to_double(seconds, "Out", &value)) {
        parsed->prebrew_off = value;
        parsed->prebrew_off_set = true;
    }
}

static void parse_brew_by_weight(cJSON *output, cloud_snapshot_t *parsed)
{
    snprintf(parsed->bbw_mode, sizeof(parsed->bbw_mode), "%s", json_optional_string(output, "mode"));
    cJSON *doses = cJSON_GetObjectItemCaseSensitive(output, "doses");
    cJSON *dose1 = cJSON_GetObjectItemCaseSensitive(doses, "Dose1");
    cJSON *dose2 = cJSON_GetObjectItemCaseSensitive(doses, "Dose2");
    double value = 0;
    if (json_number_to_double(dose1, "dose", &value)) {
        parsed->dose1 = value;
        parsed->dose1_set = true;
    }
    if (json_number_to_double(dose2, "dose", &value)) {
        parsed->dose2 = value;
        parsed->dose2_set = true;
    }
    cJSON *scale_connected = cJSON_GetObjectItemCaseSensitive(output, "scaleConnected");
    if (cJSON_IsBool(scale_connected)) {
        parsed->bbw_scale_connected = cJSON_IsTrue(scale_connected);
        parsed->bbw_scale_connected_set = true;
    }
}

static void parse_thing_scale(cJSON *output, cloud_snapshot_t *parsed)
{
    cJSON *connected = cJSON_GetObjectItemCaseSensitive(output, "connected");
    if (cJSON_IsBool(connected)) {
        parsed->thing_scale_connected = cJSON_IsTrue(connected);
        parsed->thing_scale_connected_set = true;
    }

    double value = 0;
    cJSON *battery_level = cJSON_GetObjectItemCaseSensitive(output, "batteryLevel");
    if (json_item_to_double(battery_level, &value) && isfinite(value) && value >= 0 && value <= 100) {
        parsed->scale_battery_level = value;
        parsed->scale_battery_level_set = true;
    }

    cJSON *calibration_required = cJSON_GetObjectItemCaseSensitive(output, "calibrationRequired");
    if (cJSON_IsBool(calibration_required)) {
        parsed->scale_calibration_required = cJSON_IsTrue(calibration_required);
        parsed->scale_calibration_required_set = true;
    }

    const char *name = json_optional_string(output, "name");
    if (name[0] != '\0') {
        snprintf(parsed->scale_name, sizeof(parsed->scale_name), "%s", name);
    }
}

static void preserve_missing_bbw_fields(
    cloud_snapshot_t *parsed,
    const bbw_snapshot_fields_t *previous_bbw
)
{
    if (parsed == NULL || previous_bbw == NULL) {
        return;
    }

    if (!parsed->dose1_set && previous_bbw->dose1_set) {
        parsed->dose1 = previous_bbw->dose1;
        parsed->dose1_set = true;
    }
    if (!parsed->dose2_set && previous_bbw->dose2_set) {
        parsed->dose2 = previous_bbw->dose2;
        parsed->dose2_set = true;
    }
    if (parsed->bbw_mode[0] == '\0' && previous_bbw->mode[0] != '\0') {
        snprintf(parsed->bbw_mode, sizeof(parsed->bbw_mode), "%s", previous_bbw->mode);
    } else if (
        (strcmp(previous_bbw->mode, "Dose1") == 0 || strcmp(previous_bbw->mode, "Dose2") == 0)
        && strcmp(parsed->bbw_mode, "Continuous") == 0
    ) {
        snprintf(parsed->bbw_mode, sizeof(parsed->bbw_mode), "%s", previous_bbw->mode);
    }
}

static esp_err_t parse_dashboard_snapshot(
    const char *json,
    const char *expected_serial_number,
    uint32_t expected_selection_generation,
    const cloud_snapshot_t *previous,
    cloud_snapshot_t *parsed,
    char *parsed_name,
    size_t parsed_name_len
)
{
    if (expected_serial_number == NULL
        || expected_serial_number[0] == '\0'
        || previous == NULL
        || parsed == NULL
        || parsed_name == NULL
        || parsed_name_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!cJSON_IsObject(root) || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "widgets"))) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *response_serial = json_optional_string(root, "serialNumber");
    if (response_serial[0] != '\0' && strcmp(response_serial, expected_serial_number) != 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(parsed, 0, sizeof(*parsed));
    snprintf(parsed->serial_number, sizeof(parsed->serial_number), "%s", expected_serial_number);
    parsed->selection_generation = expected_selection_generation;
    snprintf(parsed_name, parsed_name_len, "%s", json_optional_string(root, "name"));
    bool same_machine = previous->serial_number[0] == '\0'
        || (previous->selection_generation == expected_selection_generation
            && strcmp(previous->serial_number, expected_serial_number) == 0);

    bbw_snapshot_fields_t previous_bbw = {
        .dose1_set = previous->dose1_set,
        .dose1 = previous->dose1,
        .dose2_set = previous->dose2_set,
        .dose2 = previous->dose2,
    };
    snprintf(previous_bbw.mode, sizeof(previous_bbw.mode), "%s", previous->bbw_mode);
    parsed->connected = json_optional_bool(root, "connected");

    cJSON *widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
    cJSON *widget = NULL;
    cJSON_ArrayForEach(widget, widgets) {
        const char *code = json_optional_string(widget, "code");
        cJSON *output = cJSON_GetObjectItemCaseSensitive(widget, "output");
        if (!cJSON_IsObject(output)) {
            continue;
        }

        if (strcmp(code, "CMMachineStatus") == 0) {
            // Standby is an operating mode, not loss of the Cloud connection.
            if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "connected")))
                parse_machine_status(output, parsed);
        } else if (strcmp(code, "CMCoffeeBoiler") == 0) {
            parse_coffee_boiler(output, parsed);
        } else if (strcmp(code, "CMSteamBoilerLevel") == 0) {
            parse_steam_level(output, parsed);
        } else if (strcmp(code, "CMPreBrewing") == 0 || strcmp(code, "CMPreExtraction") == 0) {
            parse_prebrew(output, parsed);
        } else if (strcmp(code, "CMBrewByWeightDoses") == 0) {
            parse_brew_by_weight(output, parsed);
        } else if (strcmp(code, "ThingScale") == 0) {
            parse_thing_scale(output, parsed);
        }
    }

    if (same_machine) {
        preserve_missing_bbw_fields(parsed, &previous_bbw);
        if (previous->total_coffee_set) {
            parsed->total_coffee = previous->total_coffee;
            parsed->total_coffee_set = true;
        }
        if (previous->total_flush_set) {
            parsed->total_flush = previous->total_flush;
            parsed->total_flush_set = true;
        }
    }

    time_t now = time(NULL);
    struct tm tm = {0};
    gmtime_r(&now, &tm);
    strftime(parsed->last_updated, sizeof(parsed->last_updated), "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (parsed->bbw_scale_connected_set || parsed->thing_scale_connected_set) {
        parsed->scale_observed_ms = esp_timer_get_time() / 1000;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static bool scale_snapshot_fresh(const cloud_snapshot_t *current, int64_t now)
{
    return current->connected && current->scale_observed_ms > 0
        && now >= current->scale_observed_ms && now - current->scale_observed_ms < 60000;
}

static bool publish_scale_snapshot(const cloud_snapshot_t *observed)
{
    hub_machine_selection_identity_t target = {.generation = observed->selection_generation};
    snprintf(target.serial_number, sizeof(target.serial_number), "%s", observed->serial_number);
    if (!hub_config_machine_selection_matches(&target)) return false;
    portENTER_CRITICAL(&snapshot_lock);
    bool matches = cached_snapshot && snapshot.selection_generation == target.generation
        && strcmp(snapshot.serial_number, target.serial_number) == 0;
    if (matches) {
        // Replace missing fields too; a successful read must not renew an older accessory value.
        snapshot.bbw_scale_connected_set = observed->bbw_scale_connected_set;
        snapshot.bbw_scale_connected = observed->bbw_scale_connected;
        snapshot.thing_scale_connected_set = observed->thing_scale_connected_set;
        snapshot.thing_scale_connected = observed->thing_scale_connected;
        snapshot.scale_battery_level_set = observed->scale_battery_level_set;
        snapshot.scale_battery_level = observed->scale_battery_level;
        snapshot.scale_calibration_required_set = observed->scale_calibration_required_set;
        snapshot.scale_calibration_required = observed->scale_calibration_required;
        snprintf(snapshot.scale_name, sizeof(snapshot.scale_name), "%s", observed->scale_name);
        snapshot.scale_observed_ms = observed->scale_observed_ms;
        snapshot.connected = observed->connected;
    }
    portEXIT_CRITICAL(&snapshot_lock);
    return matches;
}

esp_err_t hub_cloud_refresh_scale(void)
{
    if (hub_command_interactive_pending() || cloud_command_is_waiting()) return ESP_ERR_INVALID_STATE;
    hub_machine_selection_identity_t target = {0};
    if (hub_config_get_machine_selection_identity(&target) != ESP_OK) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(ensure_refresh_mutex(), TAG, "Scale refresh mutex unavailable");
    if (xSemaphoreTake(refresh_mutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_ERR_INVALID_STATE;
    hub_cloud_settings_t *settings = NULL;
    installation_key_t *key = NULL;
    cloud_tokens_t *tokens = NULL;
    http_response_t response = {0};
    int status_code = 0;
    int64_t started_us = esp_timer_get_time();
    if (hub_command_interactive_pending() || cloud_command_is_waiting()) goto cleanup;
    settings = heap_caps_calloc(1, sizeof(*settings), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    key = heap_caps_calloc(1, sizeof(*key), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tokens = heap_caps_calloc(1, sizeof(*tokens), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!settings || !key || !tokens) { err = ESP_ERR_NO_MEM; goto cleanup; }
    err = load_cloud_context(settings, key, tokens);
    if (err != ESP_OK) { (void)hub_cloud_request_refresh(); goto cleanup; }
    if (!hub_config_machine_selection_matches(&target)) { err = ESP_ERR_INVALID_STATE; goto cleanup; }
    if (hub_command_interactive_pending() || cloud_command_is_waiting()) { err = ESP_ERR_INVALID_STATE; goto cleanup; }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        < CLOUD_READONLY_MIN_INTERNAL_BLOCK_BYTES) { err = ESP_ERR_NO_MEM; goto cleanup; }
    char url[160];
    snprintf(url, sizeof(url), "%s/things/%s/dashboard", CUSTOMER_APP_URL, target.serial_number);
    // One GET, no retries or discovery; keep the shared TLS path available for controls.
    err = cloud_http_request_no_retry(url, HTTP_METHOD_GET, key, tokens->access_token,
                                    NULL, &response, &status_code, CLOUD_READONLY_HTTP_TIMEOUT_MS);
    if (status_code == 401 || status_code == 403) {
        invalidate_cached_tokens();
        (void)hub_cloud_request_refresh();
    }
    if (err == ESP_OK && status_code != 200) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        cloud_snapshot_t previous = {0}, observed = {0};
        char name[HUB_CONFIG_MAX_MACHINE_NAME_LEN + 1];
        err = parse_dashboard_snapshot(response.data, target.serial_number, target.generation,
                                       &previous, &observed, name, sizeof(name));
        if (err == ESP_OK && !publish_scale_snapshot(&observed)) err = ESP_ERR_INVALID_STATE;
    }
cleanup:
    record_readonly_http_diag(READONLY_DIAG_DASHBOARD, err, status_code,
        (int)((esp_timer_get_time() - started_us) / 1000), response.len, http_response_first_bytes(&response));
    hub_http_response_reset(&response);
    free_cloud_context(settings, key, tokens);
    cloud_readonly_io_unlock();
    return err;
}

static esp_err_t cloud_fetch_dashboard(
    const installation_key_t *key,
    const cloud_tokens_t *tokens,
    const hub_machine_selection_t *selection,
    const hub_machine_selection_identity_t *target
)
{
    if (selection == NULL || target == NULL || selection->serial_number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char url[160] = {0};
    snprintf(url, sizeof(url), "%s/things/%s/dashboard", CUSTOMER_APP_URL, selection->serial_number);

    http_response_t response = {0};
    int status_code = 0;
    int64_t started_us = esp_timer_get_time();
    esp_err_t err = cloud_refresh_http_request(url, HTTP_METHOD_GET, key, tokens->access_token, NULL, &response, &status_code);
    uint32_t first_bytes = http_response_first_bytes(&response);
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        cloud_snapshot_t previous = {0};
        copy_snapshot(&previous, NULL);
        cloud_snapshot_t parsed = {0};
        char parsed_name[HUB_CONFIG_MAX_MACHINE_NAME_LEN + 1] = {0};
        if (!hub_config_machine_selection_matches(target)) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = parse_dashboard_snapshot(
                response.data,
                selection->serial_number,
                target->generation,
                &previous,
                &parsed,
                parsed_name,
                sizeof(parsed_name)
            );
        }
        if (err == ESP_OK && !hub_config_machine_selection_matches(target)) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            portENTER_CRITICAL(&snapshot_lock);
            snapshot = parsed;
            cached_snapshot = true;
            portEXIT_CRITICAL(&snapshot_lock);
            snprintf(machine_serial, sizeof(machine_serial), "%s", selection->serial_number);
            snprintf(
                machine_name,
                sizeof(machine_name),
                "%s",
                parsed_name[0] != '\0'
                    ? parsed_name
                    : (selection->machine_name[0] != '\0' ? selection->machine_name : "Selected machine")
            );
            machine_selected = true;
            ble_token_cached = selection->ble_auth_token[0] != '\0';
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    record_readonly_http_diag(
        READONLY_DIAG_DASHBOARD,
        err,
        status_code,
        (int)((esp_timer_get_time() - started_us) / 1000),
        response.len,
        first_bytes
    );
    hub_http_response_reset(&response);
    return err;
}

esp_err_t hub_cloud_refresh(void)
{
    if (hub_command_interactive_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ensure_refresh_mutex(), TAG, "Cloud refresh mutex unavailable");
    if (hub_command_interactive_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(refresh_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hub_command_interactive_pending()) {
        xSemaphoreGive(refresh_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    set_refresh_running(true);

    esp_err_t result = ESP_FAIL;
    hub_cloud_settings_t *settings = calloc(1, sizeof(*settings));
    hub_machine_selection_t *selection = calloc(1, sizeof(*selection));
    installation_key_t *installation_key = calloc(1, sizeof(*installation_key));
    cloud_tokens_t *tokens = calloc(1, sizeof(*tokens));

    if (settings == NULL || selection == NULL || installation_key == NULL || tokens == NULL) {
        status = HUB_CLOUD_STATUS_ERROR;
        set_error("Cloud refresh failed because ESP32-S3 memory is low.");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Starting Cloud refresh");
    esp_err_t settings_err = hub_config_get_cloud_settings(settings);
    last_refresh_ms = esp_timer_get_time() / 1000;

    if (
        settings_err != ESP_OK
        || settings->username[0] == '\0'
        || settings->installation_key[0] == '\0'
        || (settings->password[0] == '\0' && settings->refresh_token[0] == '\0')
    ) {
        status = HUB_CLOUD_STATUS_UNCONFIGURED;
        clear_cached_fields();
        set_error("Cloud credentials are not configured.");
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    if (status != HUB_CLOUD_STATUS_AUTHENTICATED) {
        status = HUB_CLOUD_STATUS_CONFIGURED;
    }

    if (ensure_time_synced() != ESP_OK) {
        status = HUB_CLOUD_STATUS_ERROR;
        set_error("Cloud refresh failed because ESP32-S3 time is not synced.");
        result = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Cloud time synced");

    if (parse_installation_key(settings->installation_key, installation_key) != ESP_OK) {
        status = HUB_CLOUD_STATUS_ERROR;
        set_error("Cloud installation key format is invalid.");
        result = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    bool used_cached_tokens = copy_cached_tokens_for_refresh(tokens);
    if (used_cached_tokens) {
        clear_auth_error();
    } else {
        esp_err_t auth_err = get_access_tokens(settings, installation_key, tokens);
        if (auth_err != ESP_OK) {
            bool retained_valid_token = cloud_http_retryable_error(auth_err)
                && copy_cached_tokens_if_valid(tokens)
                && hub_cloud_has_cached_snapshot();
            if (retained_valid_token) {
                status = HUB_CLOUD_STATUS_AUTHENTICATED;
                clear_auth_error();
                set_error("");
                defer_token_renewal_retry();
                result = ESP_OK;
                ESP_LOGW(
                    TAG,
                    "Cloud token renewal deferred; cached control token remains usable: %s",
                    esp_err_to_name(auth_err)
                );
                goto cleanup;
            }
            status = HUB_CLOUD_STATUS_ERROR;
            set_authentication_failed_error();
            invalidate_cached_tokens();
            result = auth_err;
            goto cleanup;
        }
    }
    ESP_LOGI(TAG, "Cloud authentication succeeded");
    if (!used_cached_tokens) {
        (void)hub_config_save_cloud_tokens(tokens->refresh_token);
        update_cached_tokens(tokens);
    }

    hub_machine_selection_identity_t selection_before_refresh = {0};
    esp_err_t identity_err = hub_config_get_machine_selection_identity(&selection_before_refresh);
    if (identity_err != ESP_OK && identity_err != ESP_ERR_INVALID_STATE) {
        status = HUB_CLOUD_STATUS_ERROR;
        snprintf(last_error, sizeof(last_error), "Machine selection read failed: %s.", esp_err_to_name(identity_err));
        result = identity_err;
        goto cleanup;
    }

    esp_err_t things_err = cloud_fetch_things(settings, installation_key, tokens, selection);
    if (things_err != ESP_OK) {
        if (!select_saved_machine_for_refresh(settings, selection)) {
            status = HUB_CLOUD_STATUS_ERROR;
            snprintf(last_error, sizeof(last_error), "Cloud things request failed: %s.", esp_err_to_name(things_err));
            result = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
        ESP_LOGW(TAG, "Cloud things request failed: %s; using saved machine selection", esp_err_to_name(things_err));
    }

    esp_err_t save_selection_err = hub_config_save_machine_selection_if_unchanged(
        &selection_before_refresh,
        selection->serial_number,
        selection->machine_name,
        selection->ble_auth_token,
        selection->ble_device_name,
        selection->ble_device_address
    );
    if (save_selection_err != ESP_OK) {
        status = HUB_CLOUD_STATUS_ERROR;
        snprintf(
            last_error,
            sizeof(last_error),
            "Machine selection changed during Cloud refresh: %s.",
            esp_err_to_name(save_selection_err)
        );
        result = save_selection_err;
        goto cleanup;
    }

    hub_machine_selection_identity_t refresh_target = {0};
    if (hub_config_get_machine_selection_identity(&refresh_target) != ESP_OK) {
        status = HUB_CLOUD_STATUS_ERROR;
        set_error("Machine selection became unavailable during Cloud refresh.");
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    esp_err_t dashboard_err = cloud_fetch_dashboard(
        installation_key,
        tokens,
        selection,
        &refresh_target
    );
    if (dashboard_err != ESP_OK) {
        snprintf(last_error, sizeof(last_error), "Cloud dashboard request failed: %s.", esp_err_to_name(dashboard_err));
        cloud_snapshot_t previous_snapshot = {0};
        bool has_previous_snapshot = false;
        copy_snapshot(&previous_snapshot, &has_previous_snapshot);
        if (has_previous_snapshot
            && previous_snapshot.selection_generation == refresh_target.generation
            && strcmp(previous_snapshot.serial_number, selection->serial_number) == 0
            && hub_config_machine_selection_matches(&refresh_target)) {
            status = HUB_CLOUD_STATUS_AUTHENTICATED;
            result = ESP_OK;
            ESP_LOGW(TAG, "Cloud dashboard request failed but cached snapshot remains usable: %s", esp_err_to_name(dashboard_err));
            goto cleanup;
        }
        status = HUB_CLOUD_STATUS_ERROR;
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    status = HUB_CLOUD_STATUS_AUTHENTICATED;
    set_error("");
    result = ESP_OK;
    ESP_LOGI(TAG, "Cloud refresh completed");

cleanup:
    if (settings != NULL) {
        memset(settings, 0, sizeof(*settings));
        free(settings);
    }
    if (selection != NULL) {
        memset(selection, 0, sizeof(*selection));
        free(selection);
    }
    if (installation_key != NULL) {
        memset(installation_key, 0, sizeof(*installation_key));
        free(installation_key);
    }
    if (tokens != NULL) {
        memset(tokens, 0, sizeof(*tokens));
        free(tokens);
    }
    mark_cloud_io_completed();
    set_refresh_running(false);
    xSemaphoreGive(refresh_mutex);
    return result;
}

static bool snapshot_matches_selection(
    const cloud_snapshot_t *candidate,
    bool has_cached_snapshot
)
{
    hub_machine_selection_identity_t identity = {0};
    return candidate != NULL
        && has_cached_snapshot
        && candidate->serial_number[0] != '\0'
        && hub_config_get_machine_selection_identity(&identity) == ESP_OK
        && candidate->selection_generation == identity.generation
        && strcmp(candidate->serial_number, identity.serial_number) == 0;
}

static bool cached_snapshot_matches_selection(void)
{
    cloud_snapshot_t candidate = {0};
    bool has_cached_snapshot = false;
    copy_snapshot(&candidate, &has_cached_snapshot);
    return snapshot_matches_selection(&candidate, has_cached_snapshot);
}

void hub_cloud_get_snapshot_status(hub_cloud_snapshot_status_t *out)
{
    if (out == NULL) {
        return;
    }

    cloud_snapshot_t current = {0};
    bool has_cached_snapshot = false;
    copy_snapshot(&current, &has_cached_snapshot);
    bool snapshot_usable = snapshot_matches_selection(&current, has_cached_snapshot);
    bool has_complete_bbw = snapshot_usable
        && current.dose1_set
        && current.dose2_set
        && current.bbw_mode[0] != '\0';

    memset(out, 0, sizeof(*out));
    out->has_cached_snapshot = snapshot_usable;
    out->is_connected = snapshot_usable && current.connected;
    out->has_complete_bbw = has_complete_bbw;
    out->is_ready = snapshot_usable && current.connected && has_complete_bbw;
    out->refresh_pending = cloud_refresh_pending_snapshot();
    out->last_refresh_ms = last_refresh_ms;
    out->scale_observed_ms = snapshot_usable ? current.scale_observed_ms : 0;
    out->dose1 = current.dose1;
    out->dose2 = current.dose2;
}

bool hub_cloud_controls_available(void)
{
    return hub_cloud_configured()
        && machine_selected
        && machine_serial[0] != '\0'
        && cached_snapshot_matches_selection();
}

static bool cloud_controls_ready_for_command(void)
{
    return hub_cloud_controls_available();
}

static esp_err_t ensure_cloud_controls_ready(void)
{
    if (cloud_controls_ready_for_command()) {
        return ESP_OK;
    }
    (void)hub_cloud_request_refresh();
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t ensure_cloud_controls_ready_or_fail(hub_cloud_command_result_t *result, int64_t started_us)
{
    esp_err_t ready_err = ensure_cloud_controls_ready();
    if (ready_err != ESP_OK) {
        set_command_result(result, false, started_us, "Cloud control is unavailable. Refresh Cloud diagnostics first.");
    }
    return ready_err;
}

bool hub_cloud_readonly_available(void)
{
    return hub_cloud_configured() && hub_cloud_machine_selected();
}

esp_err_t hub_cloud_statistics_json_alloc_for_serial(
    const char *expected_serial,
    int days,
    char **body_out
)
{
    if (body_out == NULL
        || days < 1
        || days > 30
        || (expected_serial != NULL
            && strlen(expected_serial) > HUB_CONFIG_MAX_MACHINE_SERIAL_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    *body_out = NULL;

    esp_err_t err = ESP_FAIL;
    hub_cloud_settings_t *settings = calloc(1, sizeof(*settings));
    installation_key_t *installation_key = calloc(1, sizeof(*installation_key));
    cloud_tokens_t *tokens = calloc(1, sizeof(*tokens));
    cJSON *counter_output = NULL;
    cJSON *recent_output = NULL;
    cJSON *trend_output = NULL;
    char *counter_json = NULL;
    char *recent_json = NULL;
    char *trend_json = NULL;
    cJSON *root = NULL;

    if (settings == NULL || installation_key == NULL || tokens == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = load_cloud_context(settings, installation_key, tokens);
    if (err != ESP_OK) {
        (void)hub_cloud_request_refresh();
        goto cleanup;
    }

    char serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};
    if (!selected_machine_serial_for_request(settings, serial, sizeof(serial))) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    if (expected_serial != NULL
        && expected_serial[0] != '\0'
        && strcmp(serial, expected_serial) != 0) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    char url[224] = {0};
    snprintf(url, sizeof(url), "%s/things/%s/stats/COFFEE_AND_FLUSH_COUNTER/1", CUSTOMER_APP_URL, serial);
    esp_err_t counter_err = cloud_fetch_json_text(
        url,
        installation_key,
        tokens,
        READONLY_DIAG_COUNTER,
        &counter_json
    );
    if (counter_err != ESP_OK) {
        err = counter_err;
        ESP_LOGW(
            TAG,
            "Cloud statistics counter request failed: %s",
            esp_err_to_name(err)
        );
        goto cleanup;
    }
    err = cloud_statistics_pause_between_endpoints();
    if (err != ESP_OK) {
        goto cleanup;
    }

    int recent_days = days < 30 ? 30 : days;
    snprintf(url, sizeof(url), "%s/things/%s/stats/LAST_COFFEE/1?days=%d", CUSTOMER_APP_URL, serial, recent_days);
    esp_err_t recent_err = cloud_fetch_json_text(
        url,
        installation_key,
        tokens,
        READONLY_DIAG_RECENT,
        &recent_json
    );
    if (recent_err == ESP_OK) {
        err = cloud_statistics_pause_between_endpoints();
        if (err != ESP_OK) {
            goto cleanup;
        }
    }
    snprintf(
        url,
        sizeof(url),
        "%s/things/%s/stats/COFFEE_AND_FLUSH_TREND/1?days=%d&timezone=Asia/Shanghai",
        CUSTOMER_APP_URL,
        serial,
        days
    );
    esp_err_t trend_err = cloud_fetch_json_text(
        url,
        installation_key,
        tokens,
        READONLY_DIAG_TREND,
        &trend_json
    );

    counter_err = cloud_parse_json_text(counter_json, true, &counter_output);
    if (counter_err != ESP_OK || !statistics_output_has_counter(counter_output)) {
        err = counter_err != ESP_OK ? counter_err : ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(
            TAG,
            "Cloud statistics rejected candidate without a complete counter source: %s",
            esp_err_to_name(err)
        );
        goto cleanup;
    }
    update_counter_snapshot(serial, counter_output);
    if (recent_err == ESP_OK) {
        recent_err = cloud_parse_json_text(recent_json, true, &recent_output);
    }
    if (trend_err == ESP_OK) {
        trend_err = cloud_parse_json_text(trend_json, true, &trend_output);
    }

    root = cJSON_CreateObject();
    cJSON *counter = cJSON_CreateObject();
    cJSON *recent = statistics_recent_coffees_json(recent_output);
    cJSON *trend = cJSON_CreateObject();
    cJSON *coffees = statistics_trend_points_json(trend_output, "coffees");
    cJSON *flushes = statistics_trend_points_json(trend_output, "flushes");
    if (root == NULL || counter == NULL || recent == NULL || trend == NULL || coffees == NULL || flushes == NULL) {
        cJSON_Delete(counter);
        cJSON_Delete(recent);
        cJSON_Delete(trend);
        cJSON_Delete(coffees);
        cJSON_Delete(flushes);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    if (!statistics_json_has_meaningful_data(counter_output, recent, coffees, flushes)) {
        cJSON_Delete(counter);
        cJSON_Delete(recent);
        cJSON_Delete(trend);
        cJSON_Delete(coffees);
        cJSON_Delete(flushes);
        err = statistics_best_error_for_empty_outputs(counter_err, recent_err, trend_err);
        ESP_LOGW(
            TAG,
            "Cloud statistics returned no usable data: counter=%s recent=%s trend=%s",
            esp_err_to_name(counter_err),
            esp_err_to_name(recent_err),
            esp_err_to_name(trend_err)
        );
        goto cleanup;
    }

    if (statistics_missing_required_recent(counter_output, recent)) {
        cJSON_Delete(counter);
        cJSON_Delete(recent);
        cJSON_Delete(trend);
        cJSON_Delete(coffees);
        cJSON_Delete(flushes);
        err = statistics_best_error_for_missing_recent(recent_err);
        ESP_LOGW(
            TAG,
            "Cloud statistics returned counters without required recent records: counter=%s recent=%s",
            esp_err_to_name(counter_err),
            esp_err_to_name(recent_err)
        );
        goto cleanup;
    }

    if (statistics_missing_required_trend(coffees, flushes)) {
        cJSON_Delete(counter);
        cJSON_Delete(recent);
        cJSON_Delete(trend);
        cJSON_Delete(coffees);
        cJSON_Delete(flushes);
        err = trend_err != ESP_OK ? trend_err : ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(
            TAG,
            "Cloud statistics returned no complete coffee/flush trend: %s",
            esp_err_to_name(err)
        );
        goto cleanup;
    }

    add_counter_number_or_null(
        counter,
        "totalCoffee",
        counter_output,
        false,
        0
    );
    add_counter_number_or_null(
        counter,
        "totalFlush",
        counter_output,
        false,
        0
    );
    cJSON_AddItemToObject(root, "counter", counter);
    cJSON_AddItemToObject(root, "recentCoffees", recent);
    cJSON_AddNumberToObject(trend, "days", days);
    cJSON *timezone = cJSON_GetObjectItemCaseSensitive(trend_output, "timezone");
    cJSON_AddStringToObject(
        trend,
        "timezone",
        cJSON_IsString(timezone) && timezone->valuestring != NULL ? timezone->valuestring : "Asia/Shanghai"
    );
    cJSON_AddItemToObject(trend, "coffees", coffees);
    cJSON_AddItemToObject(trend, "flushes", flushes);
    if (trend_err == ESP_OK) {
        cJSON_AddItemToObject(root, "trend", trend);
    } else {
        cJSON_AddNullToObject(root, "trend");
        cJSON_Delete(trend);
    }

    char *printed = cJSON_PrintUnformatted(root);
    if (printed == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    *body_out = printed;
    err = ESP_OK;
    if (err == ESP_OK && counter_err != ESP_OK && recent_err != ESP_OK && trend_err != ESP_OK) {
        ESP_LOGW(TAG, "Cloud statistics returned cached/empty data because all statistics requests failed");
    }

cleanup:
    free(counter_json);
    free(recent_json);
    free(trend_json);
    cJSON_Delete(root);
    cJSON_Delete(counter_output);
    cJSON_Delete(recent_output);
    cJSON_Delete(trend_output);
    free_cloud_context(settings, installation_key, tokens);
    return err;
}

esp_err_t hub_cloud_latest_coffee_json(const char *expected_serial, char **body_out)
{
    if (!expected_serial || !expected_serial[0] || !body_out) return ESP_ERR_INVALID_ARG;
    *body_out = NULL;
    if (hub_command_interactive_pending()) return ESP_ERR_TIMEOUT;
    hub_machine_selection_identity_t selection = {0};
    esp_err_t err = hub_config_get_machine_selection_identity(&selection);
    if (err != ESP_OK || strcmp(selection.serial_number, expected_serial)) return ESP_ERR_INVALID_STATE;
    hub_cloud_settings_t *settings = calloc(1, sizeof(*settings));
    installation_key_t *key = calloc(1, sizeof(*key));
    cloud_tokens_t *tokens = calloc(1, sizeof(*tokens));
    if (!settings || !key || !tokens) { err = ESP_ERR_NO_MEM; goto cleanup; }
    err = load_cloud_context(settings, key, tokens);
    if (err != ESP_OK) { (void)hub_cloud_request_refresh(); goto cleanup; }
    char url[224];
    int length = snprintf(url, sizeof(url), "%s/things/%s/stats/LAST_COFFEE/1?days=1", CUSTOMER_APP_URL, expected_serial);
    if (length < 0 || (size_t)length >= sizeof(url)) { err = ESP_ERR_INVALID_SIZE; goto cleanup; }
    err = cloud_fetch_json_text(url, key, tokens, READONLY_DIAG_RECENT, body_out);
    if (err == ESP_OK && !hub_config_machine_selection_matches(&selection)) {
        free(*body_out); *body_out = NULL; err = ESP_ERR_INVALID_STATE;
    }
cleanup:
    free_cloud_context(settings, key, tokens);
    return err;
}

esp_err_t hub_cloud_statistics_json_alloc(int days, char **body_out)
{
    return hub_cloud_statistics_json_alloc_for_serial(NULL, days, body_out);
}

esp_err_t hub_cloud_statistics_json(int days, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *body = NULL;
    esp_err_t err = hub_cloud_statistics_json_alloc(days, &body);
    if (err == ESP_OK) {
        if (strlen(body) >= buffer_len) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            snprintf(buffer, buffer_len, "%s", body);
        }
    }
    cJSON_free(body);
    return err;
}

esp_err_t hub_cloud_schedule_json(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;
    hub_cloud_settings_t *settings = calloc(1, sizeof(*settings));
    installation_key_t *installation_key = calloc(1, sizeof(*installation_key));
    cloud_tokens_t *tokens = calloc(1, sizeof(*tokens));
    cJSON *schedule_payload = NULL;
    cJSON *root = NULL;

    if (settings == NULL || installation_key == NULL || tokens == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = load_cloud_context(settings, installation_key, tokens);
    if (err != ESP_OK) {
        (void)hub_cloud_request_refresh();
        goto cleanup;
    }

    char serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};
    if (!selected_machine_serial_for_request(settings, serial, sizeof(serial))) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    char url[192] = {0};
    snprintf(url, sizeof(url), "%s/things/%s/scheduling", CUSTOMER_APP_URL, serial);
    err = cloud_fetch_json_payload(
        url,
        installation_key,
        tokens,
        false,
        READONLY_DIAG_SCHEDULE,
        &schedule_payload
    );
    if (err != ESP_OK) {
        goto cleanup;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    cJSON *redacted_raw = cJSON_Duplicate(schedule_payload, true);
    if (redacted_raw == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    redact_secret_fields(redacted_raw);
    cJSON_AddItemToObject(root, "raw", redacted_raw);
    err = cjson_print_to_buffer(root, buffer, buffer_len);

cleanup:
    cJSON_Delete(root);
    cJSON_Delete(schedule_payload);
    free_cloud_context(settings, installation_key, tokens);
    return err;
}

esp_err_t hub_cloud_firmware_json(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;
    hub_cloud_settings_t *settings = calloc(1, sizeof(*settings));
    installation_key_t *installation_key = calloc(1, sizeof(*installation_key));
    cloud_tokens_t *tokens = calloc(1, sizeof(*tokens));
    cJSON *firmware_payload = NULL;
    cJSON *root = NULL;

    if (settings == NULL || installation_key == NULL || tokens == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = load_cloud_context(settings, installation_key, tokens);
    if (err != ESP_OK) {
        (void)hub_cloud_request_refresh();
        goto cleanup;
    }

    char serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};
    if (!selected_machine_serial_for_request(settings, serial, sizeof(serial))) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    char url[192] = {0};
    snprintf(url, sizeof(url), "%s/things/%s/update-fw", CUSTOMER_APP_URL, serial);
    err = cloud_fetch_json_payload(
        url,
        installation_key,
        tokens,
        false,
        READONLY_DIAG_FIRMWARE,
        &firmware_payload
    );
    if (err != ESP_OK) {
        goto cleanup;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    add_string_or_null(root, "status", cJSON_GetObjectItemCaseSensitive(firmware_payload, "status"));
    add_string_or_null(root, "commandStatus", cJSON_GetObjectItemCaseSensitive(firmware_payload, "commandStatus"));
    add_number_or_null(root, "progressPercentage", cJSON_GetObjectItemCaseSensitive(firmware_payload, "progressPercentage"));
    cJSON *redacted_raw = cJSON_Duplicate(firmware_payload, true);
    if (redacted_raw == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    redact_secret_fields(redacted_raw);
    cJSON_AddItemToObject(root, "raw", redacted_raw);
    err = cjson_print_to_buffer(root, buffer, buffer_len);

cleanup:
    cJSON_Delete(root);
    cJSON_Delete(firmware_payload);
    free_cloud_context(settings, installation_key, tokens);
    return err;
}

static esp_err_t read_control_snapshot_locked(
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    cloud_snapshot_t *out
)
{
    int64_t started_us = esp_timer_get_time();
    hub_machine_selection_identity_t target = {0};
    if (out == NULL || !active_command_target(&target)
        || reserve_after_ms > UINT32_MAX - 2 * HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS) {
        record_readonly_http_diag(READONLY_DIAG_DASHBOARD, ESP_ERR_INVALID_STATE, 0,
                                  (int)((esp_timer_get_time() - started_us) / 1000), 0, 0);
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));
    hub_cloud_settings_t *settings = heap_caps_calloc(
        1, sizeof(*settings), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    installation_key_t *key = heap_caps_calloc(
        1, sizeof(*key), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    cloud_tokens_t *tokens = heap_caps_calloc(
        1, sizeof(*tokens), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (settings == NULL || key == NULL || tokens == NULL) {
        free_cloud_context(settings, key, tokens);
        record_readonly_http_diag(READONLY_DIAG_DASHBOARD, ESP_ERR_NO_MEM, 0,
                                  (int)((esp_timer_get_time() - started_us) / 1000), 0, 0);
        return ESP_ERR_NO_MEM;
    }
    http_response_t response = {0};
    int status_code = 0;
    esp_err_t err = load_cloud_context(settings, key, tokens);
    if (err == ESP_OK && !hub_config_machine_selection_matches(&target)) {
        err = ESP_ERR_INVALID_STATE;
    }
    for (unsigned attempt = 0; err == ESP_OK && attempt < 2; ++attempt) {
        int timeout_ms = hub_command_deadline_timeout_ms(
            deadline, esp_timer_get_time() / 1000, CLOUD_READONLY_HTTP_TIMEOUT_MS,
            reserve_after_ms + HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS
        );
        if (timeout_ms <= 0) {
            err = ESP_ERR_TIMEOUT;
        } else {
            char url[160] = {0};
            snprintf(url, sizeof(url), "%s/things/%s/dashboard", CUSTOMER_APP_URL, target.serial_number);
            err = cloud_http_request_no_retry(
                url, HTTP_METHOD_GET, key, tokens->access_token, NULL,
                &response, &status_code, timeout_ms
            );
        }
        if (attempt || !cloud_http_retryable_error(err) ||
            !hub_command_deadline_can_start_step(deadline, esp_timer_get_time() / 1000 + 250,
                reserve_after_ms + 2 * HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS)) break;
        // Retry only the fresh GET; no business command has been sent yet.
        hub_http_response_reset(&response);
        vTaskDelay(pdMS_TO_TICKS(250));
        err = hub_config_machine_selection_matches(&target) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (status_code == 401 || status_code == 403) {
        invalidate_cached_tokens();
    }
    if (err == ESP_OK && status_code != 200) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(response.data);
        if (root == NULL || strcmp(json_optional_string(root, "serialNumber"), target.serial_number) != 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        cJSON_Delete(root);
    }
    if (err == ESP_OK) {
        cloud_snapshot_t empty = {0};
        char name[HUB_CONFIG_MAX_MACHINE_NAME_LEN + 1] = {0};
        err = parse_dashboard_snapshot(
            response.data, target.serial_number, target.generation,
            &empty, out, name, sizeof(name)
        );
    }
    if (err == ESP_OK && !hub_config_machine_selection_matches(&target)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) (void)publish_scale_snapshot(out);
    record_readonly_http_diag(
        READONLY_DIAG_DASHBOARD, err, status_code,
        (int)((esp_timer_get_time() - started_us) / 1000), response.len, 0
    );
    hub_http_response_reset(&response);
    free_cloud_context(settings, key, tokens);
    hub_cloud_command_attempt_t read_attempt = {.err = err, .http_status = status_code};
    schedule_command_auth_recovery(&read_attempt);
    return err;
}

static bool control_decimal_value_is_valid(double value, double maximum)
{
    return isfinite(value) && value >= 0.0 && value <= maximum
        && fabs(value * 10.0 - round(value * 10.0)) <= 1e-6;
}

esp_err_t hub_cloud_set_bbw_dose(const char *dose, int grams, hub_cloud_command_result_t *result)
{
    return hub_cloud_set_bbw_dose_with_deadline(
        dose,
        grams,
        NULL,
        0,
        result
    );
}

esp_err_t hub_cloud_set_bbw_dose_with_deadline(
    const char *dose,
    int grams,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    if (dose == NULL || (strcmp(dose, "dose1") != 0 && strcmp(dose, "dose2") != 0)) {
        set_command_result(result, false, started_us, "Invalid Brew-by-Weight dose key.");
        return ESP_ERR_INVALID_ARG;
    }
    if (grams < 0 || grams > 200) {
        set_command_result(result, false, started_us, "Brew-by-Weight dose must be between 0 and 200 grams.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    esp_err_t lock_err = cloud_command_io_lock(deadline, reserve_after_ms);
    if (lock_err != ESP_OK) {
        set_command_result(result, false, started_us, "Cloud command is busy or its deadline expired.");
        return lock_err;
    }
    cloud_snapshot_t current_snapshot = {0};
    esp_err_t read_err = read_control_snapshot_locked(deadline, reserve_after_ms, &current_snapshot);
    if (read_err != ESP_OK) {
        cloud_command_io_unlock();
        char message[128] = {0};
        snprintf(message, sizeof(message), "Cloud dose read failed (%s); no dose command sent.", esp_err_to_name(read_err));
        set_command_result(result, false, started_us, message);
        return read_err;
    }
    if (!current_snapshot.dose1_set || !current_snapshot.dose2_set
        || !control_decimal_value_is_valid(current_snapshot.dose1, 200.0)
        || !control_decimal_value_is_valid(current_snapshot.dose2, 200.0)) {
        cloud_command_io_unlock();
        set_command_result(result, false, started_us, "Cloud dose fields are missing or invalid; no dose command sent.");
        return ESP_ERR_INVALID_STATE;
    }

    double dose1 = current_snapshot.dose1;
    double dose2 = current_snapshot.dose2;
    if (strcmp(dose, "dose1") == 0) {
        dose1 = grams;
    } else {
        dose2 = grams;
    }

    char body[96] = {0};
    snprintf(body, sizeof(body), "{\"doses\":{\"Dose1\":%.1f,\"Dose2\":%.1f}}", dose1, dose2);
    hub_cloud_command_attempt_t attempt = execute_command_with_auth_locked(
        "CoffeeMachineBrewByWeightSettingDoses",
        body,
        deadline,
        reserve_after_ms
    );
    cloud_command_io_unlock();
    schedule_command_auth_recovery(&attempt);
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;

    if (err != ESP_OK) {
        char message[128] = {0};
        cloud_command_failure_message(message, sizeof(message), "Brew-by-Weight", err, &attempt);
        set_command_result(result, false, started_us, message);
        return err;
    }

    update_snapshot_bbw_dose(dose1, dose2);
    set_command_result(result, true, started_us, "Brew-by-Weight dose sent through ESP32-S3 Cloud; local state updated.");
    return ESP_OK;
}

static bool bbw_mode_is_valid(const char *mode)
{
    return strcmp(mode, "Continuous") == 0
        || strcmp(mode, "Dose1") == 0
        || strcmp(mode, "Dose2") == 0
        || strcmp(mode, "PulsesType") == 0;
}

static bool steam_level_is_valid(int level)
{
    return level >= 1 && level <= 3;
}

static const char *steam_level_cloud_value(int level)
{
    switch (level) {
        case 1:
            return "Level1";
        case 2:
            return "Level2";
        case 3:
            return "Level3";
        default:
            return "";
    }
}

static bool smart_standby_cloud_mode_is_valid(const char *mode)
{
    return mode != NULL && (strcmp(mode, "LastBrewing") == 0 || strcmp(mode, "PowerOn") == 0);
}

esp_err_t hub_cloud_set_bbw_mode(const char *mode, hub_cloud_command_result_t *result)
{
    return hub_cloud_set_bbw_mode_with_deadline(mode, NULL, 0, result);
}

esp_err_t hub_cloud_set_bbw_mode_with_deadline(
    const char *mode,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
)
{
    return hub_cloud_set_bbw_mode_checked_with_deadline(
        mode, -1, false, deadline, reserve_after_ms, result
    );
}


esp_err_t hub_cloud_set_bbw_mode_checked_with_deadline(
    const char *mode,
    int preset_grams,
    bool require_no_stored_match,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    if (mode == NULL || !bbw_mode_is_valid(mode)) {
        set_command_result(result, false, started_us, "Invalid Brew-by-Weight mode.");
        return ESP_ERR_INVALID_ARG;
    }
    const bool guarded = preset_grams >= 0;
    if (preset_grams < -1 || preset_grams > 200
        || (require_no_stored_match && (!guarded || strcmp(mode, "Dose1") != 0))
        || (guarded && strcmp(mode, "Dose1") != 0 && strcmp(mode, "Dose2") != 0)) {
        set_command_result(result, false, started_us, "Invalid Brew-by-Weight preset constraint.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }

    if (guarded) {
        esp_err_t lock_err = cloud_command_io_lock(deadline, reserve_after_ms);
        if (lock_err != ESP_OK) {
            set_command_result(result, false, started_us, "Cloud command is busy or its deadline expired.");
            return lock_err;
        }
        cloud_snapshot_t current = {0};
        esp_err_t read_err = read_control_snapshot_locked(deadline, reserve_after_ms, &current);
        if (read_err != ESP_OK) {
            cloud_command_io_unlock();
            char message[128] = {0};
            snprintf(message, sizeof(message), "Cloud preset read failed (%s); no command sent.", esp_err_to_name(read_err));
            set_command_result(result, false, started_us, message);
            return read_err;
        }
        if (hub_command_requires_online_machine() && !current.connected) {
            cloud_command_io_unlock();
            set_command_result(result, false, started_us, "Coffee machine is offline; no automatic weight command sent.");
            return ESP_ERR_INVALID_STATE;
        }
        const bool complete = current.dose1_set && current.dose2_set
            && control_decimal_value_is_valid(current.dose1, 200.0)
            && control_decimal_value_is_valid(current.dose2, 200.0);
        if (!complete) {
            cloud_command_io_unlock();
            char message[160] = {0};
            snprintf(message, sizeof(message),
                "Cloud doses invalid (Dose1=%s%.3f, Dose2=%s%.3f); no command sent.",
                current.dose1_set ? "" : "missing/", current.dose1,
                current.dose2_set ? "" : "missing/", current.dose2);
            set_command_result(result, false, started_us, message);
            return ESP_ERR_INVALID_STATE;
        }
        const bool matches = require_no_stored_match
            ? current.dose1 != preset_grams && current.dose2 != preset_grams
            : (strcmp(mode, "Dose1") == 0 ? current.dose1 : current.dose2) == preset_grams;
        if (!matches) {
            cloud_command_io_unlock();
            result->preset_changed = true;
            set_command_result(result, false, started_us, "Stored doses changed or are unavailable; no command sent.");
            return ESP_ERR_INVALID_STATE;
        }
    }

    char body[48] = {0};
    snprintf(body, sizeof(body), "{\"mode\":\"%s\"}", mode);
    hub_cloud_command_attempt_t attempt = guarded
        ? execute_command_with_auth_locked("CoffeeMachineBrewByWeightChangeMode", body, deadline, reserve_after_ms)
        : execute_setting_command_with_auth("CoffeeMachineBrewByWeightChangeMode", body, deadline, reserve_after_ms);
    if (guarded) {
        cloud_command_io_unlock();
        schedule_command_auth_recovery(&attempt);
    }
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Brew-by-Weight mode Cloud command failed.");
        return err;
    }

    update_snapshot_bbw_mode(mode);
    set_command_result(result, true, started_us, "Brew-by-Weight mode sent through ESP32-S3 Cloud; local state updated.");
    return ESP_OK;
}

esp_err_t hub_cloud_set_power(bool enabled, hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    char body[48] = {0};
    snprintf(body, sizeof(body), "{\"mode\":\"%s\"}", enabled ? "BrewingMode" : "StandBy");
    hub_cloud_command_attempt_t attempt = execute_high_risk_command_with_auth(
        "CoffeeMachineChangeMode",
        body
    );
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Power Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Power command sent through La Marzocco Cloud.");
    return ESP_OK;
}

esp_err_t hub_cloud_set_coffee_temperature(double temperature, hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (!isfinite(temperature)) {
        set_command_result(result, false, started_us, "Coffee temperature must be a finite value.");
        return ESP_ERR_INVALID_ARG;
    }
    if (temperature < 80.0 || temperature > 100.0) {
        set_command_result(result, false, started_us, "Coffee temperature must be between 80.0 and 100.0 C.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    char body[80] = {0};
    snprintf(body, sizeof(body), "{\"boilerIndex\":1,\"targetTemperature\":%.1f}", temperature);
    hub_cloud_command_attempt_t attempt = execute_setting_command_with_auth(
        "CoffeeMachineSettingCoffeeBoilerTargetTemperature",
        body,
        NULL,
        0
    );
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Coffee temperature Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Coffee temperature command sent through La Marzocco Cloud.");
    return ESP_OK;
}

esp_err_t hub_cloud_set_steam_level(int level, hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (!steam_level_is_valid(level)) {
        set_command_result(result, false, started_us, "Steam level must be 1, 2, or 3.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    char body[80] = {0};
    snprintf(body, sizeof(body), "{\"boilerIndex\":1,\"targetLevel\":\"%s\"}", steam_level_cloud_value(level));
    hub_cloud_command_attempt_t attempt = execute_setting_command_with_auth(
        "CoffeeMachineSettingSteamBoilerTargetLevel",
        body,
        NULL,
        0
    );
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Steam level Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Steam level command sent through La Marzocco Cloud.");
    return ESP_OK;
}

esp_err_t hub_cloud_set_steam_enabled(bool enabled, hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    char body[64] = {0};
    snprintf(body, sizeof(body), "{\"boilerIndex\":1,\"enabled\":%s}", enabled ? "true" : "false");
    hub_cloud_command_attempt_t attempt = execute_setting_command_with_auth(
        "CoffeeMachineSettingSteamBoilerEnabled",
        body,
        NULL,
        0
    );
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Steam enabled Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Steam enabled command sent through La Marzocco Cloud.");
    return ESP_OK;
}

esp_err_t hub_cloud_set_steam_temperature(double temperature, hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (!isfinite(temperature)) {
        set_command_result(result, false, started_us, "Steam temperature must be a finite value.");
        return ESP_ERR_INVALID_ARG;
    }
    if (temperature < 126.0 || temperature > 131.0) {
        set_command_result(result, false, started_us, "Steam temperature must be between 126.0 and 131.0 C.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    char body[80] = {0};
    snprintf(body, sizeof(body), "{\"boilerIndex\":1,\"targetTemperature\":%.1f}", temperature);
    hub_cloud_command_attempt_t attempt = execute_setting_command_with_auth(
        "CoffeeMachineSettingSteamBoilerTargetTemperature",
        body,
        NULL,
        0
    );
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Steam temperature Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Steam temperature command sent through La Marzocco Cloud.");
    return ESP_OK;
}

esp_err_t hub_cloud_set_smart_standby(bool enabled, int minutes, const char *mode, hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (minutes < 1 || minutes > 240) {
        set_command_result(result, false, started_us, "Smart standby minutes must be between 1 and 240.");
        return ESP_ERR_INVALID_ARG;
    }
    if (!smart_standby_cloud_mode_is_valid(mode)) {
        set_command_result(result, false, started_us, "Smart standby mode must be LastBrewing or PowerOn.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    char body[96] = {0};
    snprintf(body, sizeof(body), "{\"enabled\":%s,\"minutes\":%d,\"after\":\"%s\"}", enabled ? "true" : "false", minutes, mode);
    hub_cloud_command_attempt_t attempt = execute_high_risk_command_with_auth(
        "CoffeeMachineSettingSmartStandBy",
        body
    );
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Smart standby Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Smart standby command sent through La Marzocco Cloud.");
    return ESP_OK;
}

esp_err_t hub_cloud_start_backflush(hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }
    hub_cloud_command_attempt_t attempt = execute_high_risk_command_with_auth(
        "CoffeeMachineBackFlushStartCleaning",
        "{\"enabled\":true}"
    );
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;
    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Backflush Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Backflush command sent through La Marzocco Cloud.");
    return ESP_OK;
}

esp_err_t hub_cloud_update_machine_firmware(hub_cloud_command_result_t *result)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }

    hub_cloud_command_attempt_t attempt = execute_firmware_update_with_auth();
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;

    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Machine firmware update Cloud command failed.");
        return err;
    }
    schedule_refresh_after_command();
    set_command_result(result, true, started_us, "Machine firmware update command sent through La Marzocco Cloud.");
    return ESP_OK;
}

static bool prebrew_mode_is_valid(const char *value)
{
    return strcmp(value, "Disabled") == 0
        || strcmp(value, "PreBrewing") == 0
        || strcmp(value, "PreInfusion") == 0;
}

static bool parse_prebrew_time_value(const char *value, double *number)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (end == value || *end != '\0' || !control_decimal_value_is_valid(parsed, 10.0)) {
        return false;
    }
    *number = parsed;
    return true;
}

static void update_snapshot_prebrew(const char *field, const char *value, double on_time, double off_time)
{
    hub_machine_selection_identity_t target = {0};
    if (field == NULL || value == NULL || !active_command_target(&target)) {
        return;
    }
    portENTER_CRITICAL(&snapshot_lock);
    bool matches = snapshot.selection_generation == target.generation
        && strcmp(snapshot.serial_number, target.serial_number) == 0;
    if (matches && strcmp(field, "mode") == 0) {
        snprintf(snapshot.prebrew_mode, sizeof(snapshot.prebrew_mode), "%s", value);
    } else if (matches && (strcmp(field, "onTime") == 0 || strcmp(field, "offTime") == 0)) {
        snapshot.prebrew_on = on_time;
        snapshot.prebrew_on_set = true;
        snapshot.prebrew_off = off_time;
        snapshot.prebrew_off_set = true;
    }
    portEXIT_CRITICAL(&snapshot_lock);
}

esp_err_t hub_cloud_set_prebrew(const char *field, const char *value, hub_cloud_command_result_t *result)
{
    return hub_cloud_set_prebrew_with_deadline(
        field,
        value,
        NULL,
        0,
        result
    );
}

esp_err_t hub_cloud_set_prebrew_with_deadline(
    const char *field,
    const char *value,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
)
{
    int64_t started_us = esp_timer_get_time();
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    if (field == NULL || value == NULL || value[0] == '\0') {
        set_command_result(result, false, started_us, "Prebrew field and value are required.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ready_err = ensure_cloud_controls_ready_or_fail(result, started_us);
    if (ready_err != ESP_OK) {
        return ready_err;
    }

    char command[48] = {0};
    char body[128] = {0};
    bool command_io_locked = false;
    double on_time = 0.0;
    double off_time = 0.0;
    if (strcmp(field, "mode") == 0) {
        if (!prebrew_mode_is_valid(value)) {
            set_command_result(result, false, started_us, "Invalid prebrew mode.");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(command, sizeof(command), "CoffeeMachinePreBrewingChangeMode");
        snprintf(body, sizeof(body), "{\"mode\":\"%s\"}", value);
    } else if (strcmp(field, "onTime") == 0 || strcmp(field, "offTime") == 0) {
        double parsed = 0.0;
        if (!parse_prebrew_time_value(value, &parsed)) {
            set_command_result(result, false, started_us, "Prebrew time must be between 0 and 10 seconds.");
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t lock_err = cloud_command_io_lock(deadline, reserve_after_ms);
        if (lock_err != ESP_OK) {
            set_command_result(result, false, started_us, "Cloud command is busy or its deadline expired.");
            return lock_err;
        }
        command_io_locked = true;
        cloud_snapshot_t current_snapshot = {0};
        esp_err_t read_err = read_control_snapshot_locked(deadline, reserve_after_ms, &current_snapshot);
        if (read_err != ESP_OK) {
            cloud_command_io_unlock();
            char message[128] = {0};
            snprintf(message, sizeof(message), "Cloud prebrew read failed (%s); no time command sent.", esp_err_to_name(read_err));
            set_command_result(result, false, started_us, message);
            return read_err;
        }
        if (!current_snapshot.prebrew_on_set || !current_snapshot.prebrew_off_set
            || !control_decimal_value_is_valid(current_snapshot.prebrew_on, 10.0)
            || !control_decimal_value_is_valid(current_snapshot.prebrew_off, 10.0)) {
            cloud_command_io_unlock();
            set_command_result(result, false, started_us, "Cloud prebrew fields are missing or invalid; no time command sent.");
            return ESP_ERR_INVALID_STATE;
        }
        on_time = current_snapshot.prebrew_on;
        off_time = current_snapshot.prebrew_off;
        if (strcmp(field, "onTime") == 0) {
            on_time = parsed;
        } else {
            off_time = parsed;
        }
        snprintf(command, sizeof(command), "CoffeeMachinePreBrewingSettingTimes");
        snprintf(
            body,
            sizeof(body),
            "{\"times\":{\"In\":%.1f,\"Out\":%.1f},\"groupIndex\":1,\"doseIndex\":\"ByGroup\"}",
            on_time,
            off_time
        );
    } else {
        set_command_result(result, false, started_us, "Invalid prebrew field.");
        return ESP_ERR_INVALID_ARG;
    }

    hub_cloud_command_attempt_t attempt = command_io_locked
        ? execute_command_with_auth_locked(command, body, deadline, reserve_after_ms)
        : execute_setting_command_with_auth(command, body, deadline, reserve_after_ms);
    if (command_io_locked) {
        cloud_command_io_unlock();
        schedule_command_auth_recovery(&attempt);
    }
    set_command_attempt(result, &attempt);
    esp_err_t err = attempt.err;

    if (err != ESP_OK) {
        set_command_result(result, false, started_us, "Prebrew Cloud command failed.");
        return err;
    }

    update_snapshot_prebrew(field, value, on_time, off_time);
    set_command_result(result, true, started_us, "Prebrew setting sent through ESP32-S3 Cloud; local state updated.");
    return ESP_OK;
}

hub_cloud_status_t hub_cloud_status(void)
{
    if (status == HUB_CLOUD_STATUS_UNCONFIGURED && hub_config_has_cloud_credentials()) {
        return HUB_CLOUD_STATUS_CONFIGURED;
    }
    return status;
}

const char *hub_cloud_status_string(void)
{
    switch (hub_cloud_status()) {
        case HUB_CLOUD_STATUS_CONFIGURED:
            return "configured";
        case HUB_CLOUD_STATUS_NEEDS_LIVE_VALIDATION:
            return "needs_live_validation";
        case HUB_CLOUD_STATUS_AUTHENTICATED:
            return "authenticated";
        case HUB_CLOUD_STATUS_ERROR:
            return "error";
        case HUB_CLOUD_STATUS_UNCONFIGURED:
        default:
            return "unconfigured";
    }
}

bool hub_cloud_configured(void)
{
    return hub_config_has_cloud_credentials();
}

bool hub_cloud_authenticated(void)
{
    return hub_cloud_status() == HUB_CLOUD_STATUS_AUTHENTICATED;
}

bool hub_cloud_live_validation_required(void)
{
    return hub_cloud_status() == HUB_CLOUD_STATUS_CONFIGURED
        || hub_cloud_status() == HUB_CLOUD_STATUS_NEEDS_LIVE_VALIDATION;
}

bool hub_cloud_refresh_pending(void)
{
    return cloud_refresh_pending_snapshot();
}

bool hub_cloud_machine_selected(void)
{
    return machine_selected || hub_config_has_machine_selection();
}

bool hub_cloud_ble_token_cached(void)
{
    return ble_token_cached || hub_config_has_ble_auth_token();
}

bool hub_cloud_has_cached_snapshot(void)
{
    return cached_snapshot_matches_selection();
}

const char *hub_cloud_command_readiness(void)
{
    if (!hub_cloud_configured()) return "unconfigured";
    if (strcmp(hub_cloud_status_string(), "error") == 0) return "degraded";
    if (hub_cloud_authenticated() && hub_cloud_has_cached_snapshot()) return "ready";
    return "warming";
}

const char *hub_cloud_machine_name(void)
{
    if (cached_snapshot_matches_selection() && machine_name[0] != '\0') {
        return machine_name;
    }
    static char selected_name[HUB_CONFIG_MAX_MACHINE_NAME_LEN + 1];
    hub_machine_selection_t selection = {0};
    if (hub_config_get_machine_selection(&selection) == ESP_OK) {
        snprintf(
            selected_name,
            sizeof(selected_name),
            "%s",
            selection.machine_name[0] != '\0' ? selection.machine_name : "Selected machine"
        );
        return selected_name;
    }
    return machine_name[0] != '\0' ? machine_name : "";
}

const char *hub_cloud_machine_serial(void)
{
    static char selected_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    hub_machine_selection_identity_t identity = {0};
    if (hub_config_get_machine_selection_identity(&identity) == ESP_OK) {
        snprintf(selected_serial, sizeof(selected_serial), "%s", identity.serial_number);
        return selected_serial;
    }
    return machine_serial;
}

const char *hub_cloud_machine_state_json(void)
{
    static char body[2048];
    cloud_snapshot_t current = {0};
    bool has_cached_snapshot = false;
    copy_snapshot(&current, &has_cached_snapshot);
    if (!snapshot_matches_selection(&current, has_cached_snapshot)) {
        memset(&current, 0, sizeof(current));
    }
    char coffee_temperature[32] = "null";
    char steam_level[8] = "null";
    char coffee_ready_time[48] = "null";
    char steam_ready_time[48] = "null";
    char total_coffees[16] = "null";
    char dose1[32] = "null";
    char dose2[32] = "null";
    char bbw_mode[48] = "null";
    char scale_connected[8] = "null";
    char thing_scale_connected[8] = "null";
    char scale_battery_level[32] = "null";
    char scale_calibration_required[8] = "null";
    char scale_name[96] = "null";
    char prebrew_on[32] = "null";
    char prebrew_off[32] = "null";
    hub_machine_selection_identity_t selection = {0};
    hub_bbw_selection_t confirmed = {0};
    char confirmation[128] = "null";
    hub_config_get_machine_selection_identity(&selection);
    if (hub_config_get_bbw_selection(&selection, &confirmed) == ESP_OK) {
        snprintf(confirmation, sizeof(confirmation),
            "{\"mode\":\"%s\",\"confirmedAtUnixSeconds\":%.6f}", confirmed.mode, confirmed.confirmed_at);
        snprintf(current.bbw_mode, sizeof(current.bbw_mode), "%s", confirmed.mode);
    }

    if (current.coffee_temperature_set) {
        snprintf(coffee_temperature, sizeof(coffee_temperature), "%.1f", current.coffee_temperature);
    }
    if (current.steam_level_set) {
        snprintf(steam_level, sizeof(steam_level), "%d", current.steam_level);
    }
    if (current.coffee_boiler_ready_time[0] != '\0') {
        snprintf(coffee_ready_time, sizeof(coffee_ready_time), "\"%s\"", current.coffee_boiler_ready_time);
    }
    if (current.steam_boiler_ready_time[0] != '\0') {
        snprintf(steam_ready_time, sizeof(steam_ready_time), "\"%s\"", current.steam_boiler_ready_time);
    }
    if (current.total_coffee_set) {
        snprintf(total_coffees, sizeof(total_coffees), "%d", current.total_coffee);
    }
    if (current.dose1_set) {
        snprintf(dose1, sizeof(dose1), "%.0f", current.dose1);
    }
    if (current.dose2_set) {
        snprintf(dose2, sizeof(dose2), "%.0f", current.dose2);
    }
    if (current.bbw_mode[0] != '\0') {
        snprintf(bbw_mode, sizeof(bbw_mode), "\"%s\"", current.bbw_mode);
    }
    bool scale_fresh = scale_snapshot_fresh(&current, esp_timer_get_time() / 1000);
    bool scale_online = current.bbw_scale_connected_set ? current.bbw_scale_connected
        : current.thing_scale_connected_set && current.thing_scale_connected;
    if (scale_fresh && current.bbw_scale_connected_set) {
        snprintf(scale_connected, sizeof(scale_connected), "%s", current.bbw_scale_connected ? "true" : "false");
    }
    if (scale_fresh && current.thing_scale_connected_set) {
        snprintf(thing_scale_connected, sizeof(thing_scale_connected), "%s", current.thing_scale_connected ? "true" : "false");
    }
    if (scale_fresh && scale_online && current.scale_battery_level_set) {
        snprintf(scale_battery_level, sizeof(scale_battery_level), "%.0f", current.scale_battery_level);
    }
    if (scale_fresh && scale_online && current.scale_calibration_required_set) {
        snprintf(
            scale_calibration_required,
            sizeof(scale_calibration_required),
            "%s",
            current.scale_calibration_required ? "true" : "false"
        );
    }
    if (scale_fresh && current.scale_name[0] != '\0') {
        char escaped_scale_name[64] = {0};
        json_safe_copy(escaped_scale_name, sizeof(escaped_scale_name), current.scale_name);
        snprintf(scale_name, sizeof(scale_name), "\"%s\"", escaped_scale_name);
    }
    if (current.prebrew_on_set) {
        snprintf(prebrew_on, sizeof(prebrew_on), "%.1f", current.prebrew_on);
    }
    if (current.prebrew_off_set) {
        snprintf(prebrew_off, sizeof(prebrew_off), "%.1f", current.prebrew_off);
    }

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"machineName\":\"%s\","
        "\"connectionState\":\"%s\","
        "\"coffeeTemperature\":%s,"
        "\"steamLevel\":%s,"
        "\"coffeeBoilerReadyTime\":%s,"
        "\"steamBoilerReadyTime\":%s,"
        "\"totalCoffeesMade\":%s,"
        "\"brewByWeightDose1\":%s,"
        "\"brewByWeightDose2\":%s,"
        "\"brewByWeightMode\":%s,"
        "\"brewByWeightSelection\":{\"serialNumber\":\"%s\",\"generation\":%lu,\"confirmation\":%s},"
        "\"scaleConnected\":%s,"
        "\"thingScaleConnected\":%s,"
        "\"scaleBatteryLevel\":%s,"
        "\"scaleObservedMs\":%lld,"
        "\"scaleCalibrationRequired\":%s,"
        "\"scaleName\":%s,"
        "\"activeDose\":\"dose1\","
        "\"prebrew\":{"
        "\"mode\":\"%s\","
        "\"onTime\":%s,"
        "\"offTime\":%s,"
        "\"source\":\"cloud\""
        "},"
        "\"coffeeBoilerCurrentTemperature\":null,"
        "\"coffeeBoilerTargetTemperature\":%s,"
        "\"steamBoilerCurrentTemperature\":null,"
        "\"steamBoilerTargetTemperature\":null,"
        "\"steamBoilerEnabled\":null,"
        "\"tankStatus\":null,"
        "\"machineMode\":null,"
        "\"smartStandby\":null,"
        "\"lastUpdated\":\"%s\""
        "}",
        hub_cloud_machine_name(),
        current.connected ? "ready" : "unknown",
        coffee_temperature,
        steam_level,
        coffee_ready_time,
        steam_ready_time,
        total_coffees,
        dose1,
        dose2,
        bbw_mode,
        selection.serial_number, (unsigned long)selection.generation, confirmation,
        scale_connected,
        thing_scale_connected,
        scale_battery_level,
        (long long)current.scale_observed_ms,
        scale_calibration_required,
        scale_name,
        current.prebrew_mode[0] != '\0' ? current.prebrew_mode : "Disabled",
        prebrew_on,
        prebrew_off,
        coffee_temperature,
        current.last_updated[0] != '\0' ? current.last_updated : "1970-01-01T00:00:00Z"
    );
    return body;
}

const char *hub_cloud_last_error(void)
{
    return last_error;
}

int64_t hub_cloud_last_refresh_ms(void)
{
    return last_refresh_ms;
}

bool hub_cloud_installation_identity_sha256(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len < sizeof(installation_identity_sha256)) {
        return false;
    }
    portENTER_CRITICAL(&auth_diag_lock);
    bool available = installation_identity_sha256[0] != '\0';
    memcpy(buffer, installation_identity_sha256, sizeof(installation_identity_sha256));
    portEXIT_CRITICAL(&auth_diag_lock);
    return available;
}

uint32_t hub_cloud_auth_refresh_success_count(void)
{
    portENTER_CRITICAL(&auth_diag_lock);
    uint32_t value = auth_refresh_success_count;
    portEXIT_CRITICAL(&auth_diag_lock);
    return value;
}

uint32_t hub_cloud_auth_refresh_failure_count(void)
{
    portENTER_CRITICAL(&auth_diag_lock);
    uint32_t value = auth_refresh_failure_count;
    portEXIT_CRITICAL(&auth_diag_lock);
    return value;
}

int64_t hub_cloud_auth_refresh_last_success_uptime_ms(void)
{
    portENTER_CRITICAL(&auth_diag_lock);
    int64_t value = auth_refresh_last_success_uptime_ms;
    portEXIT_CRITICAL(&auth_diag_lock);
    return value;
}

const char *hub_cloud_readonly_diagnostics_json(void)
{
    static char body[1400];
    readonly_http_diag_t copies[READONLY_DIAG_COUNT] = {0};

    portENTER_CRITICAL(&readonly_http_diag_lock);
    memcpy(copies, readonly_http_diags, sizeof(copies));
    portEXIT_CRITICAL(&readonly_http_diag_lock);

    size_t offset = 0;
    offset += snprintf(body + offset, sizeof(body) - offset, "{");
    for (int index = 0; index < READONLY_DIAG_COUNT && offset < sizeof(body); index++) {
        readonly_diag_kind_t kind = (readonly_diag_kind_t)index;
        readonly_http_diag_t *diag = &copies[index];
        offset += snprintf(
            body + offset,
            sizeof(body) - offset,
            "%s\"%s\":{\"err\":\"%s\",\"status\":%d,\"durationMs\":%d,\"bytes\":%lu,\"firstBytes\":%lu,\"freeHeap\":%lu,\"internalLargest\":%lu,\"attempts\":%lu}",
            index == 0 ? "" : ",",
            readonly_diag_name(kind),
            esp_err_to_name(diag->err),
            diag->status_code,
            diag->duration_ms,
            (unsigned long)diag->response_bytes,
            (unsigned long)diag->first_bytes,
            (unsigned long)diag->free_heap,
            (unsigned long)diag->internal_largest,
            (unsigned long)diag->attempts
        );
    }
    snprintf(body + offset, sizeof(body) - offset, "}");
    return body;
}

esp_err_t hub_cloud_set_schedule(const hub_schedule_entry_t *expected,const hub_schedule_entry_t *requested,
                                 hub_cloud_command_result_t *result)
{
    int64_t started_us=esp_timer_get_time();
    if(result==NULL) return ESP_ERR_INVALID_ARG;
    memset(result,0,sizeof(*result));
    if(!hub_schedule_valid(expected)||!hub_schedule_valid(requested)||strcmp(expected->id,requested->id)||
        (requested->enabled&&(!requested->days||requested->on_minutes==requested->off_minutes))) {
        set_command_result(result,false,started_us,"Invalid existing schedule or repeat days."); return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err=ensure_cloud_controls_ready_or_fail(result,started_us);
    hub_machine_selection_identity_t target={0};
    if(err!=ESP_OK||!active_command_target(&target)) return err==ESP_OK?ESP_ERR_INVALID_STATE:err;
    char *json=heap_caps_calloc(1,32768,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!json) return ESP_ERR_NO_MEM;
    hub_schedule_entry_t current={0};
    // The coordinator serializes edits; read the vendor plan, never the UI/cache draft.
    err=hub_cloud_schedule_json(json,32768);
    if(err==ESP_OK&&(!hub_config_machine_selection_matches(&target)||
        !hub_schedule_find(json,target.serial_number,expected->id,&current)||!hub_schedule_equal(&current,expected))) {
        result->schedule_changed=true; err=ESP_ERR_INVALID_STATE;
    }
    if(err!=ESP_OK) {
        set_command_result(result,false,started_us,"Schedule changed or could not be checked; refresh before editing.");
        free(json); return err;
    }
    cJSON *body=hub_schedule_json(requested); char *encoded=body?cJSON_PrintUnformatted(body):NULL;
    cJSON_Delete(body);
    if(!encoded) { free(json); return ESP_ERR_NO_MEM; }
    hub_cloud_command_attempt_t attempt=execute_high_risk_command_with_auth("CoffeeMachineSetWakeUpSchedule",encoded);
    cJSON_free(encoded); set_command_attempt(result,&attempt); err=attempt.err;
    if(err==ESP_OK) {
        err=hub_cloud_schedule_json(json,32768);
        if(err==ESP_OK&&(!hub_config_machine_selection_matches(&target)||
            !hub_schedule_find(json,target.serial_number,requested->id,&current)||!hub_schedule_equal(&current,requested)))
            err=ESP_ERR_INVALID_RESPONSE;
    }
    set_command_result(result,err==ESP_OK,started_us,err==ESP_OK?
        "Schedule confirmed by fresh Cloud readback.":"Schedule result is unconfirmed; refresh before any further edit.");
    free(json); return err;
}
