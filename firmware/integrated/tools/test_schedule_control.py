#!/usr/bin/env python3
"""Run the production schedule parser and edit transaction against synthetic IO."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
from validate_stability_foundation import function_body
from test_quick_weight_control import ESP_ERR_STUB

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
HARNESS = r'''
#include "hub_cloud.h"
#include "hub_schedule.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static unsigned reads,posts;
static bool changed,rebound,miss,fail_read,lost,denied;
static hub_schedule_entry_t current;
static int64_t esp_timer_get_time(void) { return 1000000; }
static void *heap_caps_calloc(size_t count,size_t size,int flags) { (void)flags; return calloc(count,size); }
static void set_command_result(hub_cloud_command_result_t *r,bool ok,int64_t started,const char *message)
{ (void)started; r->ok=ok; snprintf(r->message,sizeof(r->message),"%s",message); }
static void set_command_attempt(hub_cloud_command_result_t *r,const hub_cloud_command_attempt_t *a)
{ r->request_started=a->request_started; r->upstream_attempts=a->attempts; r->http_status=a->http_status; }
static esp_err_t ensure_cloud_controls_ready_or_fail(hub_cloud_command_result_t *r,int64_t start)
{ (void)r; (void)start; return denied?ESP_ERR_INVALID_STATE:ESP_OK; }
static bool active_command_target(hub_machine_selection_identity_t *target)
{ strcpy(target->serial_number,"SYNTHETIC-MACHINE"); target->generation=1; return true; }
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *target)
{ assert(target->generation==1); return !rebound; }
esp_err_t hub_cloud_schedule_json(char *out,size_t size)
{
    ++reads; if(fail_read) return ESP_FAIL;
    hub_schedule_entry_t row=current; if(changed) ++row.on_minutes;
    cJSON *j=cJSON_CreateObject(),*raw=cJSON_CreateObject(),*settings=cJSON_CreateObject(),*rows=cJSON_CreateArray();
    cJSON_AddItemToObject(j,"raw",raw); cJSON_AddStringToObject(raw,"serialNumber","SYNTHETIC-MACHINE");
    cJSON_AddTrueToObject(raw,"smartWakeUpSleepSupported"); cJSON_AddItemToObject(raw,"smartWakeUpSleep",settings);
    cJSON_AddItemToObject(settings,"schedules",rows); cJSON_AddItemToArray(rows,hub_schedule_json(&row));
    assert(cJSON_PrintPreallocated(j,out,(int)size,false)); cJSON_Delete(j); return ESP_OK;
}
static hub_cloud_command_attempt_t execute_high_risk_command_with_auth(const char *name,const char *body)
{
    ++posts; assert(posts==1&&!strcmp(name,"CoffeeMachineSetWakeUpSchedule"));
    cJSON *j=cJSON_Parse(body); hub_schedule_entry_t next={0}; assert(hub_schedule_parse(j,&next)); cJSON_Delete(j);
    if(!miss) current=next;
    return (hub_cloud_command_attempt_t){.err=lost?ESP_ERR_TIMEOUT:ESP_OK,.request_started=true,.attempts=1,.http_status=lost?0:200};
}
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *target)
{
    if(denied) return ESP_FAIL;
    strcpy(target->serial_number,"SYNTHETIC-MACHINE"); target->generation=1; return ESP_OK;
}
/* PRODUCTION_BINDING */
/* PRODUCTION_TRANSACTION */
static void reset(void)
{
    reads=posts=0; changed=rebound=miss=fail_read=lost=denied=false;
    current=(hub_schedule_entry_t){.id="plan-a",.on_minutes=420,.off_minutes=570,.days=31,.enabled=true,.steam=true};
}
int main(void)
{
    reset();
    cJSON *binding=cJSON_Parse(hub_state_machine_binding_json());
    assert(cJSON_IsTrue(cJSON_GetObjectItem(binding,"selected")));
    assert(!strcmp(cJSON_GetObjectItem(binding,"serialNumber")->valuestring,"SYNTHETIC-MACHINE"));
    assert(cJSON_GetObjectItem(binding,"generation")->valueint==1); cJSON_Delete(binding);
    denied=true; binding=cJSON_Parse(hub_state_machine_binding_json());
    assert(cJSON_IsFalse(cJSON_GetObjectItem(binding,"selected")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(binding,"serialNumber")));
    assert(cJSON_GetObjectItem(binding,"generation")->valueint==0); cJSON_Delete(binding);
    reset(); hub_schedule_entry_t expected=current,next=current; next.on_minutes=480; next.steam=false;
    hub_cloud_command_result_t result={0};
    assert(hub_cloud_set_schedule(&expected,&next,&result)==ESP_OK&&result.ok&&posts==1&&reads==2);
    reset(); changed=true; assert(hub_cloud_set_schedule(&expected,&next,&result)!=ESP_OK&&result.schedule_changed&&!posts);
    reset(); rebound=true; assert(hub_cloud_set_schedule(&expected,&next,&result)!=ESP_OK&&!posts);
    reset(); fail_read=true; assert(hub_cloud_set_schedule(&expected,&next,&result)!=ESP_OK&&!posts);
    reset(); denied=true; assert(hub_cloud_set_schedule(&expected,&next,&result)!=ESP_OK&&!posts&&!reads);
    reset(); lost=true; assert(hub_cloud_set_schedule(&expected,&next,&result)!=ESP_OK&&result.request_started&&!result.ok&&posts==1&&reads==1);
    reset(); miss=true; assert(hub_cloud_set_schedule(&expected,&next,&result)!=ESP_OK&&result.request_started&&!result.ok&&posts==1);
    reset(); next.on_minutes=1440; assert(hub_cloud_set_schedule(&expected,&next,&result)==ESP_ERR_INVALID_ARG&&!posts);
    next=expected; next.days=0; assert(hub_cloud_set_schedule(&expected,&next,&result)==ESP_ERR_INVALID_ARG&&!posts);
    next=expected; next.off_minutes=next.on_minutes; assert(hub_cloud_set_schedule(&expected,&next,&result)==ESP_ERR_INVALID_ARG&&!posts);
    next=expected; strcpy(next.id,"other"); assert(hub_cloud_set_schedule(&expected,&next,&result)==ESP_ERR_INVALID_ARG&&!posts);
    cJSON *j=hub_schedule_json(&expected); assert(j); hub_schedule_entry_t parsed={0};
    assert(hub_schedule_parse(j,&parsed)&&hub_schedule_equal(&parsed,&expected));
    cJSON_ReplaceItemInObject(j,"onTimeMinutes",cJSON_CreateNumber(0.5)); assert(!hub_schedule_parse(j,&parsed));
    cJSON_ReplaceItemInObject(j,"onTimeMinutes",cJSON_CreateNumber(420));
    cJSON_AddItemToArray(cJSON_GetObjectItem(j,"days"),cJSON_CreateString("Monday")); assert(!hub_schedule_parse(j,&parsed));
    cJSON_Delete(j);
    puts("schedule parser/transaction: valid edit, conflict, rebind, bounds, duplicate days, no replay and exact readback passed");
}
'''

CACHE_HARNESS = r'''
#include "esp_err.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define SLOW_CLOUD_READ_BODY_BYTES 32768
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(value) (value)
#define ESP_LOGW(...) ((void)0)
typedef enum { SLOW_CLOUD_READ_STATISTICS, SLOW_CLOUD_READ_SCHEDULE, SLOW_CLOUD_READ_FIRMWARE } slow_cloud_read_kind_t;
typedef struct { char serial[32]; } hub_statistics_cache_ticket_t;
/* PRODUCTION_TYPES */
static unsigned schedule_epoch;
static slow_cloud_read_cache_t cache;
static int slow_cloud_read_mutex;
static bool invalidate_during_read;
static int xSemaphoreTake(int mutex,int timeout) { (void)mutex; (void)timeout; return pdTRUE; }
static void xSemaphoreGive(int mutex) { (void)mutex; }
static esp_err_t ensure_slow_cloud_read_mutex(void) { return ESP_OK; }
static int64_t esp_timer_get_time(void) { return 1000000; }
static unsigned esp_get_free_heap_size(void) { return 65536; }
static unsigned internal_free_heap(void) { return 65536; }
static unsigned internal_largest_free_block(void) { return 32768; }
static slow_cloud_read_cache_t *slow_cloud_read_cache(slow_cloud_read_kind_t kind) { (void)kind; return &cache; }
static bool take_pending_slow_cloud_read(slow_cloud_read_request_t *request)
{
    if(!cache.running||cache.started_ms) return false;
    request->kind=SLOW_CLOUD_READ_SCHEDULE; cache.started_ms=1000; return true;
}
static char *slow_cloud_read_alloc_body(void) { return calloc(1,SLOW_CLOUD_READ_BODY_BYTES); }
static esp_err_t hub_cloud_statistics_json_alloc_for_serial(const char *serial,int days,char **body)
{ (void)serial; (void)days; (void)body; return ESP_FAIL; }
static esp_err_t hub_cloud_firmware_json(char *out,size_t size) { (void)out; (void)size; return ESP_FAIL; }
static esp_err_t hub_statistics_cache_complete(const hub_statistics_cache_ticket_t *ticket,esp_err_t err,char *body,int64_t at)
{ (void)ticket; (void)body; (void)at; return err; }
void hub_state_invalidate_schedule(void);
static esp_err_t hub_cloud_schedule_json(char *out,size_t size)
{
    snprintf(out,size,"%s",invalidate_during_read?"old-plan":"updated-plan");
    if(invalidate_during_read) hub_state_invalidate_schedule();
    return ESP_OK;
}
/* PRODUCTION_CACHE */
int main(void)
{
    cache.running=true; cache.valid=true; invalidate_during_read=true;
    assert(hub_state_process_pending_slow_cloud_read());
    assert(!cache.valid&&!cache.body&&cache.running&&!cache.started_ms);
    invalidate_during_read=false;
    assert(hub_state_process_pending_slow_cloud_read());
    assert(cache.valid&&!cache.running&&!strcmp(cache.body,"updated-plan"));
    hub_state_invalidate_schedule();
    assert(!cache.valid&&!cache.completed_ms&&!cache.running);
    free(cache.body);
    puts("schedule cache: an in-flight stale read is discarded after edit; the next refresh installs new data");
}
'''

def main():
    source=(MAIN/"hub_cloud.c").read_text()
    signature="esp_err_t hub_cloud_set_schedule(const hub_schedule_entry_t *expected,const hub_schedule_entry_t *requested,hub_cloud_command_result_t *result)"
    transaction=signature+" {"+function_body(source,"hub_cloud_set_schedule")+"}"
    state=(MAIN/"hub_state.c").read_text()
    binding="static void copy_json_safe(char *dest,size_t dest_len,const char *source) {"+function_body(state,"copy_json_safe")+"}\n"
    binding+="const char *hub_state_machine_binding_json(void) {"+function_body(state,"hub_state_machine_binding_json")+"}"
    json_dir=Path(os.environ["IDF_PATH"])/"components/json/cJSON"
    with tempfile.TemporaryDirectory(prefix="schedule-host-") as temp:
        directory=Path(temp)
        (directory/"esp_err.h").write_text(ESP_ERR_STUB)
        (directory/"harness.c").write_text(HARNESS.replace("/* PRODUCTION_TRANSACTION */",transaction).replace("/* PRODUCTION_BINDING */",binding))
        command=[os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror","-Wno-deprecated-declarations",
            "-fsanitize=address,undefined","-g","-I",str(directory),"-I",str(MAIN),"-I",str(json_dir),
            str(directory/"harness.c"),str(MAIN/"hub_schedule.c"),str(json_dir/"cJSON.c"),"-lm","-o",str(directory/"test")]
        subprocess.run(command,check=True)
        subprocess.run([str(directory/"test")],check=True)

        state=(MAIN/"hub_state.c").read_text()
        types="\n".join(re.search(r"typedef struct \{[^}]+\} "+name+r";",state).group(0)
                        for name in ("slow_cloud_read_request_t","slow_cloud_read_cache_t"))
        functions="void hub_state_invalidate_schedule(void) {"+function_body(state,"hub_state_invalidate_schedule")+"}\n"
        functions+="bool hub_state_process_pending_slow_cloud_read(void) {"+function_body(state,"hub_state_process_pending_slow_cloud_read")+"}"
        (directory/"cache.c").write_text(CACHE_HARNESS.replace("/* PRODUCTION_TYPES */",types).replace("/* PRODUCTION_CACHE */",functions))
        subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
            "-fsanitize=address,undefined","-I",str(directory),str(directory/"cache.c"),"-o",str(directory/"cache")],check=True)
        subprocess.run([str(directory/"cache")],check=True)

    http=(MAIN/"hub_http.c").read_text()
    assert '{"/machine/binding", HTTP_GET, machine_binding_handler, false}' in http
    binding_handler=function_body(http,"machine_binding_handler")
    assert 'require_authorized(req)' in binding_handler and '"no-store"' in binding_handler
    assert '{"/controls/schedule", HTTP_POST, schedule_control_handler, false}' in http
    handler=function_body(http,"schedule_control_handler")
    assert 'require_authorized(req)' in handler and 'apply_v2_command_metadata' in handler
    assert '"confirm"' in handler and 'submit_control_command' in handler

if __name__=="__main__": main()
