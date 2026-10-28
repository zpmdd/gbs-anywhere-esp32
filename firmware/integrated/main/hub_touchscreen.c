#include "hub_touchscreen.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "hub_http.h"
#include "hub_cloud.h"
#include "hub_state.h"
#include <stdio.h>
#include <string.h>

static int copy(const char *text,char *reply,size_t size,bool identity)
{
    if(!text) return 503;
    if(!identity) {
        size_t length=strlen(text);
        if(length>=size) return 503;
        memcpy(reply,text,length+1); return 200;
    }
    cJSON *j=cJSON_Parse(text);
    if(!cJSON_IsObject(j)) { cJSON_Delete(j); return 503; }
    bool ok=cJSON_AddNumberToObject(j,"apiVersion",2)&&cJSON_AddStringToObject(j,"hubId","local")&&
        cJSON_AddStringToObject(j,"cloudCommandReadiness",hub_cloud_command_readiness())&&
        cJSON_PrintPreallocated(j,reply,(int)size,false);
    cJSON_Delete(j); return ok?200:503;
}

int hub_touchscreen_exchange(const char *path,const char *body,const char *serial,
                            char *reply,size_t size,int64_t deadline,bool *stale)
{
    if(!path||!reply||size<64||!stale||esp_timer_get_time()/1000>=deadline) return -1;
    reply[0]=0; *stale=false;
    if(!hub_http_local_lock()) return 503;
    int status=404;
    if(esp_timer_get_time()/1000>=deadline) { status=-1; goto done; }
    if(!body&&!strcmp(path,"/machine/state")) status=copy(hub_state_machine_state_json(),reply,size,true);
    else if(!body&&!strcmp(path,"/machine/capabilities")) status=copy(hub_state_capabilities_json(),reply,size,true);
    else if(!body&&!strcmp(path,"/machine/binding")) status=copy(hub_state_machine_binding_json(),reply,size,false);
    else if(!body&&!strcmp(path,"/diagnostics/ble")) status=copy(hub_state_ble_diagnostics_json(),reply,size,true);
    else if(!body&&(!strcmp(path,"/machine/schedule")||!strcmp(path,"/machine/statistics?days=7"))) {
        hub_state_slow_read_result_t result=!strcmp(path,"/machine/schedule")?hub_state_schedule_read():hub_state_statistics_read(7);
        *stale=result.stale;
        status=copy(result.body,reply,size,false);
        if(status==200&&result.state!=HUB_STATE_SLOW_READ_HIT) status=503;
    } else if(body&&(!strcmp(path,"/diagnostics/slow-cloud-reads/refresh?kind=schedule")||
                     !strcmp(path,"/diagnostics/slow-cloud-reads/refresh?kind=statistics&days=7"))) {
        const char *kind=strstr(path,"kind=schedule")?"schedule":"statistics";
        status=copy(hub_state_start_slow_cloud_read_json(kind,7),reply,size,false);
    } else if(body&&!strcmp(path,"/diagnostics/cloud/refresh")) {
        // Join an existing refresh without advancing its generation or adding another read.
        bool pending=hub_cloud_refresh_pending();
        esp_err_t err=pending?ESP_OK:hub_cloud_request_refresh();
        if(err==ESP_OK||hub_cloud_refresh_pending()) {
            status=copy("{\"accepted\":true,\"pending\":true}",reply,size,false);
            if(status==200) status=202;
        } else status=503;
    } else if(!body&&!strncmp(path,"/diagnostics/commands?id=",25)) {
        const char *id=path+25;
        if(hub_command_request_id_valid(id)) { hub_command_diagnostics_json(id,reply,size); status=200; }
        else status=400;
    } else if(body&&!strncmp(path,"/controls/",10)) {
        hub_command_request_t command;
        if(!hub_touchscreen_parse_command(path,body,serial,&command)) { status=400; goto done; }
        hub_command_submission_t submission;
        hub_command_submit_status_t submitted=hub_command_submit(&command,NULL,NULL,&submission);
        hub_control_result_json(&submission.result,reply,size);
        switch(submitted) {
        case HUB_COMMAND_ACCEPTED: case HUB_COMMAND_DUPLICATE_INFLIGHT: status=202; break;
        case HUB_COMMAND_DUPLICATE_COMPLETE: status=200; break;
        case HUB_COMMAND_INVALID: status=400; break;
        case HUB_COMMAND_CONFLICT: status=409; break;
        default: status=503; break;
        }
    }
done:
    hub_http_local_unlock();
    return status;
}
