#!/usr/bin/env python3
"""Execute the real keeper loop and durable display-selection code with host fakes."""
import re
import tempfile
from pathlib import Path
from test_integrated_support import ROOT, ERRORS, compile_run
from validate_stability_foundation import function_body
from host_cjson import cjson_source_dir


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
static int refresh_calls;
static int64_t refresh_call_times[32],cloud_refresh_started_ms;
static bool cloud_online, recover_cloud, complete_snapshot, full_observes_scale;
static bool wifi_available, refresh_pending;
static bool recovery_requested;
static uint32_t refresh_generation;
static int completed_requests;
static int64_t manual_request_ms;
#ifdef HUB_AMOLED_216
static bool interactive_deferred, grinder_busy, scale_attempted, scale_observes;
static bool warming;
static int64_t machine_on_ms, warmup_end_ms, defer_until_ms;
static size_t largest_block;
static int busy_calls;
static bool invalid_after_start, busy_refresh_running, other_refresh_running, request_during_refresh;
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static size_t heap_caps_get_largest_free_block(int caps) { (void)caps;return largest_block; }
static esp_err_t scale_error, identity_error;
static int64_t scale_times[32], scale_http_ms;
static hub_machine_selection_identity_t binding;
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *out)
{ *out=binding;return identity_error; }
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *target)
{ return target->generation==binding.generation&&!strcmp(target->serial_number,binding.serial_number); }
bool hub_command_idle_background_deferred(void) { return interactive_deferred||clock_ms<defer_until_ms; }
bool hub_grinder_busy(void) { return grinder_busy; }
bool hub_cloud_has_fresh_scale_battery(void)
{ return cloud_online&&observed_ms>0&&clock_ms>=observed_ms&&clock_ms-observed_ms<60000; }
#endif
static int64_t now_ms(void) { return clock_ms; }
static bool hub_wifi_station_has_ip(void) { return wifi_available; }
static bool hub_ble_has_cached_state(void) { return clock_ms >= ble_after; }
static void read_keeper_state(int64_t *activity,int64_t *active,bool *recovery)
{ *activity=1; *active=300000; *recovery=recovery_requested; }
static bool hub_state_process_pending_slow_cloud_read(void) { return false; }
static bool consume_recovery_request(void)
{ bool requested=recovery_requested;recovery_requested=false;return requested; }
static void extend_active_window(void) {}
static void hub_state_keeper_request_recovery(void) { recovery_requested=true; }
static void vTaskDelay(uint32_t ms) { clock_ms+=ms; }
static int xTaskNotifyWait(uint32_t a,uint32_t b,uint32_t *bits,uint32_t ms) {
    (void)a;(void)b;(void)bits;assert(ms>0&&ms<=30000);
    if(manual_request_ms>clock_ms&&manual_request_ms<=clock_ms+ms) {
        clock_ms=manual_request_ms;manual_request_ms=0;
        refresh_pending=true;++refresh_generation;hub_state_keeper_request_recovery();
    } else if(control_observation_ms>clock_ms&&control_observation_ms<=clock_ms+ms) {
        clock_ms=control_observation_ms;observed_ms=clock_ms;control_observation_ms=0;
    } else clock_ms+=ms;
    if(clock_ms>stop_ms)longjmp(stop,1);
    return 0;
}
void hub_cloud_get_snapshot_status(hub_cloud_snapshot_status_t *s)
{ *s=(hub_cloud_snapshot_status_t){.has_cached_snapshot=true,.has_complete_bbw=complete_snapshot,
    .is_connected=cloud_online,.scale_observed_ms=observed_ms,
    .is_ready=cloud_online&&complete_snapshot,.refresh_pending=refresh_pending};
#ifdef HUB_AMOLED_216
    s->boiler_warmup_pending=warming;
#endif
}
int64_t hub_cloud_token_renewal_due_ms(void) { return token_due_ms; }
int64_t hub_cloud_last_refresh_ms(void) { return cloud_refresh_started_ms; }
uint32_t hub_cloud_refresh_request_generation(void) { return refresh_generation; }
void hub_cloud_complete_refresh_request(uint32_t ticket) {
    if(ticket&&refresh_pending&&ticket==refresh_generation) { refresh_pending=false;++completed_requests; }
}
bool hub_cloud_refresh_is_running(void) {
#ifdef HUB_AMOLED_216
    return other_refresh_running;
#else
    return false;
#endif
}
esp_err_t hub_cloud_refresh(void) {
    assert(refresh_calls<32);refresh_call_times[refresh_calls++]=clock_ms;
#ifdef HUB_AMOLED_216
    if(refresh_calls>1&&busy_calls) {
        --busy_calls;other_refresh_running=busy_refresh_running;return ESP_ERR_INVALID_STATE;
    }
    other_refresh_running=false;
#endif
    cloud_refresh_started_ms=clock_ms;
    assert(refreshes<32); refresh_times[refreshes++]=clock_ms;
    if(recover_cloud&&hub_ble_has_cached_state()) cloud_online=true;
#ifdef HUB_AMOLED_216
    if(machine_on_ms&&clock_ms>=machine_on_ms)cloud_online=true;
    if(warmup_end_ms)warming=cloud_online&&clock_ms<warmup_end_ms;
    if(refresh_calls>1&&request_during_refresh) {
        request_during_refresh=false;++refresh_generation;refresh_pending=true;hub_state_keeper_request_recovery();
    }
    if(refresh_calls>1&&invalid_after_start)return ESP_ERR_INVALID_STATE;
#endif
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
    refresh_calls=0;cloud_refresh_started_ms=0;
    observed_ms=control_observation_ms=token_due_ms=0;
    complete_snapshot=full_observes_scale=true;
    cloud_online=online;recover_cloud=recover;ble_after=local_after;
    wifi_available=true;refresh_pending=false;
    recovery_requested=false;refresh_generation=0;completed_requests=0;manual_request_ms=0;
#ifdef HUB_AMOLED_216
    interactive_deferred=grinder_busy=scale_observes=false;scale_attempted=true;
    scale_error=identity_error=ESP_OK;
    scale_http_ms=0;
    machine_on_ms=warmup_end_ms=defer_until_ms=0;warming=false;largest_block=8192;
    busy_calls=0;invalid_after_start=busy_refresh_running=other_refresh_running=request_during_refresh=false;
    binding=(hub_machine_selection_identity_t){"SYNTHETIC",1};
#endif
}
static void execute(void) { if(!setjmp(stop))hub_state_keeper_task(NULL); }
static void run(bool online,bool recover,int64_t local_after) {
    setup(online,recover,local_after);execute();
}
#ifdef HUB_AMOLED_216
static void amoled_boiler_cases(void) {
    // An offline snapshot discovers power-on without nearby BLE, then follows both boilers past five minutes.
    setup(false,false,INT64_MAX);machine_on_ms=160000;warmup_end_ms=700000;stop_ms=1000000;execute();
    assert(refreshes==7&&cloud_online&&!warming&&scale_reads==1);
    for(int i=1;i<refreshes;++i)assert(refresh_times[i]-refresh_times[i-1]==120000);
    assert(refresh_times[6]==722000); // One successful snapshot after the estimated finish, then normal cadence.

    // A boot snapshot with missing BBW fields keeps its initial backoff but cannot retreat to 5+ minutes.
    setup(false,false,INT64_MAX);complete_snapshot=false;stop_ms=700000;execute();
    assert(refresh_times[1]-refresh_times[0]==5000);
    assert(refresh_times[2]-refresh_times[1]==15000);
    assert(refresh_times[3]-refresh_times[2]==30000);
    assert(refresh_times[4]-refresh_times[3]==60000);
    for(int i=5;i<refreshes;++i)assert(refresh_times[i]-refresh_times[i-1]==120000);
    assert(refreshes==9);

    // Optional observations yield without consuming the due attempt; token renewal keeps its own deadline.
    for(int reason=0;reason<5;++reason) {
        setup(true,false,0);warming=true;stop_ms=350000;
        if(reason==0)interactive_deferred=true;
        if(reason==1)grinder_busy=true;
        if(reason==2)largest_block=4095;
        if(reason==3)wifi_available=false;
        if(reason==4)refresh_pending=true;
        execute();assert(refreshes==1);
    }
    setup(true,false,0);warming=true;defer_until_ms=155000;stop_ms=200000;execute();
    assert(refreshes==2&&refresh_times[1]==182000);
    setup(true,false,0);warming=true;interactive_deferred=true;token_due_ms=137000;stop_ms=160000;execute();
    assert(refreshes==2&&refresh_times[1]==137000); // Earlier than the 30-second deferred wake.
    setup(true,false,0);warming=true;largest_block=4096;stop_ms=130000;execute();
    assert(refreshes==2&&refresh_times[1]==122000);

    // A busy shared TLS lock never started a refresh: retry locally after 30 seconds, not 120.
    setup(true,false,0);warming=true;busy_calls=2;stop_ms=320000;execute();
    assert(refresh_calls==5&&refreshes==3);
    assert(refresh_call_times[1]==122000&&refresh_call_times[2]==152000&&refresh_times[1]==182000);
    assert(refresh_times[2]==302000);
    setup(true,false,0);warming=true;busy_calls=1;token_due_ms=137000;stop_ms=160000;execute();
    assert(refresh_calls==3&&refreshes==2&&refresh_times[1]==137000); // Auth does not wait for the retry.
    setup(false,false,INT64_MAX);complete_snapshot=false;busy_calls=1;stop_ms=50000;execute();
    assert(refresh_call_times[1]==7000&&refresh_call_times[2]==37000&&refreshes==2);
    setup(true,false,0);warming=true;invalid_after_start=true;stop_ms=260000;execute();
    assert(refresh_calls==3&&refresh_times[1]==122000&&refresh_times[2]==242000); // A started attempt keeps normal cadence.
}

static void amoled_refresh_request_cases(void) {
    // An ordinary grinder TLS owner must not consume a manual refresh that never started.
    setup(true,false,0);manual_request_ms=10000;busy_calls=1;stop_ms=20000;execute();
    assert(refresh_calls==2&&refreshes==1&&cloud_refresh_started_ms==2000);
    assert(refresh_pending&&recovery_requested&&completed_requests==0);

    // Retain the same ticket through repeated lock contention, then complete it exactly once.
    for(int running=0;running<2;++running) {
        setup(true,false,0);manual_request_ms=10000;busy_calls=2;busy_refresh_running=running;stop_ms=90000;execute();
        assert(refresh_calls==4&&refreshes==2&&refresh_times[1]==70000);
        assert(refresh_call_times[1]==10000&&refresh_call_times[2]==40000);
        assert(!refresh_pending&&!recovery_requested&&refresh_generation==1&&completed_requests==1);
    }

    // Retried manual work yields to foreground activity and active grinder work, not to its own pending ticket.
    setup(true,false,0);manual_request_ms=10000;busy_calls=1;defer_until_ms=65000;stop_ms=90000;execute();
    assert(refresh_calls==3&&refresh_times[1]==70000&&completed_requests==1);
    setup(true,false,0);manual_request_ms=10000;busy_calls=1;grinder_busy=true;stop_ms=90000;execute();
    assert(refresh_calls==2&&refresh_pending&&recovery_requested&&completed_requests==0);

    // Future token expiry interrupts retry backoff. An already-due blocked renewal is rate-limited locally.
    setup(true,false,0);manual_request_ms=10000;busy_calls=1;token_due_ms=25000;stop_ms=60000;execute();
    assert(refresh_calls==3&&refresh_times[1]==25000&&completed_requests==1);
    setup(true,false,0);manual_request_ms=10000;busy_calls=2;token_due_ms=10000;stop_ms=20000;execute();
    assert(refresh_calls==2&&refresh_pending&&token_due_ms==10000&&completed_requests==0);
    setup(true,false,0);manual_request_ms=10000;busy_calls=2;token_due_ms=10000;stop_ms=90000;execute();
    assert(refresh_calls==4&&refresh_times[1]==70000&&completed_requests==1&&token_due_ms==0);
    setup(true,false,0);manual_request_ms=10000;busy_calls=10;token_due_ms=10000;
    interactive_deferred=true;stop_ms=60000;execute();
    assert(refresh_calls==3&&refreshes==1&&refresh_call_times[2]==40000);
    assert(refresh_pending&&recovery_requested&&completed_requests==0&&token_due_ms==10000);

    // A genuine started refresh failure consumes its ticket; it must not become an endless busy retry.
    setup(true,false,0);manual_request_ms=10000;invalid_after_start=true;stop_ms=90000;execute();
    assert(refresh_calls==2&&refreshes==2&&cloud_refresh_started_ms==10000);
    assert(!refresh_pending&&!recovery_requested&&completed_requests==1);

    // A new request arriving during the refresh survives completion of the old generation.
    setup(true,false,0);manual_request_ms=10000;request_during_refresh=true;stop_ms=90000;execute();
    assert(refresh_calls==3&&refreshes==3&&refresh_generation==2&&completed_requests==1&&!refresh_pending);
}

static void amoled_scale_battery_cases(void) {
    setup(true,false,0);scale_observes=true;stop_ms=750000;execute();
    assert(refreshes==1&&scale_reads>=3);
    for(int i=1;i<scale_reads;++i)assert(scale_times[i]-scale_times[i-1]>=180000);
    assert(clock_ms-observed_ms<300000);
    setup(true,false,0);full_observes_scale=false;execute();
    assert(refreshes==1&&scale_reads==2&&scale_times[0]==92000&&scale_times[1]==272000);
    const esp_err_t errors[]={ESP_ERR_TIMEOUT,ESP_ERR_NO_MEM,ESP_ERR_INVALID_STATE,ESP_ERR_INVALID_RESPONSE};
    for(unsigned i=0;i<4;++i) {
        setup(true,false,0);full_observes_scale=false;scale_error=errors[i];execute();
        assert(scale_reads==2&&refreshes==1);
    }
    setup(true,false,0);control_observation_ms=170000;stop_ms=340000;execute();
    assert(scale_reads==0&&observed_ms==170000); // Ordinary control postpones the optional read.
    for(unsigned i=0;i<2;++i) {
        setup(true,false,0);interactive_deferred=i==0;grinder_busy=i==1;token_due_ms=220000;execute();
        assert(scale_reads==0&&refreshes==2&&refresh_times[1]==220000);
    }
    setup(true,false,0);full_observes_scale=false;token_due_ms=100000;scale_http_ms=10000;stop_ms=130000;execute();
    assert(scale_reads==1&&scale_times[0]==92000&&refresh_times[1]==102000);
    setup(true,false,0);full_observes_scale=false;
    scale_battery_reads_t reads={0};hub_cloud_snapshot_status_t status={.is_ready=true};
    scale_battery_tick(&reads,&status,false);assert(reads.next_ms==91000);
    clock_ms=91000;
    scale_battery_tick(&reads,&status,true);assert(scale_reads==0);
    status.is_ready=false;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);status.is_ready=true;
    status.refresh_pending=true;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);status.refresh_pending=false;
    wifi_available=false;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);wifi_available=true;
    identity_error=ESP_FAIL;scale_battery_tick(&reads,&status,false);assert(scale_reads==0);identity_error=ESP_OK;
    scale_attempted=false;scale_error=ESP_ERR_NO_MEM;scale_battery_tick(&reads,&status,false);
    assert(scale_reads==1&&reads.next_ms==121000);
    clock_ms=120999;scale_battery_tick(&reads,&status,false);assert(scale_reads==1);
    clock_ms=121000;scale_attempted=true;scale_error=ESP_ERR_TIMEOUT;scale_battery_tick(&reads,&status,false);
    assert(scale_reads==2&&reads.next_ms==301000);
    ++binding.generation;scale_battery_tick(&reads,&status,false);assert(reads.next_ms==211000);
    strcpy(binding.serial_number,"OTHER");clock_ms=130000;scale_battery_tick(&reads,&status,false);assert(reads.next_ms==220000);
}

#endif
int main(void) {
    assert(ready_interval_ms(100,1,300000,false)==600000);
    assert(ready_interval_ms(400000,1,300000,false)==1800000);
    assert(ready_interval_ms(2000000,1,300000,false)==3600000);
    run(false,true,150000);
#ifdef HUB_AMOLED_216
    assert(refreshes==3&&cloud_online&&refresh_times[2]==182000&&scale_reads==0);
#else
    assert(refreshes==2&&cloud_online&&refresh_times[1]>=150000&&refresh_times[1]<=180000&&scale_reads==0);
#endif
    run(false,false,0);
    assert(refreshes==6&&scale_reads==0); // Local evidence retains only the existing connection-recovery cadence.
    for(int i=1;i<refreshes;++i)assert(refresh_times[i]-refresh_times[i-1]==60000);
    run(false,false,INT64_MAX);
#ifdef HUB_AMOLED_216
    assert(refreshes==3&&scale_reads==0);
#else
    assert(refreshes==1&&scale_reads==0); // Offline without local evidence stays quiet.
#endif
    setup(true,false,0);complete_snapshot=false;execute();
    assert(scale_reads==0); // Incomplete setup does not start accessory polling.

    setup(true,false,0);stop_ms=125000;execute();
    assert(refreshes==1&&scale_reads==0);
    assert(clock_ms-observed_ms>60000); // Expiry does not initiate another network request.

    setup(true,false,0);stop_ms=125000;control_observation_ms=27000;execute();
    assert(refreshes==1&&scale_reads==0&&observed_ms==27000);

    setup(true,false,0);stop_ms=1360000;execute();
    assert(refreshes==1);
#ifdef HUB_AMOLED_216
    assert(scale_reads>0); // Optional scale reads do not trigger full Cloud refreshes.
#else
    assert(scale_reads==0);
#endif

    setup(true,false,0);stop_ms=65000;token_due_ms=32000;execute();
    assert(refreshes==2&&refresh_times[1]==32000);
    assert(scale_reads==0);

    setup(true,false,0);stop_ms=65000;full_observes_scale=false;execute();
    assert(refreshes==1&&scale_reads==0&&observed_ms==0); // Missing scale data stays unknown without polling.
#ifdef HUB_AMOLED_216
    amoled_boiler_cases();
    amoled_refresh_request_cases();
    amoled_scale_battery_cases();
#endif
    return 0;
}
'''

WARMUP_STATUS = r'''
#define _POSIX_C_SOURCE 200809L
#include "hub_cloud.h"
#include <assert.h>
#include <string.h>
#include <time.h>
/* TYPES */
static cloud_snapshot_t snapshot;
static bool cached=true,selection_matches=true;
static int64_t last_refresh_ms=123;
static time_t clock_unix=1792716000;
#define time(out) (clock_unix)
static void copy_snapshot(cloud_snapshot_t *out,bool *present) { *out=snapshot;*present=cached; }
static bool snapshot_matches_selection(const cloud_snapshot_t *s,bool present) { (void)s;return present&&selection_matches; }
static bool cloud_refresh_pending_snapshot(void) { return false; }
/* FUNCTIONS */
int main(void) {
    hub_cloud_snapshot_status_t status;
    snapshot.connected=true;snapshot.dose1_set=snapshot.dose2_set=true;strcpy(snapshot.bbw_mode,"Dose2");
    strcpy(snapshot.last_updated,"2026-10-23T00:40:00Z");
    strcpy(snapshot.coffee_boiler_ready_time,"2026-10-23T00:42:00Z");
    strcpy(snapshot.steam_boiler_ready_time,"2026-10-23T00:48:00Z");
    hub_cloud_get_snapshot_status(&status);assert(status.is_ready&&status.last_refresh_ms==123);
#ifdef HUB_AMOLED_216
    assert(status.boiler_warmup_pending);
    clock_unix+=600; // Passing the estimate alone must not cancel the final confirming read.
    hub_cloud_get_snapshot_status(&status);assert(status.boiler_warmup_pending);
    strcpy(snapshot.last_updated,"2026-10-23T00:43:00Z");
    hub_cloud_get_snapshot_status(&status);assert(status.boiler_warmup_pending); // Steam is still pending.
    strcpy(snapshot.last_updated,"2026-10-23T00:50:00Z");
    hub_cloud_get_snapshot_status(&status);assert(!status.boiler_warmup_pending);
    strcpy(snapshot.last_updated,"2026-10-23T00:40:00Z");
    const char *invalid[]={"","2026-10-25T00:40:00Z","bad","2026-10-23T00:39:59Z"};
    snapshot.steam_boiler_ready_time[0]=0;
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        strcpy(snapshot.coffee_boiler_ready_time,invalid[i]);
        hub_cloud_get_snapshot_status(&status);assert(!status.boiler_warmup_pending);
    }
    strcpy(snapshot.coffee_boiler_ready_time,"2026-10-23T00:42:00Z");
    for(int invalid=0;invalid<5;++invalid) {
        cached=invalid!=0;selection_matches=invalid!=1;snapshot.connected=invalid!=2;
        clock_unix=invalid==3?0:1792716000;
        strcpy(snapshot.last_updated,invalid==4?"2026-10-23T01:00:00Z":"2026-10-23T00:40:00Z");
        hub_cloud_get_snapshot_status(&status);assert(!status.boiler_warmup_pending);
    }
#else
    assert(!status.boiler_warmup_pending); // No new scheduling signal on LCD/headless builds.
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
static esp_err_t read_error, write_error, commit_error;
static hub_machine_selection_identity_t machine_selection_identity={"SYNTHETIC",1};
static hub_bbw_selection_t stored, staged;
static size_t stored_size=sizeof(stored);
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
    assert(*size>=sizeof(stored));memcpy(out,&stored,sizeof(stored));*size=stored_size;return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size) {
    assert(h==1&&!strcmp(key,"bbw_select_v1")&&size==sizeof(stored));++writes;
    if(write_error)return write_error;
    memcpy(&staged,in,size);return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h==1);++commits;if(commit_error)return commit_error;
    stored=staged;have_record=true;stored_size=sizeof(stored);return ESP_OK;
}
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

DOSES = SELECTION[:SELECTION.index("/* FUNCTIONS */")].replace(
    "hub_bbw_selection_t", "hub_bbw_doses_t").replace("bbw_select_v1", "bbw_doses_v1") + r'''
/* FUNCTIONS */
int main(void) {
    const hub_machine_selection_identity_t target={"SYNTHETIC",1},other={"OTHER",1},rebound={"SYNTHETIC",2};
    hub_bbw_doses_t read;
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_ERR_NVS_NOT_FOUND);
    assert(hub_config_save_bbw_doses(&target,"Dose2",20,30)==ESP_OK&&writes==1&&commits==1);
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_OK&&read.dose1==20&&read.dose2==30&&!strcmp(read.mode,"Dose2"));
    for(int i=0;i<20;++i) assert(hub_config_save_bbw_doses(&target,"Dose2",20,30)==ESP_OK);
    assert(writes==1&&commits==1); // Repeated polling never rewrites Flash.
    assert(hub_config_save_bbw_doses(&other,"Dose1",42,36)==ESP_ERR_INVALID_STATE);
    assert(hub_config_save_bbw_doses(&rebound,"Dose1",42,36)==ESP_ERR_INVALID_STATE);
    assert(hub_config_get_bbw_doses(&other,&read)==ESP_ERR_INVALID_STATE&&!read.serial_number[0]);
    const double invalid[]={NAN,INFINITY,-1,0,201};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        assert(hub_config_save_bbw_doses(&target,"Dose2",invalid[i],30)==ESP_ERR_INVALID_ARG);
        assert(hub_config_save_bbw_doses(&target,"Dose2",20,invalid[i])==ESP_ERR_INVALID_ARG);
    }
    assert(hub_config_save_bbw_doses(&target,"unknown",20,30)==ESP_ERR_INVALID_ARG);
    read_error=ESP_FAIL;
    assert(hub_config_save_bbw_doses(&target,"Dose1",42,36)==ESP_FAIL&&writes==1);
    read_error=0;write_error=ESP_FAIL;
    assert(hub_config_save_bbw_doses(&target,"Dose1",42,36)==ESP_FAIL&&commits==1);
    write_error=0;
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_OK&&read.dose1==20&&read.dose2==30);
    assert(hub_config_save_bbw_doses(&target,"Dose1",42,36)==ESP_OK&&commits==2);
    memset(&read,0,sizeof(read)); // Fresh output after restart loads the stored pair and mode.
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_OK&&read.dose1==42&&read.dose2==36&&!strcmp(read.mode,"Dose1"));
    assert(hub_config_save_bbw_doses(&target,"Continuous",42,36)==ESP_OK&&commits==3);
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_OK&&!strcmp(read.mode,"Continuous"));
    commit_error=ESP_FAIL;
    assert(hub_config_save_bbw_doses(&target,"Dose2",20,30)==ESP_FAIL);
    commit_error=0;
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_OK&&read.dose1==42&&read.dose2==36&&!strcmp(read.mode,"Continuous"));
    stored_size=sizeof(stored)-1;
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_ERR_INVALID_STATE&&!read.serial_number[0]);
    stored_size=sizeof(stored);
    stored.dose1=NAN;
    assert(hub_config_get_bbw_doses(&target,&read)==ESP_ERR_INVALID_STATE&&!read.serial_number[0]);
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
static bool confirmed,selection_matches=true,saved_doses,source_doses=true,source_mode=true;
static void copy_snapshot(cloud_snapshot_t *out,bool *cached) {
    memset(out,0,sizeof(*out));*cached=true;
    strcpy(out->serial_number,"SYNTHETIC");out->selection_generation=1;
    out->dose1_set=out->dose2_set=source_doses;out->dose1=20;out->dose2=30;
    if(source_mode)strcpy(out->bbw_mode,"Continuous");
    strcpy(out->last_updated,"2026-10-20T01:00:00Z");
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
esp_err_t hub_config_get_bbw_doses(const hub_machine_selection_identity_t *target,hub_bbw_doses_t *out) {
    assert(!strcmp(target->serial_number,"SYNTHETIC"));
    if(!saved_doses)return ESP_ERR_NOT_FOUND;
    *out=(hub_bbw_doses_t){"SYNTHETIC","Dose1",42,36};return ESP_OK;
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
    confirmed=false;saved_doses=true;source_doses=source_mode=false;
    cJSON *restored=cJSON_Parse(hub_cloud_machine_state_json());assert(restored);
    assert(cJSON_GetObjectItem(restored,"brewByWeightDose1")->valuedouble==42);
    assert(cJSON_GetObjectItem(restored,"brewByWeightDose2")->valuedouble==36);
    assert(!strcmp(cJSON_GetObjectItem(restored,"brewByWeightMode")->valuestring,"Dose1"));
    assert(!strcmp(cJSON_GetObjectItem(restored,"connectionState")->valuestring,"unknown"));
    cJSON_Delete(restored);
    cloud_snapshot_t raw;bool cached;copy_snapshot(&raw,&cached);
    assert(!raw.dose1_set&&!raw.dose2_set&&!raw.bbw_mode[0]); // Display fallback never feeds the control snapshot.
    source_mode=true;
    restored=cJSON_Parse(hub_cloud_machine_state_json());
    assert(!strcmp(cJSON_GetObjectItem(restored,"brewByWeightMode")->valuestring,"Continuous"));
    cJSON_Delete(restored);
    source_doses=true;
    restored=cJSON_Parse(hub_cloud_machine_state_json());
    assert(cJSON_GetObjectItem(restored,"brewByWeightDose1")->valuedouble==20);
    assert(cJSON_GetObjectItem(restored,"brewByWeightDose2")->valuedouble==30);
    cJSON_Delete(restored);
    selection_matches=false;saved_doses=false;
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
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
/* TYPES */
typedef struct { int unused; } installation_key_t;
typedef struct { char access_token[16]; } cloud_tokens_t;
typedef struct { char *data; size_t len; } http_response_t;
static int snapshot_lock,source_status=200,selection_checks,rebind_at,dose_saves;
static bool cached_snapshot,machine_selected,ble_token_cached,source_scale_valid=true;
static bool source_doses_valid=true,command_target_valid=true;
static cloud_snapshot_t snapshot;
static char machine_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN+1],machine_name[HUB_CONFIG_MAX_MACHINE_NAME_LEN+1];
static esp_err_t source_error,parse_error;
static esp_err_t storage_error;
esp_err_t hub_config_save_bbw_doses(const hub_machine_selection_identity_t *target,const char *mode,double first,double second) {
    assert(target->generation==1&&!strcmp(target->serial_number,"SYNTHETIC"));
    assert(!strcmp(mode,"Dose2")&&first>0&&second==30);++dose_saves;return storage_error;
}
static bool active_command_target(hub_machine_selection_identity_t *out) {
    *out=(hub_machine_selection_identity_t){"SYNTHETIC",1};return command_target_valid;
}
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
    out->dose1_set=out->dose2_set=source_doses_valid;out->dose1=20;out->dose2=30;strcpy(out->bbw_mode,"Dose2");
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
    assert(dose_saves==1);
    for(int failure=0;failure<4;++failure) {
        selection_checks=0;source_error=failure==0?ESP_FAIL:ESP_OK;
        source_status=failure==1?500:200;parse_error=failure==2?ESP_ERR_INVALID_RESPONSE:ESP_OK;
        rebind_at=failure==3?2:0;
        assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)!=ESP_OK);
        assert(snapshot.scale_observed_ms==2100&&snapshot.thing_scale_connected);
        assert(dose_saves==1);
    }
    selection_checks=0;source_error=parse_error=ESP_OK;source_status=200;rebind_at=0;
    assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)==ESP_OK);
    assert(snapshot.scale_observed_ms==clock_ms&&snapshot.scale_observed_ms>2100);
    source_scale_valid=false;
    assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)==ESP_OK);
    assert(snapshot.scale_observed_ms==0&&!snapshot.thing_scale_connected_set);
    assert(dose_saves==3);
    source_doses_valid=false;
    assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)==ESP_OK&&dose_saves==3);
    source_doses_valid=true;storage_error=ESP_FAIL;
    assert(cloud_fetch_dashboard(&key,&tokens,&selection,&target)==ESP_OK&&dose_saves==4);
    command_target_valid=false;update_snapshot_bbw_dose(42,30);assert(dose_saves==4&&snapshot.dose1==20);
    command_target_valid=true;snapshot.selection_generation=2;
    update_snapshot_bbw_dose(42,30);assert(dose_saves==4&&snapshot.dose1==20);
    snapshot.selection_generation=1;
    update_snapshot_bbw_dose(42,30);assert(dose_saves==5&&snapshot.dose1==42&&snapshot.dose2==30);
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
        compile_run(directory, "doses", DOSES.replace("/* FUNCTIONS */",
            functions(config, ["bbw_doses_valid", "hub_config_get_bbw_doses", "hub_config_save_bbw_doses"])))
        cloud = (ROOT / "main/hub_cloud.c").read_text()
        types = re.search(r"typedef struct\s*\{[^}]+\}\s*cloud_snapshot_t;", cloud).group()
        for variant in [False, True]:
            names = (["snapshot_boiler_warmup_pending"] if variant else []) + ["hub_cloud_get_snapshot_status"]
            source = WARMUP_STATUS.replace("/* TYPES */", types).replace("/* FUNCTIONS */", functions(cloud, names))
            compile_run(directory, "warmup_status_amoled" if variant else "warmup_status", source,
                        ["-DHUB_AMOLED_216"] if variant else [])
        body = functions(cloud, ["scale_snapshot_fresh"]) + "\nconst char *hub_cloud_machine_state_json(void) {" + function_body(cloud, "hub_cloud_machine_state_json") + "}"
        json_dir = cjson_source_dir()
        compile_run(directory, "state", STATE.replace("/* TYPES */", types).replace("/* FUNCTIONS */", body),
            ["-I", json_dir, json_dir / "cJSON.c", "-Wno-deprecated-declarations"])
        signature=re.search(r"static esp_err_t cloud_fetch_dashboard\([^;{]*\)\s*\{",cloud).group()
        publish=functions(cloud,["update_snapshot_bbw_dose"])+signature+function_body(cloud,"cloud_fetch_dashboard")+"}"
        compile_run(directory,"publish",PUBLISH.replace("/* TYPES */",types).replace("/* FUNCTION */",publish))
    print("State selection PASS: legacy cadence, AMOLED warmup observation and retained refresh tickets with bounded busy retry and foreground/token priority, bounded battery acquisition, durable scoped confirmations and dose display history with live priority/control isolation")


if __name__ == "__main__":
    main()
