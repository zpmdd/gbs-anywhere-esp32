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
    double coffee,steam,dose1,dose2;
    bool enabled,lost_reply,never_ack,partial,unknown,readback_mismatch,auth_fail,wrong_hub,wrong_machine;
    bool binding_changed,missing_measurement,cloud_offline,legacy_presets,post_unknown;
    char path[96],body[256];
} fake;
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
            assert(cJSON_GetObjectItem(j,"clientTtlMs")->valueint==8000);
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
            "\"coffeeTemperature\":{\"available\":true},\"steamLevel\":{\"available\":true},\"steamEnabled\":{\"available\":true}}",
            fake.wrong_hub?ID:config.hub_id,fake.legacy_presets?"false":"true");
    } else if(!strcmp(path,"/machine/state")) {
        snprintf(reply,size,"{\"apiVersion\":2,\"hubId\":\"%s\",\"connectionState\":\"ready\","
            "\"coffeeBoilerCurrentTemperature\":%s,\"coffeeBoilerTargetTemperature\":%.1f,"
            "\"steamBoilerCurrentTemperature\":%s,\"steamBoilerTargetTemperature\":%.0f,\"steamBoilerEnabled\":%s,"
            "\"brewByWeightDose1\":%.0f,\"brewByWeightDose2\":%.0f,\"brewByWeightMode\":\"Dose%u\","
            "\"activeDose\":\"dose1\",\"cloudCommandReadiness\":\"%s\"}",config.hub_id,
            fake.missing_measurement?"null":"91.5",fake.coffee,fake.missing_measurement?"null":"127",fake.steam,
            fake.enabled?"true":"false",fake.dose1,fake.dose2,fake.mode,fake.cloud_offline?"degraded":"ready");
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
            else if(!strcmp(fake.path,"/controls/bbw/mode")) fake.mode=!strcmp(cJSON_GetObjectItem(j,"mode")->valuestring,"Dose2")?2:1;
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
    gbs_ui_machine_t machine={0}; assert(gbs_hub_refresh(&config,&io,10000,&machine));
    assert(machine.coffee==91.5&&machine.coffee_target==93&&machine.steam==127&&machine.weight==30);
    assert(machine.writable==GBS_UI_MACHINE_ACTIONS); return machine;
}
static gbs_ui_control_t run(gbs_ui_machine_t *m,gbs_ui_action_t action,double value)
{
    return gbs_hub_execute(&config,&io,action,value,ID,fake.now+5000,m);
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
int main(void)
{
    configuration();
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
    puts("Hub contract: real fields, smart dose routing, binding, capabilities, readback, lost responses and bounded no-replay commands passed");
    return 0;
}
