#include "gbs_ui_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void boiler_warmup(void)
{
    const int64_t now=1792716000;
    gbs_ui_machine_t m={.available=true,.machine_mode=1,.steam_enabled=true,.steam_enabled_known=true,
        .coffee_ready_unix=now+241,.steam_ready_unix=now+421,.boiler_observed_unix=now};
    assert(gbs_ui_boiler_warmup_minutes(&m,false,now)==5);
    assert(gbs_ui_boiler_warmup_minutes(&m,true,now)==8);
    assert(gbs_ui_boiler_warmup_minutes(&m,false,now+60)==4);
    assert(gbs_ui_boiler_warmup_minutes(&m,false,now+240)==1);
    assert(!gbs_ui_boiler_warmup_minutes(&m,false,now+241));
    assert(gbs_ui_boiler_warmup_minutes(&m,true,now+299)==3);
    assert(!gbs_ui_boiler_warmup_minutes(&m,true,now+300));
    assert(!gbs_ui_boiler_warmup_minutes(&m,true,now-1));
    assert(!gbs_ui_boiler_warmup_minutes(&m,true,0));
    m.steam_enabled=false; assert(!gbs_ui_boiler_warmup_minutes(&m,true,now));
    assert(gbs_ui_boiler_warmup_minutes(&m,false,now)==5);
    m.steam_enabled_known=false; assert(gbs_ui_boiler_warmup_minutes(&m,true,now)==8);
    m.stale=true; assert(!gbs_ui_boiler_warmup_minutes(&m,false,now)); m.stale=false;
    m.available=false; assert(!gbs_ui_boiler_warmup_minutes(&m,false,now)); m.available=true;
    m.machine_mode=0; assert(!gbs_ui_boiler_warmup_minutes(&m,false,now)); m.machine_mode=GBS_UI_VALUE_UNKNOWN;
    assert(gbs_ui_boiler_warmup_minutes(&m,false,now)==5);
    m.boiler_observed_unix=0; assert(!gbs_ui_boiler_warmup_minutes(&m,false,now)); m.boiler_observed_unix=now;
    m.coffee_ready_unix=now+86401; assert(!gbs_ui_boiler_warmup_minutes(&m,false,now));
    m.coffee_ready_unix=0; assert(!gbs_ui_boiler_warmup_minutes(&m,false,now));
}

static gbs_ui_bridge_t cup(const char *id,const char *phase)
{
    gbs_ui_bridge_t s={.valid=true,.job=true,.recipe_enabled=true};
    snprintf(s.job_id,sizeof(s.job_id),"%s",id);
    snprintf(s.flow_job_id,sizeof(s.flow_job_id),"%s",id);
    snprintf(s.recipe_job_id,sizeof(s.recipe_job_id),"%s",id);
    snprintf(s.recipe_phase,sizeof(s.recipe_phase),"%s",phase);
    return s;
}

static void flow_evidence(void)
{
    gbs_ui_flow_t f={0}; gbs_ui_bridge_t s=cup("cup-a","reading");
    gbs_ui_flow_update(&f,&s);
    assert(f.initialized&&!strcmp(f.job_id,"cup-a")&&f.nodes[GBS_FLOW_GRIND]==GBS_FLOW_WAIT);
    assert(f.nodes[GBS_FLOW_RECIPE]==GBS_FLOW_ACTIVE&&f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_WAIT);
    s.recipe_log_valid=true; s.recipe_grams=30; strcpy(s.recipe_phase,"applying");
    gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_GRIND]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_RECIPE]==GBS_FLOW_DONE&&f.recipe_grams==30);
    assert(f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_ACTIVE&&f.nodes[GBS_FLOW_RESULT]!=GBS_FLOW_DONE);
    strcpy(s.recipe_phase,"applied"); gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_ACTIVE);
    s.pending=s.flow_result_valid=true; s.flow_result_grams=29.8; s.flow_result_seconds=33.052;
    gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_ACTIVE);
    s.job=s.pending=false; s.job_id[0]=0; strcpy(s.outcome,"success");
    gbs_ui_flow_update(&f,&s);
    for(unsigned i=0;i<GBS_FLOW_COUNT;++i) assert(f.nodes[i]==GBS_FLOW_DONE);
    s.recipe_log_valid=s.flow_result_valid=false; s.recipe_job_id[0]=0;
    gbs_ui_flow_update(&f,&s);
    assert(f.recipe_grams==30&&f.result_grams==29.8&&f.result_seconds==33.052);
    for(unsigned i=0;i<GBS_FLOW_COUNT;++i) assert(f.nodes[i]==GBS_FLOW_DONE);

    s=cup("cup-b","applied"); strcpy(s.recipe_job_id,"cup-a");
    s.recipe_log_valid=true; s.recipe_grams=30; gbs_ui_flow_update(&f,&s);
    assert(!strcmp(f.job_id,"cup-b")&&f.recipe_grams==0&&f.result_grams==0);
    for(unsigned i=0;i<GBS_FLOW_COUNT;++i) assert(f.nodes[i]==GBS_FLOW_WAIT);
    strcpy(s.recipe_job_id,"cup-b"); s.recipe_log_valid=false;
    gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_RECIPE]!=GBS_FLOW_DONE);
    assert(f.nodes[GBS_FLOW_GRIND]!=GBS_FLOW_DONE);
    s=cup("cup-c","failed"); gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_GRIND]==GBS_FLOW_WAIT&&f.nodes[GBS_FLOW_RECIPE]==GBS_FLOW_FAILED);
    assert(f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_WAIT&&f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_WAIT);
    s=cup("cup-d","failed"); s.recipe_log_valid=true; s.recipe_grams=35;
    gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_RECIPE]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_FAILED);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_WAIT);
    s=cup("cup-e","unconfirmed"); s.recipe_log_valid=true; s.recipe_grams=40;
    gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_RECIPE]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_UNKNOWN);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_WAIT);
    s.job=false; s.flow_result_valid=true; s.flow_result_grams=39.8; s.flow_result_seconds=27;
    strcpy(s.outcome,"success"); gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_SETTING]==GBS_FLOW_UNKNOWN&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_DONE);

    // A fast ACK carries its own cup/result evidence, not an invented recipe history.
    memset(&f,0,sizeof(f)); s=cup("fast","disabled"); s.job=false;
    s.flow_result_valid=true; s.flow_result_grams=30; s.flow_result_seconds=28; strcpy(s.outcome,"success");
    gbs_ui_flow_update(&f,&s);
    for(unsigned i=0;i<=GBS_FLOW_SETTING;++i) assert(f.nodes[i]==GBS_FLOW_SKIPPED);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_DONE);
    memset(&f,0,sizeof(f)); s.flow_job_id[0]=0; gbs_ui_flow_update(&f,&s); assert(!f.initialized);
    s=cup("unknown","applied"); s.job=false; strcpy(s.outcome,"delivery_unknown");
    s.flow_result_valid=true; s.flow_result_grams=30; s.flow_result_seconds=28;
    gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_DONE&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_UNKNOWN);
    s.wifi=s.grinder=s.cloud=true; gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_UNKNOWN);
    s=cup("timeout","applied"); s.pending=s.canceling=true;
    s.latest_grams=s.result_grams=s.unconfirmed_grams=30;
    s.latest_seconds=s.result_seconds=s.unconfirmed_seconds=28;
    gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_FAILED&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_SKIPPED);
    s.job=false; strcpy(s.outcome,"canceled"); gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_FAILED&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_SKIPPED);
    memset(&f,0,sizeof(f)); gbs_ui_flow_update(&f,&s);
    assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_FAILED&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_SKIPPED);
    s=cup("next","reading"); strcpy(s.flow_job_id,"wrong"); gbs_ui_flow_update(&f,&s);
    assert(!strcmp(f.job_id,"timeout"));
    strcpy(s.flow_job_id,"next"); s.pending=s.flow_result_valid=true; s.flow_result_grams=NAN; s.flow_result_seconds=30;
    gbs_ui_flow_update(&f,&s); assert(f.nodes[GBS_FLOW_RESULT]!=GBS_FLOW_DONE);
}

static void flow_timeout(void)
{
    gbs_ui_flow_t f={0}; gbs_ui_bridge_t s=cup("deadline","applied");
    s.observed_ms=1000; s.job_deadline_ms=181000; gbs_ui_flow_update(&f,&s);
    assert(f.job_deadline_ms==181000&&!f.result_timed_out);
    s.pending=s.canceling=true; s.job_deadline_ms=0; s.answer_deadline_ms=200999;
    s.observed_ms=180999; gbs_ui_flow_update(&f,&s);
    assert(f.job_deadline_ms==181000&&!f.result_timed_out);
    s.observed_ms=181000; gbs_ui_flow_update(&f,&s);
    assert(!f.result_timed_out&&f.cancellation_observed&&f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_FAILED);
    s.job=false; strcpy(s.outcome,"canceled"); gbs_ui_flow_update(&f,&s);
    assert(!f.result_timed_out&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_SKIPPED);

    s=cup("elapsed","applied"); s.job_deadline_ms=181000; s.observed_ms=1000;
    gbs_ui_flow_update(&f,&s);
    s.pending=s.canceling=true; s.job_deadline_ms=0; s.observed_ms=181000;
    gbs_ui_flow_update(&f,&s); assert(f.result_timed_out);
    s.job=false; strcpy(s.outcome,"canceled"); gbs_ui_flow_update(&f,&s);
    assert(f.result_timed_out&&f.nodes[GBS_FLOW_DELIVERY]==GBS_FLOW_SKIPPED);

    // A new cup clears timeout evidence, including the previous deadline.
    s=cup("fast-cancel","reading"); gbs_ui_flow_update(&f,&s);
    assert(!f.result_timed_out&&!f.cancellation_observed&&f.job_deadline_ms==0);
    s.job_deadline_ms=250000; s.observed_ms=70000; gbs_ui_flow_update(&f,&s);
    s.job_deadline_ms=350000; gbs_ui_flow_update(&f,&s);
    assert(f.job_deadline_ms==250000);
    s.job=false; s.job_deadline_ms=0; s.observed_ms=250000; strcpy(s.outcome,"canceled");
    gbs_ui_flow_update(&f,&s); assert(f.result_timed_out);
    memset(&f,0,sizeof(f)); gbs_ui_flow_update(&f,&s);
    assert(!f.result_timed_out&&f.job_deadline_ms==0);

    // An elapsed deadline alone is not evidence of a result timeout.
    s=cup("result","applied"); s.job_deadline_ms=181000; s.observed_ms=181001;
    gbs_ui_flow_update(&f,&s); assert(!f.result_timed_out);
    s.pending=s.flow_result_valid=true; s.flow_result_grams=30; s.flow_result_seconds=28;
    gbs_ui_flow_update(&f,&s); assert(f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_DONE&&!f.result_timed_out);
    s.canceling=true; s.flow_result_valid=false; gbs_ui_flow_update(&f,&s);
    assert(!f.result_timed_out&&f.nodes[GBS_FLOW_RESULT]==GBS_FLOW_DONE);
    s.job=false; strcpy(s.outcome,"canceled"); gbs_ui_flow_update(&f,&s);
    assert(!f.result_timed_out);
}

static void flow_parser(void)
{
    gbs_ui_bridge_t s;
    assert(gbs_ui_parse_bridge("{\"phase\":\"ready\",\"flow_job_uuid\":\"cup\",\"flow_result_valid\":true,"
        "\"flow_result_grams\":29.8,\"flow_result_seconds\":27,\"recipe_follow\":{\"grind_uuid\":\"cup\","
        "\"phase\":\"applied\",\"target_grams\":30,\"csv_rows\":1,\"grind_date\":\"2026-10-23T10:00:00Z\"}}",&s));
    assert(s.flow_result_valid&&s.recipe_log_valid&&!s.recipe_current&&!strcmp(s.flow_job_id,"cup"));
    assert(s.flow_result_grams==29.8&&s.flow_result_seconds==27&&!strcmp(s.recipe_job_id,"cup"));
    assert(gbs_ui_parse_bridge("{\"phase\":\"ready\",\"flow_result_valid\":true,\"flow_result_grams\":6000,"
        "\"flow_result_seconds\":27,\"recipe_follow\":{\"grind_uuid\":\"cup\",\"phase\":\"applied\",\"target_grams\":30}}",&s));
    assert(!s.flow_result_valid&&!s.recipe_log_valid);
    assert(gbs_ui_parse_bridge("{\"phase\":\"ready\",\"recipe_follow\":{\"grind_uuid\":\"cup\","
        "\"phase\":\"probe_matched\",\"target_grams\":30,\"csv_rows\":1,\"grind_date\":\"2026-10-23T10:00:00Z\"}}",&s));
    assert(!s.recipe_log_valid);
}

int main(void)
{
    boiler_warmup();
    flow_evidence(); flow_timeout(); flow_parser();
    gbs_ui_ble_t ble={.valid=true,.observed_ms=1000};
    assert(!gbs_ui_ble_is_stale(&ble,15999));
    assert(gbs_ui_ble_is_stale(&ble,16000)&&gbs_ui_ble_is_stale(&ble,999));
    ble.valid=false; assert(gbs_ui_ble_is_stale(&ble,1000));
    _Static_assert(GBS_ACTION_STANDBY_MODE==16,"Existing action IDs must remain stable");
    assert(!gbs_ui_machine_action(GBS_ACTION_ROTATION_AUTO));
    assert(!gbs_ui_machine_action(GBS_ACTION_SOUND));
    assert(!gbs_ui_machine_action(GBS_ACTION_WEIGHT_PRESET));
    gbs_ui_machine_t offline={.available=true,.stale=true,.writable=GBS_UI_MACHINE_ACTIONS};
    assert(gbs_ui_can_control(GBS_ACTION_WEIGHT,&offline));
    assert(!gbs_ui_can_control(GBS_ACTION_COFFEE,&offline));
    assert(!gbs_ui_can_control(GBS_ACTION_POWER,&offline));
    assert(!gbs_ui_can_control(GBS_ACTION_MODE,&offline));
    offline.writable=0;assert(!gbs_ui_can_control(GBS_ACTION_WEIGHT,&offline));
    offline.writable=GBS_UI_MACHINE_ACTIONS;offline.available=false;
    assert(!gbs_ui_can_control(GBS_ACTION_WEIGHT,&offline));
    assert(gbs_ui_variable_weight_valid(1)&&gbs_ui_variable_weight_valid(35)&&gbs_ui_variable_weight_valid(200));
    assert(!gbs_ui_variable_weight_valid(20)&&!gbs_ui_variable_weight_valid(30)&&!gbs_ui_variable_weight_valid(36));
    assert(!gbs_ui_variable_weight_valid(0)&&!gbs_ui_variable_weight_valid(201)&&!gbs_ui_variable_weight_valid(30.5));
    assert(!gbs_ui_variable_weight_valid(NAN)&&!gbs_ui_variable_weight_valid(INFINITY));
    gbs_ui_machine_t scale={.scale_observed_ms=1000};
    assert(!gbs_ui_scale_is_stale(&scale,60999));
    assert(gbs_ui_scale_is_stale(&scale,61000));
    assert(gbs_ui_scale_is_stale(&scale,999));
    scale.scale_observed_ms=0; assert(gbs_ui_scale_is_stale(&scale,1000));
    gbs_ui_bridge_t s = {0};
    assert(gbs_ui_stage(&s) == GBS_UI_BOOT);
    assert(gbs_ui_parse_bridge("{\"phase\":\"waiting_for_coffee\",\"have_job\":true,\"grind_uuid\":\"current\","
        "\"recipe_follow\":{\"enabled\":true,\"phase\":\"applied\",\"grind_uuid\":\"current\",\"target_grams\":30}}", &s));
    assert(s.target_grams == 30 && s.recipe_enabled && s.recipe_current && !strcmp(s.job_id,"current") && !strcmp(s.recipe_phase, "applied"));
    assert(gbs_ui_parse_bridge("{\"phase\":\"waiting_for_coffee\",\"have_job\":true,\"grind_uuid\":\"current\","
        "\"recipe_follow\":{\"grind_uuid\":\"old\",\"target_grams\":30}}", &s));
    assert(s.target_grams == 0 && !s.recipe_current);
    assert(!gbs_ui_parse_bridge("{broken", &s));
    assert(!gbs_ui_parse_bridge("{\"phase\":2}", &s));
    assert(gbs_ui_parse_bridge("{\"phase\":\"ready\",\"mode\":\"active\",\"wifi\":true,"
        "\"grinder\":true,\"cloud\":true,\"storage\":true,\"uptime_ms\":10000,"
        "\"job_deadline_ms\":190000,\"have_job\":true}", &s));
    assert(gbs_ui_stage(&s) == GBS_UI_WAIT);
    s.invalid_coffee = true; assert(gbs_ui_stage(&s) == GBS_UI_INVALID);
    s.pending = true; assert(gbs_ui_stage(&s) == GBS_UI_SENDING);
    s.job = false; strcpy(s.outcome, "invalid_coffee"); assert(gbs_ui_stage(&s) == GBS_UI_INVALID);
    s.job = true; s.pending = s.invalid_coffee = false; s.outcome[0] = 0;
    assert(gbs_ui_remaining(s.job_deadline_ms, 10000) == 180);
    assert(gbs_ui_remaining(s.job_deadline_ms, 189001) == 1);
    assert(gbs_ui_remaining(s.job_deadline_ms, 200000) == 0);
    // Re-reading a stale snapshot cannot restart the budget or declare completion.
    assert(gbs_ui_remaining(s.job_deadline_ms, 400000) == 0);
    assert(gbs_ui_stage(&s) == GBS_UI_WAIT);
    s.pending = true; assert(gbs_ui_stage(&s) == GBS_UI_SENDING);
    s.canceling = true; assert(gbs_ui_stage(&s) == GBS_UI_CANCELING);
    s.job = false; strcpy(s.outcome, "delivery_unknown");
    s.grinder = false; assert(gbs_ui_stage(&s) == GBS_UI_UNKNOWN);
    s.grinder = true; assert(gbs_ui_stage(&s) == GBS_UI_UNKNOWN);
    s.job = true; assert(gbs_ui_stage(&s) == GBS_UI_CANCELING);
    s.storage = false; assert(gbs_ui_stage(&s) == GBS_UI_STORAGE);
    s.storage = true; s.pairing = true; assert(gbs_ui_stage(&s) == GBS_UI_PAIRING);
    s.pairing = false; s.job = false; s.outcome[0] = 0; s.baseline = true;
    assert(gbs_ui_stage(&s) == GBS_UI_BASELINE);
    s.baseline = false; s.active = false; assert(gbs_ui_stage(&s) == GBS_UI_MONITOR);
    assert(gbs_ui_backlight(80, true, 61000, false) == 20);
    assert(gbs_ui_backlight(80, true, 121000, false) == 0);
    assert(gbs_ui_backlight(80, true, 121000, true) == 80);
    assert(gbs_ui_backlight(80, false, 121000, false) == 80);
    for (unsigned brightness = 25; brightness <= 100; ++brightness) {
        assert(gbs_ui_backlight(brightness, false, 0, false) == brightness);
        assert(gbs_ui_backlight(brightness, false, 60000, false) == brightness);
        assert(gbs_ui_backlight(brightness, false, INT64_MAX, false) == brightness);
    }
    bool wake = false;
    assert(!gbs_ui_touch(true, true, &wake));
    assert(!gbs_ui_touch(true, false, &wake));
    assert(!gbs_ui_touch(false, false, &wake));
    assert(gbs_ui_touch(true, false, &wake));
    assert(!gbs_ui_busy(&s, GBS_UI_CONTROL_UNKNOWN, false));
    assert(gbs_ui_busy(&s, GBS_UI_CONTROL_PENDING, false));
    puts("display model: deadlines, precedence, missing data and wake gesture passed");
    return 0;
}
