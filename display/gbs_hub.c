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
static unsigned capabilities(const cJSON *j)
{
    return ((yes(j,"supportsSmartPresetDecisionValidation")&&capability(j,"brewByWeightDose1")&&capability(j,"brewByWeightMode"))?1u<<GBS_ACTION_WEIGHT:0)|
        (capability(j,"coffeeTemperature")?1u<<GBS_ACTION_COFFEE:0)|
        (capability(j,"steamLevel")?1u<<GBS_ACTION_STEAM:0)|
        (capability(j,"steamEnabled")?1u<<GBS_ACTION_STEAM_ENABLED:0);
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
        next.dose_mode=equal(j,"brewByWeightMode","Dose1")?1:equal(j,"brewByWeightMode","Dose2")?2:0;
        next.weight=next.dose_mode==1?next.dose1:next.dose_mode==2?next.dose2:NAN;
        if(!isfinite(next.dose1)||!isfinite(next.dose2)||!equal(j,"cloudCommandReadiness","ready")) writable&=~(1u<<GBS_ACTION_WEIGHT);
        next.writable=writable;
        next.revision=2166136261u;
        for(const unsigned char *p=(const unsigned char *)io->reply;*p;++p) next.revision=(next.revision^*p)*16777619u;
    }
    cJSON_Delete(j);
    if(ok) { j=get(io,"/machine/binding",deadline); ok=binding(j,config,&after)&&before==after; cJSON_Delete(j); }
    if(!ok) { machine->stale=true; machine->writable=0; return false; }
    next.observed_ms=io->now(io->context); *machine=next; return true;
}
bool gbs_hub_prepare(gbs_ui_action_t action,double value,const gbs_ui_machine_t *m,const char *id,
                     char *path,size_t path_size,char *body,size_t body_size)
{
    if((unsigned)action>GBS_ACTION_STEAM_ENABLED||!uuid(id)||!m->available||m->stale||!isfinite(value)||!(m->writable&(1u<<action))) return false;
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
static bool unchanged(gbs_ui_action_t a,const gbs_ui_machine_t *before,const gbs_ui_machine_t *after)
{
    if(a==GBS_ACTION_WEIGHT) return before->dose_mode==after->dose_mode&&same(before->dose1,after->dose1)&&same(before->dose2,after->dose2);
    if(a==GBS_ACTION_COFFEE) return same(before->coffee_target,after->coffee_target);
    if(a==GBS_ACTION_STEAM) return same(before->steam_target,after->steam_target);
    return before->steam_enabled_known&&after->steam_enabled_known&&before->steam_enabled==after->steam_enabled;
}
static bool matches(gbs_ui_action_t a,double value,const gbs_ui_machine_t *before,const gbs_ui_machine_t *after)
{
    if(!after->available||after->stale) return false;
    if(a==GBS_ACTION_WEIGHT) {
        unsigned mode=before->dose1==value?1:before->dose2==value?2:1;
        return after->dose_mode==mode&&same(after->weight,value)&&same(before->dose2,after->dose2)&&
            (before->dose1!=value&&before->dose2!=value?true:same(before->dose1,after->dose1));
    }
    if(a==GBS_ACTION_COFFEE) return same(after->coffee_target,value);
    if(a==GBS_ACTION_STEAM) return same(after->steam_target,value);
    return after->steam_enabled_known&&after->steam_enabled==(value!=0);
}
gbs_ui_control_t gbs_hub_execute(const gbs_hub_config_t *config,const gbs_hub_io_t *io,
                                gbs_ui_action_t action,double value,const char *id,
                                int64_t deadline,gbs_ui_machine_t *machine)
{
    gbs_ui_machine_t before=*machine;
    if(!gbs_hub_refresh(config,io,deadline,machine)||!unchanged(action,&before,machine)) return GBS_UI_CONTROL_FAILED;
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
