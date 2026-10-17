#include "network.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GBS_WORKER_TEST
#include "../tests/worker_platform.h"
#else
#include "esp_console.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

static const char *TAG = "gbs";
static atomic_bool wifi_online, desired_active;
#ifndef GBS_WORKER_TEST
static SemaphoreHandle_t state_mutex;
static char state_json[6144] = "{\"phase\":\"booting\"}";
static esp_netif_t *station;
#endif
static nvs_handle_t journal_nvs;
static gbs_ledger_t ledger;
static bool active, cloud_initialized, have_job, pending, accepted, canceled, ongoing_missing;
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

#ifndef GBS_WORKER_TEST
static void publish(const char *phase)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return;
    cJSON_AddStringToObject(j, "firmware", "gbs-bridge 0.2.1");
    cJSON_AddStringToObject(j, "upstream", "gbs-anywhere 3adf778 / cloud ba00c92");
    cJSON_AddStringToObject(j, "build", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "mode", active ? "active" : "monitor");
    cJSON_AddStringToObject(j, "phase", phase);
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

static void finish_job(const char *outcome)
{
    snprintf(ledger.last_outcome, sizeof(ledger.last_outcome), "%s", outcome);
    ledger.inflight[0] = 0;
    save_ledger();
    have_job = pending = accepted = ongoing_missing = false;
    ESP_LOGI(TAG, "job ended: %s", outcome);
}

static esp_err_t grinder_call(const char *endpoint, cJSON *body, cJSON **reply)
{
    if (!body) { strcpy(api_status, "NoMemory"); return ESP_ERR_NO_MEM; }
    esp_err_t err = gbs_grinder(endpoint, body, reply, api_status);
    cJSON_Delete(body);
    if (strcmp(endpoint, "brewJobs"))
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
    if (err == ESP_OK && strcmp(api_status, "Ok") == 0) {
        if (!canceled) {
            ++ledger.successes;
            ledger.last_duration_ms = answer.duration_ms;
            ledger.last_mass_mg = answer.mass_mg;
        }
        issue[0] = 0;
        finish_job(canceled ? "canceled" : "success");
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
    if (before.time_ms > ledger.watermark_ms) ledger.watermark_ms = before.time_ms;
    if (now > ledger.watermark_ms) ledger.watermark_ms = now;
    cJSON_ArrayForEach(item, jobs) if (gbs_parse_job(item, &job)) gbs_remember(&ledger, job.uuid);
    current = newest;
    memcpy(ledger.inflight, current.uuid, sizeof(ledger.inflight));
    strcpy(ledger.last_outcome, "waiting_for_coffee");
    if (save_ledger()) {
        have_job = true; pending = accepted = ongoing_missing = false; issue[0] = 0;
        job_deadline_us = esp_timer_get_time() + 180000000LL;
        take_job();
    }
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
        prepare_answer(&coffee, false);
        send_answer();
    } else if (coffee.time_ms > ledger.watermark_ms) {
        ledger.watermark_ms = coffee.time_ms;
        save_ledger();
        strcpy(issue, "new cloud coffee invalid or missing measured weight");
        ESP_LOGW(TAG, "%s", issue);
    }
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
    if (have_job) finish_job("stopped_by_mode_change");
    active = requested;
    baseline_jobs = true;
    ESP_LOGI(TAG, "mode=%s; taking a fresh baseline", active ? "active" : "monitor");
}

static void worker(void *unused)
{
    (void)unused;
    bool sntp_started = false;
    int64_t next_cloud = 0, next_reconnect = 0, next_status = 0;
    for (;;) {
        apply_mode();
        int64_t now = esp_timer_get_time();
        if (!atomic_load(&wifi_online)) {
            if (now >= next_reconnect) { esp_wifi_connect(); next_reconnect = now + 5000000; }
            publish("wifi_waiting"); vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        if (!sntp_started) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_err_t err = esp_netif_sntp_init(&config);
            if (err != ESP_OK) { snprintf(issue, sizeof(issue), "SNTP: %s", esp_err_to_name(err)); }
            else { sntp_started = true; if (!strncmp(issue, "SNTP:", 5)) issue[0] = 0; }
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
        // Idle reads refresh the display; pending shots always use the faster cloud poll.
        if (cloud_initialized && (now >= next_cloud || (have_job && !pending && next_cloud > now + 3000000))) {
            coffee_tick();
            next_cloud = esp_timer_get_time() + (have_job && !pending ? 3000000 : cloud_health.ok ? 60000000 : 10000000);
        }
        publish(!storage_ok ? "storage_error" : pairing_problem ? "pairing_required" :
                pending ? "sending" : have_job ? "waiting_for_coffee" :
                !grinder_health.ok ? "grinder_waiting" : !cloud_health.ok ? "cloud_waiting" : "ready");
        if (now >= next_status) {
            ESP_LOGI(TAG, "health active=%d grinder=%d cloud=%d internal=%u psram=%u stack=%u",
                     active, grinder_health.ok, cloud_health.ok, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(NULL));
            next_status = now + 60000000;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) atomic_store(&wifi_online, false);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&wifi_online, true);
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "status page: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t status_page(httpd_req_t *request)
{
    char *text = malloc(sizeof(state_json));
    if (!text) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    strcpy(text, state_json);
    xSemaphoreGive(state_mutex);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, text);
    free(text);
    return err;
}

static esp_err_t root_page(httpd_req_t *request)
{
    extern const char index_start[] asm("_binary_index_html_start");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request, index_start);
}

static int command_mode(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "active") && strcmp(argv[1], "monitor"))) {
        puts("mode monitor | mode active (stop the computer bridge first)"); return 1;
    }
    atomic_store(&desired_active, strcmp(argv[1], "active") == 0);
    puts("Mode requested; inspect status for completion.");
    return 0;
}

static int command_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    puts(state_json);
    xSemaphoreGive(state_mutex);
    return 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "gbs-anywhere ESP32-S3 0.2.1; upstream 3adf778; reset_reason=%d", esp_reset_reason());
    ESP_ERROR_CHECK(nvs_flash_init()); // Never erase a damaged config automatically.
    ESP_ERROR_CHECK(gbs_load_config());
    ESP_ERROR_CHECK(gbs_crypto_init());
    ESP_ERROR_CHECK(nvs_open("gbs_run", NVS_READWRITE, &journal_nvs));
    size_t size = sizeof(ledger);
    esp_err_t err = nvs_get_blob(journal_nvs, "ledger", &ledger, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) { memset(&ledger, 0, sizeof(ledger)); ledger.version = 1; }
    else if (err != ESP_OK || size != sizeof(ledger) || !gbs_ledger_valid(&ledger)) {
        memset(&ledger, 0, sizeof(ledger));
        storage_ok = false; strcpy(storage_issue, "invalid journal; result delivery disabled");
    }
    if (storage_ok && ledger.inflight[0]) {
        // A reboot never replays an unfinished cup into a later grind.
        gbs_remember(&ledger, ledger.inflight);
        strcpy(ledger.last_outcome, "interrupted_by_restart");
        ledger.inflight[0] = 0; save_ledger();
    }
    nvs_handle_t settings;
    ESP_ERROR_CHECK(nvs_open("gbs_cfg", NVS_READONLY, &settings));
    uint8_t saved = 0;
    err = nvs_get_u8(settings, "active", &saved);
    ESP_ERROR_CHECK(err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err);
    nvs_close(settings);
    active = saved == 1; atomic_store(&desired_active, active);
    state_mutex = xSemaphoreCreateMutex();
    assert(state_mutex);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station = esp_netif_create_default_wifi_sta();
    assert(station);
    ESP_ERROR_CHECK(esp_netif_set_hostname(station, "gbs-esp32"));
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t credentials = {0};
    memcpy(credentials.sta.ssid, gbs_config.wifi_ssid, strlen(gbs_config.wifi_ssid));
    memcpy(credentials.sta.password, gbs_config.wifi_password, strlen(gbs_config.wifi_password));
    credentials.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    credentials.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &credentials));
    memset(&credentials, 0, sizeof(credentials));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    httpd_handle_t server;
    httpd_config_t web = HTTPD_DEFAULT_CONFIG();
    web.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &web));
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_page};
    const httpd_uri_t status = {.uri = "/api/state", .method = HTTP_GET, .handler = status_page};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));

    esp_console_repl_t *repl;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "gbs>"; repl_config.max_cmdline_length = 64;
    esp_console_dev_usb_serial_jtag_config_t usb = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb, &repl_config, &repl));
    const esp_console_cmd_t mode_cmd = {.command = "mode", .help = "monitor or active; active requires the PC bridge to be stopped", .func = command_mode};
    const esp_console_cmd_t status_cmd = {.command = "status", .help = "Read bridge state", .func = command_status};
    ESP_ERROR_CHECK(esp_console_cmd_register(&mode_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    assert(xTaskCreate(worker, "gbs_worker", 24576, NULL, 5, NULL) == pdPASS);
}
#endif
