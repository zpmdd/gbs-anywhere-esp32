#!/usr/bin/env python3
"""Host checks for OTA admission, guarded presets and private NVS preparation."""
import base64
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from validate_stability_foundation import function_body
from test_quick_weight_control import ESP_ERR_STUB

ROOT = Path(__file__).resolve().parents[1]
ERRORS = ESP_ERR_STUB + '\n#define ESP_ERR_INVALID_SIZE 0x104\n#define ESP_ERR_NOT_FOUND 0x105\n'

OTA_HEADER = r'''
#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef struct { char label[16]; size_t size; } esp_partition_t;
typedef unsigned esp_ota_handle_t;
typedef struct { char version[32]; } esp_app_desc_t;
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d);
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t s,esp_ota_handle_t *h);
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *d,size_t s);
esp_err_t esp_ota_end(esp_ota_handle_t h);
esp_err_t esp_ota_abort(esp_ota_handle_t h);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
'''

OTA = r'''
#include "hub_ota.h"
#include <assert.h>
#include <string.h>
static esp_partition_t partition={"ota_0",1024};
static bool maintenance,busy;
static int failure,aborts;
const char *esp_err_to_name(esp_err_t e) { return e?"error":"ok"; }
bool hub_grinder_begin_maintenance(void) { if(busy||maintenance)return false; maintenance=true; return true; }
void hub_grinder_end_maintenance(void) { assert(maintenance); maintenance=false; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &partition; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return &partition; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { (void)p;return &partition; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d) { (void)p;strcpy(d->version,"test");return ESP_OK; }
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t s,esp_ota_handle_t *h) { (void)p;(void)s;*h=1;return failure==1?ESP_FAIL:ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *d,size_t s) { (void)h;(void)d;(void)s;return failure==2?ESP_FAIL:ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { (void)h;return failure==3?ESP_FAIL:ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { (void)p;return failure==4?ESP_FAIL:ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { (void)h;++aborts;return ESP_OK; }
int main(void) {
    hub_ota_update_t update,second;
    assert(hub_ota_begin_update(0,&update)!=ESP_OK&&!maintenance);
    assert(hub_ota_begin_update(2048,&update)!=ESP_OK&&!maintenance);
    busy=true; assert(hub_ota_begin_update(4,&update)==ESP_ERR_INVALID_STATE&&!maintenance);busy=false;
    for(failure=1;failure<=4;++failure) {
        esp_err_t start=hub_ota_begin_update(4,&update);
        if(failure==1) { assert(start!=ESP_OK&&!maintenance);continue; }
        assert(start==ESP_OK&&maintenance);
        assert(hub_ota_begin_update(4,&second)==ESP_ERR_INVALID_STATE&&maintenance);
        esp_err_t write=hub_ota_write(&update,"abcd",4);
        if(failure==2) { assert(write!=ESP_OK);hub_ota_abort(&update); }
        else { assert(write==ESP_OK&&hub_ota_finish(&update)!=ESP_OK); }
        assert(!maintenance&&!update.active);
    }
    failure=0;
    assert(hub_ota_begin_update(4,&update)==ESP_OK);
    assert(hub_ota_write(&update,"abcde",5)==ESP_ERR_INVALID_SIZE);
    assert(hub_ota_finish(&update)==ESP_ERR_INVALID_SIZE&&!maintenance&&aborts==2);
    assert(hub_ota_begin_update(4,&update)==ESP_OK);
    assert(hub_ota_write(&update,"abcd",4)==ESP_OK&&hub_ota_finish(&update)==ESP_OK);
    assert(maintenance&&!update.active); // No new job between successful staging and reboot.
    return 0;
}
'''

PRESET = r'''
#include "hub_cloud.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct { bool dose1_set,dose2_set; double dose1,dose2; bool connected; } cloud_snapshot_t;
typedef void *esp_http_client_handle_t;
static cloud_snapshot_t fresh;
static bool locked,automatic;
static int reads,posts,updates;
static esp_err_t read_error,lock_error;
static int64_t esp_timer_get_time(void) { return 1000000; }
static const char *esp_err_to_name(esp_err_t e) { return e?"error":"ok"; }
static bool bbw_mode_is_valid(const char *m) { return !strcmp(m,"Dose1")||!strcmp(m,"Dose2")||!strcmp(m,"Continuous"); }
static void set_command_result(hub_cloud_command_result_t *r,bool ok,int64_t t,const char *m) { (void)t;(void)m;r->ok=ok; }
static esp_err_t ensure_cloud_controls_ready_or_fail(hub_cloud_command_result_t *r,int64_t t) { (void)r;(void)t;return ESP_OK; }
static bool hub_command_requires_online_machine(void) { return automatic; }
static esp_err_t cloud_command_io_lock(const hub_command_deadline_t *d,uint32_t reserve) { (void)d;(void)reserve;assert(!locked);locked=!lock_error;return lock_error; }
static void cloud_command_io_unlock(void) { assert(locked);locked=false; }
static void cloud_http_session_cleanup(esp_http_client_handle_t *session) { assert(locked&&session);*session=NULL; }
static esp_err_t read_control_snapshot_locked(const hub_command_deadline_t *d,uint32_t reserve,cloud_snapshot_t *out,esp_http_client_handle_t *session) { (void)d;(void)reserve;assert(locked&&session);++reads;*out=fresh;return read_error; }
static hub_cloud_command_attempt_t execute_command_with_auth_locked(const char *name,const char *body,const hub_command_deadline_t *d,uint32_t reserve,esp_http_client_handle_t *session) {
    (void)session;
    (void)d;(void)reserve;assert(locked&&strstr(name,"ChangeMode")&&strstr(body,"mode"));++posts;
    return (hub_cloud_command_attempt_t){.err=ESP_OK,.http_status=200,.request_started=true};
}
static hub_cloud_command_attempt_t execute_setting_command_with_auth(const char *name,const char *body,const hub_command_deadline_t *d,uint32_t reserve) {
    assert(!locked);locked=true;hub_cloud_command_attempt_t a=execute_command_with_auth_locked(name,body,d,reserve,NULL);locked=false;return a;
}
static void schedule_command_auth_recovery(const hub_cloud_command_attempt_t *a) { (void)a;assert(!locked); }
static void set_command_attempt(hub_cloud_command_result_t *r,const hub_cloud_command_attempt_t *a) { r->request_started=a->request_started; }
static void update_snapshot_bbw_mode(const char *mode) { (void)mode;assert(!locked);++updates; }
/* FUNCTIONS */
int main(void) {
    hub_cloud_command_result_t result;
    for(int scenario=0;scenario<10;++scenario) {
        fresh=(cloud_snapshot_t){true,true,30,40,true};read_error=lock_error=0;reads=posts=updates=0;automatic=scenario>=8;
        if(scenario==1)fresh.dose1=31;
        if(scenario==2)fresh.dose1_set=false;
        if(scenario==3)fresh.dose2=NAN;
        if(scenario==4)fresh.dose2=40.123;
        if(scenario==5)read_error=ESP_ERR_TIMEOUT;
        if(scenario==6)read_error=ESP_ERR_INVALID_STATE;
        if(scenario==7)lock_error=ESP_ERR_TIMEOUT;
        if(scenario==8)fresh.connected=false;
        esp_err_t error=hub_cloud_set_bbw_mode_checked_with_deadline("Dose1",30,false,NULL,0,&result);
        bool expected=scenario==0||scenario==9;
        assert(!locked&&reads==(scenario!=7)&&posts==expected&&updates==posts);
        assert((error==ESP_OK)==expected&&result.ok==expected&&result.preset_changed==(scenario==1));
    }
    automatic=false;lock_error=read_error=0;fresh=(cloud_snapshot_t){true,true,30,40,false};
    posts=0;
    assert(hub_cloud_set_bbw_mode_checked_with_deadline("Dose1",30,false,NULL,0,&result)==ESP_OK&&posts==1);
    for(int grams=30;grams<=50;grams+=10) {
        posts=0;
        esp_err_t error=hub_cloud_set_bbw_mode_checked_with_deadline("Dose1",grams,true,NULL,0,&result);
        assert(!locked&&posts==(grams==50)&&(error==ESP_OK)==(grams==50)&&result.preset_changed==(grams!=50));
    }
    automatic=true;posts=0;
    assert(hub_cloud_set_bbw_mode_checked_with_deadline("Dose1",50,true,NULL,0,&result)==ESP_ERR_INVALID_STATE&&posts==0&&!locked);
    automatic=false;
    reads=posts=0;
    assert(hub_cloud_set_bbw_mode_checked_with_deadline("Continuous",30,false,NULL,0,&result)==ESP_ERR_INVALID_ARG);
    assert(reads==0&&posts==0);
    assert(hub_cloud_set_bbw_mode_checked_with_deadline("Continuous",-1,false,NULL,0,&result)==ESP_OK&&reads==0&&posts==1);
    return 0;
}
'''


DASHBOARD = r'''
#define _POSIX_C_SOURCE 200809L
#include "hub_cloud.h"
#include "cJSON.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* TYPES */
static const char *json_optional_string(cJSON *j,const char *key) { cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);return cJSON_IsString(v)?v->valuestring:""; }
static bool json_optional_bool(cJSON *j,const char *key) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,key)); }
#define parse_coffee_boiler(...) ((void)0)
#define parse_steam_level(...) ((void)0)
#define parse_prebrew(...) ((void)0)
static int64_t esp_timer_get_time(void) { return 1000000; }
/* FUNCTIONS */
int main(void) {
    const char *connections[]={"true","false","null"};
    const char *modes[]={"StandBy","PoweredOn","Brewing"};
    for(unsigned i=0;i<3;++i) for(unsigned m=0;m<3;++m) {
        char body[256],name[80];cloud_snapshot_t previous={0},out;
        snprintf(body,sizeof(body),"{\"serialNumber\":\"SYNTHETIC\",\"connected\":%s,\"widgets\":[{\"code\":\"CMMachineStatus\",\"output\":{\"status\":\"%s\"}}]}",connections[i],modes[m]);
        assert(parse_dashboard_snapshot(body,"SYNTHETIC",1,&previous,&out,name,sizeof(name))==ESP_OK);
        assert(out.connected==(i==0||(i==2&&m!=0)));
    }
    const char *doses[]={"20.0","\"20.0\"","null"};
    for(unsigned i=0;i<3;++i) {
        char body[512],name[80];cloud_snapshot_t previous={0},out;
        snprintf(body,sizeof(body),"{\"serialNumber\":\"SYNTHETIC\",\"connected\":true,\"widgets\":[{\"code\":\"CMMachineStatus\",\"output\":{\"status\":\"StandBy\"}},{\"code\":\"CMBrewByWeightDoses\",\"output\":{\"scaleConnected\":false,\"availableModes\":[\"Continuous\"],\"mode\":\"Continuous\",\"doses\":{\"Dose1\":{\"dose\":%s},\"Dose2\":{\"dose\":30.0}}}}]}",doses[i]);
        assert(parse_dashboard_snapshot(body,"SYNTHETIC",1,&previous,&out,name,sizeof(name))==ESP_OK);
        assert(out.connected&&!out.bbw_scale_connected&&out.bbw_scale_connected_set);
        assert(out.dose1_set==(i<2)&&out.dose2_set&&out.dose2==30);
        if(i<2)assert(out.dose1==20);
        assert(!strcmp(out.bbw_mode,"Continuous"));
    }
    const char *batteries[]={"0","1","100","\"1\"","100.1","-1","null","true","\"nan\"","\"inf\""};
    const double expected[]={0,1,100,1};
    for(unsigned i=0;i<sizeof(batteries)/sizeof(batteries[0]);++i) {
        char body[512],name[80];cloud_snapshot_t previous={0},out;
        snprintf(body,sizeof(body),"{\"serialNumber\":\"SYNTHETIC\",\"widgets\":[{\"code\":\"ThingScale\",\"output\":{\"connected\":false,\"batteryLevel\":%s}}]}",batteries[i]);
        assert(parse_dashboard_snapshot(body,"SYNTHETIC",1,&previous,&out,name,sizeof(name))==ESP_OK);
        assert(out.thing_scale_connected_set&&!out.thing_scale_connected);
        assert(out.scale_battery_level_set==(i<4));
        if(i<4)assert(out.scale_battery_level==expected[i]);
        assert(out.scale_observed_ms==1000); // An explicit connection observation dates this response.
        previous=out; previous.scale_observed_ms=1000;
        assert(parse_dashboard_snapshot("{\"widgets\":[]}","SYNTHETIC",1,&previous,&out,name,sizeof(name))==ESP_OK);
        assert(!out.thing_scale_connected_set&&!out.scale_battery_level_set&&out.scale_observed_ms==0);
    }
    return 0;
}
'''

CONTROL_READ = r'''
#include "hub_cloud.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CUSTOMER_APP_URL "https://synthetic.example.test"
#define READONLY_DIAG_DASHBOARD 0
#define CLOUD_READONLY_HTTP_TIMEOUT_MS 4000
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define HTTP_METHOD_GET 0
#define ESP_ERR_HTTP_CONNECT 0x7001
#define ESP_ERR_HTTP_FETCH_HEADER 0x7002
#define pdMS_TO_TICKS(x) (x)
enum { TIMING_READ_CONTEXT, TIMING_READ_PARSE, TIMING_RETRY };
static void cloud_timing_add(int phase,int64_t started_us) { (void)phase;(void)started_us; }
typedef struct { int unused; } installation_key_t;
typedef struct { char access_token[1]; } cloud_tokens_t;
typedef struct { int parsed; } cloud_snapshot_t;
typedef struct { char *data; size_t len; } http_response_t;
typedef void *esp_http_client_handle_t;
struct cJSON { int unused; };
static int calls,scenario,invalidations,publishes;
static int64_t clock_ms;
static bool rebound;
static int64_t esp_timer_get_time(void) { return clock_ms*1000; }
static void vTaskDelay(unsigned ms) { clock_ms+=ms; }
static void *heap_caps_calloc(size_t n,size_t s,int caps) { (void)caps;return calloc(n,s); }
static bool active_command_target(hub_machine_selection_identity_t *t) { strcpy(t->serial_number,"SYNTHETIC");t->generation=1;return true; }
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *t) { assert(t->generation==1);return !rebound; }
static esp_err_t load_cloud_context(hub_cloud_settings_t *s,installation_key_t *k,cloud_tokens_t *t) { assert(s&&k&&t);return ESP_OK; }
static void free_cloud_context(hub_cloud_settings_t *s,installation_key_t *k,cloud_tokens_t *t) { free(s);free(k);free(t); }
static void hub_http_response_reset(http_response_t *r) { free(r->data);memset(r,0,sizeof(*r)); }
static void record_readonly_http_diag(int k,esp_err_t e,int s,int d,size_t n,int b) { (void)k;(void)e;(void)s;(void)d;(void)n;(void)b; }
static bool cloud_http_retryable_error(esp_err_t e) { return e==ESP_ERR_HTTP_CONNECT||e==ESP_ERR_HTTP_FETCH_HEADER||e==ESP_ERR_TIMEOUT; }
static void invalidate_cached_tokens(void) { ++invalidations; }
static void schedule_command_auth_recovery(const hub_cloud_command_attempt_t *a) { (void)a; }
static bool publish_scale_snapshot(const cloud_snapshot_t *s) { assert(s->parsed==1&&!rebound);++publishes;return true; }
static cJSON *cJSON_Parse(const char *s) { static cJSON j;return s?&j:NULL; }
static void cJSON_Delete(cJSON *j) { (void)j; }
static const char *json_optional_string(cJSON *j,const char *key) { assert(j&&!strcmp(key,"serialNumber"));return scenario==7?"OTHER":"SYNTHETIC"; }
static esp_err_t parse_dashboard_snapshot(const char *s,const char *serial,uint32_t generation,const cloud_snapshot_t *before,cloud_snapshot_t *out,char *name,size_t size) {
    (void)name;(void)size;assert(s&&!strcmp(serial,"SYNTHETIC")&&generation==1&&before->parsed==0);out->parsed=1;return ESP_OK;
}
static esp_err_t cloud_http_request_no_retry(const char *url,int method,const installation_key_t *key,const char *token,const char *body,http_response_t *r,int *status,int timeout,esp_http_client_handle_t *session,bool *request_started) {
    assert(!session&&!request_started);
    assert(strstr(url,"/things/SYNTHETIC/dashboard")&&method==HTTP_METHOD_GET&&key&&token&&!body&&timeout>0);
    ++calls;clock_ms+=scenario==4?timeout:1000;*status=0;
    if(scenario==6)rebound=true;
    if(scenario==3) { *status=401;return ESP_OK; }
    if(scenario==5)return ESP_ERR_NO_MEM;
    if((scenario==1&&calls==1)||scenario==2||scenario==4||scenario==6)return scenario==2?ESP_ERR_HTTP_FETCH_HEADER:ESP_ERR_HTTP_CONNECT;
    r->data=malloc(3);strcpy(r->data,"{}");r->len=2;*status=200;return ESP_OK;
}
/* FUNCTIONS */
int main(void) {
    for(scenario=0;scenario<8;++scenario) {
        calls=invalidations=publishes=0;rebound=false;clock_ms=1000;
        hub_command_deadline_t d={.enforced=true,.client_ttl_present=true,.client_ttl_ms=scenario==4?3000:8000};
        assert(hub_command_deadline_accept(&d,clock_ms));
        cloud_snapshot_t out;
        esp_err_t error=read_control_snapshot_locked(&d,0,&out,NULL);
        assert(calls==((scenario==1||scenario==2)?2:1));
        assert((error==ESP_OK)==(scenario<2));
        assert(out.parsed==(scenario<2)&&invalidations==(scenario==3));
        assert(publishes==(scenario<2));
        assert(clock_ms<d.deadline_at_ms);
    }
    calls=0;cloud_snapshot_t out;
    assert(read_control_snapshot_locked(NULL,UINT32_MAX,&out,NULL)==ESP_ERR_INVALID_STATE&&calls==0);
    return 0;
}
'''

GRINDER_IO = r'''
#include "hub_cloud.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
struct cJSON { int unused; };
static struct { char serial[65],grinder_host[16]; } gbs_config={"SYNTHETIC","192.0.2.1"};
static struct { char operation[24];int http_status;esp_err_t transport; } gbs_grinder_request;
static int calls,ends;
static bool busy,wrong,fail,locked,rebound;
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *s) { strcpy(s->serial_number,wrong?"OTHER":"SYNTHETIC");return ESP_OK; }
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *s) { assert(!strcmp(s->serial_number,"SYNTHETIC"));return !rebound; }
esp_err_t hub_cloud_background_io_begin(void) { assert(!locked);locked=!busy;return busy?ESP_ERR_TIMEOUT:ESP_OK; }
void hub_cloud_background_io_end(void) { assert(locked);locked=false;++ends; }
static esp_err_t request(const char *url,const cJSON *body,int *status,cJSON **reply) {
    assert(locked&&strstr(url,"/grinderApi/brewDone")&&body);++calls;*status=200;static cJSON r;*reply=&r;return fail?ESP_ERR_TIMEOUT:ESP_OK;
}
static bool string_field(const cJSON *j,const char *key,char *out,size_t n,bool empty) { (void)n;(void)empty;assert(j&&!strcmp(key,"status"));strcpy(out,"Ok");return true; }
static bool gbs_reply_valid(int http_status,const char *status) { return http_status==200&&!strcmp(status,"Ok"); }
/* FUNCTIONS */
int main(void) {
    cJSON body={0},*reply;char status[80];busy=true;
    assert(gbs_grinder("brewDone",&body,&reply,status)==ESP_ERR_TIMEOUT&&!strcmp(status,"LocalBusy")&&calls==0&&ends==0&&!reply);
    busy=false;assert(gbs_grinder("brewDone",&body,&reply,status)==ESP_OK&&calls==1&&ends==1&&!locked);
    fail=true;assert(gbs_grinder("brewDone",&body,&reply,status)==ESP_ERR_TIMEOUT&&calls==2&&ends==2&&!locked);
    wrong=true;assert(gbs_grinder("brewDone",&body,&reply,status)==ESP_ERR_INVALID_STATE&&calls==2&&ends==2&&!reply);
    wrong=false;rebound=true;assert(gbs_grinder("brewDone",&body,&reply,status)==ESP_ERR_INVALID_STATE&&calls==2&&ends==3&&!locked&&!reply);
    return 0;
}
'''

LATEST = r'''
#include "hub_cloud.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CUSTOMER_APP_URL "https://synthetic.example.test"
#define READONLY_DIAG_RECENT 1
typedef struct { int unused; } installation_key_t;
typedef struct { int unused; } cloud_tokens_t;
static bool interactive,rebind;
static int reads,loads,refreshes,cleanups;
static esp_err_t context_error;
static bool hub_command_interactive_pending(void) { return interactive; }
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *s) { strcpy(s->serial_number,"SYNTHETIC");s->generation=1;return ESP_OK; }
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *s) { assert(s->generation==1);return !rebind; }
static esp_err_t load_cloud_context(hub_cloud_settings_t *s,installation_key_t *k,cloud_tokens_t *t) { assert(s&&k&&t);++loads;return context_error; }
esp_err_t hub_cloud_request_refresh(void) { ++refreshes;return ESP_OK; }
static void free_cloud_context(hub_cloud_settings_t *s,installation_key_t *k,cloud_tokens_t *t) { free(s);free(k);free(t);++cleanups; }
static esp_err_t cloud_fetch_json_text(const char *url,installation_key_t *k,cloud_tokens_t *t,int kind,char **body) {
    assert(k&&t&&kind==READONLY_DIAG_RECENT&&strstr(url,"/things/SYNTHETIC/stats/LAST_COFFEE/1?days=1"));
    ++reads;*body=malloc(3);assert(*body);memcpy(*body,"{}",3);return ESP_OK;
}
/* FUNCTIONS */
int main(void) {
    char *body=NULL;
    assert(hub_cloud_latest_coffee_json("SYNTHETIC",&body)==ESP_OK&&reads==1&&cleanups==1);free(body);
    interactive=true;assert(hub_cloud_latest_coffee_json("SYNTHETIC",&body)==ESP_ERR_TIMEOUT&&!body&&reads==1&&loads==1);
    interactive=false;assert(hub_cloud_latest_coffee_json("OTHER",&body)==ESP_ERR_INVALID_STATE&&reads==1&&loads==1);
    context_error=ESP_ERR_INVALID_STATE;
    assert(hub_cloud_latest_coffee_json("SYNTHETIC",&body)==ESP_ERR_INVALID_STATE&&refreshes==1&&cleanups==2&&reads==1);
    context_error=ESP_OK;rebind=true;
    assert(hub_cloud_latest_coffee_json("SYNTHETIC",&body)==ESP_ERR_INVALID_STATE&&!body&&reads==2&&cleanups==3);
    return 0;
}
'''

READINESS = r'''
#include <stdbool.h>
#include <assert.h>
#include <string.h>
static bool configured,authenticated,cached;
static const char *status="idle";
static bool hub_cloud_configured(void) { return configured; }
static bool hub_cloud_authenticated(void) { return authenticated; }
static bool hub_cloud_has_cached_snapshot(void) { return cached; }
static const char *hub_cloud_status_string(void) { return status; }
/* FUNCTIONS */
int main(void) {
    assert(!strcmp(hub_cloud_command_readiness(),"unconfigured"));
    configured=true;
    assert(!strcmp(hub_cloud_command_readiness(),"warming"));
    authenticated=true;
    assert(!strcmp(hub_cloud_command_readiness(),"warming"));
    cached=true;
    assert(!strcmp(hub_cloud_command_readiness(),"ready"));
    status="error";
    assert(!strcmp(hub_cloud_command_readiness(),"degraded"));
    configured=false;
    assert(!strcmp(hub_cloud_command_readiness(),"unconfigured"));
    return 0;
}
'''

DISPATCH = r'''
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t;
typedef struct { void *user_ctx; bool secure,v2; } httpd_req_t;
typedef struct { esp_err_t (*handler)(httpd_req_t *); } hub_http_route_t;
#define HUB_TOUCHSCREEN 1
#define HUB_API_V2_HEADER_NAME "X-Hub-Api-Version"
#define portMAX_DELAY 0
#define pdTRUE 1
static int route_dispatch_mutex,takes,gives;
static bool locked;
static bool hub_http_request_is_secure(httpd_req_t *r) { return r->secure; }
static bool request_header_matches(httpd_req_t *r,const char *a,const char *b) { (void)a;(void)b;return r->v2; }
static int hub_routes_send_internal_error(httpd_req_t *r,const char *s) { (void)r;(void)s;return 500; }
static int hub_routes_send_service_unavailable(httpd_req_t *r,const char *s) { (void)r;(void)s;return 503; }
static int hub_routes_send_upgrade_required(httpd_req_t *r) { (void)r;return 426; }
static int xSemaphoreTake(int mutex,int wait) { (void)mutex;(void)wait;assert(!locked);locked=true;++takes;return 1; }
static void xSemaphoreGive(int mutex) { (void)mutex;assert(locked);locked=false;++gives; }
static int display_frame_handler(httpd_req_t *r) { (void)r;assert(!locked);return 200; }
static int normal_handler(httpd_req_t *r) { (void)r;assert(locked);return 201; }
/* FUNCTIONS */
int main(void) {
    hub_http_route_t normal={normal_handler},frame={display_frame_handler};
    httpd_req_t r={.user_ctx=&normal};
    assert(dispatch_route(&r)==201&&takes==1&&gives==1);
    r.user_ctx=&frame;
    assert(dispatch_route(&r)==200&&takes==1&&gives==1);
    r.v2=true;
    assert(dispatch_route(&r)==426&&takes==1);
    r.secure=true;
    assert(dispatch_route(&r)==200&&takes==1);
    r.user_ctx=&normal;
    assert(dispatch_route(&r)==201&&takes==2&&gives==2);
    r.user_ctx=NULL;
    assert(dispatch_route(&r)==500&&!locked);
    return 0;
}
'''

TOUCH = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
enum { WIDTH=240,HEIGHT=280 };
typedef struct { int16_t x,y; } lv_point_t;
/* FUNCTIONS */
int main(void) {
    lv_point_t point;
    assert(map_touch(0,0,&point)&&point.x==239&&point.y==279);
    assert(map_touch(239,0,&point)&&point.x==0&&point.y==279);
    assert(map_touch(0,279,&point)&&point.x==239&&point.y==0);
    assert(map_touch(239,279,&point)&&point.x==0&&point.y==0);
    assert(map_touch(59,189,&point)&&point.x==180&&point.y==90);
    assert(!map_touch(240,0,&point)&&point.x==180&&point.y==90);
    assert(!map_touch(0,280,&point)&&point.x==180&&point.y==90);
    assert(!map_touch(UINT16_MAX,0,&point));
    return 0;
}
'''


def compile_run(directory, name, source, extra=()):
    path=directory/(name+'.c');path.write_text(source)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-variable','-fsanitize=address,undefined','-g','-DHUB_GRINDER=1',
                    '-I',str(directory),'-I',str(ROOT/'main'),str(path),*map(str,extra),'-lm','-o',str(directory/name)],check=True)
    subprocess.run([str(directory/name)],check=True)


def provisioning():
    spec=importlib.util.spec_from_file_location('prepare',ROOT/'tools/prepare-integrated-nvs.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    config=dict(wifi_ssid='Synthetic',wifi_password='',serial='Synthetic-1',
                cloud_username='synthetic@example.test',cloud_password='synthetic',
                installation_id='11111111-1111-4111-8111-111111111111',
                private_key=base64.b64encode(b'x'*32).decode(),secret=base64.b64encode(b'y'*32).decode(),
                grinder_host='192.0.2.1',grinder_cert='-----BEGIN CERTIFICATE-----\nsynthetic\n-----END CERTIFICATE-----',
                brew_device_id='22222222-2222-4222-8222-222222222222',pairing_id=42)
    wifi=module.rows(config,'0'*32)
    assert {r[0] for r in wifi}=={'key','lamabuddy','wifi_ssid','wifi_pass','local_token'}
    full=module.rows(config,'0'*32,True,True)
    assert full[-1]==['active','data','u8',0]
    assert len([r for r in full if r[0]=='cloud_inst_key'])==1
    for key,value in [('serial','unsafe/path'),('pairing_id',True),('secret','invalid'),('grinder_cert',''),('wifi_ssid','x'*33)]:
        invalid={**config,key:value}
        try: module.rows(invalid,'0'*32,True,True)
        except ValueError: pass
        else: raise AssertionError('accepted invalid '+key)

    # Exercise the actual generator against both partition layouts, without hardware.
    with tempfile.TemporaryDirectory(prefix='gbs-private-nvs-') as temporary:
        directory=Path(temporary)
        source=directory/'source.json';source.write_text(json.dumps(config))
        for profile,size in [('touchscreen',0x15000),('amoled216',0x15000),('headless',0x6000)]:
            output=directory/profile
            result=subprocess.run([sys.executable,str(ROOT/'tools/prepare-integrated-nvs.py'),
                                   str(source),str(output),'--profile',profile,
                                   '--include-cloud','--include-grinder'],capture_output=True,text=True)
            assert result.returncode==0,result.stderr
            assert (output/'nvs.bin').stat().st_size==size
            assert (output/'nvs.csv').stat().st_mode&0o077==0
            assert (output/'hub-token.txt').stat().st_mode&0o077==0
            assert 'active,data,u8,0' in (output/'nvs.csv').read_text()
            assert config['cloud_username'] not in result.stdout+result.stderr
            assert (output/'hub-token.txt').read_text().strip() not in result.stdout+result.stderr


def main():
    provisioning()
    with tempfile.TemporaryDirectory(prefix='hub-integrated-support-') as temporary:
        directory=Path(temporary)
        (directory/'esp_err.h').write_text(ERRORS.replace('const char *esp_err_to_name(esp_err_t error);',''))
        (directory/'esp_ota_ops.h').write_text(OTA_HEADER)
        (directory/'esp_app_desc.h').write_text('#include "esp_ota_ops.h"\n')
        (directory/'esp_log.h').write_text('#include "esp_err.h"\nconst char *esp_err_to_name(esp_err_t e);\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n')
        compile_run(directory,'ota',OTA,[ROOT/'main/hub_ota.c'])
        cloud=(ROOT/'main/hub_cloud.c').read_text()
        types='\n'.join(re.search(r'typedef struct\s*\{[^}]+\}\s*'+name+r';',cloud).group() for name in ['cloud_snapshot_t','bbw_snapshot_fields_t'])
        pieces=[]
        for name in ['parse_double_string','json_item_to_double','json_number_to_double','parse_machine_status','parse_brew_by_weight','parse_thing_scale','preserve_missing_bbw_fields','parse_dashboard_snapshot']:
            signature=re.search(r'static (?:bool|void|esp_err_t) '+name+r'\([^;{]*\)\s*\{',cloud)
            pieces.append(signature.group()+function_body(cloud,name)+'}')
        json_dir=Path(os.environ.get('IDF_PATH',str(Path.home()/'esp/esp-idf')))/'components/json/cJSON'
        compile_run(directory,'dashboard',DASHBOARD.replace('/* TYPES */',types).replace('/* FUNCTIONS */','\n'.join(pieces)),['-I',json_dir,json_dir/'cJSON.c','-Wno-deprecated-declarations'])
        pieces=[]
        for name in ['control_decimal_value_is_valid','hub_cloud_set_bbw_mode_checked_with_deadline']:
            signature=re.search(r'(?:static )?(?:bool|esp_err_t) '+name+r'\([^;{]*\)\s*\{',cloud)
            assert signature
            pieces.append(signature.group()+function_body(cloud,name)+'}')
        compile_run(directory,'preset',PRESET.replace('/* FUNCTIONS */','\n'.join(pieces)))
        name='read_control_snapshot_locked'
        signature=re.search(r'static esp_err_t '+name+r'\([^;{]*\)\s*\{',cloud)
        compile_run(directory,'control_read',CONTROL_READ.replace('/* FUNCTIONS */',signature.group()+function_body(cloud,name)+'}'),[ROOT/'main/hub_command_deadline.c'])
        network=(ROOT/'integrated/main/network.c').read_text()
        name='gbs_grinder'
        signature=re.search(r'esp_err_t '+name+r'\([^;{]*\)\s*\{',network)
        compile_run(directory,'grinder_io',GRINDER_IO.replace('/* FUNCTIONS */',signature.group()+function_body(network,name)+'}'))
        name='hub_cloud_latest_coffee_json'
        signature=re.search(r'esp_err_t '+name+r'\([^;{]*\)\s*\{',cloud)
        compile_run(directory,'latest',LATEST.replace('/* FUNCTIONS */',signature.group()+function_body(cloud,name)+'}'))
        name='hub_cloud_command_readiness'
        signature=re.search(r'const char \*'+name+r'\(void\)\s*\{',cloud)
        compile_run(directory,'readiness',READINESS.replace('/* FUNCTIONS */',signature.group()+function_body(cloud,name)+'}'))
        http=(ROOT/'main/hub_http.c').read_text()
        name='dispatch_route'
        signature=re.search(r'static esp_err_t '+name+r'\([^;{]*\)\s*\{',http)
        compile_run(directory,'dispatch',DISPATCH.replace('/* FUNCTIONS */',signature.group()+function_body(http,name)+'}'))
        display=(ROOT/'integrated/display/gbs_display.c').read_text()
        name='map_touch'
        signature=re.search(r'static bool '+name+r'\([^;{]*\)\s*\{',display)
        compile_run(directory,'touch',TOUCH.replace('/* FUNCTIONS */',signature.group()+function_body(display,name)+'}'))
    print('Integrated support PASS: OTA lifecycle, fresh preset guards, shared Cloud reads/rebind/backpressure/readiness, private provisioning')

if __name__=='__main__': main()
