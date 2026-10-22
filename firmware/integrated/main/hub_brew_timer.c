#include "hub_brew_timer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define HUB_BREW_TIMER_FINISHED_VISIBLE_US (15LL * 1000000LL)
#define HUB_BREW_TIMER_US_PER_SECOND 1000000.0
#define HUB_BREW_TIMER_MAX_EXTRACTION_SECONDS (60.0 * 60.0)

typedef enum {
    HUB_BREW_TIMER_STATE_IDLE = 0,
    HUB_BREW_TIMER_STATE_BREWING,
    HUB_BREW_TIMER_STATE_FINISHED,
    HUB_BREW_TIMER_STATE_STALE,
} hub_brew_timer_state_t;

typedef enum {
    HUB_BREW_TIMER_SOURCE_NONE = 0,
    HUB_BREW_TIMER_SOURCE_BLE_MACHINE_MODE,
    HUB_BREW_TIMER_SOURCE_HYBRID_BLE_CLOUD,
    HUB_BREW_TIMER_SOURCE_CLOUD_WEBSOCKET,
} hub_brew_timer_source_t;

typedef struct {
    hub_brew_timer_state_t state;
    hub_brew_timer_source_t source;
    bool has_live_source;
    bool has_started_us;
    bool has_finished_us;
    bool has_cloud_seconds;
    bool has_ble_started;
    unsigned int sequence;
    int64_t started_us;
    int64_t finished_us;
    int64_t last_event_us;
    double cloud_seconds;
    double final_seconds;
    const char *confidence;
} hub_brew_timer_runtime_t;

static hub_brew_timer_runtime_t s_timer;

static const char *state_name(hub_brew_timer_state_t state) {
    switch (state) {
    case HUB_BREW_TIMER_STATE_BREWING:
        return "brewing";
    case HUB_BREW_TIMER_STATE_FINISHED:
        return "finished";
    case HUB_BREW_TIMER_STATE_STALE:
        return "stale";
    case HUB_BREW_TIMER_STATE_IDLE:
    default:
        return "idle";
    }
}

static const char *source_name(hub_brew_timer_source_t source) {
    switch (source) {
    case HUB_BREW_TIMER_SOURCE_BLE_MACHINE_MODE:
        return "ble_machine_mode";
    case HUB_BREW_TIMER_SOURCE_HYBRID_BLE_CLOUD:
        return "hybrid_ble_cloud";
    case HUB_BREW_TIMER_SOURCE_CLOUD_WEBSOCKET:
        return "cloud_websocket";
    case HUB_BREW_TIMER_SOURCE_NONE:
    default:
        return "unconfigured";
    }
}

static bool seconds_to_us(double seconds, int64_t *dest_us) {
    if (dest_us == NULL || !isfinite(seconds) || seconds < 0.0 ||
        seconds > HUB_BREW_TIMER_MAX_EXTRACTION_SECONDS) {
        return false;
    }
    if (seconds > (double)INT64_MAX / HUB_BREW_TIMER_US_PER_SECOND) {
        return false;
    }
    *dest_us = (int64_t)(seconds * HUB_BREW_TIMER_US_PER_SECOND);
    return true;
}

static void age_finished_state(int64_t now_us) {
    if (s_timer.state == HUB_BREW_TIMER_STATE_FINISHED && s_timer.has_finished_us &&
        now_us - s_timer.finished_us >= HUB_BREW_TIMER_FINISHED_VISIBLE_US) {
        s_timer.state = HUB_BREW_TIMER_STATE_IDLE;
        s_timer.source = HUB_BREW_TIMER_SOURCE_NONE;
        s_timer.has_started_us = false;
        s_timer.has_finished_us = false;
        s_timer.has_cloud_seconds = false;
        s_timer.has_ble_started = false;
        s_timer.started_us = 0;
        s_timer.finished_us = 0;
        s_timer.cloud_seconds = 0.0;
        s_timer.final_seconds = 0.0;
        s_timer.confidence = "unconfigured";
    }
}

static double elapsed_seconds(int64_t from_us, int64_t to_us) {
    if (to_us <= from_us) {
        return 0.0;
    }
    return (double)(to_us - from_us) / HUB_BREW_TIMER_US_PER_SECOND;
}

static void append_seconds(char *dest, size_t dest_len, double seconds) {
    snprintf(dest, dest_len, "%.6g", seconds);
}

void hub_brew_timer_reset(void) {
    memset(&s_timer, 0, sizeof(s_timer));
    s_timer.state = HUB_BREW_TIMER_STATE_IDLE;
    s_timer.source = HUB_BREW_TIMER_SOURCE_NONE;
    s_timer.confidence = "unconfigured";
}

void hub_brew_timer_update_mode(const char *machine_mode, int64_t now_us) {
    age_finished_state(now_us);

    s_timer.last_event_us = now_us;

    if (s_timer.state == HUB_BREW_TIMER_STATE_FINISHED) {
        return;
    }

    if (s_timer.state == HUB_BREW_TIMER_STATE_BREWING) {
        return;
    }

    (void)machine_mode;
    if (s_timer.state == HUB_BREW_TIMER_STATE_STALE) {
        s_timer.source = HUB_BREW_TIMER_SOURCE_BLE_MACHINE_MODE;
        s_timer.confidence = "machine-mode-heartbeat";
    }
    s_timer.state = HUB_BREW_TIMER_STATE_IDLE;
}

void hub_brew_timer_record_cloud_seconds(double extraction_seconds, int64_t event_us) {
    int64_t extraction_us = 0;

    age_finished_state(event_us);

    if (!seconds_to_us(extraction_seconds, &extraction_us)) {
        return;
    }

    s_timer.has_live_source = true;
    s_timer.has_cloud_seconds = true;
    s_timer.cloud_seconds = extraction_seconds;
    s_timer.last_event_us = event_us;

    if (s_timer.state == HUB_BREW_TIMER_STATE_IDLE || s_timer.state == HUB_BREW_TIMER_STATE_STALE) {
        s_timer.sequence += 1;
        s_timer.started_us = event_us - extraction_us;
        s_timer.has_started_us = true;
        s_timer.finished_us = 0;
        s_timer.has_finished_us = false;
        s_timer.has_ble_started = false;
        s_timer.final_seconds = 0.0;
        s_timer.state = HUB_BREW_TIMER_STATE_BREWING;
        s_timer.source = HUB_BREW_TIMER_SOURCE_CLOUD_WEBSOCKET;
        s_timer.confidence = "cloud-inferred";
        return;
    }

    if (s_timer.state == HUB_BREW_TIMER_STATE_BREWING) {
        if (s_timer.has_ble_started) {
            s_timer.source = HUB_BREW_TIMER_SOURCE_HYBRID_BLE_CLOUD;
            s_timer.confidence = "hybrid";
        } else {
            s_timer.source = HUB_BREW_TIMER_SOURCE_CLOUD_WEBSOCKET;
            s_timer.confidence = "cloud-inferred";
        }
    }
}

void hub_brew_timer_update_source_stale(int64_t now_us) {
    age_finished_state(now_us);

    if (s_timer.state == HUB_BREW_TIMER_STATE_FINISHED) {
        s_timer.has_live_source = false;
        s_timer.last_event_us = now_us;
        return;
    }

    s_timer.state = HUB_BREW_TIMER_STATE_STALE;
    s_timer.source = HUB_BREW_TIMER_SOURCE_NONE;
    s_timer.has_live_source = false;
    s_timer.has_cloud_seconds = false;
    s_timer.has_finished_us = false;
    s_timer.has_ble_started = false;
    s_timer.last_event_us = now_us;
    s_timer.confidence = "unconfigured";
}

bool hub_brew_timer_available(void) {
    return s_timer.has_live_source;
}

bool hub_brew_timer_is_brewing(void) {
    return s_timer.state == HUB_BREW_TIMER_STATE_BREWING;
}

bool hub_brew_timer_live_state_json(char *dest, size_t dest_len, int64_t now_us) {
    if (dest == NULL || dest_len == 0) {
        return false;
    }

    age_finished_state(now_us);

    const hub_brew_timer_state_t state = s_timer.state;
    const char *source = source_name(state == HUB_BREW_TIMER_STATE_IDLE ||
                                             state == HUB_BREW_TIMER_STATE_STALE
                                         ? HUB_BREW_TIMER_SOURCE_NONE
                                         : s_timer.source);
    const char *confidence = s_timer.confidence != NULL ? s_timer.confidence : "unconfigured";
    char extraction[32] = "null";
    char source_json[40];

    if (state == HUB_BREW_TIMER_STATE_BREWING) {
        if (s_timer.has_cloud_seconds) {
            append_seconds(extraction, sizeof(extraction), s_timer.cloud_seconds);
        } else if (s_timer.has_started_us) {
            append_seconds(extraction, sizeof(extraction), elapsed_seconds(s_timer.started_us, now_us));
        }
    } else if (state == HUB_BREW_TIMER_STATE_FINISHED) {
        append_seconds(extraction, sizeof(extraction), s_timer.final_seconds);
    }

    snprintf(source_json, sizeof(source_json), "\"%s\"", source);

    const int written = snprintf(dest,
                                 dest_len,
                                 "{\"brewTimerState\":\"%s\","
                                 "\"extractionSeconds\":%s,"
                                 "\"lastEventAt\":null,"
                                 "\"source\":%s,"
                                 "\"startedAt\":null,"
                                 "\"finishedAt\":null,"
                                 "\"sequence\":%u,"
                                 "\"confidence\":\"%s\"}",
                                 state_name(state),
                                 extraction,
                                 source_json,
                                 s_timer.sequence,
                                 confidence);

    return written >= 0 && (size_t)written < dest_len;
}
