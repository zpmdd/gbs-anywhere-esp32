#!/usr/bin/env python3
"""Exercise the real S31 automation adapter with synthetic storage and commands."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_command_coordinator import ESP_ERR_STUB, FREERTOS_STUB, SEMPHR_STUB, TIMER_STUB

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "gbs_smart_steam.h"
#include "hub_command_coordinator.h"
#include "hub_cloud.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static int64_t ms=1000;
static time_t utc=1793577600;
static bool synchronized=true, busy, brewing, commit_failure, config;
static unsigned writes,commits;
static unsigned refreshes;
static hub_cloud_snapshot_status_t cloud;
static unsigned char blob[256];
static size_t blob_size;
static hub_command_lookup_status_t lookup=HUB_COMMAND_LOOKUP_INFLIGHT;
static hub_control_result_t command_result;
static esp_err_t command_error;
static hub_machine_selection_identity_t selected={.serial_number="SYNTHETIC",.generation=1};
int64_t esp_timer_get_time(void){return ms*1000;}
time_t time(time_t *out){if(out)*out=utc;return utc;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t){(void)s;(void)t;return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s){(void)s;return pdTRUE;}
esp_err_t nvs_open(const char *name,nvs_open_mode_t mode,nvs_handle_t *h)
{assert(!strcmp(name,"gbs_steam")&&mode==NVS_READWRITE);*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *value)
{(void)h;assert(!strcmp(key,"enabled"));*value=config;return ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value)
{(void)h;assert(!strcmp(key,"enabled"));config=value;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *value,size_t *size)
{(void)h;assert(!strcmp(key,"record"));if(!blob_size)return ESP_ERR_NVS_NOT_FOUND;assert(*size>=blob_size);memcpy(value,blob,blob_size);*size=blob_size;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size)
{(void)h;assert(!strcmp(key,"record")&&size<=sizeof(blob));memcpy(blob,value,size);blob_size=size;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;++commits;return commit_failure?ESP_FAIL:ESP_OK;}
const char *hub_cloud_time_status(void){return synchronized?"synchronized":"syncing";}
void hub_cloud_get_snapshot_status(hub_cloud_snapshot_status_t *out){*out=cloud;}
bool hub_cloud_refresh_pending(void){return false;}
esp_err_t hub_cloud_request_refresh(void){++refreshes;return ESP_OK;}
bool hub_grinder_busy(void){return busy;}
bool hub_brew_timer_is_brewing(void){return brewing;}
bool hub_wifi_station_has_ip(void){return true;}
bool hub_command_idle_background_deferred(void){return busy;}
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *out){*out=selected;return ESP_OK;}
hub_command_lookup_status_t hub_command_lookup(const char *id,esp_err_t *error,hub_control_result_t *result)
{assert(*id);*error=command_error;*result=command_result;return lookup;}
hub_command_submit_status_t hub_command_submit(const hub_command_request_t *c,hub_command_completion_fn fn,void *ctx,hub_command_submission_t *out)
{
    assert(c->kind==HUB_COMMAND_STEAM_ENABLED&&c->automatic_steam);
    assert(!strcmp(c->expected_serial_number,"SYNTHETIC")&&!fn&&!ctx);
    assert(blob_size&&commits&&!commit_failure&&gbs_smart_steam_write_allowed());
    ++writes;memset(out,0,sizeof(*out));strcpy(out->result.request_id,"00000000-0000-4000-8000-000000000001");
    return HUB_COMMAND_ACCEPTED;
}
static gbs_ui_machine_t machine(void)
{
    return (gbs_ui_machine_t){.available=true,.steam_enabled_known=true,.steam_enabled=true,.machine_mode=1,
        .ble={.valid=true,.connected=true,.cached_fresh=true,.source_observed_ms=ms}};
}
int main(void)
{
    assert(gbs_smart_steam_init()==ESP_OK);
    gbs_ui_machine_t m=machine();
    gbs_smart_steam_tick(&m);assert(!writes);
    assert(gbs_smart_steam_enable(true)==ESP_OK);
    synchronized=false;gbs_smart_steam_tick(&m);assert(!writes);
    synchronized=true;busy=true;gbs_smart_steam_tick(&m);assert(!writes);
    busy=false;brewing=true;gbs_smart_steam_tick(&m);assert(!writes);
    brewing=false;m.ble.source_observed_ms=0;gbs_smart_steam_tick(&m);assert(!writes);
    m=machine();gbs_smart_steam_tick(&m);assert(writes==1&&gbs_smart_steam_write_allowed());
    lookup=HUB_COMMAND_LOOKUP_COMPLETE;command_result.ok=true;command_result.state_confirmed=true;
    ms+=1000;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_APPLYING); // ACK is not a new model observation.
    m.steam_enabled=false;m.ble.source_observed_ms=ms;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_READY&&!gbs_smart_steam_write_allowed());
    gbs_smart_steam_manual_override();assert(!gbs_smart_steam_write_allowed());
    gbs_smart_steam_tick(&m);assert(gbs_smart_steam_status().phase==GBS_STEAM_MANUAL);
    assert(gbs_smart_steam_init()==ESP_OK);m=machine();gbs_smart_steam_tick(&m);
    assert(writes==1&&gbs_smart_steam_status().phase==GBS_STEAM_MANUAL);
    // New afternoon wake: the old morning journal does not suppress the new rule.
    utc+=6*3600;assert(gbs_smart_steam_init()==ESP_OK);
    m=machine();m.steam_enabled=false;lookup=HUB_COMMAND_LOOKUP_INFLIGHT;
    gbs_smart_steam_tick(&m);assert(writes==2&&gbs_smart_steam_status().target);
    gbs_smart_steam_manual_override();assert(!gbs_smart_steam_write_allowed());
    gbs_smart_steam_tick(&m);ms+=31000;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_MANUAL&&writes==2);
    // Persist-before-send failure prevents physical writes.
    utc+=86400;ms+=1000;assert(gbs_smart_steam_init()==ESP_OK);
    m=machine();m.steam_enabled=false;commit_failure=true;
    gbs_smart_steam_tick(&m);assert(writes==2&&gbs_smart_steam_status().phase==GBS_STEAM_STORAGE);
    assert(!gbs_smart_steam_write_allowed());
    // No Bluetooth: dated Cloud state is sufficient; stale state and ACK alone are not.
    utc=1793664000;ms=200000;commit_failure=false;blob_size=0;lookup=HUB_COMMAND_LOOKUP_INFLIGHT;
    command_result=(hub_control_result_t){0};cloud=(hub_cloud_snapshot_status_t){.has_cached_snapshot=true,
        .is_connected=true,.machine_powered_on=true,.steam_enabled_known=true,.steam_enabled=true,
        .boiler_observed_ms=ms-90000};m=(gbs_ui_machine_t){0};
    assert(gbs_smart_steam_init()==ESP_OK);
    unsigned before_refresh=refreshes;
    gbs_smart_steam_tick(&m);gbs_smart_steam_tick(&m);
    assert(writes==2&&refreshes==before_refresh+1);
    cloud.boiler_observed_ms=ms;cloud.machine_brewing=true;
    gbs_smart_steam_tick(&m);assert(writes==2);
    cloud.machine_brewing=false;gbs_smart_steam_tick(&m);assert(writes==3&&gbs_smart_steam_write_allowed());
    cloud.is_connected=false;assert(!gbs_smart_steam_write_allowed());
    cloud.is_connected=true;cloud.machine_brewing=true;assert(!gbs_smart_steam_write_allowed());
    cloud.machine_brewing=false;assert(gbs_smart_steam_write_allowed());
    lookup=HUB_COMMAND_LOOKUP_COMPLETE;command_result.ok=true;command_result.upstream_attempts=1;
    cloud.steam_enabled=false;ms+=1000;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_APPLYING);
    cloud.boiler_observed_ms=ms;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_READY&&writes==3);
    cloud.steam_enabled=true;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_MANUAL&&writes==3);
    // A Cloud ACK without a matching observation becomes unknown and is never replayed.
    utc+=86400;assert(gbs_smart_steam_init()==ESP_OK);cloud.boiler_observed_ms=ms;
    int64_t sent_ms=ms;
    gbs_smart_steam_tick(&m);assert(writes==4);
    ms+=31000;cloud.boiler_observed_ms=ms;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_APPLYING);
    ms+=30000;cloud.boiler_observed_ms=ms;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_UNKNOWN&&writes==4);
    cloud.steam_enabled=false;cloud.boiler_observed_ms=sent_ms;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase!=GBS_STEAM_READY&&writes==4); // Pre-write data cannot resolve UNKNOWN.
    cloud.steam_enabled=true;cloud.boiler_observed_ms=ms;
    assert(gbs_smart_steam_init()==ESP_OK);gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_UNKNOWN&&writes==4);
    // Reconcile delayed/current-boot data and a restored journal, never by resending the command.
    cloud.steam_enabled=false;cloud.boiler_observed_ms=ms-90000;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase!=GBS_STEAM_READY&&writes==4);
    cloud.boiler_observed_ms=ms;cloud.steam_enabled_known=false;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase!=GBS_STEAM_READY&&writes==4);
    cloud.steam_enabled_known=true;cloud.is_connected=false;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase!=GBS_STEAM_READY&&writes==4);
    cloud.is_connected=true;cloud.machine_powered_on=false;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase!=GBS_STEAM_READY&&writes==4);
    cloud.machine_powered_on=true;gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_READY&&writes==4&&!gbs_smart_steam_write_allowed());
    assert(gbs_smart_steam_init()==ESP_OK);gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_READY&&writes==4);
    gbs_smart_steam_manual_override();gbs_smart_steam_tick(&m);
    assert(gbs_smart_steam_status().phase==GBS_STEAM_MANUAL&&writes==4);
    puts("Smart steam runtime: BLE/Cloud freshness, bounded reads, storage-before-send, confirmation, manual cancellation and no replay PASS");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="smart-steam-test-") as directory:
        temp = Path(directory)
        (temp / "freertos").mkdir()
        (temp / "esp_err.h").write_text(ESP_ERR_STUB + "\n#define ESP_ERR_NVS_NOT_FOUND 0x1102\n")
        (temp / "esp_timer.h").write_text(TIMER_STUB)
        (temp / "freertos/FreeRTOS.h").write_text(FREERTOS_STUB)
        (temp / "freertos/semphr.h").write_text(SEMPHR_STUB)
        nvs = (ROOT / "integrated/tests/stubs/nvs.h").read_text()
        (temp / "nvs.h").write_text(nvs + "\nesp_err_t nvs_get_u8(nvs_handle_t,const char*,uint8_t*);\nesp_err_t nvs_set_u8(nvs_handle_t,const char*,uint8_t);\n")
        (temp / "test.c").write_text(HARNESS)
        display = ROOT / "integrated/display"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-g", "-I", str(temp), "-I", str(display),
                        "-I", str(ROOT / "main"), str(temp / "test.c"), str(display / "gbs_smart_steam.c"),
                        str(display / "gbs_steam_policy.c"), "-o", str(temp / "test")], check=True)
        subprocess.run([str(temp / "test")], check=True)


if __name__ == "__main__":
    main()
