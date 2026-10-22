#include "hub_schedule.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

static const char *const weekdays[]={"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
static const cJSON *item(const cJSON *j,const char *key) { return cJSON_GetObjectItemCaseSensitive(j,key); }
static bool minutes(const cJSON *value,uint16_t *out)
{
    if(!cJSON_IsNumber(value)||!isfinite(value->valuedouble)||value->valuedouble<0||
        value->valuedouble>1439||value->valuedouble!=floor(value->valuedouble)) return false;
    *out=(uint16_t)value->valuedouble; return true;
}
bool hub_schedule_valid(const hub_schedule_entry_t *entry)
{
    if(!entry||!entry->id[0]||!memchr(entry->id,0,sizeof(entry->id))||entry->on_minutes>1439||
        entry->off_minutes>1439||entry->days>127) return false;
    for(const unsigned char *p=(const unsigned char *)entry->id;*p;++p) if(*p<32||*p>126) return false;
    return true;
}
bool hub_schedule_parse(const cJSON *j,hub_schedule_entry_t *entry)
{
    hub_schedule_entry_t next={0}; const cJSON *id=item(j,"id"),*days=item(j,"days"),*day=NULL;
    if(!cJSON_IsObject(j)||!cJSON_IsString(id)||strlen(id->valuestring)>=sizeof(next.id)||
        !minutes(item(j,"onTimeMinutes"),&next.on_minutes)||!minutes(item(j,"offTimeMinutes"),&next.off_minutes)||
        !cJSON_IsBool(item(j,"enabled"))||!cJSON_IsBool(item(j,"steamBoiler"))||
        !cJSON_IsArray(days)||cJSON_GetArraySize(days)>7) return false;
    strcpy(next.id,id->valuestring); next.enabled=cJSON_IsTrue(item(j,"enabled")); next.steam=cJSON_IsTrue(item(j,"steamBoiler"));
    cJSON_ArrayForEach(day,days) {
        unsigned index=0; if(!cJSON_IsString(day)) return false;
        while(index<7&&strcmp(day->valuestring,weekdays[index])) ++index;
        if(index==7||(next.days&(1u<<index))) return false;
        next.days|=1u<<index;
    }
    if(!hub_schedule_valid(&next)) return false;
    *entry=next; return true;
}
bool hub_schedule_find(const char *json,const char *serial,const char *id,hub_schedule_entry_t *entry)
{
    cJSON *j=cJSON_ParseWithOpts(json,NULL,true); const cJSON *raw=item(j,"raw"),*sn=item(raw,"serialNumber");
    const cJSON *rows=item(item(raw,"smartWakeUpSleep"),"schedules"),*row=NULL;
    bool ok=cJSON_IsString(sn)&&!strcmp(sn->valuestring,serial)&&cJSON_IsTrue(item(raw,"smartWakeUpSleepSupported"))&&
        cJSON_IsArray(rows)&&cJSON_GetArraySize(rows)<=64;
    unsigned matches=0; hub_schedule_entry_t found={0};
    if(ok) cJSON_ArrayForEach(row,rows) {
        const cJSON *row_id=item(row,"id");
        if(cJSON_IsString(row_id)&&!strcmp(row_id->valuestring,id)) {
            ++matches; if(!hub_schedule_parse(row,&found)) ok=false;
        }
    }
    cJSON_Delete(j);
    if(!ok||matches!=1) return false;
    *entry=found; return true;
}
cJSON *hub_schedule_json(const hub_schedule_entry_t *entry)
{
    if(!hub_schedule_valid(entry)) return NULL;
    cJSON *j=cJSON_CreateObject(),*days=cJSON_CreateArray();
    bool ok=j&&days&&cJSON_AddStringToObject(j,"id",entry->id)&&
        cJSON_AddBoolToObject(j,"enabled",entry->enabled)&&cJSON_AddNumberToObject(j,"onTimeMinutes",entry->on_minutes)&&
        cJSON_AddNumberToObject(j,"offTimeMinutes",entry->off_minutes)&&cJSON_AddBoolToObject(j,"steamBoiler",entry->steam);
    for(unsigned i=0;i<7&&ok;++i) if(entry->days&(1u<<i)) ok=cJSON_AddItemToArray(days,cJSON_CreateString(weekdays[i]));
    if(!ok||!cJSON_AddItemToObject(j,"days",days)) { cJSON_Delete(days); cJSON_Delete(j); return NULL; }
    return j;
}
