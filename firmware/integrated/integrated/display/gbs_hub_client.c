#include "gbs_hub_client.h"
#include "gbs_hub.h"
#include "esp_heap_caps.h"
#include "hub_touchscreen.h"
#include "hub_config.h"
#include "esp_log.h"
#ifdef HUB_AMOLED_216
#include "gbs_scale_battery_cache.h"
#include "hub_command_coordinator.h"
#include "hub_grinder.h"
#endif

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { REPLY_SIZE=32768 };
static const char *TAG="gbs_hub";
// Reserve internal stacks before Wi-Fi/BLE fragment the heap during startup.
static StackType_t data_stack[8192 / sizeof(StackType_t)];
static StaticTask_t data_task_storage;
#ifdef HUB_AMOLED_216
static gbs_scale_battery_cache_t scale_battery_cache;
_Static_assert(GBS_SCALE_BATTERY_SERIAL_SIZE == HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1,
    "Scale battery binding must fit the selected machine serial");
#endif
static struct {
    gbs_hub_config_t *config;
    hub_machine_selection_identity_t selection;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    gbs_ui_machine_t machine, submitted_machine;
    gbs_ui_action_t action;
    gbs_ui_control_t result;
    uint32_t request_id, completed_id;
    double value;
    int64_t deadline;
    bool busy, queued, response_stale;
    gbs_ui_data_t *data;
    bool schedule_queued;
    gbs_ui_schedule_t schedule_expected,schedule_requested;
    unsigned data_pending, data_refresh;
    int64_t data_deadline[GBS_DATA_COUNT], data_poll[GBS_DATA_COUNT];
    bool scale_refresh_queued;
    int64_t scale_refresh_after;
    char *reply;
} hub;

static int64_t now(void *context) { (void)context; return esp_timer_get_time()/1000; }
static void wait_ms(void *context,unsigned ms) { (void)context; vTaskDelay(pdMS_TO_TICKS(ms)); }
static int exchange(void *context,const char *path,const char *body,char *reply,size_t size,int64_t deadline)
{
    (void)context;
    if(!hub_config_machine_selection_matches(&hub.selection)) return -1;
    return hub_touchscreen_exchange(path,body,hub.config->serial,reply,size,deadline,&hub.response_stale);
}
static void request_uuid(char out[37])
{
    unsigned char bytes[16]; esp_fill_random(bytes,sizeof(bytes));
    bytes[6]=(bytes[6]&15)|64; bytes[8]=(bytes[8]&63)|128;
    snprintf(out,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
}
#ifdef HUB_AMOLED_216
static void update_scale_battery(gbs_ui_machine_t *machine)
{
    if (!hub_config_machine_selection_matches(&hub.selection)) {
        machine->scale_battery_last_known=false;
        machine->scale_battery_last_percent=0;
        machine->scale_battery_last_observed_unix=0;
        return;
    }
    gbs_scale_battery_cache_observe(&scale_battery_cache,hub.selection.serial_number,
        hub.selection.generation,machine,now(NULL),(int64_t)time(NULL));
    gbs_scale_battery_cache_apply(&scale_battery_cache,machine);
}

static void save_scale_battery_when_idle(void)
{
    if (!hub_config_machine_selection_matches(&hub.selection)
        || hub_command_idle_background_deferred()) return;
    if (hub_grinder_busy()) return;
    xSemaphoreTake(hub.mutex,portMAX_DELAY);
    bool idle=!hub.busy&&!hub.queued;
    xSemaphoreGive(hub.mutex);
    if (!idle || !hub_config_machine_selection_matches(&hub.selection)) return;
    // Flash writes stay outside the model mutex and never change command results.
    esp_err_t err=gbs_scale_battery_cache_save(&scale_battery_cache,now(NULL),idle);
    if (err!=ESP_OK) ESP_LOGW(TAG,"Scale battery history save deferred: %s",esp_err_to_name(err));
}
#endif
static void task(void *unused)
{
    (void)unused;
    const gbs_hub_io_t io={.exchange=exchange,.now=now,.wait=wait_ms,.reply=hub.reply,.reply_size=REPLY_SIZE,
        .response_stale=&hub.response_stale};
    int64_t next_poll=0;
    for(;;) {
        xSemaphoreTake(hub.mutex,portMAX_DELAY);
        bool queued=hub.queued; hub.queued=false;
        bool schedule_queued=hub.schedule_queued; hub.schedule_queued=false;
        bool scale_refresh_queued=hub.scale_refresh_queued;
        gbs_ui_machine_t machine=queued?hub.submitted_machine:hub.machine;
        gbs_ui_action_t action=hub.action; double value=hub.value;
        uint32_t request_id=hub.request_id; int64_t deadline=hub.deadline;
        unsigned kind=GBS_DATA_COUNT;
        for(unsigned i=0;i<GBS_DATA_COUNT;++i)
            if((hub.data_pending&(1u<<i))&&now(NULL)>=hub.data_poll[i]) { kind=i; break; }
        xSemaphoreGive(hub.mutex);
        bool online=true; // Local Hub services also work without LAN or a wall clock.
        {
            hub_machine_selection_identity_t selected={0};
            hub_config_get_machine_selection_identity(&selected);
            if(strcmp(selected.serial_number,hub.config->serial)||selected.generation!=hub.selection.generation) {
                online=!queued;
#ifdef HUB_AMOLED_216
                gbs_scale_battery_cache_bind(&scale_battery_cache,selected.serial_number,
                    selected.generation,(int64_t)time(NULL));
#endif
                snprintf(hub.config->serial,sizeof(hub.config->serial),"%s",selected.serial_number);
                xSemaphoreTake(hub.mutex,portMAX_DELAY);
                hub.selection=selected;
                memset(&hub.machine,0,sizeof(hub.machine)); hub.machine.stale=hub.machine.scale_stale=true;
#ifdef HUB_AMOLED_216
                gbs_scale_battery_cache_apply(&scale_battery_cache,&hub.machine);
#endif
                hub.scale_refresh_queued=scale_refresh_queued=false; hub.scale_refresh_after=0;
                for(unsigned i=0;i<GBS_DATA_COUNT;++i) {
                    uint32_t revision=hub.data[i].revision+1;
                    memset(&hub.data[i],0,sizeof(hub.data[i]));
                    hub.data[i].revision=revision; hub.data[i].kind=i;
                }
                hub.data_pending=hub.data_refresh=0; machine=hub.machine;
                xSemaphoreGive(hub.mutex); next_poll=0;
            }
        }
        if(queued) {
            char id[37]; request_uuid(id);
            gbs_ui_control_t result=GBS_UI_CONTROL_FAILED;
            gbs_ui_data_t *data=&hub.data[GBS_DATA_COUNT];
            if(schedule_queued) { memset(data,0,sizeof(*data)); data->kind=GBS_DATA_SCHEDULE; data->state=GBS_DATA_ERROR; }
            if(online) result=schedule_queued?
                gbs_hub_execute_schedule(hub.config,&io,&hub.schedule_expected,&hub.schedule_requested,id,deadline,data):
                gbs_hub_execute(hub.config,&io,action,value,id,deadline,&machine);
#ifdef HUB_AMOLED_216
            update_scale_battery(&machine);
#endif
            xSemaphoreTake(hub.mutex,portMAX_DELAY);
            hub.machine=machine; hub.result=result; hub.completed_id=request_id; hub.busy=false;
            if(schedule_queued) {
                if(!online) { memset(data,0,sizeof(*data)); data->kind=GBS_DATA_SCHEDULE; data->state=GBS_DATA_UNAVAILABLE; }
                data->editable=false; data->revision=hub.data[GBS_DATA_SCHEDULE].revision+1;
                hub.data[GBS_DATA_SCHEDULE]=*data;
            }
            xSemaphoreGive(hub.mutex);
            next_poll=now(NULL)+1000;
        } else if(scale_refresh_queued) {
            xSemaphoreTake(hub.mutex,portMAX_DELAY); hub.scale_refresh_queued=false; xSemaphoreGive(hub.mutex);
            // This only wakes the existing Cloud owner; it never performs a network read here.
            if(online) exchange(NULL,"/diagnostics/cloud/refresh","{}",hub.reply,REPLY_SIZE,now(NULL)+2000);
            next_poll=0;
        } else if(now(NULL)>=next_poll) {
            if(!online||!gbs_hub_refresh(hub.config,&io,now(NULL)+12000,&machine)) {
                machine.stale=true; machine.writable=0;
            }
#ifdef HUB_AMOLED_216
            update_scale_battery(&machine);
#endif
            xSemaphoreTake(hub.mutex,portMAX_DELAY); hub.machine=machine; xSemaphoreGive(hub.mutex);
            next_poll=now(NULL)+1000;
        } else if(kind<GBS_DATA_COUNT) {
            xSemaphoreTake(hub.mutex,portMAX_DELAY);
            bool refresh=(hub.data_refresh&(1u<<kind))!=0;
            hub.data_refresh&=~(1u<<kind);
            deadline=hub.data_deadline[kind];
            xSemaphoreGive(hub.mutex);
            gbs_ui_data_t *data=&hub.data[GBS_DATA_COUNT];
            memset(data,0,sizeof(*data)); data->kind=kind; data->state=GBS_DATA_UNAVAILABLE;
            int64_t step_deadline=now(NULL)+8000;
            if(step_deadline>deadline) step_deadline=deadline;
            if(online&&now(NULL)<deadline) gbs_hub_fetch_data(hub.config,&io,kind,refresh,step_deadline,data);
            xSemaphoreTake(hub.mutex,portMAX_DELAY);
            if(data->state==GBS_DATA_UNAVAILABLE&&hub.data[kind].valid) {
                *data=hub.data[kind]; data->state=GBS_DATA_STALE; data->editable=false;
            }
            bool waiting=data->state==GBS_DATA_LOADING||data->state==GBS_DATA_STALE;
            if(waiting&&now(NULL)<deadline) {
                if(!data->valid&&hub.data[kind].valid) *data=hub.data[kind];
                data->state=GBS_DATA_LOADING; hub.data_poll[kind]=now(NULL)+2000;
            } else {
                hub.data_pending&=~(1u<<kind);
                if(data->state==GBS_DATA_LOADING) data->state=data->valid?GBS_DATA_STALE:GBS_DATA_UNAVAILABLE;
            }
            data->revision=hub.data[kind].revision+1;
            hub.data[kind]=*data;
            xSemaphoreGive(hub.mutex);
        }
#ifdef HUB_AMOLED_216
        save_scale_battery_when_idle();
#endif
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(250));
    }
}
bool gbs_hub_request_scale_refresh(void)
{
    if(!hub.task||xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    int64_t current=now(NULL);
    bool allowed=!hub.busy&&!hub.scale_refresh_queued&&current>=hub.scale_refresh_after;
    if(allowed) {
        hub.scale_refresh_queued=true;
        hub.scale_refresh_after=current+30000;
    }
    xSemaphoreGive(hub.mutex);
    if(allowed) xTaskNotifyGive(hub.task);
    return allowed;
}
bool gbs_hub_request_data(gbs_ui_data_kind_t kind)
{
    if(!hub.task||kind>=GBS_DATA_COUNT||xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    if(!(hub.data_pending&(1u<<kind))) {
        hub.data_pending|=1u<<kind; hub.data_refresh|=1u<<kind;
        hub.data_deadline[kind]=now(NULL)+30000; hub.data_poll[kind]=0;
        hub.data[kind].kind=kind; hub.data[kind].state=GBS_DATA_LOADING; ++hub.data[kind].revision;
    }
    xSemaphoreGive(hub.mutex); xTaskNotifyGive(hub.task); return true;
}
bool gbs_hub_read_data(gbs_ui_data_kind_t kind,gbs_ui_data_t *data)
{
    if(!hub.task||kind>=GBS_DATA_COUNT||xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    bool changed=data->revision!=hub.data[kind].revision;
    if(changed) *data=hub.data[kind];
    xSemaphoreGive(hub.mutex); return changed;
}
bool gbs_hub_submit(gbs_ui_action_t action,double value,uint32_t request_id,const gbs_ui_machine_t *expected)
{
    if(!hub.task||!request_id||!expected||!gbs_ui_machine_action(action)||!isfinite(value)) return false;
    if(xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    bool allowed=!hub.busy&&gbs_ui_can_control(action,&hub.machine)&&
        now(NULL)-hub.machine.observed_ms<GBS_UI_MACHINE_FRESH_MS&&
        gbs_ui_action_unchanged(action,expected,&hub.machine);
    if(allowed) {
        hub.action=action; hub.value=value; hub.request_id=request_id; hub.submitted_machine=*expected;
        hub.deadline=now(NULL)+GBS_UI_CONTROL_WAIT_MS; hub.busy=hub.queued=true;
    }
    xSemaphoreGive(hub.mutex);
    if(allowed) xTaskNotifyGive(hub.task);
    return allowed;
}
bool gbs_hub_submit_schedule(const gbs_ui_schedule_t *expected,const gbs_ui_schedule_t *requested,uint32_t request_id)
{
    if(!hub.task||!expected||!requested||!request_id||xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    bool allowed=!hub.busy&&hub.data[GBS_DATA_SCHEDULE].editable&&hub.data[GBS_DATA_SCHEDULE].state==GBS_DATA_READY;
    if(allowed) {
        hub.schedule_expected=*expected; hub.schedule_requested=*requested; hub.request_id=request_id;
        hub.submitted_machine=hub.machine; hub.deadline=now(NULL)+GBS_UI_CONTROL_WAIT_MS;
        hub.busy=hub.queued=hub.schedule_queued=true;
        hub.data[GBS_DATA_SCHEDULE].editable=false; ++hub.data[GBS_DATA_SCHEDULE].revision;
    }
    xSemaphoreGive(hub.mutex); if(allowed) xTaskNotifyGive(hub.task); return allowed;
}
bool gbs_hub_read(gbs_ui_machine_t *machine,uint32_t *request_id,gbs_ui_control_t *result)
{
    if(!hub.task||xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    *machine=hub.machine; *request_id=hub.completed_id; *result=hub.result;
#ifdef HUB_AMOLED_216
    hub_machine_selection_identity_t selection=hub.selection;
#endif
    xSemaphoreGive(hub.mutex);
#ifdef HUB_AMOLED_216
    if (!hub_config_machine_selection_matches(&selection)) {
        machine->scale_battery_last_known=false;
        machine->scale_battery_last_percent=0;
        machine->scale_battery_last_observed_unix=0;
    }
#endif
    if(machine->available&&now(NULL)-machine->observed_ms>=GBS_UI_MACHINE_FRESH_MS) {
        machine->stale=true; machine->writable=0;
    }
    if(gbs_ui_scale_is_stale(machine,now(NULL))) machine->scale_stale=true;
    return true;
}
bool gbs_hub_running(void) { return hub.task != NULL; }
esp_err_t gbs_hub_start(const char *machine_serial)
{
    (void)machine_serial;
    hub.config=heap_caps_calloc(1,sizeof(*hub.config),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!hub.config) return ESP_ERR_NO_MEM;
    strcpy(hub.config->hub_id,"local");
    hub.machine=(gbs_ui_machine_t){.coffee=NAN,.coffee_target=NAN,.steam=NAN,.steam_target=NAN,.weight=NAN,.dose1=NAN,.dose2=NAN};
    hub.reply=heap_caps_malloc(REPLY_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    hub.data=heap_caps_calloc(GBS_DATA_COUNT+1,sizeof(*hub.data),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    hub.mutex=xSemaphoreCreateMutex();
    if(hub.reply&&hub.data&&hub.mutex)
        hub.task=xTaskCreateStatic(task,"hub_touch_data",sizeof(data_stack),NULL,3,data_stack,&data_task_storage);
    if(!hub.task) {
        free(hub.reply); free(hub.config); free(hub.data);
        if(hub.mutex) vSemaphoreDelete(hub.mutex);
        memset(&hub,0,sizeof(hub)); return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,"Local Hub adapter started; touchscreen uses the shared command coordinator");
    return ESP_OK;
}
