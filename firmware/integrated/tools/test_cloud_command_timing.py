#!/usr/bin/env python3
"""Exercise the actual bounded Cloud timing collector, HTTP events and JSON offline."""

import os
from pathlib import Path
import re
import tempfile

from test_integrated_support import ERRORS, ROOT, compile_run, function_body


HARNESS = r'''
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "hub_http_response.h"
#define EXT_RAM_BSS_ATTR
#define HUB_COMMAND_REQUEST_ID_CAPACITY 37
#define HUB_CONFIG_MAX_CLOUD_TOKEN_LEN 64
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define CLOUD_HTTP_CLIENT_BUFFER_BYTES 8192
#define ESP_LOGE(...) ((void)0)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static bool locked;
#define portENTER_CRITICAL(lock) do { (void)(lock);assert(!locked);locked=true; } while(0)
#define portEXIT_CRITICAL(lock) do { (void)(lock);assert(locked);locked=false; } while(0)
static uintptr_t task=1;
static int64_t clock_us;
static uintptr_t xTaskGetCurrentTaskHandle(void) { return task; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool hub_command_request_id_valid(const char *id) { return id&&strlen(id)==36; }
static const char *esp_err_to_name(int error) { return error==ESP_OK?"ESP_OK":"ESP_ERR_INVALID_STATE"; }
static size_t heap_caps_get_free_size(int caps) { (void)caps;assert(!locked);return 50000; }
static size_t heap_caps_get_largest_free_block(int caps) { (void)caps;assert(!locked);return 16000; }
typedef hub_http_response_t http_response_t;
typedef struct { char installation_id[40]; } installation_key_t;
typedef int esp_http_client_method_t;
enum { HTTP_METHOD_GET,HTTP_METHOD_POST };
enum { HTTP_EVENT_ON_CONNECTED,HTTP_EVENT_HEADERS_SENT,HTTP_EVENT_ON_HEADER,HTTP_EVENT_ON_DATA };
typedef struct { int event_id;void *user_data;const char *header_key,*header_value;void *data;int data_len; } esp_http_client_event_t;
typedef struct {
    const char *url;int method,timeout_ms;esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;void (*crt_bundle_attach)(void);int buffer_size,buffer_size_tx;
    bool disable_auto_redirect;int max_authorization_retries;
} esp_http_client_config_t;
typedef esp_http_client_config_t *esp_http_client_handle_t;
static int sign_error;
static bool init_error,skip_connected;
static esp_http_client_config_t client;
static void esp_crt_bundle_attach(void) {}
static esp_err_t generate_signed_headers(const installation_key_t *key,char *stamp,size_t sn,char *nonce,size_t nn,char *signature,size_t sig_n)
{ (void)key;(void)stamp;(void)sn;(void)nonce;(void)nn;(void)signature;(void)sig_n;clock_us+=100;return sign_error; }
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{ clock_us+=200;client=*config;return init_error?NULL:&client; }
static esp_err_t esp_http_client_set_url(esp_http_client_handle_t c,const char *url) { c->url=url;return ESP_OK; }
static esp_err_t esp_http_client_set_method(esp_http_client_handle_t c,int method) { c->method=method;return ESP_OK; }
static esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int timeout) { c->timeout_ms=timeout;return ESP_OK; }
static esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c,void *data) { c->user_data=data;return ESP_OK; }
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value) { (void)c;(void)key;(void)value;return ESP_OK; }
static esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c,const char *key) { (void)c;(void)key;return ESP_OK; }
static esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,const char *body,size_t size) { (void)c;(void)body;(void)size;return ESP_OK; }
static esp_err_t send_event(esp_http_client_handle_t c,int id,const char *key,const char *value,void *data,int size)
{ esp_http_client_event_t event={id,c->user_data,key,value,data,size};return c->event_handler(&event); }
static esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    clock_us+=300;
    if(!skip_connected) assert(send_event(c,HTTP_EVENT_ON_CONNECTED,NULL,NULL,NULL,0)==ESP_OK);
    clock_us+=400;assert(send_event(c,HTTP_EVENT_HEADERS_SENT,NULL,NULL,NULL,0)==ESP_OK);
    clock_us+=500;assert(send_event(c,HTTP_EVENT_ON_HEADER,"Authorization","SECRET_HEADER",NULL,0)==ESP_OK);
    assert(send_event(c,HTTP_EVENT_ON_HEADER,"Connection","keep-alive",NULL,0)==ESP_OK);
    clock_us+=600;return send_event(c,HTTP_EVENT_ON_DATA,NULL,NULL,"ok",2);
}
static int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { (void)c;return 2; }
static bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { (void)c;return false; }
static int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c;return 200; }
static void esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c;clock_us+=100; }
esp_err_t hub_http_response_append(http_response_t *r,const void *data,size_t len)
{ if(!r)return ESP_ERR_INVALID_ARG;r->data=realloc(r->data,r->len+len+1);assert(r->data);memcpy(r->data+r->len,data,len);r->len+=len;r->data[r->len]=0;return ESP_OK; }
esp_err_t hub_http_response_finish(const http_response_t *r,int64_t expected,bool chunked)
{ (void)chunked;return r&&r->len==(size_t)expected?ESP_OK:ESP_ERR_INVALID_STATE; }
static esp_err_t inflate_http_response_if_needed(http_response_t *r) { (void)r;return ESP_OK; }
/* TYPES AND COLLECTOR */
static readonly_http_diag_t readonly_http_diags[READONLY_DIAG_COUNT];
static portMUX_TYPE readonly_http_diag_lock;
/* FUNCTIONS */
static const char *ID1="00000000-0000-4000-8000-000000000001";
static const char *ID2="00000000-0000-4000-8000-000000000002";
static cJSON *json(void)
{ char body[4096];assert(cloud_command_timing_json(body,sizeof(body))&&!locked);cJSON *j=cJSON_Parse(body);assert(j);return j; }
static cJSON *field(cJSON *j,const char *name) { cJSON *v=cJSON_GetObjectItemCaseSensitive(j,name);assert(v);return v; }
static void request_once(void)
{
    installation_key_t key={.installation_id="SECRET_IDENTITY"};http_response_t response={0};int status=0;
    esp_err_t err=cloud_http_request_once("https://SECRET_URL",HTTP_METHOD_POST,&key,"SECRET_TOKEN","{}",&response,&status,1000,NULL,NULL);
    assert(err==(sign_error?sign_error:init_error?ESP_ERR_NO_MEM:ESP_OK));free(response.data);
}
static void ownership(void)
{
    cJSON *j=json();assert(cJSON_IsFalse(field(j,"available")));cJSON_Delete(j);
    hub_cloud_command_timing_begin("invalid");assert(!cloud_timing_owned());
    hub_cloud_command_timing_begin(ID1);clock_us+=1200;cloud_timing_add(TIMING_LOCK,clock_us-1200);
    task=2;cloud_timing_add(TIMING_HEAP,0);request_once();hub_cloud_command_timing_end();
    j=json();assert(cJSON_IsFalse(field(j,"available")));cJSON_Delete(j);
    task=1;hub_cloud_command_timing_end();assert(!cloud_timing_owned());
    j=json();assert(!strcmp(field(j,"requestId")->valuestring,ID1));assert(field(j,"lockWaitMs")->valuedouble==1.2);
    assert(cJSON_IsNull(field(j,"heapWaitMs"))&&field(j,"httpCount")->valueint==0);cJSON_Delete(j);
}
static void event_privacy(void)
{
    http_response_t response={0};
    cloud_http_event_context_t context={.response=&response,.perform_started_us=clock_us,
        .timing={.connect_us=-1,.headers_us=-1,.first_response_us=-1,.connection_close=-1}};
    esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_HEADER,.user_data=&context,.header_key="Authorization",.header_value="close"};
    clock_us+=100;assert(http_event_handler(&event)==ESP_OK&&context.timing.connection_close==-1);
    event.header_key="cOnNeCtIoN";event.header_value="cLoSe";assert(http_event_handler(&event)==ESP_OK&&context.timing.connection_close==1);
    event.header_value="keep-alive";assert(http_event_handler(&event)==ESP_OK&&context.timing.connection_close==0);
    event.header_key="X-Other";event.header_value="SECRET_HEADER";assert(http_event_handler(&event)==ESP_OK&&context.timing.connection_close==0);
    event.event_id=HTTP_EVENT_ON_CONNECTED;clock_us+=200;http_event_handler(&event);int first=context.timing.connect_us;
    clock_us+=200;http_event_handler(&event);assert(context.timing.connect_us==first&&context.timing.connection_events==2);
    event.event_id=HTTP_EVENT_ON_DATA;event.data="ok";event.data_len=2;assert(http_event_handler(&event)==ESP_OK&&response.len==2);free(response.data);
    event.user_data=NULL;assert(http_event_handler(&event)==ESP_OK);
}
static void pipeline_and_bounds(void)
{
    hub_cloud_command_timing_begin(ID2);request_once();skip_connected=true;request_once();skip_connected=false;
    sign_error=ESP_ERR_INVALID_STATE;request_once();sign_error=0;init_error=true;request_once();init_error=false;request_once();
    hub_cloud_command_timing_end();cJSON *j=json();assert(!strcmp(field(j,"requestId")->valuestring,ID2));
    assert(field(j,"httpCount")->valueint==5&&cJSON_IsTrue(field(j,"httpTruncated")));
    cJSON *http=field(j,"http");assert(cJSON_GetArraySize(http)==4);
    cJSON *first=cJSON_GetArrayItem(http,0);assert(field(first,"connectMs")->valuedouble==0.3);
    assert(field(first,"headersSentMs")->valuedouble==0.7&&field(first,"firstResponseMs")->valuedouble==1.2);
    assert(field(first,"performMs")->valuedouble==1.8&&cJSON_IsFalse(field(first,"connectionClose")));
    assert(cJSON_IsFalse(field(first,"reusedHandle"))&&field(first,"connectionEvents")->valueint==1);
    cJSON *unobserved=cJSON_GetArrayItem(http,1);
    assert(cJSON_IsNull(field(unobserved,"connectMs"))&&cJSON_IsFalse(field(unobserved,"reusedHandle")));
    assert(field(unobserved,"connectionEvents")->valueint==0);
    cJSON *sign=cJSON_GetArrayItem(http,2);assert(cJSON_IsNull(field(sign,"initMs"))&&cJSON_IsNull(field(sign,"performMs")));
    cJSON *init=cJSON_GetArrayItem(http,3);assert(cJSON_IsNumber(field(init,"initMs"))&&cJSON_IsNull(field(init,"performMs")));cJSON_Delete(j);
    const char *full=hub_cloud_readonly_diagnostics_json();assert(!strstr(full,"SECRET_")&&!strstr(full,"Authorization")&&!strstr(full,"https://"));
    j=cJSON_Parse(full);assert(j&&field(j,"commandTiming"));cJSON_Delete(j);
    for(unsigned i=0;i<TIMING_PHASE_COUNT;++i)completed_timing.phase_us[i]=INT32_MAX;
    completed_timing.phases=(1u<<TIMING_PHASE_COUNT)-1;completed_timing.execution_us=UINT32_MAX;completed_timing.completed_ms=INT64_MAX;
    completed_timing.http_count=UINT32_MAX;
    for(unsigned i=0;i<CLOUD_TIMING_HTTP_CAPACITY;++i) {
        cloud_http_timing_t *h=&completed_timing.http[i];h->sign_us=h->internal_free_before=h->internal_largest_before=UINT32_MAX;
        h->connect_us=h->headers_us=h->first_response_us=h->init_us=h->perform_us=h->finish_us=INT32_MAX;h->status=INT_MAX;h->error=ESP_FAIL;
        h->connection_events=UINT32_MAX;h->reused_handle=false;
    }
    for(unsigned i=0;i<READONLY_DIAG_COUNT;++i)readonly_http_diags[i]=(readonly_http_diag_t){.err=ESP_FAIL,.status_code=INT_MAX,.duration_ms=INT_MAX,.response_bytes=SIZE_MAX,.first_bytes=UINT32_MAX,.free_heap=UINT32_MAX,.internal_largest=UINT32_MAX,.attempts=UINT32_MAX};
    full=hub_cloud_readonly_diagnostics_json();j=cJSON_Parse(full);assert(j&&field(j,"commandTiming"));cJSON_Delete(j);
    printf("Cloud timing maximum synthetic diagnostic: %zu bytes\n",strlen(full));
    struct { char buffer[8];char guard[8]; } small;memset(&small,'!',sizeof(small));
    assert(!cloud_command_timing_json(small.buffer,sizeof(small.buffer)));
    for(unsigned i=0;i<sizeof(small.guard);++i)assert(small.guard[i]=='!');
    assert(!cloud_command_timing_json(small.buffer,0));
}
int main(void) { ownership();event_privacy();pipeline_and_bounds();assert(!locked);return 0; }
'''


def main():
    source = (ROOT / "main/hub_cloud.c").read_text()
    diag_types = source[source.index("typedef enum {\n    READONLY_DIAG_THINGS"):source.index("typedef struct {\n    char access_token")]
    collector = source[source.index("typedef enum {\n    TIMING_READY"):source.index("static void copy_snapshot(")]
    functions = []
    for name in ["readonly_diag_name", "http_event_handler", "cloud_http_request_once", "timing_millis_text", "cloud_command_timing_json", "hub_cloud_readonly_diagnostics_json"]:
        signature = re.search(r"^(?:static )?[^\n;{}]+\b" + name + r"\([^;{]*\)\s*\{", source, re.M)
        assert signature, name
        functions.append(signature.group() + function_body(source, name) + "}")
    json_dir = Path(os.environ["IDF_PATH"]) / "components/json/cJSON"
    with tempfile.TemporaryDirectory(prefix="cloud-timing-host-") as temporary:
        directory = Path(temporary)
        (directory / "esp_err.h").write_text(ERRORS.replace("const char *esp_err_to_name(esp_err_t error);", ""))
        harness = HARNESS.replace("/* TYPES AND COLLECTOR */", diag_types + collector).replace("/* FUNCTIONS */", "\n".join(functions))
        compile_run(directory, "timing", harness, ["-I", json_dir, json_dir / "cJSON.c", "-Wno-deprecated-declarations", "-fno-sanitize-recover=all"])
    print("Cloud command timing PASS: task ownership, request scope, actual HTTP events, missing phases, four-record bound, JSON capacity and header privacy")


if __name__ == "__main__":
    main()
