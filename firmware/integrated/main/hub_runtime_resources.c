#include "hub_runtime_resources.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#ifndef CONFIG_SPIRAM
#error "LamaBuddy Hub requires PSRAM; regenerate sdkconfig from sdkconfig.defaults"
#endif

#include "esp_psram.h"

void hub_runtime_resources_snapshot(hub_runtime_resources_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->psram_initialized = esp_psram_is_initialized();
    out->psram_bytes = out->psram_initialized ? esp_psram_get_size() : 0;
    out->psram_free_bytes = out->psram_initialized
        ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : 0;
    out->internal_free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->internal_largest_block_bytes =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
