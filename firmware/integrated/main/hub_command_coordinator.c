#include "hub_command_coordinator.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub_cloud.h"
#ifdef HUB_GRINDER
#include "recipe_follow.h"
#endif

#define HUB_COMMAND_COMPLETED_TTL_MS (10LL * 60LL * 1000LL)
#define HUB_COMMAND_MUTEX_WAIT_MS 100
#define HUB_COMMAND_WORKER_STACK_BYTES 12288
#define HUB_COMMAND_IDLE_BACKGROUND_GRACE_MS 10000LL

typedef struct {
    hub_command_request_t request;
    bool client_request_id;
    hub_command_completion_fn completion;
    void *context;
} command_queue_item_t;

typedef struct {
    bool used;
    hub_command_request_t request;
    bool client_request_id;
} command_inflight_entry_t;

typedef struct {
    bool used;
    int64_t completed_ms;
    esp_err_t error;
    hub_command_request_t request;
    hub_control_result_t result;
} command_completed_entry_t;

static const char *TAG = "hub_command_coord";
static QueueHandle_t interactive_command_queue;
static QueueHandle_t normal_command_queue;
static SemaphoreHandle_t command_mutex;
static TaskHandle_t command_worker_handle;
static EXT_RAM_BSS_ATTR command_inflight_entry_t inflight_entries[HUB_COMMAND_QUEUE_CAPACITY];
static command_completed_entry_t *completed_entries;
static size_t completed_next;
static portMUX_TYPE target_guard_lock = portMUX_INITIALIZER_UNLOCKED;
static hub_machine_selection_identity_t active_target;
static bool active_target_set;
static uint32_t active_grinder_recipe;
static portMUX_TYPE interactive_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static size_t interactive_pending_count;
static bool manual_interactive_completed;
static int64_t manual_interactive_completed_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool lock_commands(void)
{
    return command_mutex != NULL
        && xSemaphoreTake(command_mutex, pdMS_TO_TICKS(HUB_COMMAND_MUTEX_WAIT_MS)) == pdTRUE;
}

static void unlock_commands(void)
{
    xSemaphoreGive(command_mutex);
}

static const char *command_kind_name(hub_command_kind_t kind)
{
    switch (kind) {
    case HUB_COMMAND_QUICK_WEIGHT:
        return "quickWeight";
    case HUB_COMMAND_BBW_DOSE:
        return "brewByWeightDose";
    case HUB_COMMAND_BBW_MODE:
        return "brewByWeightMode";
    case HUB_COMMAND_PREBREW:
        return "prebrew";
    case HUB_COMMAND_COFFEE_TEMPERATURE:
        return "coffeeTemperature";
    case HUB_COMMAND_STEAM_LEVEL:
        return "steamLevel";
    case HUB_COMMAND_STEAM_ENABLED:
        return "steamEnabled";
    case HUB_COMMAND_STEAM_TEMPERATURE:
        return "steamTemperature";
    case HUB_COMMAND_SMART_STANDBY:
        return "smartStandby";
    case HUB_COMMAND_POWER:
        return "power";
    case HUB_COMMAND_BACKFLUSH:
        return "backflush";
    case HUB_COMMAND_MACHINE_FIRMWARE_UPDATE:
        return "machineFirmwareUpdate";
    case HUB_COMMAND_SCHEDULE:
        return "schedule";
    }
    return "unknown";
}

static bool command_is_interactive_priority(hub_command_kind_t kind)
{
    switch (kind) {
    case HUB_COMMAND_QUICK_WEIGHT:
    case HUB_COMMAND_BBW_DOSE:
    case HUB_COMMAND_BBW_MODE:
    case HUB_COMMAND_COFFEE_TEMPERATURE:
    case HUB_COMMAND_STEAM_LEVEL:
    case HUB_COMMAND_STEAM_ENABLED:
    case HUB_COMMAND_STEAM_TEMPERATURE:
        return true;
    case HUB_COMMAND_PREBREW:
    case HUB_COMMAND_SMART_STANDBY:
    case HUB_COMMAND_POWER:
    case HUB_COMMAND_BACKFLUSH:
    case HUB_COMMAND_MACHINE_FIRMWARE_UPDATE:
    case HUB_COMMAND_SCHEDULE:
        return false;
    }
    return false;
}

static bool command_supports_v2_deadline(const hub_command_request_t *request)
{
    return request->kind == HUB_COMMAND_QUICK_WEIGHT
        || request->kind == HUB_COMMAND_PREBREW
        || (request->kind == HUB_COMMAND_BBW_MODE
            && request->value.bbw_mode.has_preset_grams);
}

static bool request_is_interactive_priority(const hub_command_request_t *request)
{
    return request != NULL
        && (command_is_interactive_priority(request->kind)
            || (request->deadline.enforced
                && request->kind == HUB_COMMAND_PREBREW));
}

static uint32_t command_work_reserve_ms(const hub_command_request_t *request)
{
    if (request == NULL || !request->deadline.enforced) {
        return HUB_COMMAND_DEADLINE_LEGACY_UNKNOWN_WORK_RESERVE_MS;
    }
    switch (request->kind) {
    case HUB_COMMAND_QUICK_WEIGHT:
        return HUB_COMMAND_DEADLINE_QUICK_WEIGHT_WORK_RESERVE_MS;
    case HUB_COMMAND_BBW_MODE:
        return HUB_COMMAND_DEADLINE_QUICK_WEIGHT_WORK_RESERVE_MS;
    case HUB_COMMAND_PREBREW:
        return HUB_COMMAND_DEADLINE_PREBREW_WORK_RESERVE_MS;
    default:
        return HUB_COMMAND_DEADLINE_LEGACY_UNKNOWN_WORK_RESERVE_MS;
    }
}

static void change_interactive_pending(int delta)
{
    portENTER_CRITICAL(&interactive_pending_lock);
    if (delta > 0) {
        interactive_pending_count += (size_t)delta;
    } else if (delta < 0 && interactive_pending_count > 0) {
        interactive_pending_count--;
    }
    portEXIT_CRITICAL(&interactive_pending_lock);
}

bool hub_command_interactive_pending(void)
{
    portENTER_CRITICAL(&interactive_pending_lock);
    bool pending = interactive_pending_count > 0;
    portEXIT_CRITICAL(&interactive_pending_lock);
    return pending;
}

bool hub_command_idle_background_deferred(void)
{
    portENTER_CRITICAL(&interactive_pending_lock);
    bool pending = interactive_pending_count > 0;
    bool completed = manual_interactive_completed;
    int64_t completed_ms = manual_interactive_completed_ms;
    portEXIT_CRITICAL(&interactive_pending_lock);
    int64_t current_ms = now_ms();
    return pending || (completed && current_ms >= completed_ms
        && current_ms - completed_ms < HUB_COMMAND_IDLE_BACKGROUND_GRACE_MS);
}

static void complete_interactive_request(const hub_command_request_t *request)
{
    int64_t completed_ms = now_ms();
    portENTER_CRITICAL(&interactive_pending_lock);
    if (interactive_pending_count > 0) interactive_pending_count--;
    if (request->grinder_recipe_generation == 0) {
        manual_interactive_completed = true;
        manual_interactive_completed_ms = completed_ms;
    }
    portEXIT_CRITICAL(&interactive_pending_lock);
}

static uint64_t fingerprint_bytes(uint64_t value, const void *bytes, size_t count)
{
    const unsigned char *cursor = bytes;
    for (size_t index = 0; index < count; index++) {
        value ^= cursor[index];
        value *= 1099511628211ULL;
    }
    return value;
}

static uint64_t schedule_fingerprint(uint64_t value,const hub_schedule_entry_t *entry)
{
    size_t length=0; while(length<sizeof(entry->id)&&entry->id[length]) ++length;
    value=fingerprint_bytes(value,entry->id,length);
    const uint16_t fields[]={entry->on_minutes,entry->off_minutes,entry->days,entry->enabled,entry->steam};
    return fingerprint_bytes(value,fields,sizeof(fields));
}

static uint64_t command_payload_fingerprint(const hub_command_request_t *request)
{
    uint64_t value = 1469598103934665603ULL;
    value = fingerprint_bytes(value, &request->kind, sizeof(request->kind));
    value = fingerprint_bytes(value, &request->grinder_recipe_generation, sizeof(request->grinder_recipe_generation));
    switch (request->kind) {
    case HUB_COMMAND_QUICK_WEIGHT:
        value = fingerprint_bytes(
            value,
            &request->value.quick_weight.grams,
            sizeof(request->value.quick_weight.grams)
        );
        return fingerprint_bytes(value, &request->value.quick_weight.require_no_stored_match,
                                 sizeof(request->value.quick_weight.require_no_stored_match));
    case HUB_COMMAND_BBW_DOSE:
        value = fingerprint_bytes(value, request->value.bbw_dose.dose, strlen(request->value.bbw_dose.dose));
        return fingerprint_bytes(value, &request->value.bbw_dose.grams, sizeof(request->value.bbw_dose.grams));
    case HUB_COMMAND_BBW_MODE:
        value = fingerprint_bytes(value, request->value.bbw_mode.mode, strlen(request->value.bbw_mode.mode));
        value = fingerprint_bytes(value, &request->value.bbw_mode.has_preset_grams,
                                  sizeof(request->value.bbw_mode.has_preset_grams));
        return request->value.bbw_mode.has_preset_grams
            ? fingerprint_bytes(value, &request->value.bbw_mode.preset_grams, sizeof(request->value.bbw_mode.preset_grams))
            : value;
    case HUB_COMMAND_PREBREW:
        value = fingerprint_bytes(value, request->value.prebrew.field, strlen(request->value.prebrew.field));
        return fingerprint_bytes(value, request->value.prebrew.value, strlen(request->value.prebrew.value));
    case HUB_COMMAND_COFFEE_TEMPERATURE:
    case HUB_COMMAND_STEAM_TEMPERATURE:
        return fingerprint_bytes(value, &request->value.temperature.temperature, sizeof(request->value.temperature.temperature));
    case HUB_COMMAND_STEAM_LEVEL:
        return fingerprint_bytes(value, &request->value.steam_level.level, sizeof(request->value.steam_level.level));
    case HUB_COMMAND_STEAM_ENABLED:
        return fingerprint_bytes(value, &request->value.enabled.enabled, sizeof(request->value.enabled.enabled));
    case HUB_COMMAND_SCHEDULE:
        value=schedule_fingerprint(value,&request->value.schedule.expected);
        value=schedule_fingerprint(value,&request->value.schedule.requested);
        return fingerprint_bytes(value,&request->value.schedule.confirm,sizeof(request->value.schedule.confirm));
    case HUB_COMMAND_SMART_STANDBY:
        value = fingerprint_bytes(value, &request->value.smart_standby.enabled, sizeof(request->value.smart_standby.enabled));
        value = fingerprint_bytes(value, &request->value.smart_standby.minutes, sizeof(request->value.smart_standby.minutes));
        value = fingerprint_bytes(value, request->value.smart_standby.mode, strlen(request->value.smart_standby.mode));
        return fingerprint_bytes(value, &request->value.smart_standby.confirm, sizeof(request->value.smart_standby.confirm));
    case HUB_COMMAND_POWER:
        value = fingerprint_bytes(value, &request->value.power.enabled, sizeof(request->value.power.enabled));
        return fingerprint_bytes(value, &request->value.power.confirm, sizeof(request->value.power.confirm));
    case HUB_COMMAND_BACKFLUSH:
    case HUB_COMMAND_MACHINE_FIRMWARE_UPDATE:
        return fingerprint_bytes(value, &request->value.confirmation.confirm, sizeof(request->value.confirmation.confirm));
    }
    return value;
}

static bool command_payload_equal(
    const hub_command_request_t *left,
    const hub_command_request_t *right
)
{
    if (left->kind != right->kind || left->grinder_recipe_generation != right->grinder_recipe_generation || left->payload_fingerprint != right->payload_fingerprint) {
        return false;
    }
    switch (left->kind) {
    case HUB_COMMAND_QUICK_WEIGHT:
        return left->value.quick_weight.grams == right->value.quick_weight.grams
            && left->value.quick_weight.require_no_stored_match == right->value.quick_weight.require_no_stored_match;
    case HUB_COMMAND_BBW_DOSE:
        return strcmp(left->value.bbw_dose.dose, right->value.bbw_dose.dose) == 0
            && left->value.bbw_dose.grams == right->value.bbw_dose.grams;
    case HUB_COMMAND_BBW_MODE:
        return strcmp(left->value.bbw_mode.mode, right->value.bbw_mode.mode) == 0
            && left->value.bbw_mode.has_preset_grams == right->value.bbw_mode.has_preset_grams
            && (!left->value.bbw_mode.has_preset_grams
                || left->value.bbw_mode.preset_grams == right->value.bbw_mode.preset_grams);
    case HUB_COMMAND_PREBREW:
        return strcmp(left->value.prebrew.field, right->value.prebrew.field) == 0
            && strcmp(left->value.prebrew.value, right->value.prebrew.value) == 0;
    case HUB_COMMAND_COFFEE_TEMPERATURE:
    case HUB_COMMAND_STEAM_TEMPERATURE:
        return memcmp(
            &left->value.temperature.temperature,
            &right->value.temperature.temperature,
            sizeof(left->value.temperature.temperature)
        ) == 0;
    case HUB_COMMAND_STEAM_LEVEL:
        return left->value.steam_level.level == right->value.steam_level.level;
    case HUB_COMMAND_STEAM_ENABLED:
        return left->value.enabled.enabled == right->value.enabled.enabled;
    case HUB_COMMAND_SCHEDULE:
        return left->value.schedule.confirm==right->value.schedule.confirm
            && hub_schedule_equal(&left->value.schedule.expected,&right->value.schedule.expected)
            && hub_schedule_equal(&left->value.schedule.requested,&right->value.schedule.requested);
    case HUB_COMMAND_SMART_STANDBY:
        return left->value.smart_standby.enabled == right->value.smart_standby.enabled
            && left->value.smart_standby.minutes == right->value.smart_standby.minutes
            && strcmp(left->value.smart_standby.mode, right->value.smart_standby.mode) == 0
            && left->value.smart_standby.confirm == right->value.smart_standby.confirm;
    case HUB_COMMAND_POWER:
        return left->value.power.enabled == right->value.power.enabled
            && left->value.power.confirm == right->value.power.confirm;
    case HUB_COMMAND_BACKFLUSH:
    case HUB_COMMAND_MACHINE_FIRMWARE_UPDATE:
        return left->value.confirmation.confirm == right->value.confirmation.confirm;
    }
    return false;
}

static bool command_target_equal(
    const hub_command_request_t *left,
    const hub_command_request_t *right
)
{
    return left->target.generation == right->target.generation
        && strcmp(left->target.serial_number, right->target.serial_number) == 0;
}

static void set_active_target(const hub_machine_selection_identity_t *target)
{
    portENTER_CRITICAL(&target_guard_lock);
    active_target = *target;
    active_target_set = true;
    portEXIT_CRITICAL(&target_guard_lock);
}

static void clear_active_target(void)
{
    portENTER_CRITICAL(&target_guard_lock);
    memset(&active_target, 0, sizeof(active_target));
    active_target_set = false;
    active_grinder_recipe = false;
    portEXIT_CRITICAL(&target_guard_lock);
}

bool hub_command_target_guard_get(hub_machine_selection_identity_t *target)
{
    if (target == NULL) {
        return false;
    }
    portENTER_CRITICAL(&target_guard_lock);
    bool available = active_target_set;
    if (available) {
        *target = active_target;
    }
    portEXIT_CRITICAL(&target_guard_lock);
    return available;
}

bool hub_command_target_guard_matches(void)
{
    hub_machine_selection_identity_t target = {0};
    return !hub_command_target_guard_get(&target)
        || hub_config_machine_selection_matches(&target);
}

bool hub_command_side_effect_allowed(void)
{
    portENTER_CRITICAL(&target_guard_lock);
    uint32_t automatic = active_grinder_recipe;
    portEXIT_CRITICAL(&target_guard_lock);
#ifdef HUB_GRINDER
    if (automatic && !gbs_recipe_follow_generation_can_write(automatic)) return false;
#else
    if (automatic) return false;
#endif
    return hub_command_target_guard_matches();
}

bool hub_command_requires_online_machine(void)
{
    portENTER_CRITICAL(&target_guard_lock);
    bool required = active_grinder_recipe != 0;
    portEXIT_CRITICAL(&target_guard_lock);
    return required;
}

bool hub_command_request_id_valid(const char *request_id)
{
    if (request_id == NULL || strlen(request_id) != 36) {
        return false;
    }
    for (size_t index = 0; index < 36; index++) {
        bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphen) {
            if (request_id[index] != '-') {
                return false;
            }
        } else if (!isxdigit((unsigned char)request_id[index])) {
            return false;
        }
    }
    return true;
}

static void normalize_request_id(char *destination, const char *source)
{
    for (size_t index = 0; index < 36; index++) {
        destination[index] = (char)tolower((unsigned char)source[index]);
    }
    destination[36] = '\0';
}

static void generate_request_id(char *destination)
{
    uint32_t first = esp_random();
    uint32_t second = esp_random();
    uint32_t third = esp_random();
    uint32_t fourth = esp_random();
    snprintf(
        destination,
        HUB_COMMAND_REQUEST_ID_CAPACITY,
        "%08lx-%04lx-4%03lx-8%03lx-%04lx%08lx",
        (unsigned long)first,
        (unsigned long)((second >> 16) & 0xffffU),
        (unsigned long)(second & 0x0fffU),
        (unsigned long)((third >> 16) & 0x0fffU),
        (unsigned long)(third & 0xffffU),
        (unsigned long)fourth
    );
}

static void purge_expired_locked(int64_t current_ms)
{
    for (size_t index = 0; index < HUB_COMMAND_COMPLETED_CAPACITY; index++) {
        command_completed_entry_t *entry = &completed_entries[index];
        if (entry->used
            && current_ms >= entry->completed_ms
            && current_ms - entry->completed_ms >= HUB_COMMAND_COMPLETED_TTL_MS) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static command_completed_entry_t *find_completed_locked(const char *request_id)
{
    for (size_t index = 0; index < HUB_COMMAND_COMPLETED_CAPACITY; index++) {
        command_completed_entry_t *entry = &completed_entries[index];
        if (entry->used && strcmp(entry->result.request_id, request_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

static command_inflight_entry_t *find_inflight_locked(const char *request_id)
{
    for (size_t index = 0; index < HUB_COMMAND_QUEUE_CAPACITY; index++) {
        command_inflight_entry_t *entry = &inflight_entries[index];
        if (entry->used && strcmp(entry->request.request_id, request_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

static command_inflight_entry_t *available_inflight_locked(void)
{
    for (size_t index = 0; index < HUB_COMMAND_QUEUE_CAPACITY; index++) {
        if (!inflight_entries[index].used) {
            return &inflight_entries[index];
        }
    }
    return NULL;
}

static size_t inflight_count_for_priority_locked(bool interactive_priority)
{
    size_t count = 0;
    for (size_t index = 0; index < HUB_COMMAND_QUEUE_CAPACITY; index++) {
        command_inflight_entry_t *entry = &inflight_entries[index];
        if (entry->used
            && request_is_interactive_priority(&entry->request) == interactive_priority) {
            count++;
        }
    }
    return count;
}

static int64_t predicted_serial_start_ms_locked(int64_t current_ms)
{
    int64_t predicted_ms = current_ms;
    for (size_t index = 0; index < HUB_COMMAND_QUEUE_CAPACITY; index++) {
        const command_inflight_entry_t *entry = &inflight_entries[index];
        if (!entry->used) {
            continue;
        }
        uint32_t reserve_ms = command_work_reserve_ms(&entry->request);
        if (predicted_ms > INT64_MAX - (int64_t)reserve_ms) {
            return INT64_MAX;
        }
        predicted_ms += (int64_t)reserve_ms;
    }
    return predicted_ms;
}

static void remove_inflight_locked(const char *request_id)
{
    command_inflight_entry_t *entry = find_inflight_locked(request_id);
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    }
}

static void populate_submission_result(
    hub_command_submission_t *submission,
    hub_command_submit_status_t status,
    esp_err_t error,
    const char *request_id,
    bool client_request_id,
    bool pending
)
{
    if (submission == NULL) {
        return;
    }
    memset(submission, 0, sizeof(*submission));
    submission->status = status;
    submission->error = error;
    submission->result.ok = pending;
    submission->result.pending = pending;
    submission->result.client_request_id = client_request_id;
    snprintf(
        submission->result.request_id,
        sizeof(submission->result.request_id),
        "%s",
        request_id == NULL ? "" : request_id
    );
}

static void populate_v2_metadata(
    hub_control_result_t *result,
    const hub_command_request_t *request,
    hub_api_v2_command_status_t status,
    int64_t current_ms
)
{
    if (result == NULL || request == NULL
        || (!request->deadline.enforced && request->client_surface == HUB_API_V2_SURFACE_UNKNOWN)) {
        return;
    }
    result->api_version = HUB_API_V2_VERSION;
    result->v2_status = status;
    if (!request->deadline.enforced) return;
    result->accepted_ttl_ms = (int)request->deadline.accepted_ttl_ms;
    result->remaining_ms = hub_command_deadline_remaining_ms(
        &request->deadline,
        current_ms
    );
}

static void populate_rejection_result(
    hub_command_submission_t *submission,
    hub_command_submit_status_t status,
    esp_err_t error,
    const hub_command_request_t *request,
    bool client_request_id,
    int http_status,
    const char *error_code,
    const char *message
)
{
    populate_submission_result(
        submission,
        status,
        error,
        request != NULL ? request->request_id : "",
        client_request_id,
        false
    );
    submission->result.response_http_status = http_status;
    snprintf(submission->result.error_code, sizeof(submission->result.error_code), "%s", error_code);
    snprintf(submission->result.message, sizeof(submission->result.message), "%s", message);
}

static bool process_next(TickType_t wait_ticks)
{
    if (interactive_command_queue == NULL || normal_command_queue == NULL) {
        return false;
    }
    command_queue_item_t item = {0};
    if (xQueueReceive(interactive_command_queue, &item, 0) != pdTRUE
        && xQueueReceive(normal_command_queue, &item, wait_ticks) != pdTRUE) {
        return false;
    }

    hub_control_result_t result = {0};
    int64_t started_ms = now_ms();
    result.queue_delay_ms = (int)(started_ms - item.request.submitted_ms);
    esp_err_t error = ESP_OK;
    bool v2_deadline_expired = item.request.deadline.enforced
        && !hub_command_deadline_can_start_step(
            &item.request.deadline,
            started_ms,
            command_work_reserve_ms(&item.request)
        );
    bool legacy_start_expired = !item.request.deadline.enforced
        && started_ms >= item.request.start_deadline_ms;
    if (v2_deadline_expired || legacy_start_expired) {
        error = ESP_ERR_TIMEOUT;
        result.response_http_status = item.request.deadline.enforced ? 200 : 408;
        snprintf(result.error_code, sizeof(result.error_code), "commandExpired");
        snprintf(result.message, sizeof(result.message), "Command expired before execution started.");
    } else if (!hub_config_machine_selection_matches(&item.request.target)) {
        error = ESP_ERR_INVALID_STATE;
        result.response_http_status = 409;
        snprintf(result.error_code, sizeof(result.error_code), "machineSelectionChanged");
        snprintf(result.message, sizeof(result.message), "Active machine changed before the command started.");
    } else {
        set_active_target(&item.request.target);
        portENTER_CRITICAL(&target_guard_lock);
        active_grinder_recipe = item.request.grinder_recipe_generation;
        portEXIT_CRITICAL(&target_guard_lock);
        hub_cloud_command_timing_begin(item.request.request_id);
        error = hub_control_execute_request(&item.request, &result);
        hub_cloud_command_timing_end();
        clear_active_target();
        result.queue_delay_ms = (int)(started_ms - item.request.submitted_ms);
    }
    int64_t finished_ms = now_ms();
    hub_api_v2_command_status_t terminal_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
    if (strcmp(result.error_code, "partialApply") == 0) {
        terminal_status = HUB_API_V2_COMMAND_STATUS_PARTIAL_APPLY;
    } else if (strcmp(result.error_code, "unknownOutcome") == 0 || result.late_side_effect_risk) {
        terminal_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
    }
    if (item.request.deadline.enforced) {
        if (finished_ms >= item.request.deadline.deadline_at_ms) {
            error = ESP_ERR_TIMEOUT;
            result.ok = false;
            result.response_http_status = 200;
            if (result.upstream_attempts > 0
                || result.transport == HUB_CONTROL_TRANSPORT_CLOUD
                || result.transport == HUB_CONTROL_TRANSPORT_BLE) {
                if (terminal_status != HUB_API_V2_COMMAND_STATUS_PARTIAL_APPLY) {
                    terminal_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
                    snprintf(result.error_code, sizeof(result.error_code), "unknownOutcome");
                }
                snprintf(
                    result.message,
                    sizeof(result.message),
                    "Command reached its total deadline after an upstream side effect started."
                );
            } else {
                terminal_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
                snprintf(result.error_code, sizeof(result.error_code), "commandExpired");
                snprintf(
                    result.message,
                    sizeof(result.message),
                    "Command reached its total deadline before an upstream side effect started."
                );
            }
        }
    }
    populate_v2_metadata(&result, &item.request, terminal_status, finished_ms);
    snprintf(result.request_id, sizeof(result.request_id), "%s", item.request.request_id);
    result.client_request_id = item.client_request_id;
    result.pending = false;
    result.deduplicated = false;
    if (result.control[0] == '\0') {
        snprintf(result.control, sizeof(result.control), "%s", command_kind_name(item.request.kind));
    }

    if (command_mutex != NULL
        && xSemaphoreTake(command_mutex, portMAX_DELAY) == pdTRUE) {
        remove_inflight_locked(item.request.request_id);
        command_completed_entry_t *completed = &completed_entries[completed_next];
        completed_next = (completed_next + 1) % HUB_COMMAND_COMPLETED_CAPACITY;
        memset(completed, 0, sizeof(*completed));
        completed->used = true;
        completed->completed_ms = now_ms();
        completed->error = error;
        completed->request = item.request;
        completed->result = result;
        unlock_commands();
    }

    ESP_LOGI(TAG, "Command %.8s completed: %s", item.request.request_id, esp_err_to_name(error));
    if (item.completion != NULL) {
        item.completion(item.request.request_id, error, &result, false, item.context);
    }
    if (request_is_interactive_priority(&item.request)) {
        complete_interactive_request(&item.request);
    }
    return true;
}

static void command_worker_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (process_next(0)) {
        }
    }
}

esp_err_t hub_command_coordinator_start(void)
{
    if (completed_entries == NULL) {
        completed_entries = heap_caps_calloc(
            HUB_COMMAND_COMPLETED_CAPACITY,
            sizeof(*completed_entries),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (completed_entries == NULL) {
            completed_entries = heap_caps_calloc(
                HUB_COMMAND_COMPLETED_CAPACITY,
                sizeof(*completed_entries),
                MALLOC_CAP_8BIT
            );
        }
        if (completed_entries == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (interactive_command_queue == NULL) {
        interactive_command_queue = xQueueCreate(
            HUB_COMMAND_INTERACTIVE_QUEUE_CAPACITY,
            sizeof(command_queue_item_t)
        );
        if (interactive_command_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (normal_command_queue == NULL) {
        normal_command_queue = xQueueCreate(
            HUB_COMMAND_NORMAL_QUEUE_CAPACITY,
            sizeof(command_queue_item_t)
        );
        if (normal_command_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (command_mutex == NULL) {
        command_mutex = xSemaphoreCreateMutex();
        if (command_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (command_worker_handle == NULL) {
        if (xTaskCreate(
                command_worker_task,
                "hub_command",
                HUB_COMMAND_WORKER_STACK_BYTES,
                NULL,
                5,
                &command_worker_handle
            ) != pdPASS) {
            command_worker_handle = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

hub_command_submit_status_t hub_command_submit(
    const hub_command_request_t *request,
    hub_command_completion_fn completion,
    void *context,
    hub_command_submission_t *submission
)
{
    if (request == NULL || submission == NULL || hub_command_coordinator_start() != ESP_OK) {
        populate_submission_result(submission, HUB_COMMAND_QUEUE_FULL, ESP_ERR_INVALID_STATE, "", false, false);
        return HUB_COMMAND_QUEUE_FULL;
    }

    command_queue_item_t item = {0};
    item.request = *request;
    item.completion = completion;
    item.context = context;
    if (request->request_id[0] == '\0') {
        generate_request_id(item.request.request_id);
        item.client_request_id = false;
    } else if (!hub_command_request_id_valid(request->request_id)) {
        populate_submission_result(
            submission,
            HUB_COMMAND_INVALID,
            ESP_ERR_INVALID_ARG,
            request->request_id,
            true,
            false
        );
        return HUB_COMMAND_INVALID;
    } else {
        normalize_request_id(item.request.request_id, request->request_id);
        item.client_request_id = true;
    }

    esp_err_t identity_error = hub_config_get_machine_selection_identity(&item.request.target);
    if (identity_error != ESP_OK) {
        populate_rejection_result(
            submission,
            identity_error == ESP_ERR_INVALID_STATE ? HUB_COMMAND_CONFLICT : HUB_COMMAND_QUEUE_FULL,
            identity_error,
            &item.request,
            item.client_request_id,
            identity_error == ESP_ERR_INVALID_STATE ? 409 : 503,
            identity_error == ESP_ERR_INVALID_STATE ? "machineNotSelected" : "machineSelectionUnavailable",
            identity_error == ESP_ERR_INVALID_STATE
                ? "No active machine is selected."
                : "Active machine selection is temporarily unavailable."
        );
        return submission->status;
    }
    if (item.request.expected_serial_number[0] != '\0'
        && strcmp(
            item.request.expected_serial_number,
            item.request.target.serial_number
        ) != 0) {
        populate_rejection_result(
            submission,
            HUB_COMMAND_CONFLICT,
            ESP_ERR_INVALID_STATE,
            &item.request,
            item.client_request_id,
            409,
            "machineTargetMismatch",
            "Requested machine does not match the active machine selection."
        );
        return HUB_COMMAND_CONFLICT;
    }
    item.request.submitted_ms = now_ms();
    int64_t start_deadline_window_ms = request_is_interactive_priority(&item.request)
        ? HUB_COMMAND_INTERACTIVE_START_DEADLINE_MS
        : HUB_COMMAND_START_DEADLINE_MS;
    item.request.start_deadline_ms = item.request.submitted_ms + start_deadline_window_ms;
    if (item.request.deadline.enforced
        && (!command_supports_v2_deadline(&item.request)
            || !hub_command_deadline_accept(
                &item.request.deadline,
                item.request.submitted_ms
            ))) {
        bool supported = command_supports_v2_deadline(&item.request);
        populate_rejection_result(
            submission,
            HUB_COMMAND_INVALID,
            ESP_ERR_INVALID_ARG,
            &item.request,
            item.client_request_id,
            400,
            supported ? "invalidTtl" : "unsupportedDeadlineKind",
            supported
                ? "clientTtlMs must be an integer between 1 and 12000."
                : "The v2 deadline is available only for Quick Weight, Prebrew and guarded preset mode."
        );
        populate_v2_metadata(
            &submission->result,
            &item.request,
            HUB_API_V2_COMMAND_STATUS_COMPLETED,
            item.request.submitted_ms
        );
        return HUB_COMMAND_INVALID;
    }
    item.request.payload_fingerprint = command_payload_fingerprint(&item.request);

    if (!lock_commands()) {
        populate_submission_result(
            submission,
            HUB_COMMAND_QUEUE_FULL,
            ESP_ERR_TIMEOUT,
            item.request.request_id,
            item.client_request_id,
            false
        );
        return HUB_COMMAND_QUEUE_FULL;
    }
    purge_expired_locked(now_ms());

    command_completed_entry_t *completed = find_completed_locked(item.request.request_id);
    if (completed != NULL) {
        if (!command_payload_equal(&completed->request, &item.request)) {
            populate_rejection_result(
                submission,
                HUB_COMMAND_CONFLICT,
                ESP_ERR_INVALID_STATE,
                &item.request,
                item.client_request_id,
                409,
                "requestIdPayloadConflict",
                "Request ID was already used for a different command payload."
            );
            unlock_commands();
            return HUB_COMMAND_CONFLICT;
        }
        if (!command_target_equal(&completed->request, &item.request)) {
            populate_rejection_result(
                submission,
                HUB_COMMAND_CONFLICT,
                ESP_ERR_INVALID_STATE,
                &item.request,
                item.client_request_id,
                409,
                "requestIdTargetConflict",
                "Request ID was already used for a different machine selection."
            );
            unlock_commands();
            return HUB_COMMAND_CONFLICT;
        }
        memset(submission, 0, sizeof(*submission));
        submission->status = HUB_COMMAND_DUPLICATE_COMPLETE;
        submission->error = completed->error;
        submission->result = completed->result;
        submission->result.deduplicated = true;
        unlock_commands();
        return HUB_COMMAND_DUPLICATE_COMPLETE;
    }

    command_inflight_entry_t *existing = find_inflight_locked(item.request.request_id);
    if (existing != NULL) {
        if (!command_payload_equal(&existing->request, &item.request)) {
            populate_rejection_result(
                submission,
                HUB_COMMAND_CONFLICT,
                ESP_ERR_INVALID_STATE,
                &item.request,
                item.client_request_id,
                409,
                "requestIdPayloadConflict",
                "Request ID is already pending with a different command payload."
            );
            unlock_commands();
            return HUB_COMMAND_CONFLICT;
        }
        if (!command_target_equal(&existing->request, &item.request)) {
            populate_rejection_result(
                submission,
                HUB_COMMAND_CONFLICT,
                ESP_ERR_INVALID_STATE,
                &item.request,
                item.client_request_id,
                409,
                "requestIdTargetConflict",
                "Request ID is already pending for a different machine selection."
            );
            unlock_commands();
            return HUB_COMMAND_CONFLICT;
        }
        populate_submission_result(
            submission,
            HUB_COMMAND_DUPLICATE_INFLIGHT,
            ESP_OK,
            existing->request.request_id,
            existing->client_request_id,
            true
        );
        submission->result.deduplicated = true;
        populate_v2_metadata(
            &submission->result,
            &existing->request,
            HUB_API_V2_COMMAND_STATUS_ACCEPTED,
            now_ms()
        );
        unlock_commands();
        return HUB_COMMAND_DUPLICATE_INFLIGHT;
    }

    if (item.request.deadline.enforced) {
        int64_t predicted_start_ms = predicted_serial_start_ms_locked(now_ms());
        if (!hub_command_deadline_can_admit(
                &item.request.deadline,
                predicted_start_ms,
                command_work_reserve_ms(&item.request)
            )) {
            populate_rejection_result(
                submission,
                HUB_COMMAND_DEADLINE_CAPACITY,
                ESP_ERR_TIMEOUT,
                &item.request,
                item.client_request_id,
                503,
                "commandDeadlineCapacity",
                "The command cannot reach a terminal result within its accepted deadline."
            );
            submission->result.api_version = HUB_API_V2_VERSION;
            submission->result.v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
            submission->result.accepted_ttl_ms = 0;
            submission->result.remaining_ms = 0;
            submission->result.upstream_attempts = 0;
            unlock_commands();
            return HUB_COMMAND_DEADLINE_CAPACITY;
        }
    }

    bool interactive_priority = request_is_interactive_priority(&item.request);
    const size_t class_capacity = interactive_priority
        ? HUB_COMMAND_INTERACTIVE_QUEUE_CAPACITY
        : HUB_COMMAND_NORMAL_QUEUE_CAPACITY;
    if (inflight_count_for_priority_locked(interactive_priority) >= class_capacity) {
        populate_submission_result(
            submission,
            HUB_COMMAND_QUEUE_FULL,
            ESP_ERR_NO_MEM,
            item.request.request_id,
            item.client_request_id,
            false
        );
        unlock_commands();
        return HUB_COMMAND_QUEUE_FULL;
    }

    command_inflight_entry_t *reservation = available_inflight_locked();
    if (reservation == NULL) {
        populate_submission_result(
            submission,
            HUB_COMMAND_QUEUE_FULL,
            ESP_ERR_NO_MEM,
            item.request.request_id,
            item.client_request_id,
            false
        );
        unlock_commands();
        return HUB_COMMAND_QUEUE_FULL;
    }
    memset(reservation, 0, sizeof(*reservation));
    reservation->used = true;
    reservation->request = item.request;
    reservation->client_request_id = item.client_request_id;
    if (interactive_priority) {
        change_interactive_pending(1);
    }
#ifdef HUB_GRINDER
    if (!item.request.grinder_recipe_generation && (item.request.kind == HUB_COMMAND_QUICK_WEIGHT ||
        item.request.kind == HUB_COMMAND_BBW_MODE || item.request.kind == HUB_COMMAND_BBW_DOSE))
        gbs_recipe_follow_cancel();
#endif
    unlock_commands();

    QueueHandle_t destination_queue = interactive_priority
        ? interactive_command_queue
        : normal_command_queue;
    if (xQueueSend(destination_queue, &item, 0) != pdTRUE) {
        if (lock_commands()) {
            remove_inflight_locked(item.request.request_id);
            unlock_commands();
        }
        if (interactive_priority) {
            change_interactive_pending(-1);
        }
        populate_submission_result(
            submission,
            HUB_COMMAND_QUEUE_FULL,
            ESP_ERR_NO_MEM,
            item.request.request_id,
            item.client_request_id,
            false
        );
        return HUB_COMMAND_QUEUE_FULL;
    }
    (void)xTaskNotifyGive(command_worker_handle);

    populate_submission_result(
        submission,
        HUB_COMMAND_ACCEPTED,
        ESP_OK,
        item.request.request_id,
        item.client_request_id,
        true
    );
    populate_v2_metadata(
        &submission->result,
        &item.request,
        HUB_API_V2_COMMAND_STATUS_ACCEPTED,
        now_ms()
    );
    return HUB_COMMAND_ACCEPTED;
}

hub_command_lookup_status_t hub_command_lookup(
    const char *request_id,
    esp_err_t *error,
    hub_control_result_t *result
)
{
    if (!hub_command_request_id_valid(request_id) || !lock_commands()) {
        return HUB_COMMAND_LOOKUP_NOT_FOUND;
    }
    char normalized[HUB_COMMAND_REQUEST_ID_CAPACITY] = {0};
    normalize_request_id(normalized, request_id);
    purge_expired_locked(now_ms());
    command_completed_entry_t *completed = find_completed_locked(normalized);
    if (completed != NULL) {
        if (error != NULL) {
            *error = completed->error;
        }
        if (result != NULL) {
            *result = completed->result;
        }
        unlock_commands();
        return HUB_COMMAND_LOOKUP_COMPLETE;
    }
    command_inflight_entry_t *inflight = find_inflight_locked(normalized);
    if (inflight != NULL) {
        if (error != NULL) {
            *error = ESP_OK;
        }
        if (result != NULL) {
            memset(result, 0, sizeof(*result));
            result->ok = true;
            result->pending = true;
            result->client_request_id = inflight->client_request_id;
            snprintf(
                result->request_id,
                sizeof(result->request_id),
                "%s",
                inflight->request.request_id
            );
            populate_v2_metadata(
                result,
                &inflight->request,
                HUB_API_V2_COMMAND_STATUS_ACCEPTED,
                now_ms()
            );
        }
        unlock_commands();
        return HUB_COMMAND_LOOKUP_INFLIGHT;
    }
    unlock_commands();
    return HUB_COMMAND_LOOKUP_NOT_FOUND;
}

void hub_command_diagnostics_json(
    const char *request_id,
    char *buffer,
    size_t buffer_len
)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    buffer[0] = '\0';
    if (request_id != NULL && request_id[0] != '\0') {
        hub_control_result_t result = {0};
        esp_err_t error = ESP_OK;
        hub_command_lookup_status_t status = hub_command_lookup(request_id, &error, &result);
        if (status == HUB_COMMAND_LOOKUP_NOT_FOUND) {
            snprintf(buffer, buffer_len, "{\"status\":\"notFound\"}");
            return;
        }
        hub_control_result_json(&result, buffer, buffer_len);
        return;
    }

    if (!lock_commands()) {
        snprintf(buffer, buffer_len, "{\"commands\":[],\"inflight\":[]}");
        return;
    }
    purge_expired_locked(now_ms());
    size_t offset = 0;
    offset += snprintf(buffer + offset, buffer_len - offset, "{\"commands\":[");
    bool first = true;
    for (size_t count = 0; count < HUB_COMMAND_COMPLETED_CAPACITY && offset < buffer_len; count++) {
        size_t index = (completed_next + HUB_COMMAND_COMPLETED_CAPACITY - 1 - count)
            % HUB_COMMAND_COMPLETED_CAPACITY;
        command_completed_entry_t *entry = &completed_entries[index];
        if (!entry->used) {
            continue;
        }
        offset += snprintf(
            buffer + offset,
            buffer_len - offset,
            "%s{\"requestId\":\"%s\",\"pending\":false,"
            "\"deduplicated\":false,\"upstreamAttempts\":%d,"
            "\"control\":\"%s\",\"ok\":%s,\"transport\":\"%s\","
            "\"durationMs\":%d}",
            first ? "" : ",",
            entry->result.request_id,
            entry->result.upstream_attempts,
            entry->result.control,
            entry->result.ok ? "true" : "false",
            hub_control_transport_string(entry->result.transport),
            entry->result.duration_ms
        );
        first = false;
    }
    if (offset < buffer_len) {
        offset += snprintf(buffer + offset, buffer_len - offset, "],\"inflight\":[");
    }
    first = true;
    for (size_t index = 0; index < HUB_COMMAND_QUEUE_CAPACITY && offset < buffer_len; index++) {
        command_inflight_entry_t *entry = &inflight_entries[index];
        if (!entry->used) {
            continue;
        }
        offset += snprintf(
            buffer + offset,
            buffer_len - offset,
            "%s{\"requestId\":\"%s\",\"pending\":true,\"clientRequestId\":%s}",
            first ? "" : ",",
            entry->request.request_id,
            entry->client_request_id ? "true" : "false"
        );
        first = false;
    }
    if (offset < buffer_len) {
        snprintf(
            buffer + offset,
            buffer_len - offset,
            "],\"capacity\":%d,\"interactiveCapacity\":%d,"
            "\"normalCapacity\":%d,\"ttlSeconds\":600}",
            HUB_COMMAND_QUEUE_CAPACITY,
            HUB_COMMAND_INTERACTIVE_QUEUE_CAPACITY,
            HUB_COMMAND_NORMAL_QUEUE_CAPACITY
        );
    }
    unlock_commands();
}

bool hub_command_coordinator_process_next_for_test(void)
{
    return process_next(0);
}

void hub_command_coordinator_reset(void)
{
    clear_active_target();
    if (interactive_command_queue != NULL) {
        xQueueReset(interactive_command_queue);
    }
    if (normal_command_queue != NULL) {
        xQueueReset(normal_command_queue);
    }
    portENTER_CRITICAL(&interactive_pending_lock);
    interactive_pending_count = 0;
    manual_interactive_completed = false;
    manual_interactive_completed_ms = 0;
    portEXIT_CRITICAL(&interactive_pending_lock);
    if (!lock_commands()) {
        return;
    }
    memset(inflight_entries, 0, sizeof(inflight_entries));
    if (completed_entries != NULL) {
        memset(
            completed_entries,
            0,
            HUB_COMMAND_COMPLETED_CAPACITY * sizeof(*completed_entries)
        );
    }
    completed_next = 0;
    unlock_commands();
}
