#!/usr/bin/env python3
"""Offline scale parsing/cache checks and bounded accessory-read admission."""
import os
from pathlib import Path
import re
import tempfile

from test_integrated_support import ERRORS, ROOT, compile_run, function_body


HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include "hub_cloud.h"
#include "cJSON.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
/* TYPES */
/* CONSTANTS */
static cloud_snapshot_t snapshot;
static bool cached_snapshot=true,locked;
static int64_t clock_ms=10000;
static hub_machine_selection_identity_t selection={"SYNTHETIC",1};
static esp_err_t identity_error;
#define portENTER_CRITICAL(lock) do { assert(!locked);locked=true; } while(0)
#define portEXIT_CRITICAL(lock) do { assert(locked);locked=false; } while(0)
#define parse_coffee_boiler(...) ((void)0)
#define parse_steam_level(...) ((void)0)
#define parse_prebrew(...) ((void)0)
static int64_t esp_timer_get_time(void) { return clock_ms*1000; }
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *out) {
    *out=selection;return identity_error;
}
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *target) {
    return target->generation==selection.generation&&!strcmp(target->serial_number,selection.serial_number);
}
esp_err_t hub_config_get_bbw_selection(const hub_machine_selection_identity_t *target,hub_bbw_selection_t *out) {
    (void)target;(void)out;return ESP_ERR_NOT_FOUND;
}
const char *hub_cloud_machine_name(void) { return "Synthetic"; }

typedef struct { int unused; } installation_key_t;
typedef struct { char access_token[16]; } cloud_tokens_t;
typedef struct { char *data;size_t len; } http_response_t;
#define ESP_RETURN_ON_ERROR(expression,...) do { esp_err_t result=(expression);if(result!=ESP_OK){return result;} } while(0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define HTTP_METHOD_GET 1
#define READONLY_DIAG_DASHBOARD 1
#define CUSTOMER_APP_URL "https://synthetic.example.test"
#define pdTRUE 1
static int refresh_mutex;
static bool interactive,command_waiting,mutex_busy,io_locked,pending_after_lock,pending_after_load;
static bool grace,grinder_busy,grace_after_lock,grace_after_load,busy_after_lock,busy_after_load,preperform_failure;
static bool rebind_after_load,rebind_after_http;
static int allocations,allocation_attempts,fail_allocation,requests,loads,cleanups,refresh_requests,invalidations,unlocks;
static int http_calls,io_completions,session_cleanup_calls,sessions_live,sessions_closed,session_marker;
static int response_status=200;
static esp_err_t mutex_error,context_error,request_error;
static size_t largest_block=4096;
static const char *response_body;
static bool hub_command_idle_background_deferred(void) { return interactive||grace; }
static bool cloud_command_is_waiting(void) { return command_waiting; }
#ifdef HUB_GRINDER
static bool hub_grinder_busy(void) { return grinder_busy; }
#endif
static esp_err_t ensure_refresh_mutex(void) { return mutex_error; }
static int xSemaphoreTake(int mutex,int timeout) {
    (void)mutex;assert(timeout==0&&!io_locked);
    if(mutex_busy) { return 0; }
    io_locked=true;interactive=pending_after_lock;grace=grace_after_lock;grinder_busy=busy_after_lock;return pdTRUE;
}
static void xSemaphoreGive(int mutex) { (void)mutex;assert(io_locked);io_locked=false;++unlocks; }
static void *heap_caps_calloc(size_t n,size_t size,int caps) {
    assert(io_locked&&caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(++allocation_attempts==fail_allocation) { return NULL; }
    void *p=calloc(n,size);assert(p);++allocations;return p;
}
static size_t heap_caps_get_largest_free_block(int caps) {
    assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return largest_block;
}
static esp_err_t load_cloud_context(hub_cloud_settings_t *settings,installation_key_t *key,cloud_tokens_t *tokens) {
    assert(io_locked&&settings&&key&&tokens);++loads;
    interactive=pending_after_load;
    grace=grace_after_load;grinder_busy=busy_after_load;
    if(rebind_after_load) { ++selection.generation; }
    strcpy(tokens->access_token,"synthetic");return context_error;
}
static void free_cloud_context(hub_cloud_settings_t *settings,installation_key_t *key,cloud_tokens_t *tokens) {
    assert(io_locked);++cleanups;
    if(settings) { --allocations;free(settings); }
    if(key) { --allocations;free(key); }
    if(tokens) { --allocations;free(tokens); }
}
esp_err_t hub_cloud_request_refresh(void) { ++refresh_requests;return ESP_OK; }
static void invalidate_cached_tokens(void) { ++invalidations; }
typedef void *esp_http_client_handle_t;
static esp_err_t cloud_http_request_no_retry(const char *url,int method,const installation_key_t *key,
        const char *token,const char *body,http_response_t *response,int *status,int timeout,
        esp_http_client_handle_t *session,bool *request_started) {
    assert(session&&!*session&&request_started&&!*request_started);++http_calls;
    assert(io_locked&&method==HTTP_METHOD_GET&&key&&!strcmp(token,"synthetic")&&!body&&timeout==4000);
    assert(!strcmp(url,"https://synthetic.example.test/things/SYNTHETIC/dashboard"));
    if(preperform_failure)return request_error;
    *request_started=true;++requests;*session=&session_marker;++sessions_live;
    *status=response_status;response->data=strdup(response_body);assert(response->data);response->len=strlen(response->data);
    if(rebind_after_http) { ++selection.generation; }
    if(request_error!=ESP_OK) { *session=NULL;--sessions_live;++sessions_closed; }
    return request_error;
}
static void cloud_http_session_cleanup(esp_http_client_handle_t *session) {
    assert(io_locked);++session_cleanup_calls;
    if(*session) { assert(*session==&session_marker&&sessions_live==1);*session=NULL;--sessions_live;++sessions_closed; }
}
static uint32_t http_response_first_bytes(const http_response_t *response) { (void)response;return 0; }
static void record_readonly_http_diag(int kind,esp_err_t error,int status,int elapsed,size_t length,uint32_t first) {
    (void)error;(void)status;(void)elapsed;(void)length;(void)first;assert(kind==READONLY_DIAG_DASHBOARD&&io_locked);
}
static void hub_http_response_reset(http_response_t *response) { free(response->data);memset(response,0,sizeof(*response)); }
static void cloud_readonly_io_unlock(void) { ++io_completions;xSemaphoreGive(refresh_mutex); }
/* FUNCTIONS */

static cloud_snapshot_t baseline(void) {
    cloud_snapshot_t state={.selection_generation=1,.connected=true,
        .coffee_temperature_set=true,.coffee_temperature=93,.steam_level_set=true,.steam_level=2,
        .dose1_set=true,.dose1=20,.dose2_set=true,.dose2=30,
        .total_coffee_set=true,.total_coffee=12,.total_flush_set=true,.total_flush=7,
        .prebrew_on_set=true,.prebrew_on=1.5,.prebrew_off_set=true,.prebrew_off=2.5};
    strcpy(state.serial_number,"SYNTHETIC");strcpy(state.bbw_mode,"Dose2");
    strcpy(state.last_updated,"2026-10-20T01:00:00Z");strcpy(state.prebrew_mode,"PreBrewing");
    strcpy(state.coffee_boiler_ready_time,"2026-10-20T00:00:00Z");
    strcpy(state.steam_boiler_ready_time,"2026-10-20T00:00:01Z");
    return state;
}

static cloud_snapshot_t parse_widgets(const char *widgets,const cloud_snapshot_t *previous) {
    char body[2048],name[100];cloud_snapshot_t out={0};
    snprintf(body,sizeof(body),"{\"serialNumber\":\"SYNTHETIC\",\"connected\":true,\"widgets\":[%s]}",widgets);
    assert(parse_dashboard_snapshot(body,"SYNTHETIC",1,previous,&out,name,sizeof(name))==ESP_OK);
    return out;
}

static void unrelated_unchanged(const cloud_snapshot_t *before) {
    assert(snapshot.selection_generation==before->selection_generation);
    assert(!strcmp(snapshot.serial_number,before->serial_number));
    assert(snapshot.dose1_set==before->dose1_set&&snapshot.dose1==before->dose1);
    assert(snapshot.dose2_set==before->dose2_set&&snapshot.dose2==before->dose2);
    assert(!strcmp(snapshot.bbw_mode,before->bbw_mode)&&!strcmp(snapshot.last_updated,before->last_updated));
    assert(snapshot.coffee_temperature_set==before->coffee_temperature_set&&snapshot.coffee_temperature==before->coffee_temperature);
    assert(snapshot.steam_level_set==before->steam_level_set&&snapshot.steam_level==before->steam_level);
    assert(snapshot.total_coffee_set==before->total_coffee_set&&snapshot.total_coffee==before->total_coffee);
    assert(snapshot.total_flush_set==before->total_flush_set&&snapshot.total_flush==before->total_flush);
    assert(snapshot.prebrew_on_set==before->prebrew_on_set&&snapshot.prebrew_on==before->prebrew_on);
    assert(snapshot.prebrew_off_set==before->prebrew_off_set&&snapshot.prebrew_off==before->prebrew_off);
    assert(!strcmp(snapshot.prebrew_mode,before->prebrew_mode));
    assert(!strcmp(snapshot.coffee_boiler_ready_time,before->coffee_boiler_ready_time));
    assert(!strcmp(snapshot.steam_boiler_ready_time,before->steam_boiler_ready_time));
}

static cJSON *state_json(void) {
    cJSON *json=cJSON_Parse(hub_cloud_machine_state_json());assert(json&&!locked);return json;
}

static void assert_null(cJSON *json,const char *key) { assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(json,key))); }
static void assert_bool(cJSON *json,const char *key,bool expected) {
    cJSON *item=cJSON_GetObjectItemCaseSensitive(json,key);assert(cJSON_IsBool(item)&&cJSON_IsTrue(item)==expected);
}

static void on_off_on(void) {
    snapshot=baseline();
    for(int i=0;i<3;++i) {
        bool on=i!=1;clock_ms+=10000;char widgets[512];
        snprintf(widgets,sizeof(widgets),
            "{\"code\":\"CMBrewByWeightDoses\",\"output\":{\"scaleConnected\":%s}},"
            "{\"code\":\"ThingScale\",\"output\":{\"connected\":%s,\"batteryLevel\":42,\"calibrationRequired\":true,\"name\":\"Synthetic scale\"}}",
            on?"true":"false",on?"true":"false");
        cloud_snapshot_t before=snapshot,observed=parse_widgets(widgets,&snapshot);
        assert(observed.scale_observed_ms==clock_ms&&observed.bbw_scale_connected==on&&observed.thing_scale_connected==on);
        // A scale read may contain differing machine settings; publishing it must not overwrite them.
        observed.dose1=80;observed.dose2=90;strcpy(observed.bbw_mode,"Continuous");
        assert(publish_scale_snapshot(&observed));unrelated_unchanged(&before);
        cJSON *json=state_json();assert_bool(json,"scaleConnected",on);assert_bool(json,"thingScaleConnected",on);
        assert(cJSON_GetObjectItem(json,"scaleObservedMs")->valuedouble==clock_ms);
        if(on) { assert(cJSON_GetObjectItem(json,"scaleBatteryLevel")->valuedouble==42);assert_bool(json,"scaleCalibrationRequired",true); }
        else { assert_null(json,"scaleBatteryLevel");assert_null(json,"scaleCalibrationRequired"); }
        assert(!strcmp(cJSON_GetObjectItem(json,"brewByWeightMode")->valuestring,"Dose2"));
        assert(!strcmp(cJSON_GetObjectItem(json,"lastUpdated")->valuestring,before.last_updated));
        cJSON_Delete(json);
    }
}

static void missing_or_invalid(void) {
    cloud_snapshot_t good=snapshot;
    const char *invalid[]={"null","1","\"true\"","{}","[]"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid)+2;++i) {
        char widgets[512]="";
        if(i==1) { strcpy(widgets,"{\"code\":\"ThingScale\",\"output\":{\"batteryLevel\":42,\"name\":\"Synthetic\"}}"); }
        if(i>=2) {
            snprintf(widgets,sizeof(widgets),
                "{\"code\":\"CMBrewByWeightDoses\",\"output\":{\"scaleConnected\":%s}},"
                "{\"code\":\"ThingScale\",\"output\":{\"connected\":%s}}",invalid[i-2],invalid[i-2]);
        }
        snapshot=good;clock_ms+=1000;cloud_snapshot_t observed=parse_widgets(widgets,&snapshot);
        assert(!observed.bbw_scale_connected_set&&!observed.thing_scale_connected_set&&!observed.scale_observed_ms);
        assert(publish_scale_snapshot(&observed)&&!snapshot.scale_observed_ms);unrelated_unchanged(&good);
        cJSON *json=state_json();assert_null(json,"scaleConnected");assert_null(json,"thingScaleConnected");
        assert_null(json,"scaleBatteryLevel");assert_null(json,"scaleCalibrationRequired");assert_null(json,"scaleName");cJSON_Delete(json);
    }
    snapshot=good;
    cloud_snapshot_t observed=parse_widgets("{\"code\":\"ThingScale\",\"output\":{\"connected\":true}}",&snapshot);
    assert(observed.scale_observed_ms==clock_ms&&publish_scale_snapshot(&observed));
    cJSON *json=state_json();assert_null(json,"scaleConnected");assert_bool(json,"thingScaleConnected",true);
    assert_null(json,"scaleBatteryLevel");assert_null(json,"scaleCalibrationRequired");assert_null(json,"scaleName");cJSON_Delete(json);
}

static void battery_bounds(void) {
    const char *values[]={"0","100","0.5","\"42\"","-1","101","\"NaN\"","\"Inf\"","\"-Inf\"","1e309","null","true","{}","[]"};
    const double expected[]={0,100,0.5,42};
    for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i) {
        char widgets[256];snprintf(widgets,sizeof(widgets),
            "{\"code\":\"ThingScale\",\"output\":{\"connected\":true,\"batteryLevel\":%s}}",values[i]);
        cloud_snapshot_t observed=parse_widgets(widgets,&snapshot);
        assert(observed.scale_battery_level_set==(i<4));
        if(i<4) { assert(observed.scale_battery_level==expected[i]); }
        assert(publish_scale_snapshot(&observed));cJSON *json=state_json();
        assert_bool(json,"thingScaleConnected",true);assert_null(json,"scaleConnected");
        assert(cJSON_IsNumber(cJSON_GetObjectItem(json,"scaleBatteryLevel"))==(i<4));cJSON_Delete(json);
    }
    const double nonfinite[]={NAN,INFINITY,-INFINITY};
    for(unsigned i=0;i<sizeof(nonfinite)/sizeof(*nonfinite);++i) {
        cJSON *output=cJSON_CreateObject();assert(output);
        cJSON *number=cJSON_AddNumberToObject(output,"batteryLevel",0);assert(number);
        number->valuedouble=nonfinite[i];
        cloud_snapshot_t parsed={0};parse_thing_scale(output,&parsed);
        assert(!parsed.scale_battery_level_set);cJSON_Delete(output);
    }
}

static void freshness_and_connection(void) {
    snapshot=baseline();snapshot.scale_observed_ms=10000;
    snapshot.bbw_scale_connected_set=snapshot.bbw_scale_connected=true;
    snapshot.thing_scale_connected_set=snapshot.thing_scale_connected=true;
    snapshot.scale_battery_level_set=true;snapshot.scale_battery_level=70;
    snapshot.scale_calibration_required_set=snapshot.scale_calibration_required=true;
    strcpy(snapshot.scale_name,"Synthetic scale");
    const int64_t times[]={10000,69999,70000,9999};
    for(unsigned i=0;i<sizeof(times)/sizeof(*times);++i) {
        clock_ms=times[i];bool fresh=i<2;assert(scale_snapshot_fresh(&snapshot,clock_ms)==fresh);
        cJSON *json=state_json();
        if(fresh) { assert_bool(json,"scaleConnected",true);assert_bool(json,"thingScaleConnected",true); }
        else { assert_null(json,"scaleConnected");assert_null(json,"thingScaleConnected");assert_null(json,"scaleBatteryLevel");assert_null(json,"scaleCalibrationRequired");assert_null(json,"scaleName"); }
        cJSON_Delete(json);
    }
    clock_ms=10000;snapshot.scale_observed_ms=0;assert(!scale_snapshot_fresh(&snapshot,clock_ms));
    cJSON *json=state_json();assert_null(json,"scaleConnected");cJSON_Delete(json);
    snapshot.scale_observed_ms=clock_ms;
    cloud_snapshot_t before=snapshot,offline=snapshot;offline.connected=false;
    assert(publish_scale_snapshot(&offline)&&!snapshot.connected);unrelated_unchanged(&before);
    assert(!scale_snapshot_fresh(&snapshot,clock_ms));json=state_json();assert_null(json,"scaleConnected");assert_null(json,"scaleBatteryLevel");cJSON_Delete(json);
    assert(publish_scale_snapshot(&before)&&snapshot.connected);
    // Brew-by-weight's explicit offline observation takes precedence for battery/calibration.
    snapshot.bbw_scale_connected=false;json=state_json();assert_bool(json,"scaleConnected",false);
    assert_bool(json,"thingScaleConnected",true);assert_null(json,"scaleBatteryLevel");assert_null(json,"scaleCalibrationRequired");cJSON_Delete(json);
    snapshot.bbw_scale_connected=true;snapshot.thing_scale_connected=false;json=state_json();
    assert_bool(json,"scaleConnected",true);assert_bool(json,"thingScaleConnected",false);
    assert(cJSON_GetObjectItem(json,"scaleBatteryLevel")->valuedouble==70);assert_bool(json,"scaleCalibrationRequired",true);cJSON_Delete(json);
}

static void binding(void) {
    snapshot=baseline();cloud_snapshot_t before=snapshot,observed=snapshot;
    observed.scale_observed_ms=clock_ms;observed.bbw_scale_connected_set=observed.bbw_scale_connected=true;
    observed.selection_generation=2;assert(!publish_scale_snapshot(&observed));
    assert(!memcmp(&snapshot,&before,sizeof(snapshot)));
    observed.selection_generation=1;strcpy(observed.serial_number,"OTHER");assert(!publish_scale_snapshot(&observed));
    assert(!memcmp(&snapshot,&before,sizeof(snapshot)));
    strcpy(observed.serial_number,"SYNTHETIC");cached_snapshot=false;assert(!publish_scale_snapshot(&observed));cached_snapshot=true;
    // Current configuration matches the reply, but a rebound cache must still reject it.
    selection.generation=observed.selection_generation=2;assert(!publish_scale_snapshot(&observed));
    cJSON *json=state_json();assert_null(json,"scaleConnected");assert(cJSON_GetObjectItem(json,"scaleObservedMs")->valuedouble==0);cJSON_Delete(json);
    selection.generation=observed.selection_generation=1;
    strcpy(selection.serial_number,"OTHER");strcpy(observed.serial_number,"OTHER");assert(!publish_scale_snapshot(&observed));
    json=state_json();assert_null(json,"scaleConnected");cJSON_Delete(json);
    strcpy(selection.serial_number,"SYNTHETIC");strcpy(observed.serial_number,"SYNTHETIC");
    assert(publish_scale_snapshot(&observed));
    char name[32];cloud_snapshot_t parsed={0};
    assert(parse_dashboard_snapshot("{\"serialNumber\":\"OTHER\"}","SYNTHETIC",1,&snapshot,&parsed,name,sizeof(name))==ESP_ERR_INVALID_RESPONSE);
    assert(parse_dashboard_snapshot("not-json","SYNTHETIC",1,&snapshot,&parsed,name,sizeof(name))==ESP_ERR_INVALID_RESPONSE);
}

static void fresh_battery_observation(void) {
    snapshot=baseline();clock_ms=10000;snapshot.scale_observed_ms=10000;
    snapshot.bbw_scale_connected_set=snapshot.bbw_scale_connected=true;
    snapshot.thing_scale_connected_set=snapshot.thing_scale_connected=true;
    snapshot.scale_battery_level_set=true;
    snapshot.scale_battery_level=0;assert(hub_cloud_has_fresh_scale_battery());
    snapshot.scale_battery_level=100;assert(hub_cloud_has_fresh_scale_battery());
    const double invalid[]={-1,101,NAN,INFINITY,-INFINITY};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        snapshot.scale_battery_level=invalid[i];assert(!hub_cloud_has_fresh_scale_battery());
    }
    snapshot.scale_battery_level=73;
    snapshot.scale_battery_level_set=false;assert(!hub_cloud_has_fresh_scale_battery());snapshot.scale_battery_level_set=true;
    snapshot.bbw_scale_connected=false;assert(!hub_cloud_has_fresh_scale_battery());
    snapshot.bbw_scale_connected_set=false;assert(hub_cloud_has_fresh_scale_battery());
    snapshot.thing_scale_connected=false;assert(!hub_cloud_has_fresh_scale_battery());
    snapshot.bbw_scale_connected_set=snapshot.bbw_scale_connected=true;assert(hub_cloud_has_fresh_scale_battery());
    snapshot.connected=false;assert(!hub_cloud_has_fresh_scale_battery());snapshot.connected=true;
    clock_ms=69999;assert(hub_cloud_has_fresh_scale_battery());
    clock_ms=70000;assert(!hub_cloud_has_fresh_scale_battery());
    clock_ms=9999;assert(!hub_cloud_has_fresh_scale_battery());clock_ms=10000;
    snapshot.scale_observed_ms=0;assert(!hub_cloud_has_fresh_scale_battery());snapshot.scale_observed_ms=10000;
    cached_snapshot=false;assert(!hub_cloud_has_fresh_scale_battery());cached_snapshot=true;
    ++selection.generation;assert(!hub_cloud_has_fresh_scale_battery());--selection.generation;
    strcpy(selection.serial_number,"OTHER");assert(!hub_cloud_has_fresh_scale_battery());
    strcpy(selection.serial_number,"SYNTHETIC");assert(hub_cloud_has_fresh_scale_battery());
}

static void retained_refresh_api_cases(void) {
    const char *good="{\"serialNumber\":\"SYNTHETIC\",\"connected\":true,\"widgets\":[{\"code\":\"ThingScale\",\"output\":{\"connected\":true}}]}";
    for(int scenario=0;scenario<33;++scenario) {
        snapshot=baseline();snapshot.scale_observed_ms=1234;cloud_snapshot_t before=snapshot;
        clock_ms=80000;selection=(hub_machine_selection_identity_t){"SYNTHETIC",1};
        identity_error=mutex_error=context_error=request_error=0;
        interactive=command_waiting=mutex_busy=io_locked=pending_after_lock=pending_after_load=false;
        grace=grinder_busy=grace_after_lock=grace_after_load=busy_after_lock=busy_after_load=preperform_failure=false;
        rebind_after_load=rebind_after_http=false;
        allocations=allocation_attempts=fail_allocation=requests=loads=cleanups=refresh_requests=invalidations=unlocks=0;
        http_calls=io_completions=session_cleanup_calls=sessions_live=sessions_closed=0;
        largest_block=CLOUD_READONLY_MIN_INTERNAL_BLOCK_BYTES;response_status=200;response_body=good;
        esp_err_t expected=ESP_OK;bool get=true,cleanup=true;
        switch(scenario) {
            case 1:interactive=true;expected=ESP_ERR_INVALID_STATE;get=cleanup=false;break;
            case 2:command_waiting=true;expected=ESP_ERR_INVALID_STATE;get=cleanup=false;break;
            case 3:mutex_busy=true;expected=ESP_ERR_INVALID_STATE;get=cleanup=false;break;
            case 4:identity_error=ESP_FAIL;expected=ESP_ERR_INVALID_STATE;get=cleanup=false;break;
            case 5:mutex_error=ESP_FAIL;expected=ESP_FAIL;get=cleanup=false;break;
            case 6:pending_after_lock=true;expected=ESP_ERR_INVALID_STATE;get=false;break;
            case 7:case 8:case 9:fail_allocation=scenario-6;expected=ESP_ERR_NO_MEM;get=false;break;
            case 10:context_error=ESP_FAIL;expected=ESP_FAIL;get=false;break;
            case 11:pending_after_load=true;expected=ESP_ERR_INVALID_STATE;get=false;break;
            case 12:largest_block=CLOUD_READONLY_MIN_INTERNAL_BLOCK_BYTES-1;expected=ESP_ERR_NO_MEM;get=false;break;
            case 13:request_error=ESP_ERR_TIMEOUT;expected=ESP_ERR_TIMEOUT;break;
            case 14:response_status=500;expected=ESP_ERR_INVALID_RESPONSE;break;
            case 15:response_status=401;expected=ESP_ERR_INVALID_RESPONSE;break;
            case 16:response_status=403;expected=ESP_ERR_INVALID_RESPONSE;break;
            case 17:response_body="not-json";expected=ESP_ERR_INVALID_RESPONSE;break;
            case 18:response_body="{}";expected=ESP_ERR_INVALID_RESPONSE;break;
            case 19:response_body="[]";expected=ESP_ERR_INVALID_RESPONSE;break;
            case 20:rebind_after_load=true;expected=ESP_ERR_INVALID_STATE;get=false;break;
            case 21:rebind_after_http=true;expected=ESP_ERR_INVALID_STATE;break;
            case 22:response_body="{\"connected\":true,\"widgets\":[]}";break;
            case 23:grace=true;expected=ESP_ERR_INVALID_STATE;get=cleanup=false;break;
            case 24:grinder_busy=true;
#ifdef HUB_GRINDER
                expected=ESP_ERR_INVALID_STATE;get=cleanup=false;
#endif
                break;
            case 25:grace_after_lock=true;expected=ESP_ERR_INVALID_STATE;get=false;break;
            case 26:busy_after_lock=true;
#ifdef HUB_GRINDER
                expected=ESP_ERR_INVALID_STATE;get=false;
#endif
                break;
            case 27:grace_after_load=true;expected=ESP_ERR_INVALID_STATE;get=false;break;
            case 28:busy_after_load=true;
#ifdef HUB_GRINDER
                expected=ESP_ERR_INVALID_STATE;get=false;
#endif
                break;
            case 29:preperform_failure=true;request_error=ESP_ERR_NO_MEM;expected=ESP_ERR_NO_MEM;break;
            case 30:request_error=ESP_ERR_NO_MEM;expected=ESP_ERR_NO_MEM;break;
            case 31:response_status=302;expected=ESP_ERR_INVALID_RESPONSE;break;
            case 32:break; // A NULL optional attempted output still records actual IO completion.
        }
        bool attempted=true;
        assert(hub_cloud_refresh_scale(scenario==32?NULL:&attempted)==expected);
        bool performed=get&&!preperform_failure;
        assert(http_calls==(int)get&&requests==(int)performed&&cleanups==(int)cleanup&&unlocks==cleanups&&!io_locked&&!allocations);
        assert(io_completions==(int)performed&&sessions_closed==(int)performed&&session_cleanup_calls==(int)cleanup&&!sessions_live);
        if(scenario!=32)assert(attempted==performed);
        if(!cleanup) { assert(!loads&&!allocation_attempts); }
        assert(invalidations==(scenario==15||scenario==16));
        // Missing/expired local auth context must not turn the optional read into a recovery loop.
        assert(refresh_requests==(scenario==15||scenario==16));
        if(expected!=ESP_OK) { assert(!memcmp(&snapshot,&before,sizeof(snapshot))); }
        else {
            unrelated_unchanged(&before);
            assert(snapshot.scale_observed_ms==(scenario==22?0:clock_ms));
            assert(snapshot.thing_scale_connected_set==(scenario!=22));
        }
    }
}

int main(void) {
    on_off_on();missing_or_invalid();battery_bounds();freshness_and_connection();binding();fresh_battery_observation();retained_refresh_api_cases();
    assert(!locked);return 0;
}
'''


def main():
    cloud = (ROOT / "main/hub_cloud.c").read_text()
    types = "\n".join(re.search(r"typedef struct\s*\{[^}]+\}\s*" + name + r";", cloud).group()
                      for name in ["cloud_snapshot_t", "bbw_snapshot_fields_t"])
    names = ["json_optional_string", "json_optional_bool", "json_safe_copy", "parse_double_string",
             "json_item_to_double", "json_number_to_double", "parse_machine_status", "parse_brew_by_weight",
             "parse_thing_scale", "preserve_missing_bbw_fields", "parse_dashboard_snapshot", "copy_snapshot",
             "snapshot_matches_selection", "scale_snapshot_fresh", "publish_scale_snapshot",
             "hub_cloud_machine_state_json", "hub_cloud_has_fresh_scale_battery", "scale_read_deferred", "hub_cloud_refresh_scale"]
    functions = []
    for name in names:
        signature = re.search(r"^(?:static )?[^\n;{}]+\b" + name + r"\([^;{]*\)\s*\{", cloud, re.M)
        assert signature, name
        functions.append(signature.group() + function_body(cloud, name) + "}")
    json_dir = Path(os.environ["IDF_PATH"]) / "components/json/cJSON"
    with tempfile.TemporaryDirectory(prefix="hub-scale-state-") as temp:
        directory = Path(temp)
        (directory / "esp_err.h").write_text(ERRORS)
        constants = "\n".join(re.search(r"static const [^;]+\b" + name + r"\s*=\s*[^;]+;", cloud).group()
                              for name in ["CLOUD_READONLY_MIN_INTERNAL_BLOCK_BYTES", "CLOUD_READONLY_HTTP_TIMEOUT_MS"])
        source = HARNESS.replace("/* TYPES */", types).replace("/* FUNCTIONS */", "\n".join(functions)).replace("/* CONSTANTS */", constants)
        compile_run(directory, "scale_state", source,
                    ["-UHUB_GRINDER", "-I", json_dir, json_dir / "cJSON.c", "-Wno-deprecated-declarations", "-fno-sanitize-recover=all"])
        compile_run(directory, "scale_state_grinder", source,
                    ["-DHUB_GRINDER", "-I", json_dir, json_dir / "cJSON.c", "-Wno-deprecated-declarations", "-fno-sanitize-recover=all"])
    print("Scale state PASS: on/off/on, battery observation bounds/TTL/binding, priority/grace/grinder admission, attempted-budget classification, session cleanup and no-IO cooldown suppression")


if __name__ == "__main__":
    main()
