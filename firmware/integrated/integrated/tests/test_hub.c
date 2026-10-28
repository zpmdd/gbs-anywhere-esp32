#include "gbs_hub.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *ID="11111111-1111-4111-8111-111111111111";
static gbs_hub_config_t config={.hub_id="22222222-2222-4222-8222-222222222222",.serial="SYNTHETIC-MACHINE"};
static struct {
    int64_t now;
    unsigned posts,polls,bindings,mode;
    double coffee,steam,dose1,dose2,prebrew_on,prebrew_off;
    int standby_minutes;
    bool standby_enabled,corrupt_preserved;
    char prebrew_mode[24],machine_mode[24],standby_mode[24];
    const char *state_patch,*unavailable,*ble_reply;
    unsigned ble_reads;
    int ble_status;
    bool enabled,lost_reply,never_ack,partial,unknown,readback_mismatch,auth_fail,wrong_hub,wrong_machine;
    bool binding_changed,missing_measurement,cloud_offline,legacy_presets,post_unknown;
    char path[96],body[256];
} fake;
static const char *const bbw_modes[]={"Continuous","Dose1","Dose2","PulsesType"};
static int64_t clock_ms(void *unused) { (void)unused; return fake.now; }
static void pause_ms(void *unused,unsigned ms) { (void)unused; fake.now+=ms; }
static int exchange(void *unused,const char *path,const char *body,char *reply,size_t size,int64_t deadline)
{
    (void)unused; assert(fake.now<deadline); fake.now+=20;
    if(fake.auth_fail) return 401;
    if(body) {
        assert(!strncmp(path,"/controls/",10)); ++fake.posts;
        assert(fake.posts==1); // The client must never retry a control POST.
        snprintf(fake.path,sizeof(fake.path),"%s",path); snprintf(fake.body,sizeof(fake.body),"%s",body);
        cJSON *j=cJSON_Parse(body); assert(j);
        assert(!strcmp(cJSON_GetObjectItem(j,"requestId")->valuestring,ID));
        assert(!strcmp(cJSON_GetObjectItem(j,"clientSurface")->valuestring,"diagnostic"));
        if(!strcmp(path,"/controls/coffee-temperature")||!strcmp(path,"/controls/steam-level")||!strcmp(path,"/controls/steam/enabled")) {
            assert(strlen(body)<128); assert(!cJSON_GetObjectItem(j,"clientTtlMs"));
        }
        else if(!strcmp(path,"/controls/quick-weight")) {
            assert(cJSON_IsTrue(cJSON_GetObjectItem(j,"requireNoStoredMatch")));
            assert(cJSON_GetObjectItem(j,"clientTtlMs")->valueint==12000);
        }
        if(!strcmp(path,"/controls/bbw/mode")&&cJSON_GetObjectItem(j,"presetGrams"))
            assert(cJSON_GetObjectItem(j,"clientTtlMs")->valueint==12000);
        if(!strcmp(path,"/controls/power")||!strcmp(path,"/controls/smart-standby")) {
            assert(cJSON_IsTrue(cJSON_GetObjectItem(j,"confirm")));
            assert(strlen(body)<(!strcmp(path,"/controls/power")?128u:256u));
        }
        cJSON_Delete(j);
        if(fake.lost_reply) return -1;
        if(fake.post_unknown) {
            snprintf(reply,size,"{\"apiVersion\":2,\"requestId\":\"%s\",\"status\":\"unknownOutcome\"}",ID);
            return 503;
        }
        snprintf(reply,size,"{\"apiVersion\":2,\"requestId\":\"%s\",\"status\":\"accepted\",\"pending\":true}",ID);
        return 202;
    }
    if(!strcmp(path,"/machine/binding")) {
        ++fake.bindings;
        snprintf(reply,size,"{\"selected\":true,\"serialNumber\":\"%s\",\"generation\":%u}",
            fake.wrong_machine?"ANOTHER-MACHINE":config.serial,fake.binding_changed?fake.bindings:1);
    } else if(!strcmp(path,"/machine/capabilities")) {
        snprintf(reply,size,"{\"apiVersion\":2,\"hubId\":\"%s\",\"supportsSmartPresetDecisionValidation\":%s,"
            "\"brewByWeightDose1\":{\"available\":true},\"brewByWeightMode\":{\"available\":true},"
            "\"brewByWeightDose2\":{\"available\":true},\"prebrew\":{\"available\":true},"
            "\"power\":{\"available\":true},\"smartStandby\":{\"available\":true},"
            "\"coffeeTemperature\":{\"available\":true},\"steamLevel\":{\"available\":true},\"steamEnabled\":{\"available\":true}}",
            fake.wrong_hub?ID:config.hub_id,fake.legacy_presets?"false":"true");
        if(fake.unavailable) {
            cJSON *j=cJSON_Parse(reply); assert(j);
            cJSON_ReplaceItemInObject(cJSON_GetObjectItem(j,fake.unavailable),"available",cJSON_CreateFalse());
            assert(cJSON_PrintPreallocated(j,reply,(int)size,false)); cJSON_Delete(j);
        }
    } else if(!strcmp(path,"/machine/state")) {
        snprintf(reply,size,"{\"apiVersion\":2,\"hubId\":\"%s\",\"connectionState\":\"ready\","
            "\"coffeeBoilerCurrentTemperature\":%s,\"coffeeBoilerTargetTemperature\":%.1f,"
            "\"steamBoilerCurrentTemperature\":%s,\"steamBoilerTargetTemperature\":%.0f,\"steamBoilerEnabled\":%s,"
            "\"brewByWeightDose1\":%.0f,\"brewByWeightDose2\":%.0f,\"brewByWeightMode\":\"%s\","
            "\"prebrew\":{\"mode\":\"%s\",\"onTime\":%.1f,\"offTime\":%.1f},\"machineMode\":\"%s\","
            "\"smartStandby\":{\"enabled\":%s,\"minutes\":%d,\"mode\":\"%s\"},"
            "\"thingScaleConnected\":null,\"scaleConnected\":null,\"scaleBatteryLevel\":null,\"scaleObservedMs\":0,"
            "\"coffeeBoilerReadyTime\":null,\"steamBoilerReadyTime\":null,\"lastUpdated\":null,"
            "\"activeDose\":\"dose1\",\"cloudCommandReadiness\":\"%s\"}",config.hub_id,
            fake.missing_measurement?"null":"91.5",fake.coffee,fake.missing_measurement?"null":"127",fake.steam,
            fake.enabled?"true":"false",fake.dose1,fake.dose2,bbw_modes[fake.mode],
            fake.prebrew_mode,fake.prebrew_on,fake.prebrew_off,fake.machine_mode,
            fake.standby_enabled?"true":"false",fake.standby_minutes,fake.standby_mode,fake.cloud_offline?"degraded":"ready");
        if(fake.state_patch) {
            cJSON *j=cJSON_Parse(reply),*patch=cJSON_Parse(fake.state_patch),*v=NULL; assert(j&&patch);
            cJSON_ArrayForEach(v,patch) assert(cJSON_ReplaceItemInObject(j,v->string,cJSON_Duplicate(v,true)));
            assert(cJSON_PrintPreallocated(j,reply,(int)size,false)); cJSON_Delete(j); cJSON_Delete(patch);
        }
    } else if(!strcmp(path,"/diagnostics/ble")) {
        ++fake.ble_reads;
        if(fake.ble_status) return fake.ble_status;
        if(fake.ble_reply) snprintf(reply,size,"%s",fake.ble_reply);
        else snprintf(reply,size,"{\"apiVersion\":2,\"hubId\":\"%s\",\"configured\":true,"
            "\"workerConnected\":false,\"controlSessionReady\":false,\"cachedConnected\":true,\"workerStatus\":\"backoff\"}",config.hub_id);
    } else {
        assert(!strncmp(path,"/diagnostics/commands?id=",24)); ++fake.polls;
        bool pending=fake.never_ack||fake.polls==1;
        if(!pending&&!fake.readback_mismatch&&!fake.partial&&!fake.unknown) {
            cJSON *j=cJSON_Parse(fake.body); assert(j);
            if(!strcmp(fake.path,"/controls/coffee-temperature")) fake.coffee=cJSON_GetObjectItem(j,"temperature")->valuedouble;
            else if(!strcmp(fake.path,"/controls/steam-level")) {
                const int values[]={126,128,131}; fake.steam=values[cJSON_GetObjectItem(j,"level")->valueint-1];
            } else if(!strcmp(fake.path,"/controls/steam/enabled")) fake.enabled=cJSON_IsTrue(cJSON_GetObjectItem(j,"enabled"));
            else if(!strcmp(fake.path,"/controls/quick-weight")) { fake.dose1=cJSON_GetObjectItem(j,"grams")->valuedouble; fake.mode=1; }
            else if(!strcmp(fake.path,"/controls/bbw/mode")) {
                for(unsigned i=0;i<4;++i) if(!strcmp(cJSON_GetObjectItem(j,"mode")->valuestring,bbw_modes[i])) fake.mode=i;
                if(fake.corrupt_preserved) fake.dose1=99;
            } else if(!strcmp(fake.path,"/controls/bbw/dose")) {
                bool first=!strcmp(cJSON_GetObjectItem(j,"dose")->valuestring,"dose1");
                if(first) fake.dose1=cJSON_GetObjectItem(j,"grams")->valuedouble;
                else fake.dose2=cJSON_GetObjectItem(j,"grams")->valuedouble;
                if(fake.corrupt_preserved) { if(first) fake.dose2=99; else fake.dose1=99; }
            } else if(!strcmp(fake.path,"/controls/prebrew")) {
                const char *field=cJSON_GetObjectItem(j,"field")->valuestring;
                cJSON *value=cJSON_GetObjectItem(j,"value");
                if(!strcmp(field,"mode")) snprintf(fake.prebrew_mode,sizeof(fake.prebrew_mode),"%s",value->valuestring);
                else if(!strcmp(field,"onTime")) fake.prebrew_on=value->valuedouble;
                else { assert(!strcmp(field,"offTime")); fake.prebrew_off=value->valuedouble; }
                if(fake.corrupt_preserved) { if(!strcmp(field,"offTime")) fake.prebrew_on=9.9; else fake.prebrew_off=9.9; }
            } else if(!strcmp(fake.path,"/controls/power")) {
                snprintf(fake.machine_mode,sizeof(fake.machine_mode),"%s",cJSON_IsTrue(cJSON_GetObjectItem(j,"enabled"))?"BrewingMode":"StandBy");
            } else if(!strcmp(fake.path,"/controls/smart-standby")) {
                fake.standby_enabled=cJSON_IsTrue(cJSON_GetObjectItem(j,"enabled"));
                fake.standby_minutes=cJSON_GetObjectItem(j,"minutes")->valueint;
                snprintf(fake.standby_mode,sizeof(fake.standby_mode),"%s",cJSON_GetObjectItem(j,"mode")->valuestring);
                if(fake.corrupt_preserved) fake.standby_minutes=239;
            }
            else assert(false);
            cJSON_Delete(j);
        }
        snprintf(reply,size,"{\"apiVersion\":2,\"requestId\":\"%s\",\"status\":\"%s\",\"pending\":%s,"
            "\"ok\":true,\"writeAccepted\":true,\"stateConfirmed\":false}",ID,
            pending?"running":fake.partial?"partialApply":fake.unknown?"unknownOutcome":"completed",pending?"true":"false");
        return pending?202:200;
    }
    return 200;
}
static char reply[32768];
static const gbs_hub_io_t io={.exchange=exchange,.now=clock_ms,.wait=pause_ms,.reply=reply,.reply_size=sizeof(reply)};
static gbs_ui_machine_t initial(void)
{
    memset(&fake,0,sizeof(fake)); fake.now=1000; fake.coffee=93; fake.steam=128; fake.enabled=true;
    fake.mode=1; fake.dose1=30; fake.dose2=40;
    fake.prebrew_on=2.5; fake.prebrew_off=3.5; strcpy(fake.prebrew_mode,"PreBrewing");
    strcpy(fake.machine_mode,"BrewingMode"); strcpy(fake.standby_mode,"PowerOn"); fake.standby_enabled=true; fake.standby_minutes=30;
    gbs_ui_machine_t machine={0}; assert(gbs_hub_refresh(&config,&io,10000,&machine));
    assert(machine.coffee==91.5&&machine.coffee_target==93&&machine.steam==127&&machine.weight==30);
    assert(machine.writable==GBS_UI_MACHINE_ACTIONS); return machine;
}
static gbs_ui_control_t run(gbs_ui_machine_t *m,gbs_ui_action_t action,double value)
{
    return gbs_hub_execute(&config,&io,action,value,ID,fake.now+5000,m);
}
static void ble_observations(void)
{
    gbs_ui_machine_t m=initial();
    assert(m.ble.valid&&m.ble.configured&&!m.ble.connected&&!m.ble.control_ready&&m.ble.cached_fresh);
    assert(!strcmp(m.ble.status,"backoff")&&!gbs_ui_ble_is_stale(&m.ble,fake.now));
    assert(fake.ble_reads==1&&fake.posts==0);
    fake.ble_reply="{\"apiVersion\":2,\"hubId\":\"22222222-2222-4222-8222-222222222222\","
        "\"configured\":true,\"workerConnected\":true,\"controlSessionReady\":true,\"cachedConnected\":false,\"workerStatus\":\"connected\"}";
    assert(gbs_hub_refresh(&config,&io,10000,&m));
    assert(m.ble.valid&&m.ble.connected&&m.ble.control_ready&&!m.ble.cached_fresh);
    assert(gbs_ui_ble_is_stale(&m.ble,m.ble.observed_ms+GBS_UI_BLE_FRESH_MS));
    fake.ble_status=404;
    assert(gbs_hub_refresh(&config,&io,10000,&m)&&!m.ble.valid&&!m.stale&&m.writable);
    fake.ble_status=0; fake.ble_reply="{\"apiVersion\":2,\"hubId\":\"22222222-2222-4222-8222-222222222222\","
        "\"configured\":true,\"workerConnected\":\"true\",\"controlSessionReady\":true,\"workerStatus\":\"connected\"}";
    assert(gbs_hub_refresh(&config,&io,10000,&m)&&!m.ble.valid);
    fake.ble_reply="{\"apiVersion\":2,\"hubId\":\"wrong-hub\",\"configured\":true,"
        "\"workerConnected\":true,\"controlSessionReady\":true,\"workerStatus\":\"connected\"}";
    assert(gbs_hub_refresh(&config,&io,10000,&m)&&!m.ble.valid);
    fake.ble_reply=NULL; assert(gbs_hub_refresh(&config,&io,10000,&m)&&m.ble.valid);
    fake.binding_changed=true;
    assert(!gbs_hub_refresh(&config,&io,10000,&m)&&!m.ble.valid);
    assert(fake.posts==0);
}
static void scale_observations(void)
{
    gbs_ui_machine_t m=initial();
    assert(!m.scale_connected_known&&!m.scale_battery_known&&m.scale_stale&&m.scale_observed_ms==0);
    const struct { const char *patch; bool known,connected,battery,stale; unsigned percent; int64_t observed; } cases[]={
        {"{\"thingScaleConnected\":true,\"scaleConnected\":false,\"scaleBatteryLevel\":1,\"scaleObservedMs\":900}",true,false,true,false,1,900},
        {"{\"thingScaleConnected\":false,\"scaleConnected\":true,\"scaleBatteryLevel\":0,\"scaleObservedMs\":900}",true,true,true,false,0,900},
        {"{\"thingScaleConnected\":true,\"scaleConnected\":null,\"scaleBatteryLevel\":42,\"scaleObservedMs\":900}",true,true,true,false,42,900},
        {"{\"thingScaleConnected\":true,\"scaleConnected\":\"false\",\"scaleObservedMs\":900}",true,true,false,false,0,900},
        {"{\"scaleConnected\":true,\"scaleBatteryLevel\":100,\"scaleObservedMs\":900}",true,true,true,false,100,900},
        {"{\"thingScaleConnected\":\"true\",\"scaleConnected\":false,\"scaleObservedMs\":900}",true,false,false,false,0,900},
        {"{\"scaleConnected\":1,\"scaleObservedMs\":900}",false,false,false,false,0,900},
        {"{\"thingScaleConnected\":true,\"scaleBatteryLevel\":100.1,\"scaleObservedMs\":900}",true,true,false,false,0,900},
        {"{\"thingScaleConnected\":true,\"scaleBatteryLevel\":-0.1,\"scaleObservedMs\":900}",true,true,false,false,0,900},
        {"{\"thingScaleConnected\":true,\"scaleBatteryLevel\":\"78\",\"scaleObservedMs\":900}",true,true,false,false,0,900},
        {"{\"thingScaleConnected\":true,\"scaleBatteryLevel\":78,\"scaleObservedMs\":999999}",true,true,true,true,78,0},
        {"{\"thingScaleConnected\":true,\"scaleBatteryLevel\":78,\"scaleObservedMs\":900.5}",true,true,true,true,78,0},
        {"{\"thingScaleConnected\":true,\"scaleBatteryLevel\":78}",true,true,true,true,78,0}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        m=initial(); fake.state_patch=cases[i].patch;
        assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m));
        assert(m.scale_connected_known==cases[i].known&&m.scale_connected==cases[i].connected);
        assert(m.scale_battery_known==cases[i].battery&&m.scale_battery_percent==cases[i].percent);
        assert(m.scale_stale==cases[i].stale&&m.scale_observed_ms==cases[i].observed);
    }
    m=initial(); fake.state_patch=cases[0].patch;
    assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m)&&!m.scale_stale);
    fake.now=900+GBS_UI_SCALE_FRESH_MS;
    assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m));
    assert(m.scale_stale&&m.scale_observed_ms==900&&m.scale_battery_percent==1);
    fake.state_patch="{\"thingScaleConnected\":true,\"scaleObservedMs\":61000}";
    assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m)&&!m.scale_stale);
    fake.auth_fail=true;
    assert(!gbs_hub_refresh(&config,&io,fake.now+10000,&m)&&m.scale_stale&&m.scale_observed_ms==61000);
    fake.auth_fail=false; fake.state_patch="{}";
    assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m));
    assert(!m.scale_connected_known&&!m.scale_battery_known&&m.scale_stale&&m.scale_observed_ms==0);
    char path[96],body[256]; m=initial();
    assert(!gbs_hub_prepare(GBS_ACTION_ROTATION_AUTO,1,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_SOUND,1,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_WEIGHT_PRESET,42,&m,ID,path,sizeof(path),body,sizeof(body)));
}
static void boiler_ready_times(void)
{
    gbs_ui_machine_t m=initial();
    assert(!m.coffee_ready_unix&&!m.steam_ready_unix&&!m.boiler_observed_unix);
    fake.state_patch="{\"coffeeBoilerReadyTime\":\"2026-10-23T00:43:14Z\","
        "\"steamBoilerReadyTime\":\"2026-10-23T00:46:34Z\",\"lastUpdated\":\"2026-10-23T00:40:00Z\"}";
    assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m));
    assert(m.coffee_ready_unix==1792716194&&m.steam_ready_unix==1792716394&&m.boiler_observed_unix==1792716000);
    assert(gbs_ui_boiler_warmup_minutes(&m,false,1792716000)==4);
    assert(gbs_ui_boiler_warmup_minutes(&m,true,1792716000)==7);
    assert(!fake.posts&&m.writable==GBS_UI_MACHINE_ACTIONS);
    const struct { const char *stamp; int64_t epoch; } cases[]={
        {"1970-01-01T00:00:01Z",1}, {"2000-02-29T23:59:59Z",951868799},
        {"2000-03-01T00:00:00Z",951868800}, {"2100-03-01T00:00:00Z",4107542400LL},
        {"2024-02-29T12:34:56Z",1709210096}, {"2025-02-29T00:00:00Z",0},
        {"2100-02-29T00:00:00Z",0}, {"2026-04-31T00:00:00Z",0},
        {"2026-10-23T24:00:00Z",0}, {"2026-10-23T00:00:00+08:00",0},
        {"2026-10-23T00:00:00Zjunk",0}, {"2026-10-23T00:00:00z",0}, {"",0}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        char patch[128]; snprintf(patch,sizeof(patch),"{\"coffeeBoilerReadyTime\":\"%s\"}",cases[i].stamp);
        fake.state_patch=patch; assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m));
        assert(m.coffee_ready_unix==cases[i].epoch&&!m.steam_ready_unix&&!m.boiler_observed_unix);
    }
    fake.state_patch="{\"coffeeBoilerReadyTime\":12,\"steamBoilerReadyTime\":false}";
    assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m)&&!m.coffee_ready_unix&&!m.steam_ready_unix);
}
static void scale_does_not_gate_weight(void)
{
    const struct { const char *patch; int64_t now; bool known,connected,stale; } states[]={
        {"{\"scaleConnected\":false,\"scaleObservedMs\":900}",2000,true,false,false},
        {"{}",2000,false,false,true},
        {"{\"scaleConnected\":true,\"scaleObservedMs\":900}",62000,true,true,true}
    };
    const struct { double grams; const char *route; } targets[]={
        {40,"/controls/bbw/mode"}, {35,"/controls/quick-weight"}
    };
    for(unsigned i=0;i<sizeof(states)/sizeof(states[0]);++i) {
        for(unsigned j=0;j<sizeof(targets)/sizeof(targets[0]);++j) {
            gbs_ui_machine_t m=initial(); fake.state_patch=states[i].patch; fake.now=states[i].now;
            assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m));
            assert(m.scale_connected_known==states[i].known&&m.scale_connected==states[i].connected);
            assert(m.scale_stale==states[i].stale);
            // Scale telemetry does not gate a Cloud setting or imply physical confirmation.
            assert(run(&m,GBS_ACTION_WEIGHT,targets[j].grams)==GBS_UI_CONTROL_CONFIRMED);
            assert(fake.posts==1&&!strcmp(fake.path,targets[j].route)&&m.weight==targets[j].grams);
            assert(m.scale_connected_known==states[i].known&&m.scale_connected==states[i].connected);
            assert(m.scale_stale==states[i].stale);
        }
    }
}
static void daily_controls(void)
{
    const struct { gbs_ui_action_t action; double value; const char *route,*capability; } cases[]={
        {GBS_ACTION_DOSE1,26,"/controls/bbw/dose","brewByWeightDose1"},
        {GBS_ACTION_DOSE2,42,"/controls/bbw/dose","brewByWeightDose2"},
        {GBS_ACTION_BBW_MODE,0,"/controls/bbw/mode","brewByWeightMode"},
        {GBS_ACTION_PREBREW_MODE,2,"/controls/prebrew","prebrew"},
        {GBS_ACTION_PREBREW_ON,4.1,"/controls/prebrew","prebrew"},
        {GBS_ACTION_PREBREW_OFF,1.2,"/controls/prebrew","prebrew"},
        {GBS_ACTION_POWER,0,"/controls/power","power"},
        {GBS_ACTION_STANDBY_ENABLED,0,"/controls/smart-standby","smartStandby"},
        {GBS_ACTION_STANDBY_MINUTES,60,"/controls/smart-standby","smartStandby"},
        {GBS_ACTION_STANDBY_MODE,1,"/controls/smart-standby","smartStandby"}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        gbs_ui_action_t a=cases[i].action; double v=cases[i].value;
        gbs_ui_machine_t m=initial();
        assert(run(&m,a,v)==GBS_UI_CONTROL_CONFIRMED&&fake.posts==1&&!strcmp(fake.path,cases[i].route));
        assert(fabs(gbs_ui_action_value(a,&m)-v)<0.001);
        assert(m.coffee_target==93&&m.steam_enabled);
        m=initial(); fake.lost_reply=true; assert(run(&m,a,v)==GBS_UI_CONTROL_CONFIRMED&&fake.posts==1);
        m=initial(); fake.readback_mismatch=true; assert(run(&m,a,v)==GBS_UI_CONTROL_UNKNOWN&&fake.posts==1);
        m=initial(); fake.partial=true; assert(run(&m,a,v)==GBS_UI_CONTROL_PARTIAL&&fake.posts==1);
        m=initial(); fake.post_unknown=true; assert(run(&m,a,v)==GBS_UI_CONTROL_UNKNOWN&&fake.posts==1&&fake.polls==0);
        m=initial(); fake.unavailable=cases[i].capability; assert(run(&m,a,v)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
        m=initial(); fake.auth_fail=true; assert(run(&m,a,v)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
        m=initial();
        if(a<=GBS_ACTION_BBW_MODE) fake.dose2=44;
        else if(a<=GBS_ACTION_PREBREW_OFF) fake.prebrew_off=8;
        else if(a==GBS_ACTION_POWER) strcpy(fake.machine_mode,"EcoMode");
        else fake.standby_minutes=45;
        assert(run(&m,a,v)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
        if(a!=GBS_ACTION_POWER) {
            m=initial(); fake.corrupt_preserved=true;
            assert(run(&m,a,v)==GBS_UI_CONTROL_UNKNOWN&&fake.posts==1);
        }
    }
    for(unsigned mode=0;mode<4;++mode) {
        gbs_ui_machine_t m=initial(); assert(run(&m,GBS_ACTION_BBW_MODE,mode)==GBS_UI_CONTROL_CONFIRMED);
        assert(m.dose_mode==mode&&m.dose1==30&&m.dose2==40);
        assert(mode==1?m.weight==30:mode==2?m.weight==40:isnan(m.weight));
    }
    gbs_ui_machine_t m=initial(); strcpy(fake.machine_mode,"StandBy");
    assert(gbs_hub_refresh(&config,&io,10000,&m)); assert(run(&m,GBS_ACTION_POWER,1)==GBS_UI_CONTROL_CONFIRMED&&m.machine_mode==1);
    const char *patches[]={"{\"prebrew\":null}","{\"prebrew\":{\"mode\":\"FutureMode\",\"onTime\":2,\"offTime\":3}}",
        "{\"smartStandby\":{\"enabled\":true,\"minutes\":30.5,\"mode\":\"PowerOn\"}}",
        "{\"machineMode\":\"Unknown\"}","{\"brewByWeightMode\":null}","{\"brewByWeightDose2\":null}"};
    const gbs_ui_action_t actions[]={GBS_ACTION_PREBREW_ON,GBS_ACTION_PREBREW_MODE,GBS_ACTION_STANDBY_ENABLED,GBS_ACTION_POWER,GBS_ACTION_BBW_MODE,GBS_ACTION_DOSE1};
    for(unsigned i=0;i<sizeof(actions)/sizeof(actions[0]);++i) {
        m=initial(); fake.state_patch=patches[i]; assert(gbs_hub_refresh(&config,&io,10000,&m));
        assert(!(m.writable&(1u<<actions[i])));
        assert(run(&m,actions[i],1)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
    }
    m=initial(); fake.cloud_offline=true; assert(gbs_hub_refresh(&config,&io,10000,&m));
    assert(!(m.writable&(1u<<GBS_ACTION_DOSE1))&&!(m.writable&(1u<<GBS_ACTION_PREBREW_MODE)));
    assert(m.writable&(1u<<GBS_ACTION_POWER));
    m=initial(); fake.state_patch="{\"prebrew\":{\"mode\":\"PreBrewing\",\"onTime\":0.15,\"offTime\":3.5}}";
    assert(gbs_hub_refresh(&config,&io,10000,&m)&&isnan(m.prebrew_on));
    assert(!(m.writable&(1u<<GBS_ACTION_PREBREW_ON))&&!(m.writable&(1u<<GBS_ACTION_PREBREW_OFF)));
    char path[96],body[256]; m=initial();
    const double invalid[]={-1,201,0.5,NAN,INFINITY};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        assert(!gbs_hub_prepare(GBS_ACTION_DOSE2,invalid[i],&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_BBW_MODE,4,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_PREBREW_ON,0.15,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_PREBREW_OFF,10.1,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_PREBREW_MODE,3,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_POWER,2,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_STANDBY_MINUTES,241,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_STANDBY_MINUTES,0,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_STANDBY_MODE,2,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(gbs_hub_prepare(GBS_ACTION_PREBREW_ON,10,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(gbs_hub_prepare(GBS_ACTION_PREBREW_OFF,0,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(gbs_hub_prepare(GBS_ACTION_STANDBY_MINUTES,240,&m,ID,path,sizeof(path),body,sizeof(body)));
    m.stale=true; assert(!gbs_hub_prepare(GBS_ACTION_POWER,0,&m,ID,path,sizeof(path),body,sizeof(body)));
}
static void configuration(void)
{
    const char *pem="-----BEGIN CERTIFICATE-----\nsynthetic\n-----END CERTIFICATE-----\n";
    cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"url","https://hub.example.test:443");
    cJSON_AddStringToObject(j,"hub_id",config.hub_id); cJSON_AddStringToObject(j,"serial",config.serial);
    cJSON_AddStringToObject(j,"token","synthetic-hub-token"); cJSON_AddStringToObject(j,"certificate",pem);
    char *encoded=cJSON_PrintUnformatted(j); gbs_hub_config_t parsed={0};
    assert(gbs_hub_config_parse(encoded,&parsed)&&!strcmp(parsed.hub_id,config.hub_id)); cJSON_free(encoded);
    const char *invalid[]={"http://hub.example.test","https://a@hub.example.test","https://hub.example.test/path",
        "https://hub.example.test:65536","https://hub.example.test?token=x","https://-host","https://hub.example.test\r\n"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        cJSON_ReplaceItemInObject(j,"url",cJSON_CreateString(invalid[i])); encoded=cJSON_PrintUnformatted(j);
        assert(!gbs_hub_config_parse(encoded,&parsed)); cJSON_free(encoded);
    }
    cJSON_ReplaceItemInObject(j,"url",cJSON_CreateString("https://hub.example.test"));
    cJSON_ReplaceItemInObject(j,"token",cJSON_CreateString("synthetic-token\r\nInjected:value"));
    encoded=cJSON_PrintUnformatted(j); assert(!gbs_hub_config_parse(encoded,&parsed)); cJSON_free(encoded);
    cJSON_ReplaceItemInObject(j,"token",cJSON_CreateString("synthetic-hub-token"));
    cJSON_ReplaceItemInObject(j,"certificate",cJSON_CreateString("-----BEGIN CERTIFICATE-----\nx\n-----END CERTIFICATE-----\nPRIVATE"));
    encoded=cJSON_PrintUnformatted(j); assert(!gbs_hub_config_parse(encoded,&parsed)); cJSON_free(encoded);
    cJSON_Delete(j);
    assert(!gbs_hub_config_parse(NULL,&parsed)&&!gbs_hub_config_parse("{}junk",&parsed));
}
static void offline_weight_selection(void)
{
    gbs_ui_machine_t m=initial();
    const char *patches[]={
        "{\"connectionState\":\"unknown\",\"brewByWeightMode\":\"Dose2\",\"brewByWeightDose1\":20,\"brewByWeightDose2\":30}",
        "{\"connectionState\":\"disconnected\",\"brewByWeightMode\":\"Dose1\",\"brewByWeightDose1\":42}",
        "{\"connectionState\":\"unknown\",\"brewByWeightMode\":\"Continuous\"}",
        "{\"connectionState\":\"unknown\",\"brewByWeightMode\":null}",
        "{\"connectionState\":\"unknown\",\"brewByWeightMode\":\"Dose2\",\"brewByWeightDose2\":null}"
    };
    for(unsigned i=0;i<sizeof(patches)/sizeof(*patches);++i) {
        fake.state_patch=patches[i];
        assert(gbs_hub_refresh(&config,&io,fake.now+10000,&m)&&m.available&&m.stale);
        assert(i==0?m.weight==30:i==1?m.weight==42:isnan(m.weight));
        char path[96],body[256];
        assert(gbs_hub_prepare(GBS_ACTION_WEIGHT,36,&m,ID,path,sizeof(path),body,sizeof(body))==(i<3));
        assert(!gbs_hub_prepare(GBS_ACTION_COFFEE,94,&m,ID,path,sizeof(path),body,sizeof(body)));
    }
    assert(fake.posts==0);
    m=initial();fake.state_patch="{\"connectionState\":\"unknown\"}";
    assert(run(&m,GBS_ACTION_WEIGHT,40)==GBS_UI_CONTROL_CONFIRMED&&m.stale&&m.weight==40);
    m=initial();fake.state_patch="{\"connectionState\":\"unknown\"}";
    assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_CONFIRMED&&m.stale&&m.weight==35&&m.dose2==40);
    m=initial();fake.state_patch="{\"connectionState\":\"unknown\"}";fake.cloud_offline=true;
    assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
    puts("Hub offline selection: retained Cloud dose, manual preset confirmation, missing/failed Cloud guards and other controls unchanged PASS");
}
int main(void)
{
    configuration();
    offline_weight_selection();
    ble_observations();
    scale_observations();
    boiler_ready_times();
    scale_does_not_gate_weight();
    daily_controls();
    gbs_ui_machine_t m=initial();
    assert(run(&m,GBS_ACTION_WEIGHT,40)==GBS_UI_CONTROL_CONFIRMED);
    assert(!strcmp(fake.path,"/controls/bbw/mode")&&m.dose_mode==2&&m.weight==40&&m.dose1==30&&m.dose2==40);
    m=initial(); assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_CONFIRMED);
    assert(!strcmp(fake.path,"/controls/quick-weight")&&m.weight==35&&m.dose2==40);
    m=initial(); fake.lost_reply=true; assert(run(&m,GBS_ACTION_COFFEE,94.5)==GBS_UI_CONTROL_CONFIRMED);
    assert(fake.posts==1&&m.coffee==91.5&&m.coffee_target==94.5);
    m=initial(); assert(run(&m,GBS_ACTION_STEAM,131)==GBS_UI_CONTROL_CONFIRMED&&m.steam_target==131&&m.steam==127);
    m=initial(); assert(run(&m,GBS_ACTION_STEAM_ENABLED,0)==GBS_UI_CONTROL_CONFIRMED&&!m.steam_enabled);
    m=initial(); fake.never_ack=true; assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_UNKNOWN&&fake.posts==1);
    m=initial(); fake.readback_mismatch=true; assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_UNKNOWN&&m.weight==30);
    m=initial(); fake.partial=true; assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_PARTIAL);
    m=initial(); fake.unknown=true; assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_UNKNOWN);
    m=initial(); fake.post_unknown=true; assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_UNKNOWN&&fake.polls==0&&fake.posts==1);
    m=initial(); fake.coffee=94; assert(run(&m,GBS_ACTION_COFFEE,95)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
    m=initial(); fake.dose2=35; assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
    m=initial(); fake.auth_fail=true; assert(run(&m,GBS_ACTION_COFFEE,94)==GBS_UI_CONTROL_FAILED&&fake.posts==0&&m.stale);
    m=initial(); fake.wrong_hub=true; assert(!gbs_hub_refresh(&config,&io,10000,&m)&&m.stale&&!m.writable);
    m=initial(); fake.wrong_machine=true; assert(!gbs_hub_refresh(&config,&io,10000,&m)&&m.stale);
    m=initial(); fake.binding_changed=true; assert(!gbs_hub_refresh(&config,&io,10000,&m)&&m.stale);
    m=initial(); fake.missing_measurement=true; assert(gbs_hub_refresh(&config,&io,10000,&m));
    assert(isnan(m.coffee)&&isnan(m.steam)&&m.coffee_target==93&&m.steam_target==128);
    m=initial(); fake.cloud_offline=true; assert(gbs_hub_refresh(&config,&io,10000,&m));
    assert(!(m.writable&(1u<<GBS_ACTION_WEIGHT))&&(m.writable&(1u<<GBS_ACTION_COFFEE)));
    assert(run(&m,GBS_ACTION_COFFEE,94)==GBS_UI_CONTROL_CONFIRMED);
    m=initial(); fake.legacy_presets=true; assert(gbs_hub_refresh(&config,&io,10000,&m));
    assert(run(&m,GBS_ACTION_WEIGHT,35)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
    m=initial(); assert(gbs_hub_execute(&config,&io,GBS_ACTION_WEIGHT,35,ID,fake.now,&m)==GBS_UI_CONTROL_FAILED&&fake.posts==0);
    m=initial(); char path[96],body[256];
    assert(!gbs_hub_prepare(GBS_ACTION_COFFEE,94.2,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_STEAM,129,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_WEIGHT,NAN,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare((gbs_ui_action_t)-1,35,&m,ID,path,sizeof(path),body,sizeof(body)));
    assert(!gbs_hub_prepare(GBS_ACTION_WEIGHT,35,&m,"bad\r\nid",path,sizeof(path),body,sizeof(body)));
    assert(gbs_hub_result("{\"ok\":true}",ID)==GBS_UI_CONTROL_UNKNOWN);
    assert(gbs_hub_result("broken",ID)==GBS_UI_CONTROL_UNKNOWN);
    assert(gbs_hub_result("{\"apiVersion\":2,\"requestId\":\"another\",\"status\":\"completed\",\"ok\":true}",ID)==GBS_UI_CONTROL_UNKNOWN);
    puts("Hub contract: telemetry, presets, BBW, prebrew, power, standby, capabilities, preserved fields, readback and no-replay commands passed");
    return 0;
}
