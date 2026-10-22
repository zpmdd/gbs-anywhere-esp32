#include "recipe_follow.h"
#include "network.h"
#include "hub_grinder.h"
#include "hub_cloud.h"
#include "hub_brew_timer.h"
#include "hub_command_coordinator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_bool enabled;
static atomic_uint permit_until_ms;
static atomic_uint job_generation;
static SemaphoreHandle_t probe_mutex;
static char probe_requested[37], job_uuid[37], command_id[37];
static int64_t probe_requested_us;
static bool attempted, probe;
static const char *phase = "disabled";
static gbs_recipe_match_t match;
static unsigned bytes, elapsed_ms;
static esp_err_t last_error;

esp_err_t gbs_recipe_follow_init(void)
{
    probe_mutex = xSemaphoreCreateMutex();
    if (!probe_mutex) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs; uint8_t saved = 0;
    esp_err_t err = nvs_open("gbs_cfg", NVS_READONLY, &nvs);
    if (err == ESP_OK) { nvs_get_u8(nvs, "recipe_follow", &saved); nvs_close(nvs); }
    atomic_store(&enabled, saved == 1);
    phase = saved == 1 ? "ready" : "disabled";
    return ESP_OK;
}

bool hub_grinder_configure_recipe(bool follow, const char *password)
{
    if (!probe_mutex || (password && (!password[0] || strlen(password) > 64)) || !hub_grinder_begin_maintenance()) return false;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("gbs_cfg", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        if (password) err = nvs_set_str(nvs, "owner_pass", password);
        size_t length = 0;
        if (err == ESP_OK && follow) err = nvs_get_str(nvs, "owner_pass", NULL, &length);
        if (err == ESP_OK && follow && length < 2) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) err = nvs_set_u8(nvs, "recipe_follow", follow ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err == ESP_OK) atomic_store(&enabled, follow);
    hub_grinder_end_maintenance(); return err == ESP_OK;
}

bool hub_grinder_probe_recipe(const char *uuid)
{
    if (!probe_mutex || !gbs_uuid_valid(uuid) || !hub_grinder_begin_recipe_probe()) return false;
    xSemaphoreTake(probe_mutex, portMAX_DELAY);
    strcpy(probe_requested, uuid);
    probe_requested_us = esp_timer_get_time();
    xSemaphoreGive(probe_mutex); return true;
}

void gbs_recipe_follow_cancel(void) { atomic_store(&permit_until_ms, 0); }

bool gbs_recipe_follow_can_write(void)
{
    // shortcut: no verified extraction-start feed yet; require applied status before pulling a shot until that feed is wired.
    uint32_t deadline = atomic_load(&permit_until_ms), now = (uint32_t)(esp_timer_get_time() / 1000);
    return atomic_load(&enabled) && deadline && (int32_t)(deadline - now) > 0 && !hub_brew_timer_is_brewing();
}

bool gbs_recipe_follow_generation_can_write(uint32_t generation)
{
    return generation && generation == atomic_load(&job_generation) && gbs_recipe_follow_can_write() &&
        generation == atomic_load(&job_generation);
}

void gbs_recipe_follow_begin(const char *uuid)
{
    gbs_recipe_follow_cancel();
    if (atomic_fetch_add(&job_generation, 1) == UINT32_MAX) atomic_store(&job_generation, 1);
    strcpy(job_uuid, uuid); command_id[0] = 0; attempted = probe = false;
    memset(&match, 0, sizeof(match)); bytes = elapsed_ms = 0; last_error = ESP_OK;
    phase = atomic_load(&enabled) ? "waiting" : "disabled";
    if (atomic_load(&enabled)) atomic_store(&permit_until_ms, (uint32_t)(esp_timer_get_time() / 1000) + 15000);
}

void gbs_recipe_follow_tick(bool accepted)
{
    xSemaphoreTake(probe_mutex, portMAX_DELAY);
    if (probe_requested[0]) {
        strcpy(job_uuid, probe_requested); probe_requested[0] = 0;
        probe = true; attempted = false; command_id[0] = 0;
        memset(&match, 0, sizeof(match)); bytes = elapsed_ms = 0;
    }
    xSemaphoreGive(probe_mutex);
    if (command_id[0]) {
        hub_control_result_t result; esp_err_t err;
        if (hub_command_lookup(command_id, &err, &result) == HUB_COMMAND_LOOKUP_COMPLETE) {
            last_error = err;
            phase = err == ESP_OK && result.ok ? "applied" :
                result.late_side_effect_risk || !strcmp(result.error_code, "unknownOutcome") ||
                !strcmp(result.error_code, "partialApply") ? "unconfirmed" : "failed";
            command_id[0] = 0; gbs_recipe_follow_cancel();
        }
        return;
    }
    if (!job_uuid[0] || attempted || (!probe && !accepted)) return;
    if (!probe && !gbs_recipe_follow_can_write()) { attempted = true; phase = "skipped"; return; }
    if (hub_command_interactive_pending()) return;
    attempted = true; phase = "reading";
    last_error = gbs_grinder_recipe(job_uuid, &match, &bytes, &elapsed_ms);
    if (last_error == ESP_ERR_NOT_FINISHED) { attempted = false; phase = "waiting"; return; }
    if (probe) {
        phase = last_error == ESP_OK ? "probe_matched" : "probe_failed";
        probe = false; hub_grinder_end_recipe_probe(); return;
    }
    if (last_error != ESP_OK) { phase = "failed"; gbs_recipe_follow_cancel(); return; }
    if (!gbs_recipe_follow_can_write()) { phase = "skipped"; return; }
    hub_cloud_snapshot_status_t snapshot;
    hub_cloud_get_snapshot_status(&snapshot);
    // Display refreshes may be 10+ minutes apart; guarded commands verify live presets and connectivity before writing.
    if (!snapshot.has_cached_snapshot || !snapshot.has_complete_bbw) {
        phase = "machine_unavailable"; gbs_recipe_follow_cancel(); return;
    }
    hub_command_request_t request = {0};
    gbs_uuid(request.request_id); strcpy(request.expected_serial_number, gbs_config.serial);
    request.grinder_recipe_generation = atomic_load(&job_generation); request.client_surface = HUB_API_V2_SURFACE_DIAGNOSTIC;
    request.deadline.enforced = true;
    uint32_t remaining = atomic_load(&permit_until_ms) - (uint32_t)(esp_timer_get_time() / 1000);
    request.deadline.client_ttl_present = true;
    request.deadline.client_ttl_ms = remaining < 8000 ? remaining : 8000;
    unsigned dose = snapshot.dose1 == match.grams ? 1 : snapshot.dose2 == match.grams ? 2 : 0;
    if (dose) {
        request.kind = HUB_COMMAND_BBW_MODE;
        snprintf(request.value.bbw_mode.mode, sizeof(request.value.bbw_mode.mode), "Dose%u", dose);
        request.value.bbw_mode.has_preset_grams = true; request.value.bbw_mode.preset_grams = (int)match.grams;
    } else {
        request.kind = HUB_COMMAND_QUICK_WEIGHT;
        request.value.quick_weight.grams = (int)match.grams; request.value.quick_weight.require_no_stored_match = true;
    }
    hub_command_submission_t submission;
    if (hub_command_submit(&request, NULL, NULL, &submission) == HUB_COMMAND_ACCEPTED) {
        strcpy(command_id, request.request_id); phase = "applying";
    } else { phase = "failed"; last_error = submission.error; gbs_recipe_follow_cancel(); }
}

void gbs_recipe_follow_diagnostics(cJSON *parent)
{
    xSemaphoreTake(probe_mutex, portMAX_DELAY);
    if ((probe_requested[0] || probe) && esp_timer_get_time() - probe_requested_us >= 15000000) {
        probe_requested[0] = 0; probe = false; attempted = true; phase = "probe_failed"; last_error = ESP_ERR_TIMEOUT;
        hub_grinder_end_recipe_probe();
    }
    xSemaphoreGive(probe_mutex);
    cJSON *j = cJSON_AddObjectToObject(parent, "recipe_follow");
    cJSON_AddBoolToObject(j, "enabled", atomic_load(&enabled));
    cJSON_AddStringToObject(j, "phase", phase);
    cJSON_AddStringToObject(j, "grind_uuid", job_uuid);
    cJSON_AddNumberToObject(j, "target_grams", match.grams);
    cJSON_AddNumberToObject(j, "recipe_index", match.recipe_index);
    cJSON_AddNumberToObject(j, "download_bytes", bytes);
    cJSON_AddNumberToObject(j, "elapsed_ms", elapsed_ms);
    cJSON_AddNumberToObject(j, "csv_rows", match.rows);
    cJSON_AddStringToObject(j, "grind_date", match.date);
    cJSON_AddStringToObject(j, "error", esp_err_to_name(last_error));
}
