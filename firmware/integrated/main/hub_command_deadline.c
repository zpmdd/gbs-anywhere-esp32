#include "hub_command_deadline.h"

#include <limits.h>
#include <stddef.h>

static int64_t saturating_deadline_at(int64_t accepted_at_ms, uint32_t ttl_ms)
{
    if (accepted_at_ms > INT64_MAX - (int64_t)ttl_ms) {
        return INT64_MAX;
    }
    return accepted_at_ms + (int64_t)ttl_ms;
}

bool hub_command_deadline_accept(
    hub_command_deadline_t *deadline,
    int64_t accepted_at_ms
)
{
    if (deadline == NULL) {
        return false;
    }
    if (!deadline->enforced) {
        deadline->accepted_ttl_ms = 0;
        deadline->accepted_at_ms = 0;
        deadline->deadline_at_ms = 0;
        return true;
    }

    uint32_t ttl_ms = HUB_COMMAND_DEADLINE_DEFAULT_TTL_MS;
    if (deadline->client_ttl_present) {
        if (deadline->client_ttl_ms == 0
            || deadline->client_ttl_ms > HUB_COMMAND_DEADLINE_MAX_TTL_MS) {
            return false;
        }
        ttl_ms = deadline->client_ttl_ms;
    }

    deadline->accepted_ttl_ms = ttl_ms;
    deadline->accepted_at_ms = accepted_at_ms;
    deadline->deadline_at_ms = saturating_deadline_at(accepted_at_ms, ttl_ms);
    return true;
}

int hub_command_deadline_remaining_ms(
    const hub_command_deadline_t *deadline,
    int64_t now_ms
)
{
    if (deadline == NULL || !deadline->enforced) {
        return -1;
    }
    if (now_ms >= deadline->deadline_at_ms) {
        return 0;
    }
    int64_t remaining = deadline->deadline_at_ms - now_ms;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

bool hub_command_deadline_can_admit(
    const hub_command_deadline_t *deadline,
    int64_t predicted_start_ms,
    uint32_t work_reserve_ms
)
{
    return hub_command_deadline_can_start_step(
        deadline,
        predicted_start_ms,
        work_reserve_ms
    );
}

bool hub_command_deadline_can_start_step(
    const hub_command_deadline_t *deadline,
    int64_t now_ms,
    uint32_t work_reserve_ms
)
{
    if (deadline == NULL || !deadline->enforced) {
        return true;
    }
    int remaining_ms = hub_command_deadline_remaining_ms(deadline, now_ms);
    uint64_t required_ms = (uint64_t)work_reserve_ms
        + HUB_COMMAND_DEADLINE_TERMINAL_RESERVE_MS;
    return remaining_ms > 0 && (uint64_t)remaining_ms >= required_ms;
}

int hub_command_deadline_timeout_ms(
    const hub_command_deadline_t *deadline,
    int64_t now_ms,
    uint32_t operation_cap_ms,
    uint32_t reserve_after_ms
)
{
    if (operation_cap_ms == 0) {
        return 0;
    }
    if (deadline == NULL || !deadline->enforced) {
        return operation_cap_ms > INT_MAX ? INT_MAX : (int)operation_cap_ms;
    }

    int remaining_ms = hub_command_deadline_remaining_ms(deadline, now_ms);
    uint64_t reserved_ms = (uint64_t)HUB_COMMAND_DEADLINE_TERMINAL_RESERVE_MS
        + reserve_after_ms;
    if (remaining_ms <= 0 || (uint64_t)remaining_ms <= reserved_ms) {
        return 0;
    }
    uint64_t available_ms = (uint64_t)remaining_ms - reserved_ms;
    uint64_t timeout_ms = available_ms < operation_cap_ms
        ? available_ms
        : operation_cap_ms;
    return timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms;
}
