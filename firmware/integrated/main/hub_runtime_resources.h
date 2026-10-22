#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool psram_initialized;
    size_t psram_bytes;
    size_t psram_free_bytes;
    size_t internal_free_bytes;
    size_t internal_largest_block_bytes;
} hub_runtime_resources_t;

void hub_runtime_resources_snapshot(hub_runtime_resources_t *out);
