#include "hub_statistics_cache.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define HUB_STATISTICS_CACHE_SLOT_COUNT 3
#define HUB_STATISTICS_CACHE_LOCK_TIMEOUT_MS 100

typedef struct {
    bool used;
    char serial[HUB_STATISTICS_CACHE_SERIAL_CAPACITY];
    int days;
    int64_t completed_ms;
    int64_t failure_ms;
    esp_err_t last_error;
    uint64_t last_used_sequence;
    char *body;
} statistics_slot_t;

typedef struct {
    bool active;
    uint32_t generation;
    char serial[HUB_STATISTICS_CACHE_SERIAL_CAPACITY];
    int days;
    int64_t started_ms;
} statistics_inflight_t;

typedef struct {
    bool used;
    char serial[HUB_STATISTICS_CACHE_SERIAL_CAPACITY];
    int days;
    int64_t failure_ms;
    esp_err_t error;
} statistics_failure_t;

static statistics_slot_t slots[HUB_STATISTICS_CACHE_SLOT_COUNT];
static statistics_inflight_t inflight;
static statistics_failure_t last_miss_failure;
static uint32_t next_generation;
static uint64_t next_use_sequence;
static SemaphoreHandle_t cache_mutex;
static portMUX_TYPE cache_mutex_lock = portMUX_INITIALIZER_UNLOCKED;

static bool valid_key(const char *serial, int days)
{
    return serial != NULL
        && serial[0] != '\0'
        && strlen(serial) < HUB_STATISTICS_CACHE_SERIAL_CAPACITY
        && days >= 1
        && days <= 30;
}

static bool same_key(const char *left_serial, int left_days, const char *right_serial, int right_days)
{
    return left_days == right_days && strcmp(left_serial, right_serial) == 0;
}

static void copy_serial(char *destination, const char *serial)
{
    snprintf(destination, HUB_STATISTICS_CACHE_SERIAL_CAPACITY, "%s", serial);
}

static esp_err_t ensure_cache_mutex(void)
{
    if (cache_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&cache_mutex_lock);
    if (cache_mutex == NULL) {
        cache_mutex = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&cache_mutex_lock);

    if (created != NULL) {
        vSemaphoreDelete(created);
    }
    return ESP_OK;
}

static bool lock_cache(void)
{
    return ensure_cache_mutex() == ESP_OK
        && xSemaphoreTake(
            cache_mutex,
            pdMS_TO_TICKS(HUB_STATISTICS_CACHE_LOCK_TIMEOUT_MS)
        ) == pdTRUE;
}

static void unlock_cache(void)
{
    xSemaphoreGive(cache_mutex);
}

static statistics_slot_t *find_slot_locked(const char *serial, int days)
{
    for (size_t index = 0; index < HUB_STATISTICS_CACHE_SLOT_COUNT; index++) {
        statistics_slot_t *slot = &slots[index];
        if (slot->used && same_key(slot->serial, slot->days, serial, days)) {
            return slot;
        }
    }
    return NULL;
}

static statistics_slot_t *replacement_slot_locked(void)
{
    statistics_slot_t *oldest = &slots[0];
    for (size_t index = 0; index < HUB_STATISTICS_CACHE_SLOT_COUNT; index++) {
        statistics_slot_t *slot = &slots[index];
        if (!slot->used) {
            return slot;
        }
        if (slot->last_used_sequence < oldest->last_used_sequence) {
            oldest = slot;
        }
    }
    return oldest;
}

static bool valid_candidate_body(const char *body)
{
    if (body == NULL) {
        return false;
    }
    while (*body != '\0' && isspace((unsigned char)*body)) {
        body++;
    }
    return *body == '{' || *body == '[';
}

static bool ticket_matches_locked(const hub_statistics_cache_ticket_t *ticket)
{
    return ticket != NULL
        && inflight.active
        && ticket->generation == inflight.generation
        && same_key(ticket->serial, ticket->days, inflight.serial, inflight.days);
}

esp_err_t hub_statistics_cache_begin(
    const char *serial,
    int days,
    int64_t now_ms,
    hub_statistics_cache_ticket_t *ticket
)
{
    if (!valid_key(serial, days) || ticket == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_cache()) {
        return ESP_ERR_TIMEOUT;
    }
    if (inflight.active) {
        unlock_cache();
        return ESP_ERR_INVALID_STATE;
    }

    next_generation++;
    if (next_generation == 0) {
        next_generation++;
    }
    memset(&inflight, 0, sizeof(inflight));
    inflight.active = true;
    inflight.generation = next_generation;
    inflight.days = days;
    inflight.started_ms = now_ms;
    copy_serial(inflight.serial, serial);

    memset(ticket, 0, sizeof(*ticket));
    ticket->generation = inflight.generation;
    ticket->days = days;
    copy_serial(ticket->serial, serial);
    unlock_cache();
    return ESP_OK;
}

esp_err_t hub_statistics_cache_complete(
    const hub_statistics_cache_ticket_t *ticket,
    esp_err_t error,
    char *owned_body,
    int64_t completed_ms
)
{
    if (ticket == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_cache()) {
        return ESP_ERR_TIMEOUT;
    }
    if (!ticket_matches_locked(ticket)) {
        unlock_cache();
        return ESP_ERR_INVALID_STATE;
    }

    memset(&inflight, 0, sizeof(inflight));
    esp_err_t final_error = error;
    if (final_error == ESP_OK && !valid_candidate_body(owned_body)) {
        final_error = ESP_ERR_INVALID_RESPONSE;
    }
    if (final_error != ESP_OK) {
        statistics_slot_t *slot = find_slot_locked(ticket->serial, ticket->days);
        if (slot != NULL) {
            slot->failure_ms = completed_ms;
            slot->last_error = final_error;
        } else {
            memset(&last_miss_failure, 0, sizeof(last_miss_failure));
            last_miss_failure.used = true;
            last_miss_failure.days = ticket->days;
            last_miss_failure.failure_ms = completed_ms;
            last_miss_failure.error = final_error;
            copy_serial(last_miss_failure.serial, ticket->serial);
        }
        unlock_cache();
        return final_error;
    }

    statistics_slot_t *slot = find_slot_locked(ticket->serial, ticket->days);
    if (slot == NULL) {
        slot = replacement_slot_locked();
    }
    char *old_body = slot->body;
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->days = ticket->days;
    slot->completed_ms = completed_ms;
    slot->last_used_sequence = ++next_use_sequence;
    slot->body = owned_body;
    copy_serial(slot->serial, ticket->serial);

    if (last_miss_failure.used
        && same_key(last_miss_failure.serial, last_miss_failure.days, ticket->serial, ticket->days)) {
        memset(&last_miss_failure, 0, sizeof(last_miss_failure));
    }
    unlock_cache();
    free(old_body);
    return ESP_OK;
}

hub_statistics_read_state_t hub_statistics_cache_copy(
    const char *serial,
    int days,
    char *destination,
    size_t capacity,
    hub_statistics_cache_metadata_t *metadata
)
{
    if (destination != NULL && capacity > 0) {
        destination[0] = '\0';
    }
    if (metadata != NULL) {
        memset(metadata, 0, sizeof(*metadata));
        metadata->days = days;
        metadata->last_error = ESP_OK;
    }
    if (!valid_key(serial, days) || destination == NULL || capacity == 0) {
        if (metadata != NULL) {
            metadata->last_error = ESP_ERR_INVALID_ARG;
        }
        return HUB_STATISTICS_UNAVAILABLE;
    }
    if (!lock_cache()) {
        if (metadata != NULL) {
            metadata->last_error = ESP_ERR_TIMEOUT;
        }
        return HUB_STATISTICS_UNAVAILABLE;
    }

    statistics_slot_t *slot = find_slot_locked(serial, days);
    bool same_inflight = inflight.active
        && same_key(inflight.serial, inflight.days, serial, days);
    bool same_miss_failure = last_miss_failure.used
        && same_key(last_miss_failure.serial, last_miss_failure.days, serial, days);
    if (slot != NULL && slot->body != NULL) {
        size_t body_length = strlen(slot->body);
        if (body_length + 1 > capacity) {
            if (metadata != NULL) {
                metadata->last_error = ESP_ERR_INVALID_SIZE;
            }
            unlock_cache();
            return HUB_STATISTICS_UNAVAILABLE;
        }
        memcpy(destination, slot->body, body_length + 1);
        slot->last_used_sequence = ++next_use_sequence;
        if (metadata != NULL) {
            metadata->days = slot->days;
            metadata->completed_ms = slot->completed_ms;
            metadata->stale = same_inflight;
            if (slot->failure_ms >= slot->completed_ms) {
                metadata->failure_ms = slot->failure_ms;
                metadata->last_error = slot->last_error;
                metadata->stale = true;
            }
        }
        unlock_cache();
        return HUB_STATISTICS_HIT;
    }

    if (metadata != NULL && same_miss_failure) {
        metadata->failure_ms = last_miss_failure.failure_ms;
        metadata->last_error = last_miss_failure.error;
    }
    hub_statistics_read_state_t state = same_inflight
        ? HUB_STATISTICS_REFRESHING
        : HUB_STATISTICS_UNAVAILABLE;
    unlock_cache();
    return state;
}

bool hub_statistics_cache_is_fresh(
    const char *serial,
    int days,
    int64_t now_ms,
    int64_t ttl_ms
)
{
    if (!valid_key(serial, days) || ttl_ms <= 0 || !lock_cache()) {
        return false;
    }
    statistics_slot_t *slot = find_slot_locked(serial, days);
    bool fresh = slot != NULL
        && slot->body != NULL
        && slot->completed_ms > 0
        && now_ms >= slot->completed_ms
        && now_ms - slot->completed_ms < ttl_ms
        && slot->failure_ms < slot->completed_ms;
    unlock_cache();
    return fresh;
}

hub_statistics_refresh_state_t hub_statistics_cache_refresh_state(
    const char *serial,
    int days,
    int64_t now_ms,
    int64_t ttl_ms
)
{
    if (!valid_key(serial, days) || ttl_ms <= 0 || !lock_cache()) {
        return HUB_STATISTICS_REFRESH_BLOCKED;
    }

    statistics_slot_t *slot = find_slot_locked(serial, days);
    bool same_inflight = inflight.active
        && same_key(inflight.serial, inflight.days, serial, days);
    bool newer_same_failure = slot != NULL
        ? slot->failure_ms >= slot->completed_ms
        : last_miss_failure.used
            && same_key(last_miss_failure.serial, last_miss_failure.days, serial, days);
    bool fresh = slot != NULL
        && slot->body != NULL
        && slot->completed_ms > 0
        && now_ms >= slot->completed_ms
        && now_ms - slot->completed_ms < ttl_ms
        && !newer_same_failure;
    hub_statistics_refresh_state_t state = HUB_STATISTICS_REFRESH_NEEDED;
    if (same_inflight) {
        state = HUB_STATISTICS_REFRESH_INFLIGHT;
    } else if (fresh) {
        state = HUB_STATISTICS_REFRESH_READY;
    } else if (inflight.active) {
        state = HUB_STATISTICS_REFRESH_BLOCKED;
    }
    unlock_cache();
    return state;
}

bool hub_statistics_cache_is_inflight(const char *serial, int days)
{
    if (!valid_key(serial, days) || !lock_cache()) {
        return false;
    }
    bool matches = inflight.active
        && same_key(inflight.serial, inflight.days, serial, days);
    unlock_cache();
    return matches;
}

bool hub_statistics_cache_any_inflight(void)
{
    if (!lock_cache()) {
        return false;
    }
    bool active = inflight.active;
    unlock_cache();
    return active;
}

void hub_statistics_cache_reset(void)
{
    if (!lock_cache()) {
        return;
    }
    char *bodies[HUB_STATISTICS_CACHE_SLOT_COUNT] = {0};
    for (size_t index = 0; index < HUB_STATISTICS_CACHE_SLOT_COUNT; index++) {
        bodies[index] = slots[index].body;
    }
    memset(slots, 0, sizeof(slots));
    memset(&inflight, 0, sizeof(inflight));
    memset(&last_miss_failure, 0, sizeof(last_miss_failure));
    next_generation = 0;
    next_use_sequence = 0;
    unlock_cache();

    for (size_t index = 0; index < HUB_STATISTICS_CACHE_SLOT_COUNT; index++) {
        free(bodies[index]);
    }
}
