#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hub_api_v2.h"
#include "hub_command_deadline.h"
#include "hub_config.h"
#include "hub_control.h"
#include "hub_schedule.h"

#define HUB_COMMAND_REQUEST_ID_CAPACITY 37
#define HUB_COMMAND_QUEUE_CAPACITY 7
#define HUB_COMMAND_INTERACTIVE_QUEUE_CAPACITY 4
#define HUB_COMMAND_NORMAL_QUEUE_CAPACITY 3
#define HUB_COMMAND_COMPLETED_CAPACITY 32
#define HUB_COMMAND_START_DEADLINE_MS 4000
#define HUB_COMMAND_INTERACTIVE_START_DEADLINE_MS 10000

typedef enum {
    HUB_COMMAND_QUICK_WEIGHT,
    HUB_COMMAND_BBW_DOSE,
    HUB_COMMAND_BBW_MODE,
    HUB_COMMAND_PREBREW,
    HUB_COMMAND_COFFEE_TEMPERATURE,
    HUB_COMMAND_STEAM_LEVEL,
    HUB_COMMAND_STEAM_ENABLED,
    HUB_COMMAND_STEAM_TEMPERATURE,
    HUB_COMMAND_SMART_STANDBY,
    HUB_COMMAND_POWER,
    HUB_COMMAND_BACKFLUSH,
    HUB_COMMAND_MACHINE_FIRMWARE_UPDATE,
    HUB_COMMAND_SCHEDULE,
} hub_command_kind_t;

typedef struct hub_command_request {
    char request_id[HUB_COMMAND_REQUEST_ID_CAPACITY];
    char expected_serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    hub_command_kind_t kind;
    hub_machine_selection_identity_t target;
    int64_t submitted_ms;
    int64_t start_deadline_ms;
    uint64_t payload_fingerprint;
    hub_command_deadline_t deadline;
    hub_api_v2_client_surface_t client_surface;
    uint32_t grinder_recipe_generation;
    union {
        struct {
            hub_schedule_entry_t expected, requested;
            bool confirm;
        } schedule;
        struct {
            int grams;
            bool require_no_stored_match;
        } quick_weight;
        struct {
            char dose[12];
            int grams;
        } bbw_dose;
        struct {
            char mode[24];
            bool has_preset_grams;
            int preset_grams;
        } bbw_mode;
        struct {
            char field[16];
            char value[32];
        } prebrew;
        struct {
            double temperature;
        } temperature;
        struct {
            int level;
        } steam_level;
        struct {
            bool enabled;
        } enabled;
        struct {
            bool enabled;
            int minutes;
            char mode[24];
            bool confirm;
        } smart_standby;
        struct {
            bool enabled;
            bool confirm;
        } power;
        struct {
            bool confirm;
        } confirmation;
    } value;
} hub_command_request_t;

typedef enum {
    HUB_COMMAND_ACCEPTED,
    HUB_COMMAND_DUPLICATE_INFLIGHT,
    HUB_COMMAND_DUPLICATE_COMPLETE,
    HUB_COMMAND_QUEUE_FULL,
    HUB_COMMAND_DEADLINE_CAPACITY,
    HUB_COMMAND_INVALID,
    HUB_COMMAND_CONFLICT,
} hub_command_submit_status_t;

typedef enum {
    HUB_COMMAND_LOOKUP_NOT_FOUND,
    HUB_COMMAND_LOOKUP_INFLIGHT,
    HUB_COMMAND_LOOKUP_COMPLETE,
} hub_command_lookup_status_t;

typedef struct {
    hub_command_submit_status_t status;
    esp_err_t error;
    hub_control_result_t result;
} hub_command_submission_t;

typedef void (*hub_command_completion_fn)(
    const char *request_id,
    esp_err_t error,
    const hub_control_result_t *result,
    bool deduplicated,
    void *context
);

esp_err_t hub_command_coordinator_start(void);
bool hub_command_request_id_valid(const char *request_id);
hub_command_submit_status_t hub_command_submit(
    const hub_command_request_t *request,
    hub_command_completion_fn completion,
    void *context,
    hub_command_submission_t *submission
);
hub_command_lookup_status_t hub_command_lookup(
    const char *request_id,
    esp_err_t *error,
    hub_control_result_t *result
);
void hub_command_diagnostics_json(
    const char *request_id,
    char *buffer,
    size_t buffer_len
);

/* Deterministic host-test hooks; production uses the worker task. */
bool hub_command_coordinator_process_next_for_test(void);
void hub_command_coordinator_reset(void);
bool hub_command_target_guard_get(hub_machine_selection_identity_t *target);
bool hub_command_target_guard_matches(void);
bool hub_command_interactive_pending(void);
/* Idle-only reads yield while interactive work is pending or for 10 seconds after
 * a manual interactive completion. This does not cancel an in-flight read. */
bool hub_command_idle_background_deferred(void);
bool hub_command_side_effect_allowed(void);
bool hub_command_requires_online_machine(void);
