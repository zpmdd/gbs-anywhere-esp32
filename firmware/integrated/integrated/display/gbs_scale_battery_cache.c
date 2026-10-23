#include "gbs_scale_battery_cache.h"
#include "nvs.h"
#include <string.h>

enum { RECORD_VERSION = 1, SAVE_INTERVAL_MS = 60000 };
static const int64_t MIN_UNIX = INT64_C(1704067200);
static const int64_t MAX_UNIX = INT64_C(4102444800);
static const char *NVS_NAMESPACE = "gbs_display";
static const char *NVS_KEY = "scale_batt_v1";

typedef struct {
    uint32_t version, percent;
    int64_t observed_unix;
    char serial[GBS_SCALE_BATTERY_SERIAL_SIZE];
    uint8_t reserved[7];
} battery_record_t;

static bool valid_time(int64_t value)
{
    return value >= MIN_UNIX && value <= MAX_UNIX;
}

void gbs_scale_battery_cache_bind(gbs_scale_battery_cache_t *cache,
    const char *serial, uint32_t generation, int64_t now_unix)
{
    if (!serial) serial = "";
    if (!cache->bound && !serial[0]) return;
    if (cache->bound && cache->generation == generation && !strcmp(cache->serial, serial)) return;
    bool restore = !cache->bound;
    memset(cache, 0, sizeof(*cache));
    cache->bound = true;
    cache->generation = generation;
    if (strlen(serial) >= sizeof(cache->serial)) return;
    memcpy(cache->serial, serial, strlen(serial) + 1);
    // A runtime rebind must not resurrect an earlier record, even for the same serial.
    cache->clear_pending = !restore;
    if (!restore || !serial[0]) return;

    battery_record_t record = {0};
    size_t size = sizeof(record);
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    esp_err_t err = nvs_get_blob(handle, NVS_KEY, &record, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(record) || record.version != RECORD_VERSION
        || record.percent > 100 || !memchr(record.serial, 0, sizeof(record.serial))
        || strcmp(record.serial, serial)
        || (record.observed_unix != 0 && !valid_time(record.observed_unix))) return;
    cache->known = cache->saved_known = true;
    cache->percent = cache->saved_percent = record.percent;
    cache->observed_unix = valid_time(now_unix) && record.observed_unix > now_unix
        ? 0 : record.observed_unix;
}

void gbs_scale_battery_cache_observe(gbs_scale_battery_cache_t *cache,
    const char *serial, uint32_t generation, const gbs_ui_machine_t *machine,
    int64_t now_ms, int64_t now_unix)
{
    if (!cache->bound || !cache->serial[0] || !serial || strcmp(serial, cache->serial)
        || !machine->available || machine->stale
        || generation != cache->generation || machine->scale_observed_ms <= cache->observed_ms
        || machine->scale_observed_ms <= 0 || machine->scale_observed_ms > now_ms) return;
    cache->observed_ms = machine->scale_observed_ms;
    int64_t age_ms = now_ms - machine->scale_observed_ms;
    if (!machine->scale_connected_known || !machine->scale_connected
        || !machine->scale_battery_known || machine->scale_battery_percent > 100
        || machine->scale_stale || age_ms >= GBS_UI_SCALE_FRESH_MS) return;
    cache->known = true;
    cache->percent = machine->scale_battery_percent;
    cache->observed_unix = valid_time(now_unix) && valid_time(now_unix - age_ms / 1000)
        ? now_unix - age_ms / 1000 : 0;
}

void gbs_scale_battery_cache_apply(const gbs_scale_battery_cache_t *cache,
    gbs_ui_machine_t *machine)
{
    machine->scale_battery_last_known = cache->known;
    machine->scale_battery_last_percent = cache->known ? cache->percent : 0;
    machine->scale_battery_last_observed_unix = cache->known ? cache->observed_unix : 0;
}

esp_err_t gbs_scale_battery_cache_save(gbs_scale_battery_cache_t *cache,
    int64_t now_ms, bool idle)
{
    bool changed = cache->known && (!cache->saved_known || cache->percent != cache->saved_percent);
    if (!idle || (!changed && !cache->clear_pending) || now_ms < 0
        || (cache->save_attempted && (now_ms < cache->save_attempt_ms
            || now_ms - cache->save_attempt_ms < SAVE_INTERVAL_MS))) return ESP_OK;
    cache->save_attempted = true;
    cache->save_attempt_ms = now_ms;
    battery_record_t record = { .version = RECORD_VERSION,
        .percent = cache->known ? cache->percent : UINT32_MAX,
        .observed_unix = cache->known ? cache->observed_unix : 0 };
    memcpy(record.serial, cache->serial, sizeof(record.serial));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, NVS_KEY, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        cache->clear_pending = false;
        cache->saved_known = cache->known;
        cache->saved_percent = cache->percent;
    }
    return err;
}
