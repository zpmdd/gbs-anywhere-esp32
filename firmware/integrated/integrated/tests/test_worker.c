// Run the real worker's job/coffee/delivery functions with in-memory services only.
#define GBS_WORKER_TEST
#include "../main/main.c"
#include <assert.h>

gbs_config_t gbs_config;
gbs_request_info_t gbs_grinder_request, gbs_cloud_request;
static int64_t clock_us;
static bool listed_job, ongoing, fail_poll, fail_accept_reply, fail_done_reply, fail_storage, local_busy;
static bool fail_commit, fail_commit_after_done, stored, staged;
static unsigned commits;
static gbs_ledger_t durable_ledger, staged_ledger;
static int64_t wall_epoch;
static int accept_calls, done_calls, http_refusal;
static unsigned recipe_begins, recipe_cancels, recipe_ticks, coffee_reads, baseline_reads;
static bool idle_background_defer;
static unsigned idle_defer_queries;
bool hub_command_idle_background_deferred(void) { ++idle_defer_queries; return idle_background_defer; }
void gbs_recipe_follow_begin(const char *uuid) { assert(uuid && uuid[0]); ++recipe_begins; baseline_reads = coffee_reads; }
void gbs_recipe_follow_cancel(void) { ++recipe_cancels; }
void gbs_recipe_follow_tick(bool allowed) {
    assert(allowed && accepted && have_job && !pending && coffee_reads == baseline_reads);
    ++recipe_ticks;
}
static gbs_coffee_t fake_coffee;
static const char *ID = "11111111-1111-4111-8111-111111111111";
static const char *NEW_ID = "33333333-3333-4333-8333-333333333333";

void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_ERR_TIMEOUT"; }
int64_t esp_timer_get_time(void) { return clock_us; }
int64_t gbs_now_ms(void) { return wall_epoch + clock_us / 1000; }
void gbs_uuid(char out[37]) { strcpy(out, "22222222-2222-4222-8222-222222222222"); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *d, size_t s)
{
    (void)h; assert(!strcmp(k,"ledger") && s == sizeof(ledger));
    if (fail_storage) return ESP_FAIL;
    memcpy(&staged_ledger,d,s); staged=true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h; ++commits; if (fail_commit) return ESP_FAIL;
    if (staged) { durable_ledger=staged_ledger; stored=true; staged=false; }
    return ESP_OK;
}
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
        assert(stored&&gbs_seen(&durable_ledger,ID)&&!strcmp(durable_ledger.inflight,ID));
        ++accept_calls;
        if (!listed_job) strcpy(status, "ClientErrorBrewJobNotExisting");
        else { listed_job = false; ongoing = true; }
        if (fail_accept_reply) { strcpy(status, "TransportError"); return ESP_ERR_TIMEOUT; }
    } else {
        assert(!strcmp(endpoint, "brewDone")); ++done_calls;
        assert(stored&&gbs_seen(&durable_ledger,ID)&&!strcmp(durable_ledger.inflight,ID));
        assert(!strcmp(durable_ledger.last_outcome,"pending_result")||!strcmp(durable_ledger.last_outcome,"pending_cancel"));
        assert(durable_ledger.watermark_ms>=answer.time_ms);
        assert(!strcmp(cJSON_GetObjectItem(body, "grindUuid")->valuestring, ID));
        assert(!strcmp(cJSON_GetObjectItem(body, "brewEventUuid")->valuestring, "22222222-2222-4222-8222-222222222222"));
        if (!ongoing) strcpy(status, "ClientErrorOngoingJobNotExisting");
        else ongoing = false;
        if (fail_commit_after_done) fail_commit=true;
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
    memset(&answer, 0, sizeof(answer)); event_uuid[0]=0;
    memset(&last_result, 0, sizeof(last_result)); last_job_uuid[0]=0;
    active = cloud_initialized = storage_ok = true;
    atomic_store(&desired_active, true); atomic_store(&wifi_online, true);
    baseline_jobs = have_job = pending = accepted = canceled = ongoing_missing = pairing_problem = invalid_coffee = local_busy = false;
    listed_job = ongoing = fail_poll = fail_accept_reply = fail_done_reply = fail_storage = false;
    fail_commit=fail_commit_after_done=stored=staged=false;
    commits=0;
    next_jobs = job_deadline_us = answer_deadline_us = 0;
    pairing_refused_since_us = -1;
    clock_us = 1000000; wall_epoch=1800000000000LL;
    accepts = 0; accept_calls = done_calls = http_refusal = 0;
    recipe_begins = recipe_cancels = recipe_ticks = coffee_reads = baseline_reads = 0;
    idle_background_defer = false; idle_defer_queries = 0;
    gbs_config.pairing_id = 42;
    issue[0] = storage_issue[0] = 0;
    fake_coffee = (gbs_coffee_t){.time_ms = 1799999990000LL, .duration_ms = 30000, .mass_mg = 30000, .usable = true};
}
static void cold_boot(void)
{
    gbs_ledger_t persisted=durable_ledger;
    bool had_record=stored, remote_listed=listed_job, remote_ongoing=ongoing;
    gbs_coffee_t remote_coffee=fake_coffee;
    int64_t wall_now=gbs_now_ms()+3000;
    reset();
    wall_epoch=wall_now-clock_us/1000;
    listed_job=remote_listed; ongoing=remote_ongoing; fake_coffee=remote_coffee;
    durable_ledger=persisted; stored=had_record;
    if (had_record) ledger=persisted;
    baseline_jobs=true; cloud_initialized=false;
    restore_ledger(had_record?ESP_OK:ESP_ERR_NVS_NOT_FOUND,sizeof(ledger));
    assert(!have_job&&!pending&&!accepted&&!answer.usable&&!current.uuid[0]);
    assert(!cloud_health.ok&&!grinder_health.ok&&!latest.time_ms);
    assert(!accept_calls&&!done_calls&&!recipe_begins&&!recipe_ticks);
    cJSON *display=cJSON_CreateObject(); add_display_state(display);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"baseline_pending")));
    assert(cJSON_GetObjectItemCaseSensitive(display,"job_deadline_ms")->valuedouble==0);
    assert(cJSON_GetObjectItemCaseSensitive(display,"answer_deadline_ms")->valuedouble==0);
    assert(!cJSON_GetObjectItemCaseSensitive(display,"unconfirmed_grams"));
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
    assert(!cJSON_GetObjectItemCaseSensitive(display,"flow_job_uuid")->valuestring[0]);
    cJSON_Delete(display);
}
static void measured(void);
static void boot_recovery(void)
{
    for(unsigned checkpoint=0;checkpoint<5;++checkpoint) {
        reset(); listed_job=true;
        if(checkpoint) jobs_tick(); // The accept is preceded by a committed seen/inflight record.
        if(checkpoint==2) {
            clock_us+=45000000;
            gbs_coffee_t result={.time_ms=gbs_now_ms(),.duration_ms=31000,.mass_mg=30000,.usable=true};
            prepare_answer(&result,false); assert(pending&&done_calls==0);
        } else if(checkpoint==3) {
            fail_commit_after_done=true; measured();
            assert(done_calls==1&&!storage_ok&&!strcmp(durable_ledger.last_outcome,"pending_result"));
        } else if(checkpoint==4) {
            fail_done_reply=true; measured();
            assert(done_calls==1&&!strcmp(durable_ledger.last_outcome,"delivery_unknown"));
        }
        cold_boot();
        assert(storage_ok);
        if(checkpoint&&checkpoint<4) assert(!strcmp(ledger.last_outcome,"interrupted_by_restart"));
        if(checkpoint==4) assert(!strcmp(ledger.last_outcome,"delivery_unknown"));
        listed_job=true; jobs_tick(); // Even a relisted pre-boot job is only a baseline.
        assert(!baseline_jobs&&gbs_seen(&ledger,ID)&&!have_job);
        coffee_tick(); jobs_tick();
        assert(!accept_calls&&!done_calls&&!recipe_begins&&!recipe_ticks);
        cloud_initialized=true;
        jobs_tick(); assert(!have_job&&!accept_calls&&!done_calls);
        const char *old_id=ID; ID=NEW_ID; listed_job=true; jobs_tick();
        assert(have_job&&accepted&&accept_calls==1&&recipe_begins==1&&recipe_ticks==1);
        coffee_tick(); assert(!done_calls); // The old Cloud coffee is below the fresh acceptance watermark.
        measured(); assert(done_calls==1&&ledger.successes==1&&!have_job);
        ID=old_id;
    }
    reset(); listed_job=true; jobs_tick(); measured();
    assert(ledger.successes==1&&!strcmp(ledger.last_outcome,"success"));
    cJSON *display=cJSON_CreateObject(); add_display_state(display);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(display,"flow_job_uuid")->valuestring,ID));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
    assert(cJSON_GetObjectItemCaseSensitive(display,"flow_result_grams")->valuedouble==29.8);
    cJSON_Delete(display);
    const char *completed_id=ID; ID=NEW_ID; listed_job=true; jobs_tick();
    assert(have_job);
    display=cJSON_CreateObject(); add_display_state(display);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(display,"flow_job_uuid")->valuestring,NEW_ID));
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
    cJSON_Delete(display);
    ID=completed_id;
    reset(); listed_job=true; jobs_tick(); measured();
    cold_boot();
    assert(ledger.successes==1&&ledger.last_duration_ms==33052&&ledger.last_mass_mg==29800);
    assert(!ledger.last_outcome[0]&&gbs_seen(&ledger,ID)); // History survives without a current-cup success.
    assert(commits==0); // Clearing the current-cup display state does not add a Flash write.
    reset(); ledger.version=99;
    restore_ledger(ESP_OK,sizeof(ledger)); listed_job=true; jobs_tick();
    assert(!storage_ok&&!accept_calls&&!done_calls&&!recipe_begins);
    reset(); listed_job=true; jobs_tick();
    ledger=durable_ledger; fail_commit=true;
    restore_ledger(ESP_OK,sizeof(ledger));
    assert(!storage_ok&&!strcmp(ledger.last_outcome,"interrupted_by_restart"));
    int old_accepts=accept_calls; listed_job=true; jobs_tick();
    assert(accept_calls==old_accepts&&!done_calls);
}
static void measured(void)
{
    clock_us += 45000000;
    fake_coffee.time_ms = gbs_now_ms(); fake_coffee.duration_ms = 33052; fake_coffee.mass_mg = 29800;
    coffee_tick();
}
static void idle_coffee_scheduling(void)
{
    reset(); idle_background_defer=true;
    int64_t due=clock_us-123, original_due=due;
    gbs_health_t before=cloud_health;
    scheduled_coffee_tick(&due); clock_us+=1000000; scheduled_coffee_tick(&due);
    assert(due==original_due&&coffee_reads==0&&idle_defer_queries==2);
    assert(!memcmp(&before,&cloud_health,sizeof(before))&&!recipe_ticks&&!commits);
    idle_background_defer=false; scheduled_coffee_tick(&due);
    assert(coffee_reads==1&&due==clock_us+60000000&&cloud_health.ok);
    unsigned queries=idle_defer_queries;
    scheduled_coffee_tick(&due); assert(coffee_reads==1&&idle_defer_queries==queries);

    reset(); idle_background_defer=true; have_job=true; accepted=false;
    ledger.watermark_ms=gbs_now_ms(); due=clock_us+60000000;
    scheduled_coffee_tick(&due);
    assert(coffee_reads==1&&idle_defer_queries==0&&due==clock_us+3000000);
    clock_us+=2999999; scheduled_coffee_tick(&due); assert(coffee_reads==1);
    accepted=true;
    ++clock_us; scheduled_coffee_tick(&due);
    assert(coffee_reads==2&&idle_defer_queries==0&&due==clock_us+3000000);
    pending=true; due=clock_us; scheduled_coffee_tick(&due);
    assert(coffee_reads==3&&idle_defer_queries==0&&due==clock_us+60000000);

    reset(); idle_background_defer=true; cloud_initialized=false; due=0;
    scheduled_coffee_tick(&due);
    assert(coffee_reads==0&&idle_defer_queries==0&&due==0);
    assert(read_coffee(&latest)==ESP_OK&&coffee_reads==1&&idle_defer_queries==0);
    // Job admission takes its own fresh baseline and starts recipe following immediately.
    cloud_initialized=true; listed_job=true; jobs_tick();
    assert(have_job&&accepted&&coffee_reads==2&&idle_defer_queries==0&&recipe_ticks==1);
}
int main(void)
{
    idle_coffee_scheduling();
    boot_recovery();
    // Background battery acquisition observes admission without reserving it or changing worker state.
    for(int state=0;state<=3;++state) {
        atomic_store(&admission,state);
        assert(hub_grinder_busy()==(state!=0));
        assert(atomic_load(&admission)==state);
        assert(hub_grinder_busy()==(state!=0));
        assert(atomic_load(&admission)==state);
    }
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
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(display,"flow_job_uuid")->valuestring,ID));
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
    cJSON_Delete(display);
    gbs_coffee_t empty = {0}; prepare_answer(&empty, true);
    display = cJSON_CreateObject(); add_display_state(display);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display, "canceling")));
    assert(cJSON_GetObjectItemCaseSensitive(display, "answer_deadline_ms")->valuedouble == answer_deadline_us / 1000);
    assert(cJSON_GetObjectItemCaseSensitive(display, "job_deadline_ms")->valuedouble == 0);
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
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
    latest.mass_mg = 61200; latest.duration_ms = 40500;
    display = cJSON_CreateObject(); add_display_state(display);
    assert(cJSON_GetObjectItemCaseSensitive(display, "unconfirmed_grams")->valuedouble == 29.8);
    assert(cJSON_GetObjectItemCaseSensitive(display, "unconfirmed_seconds")->valuedouble == 33.052);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
    assert(cJSON_GetObjectItemCaseSensitive(display,"flow_result_grams")->valuedouble==29.8);
    cJSON_Delete(display);
    canceled = true;
    display = cJSON_CreateObject(); add_display_state(display);
    assert(!cJSON_GetObjectItemCaseSensitive(display, "unconfirmed_grams"));
    assert(!cJSON_GetObjectItemCaseSensitive(display, "unconfirmed_seconds"));
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
    cJSON_Delete(display); canceled = false;
    local_busy = false; jobs_tick();
    assert(!have_job && done_calls == 1 && ledger.successes == 1);
    display = cJSON_CreateObject(); add_display_state(display);
    assert(!cJSON_GetObjectItemCaseSensitive(display, "unconfirmed_grams"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(display,"flow_job_uuid")->valuestring,ID));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display,"flow_result_valid")));
    assert(cJSON_GetObjectItemCaseSensitive(display,"flow_result_grams")->valuedouble==29.8);
    assert(cJSON_GetObjectItemCaseSensitive(display,"flow_result_seconds")->valuedouble==33.052);
    cJSON_Delete(display);

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
    puts("PASS: idle-read defer preserves due/health and live baselines, cold-boot dedup/no replay, fresh next cup, worker recovery, ambiguous delivery and NVS failure");
}
