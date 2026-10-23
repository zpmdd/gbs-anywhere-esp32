#include "network.h"
#include "recipe_follow.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GBS_WORKER_TEST
#include "../tests/worker_platform.h"
#else
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "hub_config.h"
#include "hub_wifi.h"
#include "hub_grinder.h"
#include "hub_command_coordinator.h"
#include "hub_version.h"
#endif

static const char *TAG = "gbs";
static atomic_bool wifi_online, desired_active;
// One admission gate prevents an OTA from interrupting or accepting a grinder job.
static atomic_int admission; // 0 idle, 1 job, 2 maintenance, 3 read-only recipe probe
bool hub_grinder_busy(void)
{
    return atomic_load(&admission) != 0;
}
bool hub_grinder_begin_maintenance(void)
{
    int idle=0;
    return atomic_compare_exchange_strong(&admission,&idle,2);
}
void hub_grinder_end_maintenance(void)
{
    int updating=2;
    atomic_compare_exchange_strong(&admission,&updating,0);
}
bool hub_grinder_begin_recipe_probe(void)
{
    int idle = 0;
    return atomic_compare_exchange_strong(&admission, &idle, 3);
}
void hub_grinder_end_recipe_probe(void)
{
    int probing = 3;
    atomic_compare_exchange_strong(&admission, &probing, 0);
}
#ifndef GBS_WORKER_TEST
static SemaphoreHandle_t state_mutex;
static EXT_RAM_BSS_ATTR char state_json[6144];
static atomic_bool configured, startup_failed;
#endif
static nvs_handle_t journal_nvs;
static gbs_ledger_t ledger;
static bool active, cloud_initialized, have_job, pending, accepted, canceled, ongoing_missing, invalid_coffee;
static gbs_health_t cloud_health, grinder_health;
static bool baseline_jobs = true, storage_ok = true;
static gbs_job_t current;
static gbs_coffee_t latest, answer;
static char event_uuid[37], issue[192], storage_issue[128], api_status[80];
static int64_t job_deadline_us, answer_deadline_us;
static int64_t next_jobs;
static uint32_t accepts;
static int64_t pairing_refused_since_us = -1;
static bool pairing_problem;

static void note_health(gbs_health_t *health, bool ok, const gbs_request_info_t *request,
                        esp_err_t err, const char *api_status)
{
    uint32_t recoveries = health->recoveries;
    char previous[sizeof(health->error)];
    strcpy(previous, health->error);
    gbs_health_update(health, ok, request->operation, request->http_status,
                      esp_err_to_name(err), api_status, gbs_now_ms());
    const char *name = health == &grinder_health ? "grinder" : "cloud";
    if (!ok && strcmp(previous, health->error)) ESP_LOGW(TAG, "%s %s; retrying", name, health->error);
    if (health->recoveries != recoveries) ESP_LOGI(TAG, "%s connection recovered", name);
}

static esp_err_t read_coffee(gbs_coffee_t *coffee)
{
    esp_err_t err = gbs_cloud_coffee(coffee);
    note_health(&cloud_health, err == ESP_OK, &gbs_cloud_request, err, "");
    return err;
}

static void add_display_state(cJSON *j)
{
    cJSON_AddNumberToObject(j, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddBoolToObject(j, "have_job", have_job);
    cJSON_AddBoolToObject(j, "pending", pending);
    cJSON_AddBoolToObject(j, "canceling", pending && canceled);
    cJSON_AddBoolToObject(j, "coffee_invalid", invalid_coffee);
    cJSON_AddBoolToObject(j, "baseline_pending", baseline_jobs || !cloud_initialized);
    cJSON_AddNumberToObject(j, "job_deadline_ms", have_job && !pending ? job_deadline_us / 1000 : 0);
    cJSON_AddNumberToObject(j, "answer_deadline_ms", pending ? answer_deadline_us / 1000 : 0);
    cJSON_AddNumberToObject(j, "grinder_retry_at_ms", next_jobs / 1000);
    cJSON_AddBoolToObject(j, "mode_change_failed", !strcmp(issue, "mode persistence failed"));
    if (!canceled && answer.usable &&
        (pending || (!have_job && !strcmp(ledger.last_outcome, "delivery_unknown")))) {
        cJSON_AddNumberToObject(j, "unconfirmed_grams", answer.mass_mg / 1000.0);
        cJSON_AddNumberToObject(j, "unconfirmed_seconds", answer.duration_ms / 1000.0);
    }
}

#ifndef GBS_WORKER_TEST
static void publish(const char *phase)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return;
    cJSON_AddStringToObject(j, "firmware", LAMABUDDY_FIRMWARE_VERSION);
    cJSON_AddStringToObject(j, "upstream", "companion 423e086; upstream reviewed ebaac65");
    cJSON_AddStringToObject(j, "build", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "mode", active ? "active" : "monitor");
    cJSON_AddStringToObject(j, "phase", phase);
    add_display_state(j);
    gbs_recipe_follow_diagnostics(j);
    cJSON_AddBoolToObject(j, "wifi", atomic_load(&wifi_online));
    cJSON_AddBoolToObject(j, "grinder", grinder_health.ok && atomic_load(&wifi_online));
    cJSON_AddBoolToObject(j, "cloud", cloud_health.ok && atomic_load(&wifi_online));
    cJSON_AddBoolToObject(j, "storage", storage_ok);
    const char *problem = !storage_ok ? storage_issue : issue[0] ? issue :
                          grinder_health.error[0] ? grinder_health.error : cloud_health.error;
    const char *source = !storage_ok ? "storage" : issue[0] ? "task" :
                         grinder_health.error[0] ? "grinder" : cloud_health.error[0] ? "cloud" : "";
    cJSON_AddStringToObject(j, "issue", problem);
    cJSON_AddStringToObject(j, "issue_source", source);
    cJSON_AddBoolToObject(j, "pairing_required", pairing_problem);
    cJSON *connections = cJSON_AddObjectToObject(j, "connections");
    cJSON_AddItemToObject(connections, "grinder", gbs_health_json(&grinder_health));
    cJSON_AddItemToObject(connections, "cloud", gbs_health_json(&cloud_health));
    cJSON_AddNumberToObject(j, "grinder_retry_s", next_jobs > esp_timer_get_time() ?
        (next_jobs - esp_timer_get_time()) / 1000000 + 1 : 0);
    cJSON_AddNumberToObject(j, "job_remaining_s", have_job && !pending && job_deadline_us > esp_timer_get_time() ?
        (job_deadline_us - esp_timer_get_time()) / 1000000 : 0);
    cJSON_AddStringToObject(j, "last_outcome", ledger.last_outcome);
    cJSON_AddStringToObject(j, "grind_uuid", have_job ? current.uuid : "");
    cJSON_AddNumberToObject(j, "target_seconds", have_job ? current.target_ms / 1000.0 : 0);
    cJSON_AddNumberToObject(j, "target_grams", have_job ? current.target_grams : 0);
    cJSON_AddNumberToObject(j, "latest_coffee_ms", latest.time_ms);
    cJSON_AddNumberToObject(j, "latest_seconds", latest.duration_ms / 1000.0);
    cJSON_AddNumberToObject(j, "latest_grams", latest.mass_mg / 1000.0);
    cJSON_AddNumberToObject(j, "successes", ledger.successes);
    cJSON_AddNumberToObject(j, "accepts_this_boot", accepts);
    cJSON_AddNumberToObject(j, "last_success_seconds", ledger.last_duration_ms / 1000.0);
    cJSON_AddNumberToObject(j, "last_success_grams", ledger.last_mass_mg / 1000.0);
    cJSON_AddNumberToObject(j, "now_ms", gbs_now_ms());
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(j, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(j, "largest_internal", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(j, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "worker_stack_free", uxTaskGetStackHighWaterMark(NULL));
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        if (strlen(text) < sizeof(state_json)) strcpy(state_json, text);
        xSemaphoreGive(state_mutex);
        cJSON_free(text);
    }
}
#endif

static bool save_ledger(void)
{
    esp_err_t err = nvs_set_blob(journal_nvs, "ledger", &ledger, sizeof(ledger));
    if (err == ESP_OK) err = nvs_commit(journal_nvs);
    if (err != ESP_OK) {
        storage_ok = false;
        snprintf(storage_issue, sizeof(storage_issue), "journal failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s; refusing further result delivery", storage_issue);
    }
    return err == ESP_OK;
}

static void restore_ledger(esp_err_t read_error, size_t size)
{
    if (read_error == ESP_ERR_NVS_NOT_FOUND) { memset(&ledger, 0, sizeof(ledger)); ledger.version = 1; }
    else if (read_error != ESP_OK || size != sizeof(ledger) || !gbs_ledger_valid(&ledger)) {
        memset(&ledger, 0, sizeof(ledger));
        storage_ok = false; strcpy(storage_issue, "invalid journal; result delivery disabled");
    }
    if (storage_ok && ledger.inflight[0]) {
        gbs_remember(&ledger, ledger.inflight);
        strcpy(ledger.last_outcome, "interrupted_by_restart");
        ledger.inflight[0] = 0; save_ledger();
    } else if (storage_ok && !strcmp(ledger.last_outcome, "success")) {
        // Keep the last measurement as history, not a current cup after a cold boot.
        ledger.last_outcome[0] = 0;
    }
}

static void finish_job(const char *outcome)
{
    gbs_recipe_follow_cancel();
    snprintf(ledger.last_outcome, sizeof(ledger.last_outcome), "%s", outcome);
    ledger.inflight[0] = 0;
    save_ledger();
    have_job = pending = accepted = ongoing_missing = false;
    atomic_store(&admission,0);
    ESP_LOGI(TAG, "job ended: %s", outcome);
}

static esp_err_t grinder_call(const char *endpoint, cJSON *body, cJSON **reply)
{
    if (!body) { strcpy(api_status, "NoMemory"); return ESP_ERR_NO_MEM; }
    esp_err_t err = gbs_grinder(endpoint, body, reply, api_status);
    cJSON_Delete(body);
    if (strcmp(endpoint, "brewJobs") && strcmp(api_status, "LocalBusy"))
        note_health(&grinder_health, err == ESP_OK && (!strcmp(api_status, "Ok") || gbs_job_error(api_status)),
                    &gbs_grinder_request, err, api_status);
    return err;
}

static bool take_job(void)
{
    // Acceptance may succeed even when its reply is lost; an earlier absence is stale.
    ongoing_missing = false;
    cJSON *reply = NULL;
    esp_err_t err = grinder_call("brewAccept", gbs_accept_body(gbs_config.pairing_id, &current), &reply);
    cJSON_Delete(reply);
    if (err == ESP_OK && strcmp(api_status, "Ok") == 0) {
        accepted = true; ++accepts;
        ESP_LOGI(TAG, "accepted grind=%s target_ms=%" PRIu32, current.uuid, current.target_ms);
        return true;
    }
    if (err == ESP_OK && pending && !strcmp(api_status, "ClientErrorBrewJobNotExisting")) {
        // As upstream: treat it as taken until brewDone explicitly says otherwise.
        accepted = true;
        return true;
    }
    return false;
}

static void prepare_answer(const gbs_coffee_t *coffee, bool cancel)
{
    gbs_recipe_follow_cancel();
    canceled = cancel;
    answer = *coffee;
    gbs_uuid(event_uuid);
    pending = true;
    answer_deadline_us = esp_timer_get_time() + (cancel ? 20000000LL : 120000000LL);
    if (coffee->time_ms > ledger.watermark_ms) ledger.watermark_ms = coffee->time_ms;
    strcpy(ledger.last_outcome, cancel ? "pending_cancel" : "pending_result");
    save_ledger();
    ESP_LOGI(TAG, "prepared %s grind=%s event=%s ms=%" PRIu32 " mg=%" PRIu32,
             cancel ? "Canceled" : "Success", current.uuid, event_uuid, coffee->duration_ms, coffee->mass_mg);
}

static void send_answer(void)
{
    if (!storage_ok || !atomic_load(&desired_active)) return;
    if (esp_timer_get_time() > answer_deadline_us) {
        strcpy(issue, "result delivery timed out"); finish_job("delivery_expired"); return;
    }
    if (!accepted && !take_job()) {
        if (!strncmp(api_status, "ClientError", 11) && !gbs_job_error(api_status)) {
            snprintf(issue, sizeof(issue), "brewAccept refused: %.79s", api_status);
            finish_job("delivery_refused");
        }
        return;
    }
    cJSON *reply = NULL;
    esp_err_t err = grinder_call("brewDone", gbs_done_body(gbs_config.pairing_id, &current, &answer,
                                                         event_uuid, canceled), &reply);
    cJSON_Delete(reply);
    if (!strcmp(api_status, "LocalBusy")) return;
    if (err == ESP_OK && strcmp(api_status, "Ok") == 0) {
        if (!canceled) {
            ++ledger.successes;
            ledger.last_duration_ms = answer.duration_ms;
            ledger.last_mass_mg = answer.mass_mg;
        }
        issue[0] = 0;
        finish_job(canceled ? (invalid_coffee ? "invalid_coffee" : "canceled") : "success");
    } else if (err == ESP_OK && strcmp(api_status, "ClientErrorOngoingJobNotExisting") == 0) {
        accepted = false;
        ongoing_missing = true;
    } else {
        // shortcut: ambiguous delivery is not replayed; add retries only after verifying device idempotency.
        snprintf(issue, sizeof(issue), "brewDone %s: %.79s", err == ESP_OK ? "refused" : "outcome unknown", api_status);
        finish_job(err == ESP_OK ? "delivery_refused" : "delivery_unknown");
    }
}

static void jobs_tick(void)
{
    cJSON *request = cJSON_CreateObject(), *reply = NULL;
    if (!request || !cJSON_AddNumberToObject(request, "version", 1) ||
        !cJSON_AddNumberToObject(request, "pairingId", gbs_config.pairing_id)) {
        cJSON_Delete(request); return;
    }
    esp_err_t err = grinder_call("brewJobs", request, &reply);
    if (!strcmp(api_status, "LocalBusy")) { cJSON_Delete(reply); return; }
    if (err == ESP_OK && gbs_grinder_request.http_status == 403 && !gbs_job_error(api_status)) {
        int64_t now = esp_timer_get_time();
        if (pairing_refused_since_us < 0) pairing_refused_since_us = now;
        pairing_problem = now - pairing_refused_since_us >= 60000000LL;
    } else {
        // A timeout or any other response interrupts consecutive refusals.
        pairing_refused_since_us = -1;
        pairing_problem = false;
    }
    if (err != ESP_OK || strcmp(api_status, "Ok")) {
        note_health(&grinder_health, false, &gbs_grinder_request, err, api_status);
        cJSON_Delete(reply); return;
    }
    const cJSON *jobs = cJSON_GetObjectItemCaseSensitive(reply, "brewJobs");
    if (!cJSON_IsArray(jobs) || cJSON_GetArraySize(jobs) > GBS_SEEN_CAP) {
        note_health(&grinder_health, false, &gbs_grinder_request, ESP_ERR_INVALID_RESPONSE, "InvalidJobs");
        cJSON_Delete(reply); return;
    }
    const cJSON *item;
    gbs_job_t job, newest = {0};
    bool fresh = false, listed = false, changed = false;
    cJSON_ArrayForEach(item, jobs) {
        if (!gbs_parse_job(item, &job)) {
            note_health(&grinder_health, false, &gbs_grinder_request, ESP_ERR_INVALID_RESPONSE, "MalformedJob");
            cJSON_Delete(reply); return;
        }
        if (have_job && strcmp(job.uuid, current.uuid) == 0) listed = true;
        if (!gbs_seen(&ledger, job.uuid)) {
            newest = job; fresh = true;
            if (!active || baseline_jobs) { gbs_remember(&ledger, job.uuid); changed = true; }
        }
    }
    note_health(&grinder_health, true, &gbs_grinder_request, ESP_OK, "Ok");
    if (!active || baseline_jobs) {
        if (changed) save_ledger();
        if (baseline_jobs) ESP_LOGI(TAG, "job baseline captured; existing tasks will not be replayed");
        baseline_jobs = false;
        cJSON_Delete(reply); return;
    }
    if (!storage_ok) { cJSON_Delete(reply); return; }
    if (have_job) {
        if (listed) accepted = false;
        if (pending) {
            if (gbs_answer_gone(ongoing_missing, listed)) {
                strcpy(issue, canceled ? "cancellation expired" : "result not delivered: grinder no longer has the task");
                finish_job(canceled ? "canceled" : "delivery_expired");
            } else send_answer();
        }
        else if (fresh) {
            gbs_coffee_t empty = {0};
            prepare_answer(&empty, true);
            send_answer();
        } else if (listed) {
            if (!take_job() && !strncmp(api_status, "ClientError", 11)) {
                snprintf(issue, sizeof(issue), "grinder stopped waiting: %.79s", api_status);
                finish_job("job_disappeared");
            }
        } else accepted = true; // A lost accept reply is reconciled by the next job list, as in PR16.
        cJSON_Delete(reply); return;
    }
    if (!fresh || !cloud_initialized) { cJSON_Delete(reply); return; }
    // A successful baseline is mandatory before any new job can be accepted.
    gbs_coffee_t before;
    err = read_coffee(&before);
    if (err != ESP_OK || !atomic_load(&desired_active)) { cJSON_Delete(reply); return; }
    latest = before;
    int64_t now = gbs_now_ms();
    if (before.time_ms > now + 120000) {
        strcpy(issue, "cloud clock is ahead; job not accepted"); cJSON_Delete(reply); return;
    }
    int idle=0;
    if(!atomic_compare_exchange_strong(&admission,&idle,1)) { cJSON_Delete(reply); return; }
    if (before.time_ms > ledger.watermark_ms) ledger.watermark_ms = before.time_ms;
    if (now > ledger.watermark_ms) ledger.watermark_ms = now;
    cJSON_ArrayForEach(item, jobs) if (gbs_parse_job(item, &job)) gbs_remember(&ledger, job.uuid);
    current = newest;
    memcpy(ledger.inflight, current.uuid, sizeof(ledger.inflight));
    strcpy(ledger.last_outcome, "waiting_for_coffee");
    if (save_ledger()) {
        have_job = true; pending = accepted = ongoing_missing = invalid_coffee = false; issue[0] = 0;
        job_deadline_us = esp_timer_get_time() + 180000000LL;
        gbs_recipe_follow_begin(current.uuid);
        if (take_job() && atomic_load(&desired_active)) {
#ifndef GBS_WORKER_TEST
            publish("waiting_for_coffee");
#endif
            // The baseline was just read; start recipe lookup before another Cloud result poll consumes its window.
            gbs_recipe_follow_tick(true);
        }
    } else atomic_store(&admission,0);
    cJSON_Delete(reply);
}

static void coffee_tick(void)
{
    gbs_coffee_t coffee;
    esp_err_t err = read_coffee(&coffee);
    if (err != ESP_OK) return;
    latest = coffee;
    if (!active || !have_job || pending || !storage_ok || !atomic_load(&desired_active)) return;
    if (coffee.time_ms > gbs_now_ms() + 120000) { strcpy(issue, "cloud timestamp rejected"); return; }
    if (gbs_new_coffee(&coffee, ledger.watermark_ms)) {
        invalid_coffee = false;
        prepare_answer(&coffee, false);
        send_answer();
    } else if (coffee.time_ms > ledger.watermark_ms) {
        // An incomplete record may be corrected under the same timestamp.
        if (!invalid_coffee) ESP_LOGW(TAG, "new cloud coffee invalid or missing measured weight");
        invalid_coffee = true;
        strcpy(issue, "new cloud coffee invalid or missing measured weight");
    }
}

static void scheduled_coffee_tick(int64_t *next_cloud)
{
    int64_t now = esp_timer_get_time();
    if (!cloud_initialized || (now < *next_cloud
        && !(have_job && !pending && *next_cloud > now + 3000000))) return;
    // Leave an overdue idle read due; live jobs and their baselines do not wait.
    if (!have_job && !pending && hub_command_idle_background_deferred()) return;
    coffee_tick();
    *next_cloud = esp_timer_get_time()
        + (have_job && !pending ? 3000000 : cloud_health.ok ? 60000000 : 10000000);
}

#ifndef GBS_WORKER_TEST
static void apply_mode(void)
{
    bool requested = atomic_load(&desired_active);
    if (requested == active) return;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("gbs_cfg", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "active", requested ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) { strcpy(issue, "mode persistence failed"); atomic_store(&desired_active, active); return; }
    if (!strcmp(issue, "mode persistence failed")) issue[0] = 0;
    if (have_job) finish_job("stopped_by_mode_change");
    active = requested;
    baseline_jobs = true;
    ESP_LOGI(TAG, "mode=%s; taking a fresh baseline", active ? "active" : "monitor");
}

static void worker(void *unused)
{
    (void)unused;
    int64_t next_cloud = 0, next_status = 0;
    for (;;) {
        atomic_store(&wifi_online, hub_wifi_station_has_ip());
        apply_mode();
        if(atomic_load(&admission)==2) { publish("maintenance"); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        int64_t now = esp_timer_get_time();
        if (!atomic_load(&wifi_online)) {
            publish("wifi_waiting"); vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        if (gbs_now_ms() < 1735689600000LL) {
            publish("clock_waiting"); vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        if (!cloud_initialized && now >= next_cloud) {
            esp_err_t err = gbs_cloud_connect();
            if (err == ESP_OK) err = read_coffee(&latest);
            else note_health(&cloud_health, false, &gbs_cloud_request, err, "");
            cloud_initialized = err == ESP_OK;
            next_cloud = esp_timer_get_time() + (cloud_initialized ? 60000000 : 10000000);
            if (cloud_initialized) ESP_LOGI(TAG, "cloud connected; latest ms=%" PRIu32 " mg=%" PRIu32,
                                           latest.duration_ms, latest.mass_mg);
        }
        if (now >= next_jobs) {
            jobs_tick();
            unsigned seconds = pairing_problem ? 60 : grinder_health.ok ? (active ? 2 : 5) :
                               gbs_retry_seconds(grinder_health.failures);
            next_jobs = esp_timer_get_time() + (int64_t)seconds * 1000000;
        }
        if (have_job && !pending && esp_timer_get_time() >= job_deadline_us) {
            gbs_coffee_t empty = {0};
            prepare_answer(&empty, true); send_answer();
        }
        scheduled_coffee_tick(&next_cloud);
        publish(!storage_ok ? "storage_error" : pairing_problem ? "pairing_required" :
                pending ? "sending" : have_job ? "waiting_for_coffee" :
                !grinder_health.ok ? "grinder_waiting" : !cloud_health.ok ? "cloud_waiting" : "ready");
        gbs_recipe_follow_tick(active && have_job && accepted && !pending && storage_ok && atomic_load(&desired_active));
        if (now >= next_status) {
            ESP_LOGI(TAG, "health active=%d grinder=%d cloud=%d internal=%u psram=%u stack=%u",
                     active, grinder_health.ok, cloud_health.ok, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(NULL));
            next_status = now + 60000000;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

bool hub_grinder_state(char *buffer, size_t size)
{
    // Failed startup has no worker or writer; report it even if the mutex allocation failed.
    if (atomic_load(&startup_failed)) {
        if (!buffer || strlen(state_json) >= size) return false;
        strcpy(buffer, state_json);
        return true;
    }
    if (!state_mutex || !buffer || xSemaphoreTake(state_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    bool fits = strlen(state_json) < size;
    if (fits) strcpy(buffer, state_json);
    xSemaphoreGive(state_mutex);
    return fits;
}

bool hub_grinder_set_active(bool enabled)
{
    if(!atomic_load(&configured) || (enabled && atomic_load(&admission)==2)) return false;
    if (!enabled) gbs_recipe_follow_cancel();
    atomic_store(&desired_active,enabled); return true;
}

static esp_err_t grinder_startup_failure(const char *stage, esp_err_t error)
{
    unsigned free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char *name = esp_err_to_name(error);
    // No allocations: this path must remain observable when internal RAM is exhausted.
    if (state_mutex) xSemaphoreTake(state_mutex, portMAX_DELAY);
    snprintf(state_json, sizeof(state_json),
             "{\"phase\":\"startup_failed\",\"startup_stage\":\"%s\",\"startup_error\":\"%s\","
             "\"startup_internal_free\":%u,\"startup_largest_internal\":%u,"
             "\"have_job\":false,\"pending\":false,\"baseline_pending\":true,"
             "\"issue_source\":\"startup\",\"issue\":\"Grinder startup failed: %s (%s)\"}",
             stage, name, free_internal, largest_internal, stage, name);
    atomic_store(&configured, false);
    atomic_store(&desired_active, false);
    atomic_store(&startup_failed, true);
    if (state_mutex) xSemaphoreGive(state_mutex);
    ESP_LOGE(TAG, "startup stage=%s error=%s internal_free=%u largest=%u",
             stage, name, free_internal, largest_internal);
    return error;
}

esp_err_t hub_grinder_start(void)
{
    state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex) return grinder_startup_failure("state_mutex", ESP_ERR_NO_MEM);
    strcpy(state_json,"{\"phase\":\"booting\"}");
    esp_err_t err = gbs_load_config();
    if (err != ESP_OK) {
        if (err == ESP_ERR_NO_MEM) return grinder_startup_failure("configuration", err);
        strcpy(state_json, "{\"phase\":\"configuration_required\"}");
        ESP_LOGI(TAG, "Grinder not configured; Hub and touchscreen remain available");
        return ESP_OK;
    }
    err = nvs_open("gbs_run", NVS_READWRITE, &journal_nvs);
    if (err != ESP_OK) return grinder_startup_failure("journal_open", err);
    size_t size = sizeof(ledger);
    err = nvs_get_blob(journal_nvs, "ledger", &ledger, &size);
    restore_ledger(err, size);
    nvs_handle_t settings;
    err = nvs_open("gbs_cfg", NVS_READONLY, &settings);
    if (err != ESP_OK) return grinder_startup_failure("settings_open", err);
    uint8_t saved = 0;
    err = nvs_get_u8(settings, "active", &saved);
    nvs_close(settings);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return grinder_startup_failure("mode_read", err);
    active = saved == 1; atomic_store(&desired_active, active);
    err = gbs_recipe_follow_init();
    if (err != ESP_OK) return grinder_startup_failure("recipe_init", err);
    if (xTaskCreate(worker, "hub_grinder", 12288, NULL, 3, NULL) != pdPASS)
        return grinder_startup_failure("worker_task", ESP_ERR_NO_MEM);
    atomic_store(&configured,true);
    return ESP_OK;
}
#endif
