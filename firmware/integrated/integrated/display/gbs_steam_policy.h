#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GBS_STEAM_DISABLED, GBS_STEAM_CLOCK, GBS_STEAM_WAIT_MACHINE,
    GBS_STEAM_OUTSIDE, GBS_STEAM_WAIT_IDLE, GBS_STEAM_APPLYING,
    GBS_STEAM_READY, GBS_STEAM_MANUAL, GBS_STEAM_FAILED,
    GBS_STEAM_UNKNOWN, GBS_STEAM_STORAGE
} gbs_steam_phase_t;

typedef struct { uint32_t window, outcome; } gbs_steam_record_t;
typedef struct {
    bool enabled, target;
    gbs_steam_phase_t phase;
} gbs_steam_status_t;
typedef struct {
    gbs_steam_status_t status;
    gbs_steam_record_t record;
    uint32_t session_window;
    bool session;
    int64_t offline_since;
} gbs_steam_policy_t;
typedef struct {
    int64_t now_ms, unix_seconds;
    bool clock_ready, network_ready, fresh, online, idle, steam_enabled;
} gbs_steam_input_t;

// Beijing windows: [07:00, 12:00), [14:00, 20:00); zero means manual hours.
uint32_t gbs_steam_window(int64_t unix_seconds);
bool gbs_steam_policy_step(gbs_steam_policy_t *policy, const gbs_steam_input_t *input);
