#include "hub_state.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub_brew_timer.h"
#include "hub_ble.h"
#include "hub_cloud.h"
#include "hub_config.h"
#include "hub_state_keeper.h"
#include "hub_statistics_cache.h"
#include "hub_wifi.h"

#define RECENT_COMMAND_CAPACITY 6
#define SLOW_CLOUD_READ_BODY_BYTES 8192
static const char *TAG = "hub_state";
static const int64_t SLOW_CLOUD_READ_FRESH_CACHE_TTL_MS = 120000;
static const int64_t SLOW_CLOUD_READ_STATISTICS_FRESH_CACHE_TTL_MS = 600000;

typedef struct {
    bool used;
    char command[32];
    char transport[16];
    bool ok;
    int duration_ms;
    int64_t recorded_ms;
    char message[96];
} recent_command_t;

typedef enum {
    SLOW_CLOUD_READ_STATISTICS = 0,
    SLOW_CLOUD_READ_SCHEDULE,
    SLOW_CLOUD_READ_FIRMWARE,
} slow_cloud_read_kind_t;

typedef enum {
    SLOW_CLOUD_READ_REQUEST_REJECTED = 0,
    SLOW_CLOUD_READ_REQUEST_SCHEDULED,
    SLOW_CLOUD_READ_REQUEST_READY,
    SLOW_CLOUD_READ_REQUEST_INFLIGHT,
} slow_cloud_read_request_result_t;

typedef struct {
    slow_cloud_read_kind_t kind;
    int days;
    hub_statistics_cache_ticket_t statistics_ticket;
} slow_cloud_read_request_t;

typedef struct {
    bool running;
    bool valid;
    int days;
    int64_t started_ms;
    int64_t completed_ms;
    int duration_ms;
    esp_err_t last_error;
    uint32_t scheduled_count;
    uint32_t success_count;
    uint32_t start_free_heap;
    uint32_t start_internal_free;
    uint32_t start_internal_largest;
    uint32_t completed_free_heap;
    uint32_t completed_internal_free;
    uint32_t completed_internal_largest;
    hub_statistics_cache_ticket_t statistics_ticket;
    char *body;
} slow_cloud_read_cache_t;

static recent_command_t recent_commands[RECENT_COMMAND_CAPACITY] = {0};
static size_t recent_command_next = 0;
static size_t recent_command_count = 0;
static portMUX_TYPE recent_command_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t slow_cloud_read_mutex = NULL;
static portMUX_TYPE slow_cloud_read_mutex_lock = portMUX_INITIALIZER_UNLOCKED;
static slow_cloud_read_cache_t slow_cloud_read_caches[3] = {0};
static uint32_t schedule_epoch;

static void copy_json_safe(char *dest, size_t dest_len, const char *source)
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

static const char *slow_cloud_read_name(slow_cloud_read_kind_t kind)
{
    switch (kind) {
    case SLOW_CLOUD_READ_STATISTICS:
        return "statistics";
    case SLOW_CLOUD_READ_SCHEDULE:
        return "schedule";
    case SLOW_CLOUD_READ_FIRMWARE:
        return "firmware";
    }
    return "unknown";
}

static bool parse_slow_cloud_read_kind(const char *name, slow_cloud_read_kind_t *kind)
{
    if (name == NULL || kind == NULL) {
        return false;
    }
    if (strcmp(name, "statistics") == 0 || strcmp(name, "stats") == 0) {
        *kind = SLOW_CLOUD_READ_STATISTICS;
        return true;
    }
    if (strcmp(name, "schedule") == 0) {
        *kind = SLOW_CLOUD_READ_SCHEDULE;
        return true;
    }
    if (strcmp(name, "firmware") == 0) {
        *kind = SLOW_CLOUD_READ_FIRMWARE;
        return true;
    }
    return false;
}

static esp_err_t ensure_slow_cloud_read_mutex(void)
{
    if (slow_cloud_read_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&slow_cloud_read_mutex_lock);
    if (slow_cloud_read_mutex == NULL) {
        slow_cloud_read_mutex = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&slow_cloud_read_mutex_lock);

    if (created != NULL) {
        vSemaphoreDelete(created);
    }
    return ESP_OK;
}

static bool selected_statistics_serial(char *serial, size_t serial_len)
{
    if (serial == NULL || serial_len == 0) {
        return false;
    }
    serial[0] = '\0';

    hub_machine_selection_t selection = {0};
    if (hub_config_get_machine_selection(&selection) == ESP_OK
        && selection.serial_number[0] != '\0') {
        snprintf(serial, serial_len, "%s", selection.serial_number);
        return true;
    }

    hub_cloud_settings_t settings = {0};
    if (hub_config_get_cloud_settings(&settings) == ESP_OK
        && settings.cloud_serial_number[0] != '\0') {
        snprintf(serial, serial_len, "%s", settings.cloud_serial_number);
        return true;
    }

    const char *cloud_serial = hub_cloud_machine_serial();
    if (cloud_serial != NULL && cloud_serial[0] != '\0') {
        snprintf(serial, serial_len, "%s", cloud_serial);
        return true;
    }
    return false;
}

static slow_cloud_read_cache_t *slow_cloud_read_cache(slow_cloud_read_kind_t kind)
{
    if ((int)kind < 0 || kind > SLOW_CLOUD_READ_FIRMWARE) {
        return NULL;
    }
    return &slow_cloud_read_caches[(int)kind];
}

static bool slow_cloud_read_any_running_locked(void)
{
    for (int index = 0; index < 3; index++) {
        if (slow_cloud_read_caches[index].running) {
            return true;
        }
    }
    return false;
}

static bool slow_cloud_read_cache_matches_locked(
    const slow_cloud_read_cache_t *cache,
    slow_cloud_read_kind_t kind,
    int days
)
{
    return cache != NULL
        && (kind != SLOW_CLOUD_READ_STATISTICS || cache->days == days);
}

static bool slow_cloud_read_cache_fresh_locked(
    const slow_cloud_read_cache_t *cache,
    slow_cloud_read_kind_t kind,
    int days,
    int64_t now_ms
)
{
    return slow_cloud_read_cache_matches_locked(cache, kind, days)
        && cache->valid
        && cache->last_error == ESP_OK
        && cache->body != NULL
        && cache->completed_ms > 0
        && now_ms - cache->completed_ms < SLOW_CLOUD_READ_FRESH_CACHE_TTL_MS;
}

static bool slow_cloud_read_same_request_running_locked(
    const slow_cloud_read_cache_t *cache,
    slow_cloud_read_kind_t kind,
    int days
)
{
    return slow_cloud_read_cache_matches_locked(cache, kind, days)
        && cache->running;
}

static char *slow_cloud_read_alloc_body(void)
{
    char *body = heap_caps_calloc(1, SLOW_CLOUD_READ_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body != NULL) {
        return body;
    }
    return heap_caps_calloc(1, SLOW_CLOUD_READ_BODY_BYTES, MALLOC_CAP_8BIT);
}

static uint32_t internal_free_heap(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t internal_largest_free_block(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static bool take_pending_slow_cloud_read(slow_cloud_read_request_t *request)
{
    if (request == NULL || ensure_slow_cloud_read_mutex() != ESP_OK) {
        return false;
    }

    bool found = false;
    if (xSemaphoreTake(slow_cloud_read_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int index = 0; index < 3; index++) {
            slow_cloud_read_cache_t *cache = &slow_cloud_read_caches[index];
            if (!cache->running) {
                continue;
            }
            request->kind = (slow_cloud_read_kind_t)index;
            request->days = cache->days;
            request->statistics_ticket = cache->statistics_ticket;
            cache->started_ms = esp_timer_get_time() / 1000;
            cache->duration_ms = 0;
            cache->start_free_heap = (uint32_t)esp_get_free_heap_size();
            cache->start_internal_free = internal_free_heap();
            cache->start_internal_largest = internal_largest_free_block();
            found = true;
            break;
        }
        xSemaphoreGive(slow_cloud_read_mutex);
    }
    return found;
}

bool hub_state_process_pending_slow_cloud_read(void)
{
    slow_cloud_read_request_t request = {0};
    uint32_t read_epoch=0;
    if (!take_pending_slow_cloud_read(&request)) {
        return false;
    }

    if(request.kind==SLOW_CLOUD_READ_SCHEDULE&&xSemaphoreTake(slow_cloud_read_mutex,portMAX_DELAY)==pdTRUE) {
        read_epoch=schedule_epoch; xSemaphoreGive(slow_cloud_read_mutex);
    }
    int64_t started_ms = esp_timer_get_time() / 1000;
    char *body = NULL;
    esp_err_t err = ESP_FAIL;

    switch (request.kind) {
    case SLOW_CLOUD_READ_STATISTICS:
        err = hub_cloud_statistics_json_alloc_for_serial(
            request.statistics_ticket.serial,
            request.days,
            &body
        );
        break;
    case SLOW_CLOUD_READ_SCHEDULE:
        body = slow_cloud_read_alloc_body();
        err = body == NULL ? ESP_ERR_NO_MEM : hub_cloud_schedule_json(body, SLOW_CLOUD_READ_BODY_BYTES);
        break;
    case SLOW_CLOUD_READ_FIRMWARE:
        body = slow_cloud_read_alloc_body();
        err = body == NULL ? ESP_ERR_NO_MEM : hub_cloud_firmware_json(body, SLOW_CLOUD_READ_BODY_BYTES);
        break;
    }

    int64_t completed_ms = esp_timer_get_time() / 1000;
    if (request.kind == SLOW_CLOUD_READ_STATISTICS) {
        esp_err_t cache_err = hub_statistics_cache_complete(
            &request.statistics_ticket,
            err,
            body,
            completed_ms
        );
        if (cache_err == ESP_OK) {
            body = NULL;
        }
        err = cache_err;
    }
    if (ensure_slow_cloud_read_mutex() == ESP_OK
        && xSemaphoreTake(slow_cloud_read_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        slow_cloud_read_cache_t *cache = slow_cloud_read_cache(request.kind);
        bool superseded=request.kind==SLOW_CLOUD_READ_SCHEDULE&&read_epoch!=schedule_epoch;
        if(superseded) err=ESP_ERR_INVALID_STATE;
        if (cache != NULL) {
            cache->running = false;
            memset(&cache->statistics_ticket, 0, sizeof(cache->statistics_ticket));
            cache->completed_ms = completed_ms;
            cache->duration_ms = (int)(completed_ms - started_ms);
            cache->last_error = err;
            cache->completed_free_heap = (uint32_t)esp_get_free_heap_size();
            cache->completed_internal_free = internal_free_heap();
            cache->completed_internal_largest = internal_largest_free_block();
            if (request.kind == SLOW_CLOUD_READ_STATISTICS && err == ESP_OK) {
                cache->valid = true;
                cache->days = request.days;
                cache->success_count++;
            } else if (err == ESP_OK && body != NULL) {
                char *old_body = cache->body;
                cache->body = body;
                body = NULL;
                cache->valid = true;
                cache->days = request.days;
                cache->success_count++;
                free(old_body);
            }
            if(superseded) { cache->running=true; cache->started_ms=0; }
        }
        xSemaphoreGive(slow_cloud_read_mutex);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Slow Cloud %s refresh failed: %s", slow_cloud_read_name(request.kind), esp_err_to_name(err));
    }
    free(body);
    return true;
}

static slow_cloud_read_request_result_t request_slow_cloud_read(
    slow_cloud_read_kind_t kind,
    int days
)
{
    if (ensure_slow_cloud_read_mutex() != ESP_OK) {
        return SLOW_CLOUD_READ_REQUEST_REJECTED;
    }

    char statistics_serial[HUB_STATISTICS_CACHE_SERIAL_CAPACITY] = {0};
    if (kind == SLOW_CLOUD_READ_STATISTICS
        && !selected_statistics_serial(statistics_serial, sizeof(statistics_serial))) {
        return SLOW_CLOUD_READ_REQUEST_REJECTED;
    }

    bool should_start = false;
    slow_cloud_read_request_result_t result = SLOW_CLOUD_READ_REQUEST_REJECTED;
    int64_t now = esp_timer_get_time() / 1000;
    if (xSemaphoreTake(slow_cloud_read_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        slow_cloud_read_cache_t *cache = slow_cloud_read_cache(kind);
        if (kind == SLOW_CLOUD_READ_STATISTICS) {
            hub_statistics_refresh_state_t statistics_state =
                hub_statistics_cache_refresh_state(
                    statistics_serial,
                    days,
                    now,
                    SLOW_CLOUD_READ_STATISTICS_FRESH_CACHE_TTL_MS
                );
            if (statistics_state == HUB_STATISTICS_REFRESH_READY) {
                result = SLOW_CLOUD_READ_REQUEST_READY;
            } else if (statistics_state == HUB_STATISTICS_REFRESH_INFLIGHT) {
                result = SLOW_CLOUD_READ_REQUEST_INFLIGHT;
            } else if (statistics_state == HUB_STATISTICS_REFRESH_NEEDED
                && cache != NULL
                && !slow_cloud_read_any_running_locked()
                && !hub_statistics_cache_any_inflight()) {
                hub_statistics_cache_ticket_t ticket = {0};
                if (hub_statistics_cache_begin(
                        statistics_serial,
                        days,
                        now,
                        &ticket
                    ) == ESP_OK) {
                    /*
                     * The diagnostics validity bit describes the requested
                     * window. Preserve it only when this refresh is replacing
                     * a usable cache for the same window; a prior 27-day hit
                     * must never make a failed 26-day refresh look ready.
                     */
                    cache->valid = cache->valid && cache->days == days;
                    cache->running = true;
                    cache->last_error = ESP_OK;
                    cache->days = days;
                    cache->statistics_ticket = ticket;
                    cache->scheduled_count++;
                    cache->duration_ms = 0;
                    should_start = true;
                    result = SLOW_CLOUD_READ_REQUEST_SCHEDULED;
                }
            }
        } else {
            if (slow_cloud_read_cache_fresh_locked(cache, kind, days, now)) {
                result = SLOW_CLOUD_READ_REQUEST_READY;
            } else if (slow_cloud_read_same_request_running_locked(cache, kind, days)) {
                result = SLOW_CLOUD_READ_REQUEST_INFLIGHT;
            } else if (cache != NULL && !slow_cloud_read_any_running_locked()) {
                cache->running = true;
                cache->last_error = ESP_OK;
                cache->days = days;
                cache->scheduled_count++;
                cache->duration_ms = 0;
                should_start = true;
                result = SLOW_CLOUD_READ_REQUEST_SCHEDULED;
            }
        }
        xSemaphoreGive(slow_cloud_read_mutex);
    }

    if (should_start) {
        hub_state_keeper_touch();
    }
    return result;
}

static hub_state_slow_read_state_t read_single_slow_cloud_body(
    slow_cloud_read_kind_t kind,
    char *dest,
    size_t dest_len,
    bool *stale
)
{
    if (dest == NULL || dest_len == 0) {
        return HUB_STATE_SLOW_READ_UNAVAILABLE;
    }
    dest[0] = '\0';
    if (stale != NULL) {
        *stale = false;
    }
    if (slow_cloud_read_mutex == NULL) {
        return HUB_STATE_SLOW_READ_UNAVAILABLE;
    }

    hub_state_slow_read_state_t state = HUB_STATE_SLOW_READ_UNAVAILABLE;
    if (xSemaphoreTake(slow_cloud_read_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        slow_cloud_read_cache_t *cache = slow_cloud_read_cache(kind);
        if (cache != NULL
            && cache->valid
            && cache->body != NULL
            && strlen(cache->body) < dest_len) {
            memcpy(dest, cache->body, strlen(cache->body) + 1);
            state = HUB_STATE_SLOW_READ_HIT;
            if (stale != NULL) {
                *stale = cache->running || cache->last_error != ESP_OK;
            }
        } else if (cache != NULL && cache->running) {
            state = HUB_STATE_SLOW_READ_REFRESHING;
        }
        xSemaphoreGive(slow_cloud_read_mutex);
    }
    return state;
}

static void selected_machine_display_name(char *dest, size_t dest_len)
{
    const char *cloud_name = hub_cloud_machine_name();
    if (cloud_name != NULL && cloud_name[0] != '\0' && strcmp(cloud_name, "Selected machine") != 0) {
        copy_json_safe(dest, dest_len, cloud_name);
        return;
    }

    hub_machine_selection_t selection = {0};
    if (hub_config_get_machine_selection(&selection) == ESP_OK) {
        if (selection.machine_name[0] != '\0') {
            copy_json_safe(dest, dest_len, selection.machine_name);
            return;
        }
        if (selection.serial_number[0] != '\0') {
            copy_json_safe(dest, dest_len, selection.serial_number);
            return;
        }
    }

    copy_json_safe(dest, dest_len, "LamaBuddy ESP32-S3 Hub");
}

void hub_state_record_command(
    const char *command,
    const char *transport,
    bool ok,
    int duration_ms,
    const char *message
)
{
    recent_command_t next_entry = {0};
    next_entry.used = true;
    copy_json_safe(next_entry.command, sizeof(next_entry.command), command);
    copy_json_safe(next_entry.transport, sizeof(next_entry.transport), transport);
    next_entry.ok = ok;
    next_entry.duration_ms = duration_ms;
    next_entry.recorded_ms = esp_timer_get_time() / 1000;
    copy_json_safe(next_entry.message, sizeof(next_entry.message), message);

    portENTER_CRITICAL(&recent_command_lock);
    recent_commands[recent_command_next] = next_entry;
    recent_command_next = (recent_command_next + 1) % RECENT_COMMAND_CAPACITY;
    if (recent_command_count < RECENT_COMMAND_CAPACITY) {
        recent_command_count++;
    }
    portEXIT_CRITICAL(&recent_command_lock);
}

const char *hub_state_machine_state_json(void)
{
    if (hub_cloud_has_cached_snapshot()) {
        if (!hub_ble_has_cached_state()) {
            return hub_cloud_machine_state_json();
        }

        static char merged_body[2048];
        cJSON *cloud = cJSON_Parse(hub_cloud_machine_state_json());
        cJSON *ble = cJSON_Parse(hub_ble_state_json());
        if (cloud == NULL || ble == NULL) {
            cJSON_Delete(cloud);
            cJSON_Delete(ble);
            return hub_cloud_machine_state_json();
        }

        cJSON *machine_mode = cJSON_GetObjectItemCaseSensitive(ble, "machineMode");
        cJSON *tank_status = cJSON_GetObjectItemCaseSensitive(ble, "tankStatus");
        cJSON *smart_standby = cJSON_GetObjectItemCaseSensitive(ble, "smartStandby");
        if (cJSON_IsString(machine_mode)) {
            cJSON_ReplaceItemInObject(cloud, "machineMode", cJSON_CreateString(machine_mode->valuestring));
        }
        if (cJSON_IsBool(tank_status)) {
            cJSON_ReplaceItemInObject(cloud, "tankStatus", cJSON_CreateBool(cJSON_IsTrue(tank_status)));
        }
        if (cJSON_IsObject(smart_standby)) {
            cJSON_ReplaceItemInObject(cloud, "smartStandby", cJSON_Duplicate(smart_standby, true));
        }

        cJSON *boilers = cJSON_GetObjectItemCaseSensitive(ble, "boilers");
        if (cJSON_IsArray(boilers)) {
            cJSON *boiler = NULL;
            cJSON_ArrayForEach(boiler, boilers) {
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(boiler, "id");
                const cJSON *current = cJSON_GetObjectItemCaseSensitive(boiler, "currentTemperature");
                const cJSON *target = cJSON_GetObjectItemCaseSensitive(boiler, "targetTemperature");
                const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(boiler, "enabled");
                if (!cJSON_IsString(id)) {
                    continue;
                }
                if (strcmp(id->valuestring, "CoffeeBoiler1") == 0) {
                    if (cJSON_IsNumber(current)) {
                        cJSON_ReplaceItemInObject(cloud, "coffeeBoilerCurrentTemperature", cJSON_CreateNumber(current->valuedouble));
                    }
                    if (cJSON_IsNumber(target)) {
                        cJSON_ReplaceItemInObject(cloud, "coffeeBoilerTargetTemperature", cJSON_CreateNumber(target->valuedouble));
                    }
                } else if (strcmp(id->valuestring, "SteamBoiler") == 0) {
                    if (cJSON_IsNumber(current)) {
                        cJSON_ReplaceItemInObject(cloud, "steamBoilerCurrentTemperature", cJSON_CreateNumber(current->valuedouble));
                    }
                    if (cJSON_IsNumber(target)) {
                        cJSON_ReplaceItemInObject(cloud, "steamBoilerTargetTemperature", cJSON_CreateNumber(target->valuedouble));
                    }
                    if (cJSON_IsBool(enabled)) {
                        cJSON_ReplaceItemInObject(cloud, "steamBoilerEnabled", cJSON_CreateBool(cJSON_IsTrue(enabled)));
                    }
                }
            }
        }

        char *printed = cJSON_PrintUnformatted(cloud);
        if (printed == NULL) {
            cJSON_Delete(cloud);
            cJSON_Delete(ble);
            return hub_cloud_machine_state_json();
        }
        snprintf(merged_body, sizeof(merged_body), "%s", printed);
        cJSON_free(printed);
        cJSON_Delete(cloud);
        cJSON_Delete(ble);
        return merged_body;
    }

    static char body[1536];
    char machine_name[96] = {0};
    selected_machine_display_name(machine_name, sizeof(machine_name));
    const char *connection_state = hub_cloud_authenticated() ? "connected" : "unknown";
    const char *prebrew_source = hub_cloud_configured() ? "cloud" : "unconfigured";
    char coffee_temperature[32] = "null";
    char coffee_current[32] = "null";
    char coffee_target[32] = "null";
    char steam_current[32] = "null";
    char steam_target[32] = "null";
    char steam_enabled[8] = "null";
    char tank_status[8] = "null";
    char machine_mode[96] = "null";
    char smart_standby[256] = "null";

    if (hub_ble_has_cached_state()) {
        cJSON *ble = cJSON_Parse(hub_ble_state_json());
        if (ble != NULL) {
            const cJSON *connected = cJSON_GetObjectItemCaseSensitive(ble, "connected");
            if (cJSON_IsTrue(connected)) {
                connection_state = "ready";
            }

            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(ble, "machineMode");
            if (cJSON_IsString(mode)) {
                char escaped_mode[80] = {0};
                copy_json_safe(escaped_mode, sizeof(escaped_mode), mode->valuestring);
                snprintf(machine_mode, sizeof(machine_mode), "\"%s\"", escaped_mode);
            }

            const cJSON *tank = cJSON_GetObjectItemCaseSensitive(ble, "tankStatus");
            if (cJSON_IsBool(tank)) {
                snprintf(tank_status, sizeof(tank_status), "%s", cJSON_IsTrue(tank) ? "true" : "false");
            }

            const cJSON *standby = cJSON_GetObjectItemCaseSensitive(ble, "smartStandby");
            if (cJSON_IsObject(standby)) {
                char *printed = cJSON_PrintUnformatted(standby);
                if (printed != NULL) {
                    snprintf(smart_standby, sizeof(smart_standby), "%s", printed);
                    cJSON_free(printed);
                }
            }

            const cJSON *boilers = cJSON_GetObjectItemCaseSensitive(ble, "boilers");
            if (cJSON_IsArray(boilers)) {
                const cJSON *boiler = NULL;
                cJSON_ArrayForEach(boiler, boilers) {
                    const cJSON *id = cJSON_GetObjectItemCaseSensitive(boiler, "id");
                    const cJSON *current = cJSON_GetObjectItemCaseSensitive(boiler, "currentTemperature");
                    const cJSON *target = cJSON_GetObjectItemCaseSensitive(boiler, "targetTemperature");
                    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(boiler, "enabled");
                    if (!cJSON_IsString(id)) {
                        continue;
                    }

                    if (strcmp(id->valuestring, "CoffeeBoiler1") == 0) {
                        if (cJSON_IsNumber(current)) {
                            snprintf(coffee_current, sizeof(coffee_current), "%.1f", current->valuedouble);
                        }
                        if (cJSON_IsNumber(target)) {
                            snprintf(coffee_target, sizeof(coffee_target), "%.1f", target->valuedouble);
                            snprintf(coffee_temperature, sizeof(coffee_temperature), "%.1f", target->valuedouble);
                        }
                    } else if (strcmp(id->valuestring, "SteamBoiler") == 0) {
                        if (cJSON_IsNumber(current)) {
                            snprintf(steam_current, sizeof(steam_current), "%.1f", current->valuedouble);
                        }
                        if (cJSON_IsNumber(target)) {
                            snprintf(steam_target, sizeof(steam_target), "%.1f", target->valuedouble);
                        }
                        if (cJSON_IsBool(enabled)) {
                            snprintf(steam_enabled, sizeof(steam_enabled), "%s", cJSON_IsTrue(enabled) ? "true" : "false");
                        }
                    }
                }
            }

            cJSON_Delete(ble);
        }
    }

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"machineName\":\"%s\","
        "\"connectionState\":\"%s\","
        "\"coffeeTemperature\":%s,"
        "\"steamLevel\":null,"
        "\"coffeeBoilerReadyTime\":null,"
        "\"steamBoilerReadyTime\":null,"
        "\"totalCoffeesMade\":null,"
        "\"brewByWeightDose1\":null,"
        "\"brewByWeightDose2\":null,"
        "\"brewByWeightMode\":null,"
        "\"scaleConnected\":null,"
        "\"scaleObservedMs\":0,"
        "\"thingScaleConnected\":null,"
        "\"scaleBatteryLevel\":null,"
        "\"scaleCalibrationRequired\":null,"
        "\"scaleName\":null,"
        "\"activeDose\":\"dose1\","
        "\"prebrew\":{"
        "\"mode\":null,"
        "\"onTime\":null,"
        "\"offTime\":null,"
        "\"source\":\"%s\""
        "},"
        "\"coffeeBoilerCurrentTemperature\":%s,"
        "\"coffeeBoilerTargetTemperature\":%s,"
        "\"steamBoilerCurrentTemperature\":%s,"
        "\"steamBoilerTargetTemperature\":%s,"
        "\"steamBoilerEnabled\":%s,"
        "\"tankStatus\":%s,"
        "\"machineMode\":%s,"
        "\"smartStandby\":%s,"
        "\"lastUpdated\":\"1970-01-01T00:00:00Z\""
        "}",
        machine_name,
        connection_state,
        coffee_temperature,
        prebrew_source,
        coffee_current,
        coffee_target,
        steam_current,
        steam_target,
        steam_enabled,
        tank_status,
        machine_mode,
        smart_standby
    );
    return body;
}

const char *hub_state_capabilities_json(void)
{
    static EXT_RAM_BSS_ATTR char body[12000];
    bool cloud_controls_available = hub_cloud_controls_available();
    bool cloud_readonly_available = hub_cloud_readonly_available();
    bool cloud_configured = hub_cloud_configured();
    bool ble_writes_available = hub_ble_configured();
    bool ble_reads_available = hub_ble_has_cached_state() || hub_ble_reads_validated();
    bool ble_first_controls_available = ble_writes_available || cloud_controls_available;
    const char *cloud_read_available = cloud_readonly_available ? "true" : "false";
    const char *cloud_configured_available = cloud_configured ? "true" : "false";
    const char *ble_first_available = ble_first_controls_available ? "true" : "false";
    const char *ble_first_transport = ble_writes_available ? "ble" : (cloud_controls_available ? "cloud" : "none");
    const char *ble_first_latency = ble_writes_available
        ? (cloud_controls_available ? "ble-local-with-cloud-fallback" : "ble-local")
        : (cloud_controls_available ? "cloud-network" : "unavailable");
    const char *cloud_read_latency = cloud_readonly_available ? "network" : "unavailable";
    const char *cloud_read_status = cloud_readonly_available ? "validated-cloud-readonly" : "unavailable";
    const char *ble_first_status = ble_writes_available
        ? (cloud_controls_available ? "ble-route-open-cloud-fallback-available" : "ble-route-open")
        : (
            cloud_controls_available
                ? "cloud-fallback-available"
                : "unavailable"
        );
    const char *high_risk_transport = ble_first_transport;
    const char *high_risk_preferred_transport = "ble";
    const char *high_risk_available = ble_first_available;
    const char *high_risk_latency = ble_first_latency;
    const char *high_risk_status = ble_first_status;
    const char *cloud_control_transport = cloud_controls_available ? "cloud" : "none";
    const char *cloud_control_available = cloud_controls_available ? "true" : "false";
    const char *cloud_control_latency = cloud_controls_available ? "cloud-network" : "unavailable";
    const char *cloud_control_status = cloud_controls_available ? "cloud-primary-validated" : "unavailable";
    const char *ble_read_status = ble_reads_available ? "ble-read-cache-or-validation-present" : "ble-read-unavailable";
    const bool brew_timer_available = hub_brew_timer_available();
    const char *brew_timer_available_text = brew_timer_available ? "true" : "false";
    const char *brew_timer_latency = brew_timer_available ? "local-live" : "unavailable";
    const char *brew_timer_status = brew_timer_available
        ? "confirmed-extraction-live"
        : "awaiting-confirmed-extraction-source";
    const char *brew_timer_notes = brew_timer_available
        ? "Live brew timer is driven by a confirmed extraction event source."
        : "BLE machineMode is treated only as machine-state freshness because real Mini R boot and heating samples can report BrewingMode while not extracting.";
    snprintf(
        body,
        sizeof(body),
        "{"
        "\"profile\":\"esp32s3-full-feature-test\","
        "\"fullFeatureTestMode\":true,"
        "\"supportsSmartPresetDecisionValidation\":true,"
        "\"scheduleEdit\":{\"transport\":\"cloud\",\"available\":%s,\"risk\":\"high\",\"requiresConfirmation\":true,\"validationStatus\":\"local-tested-only\"},"
        "\"health\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":null,\"available\":true,\"latencyClass\":\"local-http\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"local-endpoint-available\",\"notes\":\"GET /health returns Hub, Wi-Fi, Cloud and BLE summary.\"},"
        "\"transports\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":null,\"available\":true,\"latencyClass\":\"local-http\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"local-endpoint-available\",\"notes\":\"GET /diagnostics/transports returns transport status.\"},"
        "\"capabilities\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":null,\"available\":true,\"latencyClass\":\"local-http\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"local-endpoint-available\",\"notes\":\"GET /machine/capabilities returns this full-feature test matrix.\"},"
        "\"machineState\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":\"cloud-cache\",\"available\":true,\"latencyClass\":\"local-http\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"local-cache-endpoint-available\",\"notes\":\"GET /machine/state returns fused Cloud snapshot and BLE cache when present.\"},"
        "\"bleState\":{\"transport\":\"ble-cache\",\"preferredTransport\":\"ble-cache\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"local-cache\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"GET /machine/ble-state returns cached BLE state; run validate-read to refresh actively.\"},"
        "\"bleValidateRead\":{\"transport\":\"ble\",\"preferredTransport\":\"ble\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"ble-local\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"POST /diagnostics/ble/validate-read performs an active BLE read validation.\"},"
        "\"cloudRefresh\":{\"transport\":\"cloud\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"network\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"POST /diagnostics/cloud/refresh refreshes Cloud auth and machine snapshot when configured.\"},"
        "\"statistics\":{\"transport\":\"cloud\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"GET /machine/statistics?days=1...30 reads Cloud total cups, flushes, recent coffee and trend.\"},"
        "\"recentCoffee\":{\"transport\":\"cloud\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Recent coffee data is exposed through Cloud statistics on ESP32-S3.\"},"
        "\"schedule\":{\"transport\":\"cloud\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"GET /machine/schedule reads Cloud schedule coverage.\"},"
        "\"machineFirmware\":{\"transport\":\"cloud\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"GET /diagnostics/firmware reads machine firmware status from Cloud.\"},"
        "\"recentCommands\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":null,\"available\":true,\"latencyClass\":\"local-http\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"local-endpoint-available\",\"notes\":\"GET /diagnostics/recent-commands returns recent Hub command results.\"},"
        "\"ota\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":null,\"available\":true,\"latencyClass\":\"local-http\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"local-ota-endpoint-available\",\"notes\":\"GET /diagnostics/ota returns ESP local OTA status.\"},"
        "\"brewByWeightDose\":{\"transport\":\"%s\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"medium\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Brew-by-Weight uses Cloud primary because Mini R BLE command returns error executing; Cloud hot path avoids wasted BLE failure attempts.\"},"
        "\"brewByWeightDose1\":{\"transport\":\"%s\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"medium\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Brew-by-Weight uses Cloud primary because Mini R BLE command returns error executing; dose 1 writes preserve dose 2.\"},"
        "\"brewByWeightDose2\":{\"transport\":\"%s\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"medium\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Brew-by-Weight uses Cloud primary because Mini R BLE command returns error executing; dose 2 writes preserve dose 1.\"},"
        "\"brewByWeightMode\":{\"transport\":\"%s\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"medium\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Brew-by-Weight uses Cloud primary because Mini R BLE command returns error executing; unsupported machine modes remain rejected by Cloud.\"},"
        "\"coffeeTemperature\":{\"transport\":\"%s\",\"preferredTransport\":\"ble\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Cached BLE hot path first; Cloud fallback without foreground scanning.\"},"
        "\"steamLevel\":{\"transport\":\"%s\",\"preferredTransport\":\"ble\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Cached BLE hot path first; Cloud fallback without foreground scanning; BLE maps steam level to target temperature 126/128/131.\"},"
        "\"standby\":{\"transport\":\"%s\",\"preferredTransport\":\"%s\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"high\",\"requiresConfirmation\":true,\"validationStatus\":\"%s\",\"notes\":\"Confirmed standby route is BLE-first with Cloud fallback. BLE attempt is recorded and Cloud fallback remains available.\"},"
        "\"prebrew\":{\"transport\":\"%s\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"medium\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Prebrew uses Cloud primary because Mini R BLE command returns error executing; mode and time fields are still reported separately.\"},"
        "\"power\":{\"transport\":\"%s\",\"preferredTransport\":\"%s\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"high\",\"requiresConfirmation\":true,\"validationStatus\":\"%s\",\"notes\":\"Confirmed power route is BLE-first with Cloud fallback. BLE attempt is recorded and Cloud fallback remains available.\"},"
        "\"brewTimer\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"low\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"%s\"},"
        "\"steamEnabled\":{\"transport\":\"%s\",\"preferredTransport\":\"ble\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"medium\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Cached BLE hot path first; Cloud fallback without foreground scanning.\"},"
        "\"steamTemperature\":{\"transport\":\"%s\",\"preferredTransport\":\"ble\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"medium\",\"requiresConfirmation\":false,\"validationStatus\":\"%s\",\"notes\":\"Cached BLE hot path first; Cloud fallback without foreground scanning.\"},"
        "\"smartStandby\":{\"transport\":\"%s\",\"preferredTransport\":\"%s\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"high\",\"requiresConfirmation\":true,\"validationStatus\":\"%s\",\"notes\":\"Confirmed smart standby route is BLE-first with Cloud fallback. BLE attempt is recorded and Cloud fallback remains available.\"},"
        "\"backflush\":{\"transport\":\"%s\",\"preferredTransport\":\"cloud\",\"fallbackTransport\":null,\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"high\",\"requiresConfirmation\":true,\"validationStatus\":\"%s\",\"notes\":\"Backflush uses Cloud primary because Mini R BLE command returns error executing; confirm=true is still required before execution.\"},"
        "\"machineFirmwareUpdate\":{\"transport\":\"%s\",\"preferredTransport\":\"%s\",\"fallbackTransport\":\"cloud\",\"available\":%s,\"latencyClass\":\"%s\",\"risk\":\"high\",\"requiresConfirmation\":true,\"validationStatus\":\"%s\",\"notes\":\"Machine firmware update route is BLE-first with Cloud fallback. BLE attempt is recorded and Cloud fallback remains available.\"},"
        "\"espLocalOta\":{\"transport\":\"local\",\"preferredTransport\":\"local\",\"fallbackTransport\":null,\"available\":true,\"latencyClass\":\"local-network\",\"risk\":\"medium\",\"requiresConfirmation\":true,\"validationStatus\":\"local-ota-endpoint-available\",\"notes\":\"ESP local OTA is advertised for full-feature test mode.\"}"
        "}",
        cloud_control_available,
        ble_reads_available ? "true" : "false",
        ble_read_status,
        hub_ble_configured() ? "true" : "false",
        ble_read_status,
        cloud_configured_available,
        cloud_configured ? "cloud-configured" : "cloud-unconfigured",
        cloud_read_available,
        cloud_read_latency,
        cloud_read_status,
        cloud_read_available,
        cloud_read_latency,
        cloud_read_status,
        cloud_read_available,
        cloud_read_latency,
        cloud_read_status,
        cloud_read_available,
        cloud_read_latency,
        cloud_read_status,
        cloud_control_transport,
        cloud_control_available,
        cloud_control_latency,
        cloud_control_status,
        cloud_control_transport,
        cloud_control_available,
        cloud_control_latency,
        cloud_control_status,
        cloud_control_transport,
        cloud_control_available,
        cloud_control_latency,
        cloud_control_status,
        cloud_control_transport,
        cloud_control_available,
        cloud_control_latency,
        cloud_control_status,
        ble_first_transport,
        ble_first_available,
        ble_first_latency,
        ble_first_status,
        ble_first_transport,
        ble_first_available,
        ble_first_latency,
        ble_first_status,
        high_risk_transport,
        high_risk_preferred_transport,
        high_risk_available,
        high_risk_latency,
        high_risk_status,
        cloud_control_transport,
        cloud_control_available,
        cloud_control_latency,
        cloud_control_status,
        high_risk_transport,
        high_risk_preferred_transport,
        high_risk_available,
        high_risk_latency,
        high_risk_status,
        brew_timer_available_text,
        brew_timer_latency,
        brew_timer_status,
        brew_timer_notes,
        ble_first_transport,
        ble_first_available,
        ble_first_latency,
        ble_first_status,
        ble_first_transport,
        ble_first_available,
        ble_first_latency,
        ble_first_status,
        high_risk_transport,
        high_risk_preferred_transport,
        high_risk_available,
        high_risk_latency,
        high_risk_status,
        cloud_control_transport,
        cloud_control_available,
        cloud_control_latency,
        cloud_control_status,
        high_risk_transport,
        high_risk_preferred_transport,
        high_risk_available,
        high_risk_latency,
        high_risk_status
    );
    return body;
}

const char *hub_state_transports_json(void)
{
    static char body[768];
    bool cloud_configured = hub_cloud_configured();
    bool machine_selected = hub_cloud_machine_selected();
    bool ble_auth_token_cached = hub_cloud_ble_token_cached();
    bool ble_device_configured = hub_config_has_ble_device();
    bool ble_configured = hub_ble_configured();
    bool ble_reads_validated = hub_ble_reads_validated();
    bool ble_writes_validated = hub_ble_writes_validated();
    const char *ble_status = "unconfigured";
    if (ble_reads_validated) {
        ble_status = hub_ble_refresh_pending() ? "refreshing" : "cached";
    } else if (ble_configured) {
        ble_status = hub_ble_refresh_pending() ? "validating" : "idle";
    }
    snprintf(
        body,
        sizeof(body),
        "{"
        "\"hub\":{\"status\":\"online\",\"source\":\"esp32-s3\"},"
        "\"wifi\":{\"status\":\"%s\",\"configured\":%s,\"ip\":\"%s\"},"
        "\"cloud\":{\"status\":\"%s\",\"configured\":%s,\"authenticated\":%s,\"machineSelected\":%s,\"bleAuthTokenCached\":%s,\"liveValidationRequired\":%s},"
        "\"ble\":{\"status\":\"%s\",\"configured\":%s,\"tokenConfigured\":%s,\"deviceConfigured\":%s,\"readsValidated\":%s,\"writesValidated\":%s}"
        "}",
        hub_wifi_mode(),
        hub_config_has_wifi_credentials() ? "true" : "false",
        hub_wifi_ip_address(),
        hub_cloud_status_string(),
        cloud_configured ? "true" : "false",
        hub_cloud_authenticated() ? "true" : "false",
        machine_selected ? "true" : "false",
        ble_auth_token_cached ? "true" : "false",
        hub_cloud_live_validation_required() ? "true" : "false",
        ble_status,
        ble_configured ? "true" : "false",
        ble_auth_token_cached ? "true" : "false",
        ble_device_configured ? "true" : "false",
        ble_reads_validated ? "true" : "false",
        ble_writes_validated ? "true" : "false"
    );
    return body;
}

const char *hub_state_cloud_diagnostics_json(void)
{
    static char body[1024];
    char installation_identity_sha256[65] = {0};
    bool has_installation_identity = hub_cloud_installation_identity_sha256(
        installation_identity_sha256,
        sizeof(installation_identity_sha256)
    );
    snprintf(
        body,
        sizeof(body),
        "{"
        "\"configured\":%s,"
        "\"status\":\"%s\","
        "\"authenticated\":%s,"
        "\"liveValidationRequired\":%s,"
        "\"machineSelected\":%s,"
        "\"machineName\":\"%s\","
        "\"machineSerial\":\"%s\","
        "\"bleAuthTokenCached\":%s,"
        "\"hasCachedSnapshot\":%s,"
        "\"refreshPending\":%s,"
        "\"installationIdentitySha256\":%s%s%s,"
        "\"authRefreshSuccessCount\":%lu,"
        "\"authRefreshFailureCount\":%lu,"
        "\"authRefreshLastSuccessUptimeMs\":%lld,"
        "\"lastRefreshMs\":%lld,"
        "\"lastError\":\"%s\""
        "}",
        hub_cloud_configured() ? "true" : "false",
        hub_cloud_status_string(),
        hub_cloud_authenticated() ? "true" : "false",
        hub_cloud_live_validation_required() ? "true" : "false",
        hub_cloud_machine_selected() ? "true" : "false",
        hub_cloud_machine_name(),
        hub_cloud_machine_serial(),
        hub_cloud_ble_token_cached() ? "true" : "false",
        hub_cloud_has_cached_snapshot() ? "true" : "false",
        hub_cloud_refresh_pending() ? "true" : "false",
        has_installation_identity ? "\"" : "",
        has_installation_identity ? installation_identity_sha256 : "null",
        has_installation_identity ? "\"" : "",
        (unsigned long)hub_cloud_auth_refresh_success_count(),
        (unsigned long)hub_cloud_auth_refresh_failure_count(),
        (long long)hub_cloud_auth_refresh_last_success_uptime_ms(),
        (long long)hub_cloud_last_refresh_ms(),
        hub_cloud_last_error()
    );
    return body;
}

const char *hub_state_cloud_machines_json(void)
{
    static char body[640];
    if (!hub_cloud_machine_selected()) {
        return "{\"machines\":[],\"source\":\"cloud\",\"liveValidationRequired\":true}";
    }

    hub_machine_selection_t selection = {0};
    bool has_selection = hub_config_get_machine_selection(&selection) == ESP_OK
        && selection.serial_number[0] != '\0';
    const char *serial = hub_cloud_machine_serial()[0] != '\0'
        ? hub_cloud_machine_serial()
        : selection.serial_number;
    const char *display_name = hub_cloud_machine_name()[0] != '\0'
        ? hub_cloud_machine_name()
        : selection.machine_name;
    if (display_name[0] == '\0') {
        display_name = "Selected machine";
    }

    char escaped_name[96] = {0};
    char escaped_serial[48] = {0};
    char escaped_ble_name[80] = {0};
    char escaped_ble_address[48] = {0};
    copy_json_safe(escaped_name, sizeof(escaped_name), display_name);
    copy_json_safe(escaped_serial, sizeof(escaped_serial), serial);
    copy_json_safe(escaped_ble_name, sizeof(escaped_ble_name), has_selection ? selection.ble_device_name : "");
    copy_json_safe(escaped_ble_address, sizeof(escaped_ble_address), has_selection ? selection.ble_device_address : "");

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"machines\":[{"
        "\"displayName\":\"%s\","
        "\"serialNumber\":\"%s\","
        "\"selected\":true,"
        "\"bleAuthTokenCached\":%s,"
        "\"bleDeviceName\":%s%s%s,"
        "\"bleDeviceAddress\":%s%s%s"
        "}],"
        "\"source\":\"cloud\","
        "\"liveValidationRequired\":%s"
        "}",
        escaped_name,
        escaped_serial,
        hub_cloud_ble_token_cached() ? "true" : "false",
        escaped_ble_name[0] != '\0' ? "\"" : "null",
        escaped_ble_name[0] != '\0' ? escaped_ble_name : "",
        escaped_ble_name[0] != '\0' ? "\"" : "",
        escaped_ble_address[0] != '\0' ? "\"" : "null",
        escaped_ble_address[0] != '\0' ? escaped_ble_address : "",
        escaped_ble_address[0] != '\0' ? "\"" : "",
        hub_cloud_live_validation_required() ? "true" : "false"
    );
    return body;
}

const char *hub_state_recent_commands_json(void)
{
    static char body[1536];
    recent_command_t entries[RECENT_COMMAND_CAPACITY] = {0};
    size_t entry_count = 0;
    portENTER_CRITICAL(&recent_command_lock);
    entry_count = recent_command_count;
    for (size_t i = 0; i < entry_count; i++) {
        size_t index = (recent_command_next + RECENT_COMMAND_CAPACITY - 1 - i)
            % RECENT_COMMAND_CAPACITY;
        entries[i] = recent_commands[index];
    }
    portEXIT_CRITICAL(&recent_command_lock);

    size_t offset = 0;
    const char *prefix = "{\"commands\":[";
    size_t prefix_len = strlen(prefix);
    memcpy(body, prefix, prefix_len);
    offset = prefix_len;
    bool first = true;
    for (size_t i = 0; i < entry_count; i++) {
        recent_command_t *entry = &entries[i];
        if (!entry->used) {
            continue;
        }
        char entry_json[320] = {0};
        int entry_len = snprintf(
            entry_json,
            sizeof(entry_json),
            "%s{\"command\":\"%s\",\"transport\":\"%s\",\"ok\":%s,\"durationMs\":%d,\"recordedMs\":%lld,\"message\":\"%s\"}",
            first ? "" : ",",
            entry->command,
            entry->transport,
            entry->ok ? "true" : "false",
            entry->duration_ms,
            (long long)entry->recorded_ms,
            entry->message
        );
        if (entry_len < 0 || (size_t)entry_len >= sizeof(entry_json)) {
            continue;
        }
        if (offset + (size_t)entry_len + sizeof("]}") > sizeof(body)) {
            break;
        }
        memcpy(body + offset, entry_json, (size_t)entry_len);
        offset += (size_t)entry_len;
        first = false;
    }
    memcpy(body + offset, "]}", sizeof("]}"));
    return body;
}

hub_state_slow_read_result_t hub_state_statistics_read(int days)
{
    static EXT_RAM_BSS_ATTR char body[8192];
    int normalized_days = days < 1 ? 7 : (days > 30 ? 30 : days);
    char serial[HUB_STATISTICS_CACHE_SERIAL_CAPACITY] = {0};
    if (!selected_statistics_serial(serial, sizeof(serial))) {
        snprintf(body, sizeof(body), "{\"status\":\"unavailable\",\"days\":%d}", normalized_days);
        return (hub_state_slow_read_result_t) {
            .body = body,
            .state = HUB_STATE_SLOW_READ_UNAVAILABLE,
            .stale = false,
        };
    }

    hub_statistics_cache_metadata_t metadata = {0};
    hub_statistics_read_state_t cache_state = hub_statistics_cache_copy(
        serial,
        normalized_days,
        body,
        sizeof(body),
        &metadata
    );
    if (cache_state == HUB_STATISTICS_HIT) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool age_stale = metadata.completed_ms <= 0
            || now_ms < metadata.completed_ms
            || now_ms - metadata.completed_ms >= SLOW_CLOUD_READ_STATISTICS_FRESH_CACHE_TTL_MS;
        return (hub_state_slow_read_result_t) {
            .body = body,
            .state = HUB_STATE_SLOW_READ_HIT,
            .stale = metadata.stale || age_stale,
        };
    }

    const char *status = "unavailable";
    hub_state_slow_read_state_t state = HUB_STATE_SLOW_READ_UNAVAILABLE;
    if (cache_state == HUB_STATISTICS_REFRESHING) {
        status = "refreshing";
        state = HUB_STATE_SLOW_READ_REFRESHING;
    } else if (cache_state != HUB_STATISTICS_UNAVAILABLE) {
        status = "unavailable";
    }
    snprintf(body, sizeof(body), "{\"status\":\"%s\",\"days\":%d}", status, normalized_days);
    return (hub_state_slow_read_result_t) {
        .body = body,
        .state = state,
        .stale = false,
    };
}

hub_state_slow_read_result_t hub_state_schedule_read(void)
{
    static EXT_RAM_BSS_ATTR char body[6144];
    bool stale = false;
    hub_state_slow_read_state_t state = read_single_slow_cloud_body(
        SLOW_CLOUD_READ_SCHEDULE,
        body,
        sizeof(body),
        &stale
    );
    if (state != HUB_STATE_SLOW_READ_HIT) {
        snprintf(
            body,
            sizeof(body),
            "{\"status\":\"%s\"}",
            state == HUB_STATE_SLOW_READ_REFRESHING ? "refreshing" : "unavailable"
        );
    }
    return (hub_state_slow_read_result_t) {
        .body = body,
        .state = state,
        .stale = stale,
    };
}

hub_state_slow_read_result_t hub_state_firmware_read(void)
{
    static EXT_RAM_BSS_ATTR char body[4096];
    bool stale = false;
    hub_state_slow_read_state_t state = read_single_slow_cloud_body(
        SLOW_CLOUD_READ_FIRMWARE,
        body,
        sizeof(body),
        &stale
    );
    if (state != HUB_STATE_SLOW_READ_HIT) {
        snprintf(
            body,
            sizeof(body),
            "{\"status\":\"%s\"}",
            state == HUB_STATE_SLOW_READ_REFRESHING ? "refreshing" : "unavailable"
        );
    }
    return (hub_state_slow_read_result_t) {
        .body = body,
        .state = state,
        .stale = stale,
    };
}

const char *hub_state_start_slow_cloud_read_json(const char *kind_name, int days)
{
    static char body[256];
    slow_cloud_read_kind_t kind = SLOW_CLOUD_READ_STATISTICS;
    if (!parse_slow_cloud_read_kind(kind_name, &kind)) {
        return "{\"ok\":false,\"accepted\":false,\"scheduled\":false,\"ready\":false,\"inflight\":false,\"error\":\"invalid kind\"}";
    }

    int normalized_days = days < 1 ? 7 : (days > 30 ? 30 : days);
    slow_cloud_read_request_result_t result = request_slow_cloud_read(kind, normalized_days);
    bool accepted = result != SLOW_CLOUD_READ_REQUEST_REJECTED;
    bool scheduled = result == SLOW_CLOUD_READ_REQUEST_SCHEDULED;
    bool ready = result == SLOW_CLOUD_READ_REQUEST_READY;
    bool inflight = result == SLOW_CLOUD_READ_REQUEST_INFLIGHT;
    snprintf(
        body,
        sizeof(body),
        "{\"ok\":true,\"accepted\":%s,\"scheduled\":%s,\"ready\":%s,\"inflight\":%s,\"kind\":\"%s\",\"days\":%d}",
        accepted ? "true" : "false",
        scheduled ? "true" : "false",
        ready ? "true" : "false",
        inflight ? "true" : "false",
        slow_cloud_read_name(kind),
        kind == SLOW_CLOUD_READ_STATISTICS ? normalized_days : 0
    );
    return body;
}

const char *hub_state_slow_cloud_reads_json(void)
{
    static char body[2048];
    bool running[3] = {0};
    bool valid[3] = {0};
    int days[3] = {0};
    int duration_ms[3] = {0};
    esp_err_t last_error[3] = {0};
    uint32_t scheduled_count[3] = {0};
    uint32_t success_count[3] = {0};
    uint32_t start_free_heap[3] = {0};
    uint32_t start_internal_free[3] = {0};
    uint32_t start_internal_largest[3] = {0};
    uint32_t completed_free_heap[3] = {0};
    uint32_t completed_internal_free[3] = {0};
    uint32_t completed_internal_largest[3] = {0};
    if (slow_cloud_read_mutex != NULL
        && xSemaphoreTake(slow_cloud_read_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int index = 0; index < 3; index++) {
            running[index] = slow_cloud_read_caches[index].running;
            valid[index] = slow_cloud_read_caches[index].valid;
            days[index] = slow_cloud_read_caches[index].days;
            duration_ms[index] = slow_cloud_read_caches[index].duration_ms;
            last_error[index] = slow_cloud_read_caches[index].last_error;
            scheduled_count[index] = slow_cloud_read_caches[index].scheduled_count;
            success_count[index] = slow_cloud_read_caches[index].success_count;
            start_free_heap[index] = slow_cloud_read_caches[index].start_free_heap;
            start_internal_free[index] = slow_cloud_read_caches[index].start_internal_free;
            start_internal_largest[index] = slow_cloud_read_caches[index].start_internal_largest;
            completed_free_heap[index] = slow_cloud_read_caches[index].completed_free_heap;
            completed_internal_free[index] = slow_cloud_read_caches[index].completed_internal_free;
            completed_internal_largest[index] = slow_cloud_read_caches[index].completed_internal_largest;
        }
        xSemaphoreGive(slow_cloud_read_mutex);
    }

    /*
     * While TLS is active, keep the live polling payload small. Sending the
     * full per-endpoint diagnostics snapshot competes for the same scarce
     * internal TCP buffers and can make this status route wait until the Cloud
     * request completes. The complete subrequest diagnostics remain available
     * immediately after the slow read finishes.
     */
    bool any_running = running[SLOW_CLOUD_READ_STATISTICS]
        || running[SLOW_CLOUD_READ_SCHEDULE]
        || running[SLOW_CLOUD_READ_FIRMWARE];
    const char *cloud_http_diagnostics = any_running
        ? "{\"deferred\":true}"
        : hub_cloud_readonly_diagnostics_json();

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"statistics\":{\"running\":%s,\"valid\":%s,\"days\":%d,\"lastDurationMs\":%d,\"lastError\":\"%s\",\"scheduled\":%lu,\"success\":%lu,"
        "\"startFreeHeap\":%lu,\"startInternalFree\":%lu,\"startInternalLargest\":%lu,"
        "\"completedFreeHeap\":%lu,\"completedInternalFree\":%lu,\"completedInternalLargest\":%lu},"
        "\"schedule\":{\"running\":%s,\"valid\":%s,\"lastDurationMs\":%d,\"lastError\":\"%s\",\"scheduled\":%lu,\"success\":%lu},"
        "\"firmware\":{\"running\":%s,\"valid\":%s,\"lastDurationMs\":%d,\"lastError\":\"%s\",\"scheduled\":%lu,\"success\":%lu},"
        "\"cloudHttp\":%s"
        "}",
        running[SLOW_CLOUD_READ_STATISTICS] ? "true" : "false",
        valid[SLOW_CLOUD_READ_STATISTICS] ? "true" : "false",
        days[SLOW_CLOUD_READ_STATISTICS],
        duration_ms[SLOW_CLOUD_READ_STATISTICS],
        esp_err_to_name(last_error[SLOW_CLOUD_READ_STATISTICS]),
        (unsigned long)scheduled_count[SLOW_CLOUD_READ_STATISTICS],
        (unsigned long)success_count[SLOW_CLOUD_READ_STATISTICS],
        (unsigned long)start_free_heap[SLOW_CLOUD_READ_STATISTICS],
        (unsigned long)start_internal_free[SLOW_CLOUD_READ_STATISTICS],
        (unsigned long)start_internal_largest[SLOW_CLOUD_READ_STATISTICS],
        (unsigned long)completed_free_heap[SLOW_CLOUD_READ_STATISTICS],
        (unsigned long)completed_internal_free[SLOW_CLOUD_READ_STATISTICS],
        (unsigned long)completed_internal_largest[SLOW_CLOUD_READ_STATISTICS],
        running[SLOW_CLOUD_READ_SCHEDULE] ? "true" : "false",
        valid[SLOW_CLOUD_READ_SCHEDULE] ? "true" : "false",
        duration_ms[SLOW_CLOUD_READ_SCHEDULE],
        esp_err_to_name(last_error[SLOW_CLOUD_READ_SCHEDULE]),
        (unsigned long)scheduled_count[SLOW_CLOUD_READ_SCHEDULE],
        (unsigned long)success_count[SLOW_CLOUD_READ_SCHEDULE],
        running[SLOW_CLOUD_READ_FIRMWARE] ? "true" : "false",
        valid[SLOW_CLOUD_READ_FIRMWARE] ? "true" : "false",
        duration_ms[SLOW_CLOUD_READ_FIRMWARE],
        esp_err_to_name(last_error[SLOW_CLOUD_READ_FIRMWARE]),
        (unsigned long)scheduled_count[SLOW_CLOUD_READ_FIRMWARE],
        (unsigned long)success_count[SLOW_CLOUD_READ_FIRMWARE],
        cloud_http_diagnostics
    );
    return body;
}

const char *hub_state_live_state_json(void)
{
    static char body[512];
    if (hub_brew_timer_live_state_json(body, sizeof(body), esp_timer_get_time())) {
        return body;
    }
    return "{"
           "\"brewTimerState\":\"stale\","
           "\"extractionSeconds\":null,"
           "\"lastEventAt\":null,"
           "\"source\":\"unconfigured\""
           "}";
}

const char *hub_state_ble_state_json(void)
{
    return hub_ble_state_json();
}

const char *hub_state_ble_diagnostics_json(void)
{
    return hub_ble_diagnostics_json();
}

void hub_state_invalidate_schedule(void)
{
    if(ensure_slow_cloud_read_mutex()!=ESP_OK||xSemaphoreTake(slow_cloud_read_mutex,portMAX_DELAY)!=pdTRUE) return;
    ++schedule_epoch;
    slow_cloud_read_cache_t *cache=slow_cloud_read_cache(SLOW_CLOUD_READ_SCHEDULE);
    if(cache) { cache->valid=false; cache->completed_ms=0; cache->last_error=ESP_ERR_INVALID_STATE; }
    xSemaphoreGive(slow_cloud_read_mutex);
}

const char *hub_state_machine_binding_json(void)
{
    static char body[256];
    hub_machine_selection_identity_t identity = {0};
    bool selected = hub_config_get_machine_selection_identity(&identity) == ESP_OK
        && identity.serial_number[0] != '\0'
        && identity.generation > 0;
    if (!selected) {
        return "{"
            "\"selected\":false,"
            "\"serialNumber\":null,"
            "\"generation\":0,"
            "\"source\":\"local-config\""
            "}";
    }

    char escaped_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};
    copy_json_safe(
        escaped_serial,
        sizeof(escaped_serial),
        identity.serial_number
    );
    snprintf(
        body,
        sizeof(body),
        "{"
        "\"selected\":true,"
        "\"serialNumber\":\"%s\","
        "\"generation\":%lu,"
        "\"source\":\"local-config\""
        "}",
        escaped_serial,
        (unsigned long)identity.generation
    );
    return body;
}
