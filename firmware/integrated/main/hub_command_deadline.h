#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HUB_COMMAND_DEADLINE_DEFAULT_TTL_MS 8000U
#define HUB_COMMAND_DEADLINE_MAX_TTL_MS 12000U
#define HUB_COMMAND_DEADLINE_TERMINAL_RESERVE_MS 250U

/*
 * Conservative work reserves for the pre-journal Task 1 slice. Gate 0
 * hardware evidence must recalibrate these before ownership migration.
 */
#define HUB_COMMAND_DEADLINE_QUICK_WEIGHT_WORK_RESERVE_MS 6500U
#define HUB_COMMAND_DEADLINE_QUICK_WEIGHT_DOSE_RESERVE_MS 3000U
#define HUB_COMMAND_DEADLINE_PREBREW_WORK_RESERVE_MS 4500U
#define HUB_COMMAND_DEADLINE_CLOUD_MIN_START_RESERVE_MS 1000U
#define HUB_COMMAND_DEADLINE_LEGACY_UNKNOWN_WORK_RESERVE_MS 12000U

typedef struct {
    bool enforced;
    bool client_ttl_present;
    uint32_t client_ttl_ms;
    uint32_t accepted_ttl_ms;
    int64_t accepted_at_ms;
    int64_t deadline_at_ms;
} hub_command_deadline_t;

bool hub_command_deadline_accept(
    hub_command_deadline_t *deadline,
    int64_t accepted_at_ms
);
int hub_command_deadline_remaining_ms(
    const hub_command_deadline_t *deadline,
    int64_t now_ms
);
bool hub_command_deadline_can_admit(
    const hub_command_deadline_t *deadline,
    int64_t predicted_start_ms,
    uint32_t work_reserve_ms
);
bool hub_command_deadline_can_start_step(
    const hub_command_deadline_t *deadline,
    int64_t now_ms,
    uint32_t work_reserve_ms
);
int hub_command_deadline_timeout_ms(
    const hub_command_deadline_t *deadline,
    int64_t now_ms,
    uint32_t operation_cap_ms,
    uint32_t reserve_after_ms
);
