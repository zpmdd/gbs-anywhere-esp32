// Run the real worker's job/coffee/delivery functions with in-memory services only.
#define GBS_WORKER_TEST
#include "../main/main.c"
#include <assert.h>

gbs_config_t gbs_config;
gbs_request_info_t gbs_grinder_request, gbs_cloud_request;
static int64_t clock_us;
static bool listed_job, ongoing, fail_poll, fail_accept_reply, fail_done_reply, fail_storage, local_busy;
static int accept_calls, done_calls, http_refusal;
static unsigned recipe_begins, recipe_cancels, recipe_ticks, coffee_reads, baseline_reads;
void gbs_recipe_follow_begin(const char *uuid) { assert(uuid && uuid[0]); ++recipe_begins; baseline_reads = coffee_reads; }
void gbs_recipe_follow_cancel(void) { ++recipe_cancels; }
void gbs_recipe_follow_tick(bool allowed) {
    assert(allowed && accepted && have_job && !pending && coffee_reads == baseline_reads);
    ++recipe_ticks;
}
static gbs_coffee_t fake_coffee;
static const char *ID = "11111111-1111-4111-8111-111111111111";

void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_ERR_TIMEOUT"; }
int64_t esp_timer_get_time(void) { return clock_us; }
int64_t gbs_now_ms(void) { return 1800000000000LL + clock_us / 1000; }
void gbs_uuid(char out[37]) { strcpy(out, "22222222-2222-4222-8222-222222222222"); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *d, size_t s)
{ (void)h; (void)k; (void)d; assert(s == sizeof(ledger)); return fail_storage ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t gbs_cloud_coffee(gbs_coffee_t *coffee)
{
    ++coffee_reads;
    strcpy(gbs_cloud_request.operation, "coffee"); gbs_cloud_request.http_status = 200;
    *coffee = fake_coffee; return ESP_OK;
}
esp_err_t gbs_grinder(const char *endpoint, const cJSON *body, cJSON **reply, char status[80])
{
    strcpy(gbs_grinder_request.operation, endpoint); gbs_grinder_request.http_status = 200;
    strcpy(status, "Ok"); *reply = cJSON_CreateObject(); assert(*reply);
    if (local_busy) { strcpy(status, "LocalBusy"); return ESP_ERR_TIMEOUT; }
    if (!strcmp(endpoint, "brewJobs")) {
        if (fail_poll) { gbs_grinder_request.http_status = http_refusal; strcpy(status, "TransportError"); return ESP_ERR_TIMEOUT; }
        if (http_refusal) { gbs_grinder_request.http_status = http_refusal; strcpy(status, "ClientErrorPairingIdNotExisting"); return ESP_OK; }
        cJSON *jobs = cJSON_AddArrayToObject(*reply, "brewJobs");
        if (listed_job) {
            cJSON *job = cJSON_CreateObject(); cJSON_AddItemToArray(jobs, job);
            cJSON_AddStringToObject(job, "grindUuid", ID);
            cJSON_AddNumberToObject(job, "doseTypeExpected", 2);
            cJSON_AddNumberToObject(job, "durationExpectedMs", 30000);
        }
    } else if (!strcmp(endpoint, "brewAccept")) {
        ++accept_calls;
        if (!listed_job) strcpy(status, "ClientErrorBrewJobNotExisting");
        else { listed_job = false; ongoing = true; }
        if (fail_accept_reply) { strcpy(status, "TransportError"); return ESP_ERR_TIMEOUT; }
    } else {
        assert(!strcmp(endpoint, "brewDone")); ++done_calls;
        assert(!strcmp(cJSON_GetObjectItem(body, "grindUuid")->valuestring, ID));
        assert(!strcmp(cJSON_GetObjectItem(body, "brewEventUuid")->valuestring, "22222222-2222-4222-8222-222222222222"));
        if (!ongoing) strcpy(status, "ClientErrorOngoingJobNotExisting");
        else ongoing = false;
        if (fail_done_reply) { strcpy(status, "TransportError"); return ESP_ERR_TIMEOUT; }
    }
    return ESP_OK;
}

static void reset(void)
{
    atomic_store(&admission,0);
    memset(&ledger, 0, sizeof(ledger)); ledger.version = 1;
    memset(&grinder_health, 0, sizeof(grinder_health)); memset(&cloud_health, 0, sizeof(cloud_health));
    memset(&latest, 0, sizeof(latest)); memset(&current, 0, sizeof(current));
    active = cloud_initialized = storage_ok = true;
    atomic_store(&desired_active, true); atomic_store(&wifi_online, true);
    baseline_jobs = have_job = pending = accepted = canceled = ongoing_missing = pairing_problem = invalid_coffee = local_busy = false;
    listed_job = ongoing = fail_poll = fail_accept_reply = fail_done_reply = fail_storage = false;
    next_jobs = job_deadline_us = answer_deadline_us = 0;
    pairing_refused_since_us = -1;
    clock_us = 1000000; accepts = 0; accept_calls = done_calls = http_refusal = 0;
    recipe_begins = recipe_cancels = recipe_ticks = coffee_reads = baseline_reads = 0;
    gbs_config.pairing_id = 42;
    issue[0] = storage_issue[0] = 0;
    fake_coffee = (gbs_coffee_t){.time_ms = 1799999990000LL, .duration_ms = 30000, .mass_mg = 30000, .usable = true};
}
static void measured(void)
{
    clock_us += 45000000;
    fake_coffee.time_ms = gbs_now_ms(); fake_coffee.duration_ms = 33052; fake_coffee.mass_mg = 29800;
    coffee_tick();
}
int main(void)
{
    atomic_store(&admission,0);
    assert(hub_grinder_begin_maintenance());
    assert(!hub_grinder_begin_maintenance());
    hub_grinder_end_maintenance();
    assert(hub_grinder_begin_recipe_probe());
    assert(!hub_grinder_begin_maintenance());
    hub_grinder_end_recipe_probe();
    atomic_store(&admission,1);
    assert(!hub_grinder_begin_maintenance());
    atomic_store(&admission,0);
    reset(); listed_job = true;
    assert(hub_grinder_begin_maintenance());
    jobs_tick();
    assert(!have_job && accept_calls == 0 && !gbs_seen(&ledger, ID) && ledger.watermark_ms == 0);
    hub_grinder_end_maintenance();
    jobs_tick();
    assert(have_job && accept_calls == 1 && !hub_grinder_begin_maintenance());
    assert(recipe_begins == 1 && recipe_ticks == 1 && baseline_reads > 0);
    measured();
    assert(recipe_begins == 1 && recipe_cancels > 0);
    assert(hub_grinder_begin_maintenance());
    hub_grinder_end_maintenance();
    reset(); listed_job = true; jobs_tick();
    cJSON *display = cJSON_CreateObject();
    add_display_state(display);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display, "have_job")));
    assert(cJSON_GetObjectItemCaseSensitive(display, "job_deadline_ms")->valuedouble == job_deadline_us / 1000);
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display, "pending")));
    cJSON_Delete(display);
    gbs_coffee_t empty = {0}; prepare_answer(&empty, true);
    display = cJSON_CreateObject(); add_display_state(display);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display, "canceling")));
    assert(cJSON_GetObjectItemCaseSensitive(display, "answer_deadline_ms")->valuedouble == answer_deadline_us / 1000);
    assert(cJSON_GetObjectItemCaseSensitive(display, "job_deadline_ms")->valuedouble == 0);
    cJSON_Delete(display);
    reset();
    fail_poll = true; for (int i = 0; i < 100; ++i) jobs_tick();
    assert(!grinder_health.ok && grinder_health.failures == 100 && !have_job);
    fail_poll = false; listed_job = true; jobs_tick();
    assert(have_job && accepted && accept_calls == 1 && !grinder_health.error[0]);
    assert(grinder_health.recoveries == 1 && grinder_health.last_error[0]);
    measured(); assert(done_calls == 1 && ledger.successes == 1 && !have_job);
    assert(ledger.last_duration_ms == 33052 && ledger.last_mass_mg == 29800);
    listed_job = true; jobs_tick(); assert(!have_job && accept_calls == 1 && recipe_ticks == 1); // No replay.

    reset(); baseline_jobs = true; listed_job = true; jobs_tick();
    assert(!have_job && accept_calls == 0 && recipe_ticks == 0 && gbs_seen(&ledger, ID)); // Restart skips stale jobs.

    reset(); listed_job = true; local_busy = true; jobs_tick();
    assert(!have_job && accept_calls == 0 && grinder_health.failures == 0);
    local_busy = false; jobs_tick(); local_busy = true; measured();
    assert(pending && accepted && done_calls == 0 && grinder_health.failures == 0);
    local_busy = false; jobs_tick();
    assert(!have_job && done_calls == 1 && ledger.successes == 1);

    reset(); listed_job = true; jobs_tick();
    int64_t baseline = ledger.watermark_ms;
    clock_us += 45000000; fake_coffee.time_ms = gbs_now_ms(); fake_coffee.usable = false;
    coffee_tick();
    assert(invalid_coffee && have_job && !pending && done_calls == 0 && ledger.watermark_ms == baseline);
    fake_coffee.usable = true; coffee_tick(); // Cloud corrected the same record.
    assert(!invalid_coffee && !have_job && done_calls == 1 && ledger.successes == 1);
    coffee_tick(); assert(done_calls == 1);

    reset(); listed_job = true; jobs_tick();
    clock_us += 45000000; fake_coffee.time_ms = gbs_now_ms(); fake_coffee.usable = false;
    coffee_tick(); prepare_answer(&empty, true); send_answer();
    assert(!have_job && done_calls == 1 && ledger.successes == 0 && !strcmp(ledger.last_outcome, "invalid_coffee"));

    reset(); listed_job = true; jobs_tick();
    listed_job = true; jobs_tick(); // The grinder handed a waiting job back out.
    assert(accept_calls == 2 && done_calls == 0);
    measured(); assert(done_calls == 1 && ledger.successes == 1);

    reset(); listed_job = true; fail_accept_reply = true; jobs_tick();
    assert(have_job && !accepted && accept_calls == 1 && recipe_ticks == 0);
    fail_accept_reply = false; jobs_tick(); assert(accepted);
    measured(); assert(accept_calls == 1 && done_calls == 1 && ledger.successes == 1);

    reset(); listed_job = true; fail_accept_reply = true; jobs_tick();
    fail_poll = true; jobs_tick(); fail_poll = false;
    measured(); // The result arrives before a successful job-list reconciliation.
    assert(pending && !accepted && ongoing && done_calls == 0);
    fail_accept_reply = false; jobs_tick();
    assert(!have_job && !ongoing && done_calls == 1 && ledger.successes == 1);

    reset(); listed_job = true; fail_accept_reply = true; jobs_tick();
    measured(); assert(pending && !accepted);
    ongoing = false; fail_accept_reply = false; jobs_tick();
    assert(pending && ongoing_missing && done_calls == 1);
    jobs_tick(); assert(!have_job && !strcmp(ledger.last_outcome, "delivery_expired"));

    reset(); listed_job = true; jobs_tick(); fail_done_reply = true; measured();
    assert(!have_job && !strcmp(ledger.last_outcome, "delivery_unknown"));
    display = cJSON_CreateObject(); add_display_state(display);
    assert(cJSON_GetObjectItemCaseSensitive(display, "unconfirmed_grams")->valuedouble == answer.mass_mg / 1000.0);
    cJSON_Delete(display);
    jobs_tick(); coffee_tick(); assert(done_calls == 1 && ledger.successes == 0);

    reset(); listed_job = true; jobs_tick(); ongoing = false; measured();
    assert(pending && !accepted); jobs_tick();
    assert(!have_job && !strcmp(ledger.last_outcome, "delivery_expired") && done_calls == 1);

    reset(); listed_job = true; jobs_tick(); ongoing = false; measured();
    assert(pending && ongoing_missing);
    listed_job = true; fail_accept_reply = true; jobs_tick();
    assert(pending && ongoing && !ongoing_missing);
    fail_accept_reply = false; jobs_tick();
    assert(!have_job && ledger.successes == 1 && done_calls == 2);

    reset(); listed_job = true; fail_accept_reply = true; jobs_tick(); measured();
    clock_us = answer_deadline_us + 1; fail_accept_reply = false; jobs_tick();
    assert(!have_job && done_calls == 0 && !strcmp(ledger.last_outcome, "delivery_expired"));

    reset(); http_refusal = 403; jobs_tick();
    clock_us += 2000000; jobs_tick(); clock_us += 2000000; jobs_tick();
    assert(!pairing_problem && gbs_config.pairing_id == 42);
    clock_us += 56000000; jobs_tick();
    assert(pairing_problem && gbs_config.pairing_id == 42);
    fail_poll = true; jobs_tick();
    assert(!pairing_problem && pairing_refused_since_us == -1);
    fail_poll = false; jobs_tick(); clock_us += 59000000; jobs_tick();
    assert(!pairing_problem);
    clock_us += 1000000; jobs_tick(); assert(pairing_problem);
    http_refusal = 0; jobs_tick();
    assert(!pairing_problem && pairing_refused_since_us == -1 && grinder_health.ok);

    reset(); listed_job = true; fail_storage = true; jobs_tick();
    assert(!storage_ok && !have_job && accept_calls == 0 && done_calls == 0);
    puts("PASS: worker recovery, pending/lost acceptance, relisted and expired results, ambiguous delivery, timed refusals, NVS failure");
}
