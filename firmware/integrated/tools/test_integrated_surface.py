#!/usr/bin/env python3
"""Run the real local UI adapter and command parser against synthetic Hub services."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_command_coordinator import ESP_ERR_STUB

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "hub_touchscreen.h"
#include "hub_state.h"
#include "gbs_hub.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static int64_t clock_ms=1000;
static bool busy,locked;
static int submissions;
static double weight=30;
static const char *readiness="ready";
static char completed_id[37];
static const char *ID="11111111-1111-4111-8111-111111111111";
int64_t esp_timer_get_time(void) { return clock_ms*1000; }
bool hub_http_local_lock(void) { if(busy) return false; assert(!locked); locked=true; return true; }
void hub_http_local_unlock(void) { assert(locked); locked=false; }
bool hub_command_request_id_valid(const char *id) { return id&&strlen(id)==36; }
const char *hub_cloud_command_readiness(void) { return readiness; }
const char *hub_state_machine_binding_json(void)
{ return "{\"selected\":true,\"serialNumber\":\"SYNTHETIC\",\"generation\":1}"; }
const char *hub_state_capabilities_json(void)
{ return "{\"supportsSmartPresetDecisionValidation\":true,\"brewByWeightDose1\":{\"available\":true},\"brewByWeightDose2\":{\"available\":true},\"brewByWeightMode\":{\"available\":true},\"schedule\":{\"available\":true}}"; }
const char *hub_state_machine_state_json(void)
{
    static char data[1024];
    snprintf(data,sizeof(data),"{\"connectionState\":\"ready\",\"coffeeBoilerCurrentTemperature\":92.1,\"coffeeBoilerTargetTemperature\":93,\"steamBoilerCurrentTemperature\":127.2,\"steamBoilerTargetTemperature\":128,\"brewByWeightDose1\":%.0f,\"brewByWeightDose2\":35,\"brewByWeightMode\":\"Dose1\"}",weight);
    return data;
}
hub_state_slow_read_result_t hub_state_schedule_read(void)
{ return (hub_state_slow_read_result_t){.body="{\"status\":\"refreshing\"}",.state=HUB_STATE_SLOW_READ_REFRESHING}; }
hub_state_slow_read_result_t hub_state_statistics_read(int days)
{ assert(days==7); return (hub_state_slow_read_result_t){.body="{\"recentCoffees\":[]}",.state=HUB_STATE_SLOW_READ_HIT,.stale=true}; }
const char *hub_state_start_slow_cloud_read_json(const char *kind,int days)
{ assert(!strcmp(kind,"schedule")&&days==7); return "{\"ok\":true,\"accepted\":true,\"scheduled\":true,\"kind\":\"schedule\"}"; }
hub_command_submit_status_t hub_command_submit(const hub_command_request_t *c,hub_command_completion_fn fn,void *ctx,hub_command_submission_t *out)
{
    (void)fn; (void)ctx; assert(locked); ++submissions;
    assert(!strcmp(c->expected_serial_number,"SYNTHETIC"));
    assert(c->kind==HUB_COMMAND_QUICK_WEIGHT&&c->deadline.enforced);
    assert(c->value.quick_weight.require_no_stored_match);
    weight=c->value.quick_weight.grams; strcpy(completed_id,c->request_id);
    memset(out,0,sizeof(*out)); strcpy(out->result.request_id,c->request_id);
    return HUB_COMMAND_ACCEPTED;
}
void hub_control_result_json(const hub_control_result_t *r,char *out,size_t size)
{ snprintf(out,size,"{\"apiVersion\":2,\"requestId\":\"%s\",\"ok\":true,\"status\":\"accepted\",\"pending\":true}",r->request_id); }
void hub_command_diagnostics_json(const char *id,char *out,size_t size)
{ assert(!strcmp(id,completed_id)); snprintf(out,size,"{\"apiVersion\":2,\"requestId\":\"%s\",\"ok\":true,\"status\":\"completed\"}",id); }
static int64_t now(void *p) { (void)p; return clock_ms; }
static void wait_ms(void *p,unsigned ms) { (void)p; clock_ms+=ms; }
static int exchange(void *p,const char *path,const char *body,char *reply,size_t size,int64_t deadline)
{ (void)p; bool stale; return hub_touchscreen_exchange(path,body,"SYNTHETIC",reply,size,deadline,&stale); }
int main(void)
{
    gbs_ui_machine_t machine={.available=true,.writable=GBS_UI_MACHINE_ACTIONS,.steam_enabled_known=true,.standby_enabled_known=true,.dose1=30,.dose2=35,.dose_mode=1,.prebrew_mode=1,.prebrew_on=3,.prebrew_off=2,.standby_enabled=true,.standby_minutes=30,.standby_mode=0};
    const struct { gbs_ui_action_t action; double value; hub_command_kind_t kind; } cases[]={
        {GBS_ACTION_WEIGHT,40,HUB_COMMAND_QUICK_WEIGHT},{GBS_ACTION_WEIGHT,35,HUB_COMMAND_BBW_MODE},
        {GBS_ACTION_COFFEE,93,HUB_COMMAND_COFFEE_TEMPERATURE},{GBS_ACTION_STEAM,128,HUB_COMMAND_STEAM_LEVEL},
        {GBS_ACTION_STEAM_ENABLED,0,HUB_COMMAND_STEAM_ENABLED},{GBS_ACTION_DOSE1,40,HUB_COMMAND_BBW_DOSE},
        {GBS_ACTION_DOSE2,45,HUB_COMMAND_BBW_DOSE},{GBS_ACTION_BBW_MODE,2,HUB_COMMAND_BBW_MODE},
        {GBS_ACTION_PREBREW_MODE,1,HUB_COMMAND_PREBREW},{GBS_ACTION_PREBREW_ON,2.5,HUB_COMMAND_PREBREW},
        {GBS_ACTION_PREBREW_OFF,1,HUB_COMMAND_PREBREW},{GBS_ACTION_POWER,0,HUB_COMMAND_POWER},
        {GBS_ACTION_STANDBY_ENABLED,1,HUB_COMMAND_SMART_STANDBY},{GBS_ACTION_STANDBY_MINUTES,60,HUB_COMMAND_SMART_STANDBY},
        {GBS_ACTION_STANDBY_MODE,1,HUB_COMMAND_SMART_STANDBY}
    };
    char path[96],body[2048],reply[32768]; hub_command_request_t command;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        assert(gbs_hub_prepare(cases[i].action,cases[i].value,&machine,ID,path,sizeof(path),body,sizeof(body)));
        assert(hub_touchscreen_parse_command(path,body,"SYNTHETIC",&command));
        assert(command.kind==cases[i].kind);
        assert(command.client_surface==HUB_API_V2_SURFACE_DIAGNOSTIC);
        if(i==0) assert(command.value.quick_weight.require_no_stored_match);
        if(i==1) assert(command.value.bbw_mode.has_preset_grams&&command.value.bbw_mode.preset_grams==35);
    }
    snprintf(body,sizeof(body),"{\"requestId\":\"%s\",\"confirm\":false,\"enabled\":true}",ID);
    assert(!hub_touchscreen_parse_command("/controls/power",body,"SYNTHETIC",&command));
    snprintf(body,sizeof(body),"{\"requestId\":\"%s\",\"grams\":1e100}",ID);
    assert(!hub_touchscreen_parse_command("/controls/quick-weight",body,"SYNTHETIC",&command));
    assert(!hub_touchscreen_parse_command("/controls/quick-weight","{}","SYNTHETIC",&command));
    assert(!hub_touchscreen_parse_command("/controls/quick-weight",body,"",&command));
    gbs_hub_config_t config={.hub_id="local",.serial="SYNTHETIC"};
    gbs_hub_io_t io={.exchange=exchange,.now=now,.wait=wait_ms,.reply=reply,.reply_size=sizeof(reply)};
    assert(gbs_hub_refresh(&config,&io,10000,&machine));
    assert(machine.coffee==92.1&&machine.steam==127.2&&machine.weight==30);
    assert(gbs_hub_execute(&config,&io,GBS_ACTION_WEIGHT,40,ID,10000,&machine)==GBS_UI_CONTROL_CONFIRMED);
    assert(submissions==1&&machine.weight==40&&!locked);
    readiness="warming";
    assert(gbs_hub_refresh(&config,&io,10000,&machine)&&machine.available&&!machine.stale);
    assert(machine.coffee==92.1&&!(machine.writable&(1u<<GBS_ACTION_WEIGHT)));
    readiness="ready";
    assert(gbs_hub_refresh(&config,&io,10000,&machine)&&(machine.writable&(1u<<GBS_ACTION_WEIGHT)));
    bool stale=false;
    assert(hub_touchscreen_exchange("/machine/statistics?days=7",NULL,"SYNTHETIC",reply,sizeof(reply),10000,&stale)==200&&stale);
    assert(hub_touchscreen_exchange("/machine/schedule",NULL,"SYNTHETIC",reply,sizeof(reply),10000,&stale)==503);
    assert(hub_touchscreen_exchange("/diagnostics/slow-cloud-reads/refresh?kind=schedule","{}","SYNTHETIC",reply,sizeof(reply),10000,&stale)==200);
    busy=true; assert(hub_touchscreen_exchange("/machine/state",NULL,"SYNTHETIC",reply,sizeof(reply),10000,&stale)==503);
    busy=false; assert(hub_touchscreen_exchange("/machine/state",NULL,"SYNTHETIC",reply,sizeof(reply),clock_ms,&stale)==-1);
    assert(submissions==1&&!locked);
    puts("Integrated surface: 15 actions, local telemetry/write/readback, stale cache and admission PASS");
}
'''

def main():
    idf=Path(os.environ.get("IDF_PATH",Path.home()/"esp/esp-idf"))
    with tempfile.TemporaryDirectory(prefix="hub-integrated-test-") as directory:
        d=Path(directory)
        (d/"esp_err.h").write_text(ESP_ERR_STUB)
        (d/"esp_timer.h").write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
        (d/"esp_http_server.h").write_text('typedef void *httpd_handle_t;\ntypedef struct httpd_req httpd_req_t;\n')
        (d/"test.c").write_text(HARNESS)
        cmd=["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-deprecated-declarations","-fsanitize=address,undefined","-g", "-I",str(d),"-I",str(ROOT/"main"),"-I",str(ROOT/"integrated/display"),"-I",str(idf/"components/json/cJSON")]
        sources=[ROOT/"main/hub_touchscreen_command.c",ROOT/"main/hub_touchscreen.c",ROOT/"main/hub_schedule.c",ROOT/"integrated/display/gbs_hub.c",ROOT/"integrated/display/gbs_ui_model.c",idf/"components/json/cJSON/cJSON.c",d/"test.c"]
        subprocess.run(cmd+[str(p) for p in sources]+["-lm","-o",str(d/"test")],check=True)
        subprocess.run([str(d/"test")],check=True)
if __name__=="__main__": main()
