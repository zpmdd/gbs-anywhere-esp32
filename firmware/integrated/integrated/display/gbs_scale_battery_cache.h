#pragma once
#include "esp_err.h"
#include "gbs_ui_model.h"

enum { GBS_SCALE_BATTERY_SERIAL_SIZE = 65 };

// Owned by the Hub display-data task; restored values are history only.
typedef struct {
    char serial[GBS_SCALE_BATTERY_SERIAL_SIZE];
    uint32_t generation;
    bool bound, known, saved_known, clear_pending, save_attempted;
    unsigned percent, saved_percent;
    int64_t observed_unix, observed_ms, save_attempt_ms;
} gbs_scale_battery_cache_t;

void gbs_scale_battery_cache_bind(gbs_scale_battery_cache_t *cache,
    const char *serial, uint32_t generation, int64_t now_unix);
void gbs_scale_battery_cache_observe(gbs_scale_battery_cache_t *cache,
    const char *serial, uint32_t generation, const gbs_ui_machine_t *machine,
    int64_t now_ms, int64_t now_unix);
void gbs_scale_battery_cache_apply(const gbs_scale_battery_cache_t *cache,
    gbs_ui_machine_t *machine);
esp_err_t gbs_scale_battery_cache_save(gbs_scale_battery_cache_t *cache,
    int64_t now_ms, bool idle);
