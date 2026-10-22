#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HUB_API_V2_VERSION 2
#define HUB_API_V2_HEADER_NAME "X-LamaBuddy-API-Version"
#define HUB_API_V2_MACHINE_SERIAL_HEADER_NAME "X-LamaBuddy-Machine-Serial"

typedef enum {
    HUB_API_V2_COMMAND_STATUS_ACCEPTED = 0,
    HUB_API_V2_COMMAND_STATUS_RUNNING,
    HUB_API_V2_COMMAND_STATUS_COMPLETED,
    HUB_API_V2_COMMAND_STATUS_PARTIAL_APPLY,
    HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME,
} hub_api_v2_command_status_t;

typedef enum {
    HUB_API_V2_STEP_NOT_STARTED = 0,
    HUB_API_V2_STEP_STARTED,
    HUB_API_V2_STEP_COMPLETED,
    HUB_API_V2_STEP_UNKNOWN,
} hub_api_v2_step_state_t;

typedef enum {
    HUB_API_V2_SIDE_EFFECT_NOT_STARTED = 0,
    HUB_API_V2_SIDE_EFFECT_STARTED,
    HUB_API_V2_SIDE_EFFECT_CONFIRMED,
    HUB_API_V2_SIDE_EFFECT_UNKNOWN,
} hub_api_v2_side_effect_state_t;

typedef enum {
    HUB_API_V2_SURFACE_UNKNOWN = 0,
    HUB_API_V2_SURFACE_IPHONE_APP,
    HUB_API_V2_SURFACE_IPHONE_WIDGET,
    HUB_API_V2_SURFACE_SIRI,
    HUB_API_V2_SURFACE_WATCH_APP,
    HUB_API_V2_SURFACE_WATCH_WIDGET,
    HUB_API_V2_SURFACE_DIAGNOSTIC,
} hub_api_v2_client_surface_t;

static inline const char *hub_api_v2_command_status_string(
    hub_api_v2_command_status_t status
)
{
    switch (status) {
    case HUB_API_V2_COMMAND_STATUS_ACCEPTED:
        return "accepted";
    case HUB_API_V2_COMMAND_STATUS_RUNNING:
        return "running";
    case HUB_API_V2_COMMAND_STATUS_COMPLETED:
        return "completed";
    case HUB_API_V2_COMMAND_STATUS_PARTIAL_APPLY:
        return "partialApply";
    case HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME:
        return "unknownOutcome";
    }
    return "completed";
}
