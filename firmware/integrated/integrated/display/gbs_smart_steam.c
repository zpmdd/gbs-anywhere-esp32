#include "gbs_smart_steam.h"
#include "hub_cloud.h"
#include "hub_command_coordinator.h"
#include "hub_grinder.h"
#include "hub_brew_timer.h"
#include "hub_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static SemaphoreHandle_t mutex;
static gbs_steam_policy_t policy;
static hub_machine_selection_identity_t binding;
static struct {
    uint32_t version;
    char serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    gbs_steam_record_t record;
} saved;
static bool storage_ok, pending;
static char request_id[HUB_COMMAND_REQUEST_ID_CAPACITY];
static int64_t submitted_ms;
static int64_t next_cloud_refresh_ms;
static atomic_bool enabled, manual_requested, allowed_cloud;
static atomic_uint allowed_window;
static atomic_llong allowed_until;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static bool cloud_observation_fresh(const hub_cloud_snapshot_status_t *s, int64_t now)
{
    return s->has_cached_snapshot && s->steam_enabled_known && s->boiler_observed_ms > 0
        && now >= s->boiler_observed_ms && now - s->boiler_observed_ms < 90000;
}

static bool save_record(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("gbs_steam", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return false;
    saved.version = 1;
    snprintf(saved.serial, sizeof(saved.serial), "%s", binding.serial_number);
    saved.record = policy.record;
    err = nvs_set_blob(nvs, "record", &saved, sizeof(saved));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

esp_err_t gbs_smart_steam_init(void)
{
    memset(&policy, 0, sizeof(policy)); memset(&binding, 0, sizeof(binding)); memset(&saved, 0, sizeof(saved));
    pending = false;
    submitted_ms = 0;
    next_cloud_refresh_ms = 0;
    atomic_store(&allowed_cloud, false);
    atomic_store(&allowed_window, 0); atomic_store(&manual_requested, false);
    mutex = xSemaphoreCreateMutex();
    if (!mutex) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("gbs_steam", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    uint8_t value = 0;
    err = nvs_get_u8(nvs, "enabled", &value);
    storage_ok = (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && value <= 1;
    size_t size = sizeof(saved);
    err = nvs_get_blob(nvs, "record", &saved, &size);
    if (err != ESP_ERR_NVS_NOT_FOUND && (err != ESP_OK || size != sizeof(saved)
        || saved.version != 1 || !memchr(saved.serial, 0, sizeof(saved.serial))
        || saved.record.outcome < GBS_STEAM_READY || saved.record.outcome > GBS_STEAM_UNKNOWN))
        storage_ok = false;
    nvs_close(nvs);
    atomic_store(&enabled, storage_ok && value == 1);
    policy.status.enabled = atomic_load(&enabled);
    return storage_ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t gbs_smart_steam_enable(bool value)
{
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_INVALID_STATE;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("gbs_steam", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "enabled", value);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err == ESP_OK) {
        atomic_store(&enabled, value);
        policy.status.enabled = value;
        if (!value) atomic_store(&allowed_window, 0);
    }
    xSemaphoreGive(mutex);
    return err;
}

void gbs_smart_steam_manual_override(void) { atomic_store(&manual_requested, true); }

bool gbs_smart_steam_write_allowed(void)
{
    uint32_t window = atomic_load(&allowed_window);
    if (atomic_load(&allowed_cloud)) {
        hub_cloud_snapshot_status_t cloud = {0};
        hub_cloud_get_snapshot_status(&cloud);
        if (!cloud_observation_fresh(&cloud, now_ms()) || !cloud.is_connected
            || !cloud.machine_powered_on || cloud.machine_brewing) return false;
    }
    return window && atomic_load(&enabled) && !atomic_load(&manual_requested)
        && now_ms() < atomic_load(&allowed_until)
        && !strcmp(hub_cloud_time_status(), "synchronized")
        && window == gbs_steam_window(time(NULL))
        && !hub_grinder_busy() && !hub_brew_timer_is_brewing();
}

void gbs_smart_steam_tick(const gbs_ui_machine_t *m)
{
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    hub_machine_selection_identity_t selected = {0};
    if (hub_config_get_machine_selection_identity(&selected) != ESP_OK) {
        policy.status.phase = GBS_STEAM_WAIT_MACHINE;
        xSemaphoreGive(mutex); return;
    }
    if (binding.generation != selected.generation || strcmp(binding.serial_number, selected.serial_number)) {
        atomic_store(&allowed_window, 0);
        binding = selected;
        policy = (gbs_steam_policy_t){0};
        if (saved.version == 1 && !strcmp(saved.serial, binding.serial_number)) policy.record = saved.record;
        pending = false;
        submitted_ms = 0;
        next_cloud_refresh_ms = 0;
    }
    policy.status.enabled = atomic_load(&enabled);
    int64_t now = now_ms();
    bool clock_ready = !strcmp(hub_cloud_time_status(), "synchronized");
    bool fresh_ble = m->available && m->steam_enabled_known && m->ble.valid && m->ble.connected
        && m->ble.cached_fresh && m->ble.source_observed_ms > 0
        && now >= m->ble.source_observed_ms && now - m->ble.source_observed_ms < 15000;
    hub_cloud_snapshot_status_t cloud = {0};
    hub_cloud_get_snapshot_status(&cloud);
    bool use_cloud = !fresh_ble;
    bool fresh = fresh_ble || cloud_observation_fresh(&cloud, now);
    bool online = use_cloud ? cloud.is_connected && cloud.machine_powered_on : m->machine_mode == 1;
    bool steam_enabled = use_cloud ? cloud.steam_enabled : m->steam_enabled;
    int64_t observed_ms = use_cloud ? cloud.boiler_observed_ms : m->ble.source_observed_ms;
    if (policy.status.enabled && storage_ok && clock_ready && gbs_steam_window(time(NULL))
        && use_cloud && hub_wifi_station_has_ip() && now >= next_cloud_refresh_ms
        && !hub_cloud_refresh_pending() && !hub_command_idle_background_deferred()
        && !hub_grinder_busy() && !hub_brew_timer_is_brewing()) {
        // Reuse the keeper's TLS path; never read Cloud on the display/data task.
        (void)hub_cloud_request_refresh();
        next_cloud_refresh_ms = now + (pending ? 5000 : 30000);
    }
    gbs_steam_record_t before = policy.record;
    if (clock_ready && atomic_load(&manual_requested)) {
        policy.record = (gbs_steam_record_t){gbs_steam_window(time(NULL)), GBS_STEAM_MANUAL};
        policy.status.phase = GBS_STEAM_MANUAL;
        atomic_store(&allowed_window, 0);
        // Persist before clearing the cancellation observed by the command worker.
        storage_ok = save_record();
        if (storage_ok) atomic_store(&manual_requested, false);
    }
    if (pending) {
        hub_control_result_t result = {0};
        esp_err_t error = ESP_FAIL;
        hub_command_lookup_status_t status = hub_command_lookup(request_id, &error, &result);
        bool terminal = status == HUB_COMMAND_LOOKUP_COMPLETE;
        bool confirmed = terminal && error == ESP_OK && result.ok && fresh && online
            && observed_ms > submitted_ms && steam_enabled == policy.status.target;
        int timeout_ms = atomic_load(&allowed_cloud) ? 60000 : 30000;
        if (confirmed || (terminal && !result.ok) || now - submitted_ms >= timeout_ms) {
            pending = false;
            atomic_store(&allowed_window, 0);
            if (policy.record.outcome != GBS_STEAM_MANUAL) {
                policy.record.outcome = confirmed ? GBS_STEAM_READY : terminal && !result.upstream_attempts
                    ? GBS_STEAM_FAILED : GBS_STEAM_UNKNOWN;
            }
            policy.status.phase = (gbs_steam_phase_t)policy.record.outcome;
        }
    } else if (storage_ok) {
        gbs_steam_input_t input = {.now_ms = now, .unix_seconds = time(NULL), .clock_ready = clock_ready,
            .network_ready = hub_wifi_station_has_ip(),
            .fresh = fresh && (!submitted_ms || observed_ms > submitted_ms), .online = online,
            .idle = !hub_command_idle_background_deferred() && !hub_grinder_busy() && !hub_brew_timer_is_brewing()
                && !(use_cloud && cloud.machine_brewing), .steam_enabled = steam_enabled};
        bool apply = gbs_steam_policy_step(&policy, &input);
        if (apply) {
            // An interrupted/unknown write consumes this window; never replay it after sleep.
            policy.record = (gbs_steam_record_t){policy.session_window, GBS_STEAM_UNKNOWN};
            storage_ok = save_record();
            if (storage_ok) {
                hub_command_request_t command = {.kind = HUB_COMMAND_STEAM_ENABLED,
                    .automatic_steam = true, .client_surface = HUB_API_V2_SURFACE_DIAGNOSTIC};
                snprintf(command.expected_serial_number, sizeof(command.expected_serial_number), "%s", binding.serial_number);
                command.value.enabled.enabled = policy.status.target;
                atomic_store(&allowed_cloud, use_cloud);
                atomic_store(&allowed_until, observed_ms + (use_cloud ? 90000 : 15000));
                atomic_store(&allowed_window, policy.session_window);
                submitted_ms = now;
                next_cloud_refresh_ms = now + 5000;
                // Submission does not execute a machine write on this polling task.
                hub_command_submission_t submission;
                hub_command_submit_status_t status = hub_command_submit(&command, NULL, NULL, &submission);
                pending = status == HUB_COMMAND_ACCEPTED;
                if (pending) snprintf(request_id, sizeof(request_id), "%s", submission.result.request_id);
                else {
                    atomic_store(&allowed_window, 0);
                    policy.record.outcome = policy.status.phase = GBS_STEAM_FAILED;
                }
            }
        }
    }
    if (storage_ok && (before.window != policy.record.window || before.outcome != policy.record.outcome))
        storage_ok = save_record();
    if (!storage_ok) { policy.status.phase = GBS_STEAM_STORAGE; atomic_store(&allowed_window, 0); }
    xSemaphoreGive(mutex);
}

gbs_steam_status_t gbs_smart_steam_status(void)
{
    gbs_steam_status_t status = {0};
    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        status = policy.status;
        xSemaphoreGive(mutex);
    }
    return status;
}

bool gbs_smart_steam_json(char *buffer, size_t size)
{
    const char *names[] = {"disabled", "clock_waiting", "machine_waiting", "manual_hours", "idle_waiting",
        "applying", "confirmed", "manual_override", "failed", "unknown", "storage_failed"};
    gbs_steam_status_t status = gbs_smart_steam_status();
    int length = snprintf(buffer, size, "{\"enabled\":%s,\"timezone\":\"Asia/Shanghai\","
        "\"morning\":\"07:00-12:00\",\"afternoon\":\"14:00-20:00\",\"phase\":\"%s\",\"targetEnabled\":%s}",
        status.enabled ? "true" : "false", names[status.phase], status.target ? "true" : "false");
    return length >= 0 && (size_t)length < size;
}
