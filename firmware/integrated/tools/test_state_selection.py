#!/usr/bin/env python3
"""Execute the real keeper loop and durable display-selection code with host fakes."""
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
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
static jmp_buf stop;
static int64_t clock_ms, stop_ms, ble_after, refresh_times[32];
static int refreshes;
static bool cloud_online, recover_cloud;
static int64_t now_ms(void) { return clock_ms; }
static bool hub_wifi_station_has_ip(void) { return true; }
static bool hub_ble_has_cached_state(void) { return clock_ms >= ble_after; }
static void read_keeper_state(int64_t *activity,int64_t *active,bool *recovery)
{ *activity=1; *active=300000; *recovery=false; }
static bool hub_state_process_pending_slow_cloud_read(void) { return false; }
static bool consume_recovery_request(void) { return false; }
static void extend_active_window(void) {}
static void hub_state_keeper_request_recovery(void) { assert(false); }
static void vTaskDelay(uint32_t ms) { clock_ms+=ms; }
static int xTaskNotifyWait(uint32_t a,uint32_t b,uint32_t *bits,uint32_t ms) {
    (void)a;(void)b;(void)bits;assert(ms<=30000);
    clock_ms+=ms; if(clock_ms>stop_ms)longjmp(stop,1); return 0;
}
void hub_cloud_get_snapshot_status(hub_cloud_snapshot_status_t *s)
{ *s=(hub_cloud_snapshot_status_t){.has_cached_snapshot=true,.has_complete_bbw=true,.is_connected=cloud_online}; }
int64_t hub_cloud_token_renewal_due_ms(void) { return 0; }
uint32_t hub_cloud_refresh_request_generation(void) { return 0; }
void hub_cloud_complete_refresh_request(uint32_t ticket) { (void)ticket; }
bool hub_cloud_refresh_is_running(void) { return false; }
esp_err_t hub_cloud_refresh(void) {
    assert(refreshes<32); refresh_times[refreshes++]=clock_ms;
    if(recover_cloud&&hub_ble_has_cached_state()) cloud_online=true;
    return ESP_OK;
}
/* FUNCTIONS */
static void run(bool online,bool recover,int64_t local_after) {
    clock_ms=1000;stop_ms=350000;refreshes=0;
    cloud_online=online;recover_cloud=recover;ble_after=local_after;
    if(!setjmp(stop))hub_state_keeper_task(NULL);
}
int main(void) {
    assert(ready_interval_ms(100,1,300000,false)==600000);
    assert(ready_interval_ms(400000,1,300000,false)==1800000);
    assert(ready_interval_ms(2000000,1,300000,false)==3600000);
    run(false,true,150000);
    assert(refreshes==2&&cloud_online&&refresh_times[1]>=150000&&refresh_times[1]<=180000);
    run(false,false,0);
    assert(refreshes==6);
    for(int i=1;i<refreshes;++i)assert(refresh_times[i]-refresh_times[i-1]==60000);
    run(false,false,INT64_MAX);assert(refreshes==1); // No fresh local evidence: retain idle policy.
    run(true,false,0);assert(refreshes==1); // Local polling never floods an online Cloud.
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
static bool confirmed;
static void copy_snapshot(cloud_snapshot_t *out,bool *cached) {
    memset(out,0,sizeof(*out));*cached=true;
    strcpy(out->serial_number,"SYNTHETIC");out->selection_generation=1;
    out->dose1_set=out->dose2_set=true;out->dose1=20;out->dose2=30;
    strcpy(out->bbw_mode,"Continuous");strcpy(out->last_updated,"2026-10-20T01:00:00Z");
}
static bool snapshot_matches_selection(const cloud_snapshot_t *s,bool cached) { (void)s;return cached; }
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *out)
{ *out=(hub_machine_selection_identity_t){"SYNTHETIC",1};return ESP_OK; }
esp_err_t hub_config_get_bbw_selection(const hub_machine_selection_identity_t *target,hub_bbw_selection_t *out) {
    assert(!strcmp(target->serial_number,"SYNTHETIC"));
    if(!confirmed)return ESP_FAIL;
    *out=(hub_bbw_selection_t){"SYNTHETIC","Dose2",1792458000.123};return ESP_OK;
}
static void json_safe_copy(char *out,size_t size,const char *in) { snprintf(out,size,"%s",in); }
const char *hub_cloud_machine_name(void) { return "Synthetic"; }
/* FUNCTIONS */
int main(void) {
    for(int i=0;i<2;++i) {
        confirmed=i;
        cJSON *j=cJSON_Parse(hub_cloud_machine_state_json());assert(j);
        assert(!strcmp(cJSON_GetObjectItem(j,"brewByWeightMode")->valuestring,i?"Dose2":"Continuous"));
        assert(!strcmp(cJSON_GetObjectItem(j,"connectionState")->valuestring,"unknown"));
        assert(!strcmp(cJSON_GetObjectItem(j,"lastUpdated")->valuestring,"2026-10-20T01:00:00Z"));
        assert(cJSON_IsNull(cJSON_GetObjectItem(j,"coffeeBoilerCurrentTemperature")));
        cJSON *record=cJSON_GetObjectItem(j,"brewByWeightSelection");
        assert(!strcmp(cJSON_GetObjectItem(record,"serialNumber")->valuestring,"SYNTHETIC"));
        assert(cJSON_IsObject(cJSON_GetObjectItem(record,"confirmation"))==!!i);
        cJSON_Delete(j);
    }
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
        config = (ROOT / "main/hub_config.c").read_text()
        compile_run(directory, "selection", SELECTION.replace("/* FUNCTIONS */",
            functions(config, ["bbw_selection_valid", "hub_config_get_bbw_selection", "hub_config_save_bbw_selection"])))
        cloud = (ROOT / "main/hub_cloud.c").read_text()
        types = re.search(r"typedef struct\s*\{[^}]+\}\s*cloud_snapshot_t;", cloud).group()
        body = "const char *hub_cloud_machine_state_json(void) {" + function_body(cloud, "hub_cloud_machine_state_json") + "}"
        json_dir = Path.home() / "esp/esp-idf/components/json/cJSON"
        compile_run(directory, "state", STATE.replace("/* TYPES */", types).replace("/* FUNCTIONS */", body),
            ["-I", json_dir, json_dir / "cJSON.c", "-Wno-deprecated-declarations"])
    print("State selection PASS: bounded reconnection recovery, durable scoped confirmations, stale/replay/failure guards")


if __name__ == "__main__":
    main()
