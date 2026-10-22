#include "../main/recipe_follow.c"
#include <assert.h>

gbs_config_t gbs_config;
static int64_t now_us;
static bool brewing, interactive, download_fail, download_busy, cancel_during_download, completed;
static unsigned downloads, commands;
static hub_cloud_snapshot_status_t snapshot;
static hub_command_request_t captured;
static esp_err_t completion_error;
static hub_control_result_t completion_result;
static const char *ID = "11111111-1111-4111-8111-111111111111";
int64_t esp_timer_get_time(void) { return now_us; }
const char *esp_err_to_name(esp_err_t err) { return err ? "error" : "ESP_OK"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) { (void)m; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return pdTRUE; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) { (void)name; (void)mode; *out = 1; return ESP_OK; }
void nvs_close(nvs_handle_t n) { (void)n; }
esp_err_t nvs_get_u8(nvs_handle_t n, const char *key, uint8_t *value) { (void)n; (void)key; *value = 1; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t n, const char *key, uint8_t value) { (void)n; (void)key; (void)value; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t n, const char *key, char *out, size_t *size) { (void)n; (void)key; (void)out; *size = 5; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t n, const char *key, const char *value) { (void)n; (void)key; (void)value; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t n) { (void)n; return ESP_OK; }
bool hub_grinder_begin_maintenance(void) { return true; }
void hub_grinder_end_maintenance(void) {}
bool hub_grinder_begin_recipe_probe(void) { return true; }
void hub_grinder_end_recipe_probe(void) {}
bool hub_brew_timer_is_brewing(void) { return brewing; }
bool hub_command_interactive_pending(void) { return interactive; }
void hub_cloud_get_snapshot_status(hub_cloud_snapshot_status_t *out) { *out = snapshot; }
void gbs_uuid(char out[37]) { strcpy(out, "22222222-2222-4222-8222-222222222222"); }
esp_err_t gbs_grinder_recipe(const char *uuid, gbs_recipe_match_t *out, unsigned *length, unsigned *time)
{
    assert(!strcmp(uuid, ID));
    if (download_busy) return ESP_ERR_NOT_FINISHED;
    ++downloads; *length = 37000; *time = 1000;
    if (cancel_during_download) gbs_recipe_follow_cancel();
    if (download_fail) return ESP_FAIL;
    out->grams = 30; return ESP_OK;
}
hub_command_submit_status_t hub_command_submit(const hub_command_request_t *r, hub_command_completion_fn f, void *ctx, hub_command_submission_t *out)
{
    (void)f; (void)ctx; (void)out;
    assert(gbs_recipe_follow_can_write()); ++commands; captured = *r; return HUB_COMMAND_ACCEPTED;
}
hub_command_lookup_status_t hub_command_lookup(const char *id, esp_err_t *err, hub_control_result_t *out)
{
    (void)id; *err = completion_error; *out = completion_result;
    return completed ? HUB_COMMAND_LOOKUP_COMPLETE : HUB_COMMAND_LOOKUP_INFLIGHT;
}
static void reset(void)
{
    now_us = 1000000; brewing = interactive = download_fail = download_busy = cancel_during_download = completed = false;
    downloads = commands = 0; completion_error = ESP_OK; completion_result = (hub_control_result_t){.ok = true};
    snapshot = (hub_cloud_snapshot_status_t){.has_cached_snapshot = true, .has_complete_bbw = true,
        .is_ready = true, .dose1 = 20, .dose2 = 30, .last_refresh_ms = 1000};
    strcpy(gbs_config.serial, "selected-machine");
    assert(gbs_recipe_follow_init() == ESP_OK); gbs_recipe_follow_begin(ID);
}
int main(void)
{
    reset(); gbs_recipe_follow_tick(false); assert(!downloads && !commands);
    gbs_recipe_follow_tick(true); assert(downloads == 1 && commands == 1);
    assert(captured.grinder_recipe_generation && captured.deadline.enforced && captured.kind == HUB_COMMAND_BBW_MODE);
    assert(captured.deadline.client_ttl_present && captured.deadline.client_ttl_ms == 8000);
    assert(!strcmp(captured.value.bbw_mode.mode, "Dose2") && captured.value.bbw_mode.preset_grams == 30);
    assert(!strcmp(captured.expected_serial_number, gbs_config.serial));
    gbs_recipe_follow_tick(true); assert(downloads == 1 && commands == 1);
    completed = true; gbs_recipe_follow_tick(true); assert(!strcmp(phase, "applied"));
    gbs_recipe_follow_tick(true); assert(downloads == 1 && commands == 1);
    reset(); snapshot.dose2 = 40; gbs_recipe_follow_tick(true);
    assert(captured.kind == HUB_COMMAND_QUICK_WEIGHT && captured.value.quick_weight.require_no_stored_match && captured.value.quick_weight.grams == 30);
    reset(); brewing = true; gbs_recipe_follow_tick(true); assert(!commands && !downloads);
    reset(); interactive = true; gbs_recipe_follow_tick(true); assert(!commands && !downloads);
    interactive = false; now_us += 16000000; gbs_recipe_follow_tick(true); assert(!commands && !downloads);
    reset(); download_busy = true; gbs_recipe_follow_tick(true); assert(!commands && !downloads);
    download_busy = false; now_us += 9000000; gbs_recipe_follow_tick(true);
    assert(downloads == 1 && commands == 1 && captured.deadline.client_ttl_ms == 6000);
    reset(); gbs_recipe_follow_cancel(); gbs_recipe_follow_tick(true); assert(!commands && !downloads);
    reset(); cancel_during_download = true; gbs_recipe_follow_tick(true); assert(downloads == 1 && !commands);
    reset(); download_fail = true; gbs_recipe_follow_tick(true); gbs_recipe_follow_tick(true); assert(downloads == 1 && !commands);
    reset(); snapshot.is_ready = snapshot.has_cached_snapshot = false; gbs_recipe_follow_tick(true); assert(downloads == 1 && !commands);
    reset(); snapshot.is_ready = snapshot.has_complete_bbw = false; gbs_recipe_follow_tick(true); assert(downloads == 1 && !commands);
    // Idle display snapshots may be old; the guarded command must read current presets before writing.
    reset(); now_us = 600000000; snapshot.is_ready = false;
    gbs_recipe_follow_begin(ID); gbs_recipe_follow_tick(true);
    assert(downloads == 1 && commands == 1 && captured.grinder_recipe_generation &&
           captured.value.bbw_mode.has_preset_grams && captured.value.bbw_mode.preset_grams == 30);
    reset(); assert(hub_grinder_probe_recipe(ID)); gbs_recipe_follow_tick(false);
    assert(downloads == 1 && !commands && !strcmp(phase, "probe_matched"));
    reset(); gbs_recipe_follow_tick(true); completed = true; completion_error = ESP_FAIL;
    strcpy(completion_result.error_code, "unknownOutcome"); gbs_recipe_follow_tick(true);
    assert(!strcmp(phase, "unconfirmed")); gbs_recipe_follow_tick(true); assert(commands == 1);
    reset(); gbs_recipe_follow_tick(true); assert(gbs_recipe_follow_can_write());
    brewing = true; assert(!gbs_recipe_follow_can_write());
    brewing = false; gbs_recipe_follow_cancel(); assert(!gbs_recipe_follow_can_write());
    reset(); gbs_recipe_follow_tick(true); uint32_t old = captured.grinder_recipe_generation;
    assert(gbs_recipe_follow_generation_can_write(old));
    gbs_recipe_follow_begin(ID); assert(!gbs_recipe_follow_generation_can_write(old));
    reset(); assert(hub_grinder_probe_recipe(ID)); now_us += 16000000;
    cJSON *diagnostic = cJSON_CreateObject(); gbs_recipe_follow_diagnostics(diagnostic); cJSON_Delete(diagnostic);
    gbs_recipe_follow_tick(false); assert(!downloads && !commands && !strcmp(phase, "probe_failed"));
    puts("recipe follow: exact job, preset reuse, one-shot, probe, cancellation, deadline, busy, offline and ambiguous outcomes passed");
    return 0;
}
