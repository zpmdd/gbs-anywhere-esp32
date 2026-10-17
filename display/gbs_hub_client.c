#include "gbs_hub_client.h"
#include "gbs_hub.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
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
#include <time.h>

enum { REPLY_SIZE=32768 };
static const char *TAG="gbs_hub";
static struct {
    gbs_hub_config_t *config;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    esp_http_client_handle_t http;
    gbs_ui_machine_t machine, submitted_machine;
    gbs_ui_action_t action;
    gbs_ui_control_t result;
    uint32_t request_id, completed_id;
    double value;
    int64_t deadline;
    bool busy, queued, response_failed;
    char *reply;
    size_t used;
    char common_name[80];
} hub;

static int64_t now(void *context) { (void)context; return esp_timer_get_time()/1000; }
static void wait_ms(void *context,unsigned ms) { (void)context; vTaskDelay(pdMS_TO_TICKS(ms)); }
static esp_err_t response(esp_http_client_event_t *event)
{
    if(event->event_id==HTTP_EVENT_ON_DATA&&event->data_len>0) {
        if(hub.response_failed||(size_t)event->data_len>=REPLY_SIZE-hub.used) {
            hub.response_failed=true; return ESP_ERR_NO_MEM;
        }
        memcpy(hub.reply+hub.used,event->data,(size_t)event->data_len);
        hub.used+=(size_t)event->data_len; hub.reply[hub.used]=0;
    }
    return ESP_OK;
}
static int exchange(void *context,const char *path,const char *body,char *reply,size_t size,int64_t deadline)
{
    (void)context;
    int64_t remaining=deadline-now(NULL);
    if(remaining<=0||reply!=hub.reply||size!=REPLY_SIZE) return -1;
    char url[272]; int written=snprintf(url,sizeof(url),"%s%s",hub.config->url,path);
    if(written<=0||(size_t)written>=sizeof(url)) return -1;
    if(!hub.http) {
        snprintf(hub.common_name,sizeof(hub.common_name),"LamaBuddy Hub %s",hub.config->hub_id);
        const esp_http_client_config_t config={.url=url,.cert_pem=hub.config->certificate,
            .common_name=hub.common_name,.event_handler=response,.disable_auto_redirect=true,
            .keep_alive_enable=true,.max_authorization_retries=-1,.buffer_size=1024,.buffer_size_tx=1024};
        hub.http=esp_http_client_init(&config);
        if(!hub.http) return -1;
        char authorization[144]; snprintf(authorization,sizeof(authorization),"Bearer %s",hub.config->token);
        esp_err_t err=esp_http_client_set_header(hub.http,"Authorization",authorization);
        memset(authorization,0,sizeof(authorization));
        if(err==ESP_OK) err=esp_http_client_set_header(hub.http,"X-LamaBuddy-API-Version","2");
        if(err==ESP_OK) err=esp_http_client_set_header(hub.http,"X-LamaBuddy-Machine-Serial",hub.config->serial);
        if(err==ESP_OK) err=esp_http_client_set_header(hub.http,"Content-Type","application/json");
        if(err==ESP_OK) err=esp_http_client_set_header(hub.http,"Accept","application/json");
        if(err!=ESP_OK) { esp_http_client_cleanup(hub.http); hub.http=NULL; return -1; }
    }
    hub.used=0; hub.response_failed=false; reply[0]=0;
    esp_err_t err=esp_http_client_set_url(hub.http,url);
    if(err==ESP_OK) err=esp_http_client_set_method(hub.http,body?HTTP_METHOD_POST:HTTP_METHOD_GET);
    if(err==ESP_OK) err=esp_http_client_set_post_field(hub.http,body,body?(int)strlen(body):0);
    remaining=deadline-now(NULL);
    if(remaining<=0) err=ESP_ERR_TIMEOUT;
    if(err==ESP_OK) err=esp_http_client_set_timeout_ms(hub.http,(int)(remaining>3000?3000:remaining));
    if(err==ESP_OK) err=esp_http_client_perform(hub.http);
    if(err!=ESP_OK||hub.response_failed||now(NULL)>=deadline||!esp_http_client_is_complete_data_received(hub.http)) {
        esp_http_client_cleanup(hub.http); hub.http=NULL; return -1;
    }
    return esp_http_client_get_status_code(hub.http);
}
static void request_uuid(char out[37])
{
    unsigned char bytes[16]; esp_fill_random(bytes,sizeof(bytes));
    bytes[6]=(bytes[6]&15)|64; bytes[8]=(bytes[8]&63)|128;
    snprintf(out,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
}
static void task(void *unused)
{
    (void)unused;
    const gbs_hub_io_t io={.exchange=exchange,.now=now,.wait=wait_ms,.reply=hub.reply,.reply_size=REPLY_SIZE};
    int64_t next_poll=0;
    for(;;) {
        xSemaphoreTake(hub.mutex,portMAX_DELAY);
        bool queued=hub.queued; hub.queued=false;
        gbs_ui_machine_t machine=queued?hub.submitted_machine:hub.machine;
        gbs_ui_action_t action=hub.action; double value=hub.value;
        uint32_t request_id=hub.request_id; int64_t deadline=hub.deadline;
        xSemaphoreGive(hub.mutex);
        esp_netif_t *wifi=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        bool online=wifi&&esp_netif_is_netif_up(wifi)&&time(NULL)>=1735689600;
        if(queued) {
            char id[37]; request_uuid(id);
            gbs_ui_control_t result=online?gbs_hub_execute(hub.config,&io,action,value,id,deadline,&machine):GBS_UI_CONTROL_FAILED;
            xSemaphoreTake(hub.mutex,portMAX_DELAY);
            hub.machine=machine; hub.result=result; hub.completed_id=request_id; hub.busy=false;
            xSemaphoreGive(hub.mutex);
            next_poll=now(NULL)+1000;
        } else if(now(NULL)>=next_poll) {
            if(!online||!gbs_hub_refresh(hub.config,&io,now(NULL)+12000,&machine)) machine.stale=true;
            xSemaphoreTake(hub.mutex,portMAX_DELAY); hub.machine=machine; xSemaphoreGive(hub.mutex);
            next_poll=now(NULL)+5000;
        }
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(250));
    }
}
bool gbs_hub_submit(gbs_ui_action_t action,double value,uint32_t request_id,const gbs_ui_machine_t *expected)
{
    if(!hub.task||!request_id||!expected||!gbs_ui_machine_action(action)||!isfinite(value)) return false;
    if(xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    bool allowed=!hub.busy&&hub.machine.available&&!hub.machine.stale&&
        now(NULL)-hub.machine.observed_ms<GBS_UI_MACHINE_FRESH_MS&&(hub.machine.writable&(1u<<action))&&
        gbs_ui_action_unchanged(action,expected,&hub.machine);
    if(allowed) {
        hub.action=action; hub.value=value; hub.request_id=request_id; hub.submitted_machine=*expected;
        hub.deadline=now(NULL)+GBS_UI_CONTROL_WAIT_MS; hub.busy=hub.queued=true;
    }
    xSemaphoreGive(hub.mutex);
    if(allowed) xTaskNotifyGive(hub.task);
    return allowed;
}
bool gbs_hub_read(gbs_ui_machine_t *machine,uint32_t *request_id,gbs_ui_control_t *result)
{
    if(!hub.task||xSemaphoreTake(hub.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return false;
    *machine=hub.machine; *request_id=hub.completed_id; *result=hub.result;
    xSemaphoreGive(hub.mutex);
    if(machine->available&&now(NULL)-machine->observed_ms>=GBS_UI_MACHINE_FRESH_MS) machine->stale=true;
    return true;
}
esp_err_t gbs_hub_start(const char *machine_serial)
{
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("gbs_hub",NVS_READONLY,&nvs);
    if(err==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if(err!=ESP_OK) return err;
    size_t size=0; err=nvs_get_str(nvs,"config",NULL,&size);
    if(err!=ESP_OK||size<2||size>8192) { nvs_close(nvs); return ESP_ERR_INVALID_SIZE; }
    char *json=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    hub.config=heap_caps_calloc(1,sizeof(*hub.config),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!json||!hub.config) err=ESP_ERR_NO_MEM;
    else err=nvs_get_str(nvs,"config",json,&size);
    nvs_close(nvs);
    if(err==ESP_OK&&!gbs_hub_config_parse(json,hub.config)) err=ESP_ERR_INVALID_ARG;
    if(err==ESP_OK&&(!machine_serial||strcmp(machine_serial,hub.config->serial))) err=ESP_ERR_INVALID_ARG;
    if(json) { memset(json,0,size); free(json); }
    if(err!=ESP_OK) { free(hub.config); hub.config=NULL; return err; }
    hub.machine=(gbs_ui_machine_t){.coffee=NAN,.coffee_target=NAN,.steam=NAN,.steam_target=NAN,.weight=NAN,.dose1=NAN,.dose2=NAN};
    hub.reply=heap_caps_malloc(REPLY_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    hub.mutex=xSemaphoreCreateMutex();
    if(!hub.reply||!hub.mutex||xTaskCreate(task,"gbs_hub",16384,NULL,3,&hub.task)!=pdPASS) {
        free(hub.reply); free(hub.config);
        if(hub.mutex) vSemaphoreDelete(hub.mutex);
        memset(&hub,0,sizeof(hub)); return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,"Configured Hub client started; machine control uses authenticated HTTPS v2");
    return ESP_OK;
}
