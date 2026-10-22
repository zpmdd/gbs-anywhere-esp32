#include "gbs_hub.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const gbs_hub_config_t config={.hub_id="22222222-2222-4222-8222-222222222222",.serial="SYNTHETIC-MACHINE"};
static char schedule[32768],history[32768],reply[32768];
static const char *request_id="33333333-3333-4333-8333-333333333333";
static struct {
    int64_t now;
    unsigned posts,bindings,reads;
    int status;
    bool stale,response_stale,wrong_hub,wrong_machine,changed,unavailable,rejected,auth;
    bool readonly,lost,unconfirmed,rebind_after,command_failed;
    unsigned writes,polls;
    int write_status;
    const char *payload;
} fake;
static int64_t clock_ms(void *unused) { (void)unused; return fake.now; }
static void pause_ms(void *unused,unsigned ms) { (void)unused; fake.now+=ms; }
static int exchange(void *unused,const char *path,const char *body,char *out,size_t size,int64_t deadline)
{
    (void)unused; assert(fake.now<deadline); fake.now+=20; fake.response_stale=false;
    if(fake.auth) return 401;
    if(!strcmp(path,"/controls/schedule")) {
        assert(body); ++fake.writes; assert(fake.writes==1);
        cJSON *j=cJSON_Parse(body); assert(j);
        assert(!strcmp(cJSON_GetObjectItem(j,"requestId")->valuestring,request_id));
        assert(cJSON_IsTrue(cJSON_GetObjectItem(j,"confirm")));
        const cJSON *expected=cJSON_GetObjectItem(j,"expected"),*next=cJSON_GetObjectItem(j,"schedule");
        assert(!strcmp(cJSON_GetObjectItem(expected,"id")->valuestring,"weekday"));
        assert(cJSON_GetObjectItem(expected,"onTimeMinutes")->valuedouble==420);
        assert(cJSON_GetObjectItem(next,"onTimeMinutes")->valuedouble==480);
        cJSON_Delete(j);
        snprintf(out,size,"{\"apiVersion\":2,\"requestId\":\"%s\",\"status\":\"accepted\"}",request_id);
        return fake.lost?-1:fake.write_status;
    }
    if(!strncmp(path,"/diagnostics/commands?id=",24)) {
        ++fake.polls;
        snprintf(out,size,"{\"apiVersion\":2,\"requestId\":\"%s\",\"status\":\"completed\",\"control\":\"schedule\",\"ok\":%s,\"stateConfirmed\":%s}",
            request_id,fake.command_failed?"false":"true",fake.unconfirmed?"false":"true");
        return 200;
    }
    if(body) {
        assert(!strncmp(path,"/diagnostics/slow-cloud-reads/refresh?kind=",43));
        assert(!strcmp(body,"{}")); ++fake.posts;
        snprintf(out,size,"{\"ok\":true,\"accepted\":%s,\"kind\":\"%s\"}",fake.rejected?"false":"true",strstr(path,"statistics")?"statistics":"schedule");
    } else if(!strcmp(path,"/machine/binding")) {
        ++fake.bindings;
        snprintf(out,size,"{\"selected\":true,\"serialNumber\":\"%s\",\"generation\":%u}",fake.wrong_machine?"OTHER":config.serial,
            fake.changed?fake.bindings:fake.rebind_after&&fake.writes?2:1);
    } else if(!strcmp(path,"/machine/capabilities")) {
        snprintf(out,size,"{\"apiVersion\":2,\"hubId\":\"%s\",\"schedule\":{\"available\":%s},\"scheduleEdit\":{\"available\":%s},\"statistics\":{\"available\":%s}}",
            fake.wrong_hub?"another-hub":config.hub_id,fake.unavailable?"false":"true",fake.readonly?"false":"true",fake.unavailable?"false":"true");
    } else {
        assert(!strcmp(path,"/machine/schedule")||!strcmp(path,"/machine/statistics?days=7"));
        ++fake.reads; fake.response_stale=fake.stale;
        snprintf(out,size,"%s",fake.payload?fake.payload:strstr(path,"statistics")?history:schedule);
        return fake.status;
    }
    return 200;
}
static const gbs_hub_io_t io={.exchange=exchange,.now=clock_ms,.wait=pause_ms,.reply=reply,.reply_size=sizeof(reply),.response_stale=&fake.response_stale};
static gbs_ui_data_t fetch(gbs_ui_data_kind_t kind,bool refresh)
{
    gbs_ui_data_t data; gbs_hub_fetch_data(&config,&io,kind,refresh,fake.now+1000,&data); return data;
}
static void reset(void) { memset(&fake,0,sizeof(fake)); fake.status=200; fake.write_status=202; }
static void load(const char *dir,const char *name,char *out,size_t size)
{
    char path[1024]; snprintf(path,sizeof(path),"%s/%s",dir,name);
    FILE *file=fopen(path,"rb"); assert(file);
    size_t n=fread(out,1,size-1,file); assert(n&&n<size-1&&!ferror(file)); out[n]=0; fclose(file);
}
static void invalid_schedule(const char *patch)
{
    reset(); cJSON *j=cJSON_Parse(schedule),*p=cJSON_Parse(patch),*v=NULL; assert(j&&p);
    cJSON *rows=cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(j,"raw"),"smartWakeUpSleep"),"schedules");
    cJSON_ArrayForEach(v,p) cJSON_ReplaceItemInObject(cJSON_GetArrayItem(rows,0),v->string,cJSON_Duplicate(v,true));
    char *json=cJSON_PrintUnformatted(j); fake.payload=json;
    gbs_ui_data_t data=fetch(GBS_DATA_SCHEDULE,false); assert(data.state==GBS_DATA_ERROR&&!data.valid&&!data.count);
    free(json); cJSON_Delete(j); cJSON_Delete(p);
}
static void edit_schedule(void)
{
    reset(); gbs_ui_data_t data=fetch(GBS_DATA_SCHEDULE,false);
    gbs_ui_schedule_t expected=data.items.schedules[0],next=expected; next.on_minutes=480;
    for(unsigned scenario=0;scenario<8;++scenario) {
        reset(); fake.lost=scenario==1; fake.unconfirmed=scenario==2; fake.rebind_after=scenario==3;
        fake.command_failed=scenario==4; fake.readonly=scenario==5; fake.stale=scenario==6;
        fake.write_status=scenario==7?409:202;
        gbs_ui_control_t result=gbs_hub_execute_schedule(&config,&io,&expected,&next,request_id,2000,&data);
        assert(result==(scenario<2?GBS_UI_CONTROL_CONFIRMED:scenario<4?GBS_UI_CONTROL_UNKNOWN:GBS_UI_CONTROL_FAILED));
        assert(fake.writes==(scenario==5||scenario==6?0:1));
        if(scenario<2) assert(data.state==GBS_DATA_STALE&&data.items.schedules[0].on_minutes==480);
    }
    reset(); ++expected.off_minutes;
    assert(gbs_hub_execute_schedule(&config,&io,&expected,&next,request_id,2000,&data)==GBS_UI_CONTROL_FAILED&&!fake.writes);
    --expected.off_minutes; next.on_minutes=1440;
    assert(gbs_hub_execute_schedule(&config,&io,&expected,&next,request_id,2000,&data)==GBS_UI_CONTROL_FAILED&&!fake.writes);
    next=expected; next.days=0;
    assert(gbs_hub_execute_schedule(&config,&io,&expected,&next,request_id,2000,&data)==GBS_UI_CONTROL_FAILED&&!fake.writes);
    next=expected; strcpy(next.id,"other");
    assert(gbs_hub_execute_schedule(&config,&io,&expected,&next,request_id,2000,&data)==GBS_UI_CONTROL_FAILED&&!fake.writes);
}
int main(int argc,char **argv)
{
    assert(argc==2); load(argv[1],"hub-schedule.json",schedule,sizeof(schedule)); load(argv[1],"hub-history.json",history,sizeof(history));
    reset(); gbs_ui_data_t data=fetch(GBS_DATA_SCHEDULE,true);
    assert(data.valid&&data.state==GBS_DATA_READY&&data.count==4&&fake.posts==1&&fake.bindings==2);
    assert(data.items.schedules[0].days==31&&data.items.schedules[0].on_minutes==420&&data.items.schedules[0].steam);
    assert(data.items.schedules[3].on_minutes==1380&&data.items.schedules[3].off_minutes==60);
    reset(); data=fetch(GBS_DATA_HISTORY,true);
    assert(data.valid&&data.count==5&&data.total==5&&fake.posts==1);
    assert(!strcmp(data.items.coffees[0].time,"2026-10-18T01:18:20Z"));
    assert(data.items.coffees[0].seconds==31.2&&data.items.coffees[0].dose==30);
    assert(isnan(data.items.coffees[3].seconds)&&isnan(data.items.coffees[1].dose));
    reset(); fake.stale=true; data=fetch(GBS_DATA_HISTORY,false);
    assert(data.state==GBS_DATA_STALE&&data.valid&&fake.posts==0);
    reset(); fake.status=202; data=fetch(GBS_DATA_HISTORY,true);
    assert(data.state==GBS_DATA_LOADING&&!data.valid&&fake.posts==1);
    fake.status=200; data=fetch(GBS_DATA_HISTORY,false); assert(data.valid&&fake.posts==1);
    reset(); fake.status=503; assert(fetch(GBS_DATA_HISTORY,false).state==GBS_DATA_UNAVAILABLE);
    reset(); fake.auth=true; assert(fetch(GBS_DATA_HISTORY,true).state==GBS_DATA_ERROR&&fake.posts==0);
    reset(); fake.wrong_hub=true; assert(fetch(GBS_DATA_HISTORY,true).state==GBS_DATA_ERROR&&fake.posts==0);
    reset(); fake.wrong_machine=true; assert(fetch(GBS_DATA_HISTORY,true).state==GBS_DATA_ERROR&&fake.posts==0);
    reset(); fake.changed=true; data=fetch(GBS_DATA_HISTORY,true); assert(!data.valid&&!data.count&&data.state==GBS_DATA_ERROR);
    reset(); fake.unavailable=true; assert(fetch(GBS_DATA_HISTORY,true).state==GBS_DATA_UNAVAILABLE&&fake.posts==0);
    reset(); fake.rejected=true; assert(fetch(GBS_DATA_SCHEDULE,true).state==GBS_DATA_UNAVAILABLE&&fake.posts==1&&fake.reads==0);
    reset(); fake.payload="{\"recentCoffees\":[]}"; data=fetch(GBS_DATA_HISTORY,false); assert(data.valid&&!data.count);
    reset(); fake.payload="{}"; assert(fetch(GBS_DATA_HISTORY,false).state==GBS_DATA_ERROR);
    fake.payload="{\"raw\":{\"serialNumber\":\"SYNTHETIC-MACHINE\",\"smartWakeUpSleepSupported\":false}}";
    assert(fetch(GBS_DATA_SCHEDULE,false).state==GBS_DATA_UNSUPPORTED);
    fake.payload="{\"raw\":{\"serialNumber\":\"OTHER\",\"smartWakeUpSleepSupported\":false}}";
    assert(fetch(GBS_DATA_SCHEDULE,false).state==GBS_DATA_ERROR);
    invalid_schedule("{\"onTimeMinutes\":1440}"); invalid_schedule("{\"offTimeMinutes\":0.5}");
    invalid_schedule("{\"steamBoiler\":null}"); invalid_schedule("{\"days\":[\"Monday\",\"Monday\"]}");
    invalid_schedule("{\"days\":[\"Funday\"]}"); invalid_schedule("{\"enabled\":1}");
    reset(); cJSON *j=cJSON_Parse(history),*rows=cJSON_GetObjectItem(j,"recentCoffees");
    cJSON_ReplaceItemInObject(cJSON_GetArrayItem(rows,0),"time",cJSON_CreateString("2026-02-30T00:00:00Z"));
    char *json=cJSON_PrintUnformatted(j); fake.payload=json;
    assert(fetch(GBS_DATA_HISTORY,false).state==GBS_DATA_ERROR); free(json); cJSON_Delete(j);
    reset(); j=cJSON_Parse(history); rows=cJSON_GetObjectItem(j,"recentCoffees");
    for(unsigned i=0;i<40;++i) cJSON_AddItemToArray(rows,cJSON_Duplicate(cJSON_GetArrayItem(rows,0),true));
    json=cJSON_PrintUnformatted(j); fake.payload=json; data=fetch(GBS_DATA_HISTORY,false);
    assert(data.valid&&data.count==32&&data.total==45&&!strcmp(data.items.coffees[0].time,"2026-10-18T01:18:20Z"));
    free(json); cJSON_Delete(j);
    reset(); gbs_hub_fetch_data(&config,&io,GBS_DATA_HISTORY,true,0,&data);
    assert(data.state==GBS_DATA_ERROR&&fake.posts==0&&fake.reads==0);
    edit_schedule();
    puts("Hub collections: binding, refresh, stale/error/empty states, schedule bounds, UTC ordering, nullable metrics and bounded history passed");
}
