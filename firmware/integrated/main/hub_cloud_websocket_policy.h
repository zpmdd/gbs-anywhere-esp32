#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned consecutive_low_resource_samples;
    int64_t circuit_open_until_ms;
} hub_cloud_websocket_policy_t;

void hub_cloud_websocket_policy_reset(hub_cloud_websocket_policy_t *policy);
bool hub_cloud_websocket_policy_start_eligible(
    const hub_cloud_websocket_policy_t *policy,
    bool psram_initialized,
    size_t internal_free_bytes,
    size_t internal_largest_block_bytes,
    int64_t now_ms
);
bool hub_cloud_websocket_policy_sample_low_resources(
    hub_cloud_websocket_policy_t *policy,
    size_t internal_free_bytes,
    size_t internal_largest_block_bytes,
    int64_t now_ms
);
uint32_t hub_cloud_websocket_policy_backoff_ms(unsigned attempt, unsigned jitter_per_mille);
