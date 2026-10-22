#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HUB_STATISTICS_CACHE_SERIAL_CAPACITY 65

typedef enum {
    HUB_STATISTICS_HIT,
    HUB_STATISTICS_REFRESHING,
    HUB_STATISTICS_UNAVAILABLE,
} hub_statistics_read_state_t;

typedef enum {
    HUB_STATISTICS_REFRESH_READY,
    HUB_STATISTICS_REFRESH_INFLIGHT,
    HUB_STATISTICS_REFRESH_NEEDED,
    HUB_STATISTICS_REFRESH_BLOCKED,
} hub_statistics_refresh_state_t;

typedef struct {
    int days;
    int64_t completed_ms;
    int64_t failure_ms;
    esp_err_t last_error;
    bool stale;
} hub_statistics_cache_metadata_t;

typedef struct {
    uint32_t generation;
    char serial[HUB_STATISTICS_CACHE_SERIAL_CAPACITY];
    int days;
} hub_statistics_cache_ticket_t;

esp_err_t hub_statistics_cache_begin(
    const char *serial,
    int days,
    int64_t now_ms,
    hub_statistics_cache_ticket_t *ticket
);
esp_err_t hub_statistics_cache_complete(
    const hub_statistics_cache_ticket_t *ticket,
    esp_err_t error,
    char *owned_body,
    int64_t completed_ms
);
hub_statistics_read_state_t hub_statistics_cache_copy(
    const char *serial,
    int days,
    char *destination,
    size_t capacity,
    hub_statistics_cache_metadata_t *metadata
);
bool hub_statistics_cache_is_fresh(
    const char *serial,
    int days,
    int64_t now_ms,
    int64_t ttl_ms
);
hub_statistics_refresh_state_t hub_statistics_cache_refresh_state(
    const char *serial,
    int days,
    int64_t now_ms,
    int64_t ttl_ms
);
bool hub_statistics_cache_is_inflight(const char *serial, int days);
bool hub_statistics_cache_any_inflight(void);
void hub_statistics_cache_reset(void);
