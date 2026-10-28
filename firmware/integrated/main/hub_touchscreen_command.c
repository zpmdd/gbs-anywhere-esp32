#include "hub_touchscreen.h"
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool string(const cJSON *j, const char *key, char *out, size_t size)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=size) return false;
    strcpy(out,v->valuestring); return true;
}
static bool number(const cJSON *j,const char *key,double *out)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)) return false;
    *out=v->valuedouble; return true;
}
static bool integer(const cJSON *j,const char *key,int *out)
{
    double value;
    if(!number(j,key,&value)||value<INT_MIN||value>INT_MAX||floor(value)!=value) return false;
    *out=(int)value; return true;
}
static bool boolean(const cJSON *j,const char *key,bool *out)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if(!cJSON_IsBool(v)) return false;
    *out=cJSON_IsTrue(v); return true;
}

bool hub_touchscreen_parse_command(const char *path,const char *body,const char *serial,hub_command_request_t *c)
{
    if(!path||!body||!serial||!serial[0]||strlen(serial)>HUB_CONFIG_MAX_MACHINE_SERIAL_LEN||!c) return false;
    memset(c,0,sizeof(*c));
    cJSON *j=cJSON_ParseWithOpts(body,NULL,true);
    bool ok=cJSON_IsObject(j)&&string(j,"requestId",c->request_id,sizeof(c->request_id))&&hub_command_request_id_valid(c->request_id);
    if(!ok) { cJSON_Delete(j); return false; }
    strcpy(c->expected_serial_number,serial);
    c->client_surface=HUB_API_V2_SURFACE_DIAGNOSTIC;
    if(!strcmp(path,"/controls/quick-weight")) {
        c->kind=HUB_COMMAND_QUICK_WEIGHT; ok=integer(j,"grams",&c->value.quick_weight.grams);
        const cJSON *guard=cJSON_GetObjectItemCaseSensitive(j,"requireNoStoredMatch");
        if(guard) ok=ok&&boolean(j,"requireNoStoredMatch",&c->value.quick_weight.require_no_stored_match);
    } else if(!strcmp(path,"/controls/bbw/dose")) {
        c->kind=HUB_COMMAND_BBW_DOSE;
        ok=string(j,"dose",c->value.bbw_dose.dose,sizeof(c->value.bbw_dose.dose))&&integer(j,"grams",&c->value.bbw_dose.grams);
    } else if(!strcmp(path,"/controls/bbw/mode")) {
        c->kind=HUB_COMMAND_BBW_MODE; ok=string(j,"mode",c->value.bbw_mode.mode,sizeof(c->value.bbw_mode.mode));
        if(cJSON_GetObjectItemCaseSensitive(j,"presetGrams")) {
            c->value.bbw_mode.has_preset_grams=true;
            ok=ok&&integer(j,"presetGrams",&c->value.bbw_mode.preset_grams)&&c->value.bbw_mode.preset_grams>=0&&c->value.bbw_mode.preset_grams<=200&&
                (!strcmp(c->value.bbw_mode.mode,"Dose1")||!strcmp(c->value.bbw_mode.mode,"Dose2"));
        }
    } else if(!strcmp(path,"/controls/coffee-temperature")) {
        c->kind=HUB_COMMAND_COFFEE_TEMPERATURE; ok=number(j,"temperature",&c->value.temperature.temperature);
    } else if(!strcmp(path,"/controls/steam-level")) {
        c->kind=HUB_COMMAND_STEAM_LEVEL; ok=integer(j,"level",&c->value.steam_level.level);
    } else if(!strcmp(path,"/controls/steam/enabled")) {
        c->kind=HUB_COMMAND_STEAM_ENABLED; ok=boolean(j,"enabled",&c->value.enabled.enabled);
    } else if(!strcmp(path,"/controls/prebrew")) {
        c->kind=HUB_COMMAND_PREBREW;
        ok=string(j,"field",c->value.prebrew.field,sizeof(c->value.prebrew.field));
        if(ok&&!string(j,"value",c->value.prebrew.value,sizeof(c->value.prebrew.value))) {
            double value; ok=number(j,"value",&value)&&value>=0&&value<=10;
            if(ok) snprintf(c->value.prebrew.value,sizeof(c->value.prebrew.value),"%.1f",value);
        }
    } else if(!strcmp(path,"/controls/power")) {
        c->kind=HUB_COMMAND_POWER;
        ok=boolean(j,"enabled",&c->value.power.enabled)&&boolean(j,"confirm",&c->value.power.confirm)&&c->value.power.confirm;
    } else if(!strcmp(path,"/controls/smart-standby")) {
        c->kind=HUB_COMMAND_SMART_STANDBY;
        ok=boolean(j,"enabled",&c->value.smart_standby.enabled)&&integer(j,"minutes",&c->value.smart_standby.minutes)&&
            string(j,"mode",c->value.smart_standby.mode,sizeof(c->value.smart_standby.mode))&&
            boolean(j,"confirm",&c->value.smart_standby.confirm)&&c->value.smart_standby.confirm;
    } else if(!strcmp(path,"/controls/schedule")) {
        c->kind=HUB_COMMAND_SCHEDULE;
        ok=boolean(j,"confirm",&c->value.schedule.confirm)&&c->value.schedule.confirm&&
            hub_schedule_parse(cJSON_GetObjectItemCaseSensitive(j,"expected"),&c->value.schedule.expected)&&
            hub_schedule_parse(cJSON_GetObjectItemCaseSensitive(j,"schedule"),&c->value.schedule.requested)&&
            !strcmp(c->value.schedule.expected.id,c->value.schedule.requested.id);
    } else ok=false;
    c->deadline.enforced=c->kind==HUB_COMMAND_QUICK_WEIGHT||c->kind==HUB_COMMAND_PREBREW||(c->kind==HUB_COMMAND_BBW_MODE&&c->value.bbw_mode.has_preset_grams);
    if(cJSON_GetObjectItemCaseSensitive(j,"clientTtlMs")) {
        int ttl=0; ok=ok&&c->deadline.enforced&&integer(j,"clientTtlMs",&ttl)&&ttl>=1&&ttl<=(int)HUB_COMMAND_DEADLINE_MAX_TTL_MS;
        c->deadline.client_ttl_present=true; c->deadline.client_ttl_ms=(uint32_t)ttl;
    }
    cJSON_Delete(j); return ok;
}
