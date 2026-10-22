#include "gbs_hub.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cJSON *item(const cJSON *j, const char *key) { return cJSON_GetObjectItemCaseSensitive(j,key); }
static bool yes(const cJSON *j, const char *key) { return cJSON_IsTrue(item(j,key)); }
static bool equal(const cJSON *j, const char *key, const char *value)
{
    const cJSON *v=item(j,key); return cJSON_IsString(v)&&!strcmp(v->valuestring,value);
}
static double number(const cJSON *j,const char *key,double low,double high)
{
    const cJSON *v=item(j,key);
    return cJSON_IsNumber(v)&&isfinite(v->valuedouble)&&v->valuedouble>=low&&v->valuedouble<=high?v->valuedouble:NAN;
}
static bool prebrew_time(double value)
{
    return isfinite(value)&&value>=0&&value<=10&&fabs(value*10-round(value*10))<=0.000001;
}
static bool text(const cJSON *j,const char *key,char *out,size_t size,bool multiline)
{
    const cJSON *v=item(j,key);
    if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=size) return false;
    for(const unsigned char *p=(const unsigned char *)v->valuestring;*p;++p)
        if(!multiline&&(*p<33||*p>126)) return false;
    strcpy(out,v->valuestring); return true;
}
static bool uuid(const char *s)
{
    if(!s||strlen(s)!=36) return false;
    for(unsigned i=0;i<36;++i)
        if((i==8||i==13||i==18||i==23)?s[i]!='-':!isxdigit((unsigned char)s[i])) return false;
    return true;
}
bool gbs_hub_config_parse(const char *json,gbs_hub_config_t *out)
{
    if(!json||!out) return false;
    cJSON *j=cJSON_ParseWithOpts(json,NULL,true); gbs_hub_config_t next={0};
    bool ok=cJSON_IsObject(j)&&text(j,"url",next.url,sizeof(next.url),false)&&
        text(j,"token",next.token,sizeof(next.token),false)&&text(j,"hub_id",next.hub_id,sizeof(next.hub_id),false)&&
        text(j,"serial",next.serial,sizeof(next.serial),false)&&text(j,"certificate",next.certificate,sizeof(next.certificate),true);
    cJSON_Delete(j);
    if(!ok||!uuid(next.hub_id)||strncmp(next.url,"https://",8)||strlen(next.token)<16) return false;
    const char *host=next.url+8,*colon=strchr(host,':');
    size_t host_size=colon?(size_t)(colon-host):strlen(host);
    if(!host_size||!isalnum((unsigned char)host[0])||!isalnum((unsigned char)host[host_size-1])) return false;
    for(size_t i=0;i<host_size;++i)
        if(!isalnum((unsigned char)host[i])&&host[i]!='.'&&host[i]!='-') return false;
    if(colon) {
        if(!colon[1]) return false;
        for(const char *p=colon+1;*p;++p) if(!isdigit((unsigned char)*p)) return false;
        char *end; long port=strtol(colon+1,&end,10); if(*end||port<1||port>65535) return false;
    }
    const char *begin="-----BEGIN CERTIFICATE-----",*end="-----END CERTIFICATE-----";
    if(strncmp(next.certificate,begin,strlen(begin))||!strstr(next.certificate,end)||
        strstr(next.certificate+strlen(begin),"-----BEGIN")) return false;
    for(const char *p=strstr(next.certificate,end)+strlen(end);*p;++p)
        if(!isspace((unsigned char)*p)) return false;
    *out=next; return true;
}
static bool identity(const cJSON *j,const gbs_hub_config_t *config)
{
    return number(j,"apiVersion",2,2)==2&&equal(j,"hubId",config->hub_id);
}
static bool capability(const cJSON *j,const char *name)
{
    return yes(item(j,name),"available");
}
static const char *const bbw_modes[]={"Continuous","Dose1","Dose2","PulsesType"};
static const char *const prebrew_modes[]={"Disabled","PreBrewing","PreInfusion"};
static const char *const machine_modes[]={"StandBy","BrewingMode","EcoMode"};
static const char *const standby_modes[]={"PowerOn","LastBrewing"};
static unsigned mode(const cJSON *j,const char *key,const char *const *names,unsigned count)
{
    for(unsigned i=0;i<count;++i) if(equal(j,key,names[i])) return i;
    return GBS_UI_VALUE_UNKNOWN;
}
static unsigned capabilities(const cJSON *j)
{
    return ((yes(j,"supportsSmartPresetDecisionValidation")&&capability(j,"brewByWeightDose1")&&capability(j,"brewByWeightMode"))?1u<<GBS_ACTION_WEIGHT:0)|
        (capability(j,"coffeeTemperature")?1u<<GBS_ACTION_COFFEE:0)|
        (capability(j,"steamLevel")?1u<<GBS_ACTION_STEAM:0)|
        (capability(j,"steamEnabled")?1u<<GBS_ACTION_STEAM_ENABLED:0)|
        (capability(j,"brewByWeightDose1")?1u<<GBS_ACTION_DOSE1:0)|
        (capability(j,"brewByWeightDose2")?1u<<GBS_ACTION_DOSE2:0)|
        (capability(j,"brewByWeightMode")?1u<<GBS_ACTION_BBW_MODE:0)|
        (capability(j,"prebrew")?((1u<<GBS_ACTION_PREBREW_MODE)|(1u<<GBS_ACTION_PREBREW_ON)|(1u<<GBS_ACTION_PREBREW_OFF)):0)|
        (capability(j,"power")?1u<<GBS_ACTION_POWER:0)|
        (capability(j,"smartStandby")?((1u<<GBS_ACTION_STANDBY_ENABLED)|(1u<<GBS_ACTION_STANDBY_MINUTES)|(1u<<GBS_ACTION_STANDBY_MODE)):0);
}
static bool binding(const cJSON *j,const gbs_hub_config_t *config,uint32_t *generation)
{
    double g=number(j,"generation",1,UINT32_MAX);
    if(!yes(j,"selected")||!equal(j,"serialNumber",config->serial)||!isfinite(g)||g!=floor(g)) return false;
    *generation=(uint32_t)g; return true;
}
static cJSON *get(const gbs_hub_io_t *io,const char *path,int64_t deadline)
{
    if(io->now(io->context)>=deadline) return NULL;
    if(io->exchange(io->context,path,NULL,io->reply,io->reply_size,deadline)!=200) return NULL;
    cJSON *j=cJSON_ParseWithOpts(io->reply,NULL,true); if(!cJSON_IsObject(j)) { cJSON_Delete(j); return NULL; }
    return j;
}
bool gbs_hub_refresh(const gbs_hub_config_t *config,const gbs_hub_io_t *io,int64_t deadline,gbs_ui_machine_t *machine)
{
    uint32_t before=0,after=0;
    cJSON *j=get(io,"/machine/binding",deadline);
    bool ok=binding(j,config,&before); cJSON_Delete(j);
    if(!ok) { machine->stale=true; machine->writable=0; return false; }
    j=get(io,"/machine/capabilities",deadline);
    ok=identity(j,config); unsigned writable=capabilities(j); cJSON_Delete(j);
    if(!ok) { machine->stale=true; machine->writable=0; return false; }
    j=get(io,"/machine/state",deadline);
    gbs_ui_machine_t next={.coffee=NAN,.coffee_target=NAN,.steam=NAN,.steam_target=NAN,.weight=NAN,.dose1=NAN,.dose2=NAN};
    ok=identity(j,config);
    if(ok) {
        next.available=true; next.stale=!(equal(j,"connectionState","ready")||equal(j,"connectionState","heating"));
        next.coffee=number(j,"coffeeBoilerCurrentTemperature",0,150);
        next.coffee_target=number(j,"coffeeBoilerTargetTemperature",80,100);
        next.steam=number(j,"steamBoilerCurrentTemperature",0,160);
        next.steam_target=number(j,"steamBoilerTargetTemperature",126,131);
        double level=number(j,"steamLevel",1,3);
        const int targets[]={126,128,131};
        if(!isfinite(next.steam_target)&&isfinite(level)&&floor(level)==level) next.steam_target=targets[(unsigned)level-1];
        next.steam_enabled_known=cJSON_IsBool(item(j,"steamBoilerEnabled")); next.steam_enabled=yes(j,"steamBoilerEnabled");
        next.dose1=number(j,"brewByWeightDose1",0,200); next.dose2=number(j,"brewByWeightDose2",0,200);
        next.dose_mode=mode(j,"brewByWeightMode",bbw_modes,4);
        next.weight=next.dose_mode==1?next.dose1:next.dose_mode==2?next.dose2:NAN;
        const cJSON *prebrew=item(j,"prebrew"),*standby=item(j,"smartStandby");
        next.prebrew_mode=mode(prebrew,"mode",prebrew_modes,3);
        next.prebrew_on=number(prebrew,"onTime",0,10); next.prebrew_off=number(prebrew,"offTime",0,10);
        if(!prebrew_time(next.prebrew_on)) next.prebrew_on=NAN;
        if(!prebrew_time(next.prebrew_off)) next.prebrew_off=NAN;
        next.machine_mode=mode(j,"machineMode",machine_modes,3);
        next.standby_enabled_known=cJSON_IsBool(item(standby,"enabled")); next.standby_enabled=yes(standby,"enabled");
        next.standby_minutes=number(standby,"minutes",1,240);
        next.standby_mode=mode(standby,"mode",standby_modes,2);
        if(!isfinite(next.dose1)||!isfinite(next.dose2)||next.dose_mode>3||
            floor(next.dose1)!=next.dose1||floor(next.dose2)!=next.dose2||!equal(j,"cloudCommandReadiness","ready"))
            writable&=~((1u<<GBS_ACTION_WEIGHT)|(1u<<GBS_ACTION_DOSE1)|(1u<<GBS_ACTION_DOSE2)|(1u<<GBS_ACTION_BBW_MODE));
        if(next.prebrew_mode>2||!equal(j,"cloudCommandReadiness","ready"))
            writable&=~((1u<<GBS_ACTION_PREBREW_MODE)|(1u<<GBS_ACTION_PREBREW_ON)|(1u<<GBS_ACTION_PREBREW_OFF));
        if(!isfinite(next.prebrew_on)||!isfinite(next.prebrew_off))
            writable&=~((1u<<GBS_ACTION_PREBREW_ON)|(1u<<GBS_ACTION_PREBREW_OFF));
        if(next.machine_mode>2) writable&=~(1u<<GBS_ACTION_POWER);
        if(!next.standby_enabled_known||next.standby_mode>1||!isfinite(next.standby_minutes)||floor(next.standby_minutes)!=next.standby_minutes)
            writable&=~((1u<<GBS_ACTION_STANDBY_ENABLED)|(1u<<GBS_ACTION_STANDBY_MINUTES)|(1u<<GBS_ACTION_STANDBY_MODE));
        next.writable=writable;
        next.revision=2166136261u;
        for(const unsigned char *p=(const unsigned char *)io->reply;*p;++p) next.revision=(next.revision^*p)*16777619u;
    }
    cJSON_Delete(j);
    if(ok) { j=get(io,"/machine/binding",deadline); ok=binding(j,config,&after)&&before==after; cJSON_Delete(j); }
    if(!ok) { machine->stale=true; machine->writable=0; return false; }
    next.observed_ms=io->now(io->context); *machine=next; return true;
}
static bool optional_number(const cJSON *j,const char *key,double high,double *out)
{
    const cJSON *v=item(j,key);
    *out=number(j,key,0,high);
    return !v||cJSON_IsNull(v)||isfinite(*out);
}

static bool utc_time(const cJSON *j,char out[21])
{
    if(!text(j,"time",out,21,false)||strlen(out)!=20) return false;
    for(unsigned i=0;i<20;++i) {
        char separator=i==4||i==7?'-':i==10?'T':i==13||i==16?':':i==19?'Z':0;
        if(separator?out[i]!=separator:!isdigit((unsigned char)out[i])) return false;
    }
    unsigned year,month,day,hour,minute,second;
    if(sscanf(out,"%u-%u-%uT%u:%u:%uZ",&year,&month,&day,&hour,&minute,&second)!=6||
        year<1970||month<1||month>12||hour>23||minute>59||second>59) return false;
    const unsigned lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned last=lengths[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));
    return day>=1&&day<=last;
}

static bool parse_history(const cJSON *j,gbs_ui_data_t *data)
{
    const cJSON *rows=item(j,"recentCoffees"),*row=NULL;
    if(!cJSON_IsArray(rows)||cJSON_GetArraySize(rows)>512) return false;
    data->total=(unsigned)cJSON_GetArraySize(rows);
    cJSON_ArrayForEach(row,rows) {
        gbs_ui_coffee_t coffee={.mode=mode(row,"doseMode",bbw_modes,4)};
        if(!cJSON_IsObject(row)||!utc_time(row,coffee.time)||
            !optional_number(row,"extractionSeconds",86400,&coffee.seconds)||
            !optional_number(row,"doseValue",1000000,&coffee.dose)||
            !optional_number(row,"targetTemperature",160,&coffee.temperature)) return false;
        unsigned index=0;
        while(index<data->count&&strcmp(data->items.coffees[index].time,coffee.time)>=0) ++index;
        if(index>=GBS_HISTORY_LIMIT) continue;
        if(data->count<GBS_HISTORY_LIMIT) ++data->count;
        memmove(&data->items.coffees[index+1],&data->items.coffees[index],
                (data->count-index-1)*sizeof(coffee));
        data->items.coffees[index]=coffee;
    }
    return true;
}

static bool parse_schedule(const cJSON *j,const gbs_hub_config_t *config,gbs_ui_data_t *data)
{
    const cJSON *raw=item(j,"raw"),*supported=item(raw,"smartWakeUpSleepSupported");
    if(!equal(raw,"serialNumber",config->serial)||!cJSON_IsBool(supported)) return false;
    if(cJSON_IsFalse(supported)) { data->state=GBS_DATA_UNSUPPORTED; return true; }
    const cJSON *settings=item(raw,"smartWakeUpSleep"),*rows=item(settings,"schedules"),*row=NULL;
    if(!cJSON_IsObject(settings)||(rows&&!cJSON_IsArray(rows))||cJSON_GetArraySize(rows)>GBS_SCHEDULE_LIMIT) return false;
    static const char *const weekdays[]={"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
    cJSON_ArrayForEach(row,rows) {
        double on=number(row,"onTimeMinutes",0,1439),off=number(row,"offTimeMinutes",0,1439);
        const cJSON *enabled=item(row,"enabled"),*days=item(row,"days"),*day=NULL;
        if(!cJSON_IsObject(row)||!isfinite(on)||on!=floor(on)||!isfinite(off)||off!=floor(off)||
            !cJSON_IsBool(item(row,"steamBoiler"))||(enabled&&!cJSON_IsBool(enabled))||
            (days&&!cJSON_IsArray(days))||cJSON_GetArraySize(days)>7) return false;
        gbs_ui_schedule_t plan={.on_minutes=(unsigned)on,.off_minutes=(unsigned)off,
            .enabled=yes(row,"enabled"),.steam=yes(row,"steamBoiler")};
        const cJSON *id=item(row,"id");
        if(id&&!cJSON_IsNull(id)&&!text(row,"id",plan.id,sizeof(plan.id),false)) return false;
        if(plan.id[0]) for(unsigned i=0;i<data->count;++i)
            if(!strcmp(plan.id,data->items.schedules[i].id)) return false;
        cJSON_ArrayForEach(day,days) {
            unsigned index=0;
            if(!cJSON_IsString(day)) return false;
            while(index<7&&strcmp(day->valuestring,weekdays[index])) ++index;
            if(index==7||(plan.days&(1u<<index))) return false;
            plan.days|=1u<<index;
        }
        data->items.schedules[data->count++]=plan;
    }
    data->total=data->count;
    return true;
}

void gbs_hub_fetch_data(const gbs_hub_config_t *config,const gbs_hub_io_t *io,
                       gbs_ui_data_kind_t kind,bool refresh,int64_t deadline,gbs_ui_data_t *data)
{
    memset(data,0,sizeof(*data)); data->kind=kind; data->state=GBS_DATA_ERROR;
    if(kind>=GBS_DATA_COUNT) return;
    uint32_t before=0,after=0;
    cJSON *j=get(io,"/machine/binding",deadline);
    bool ok=binding(j,config,&before); cJSON_Delete(j);
    if(!ok) return;
    j=get(io,"/machine/capabilities",deadline);
    ok=identity(j,config); bool available=capability(j,kind==GBS_DATA_SCHEDULE?"schedule":"statistics");
    data->editable=kind==GBS_DATA_SCHEDULE&&capability(j,"scheduleEdit");
    cJSON_Delete(j);
    if(!ok) return;
    if(!available) { data->state=GBS_DATA_UNAVAILABLE; return; }
    if(refresh) {
        const char *path=kind==GBS_DATA_SCHEDULE?"/diagnostics/slow-cloud-reads/refresh?kind=schedule":
            "/diagnostics/slow-cloud-reads/refresh?kind=statistics&days=7";
        if(io->now(io->context)>=deadline||io->exchange(io->context,path,"{}",io->reply,io->reply_size,deadline)!=200) return;
        j=cJSON_ParseWithOpts(io->reply,NULL,true);
        ok=yes(j,"ok")&&yes(j,"accepted")&&equal(j,"kind",kind==GBS_DATA_SCHEDULE?"schedule":"statistics");
        cJSON_Delete(j);
        if(!ok) { data->state=GBS_DATA_UNAVAILABLE; return; }
    }
    const char *path=kind==GBS_DATA_SCHEDULE?"/machine/schedule":"/machine/statistics?days=7";
    if(io->now(io->context)>=deadline) return;
    int status=io->exchange(io->context,path,NULL,io->reply,io->reply_size,deadline);
    bool stale=io->response_stale&&*io->response_stale;
    if(status==202) { data->state=GBS_DATA_LOADING; return; }
    if(status==503) { data->state=GBS_DATA_UNAVAILABLE; return; }
    if(status!=200) return;
    j=cJSON_ParseWithOpts(io->reply,NULL,true);
    data->state=stale?GBS_DATA_STALE:GBS_DATA_READY;
    ok=cJSON_IsObject(j)&&(kind==GBS_DATA_SCHEDULE?parse_schedule(j,config,data):parse_history(j,data));
    cJSON_Delete(j);
    if(ok) { j=get(io,"/machine/binding",deadline); ok=binding(j,config,&after)&&before==after; cJSON_Delete(j); }
    if(ok) { data->valid=data->state!=GBS_DATA_UNSUPPORTED; data->binding_generation=before; }
    else { memset(data,0,sizeof(*data)); data->kind=kind; data->state=GBS_DATA_ERROR; }
}

bool gbs_hub_prepare(gbs_ui_action_t action,double value,const gbs_ui_machine_t *m,const char *id,
                     char *path,size_t path_size,char *body,size_t body_size)
{
    if(!gbs_ui_machine_action(action)||!uuid(id)||!m->available||m->stale||!isfinite(value)||!(m->writable&(1u<<action))) return false;
    const char *route=NULL; char payload[96];
    if(action==GBS_ACTION_WEIGHT) {
        if(value<0||value>200||value!=floor(value)||!isfinite(m->dose1)||!isfinite(m->dose2)) return false;
        unsigned dose=m->dose1==value?1:m->dose2==value?2:0;
        route=dose?"/controls/bbw/mode":"/controls/quick-weight";
        if(dose) snprintf(payload,sizeof(payload),"\"mode\":\"Dose%u\",\"presetGrams\":%.0f,\"clientTtlMs\":8000",dose,value);
        else snprintf(payload,sizeof(payload),"\"grams\":%.0f,\"requireNoStoredMatch\":true,\"clientTtlMs\":8000",value);
    } else if(action==GBS_ACTION_COFFEE) {
        if(value<80||value>100||value*2!=floor(value*2)) return false;
        route="/controls/coffee-temperature"; snprintf(payload,sizeof(payload),"\"temperature\":%.1f",value);
    } else if(action==GBS_ACTION_STEAM) {
        unsigned level=value==126?1:value==128?2:value==131?3:0; if(!level) return false;
        route="/controls/steam-level"; snprintf(payload,sizeof(payload),"\"level\":%u",level);
    } else if(action==GBS_ACTION_STEAM_ENABLED) {
        if(!m->steam_enabled_known||(value!=0&&value!=1)) return false;
        route="/controls/steam/enabled"; snprintf(payload,sizeof(payload),"\"enabled\":%s",value?"true":"false");
    } else if(action==GBS_ACTION_DOSE1||action==GBS_ACTION_DOSE2) {
        if(value<0||value>200||value!=floor(value)||!isfinite(m->dose1)||!isfinite(m->dose2)||m->dose_mode>3) return false;
        route="/controls/bbw/dose";
        snprintf(payload,sizeof(payload),"\"dose\":\"dose%d\",\"grams\":%.0f",action==GBS_ACTION_DOSE1?1:2,value);
    } else if(action==GBS_ACTION_BBW_MODE) {
        if(value<0||value>3||value!=floor(value)||!isfinite(m->dose1)||!isfinite(m->dose2)||m->dose_mode>3) return false;
        route="/controls/bbw/mode"; snprintf(payload,sizeof(payload),"\"mode\":\"%s\"",bbw_modes[(unsigned)value]);
    } else if(action==GBS_ACTION_PREBREW_MODE) {
        if(value<0||value>2||value!=floor(value)||m->prebrew_mode>2) return false;
        route="/controls/prebrew"; snprintf(payload,sizeof(payload),"\"field\":\"mode\",\"value\":\"%s\"",prebrew_modes[(unsigned)value]);
    } else if(action==GBS_ACTION_PREBREW_ON||action==GBS_ACTION_PREBREW_OFF) {
        if(!prebrew_time(value)||m->prebrew_mode>2||!prebrew_time(m->prebrew_on)||!prebrew_time(m->prebrew_off)) return false;
        route="/controls/prebrew";
        snprintf(payload,sizeof(payload),"\"field\":\"%s\",\"value\":%.1f",action==GBS_ACTION_PREBREW_ON?"onTime":"offTime",value);
    } else if(action==GBS_ACTION_POWER) {
        if((value!=0&&value!=1)||m->machine_mode>2) return false;
        route="/controls/power"; snprintf(payload,sizeof(payload),"\"enabled\":%s,\"confirm\":true",value?"true":"false");
    } else if(action>=GBS_ACTION_STANDBY_ENABLED&&action<=GBS_ACTION_STANDBY_MODE) {
        if(!m->standby_enabled_known||m->standby_mode>1||!isfinite(m->standby_minutes)||m->standby_minutes<1||m->standby_minutes>240||floor(m->standby_minutes)!=m->standby_minutes) return false;
        if(action==GBS_ACTION_STANDBY_MINUTES?(value<1||value>240||value!=floor(value)):(value!=0&&value!=1)) return false;
        route="/controls/smart-standby";
        snprintf(payload,sizeof(payload),"\"enabled\":%s,\"minutes\":%.0f,\"mode\":\"%s\",\"confirm\":true",
            (action==GBS_ACTION_STANDBY_ENABLED?value!=0:m->standby_enabled)?"true":"false",
            action==GBS_ACTION_STANDBY_MINUTES?value:m->standby_minutes,
            standby_modes[action==GBS_ACTION_STANDBY_MODE?(unsigned)value:m->standby_mode]);
    } else return false;
    if(strlen(route)>=path_size) return false;
    strcpy(path,route);
    int written=snprintf(body,body_size,"{\"requestId\":\"%s\",\"clientSurface\":\"diagnostic\",%s}",id,payload);
    return written>0&&(size_t)written<body_size;
}
gbs_ui_control_t gbs_hub_result(const char *json,const char *id)
{
    cJSON *j=cJSON_ParseWithOpts(json,NULL,true); gbs_ui_control_t result=GBS_UI_CONTROL_UNKNOWN;
    if(number(j,"apiVersion",2,2)==2&&equal(j,"requestId",id)) {
        if(equal(j,"status","partialApply")||equal(j,"errorCode","partialApply")) result=GBS_UI_CONTROL_PARTIAL;
        else if(equal(j,"status","unknownOutcome")||equal(j,"errorCode","unknownOutcome")||yes(j,"lateSideEffectRisk")) result=GBS_UI_CONTROL_UNKNOWN;
        else if(yes(j,"pending")||equal(j,"status","accepted")||equal(j,"status","running")) result=GBS_UI_CONTROL_PENDING;
        else if(equal(j,"status","completed")) result=yes(j,"ok")?GBS_UI_CONTROL_CONFIRMED:
            yes(j,"writeAccepted")?GBS_UI_CONTROL_UNKNOWN:GBS_UI_CONTROL_FAILED;
    }
    cJSON_Delete(j); return result;
}
static bool same(double a,double b) { return (isnan(a)&&isnan(b))||(isfinite(a)&&isfinite(b)&&fabs(a-b)<0.01); }
static bool matches(gbs_ui_action_t a,double value,const gbs_ui_machine_t *before,const gbs_ui_machine_t *after)
{
    if(!after->available||after->stale) return false;
    if(a==GBS_ACTION_WEIGHT) {
        unsigned mode=before->dose1==value?1:before->dose2==value?2:1;
        return after->dose_mode==mode&&same(after->weight,value)&&same(before->dose2,after->dose2)&&
            (before->dose1!=value&&before->dose2!=value?true:same(before->dose1,after->dose1));
    }
    gbs_ui_machine_t expected=*before;
    switch(a) {
    case GBS_ACTION_DOSE1: expected.dose1=value; break;
    case GBS_ACTION_DOSE2: expected.dose2=value; break;
    case GBS_ACTION_BBW_MODE: expected.dose_mode=(unsigned)value; break;
    case GBS_ACTION_PREBREW_MODE: expected.prebrew_mode=(unsigned)value; break;
    case GBS_ACTION_PREBREW_ON: expected.prebrew_on=value; break;
    case GBS_ACTION_PREBREW_OFF: expected.prebrew_off=value; break;
    case GBS_ACTION_STANDBY_ENABLED: expected.standby_enabled=value!=0; break;
    case GBS_ACTION_STANDBY_MINUTES: expected.standby_minutes=value; break;
    case GBS_ACTION_STANDBY_MODE: expected.standby_mode=(unsigned)value; break;
    default: return same(gbs_ui_action_value(a,after),value);
    }
    return gbs_ui_action_unchanged(a,&expected,after);
}
gbs_ui_control_t gbs_hub_execute(const gbs_hub_config_t *config,const gbs_hub_io_t *io,
                                gbs_ui_action_t action,double value,const char *id,
                                int64_t deadline,gbs_ui_machine_t *machine)
{
    gbs_ui_machine_t before=*machine;
    if(!gbs_hub_refresh(config,io,deadline,machine)||!gbs_ui_action_unchanged(action,&before,machine)) return GBS_UI_CONTROL_FAILED;
    char path[96],body[256];
    if(io->now(io->context)>=deadline||!gbs_hub_prepare(action,value,machine,id,path,sizeof(path),body,sizeof(body))) return GBS_UI_CONTROL_FAILED;
    int status=io->exchange(io->context,path,body,io->reply,io->reply_size,deadline);
    if(status==400||status==401||status==403||status==409||status==426) return GBS_UI_CONTROL_FAILED;
    gbs_ui_control_t result=status>0?gbs_hub_result(io->reply,id):GBS_UI_CONTROL_PENDING;
    snprintf(path,sizeof(path),"/diagnostics/commands?id=%s",id);
    while(io->now(io->context)<deadline) {
        if(result==GBS_UI_CONTROL_CONFIRMED) {
            if(gbs_hub_refresh(config,io,deadline,machine)&&matches(action,value,&before,machine)) return result;
        } else if(result!=GBS_UI_CONTROL_PENDING) return result;
        io->wait(io->context,250);
        if(io->now(io->context)>=deadline) break;
        status=io->exchange(io->context,path,NULL,io->reply,io->reply_size,deadline);
        if(status==200||status==202) result=gbs_hub_result(io->reply,id);
        else if(status==401||status==403||status==404) return GBS_UI_CONTROL_UNKNOWN;
    }
    return GBS_UI_CONTROL_UNKNOWN;
}

static cJSON *schedule_json(const gbs_ui_schedule_t *plan)
{
    if(!plan||!plan->id[0]||!memchr(plan->id,0,sizeof(plan->id))||plan->on_minutes>1439||
        plan->off_minutes>1439||plan->days>127) return NULL;
    cJSON *j=cJSON_CreateObject(),*days=cJSON_CreateArray();
    const char *names[]={"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
    bool ok=j&&days&&cJSON_AddStringToObject(j,"id",plan->id)&&cJSON_AddBoolToObject(j,"enabled",plan->enabled)&&
        cJSON_AddNumberToObject(j,"onTimeMinutes",plan->on_minutes)&&cJSON_AddNumberToObject(j,"offTimeMinutes",plan->off_minutes)&&
        cJSON_AddBoolToObject(j,"steamBoiler",plan->steam);
    for(unsigned i=0;i<7&&ok;++i) if(plan->days&(1u<<i)) ok=cJSON_AddItemToArray(days,cJSON_CreateString(names[i]));
    if(!ok||!cJSON_AddItemToObject(j,"days",days)) { cJSON_Delete(days); cJSON_Delete(j); return NULL; }
    return j;
}
gbs_ui_control_t gbs_hub_execute_schedule(const gbs_hub_config_t *config,const gbs_hub_io_t *io,
    const gbs_ui_schedule_t *expected,const gbs_ui_schedule_t *requested,const char *id,
    int64_t deadline,gbs_ui_data_t *data)
{
    if(!uuid(id)||!expected||!requested||!memchr(expected->id,0,sizeof(expected->id))||
        !memchr(requested->id,0,sizeof(requested->id))||strcmp(expected->id,requested->id)||
        (requested->enabled&&(!requested->days||requested->on_minutes==requested->off_minutes))) return GBS_UI_CONTROL_FAILED;
    gbs_hub_fetch_data(config,io,GBS_DATA_SCHEDULE,false,deadline,data);
    if(!data->valid||!data->editable||data->state!=GBS_DATA_READY) return GBS_UI_CONTROL_FAILED;
    unsigned index=0;
    while(index<data->count&&strcmp(data->items.schedules[index].id,expected->id)) ++index;
    if(index==data->count||!gbs_ui_schedule_equal(&data->items.schedules[index],expected)) return GBS_UI_CONTROL_FAILED;
    cJSON *j=cJSON_CreateObject(),*before=schedule_json(expected),*after=schedule_json(requested);
    if(!j||!before||!after) { cJSON_Delete(j); cJSON_Delete(before); cJSON_Delete(after); return GBS_UI_CONTROL_FAILED; }
    bool ok=cJSON_AddStringToObject(j,"requestId",id)&&cJSON_AddStringToObject(j,"clientSurface","diagnostic")&&
        cJSON_AddBoolToObject(j,"confirm",true);
    cJSON_AddItemToObject(j,"expected",before); cJSON_AddItemToObject(j,"schedule",after);
    char body[1536],path[96]; ok=ok&&cJSON_PrintPreallocated(j,body,sizeof(body),false); cJSON_Delete(j);
    if(!ok||io->now(io->context)>=deadline) return GBS_UI_CONTROL_FAILED;
    int status=io->exchange(io->context,"/controls/schedule",body,io->reply,io->reply_size,deadline);
    if(status==400||status==401||status==403||status==409||status==426) return GBS_UI_CONTROL_FAILED;
    gbs_ui_control_t result=status>0?gbs_hub_result(io->reply,id):GBS_UI_CONTROL_PENDING;
    snprintf(path,sizeof(path),"/diagnostics/commands?id=%s",id);
    while(io->now(io->context)<deadline) {
        if(result==GBS_UI_CONTROL_CONFIRMED) {
            j=cJSON_ParseWithOpts(io->reply,NULL,true);
            ok=yes(j,"stateConfirmed")&&equal(j,"control","schedule"); cJSON_Delete(j);
            uint32_t generation=0; j=get(io,"/machine/binding",deadline);
            ok=ok&&binding(j,config,&generation)&&generation==data->binding_generation; cJSON_Delete(j);
            if(!ok) return GBS_UI_CONTROL_UNKNOWN;
            data->items.schedules[index]=*requested; data->state=GBS_DATA_STALE;
            return result;
        }
        if(result!=GBS_UI_CONTROL_PENDING) return result;
        io->wait(io->context,250); if(io->now(io->context)>=deadline) break;
        status=io->exchange(io->context,path,NULL,io->reply,io->reply_size,deadline);
        if(status==200||status==202) result=gbs_hub_result(io->reply,id);
        else if(status==401||status==403||status==404) return GBS_UI_CONTROL_UNKNOWN;
    }
    return GBS_UI_CONTROL_UNKNOWN;
}
