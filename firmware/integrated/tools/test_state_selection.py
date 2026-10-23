#!/usr/bin/env python3
"""Execute the real keeper loop and durable display-selection code with host fakes."""
import os
import re
import tempfile
from pathlib import Path
from test_integrated_support import ROOT, ERRORS, compile_run
from validate_stability_foundation import function_body


def functions(source, names):
    result = []
    for name in names:
        signature = re.search(r"(?:static )?(?:bool|void|esp_err_t|uint32_t) " + name + r"\([^;{]*\)\s*\{", source)
        assert signature, name
        result.append(signature.group() + function_body(source, name) + "}")
    return "\n".join(result)


KEEPER = r'''
#include "hub_cloud.h"
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <string.h>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
static jmp_buf stop;
static int64_t clock_ms, stop_ms, ble_after, refresh_times[32];
static int64_t observed_ms, control_observation_ms, token_due_ms;
static int refreshes, scale_reads;
static bool cloud_online, recover_cloud, complete_snapshot, full_observes_scale;
static bool wifi_available, refresh_pending;
#ifdef HUB_AMOLED_216
static bool interactive_deferred, grinder_busy, scale_attempted, scale_observes;
static esp_err_t scale_error, identity_error;
static int64_t scale_times[32], scale_http_ms;
static hub_machine_selection_identity_t binding;
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *out)
{ *out=binding;return identity_error; }
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *target)
{ return target->generation==binding.generation&&!strcmp(target->serial_number,binding.serial_number); }
bool hub_command_idle_background_deferred(void) { return interactive_deferred; }
bool hub_grinder_busy(void) { return grinder_busy; }
bool hub_cloud_has_fresh_scale_battery(void)
{ return cloud_online&&observed_ms>0&&clock_ms>=observed_ms&&clock_ms-observed_ms<60000; }
#endif
static int64_t now_ms(void) { return clock_ms; }
static bool hub_wifi_station_has_ip(void) { return wifi_available; }
static bool hub_ble_has_cached_state(void) { return clock_ms >= ble_after; }
static void read_keeper_state(int64_t *activity,int64_t *active,bool *recovery)
{ *activity=1; *active=300000; *recovery=false; }
static bool hub_state_process_pending_slow_cloud_read(void) { return false; }
static bool consume_recovery_request(void) { return false; }
static void extend_active_window(void) {}
static void hub_state_keeper_request_recovery(void) { assert(false); }
static void vTaskDelay(uint32_t ms) { clock_ms+=ms; }
static int xTaskNotifyWait(uint32_t a,uint32_t b,uint32_t *bits,uint32_t ms) {
    (void)a;(void)b;(void)bits;assert(ms>0&&ms<=30000);
    if(control_observation_ms>clock_ms&&control_observation_ms<=clock_ms+ms) {
        clock_ms=control_observation_ms;observed_ms=clock_ms;control_observation_ms=0;
    } else clock_ms+=ms;
    if(clock_ms>stop_ms)longjmp(stop,1);
    return 0;
}
void hub_cloud_get_snapshot_status(hub_cloud_snapshot_status_t *s)
{ *s=(hub_cloud_snapshot_status_t){.has_cached_snapshot=true,.has_complete_bbw=complete_snapshot,
    .is_connected=cloud_online,.scale_observed_ms=observed_ms,
    .is_ready=cloud_online&&complete_snapshot,.refresh_pending=refresh_pending}; }
int64_t hub_cloud_token_renewal_due_ms(void) { return token_due_ms; }
uint32_t hub_cloud_refresh_request_generation(void) { return 0; }
void hub_cloud_complete_refresh_request(uint32_t ticket) { (void)ticket; }
bool hub_cloud_refresh_is_running(void) { return false; }
esp_err_t hub_cloud_refresh(void) {
    assert(refreshes<32); refresh_times[refreshes++]=clock_ms;
    if(recover_cloud&&hub_ble_has_cached_state()) cloud_online=true;
    if(full_observes_scale)observed_ms=clock_ms;
    if(token_due_ms&&clock_ms>=token_due_ms)token_due_ms=0;
    return ESP_OK;
}
esp_err_t hub_cloud_refresh_scale(bool *attempted) {
#ifdef HUB_AMOLED_216
    assert(scale_reads<32);scale_times[scale_reads++]=clock_ms;
    *attempted=scale_attempted;
    clock_ms+=scale_http_ms;
    if(scale_observes)observed_ms=clock_ms;
    return scale_error;
#else
    *attempted=true;++scale_reads;return ESP_OK;
#endif
}
/* FUNCTIONS */
static void setup(bool online,bool recover,int64_t local_after) {
    clock_ms=1000;stop_ms=350000;refreshes=scale_reads=0;
    observed_ms=control_observation_ms=token_due_ms=0;
    complete_snapshot=full_observes_scale=true;
    cloud_online=online;recover_cloud=recover;ble_after=local_after;
    wifi_available=true;refresh_pending=false;
#ifdef HUB_AMOLED_216
    interactive_deferred=grinder_busy=scale_observes=false;scale_attempted=true;
    scale_error=identity_error=ESP_OK;
    scale_http_ms=0;
    binding=(hub_machine_selection_identity_t){"SYNTHETIC",1};
#endif
}
static void execute(void) { if(!setjmp(stop))hub_state_keeper_task(NULL); }
static void run(bool online,bool recover,int64_t local_after) {
    setup(online,recover,local_after);execute();
}
#ifdef HUB_AMOLED_216
static void amoled_scale_battery_cases(void) {
    // The initial ordinary observation is latched long before the first 90-second due time.
    setup(true,false,0);stop_ms=350000;execute();
    assert(refreshes==1&&scale_reads==0&&clock_ms-observed_ms>60000);

    setup(true,false,0);full_observes_scale=false;execute();
    assert(refreshes==1&&scale_reads==2&&scale_times[0]==92000&&scale_times[1]==182000);
    // Network failures, including post-perform OOM/rebind, consume the same bounded budget.
    const esp_err_t network_errors[]={ESP_ERR_TIMEOUT,ESP_ERR_NO_MEM,ESP_ERR_INVALID_STATE,ESP_ERR_INVALID_RESPONSE};
    for(unsigned i=0;i<sizeof(network_errors)/sizeof(*network_errors);++i) {
        setup(true,false,0);full_observes_scale=false;scale_error=network_errors[i];execute();
        assert(scale_reads==2&&refreshes==1);
    }
    setup(true,false,0);full_observes_scale=false;scale_observes=true;execute();
    assert(scale_reads==1&&refreshes==1);
    setup(true,false,0);full_observes_scale=false;control_observation_ms=27000;execute();
    assert(scale_reads==0&&refreshes==1&&observed_ms==27000);

    // Auth renewal retains priority even while the accessory budget is exhausted or deferred.
    setup(true,false,0);full_observes_scale=false;token_due_ms=220000;execute();
    assert(scale_reads==2&&refreshes==2&&refresh_times[1]==220000);
    setup(true,false,0);full_observes_scale=false;interactive_deferred=true;token_due_ms=92000;execute();
    assert(scale_reads==0&&refreshes==2&&refresh_times[1]==92000);
    setup(true,false,0);full_observes_scale=false;grinder_busy=true;token_due_ms=92000;execute();
    assert(scale_reads==0&&refreshes==2&&refresh_times[1]==92000);

    // A blocking GET may outlive its nominal timeout: renew immediately on return, without stale-now sleep.
    setup(true,false,0);full_observes_scale=false;token_due_ms=100000;scale_http_ms=10000;stop_ms=130000;execute();
    assert(scale_reads==1&&scale_times[0]==92000&&refreshes==2&&refresh_times[1]==102000);
    // Five seconds is an inclusive auth reserve; one millisecond outside it may start a read.
    for(int offset=-1;offset<=1;++offset) {
        setup(true,false,0);full_observes_scale=false;token_due_ms=97000+offset;
        scale_http_ms=6000;stop_ms=125000;execute();
        assert(scale_reads==1&&refreshes==2);
        if(offset<=0)assert(refresh_times[1]==97000+offset&&scale_times[0]>=refresh_times[1]);
        else assert(scale_times[0]==92000&&refresh_times[1]==98000);
    }

    // Exercise admission and throttling with the real tick, without changing the full-refresh cadence.
    setup(true,false,0);full_observes_scale=false;
    scale_battery_reads_t reads={0};hub_cloud_snapshot_status_t status={.is_ready=true};
    scale_battery_tick(&reads,&status,false);assert(reads.next_ms==91000&&reads.attempts==0);
    clock_ms=90999;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);
    clock_ms=91000;
    scale_battery_tick(&reads,&status,true);assert(scale_reads==0); // Full/token refresh is due.
    status.is_ready=false;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);status.is_ready=true;
    status.refresh_pending=true;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);status.refresh_pending=false;
    wifi_available=false;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);wifi_available=true;
    interactive_deferred=true;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);interactive_deferred=false;
    grinder_busy=true;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);grinder_busy=false;
    identity_error=ESP_FAIL;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);identity_error=ESP_OK;
    scale_attempted=false;scale_error=ESP_ERR_NO_MEM;
    scale_battery_tick(&reads,&status,false);
    assert(scale_reads==1&&reads.attempts==0&&reads.next_ms==121000);
    clock_ms=120999;scale_battery_tick(&reads,&status,false);assert(scale_reads==1);
    clock_ms=121000;scale_error=ESP_ERR_INVALID_STATE;scale_battery_tick(&reads,&status,false);
    assert(scale_reads==2&&reads.attempts==0&&reads.next_ms==151000);
    clock_ms=151000;scale_attempted=true;scale_error=ESP_ERR_TIMEOUT;scale_battery_tick(&reads,&status,false);
    assert(scale_reads==3&&reads.attempts==1&&reads.next_ms==241000);
    clock_ms=241000;scale_error=ESP_ERR_INVALID_STATE;scale_battery_tick(&reads,&status,false);
    assert(scale_reads==4&&reads.attempts==2);
    clock_ms=999000;scale_battery_tick(&reads,&status,false);assert(scale_reads==4);

    // Generation and serial changes independently reset the observation latch and attempt budget.
    ++binding.generation;scale_battery_tick(&reads,&status,false);
    assert(reads.attempts==0&&!reads.observed&&reads.next_ms==1089000&&scale_reads==4);
    observed_ms=clock_ms;scale_battery_tick(&reads,&status,false);assert(reads.observed);
    clock_ms+=200000;scale_battery_tick(&reads,&status,false);assert(scale_reads==4);
    observed_ms=0;strcpy(binding.serial_number,"OTHER");scale_battery_tick(&reads,&status,false);
    assert(!reads.observed&&reads.attempts==0&&reads.next_ms==clock_ms+90000);
    clock_ms=reads.next_ms;scale_error=ESP_OK;scale_observes=true;scale_battery_tick(&reads,&status,false);
    assert(scale_reads==5&&reads.observed&&reads.attempts==1);
}
#endif
int main(void) {
    assert(ready_interval_ms(100,1,300000,false)==600000);
    assert(ready_interval_ms(400000,1,300000,false)==1800000);
    assert(ready_interval_ms(2000000,1,300000,false)==3600000);
    run(false,true,150000);
    assert(refreshes==2&&cloud_online&&refresh_times[1]>=150000&&refresh_times[1]<=180000&&scale_reads==0);
    run(false,false,0);
    assert(refreshes==6&&scale_reads==0); // Local evidence retains only the existing connection-recovery cadence.
    for(int i=1;i<refreshes;++i)assert(refresh_times[i]-refresh_times[i-1]==60000);
    run(false,false,INT64_MAX);
    assert(refreshes==1&&scale_reads==0); // Offline without local evidence stays quiet.
    setup(true,false,0);complete_snapshot=false;execute();
    assert(scale_reads==0); // Incomplete setup does not start accessory polling.

    setup(true,false,0);stop_ms=125000;execute();
    assert(refreshes==1&&scale_reads==0);
    assert(clock_ms-observed_ms>60000); // Expiry does not initiate another network request.

    setup(true,false,0);stop_ms=125000;control_observation_ms=27000;execute();
    assert(refreshes==1&&scale_reads==0&&observed_ms==27000);

    setup(true,false,0);stop_ms=1360000;execute();
    assert(refreshes==1&&scale_reads==0); // Scale age cannot shorten the mature keeper cadence.

    setup(true,false,0);stop_ms=65000;token_due_ms=32000;execute();
    assert(refreshes==2&&refresh_times[1]==32000);
    assert(scale_reads==0);

    setup(true,false,0);stop_ms=65000;full_observes_scale=false;execute();
    assert(refreshes==1&&scale_reads==0&&observed_ms==0); // Missing scale data stays unknown without polling.
#ifdef HUB_AMOLED_216
    amoled_scale_battery_cases();
#endif
    return 0;
}
'''

SELECTION = r'''
#include "hub_config.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <time.h>
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
static int machine_selection_mutex, writes, commits;
static bool locked, have_record;
static esp_err_t read_error, write_error;
static hub_machine_selection_identity_t machine_selection_identity={"SYNTHETIC",1};
static hub_bbw_selection_t stored;
static esp_err_t ensure_machine_selection_mutex(void) { return ESP_OK; }
static esp_err_t load_machine_selection_identity_locked(void) { assert(locked);return ESP_OK; }
static int xSemaphoreTake(int m,int timeout) { (void)m;(void)timeout;assert(!locked);locked=true;return pdTRUE; }
static void xSemaphoreGive(int m) { (void)m;assert(locked);locked=false; }
static esp_err_t open_config(int mode,nvs_handle_t *h) { (void)mode;*h=1;return ESP_OK; }
static void nvs_close(nvs_handle_t h) { assert(h==1); }
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size) {
    assert(h==1&&!strcmp(key,"bbw_select_v1"));
    if(read_error)return read_error;
    if(!have_record)return ESP_ERR_NVS_NOT_FOUND;
    assert(*size>=sizeof(stored));memcpy(out,&stored,sizeof(stored));*size=sizeof(stored);return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size) {
    assert(h==1&&!strcmp(key,"bbw_select_v1")&&size==sizeof(stored));++writes;
    if(write_error)return write_error;
    memcpy(&stored,in,size);have_record=true;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) { assert(h==1);++commits;return ESP_OK; }
/* FUNCTIONS */
int main(void) {
    const hub_machine_selection_identity_t target={"SYNTHETIC",1},other={"OTHER",1},rebound={"SYNTHETIC",2};
    hub_bbw_selection_t read;
    double t=(double)time(NULL)-100;
    assert(hub_config_save_bbw_selection(&target,"Dose2",t)==ESP_OK&&writes==1&&commits==1);
    assert(hub_config_get_bbw_selection(&target,&read)==ESP_OK&&!strcmp(read.mode,"Dose2")&&read.confirmed_at==t);
    assert(hub_config_save_bbw_selection(&target,"Dose2",t)==ESP_OK&&writes==1); // Replay does not wear Flash.
    assert(hub_config_save_bbw_selection(&target,"Dose1",t)==ESP_ERR_INVALID_STATE);
    assert(hub_config_save_bbw_selection(&target,"Dose1",t-1)==ESP_ERR_INVALID_STATE);
    assert(hub_config_save_bbw_selection(&other,"Dose1",t+1)==ESP_ERR_INVALID_STATE);
    assert(hub_config_save_bbw_selection(&rebound,"Dose1",t+1)==ESP_ERR_INVALID_STATE);
    assert(hub_config_get_bbw_selection(&other,&read)!=ESP_OK&&!read.mode[0]);
    assert(hub_config_save_bbw_selection(&target,"unknown",t+1)==ESP_ERR_INVALID_ARG);
    assert(hub_config_save_bbw_selection(&target,"Dose1",NAN)==ESP_ERR_INVALID_ARG);
    assert(hub_config_save_bbw_selection(&target,"Dose1",t+200)==ESP_ERR_INVALID_ARG);
    read_error=ESP_FAIL;
    assert(hub_config_save_bbw_selection(&target,"Dose1",t+1)==ESP_FAIL&&writes==1);
    read_error=0;write_error=ESP_FAIL;
    assert(hub_config_save_bbw_selection(&target,"Dose1",t+1)==ESP_FAIL&&commits==1);
    write_error=0;
    assert(hub_config_save_bbw_selection(&target,"Dose1",t+1)==ESP_OK&&commits==2);
    assert(hub_config_save_bbw_selection(&target,"Continuous",t+2)==ESP_OK);
    assert(hub_config_get_bbw_selection(&target,&read)==ESP_OK&&!strcmp(read.mode,"Continuous"));
    assert(!locked);return 0;
}
'''

STATE = r'''
#include "hub_cloud.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* TYPES */
static bool confirmed,selection_matches=true;
static void copy_snapshot(cloud_snapshot_t *out,bool *cached) {
    memset(out,0,sizeof(*out));*cached=true;
    strcpy(out->serial_number,"SYNTHETIC");out->selection_generation=1;
    out->dose1_set=out->dose2_set=true;out->dose1=20;out->dose2=30;
    strcpy(out->bbw_mode,"Continuous");strcpy(out->last_updated,"2026-10-20T01:00:00Z");
    out->scale_observed_ms=1234;out->thing_scale_connected_set=true;out->thing_scale_connected=true;
    out->scale_battery_level_set=true;out->scale_battery_level=1;
}
static bool snapshot_matches_selection(const cloud_snapshot_t *s,bool cached) { (void)s;return cached&&selection_matches; }
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *out)
{ *out=(hub_machine_selection_identity_t){"SYNTHETIC",1};return ESP_OK; }
esp_err_t hub_config_get_bbw_selection(const hub_machine_selection_identity_t *target,hub_bbw_selection_t *out) {
    assert(!strcmp(target->serial_number,"SYNTHETIC"));
    if(!confirmed)return ESP_FAIL;
    *out=(hub_bbw_selection_t){"SYNTHETIC","Dose2",1792458000.123};return ESP_OK;
}
static void json_safe_copy(char *out,size_t size,const char *in) { snprintf(out,size,"%s",in); }
const char *hub_cloud_machine_name(void) { return "Synthetic"; }
static int64_t esp_timer_get_time(void) { return 100000000; }
/* FUNCTIONS */
int main(void) {
    for(int i=0;i<2;++i) {
        confirmed=i;
        cJSON *j=cJSON_Parse(hub_cloud_machine_state_json());assert(j);
        assert(!strcmp(cJSON_GetObjectItem(j,"brewByWeightMode")->valuestring,i?"Dose2":"Continuous"));
        assert(!strcmp(cJSON_GetObjectItem(j,"connectionState")->valuestring,"unknown"));
        assert(!strcmp(cJSON_GetObjectItem(j,"lastUpdated")->valuestring,"2026-10-20T01:00:00Z"));
        assert(cJSON_IsNull(cJSON_GetObjectItem(j,"coffeeBoilerCurrentTemperature")));
        assert(cJSON_GetObjectItem(j,"scaleObservedMs")->valuedouble==1234);
        assert(cJSON_IsNull(cJSON_GetObjectItem(j,"scaleBatteryLevel")));
        assert(cJSON_IsNull(cJSON_GetObjectItem(j,"thingScaleConnected")));
        cJSON *record=cJSON_GetObjectItem(j,"brewByWeightSelection");
        assert(!strcmp(cJSON_GetObjectItem(record,"serialNumber")->valuestring,"SYNTHETIC"));
        assert(cJSON_IsObject(cJSON_GetObjectItem(record,"confirmation"))==!!i);
        cJSON_Delete(j);
    }
    selection_matches=false;
    cJSON *j=cJSON_Parse(hub_cloud_machine_state_json());assert(j);
    assert(cJSON_GetObjectItem(j,"scaleObservedMs")->valuedouble==0);
    assert(cJSON_IsNull(cJSON_GetObjectItem(j,"thingScaleConnected")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(j,"scaleBatteryLevel")));
    cJSON_Delete(j);
    return 0;
}
'''


PUBLISH = r'''
#include "hub_cloud.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CUSTOMER_APP_URL "https://synthetic.example.test"
#define HTTP_METHOD_GET 0
#define READONLY_DIAG_DASHBOARD 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
/* TYPES */
typedef struct { int unused; } installation_key_t;
typedef struct { char access_token[16]; } cloud_tokens_t;
typedef struct { char *data; size_t len; } http_response_t;
static int snapshot_lock,source_status=200,selection_checks,rebind_at;
static bool cached_snapshot,machine_selected,ble_token_cached,source_scale_valid=true;
static cloud_snapshot_t snapshot;
static char machine_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN+1],machine_name[HUB_CONFIG_MAX_MACHINE_NAME_LEN+1];
static esp_err_t source_error,parse_error;
static int64_t clock_ms=2000;
static int64_t esp_timer_get_time(void) { return clock_ms*1000; }
static esp_err_t cloud_refresh_http_request(const char *url,int method,const installation_key_t *key,const char *token,const char *body,http_response_t *response,int *status) {
    assert(strstr(url,"/things/SYNTHETIC/dashboard")&&method==HTTP_METHOD_GET&&key&&token&&!body);
    clock_ms+=100;*status=source_status;response->data="synthetic";response->len=9;return source_error;
}
static uint32_t http_response_first_bytes(const http_response_t *r) { (void)r;return 0; }
static void copy_snapshot(cloud_snapshot_t *out,bool *cached) { *out=snapshot;if(cached)*cached=cached_snapshot; }
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *target) {
    assert(target->generation==1);++selection_checks;return selection_checks!=rebind_at;
}
static esp_err_t parse_dashboard_snapshot(const char *data,const char *serial,uint32_t generation,const cloud_snapshot_t *old,cloud_snapshot_t *out,char *name,size_t size) {
    assert(data&&old&&size>0);name[0]=0;memset(out,0,sizeof(*out));
    strcpy(out->serial_number,serial);out->selection_generation=generation;
    out->thing_scale_connected_set=source_scale_valid;out->thing_scale_connected=source_scale_valid;
    out->scale_observed_ms=source_scale_valid?clock_ms:0;
    return parse_error;
}
static void record_readonly_http_diag(int kind,esp_err_t error,int status,int duration,size_t length,uint32_t first) {
    (void)kind;(void)error;(void)status;(void)duration;(void)length;(void)first;
}
static void hub_http_response_reset(http_response_t *r) { memset(r,0,sizeof(*r)); }
/* FUNCTION */
int main(void) {
    installation_key_t key={0};cloud_tokens_t tokens={0};
    hub_machine_selection_t selection={0};strcpy(selection.serial_number,"SYNTHETIC");
    hub_machine_selection_identity_t target={"SYNTHETIC",1};
    assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)==ESP_OK);
    assert(cached_snapshot&&snapshot.scale_observed_ms==2100&&snapshot.thing_scale_connected);
    for(int failure=0;failure<4;++failure) {
        selection_checks=0;source_error=failure==0?ESP_FAIL:ESP_OK;
        source_status=failure==1?500:200;parse_error=failure==2?ESP_ERR_INVALID_RESPONSE:ESP_OK;
        rebind_at=failure==3?2:0;
        assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)!=ESP_OK);
        assert(snapshot.scale_observed_ms==2100&&snapshot.thing_scale_connected);
    }
    selection_checks=0;source_error=parse_error=ESP_OK;source_status=200;rebind_at=0;
    assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)==ESP_OK);
    assert(snapshot.scale_observed_ms==clock_ms&&snapshot.scale_observed_ms>2100);
    source_scale_valid=false;
    assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)==ESP_OK);
    assert(snapshot.scale_observed_ms==0&&!snapshot.thing_scale_connected_set);
    return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="hub-state-selection-") as temp:
        directory = Path(temp)
        (directory / "esp_err.h").write_text(ERRORS)
        keeper = (ROOT / "main/hub_state_keeper.c").read_text()
        constants = "\n".join(re.findall(r"static const uint32_t [^;]+;", keeper))
        code = constants + "\n" + functions(keeper, ["cold_interval_ms", "ready_interval_ms", "hub_state_keeper_task"])
        compile_run(directory, "keeper", KEEPER.replace("/* FUNCTIONS */", code))
        scale_type = re.search(r"typedef struct\s*\{[^}]+\}\s*scale_battery_reads_t;", keeper).group()
        amoled_code = constants + "\n" + scale_type + "\n" + functions(keeper,
            ["cold_interval_ms", "ready_interval_ms", "scale_battery_tick", "hub_state_keeper_task"])
        compile_run(directory, "keeper_amoled", KEEPER.replace("/* FUNCTIONS */", amoled_code), ["-DHUB_AMOLED_216"])
        config = (ROOT / "main/hub_config.c").read_text()
        compile_run(directory, "selection", SELECTION.replace("/* FUNCTIONS */",
            functions(config, ["bbw_selection_valid", "hub_config_get_bbw_selection", "hub_config_save_bbw_selection"])))
        cloud = (ROOT / "main/hub_cloud.c").read_text()
        types = re.search(r"typedef struct\s*\{[^}]+\}\s*cloud_snapshot_t;", cloud).group()
        body = functions(cloud, ["scale_snapshot_fresh"]) + "\nconst char *hub_cloud_machine_state_json(void) {" + function_body(cloud, "hub_cloud_machine_state_json") + "}"
        json_dir = Path(os.environ["IDF_PATH"]) / "components/json/cJSON"
        compile_run(directory, "state", STATE.replace("/* TYPES */", types).replace("/* FUNCTIONS */", body),
            ["-I", json_dir, json_dir / "cJSON.c", "-Wno-deprecated-declarations"])
        signature=re.search(r"static esp_err_t cloud_fetch_dashboard\([^;{]*\)\s*\{",cloud).group()
        publish=signature+function_body(cloud,"cloud_fetch_dashboard")+"}"
        compile_run(directory,"publish",PUBLISH.replace("/* TYPES */",types).replace("/* FUNCTION */",publish))
    print("State selection PASS: legacy zero accessory reads, AMOLED bounded battery acquisition/priority/binding, original token/reconnection cadence, durable scoped confirmations, stale/replay/failure guards")


if __name__ == "__main__":
    main()
