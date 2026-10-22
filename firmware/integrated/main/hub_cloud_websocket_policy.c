#include "hub_cloud_websocket_policy.h"

#include <string.h>

#define START_INTERNAL_FREE_BYTES 32768U
#define START_LARGEST_BLOCK_BYTES 12288U
#define LOW_INTERNAL_FREE_BYTES 24576U
#define LOW_LARGEST_BLOCK_BYTES 8192U
#define LOW_RESOURCE_SAMPLE_LIMIT 3U
#define RESOURCE_CIRCUIT_MS 600000LL

void hub_cloud_websocket_policy_reset(hub_cloud_websocket_policy_t *policy)
{
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
    }
}

bool hub_cloud_websocket_policy_start_eligible(
    const hub_cloud_websocket_policy_t *policy,
    bool psram_initialized,
    size_t internal_free_bytes,
    size_t internal_largest_block_bytes,
    int64_t now_ms
)
{
    return policy != NULL
        && psram_initialized
        && (policy->circuit_open_until_ms <= 0 || now_ms >= policy->circuit_open_until_ms)
        && internal_free_bytes >= START_INTERNAL_FREE_BYTES
        && internal_largest_block_bytes >= START_LARGEST_BLOCK_BYTES;
}

bool hub_cloud_websocket_policy_sample_low_resources(
    hub_cloud_websocket_policy_t *policy,
    size_t internal_free_bytes,
    size_t internal_largest_block_bytes,
    int64_t now_ms
)
{
    if (policy == NULL) {
        return false;
    }
    bool low = internal_free_bytes < LOW_INTERNAL_FREE_BYTES
        || internal_largest_block_bytes < LOW_LARGEST_BLOCK_BYTES;
    if (!low) {
        policy->consecutive_low_resource_samples = 0;
        return false;
    }
    if (policy->consecutive_low_resource_samples < LOW_RESOURCE_SAMPLE_LIMIT) {
        policy->consecutive_low_resource_samples++;
    }
    if (policy->consecutive_low_resource_samples < LOW_RESOURCE_SAMPLE_LIMIT) {
        return false;
    }
    policy->consecutive_low_resource_samples = 0;
    policy->circuit_open_until_ms = now_ms + RESOURCE_CIRCUIT_MS;
    return true;
}

uint32_t hub_cloud_websocket_policy_backoff_ms(unsigned attempt, unsigned jitter_per_mille)
{
    static const uint32_t ladder_ms[] = {5000, 15000, 30000, 60000, 120000, 300000};
    size_t index = attempt < (sizeof(ladder_ms) / sizeof(ladder_ms[0]))
        ? attempt
        : (sizeof(ladder_ms) / sizeof(ladder_ms[0])) - 1;
    if (jitter_per_mille > 1000) {
        jitter_per_mille = 1000;
    }
    uint32_t scale_per_mille = 800U + (jitter_per_mille * 400U) / 1000U;
    return (uint32_t)(((uint64_t)ladder_ms[index] * scale_per_mille) / 1000U);
}
