#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid, wifi, grinder, cloud, storage, pairing, active, job, pending, canceling;
    bool baseline, mode_failed;
    char phase[32], outcome[40];
    int64_t observed_ms, job_deadline_ms, answer_deadline_ms, retry_at_ms;
    double target_grams, target_seconds, result_grams, result_seconds;
    double latest_grams, latest_seconds;
    double unconfirmed_grams, unconfirmed_seconds;
    unsigned successes, free_internal, largest_internal;
} gbs_ui_bridge_t;

typedef enum {
    GBS_UI_BOOT, GBS_UI_STORAGE, GBS_UI_PAIRING, GBS_UI_CLOCK, GBS_UI_BASELINE,
    GBS_UI_WAIT, GBS_UI_SENDING, GBS_UI_CANCELING, GBS_UI_UNKNOWN,
    GBS_UI_INTERRUPTED, GBS_UI_EXPIRED, GBS_UI_REFUSED, GBS_UI_WIFI,
    GBS_UI_GRINDER, GBS_UI_CLOUD, GBS_UI_MONITOR, GBS_UI_READY, GBS_UI_RECEIVED
} gbs_ui_stage_t;

typedef enum { GBS_ACTION_WEIGHT, GBS_ACTION_COFFEE, GBS_ACTION_STEAM,
    GBS_ACTION_MODE, GBS_ACTION_BRIGHTNESS, GBS_ACTION_DIM, GBS_ACTION_STEAM_ENABLED,
    GBS_ACTION_DOSE1, GBS_ACTION_DOSE2, GBS_ACTION_BBW_MODE,
    GBS_ACTION_PREBREW_MODE, GBS_ACTION_PREBREW_ON, GBS_ACTION_PREBREW_OFF,
    GBS_ACTION_POWER, GBS_ACTION_STANDBY_ENABLED, GBS_ACTION_STANDBY_MINUTES,
    GBS_ACTION_STANDBY_MODE, GBS_ACTION_COUNT } gbs_ui_action_t;
#define GBS_UI_MACHINE_ACTIONS (((1u << GBS_ACTION_COUNT) - 1) & \
    ~((1u << GBS_ACTION_MODE) | (1u << GBS_ACTION_BRIGHTNESS) | (1u << GBS_ACTION_DIM)))
#define GBS_UI_VALUE_UNKNOWN 255u
#define GBS_UI_CONTROL_WAIT_MS 30000
#define GBS_UI_MACHINE_FRESH_MS 15000

// Values are measured snapshots, never copied from targets. NAN means unavailable.
typedef struct {
    double coffee, coffee_target, steam, steam_target, weight;
    double dose1, dose2, prebrew_on, prebrew_off, standby_minutes;
    // Mode indexes follow the explicit Hub strings; unknown is never a default.
    unsigned dose_mode, prebrew_mode, machine_mode, standby_mode;
    bool available, stale, steam_enabled, steam_enabled_known;
    bool standby_enabled, standby_enabled_known;
    uint32_t revision, writable;
    int64_t observed_ms;
} gbs_ui_machine_t;

typedef enum { GBS_DATA_SCHEDULE, GBS_DATA_HISTORY, GBS_DATA_COUNT } gbs_ui_data_kind_t;
typedef enum { GBS_DATA_IDLE, GBS_DATA_LOADING, GBS_DATA_READY, GBS_DATA_STALE,
    GBS_DATA_UNAVAILABLE, GBS_DATA_UNSUPPORTED, GBS_DATA_ERROR } gbs_ui_data_state_t;
enum { GBS_SCHEDULE_LIMIT=64, GBS_HISTORY_LIMIT=32 };
typedef struct {
    char id[129];
    uint16_t on_minutes, off_minutes;
    uint8_t days;
    bool enabled, steam;
} gbs_ui_schedule_t;
typedef struct {
    char time[21]; // Normalized UTC, YYYY-MM-DDTHH:MM:SSZ.
    double seconds, dose, temperature; // Dose is a setting, never measured weight.
    unsigned mode;
} gbs_ui_coffee_t;
typedef struct {
    gbs_ui_data_kind_t kind;
    gbs_ui_data_state_t state;
    uint32_t revision, binding_generation;
    unsigned count, total;
    bool valid, editable;
    union {
        gbs_ui_schedule_t schedules[GBS_SCHEDULE_LIMIT];
        gbs_ui_coffee_t coffees[GBS_HISTORY_LIMIT];
    } items;
} gbs_ui_data_t;

bool gbs_ui_machine_action(gbs_ui_action_t action);
bool gbs_ui_schedule_equal(const gbs_ui_schedule_t *a, const gbs_ui_schedule_t *b);
double gbs_ui_action_value(gbs_ui_action_t action, const gbs_ui_machine_t *machine);
bool gbs_ui_action_unchanged(gbs_ui_action_t action, const gbs_ui_machine_t *before,
                            const gbs_ui_machine_t *after);

typedef enum {
    GBS_UI_CONTROL_IDLE, GBS_UI_CONTROL_PENDING, GBS_UI_CONTROL_CONFIRMED,
    GBS_UI_CONTROL_FAILED, GBS_UI_CONTROL_PARTIAL, GBS_UI_CONTROL_UNKNOWN
} gbs_ui_control_t;

bool gbs_ui_parse_bridge(const char *json, gbs_ui_bridge_t *out);
gbs_ui_stage_t gbs_ui_stage(const gbs_ui_bridge_t *state);
unsigned gbs_ui_remaining(int64_t deadline_ms, int64_t now_ms);
bool gbs_ui_busy(const gbs_ui_bridge_t *state, gbs_ui_control_t control, bool confirming);
unsigned gbs_ui_backlight(unsigned brightness, bool dim_enabled, int64_t idle_ms, bool busy);
// Consume the whole wake gesture, including its release, before allowing actions.
bool gbs_ui_touch(bool down, bool blank, bool *wake_gesture);
