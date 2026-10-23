#!/usr/bin/env python3
"""Exercise production scoped Cloud HTTP lifetime and retry decisions with offline IDF stubs."""
import re
import tempfile
from pathlib import Path
from test_integrated_support import ROOT, compile_run, function_body

HARNESS = r'''

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hub_command_deadline.h"
typedef int esp_err_t;
typedef int esp_http_client_method_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=1, ESP_ERR_NOT_FOUND=2,
       ESP_ERR_HTTP_CONNECT=3, ESP_ERR_HTTP_WRITE_DATA=4, ESP_ERR_HTTP_FETCH_HEADER=5,
       ESP_ERR_HTTP_CONNECTION_CLOSED=6, ESP_ERR_HTTP_EAGAIN=7,
       ESP_ERR_HTTP_READ_TIMEOUT=8, ESP_ERR_HTTP_INCOMPLETE_DATA=9,
       ESP_ERR_TIMEOUT=10, ESP_ERR_INVALID_RESPONSE=11, ESP_ERR_INVALID_STATE=12 };
enum { HTTP_METHOD_GET, HTTP_METHOD_POST };
enum { CLOUD_HTTP_CLIENT_BUFFER_BYTES=8192, HUB_CONFIG_MAX_CLOUD_TOKEN_LEN=1024 };
#define TAG "stub"
#define ESP_RETURN_ON_ERROR(call, tag, msg) do { int e=(call); if(e) return e; } while(0)
typedef struct { char installation_id[40]; } installation_key_t;
typedef struct { char *data; size_t len; } http_response_t;
typedef struct { char access_token[64]; } cloud_tokens_t;
enum { HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_ON_HEADER,
       HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED };
typedef struct { int event_id; void *user_data; const char *header_key,*header_value;
                 void *data; int data_len; } esp_http_client_event_t;
static int http_event_handler(esp_http_client_event_t *e);
static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return clock_us; }
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static size_t heap_caps_get_free_size(int c) { (void)c;return 32000; }
static size_t heap_caps_get_largest_free_block(int c) { (void)c;return 7680; }
static void log_stub(const char *tag,const char *format,...) { (void)tag;(void)format; }
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
static const char *esp_err_to_name(int e) { (void)e;return "error"; }
static bool side_effect_allowed=true;
static bool hub_command_side_effect_allowed(void) { return side_effect_allowed; }
#define pdMS_TO_TICKS(ms) (ms)
static int delays;
static void vTaskDelay(int ms) { ++delays;clock_us+=(int64_t)ms*1000; }
/* ACTUAL TYPES */
static struct { unsigned http_count; cloud_http_timing_t http[16]; } command_timing;
enum { CLOUD_TIMING_HTTP_CAPACITY=16, TIMING_RETRY };
static bool cloud_timing_owned(void) { return true; }
static void cloud_timing_add(int phase,int64_t start) { (void)phase;(void)start; }

static int esp_crt_bundle_attach(void *e) { (void)e; return 0; }
typedef struct {
    const char *url; int method, timeout_ms, buffer_size, buffer_size_tx;
    int (*event_handler)(esp_http_client_event_t *); void *user_data; int (*crt_bundle_attach)(void *);
    bool disable_auto_redirect; int max_authorization_retries;
} esp_http_client_config_t;
typedef struct {
    char key[40], value[1200];
} header_t;
typedef struct client {
    header_t headers[10]; int nheaders, method, timeout_ms, status;
    char url[256]; const char *body; int body_len;
    void *user_data; bool connected, redirects_disabled; int auth_retries;
    int (*event_handler)(esp_http_client_event_t *); size_t content_length;
} *esp_http_client_handle_t;
typedef struct { int error, status; bool server_close; size_t bytes; bool no_headers; } reply_t;
static reply_t plan[16]; static int plan_count, plan_index;
static int init_count, cleanup_count, connect_count, perform_count, posts, signatures;
static bool fail_sign, fail_init, fail_setter, fail_finish, fail_inflate;
static const char *fail_api;
static int api_error(const char *name) { if(fail_api&&!strcmp(name,fail_api)) {fail_api=NULL;return ESP_ERR_NO_MEM;}return 0;}
static char sent_nonce[16][64], sent_body[16][256];
static int sent_method[16], sent_timeout[16];
static void reset(void) {
    memset(plan,0,sizeof(plan)); plan_count=plan_index=0;
    init_count=cleanup_count=connect_count=perform_count=posts=signatures=0;
    fail_sign=fail_init=fail_setter=fail_finish=fail_inflate=false;
    fail_api=NULL;clock_us=1000000;delays=0;side_effect_allowed=true;memset(&command_timing,0,sizeof(command_timing));
    memset(sent_nonce,0,sizeof(sent_nonce)); memset(sent_body,0,sizeof(sent_body));
}
static void reply(int err,int status,bool close,size_t bytes) {
    plan[plan_count++]=(reply_t){.error=err,.status=status,.server_close=close,.bytes=bytes};
}
static int generate_signed_headers(const installation_key_t *k,char *t,size_t tl,
                                  char *n,size_t nl,char *s,size_t sl) {
    (void)k; ++signatures;clock_us+=100;
    snprintf(t,tl,"time-%d",signatures); snprintf(n,nl,"nonce-%d",signatures);
    snprintf(s,sl,"signature-%d",signatures); return fail_sign?ESP_ERR_NO_MEM:ESP_OK;
}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    if(fail_init) return NULL;
    esp_http_client_handle_t p=calloc(1,sizeof(*p)); assert(p); ++init_count;
    snprintf(p->url,sizeof(p->url),"%s",c->url); p->method=c->method;
    p->timeout_ms=c->timeout_ms; p->user_data=c->user_data;p->event_handler=c->event_handler;
    p->redirects_disabled=c->disable_auto_redirect; p->auth_retries=c->max_authorization_retries;
    return p;
}
static int esp_http_client_delete_header(esp_http_client_handle_t c,const char *k) {
    if(api_error("delete_header"))return ESP_ERR_NO_MEM;
    for(int i=0;i<c->nheaders;i++) if(!strcmp(c->headers[i].key,k)) {
        c->headers[i]=c->headers[--c->nheaders]; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
static int esp_http_client_set_header(esp_http_client_handle_t c,const char *k,const char *v) {
    if(fail_setter||api_error("set_header")) return ESP_ERR_NO_MEM;
    if(!v) return esp_http_client_delete_header(c,k);
    int i=0; for(;i<c->nheaders;i++) if(!strcmp(c->headers[i].key,k)) break;
    if(i==c->nheaders) { assert(i<10); c->nheaders++; }
    snprintf(c->headers[i].key,sizeof(c->headers[i].key),"%s",k);
    snprintf(c->headers[i].value,sizeof(c->headers[i].value),"%s",v); return ESP_OK;
}
static const char *header(esp_http_client_handle_t c,const char *k) {
    for(int i=0;i<c->nheaders;i++) if(!strcmp(c->headers[i].key,k)) return c->headers[i].value;
    return NULL;
}
static int esp_http_client_set_post_field(esp_http_client_handle_t c,const char *b,int n) {
    if(api_error(b?"set_body":"clear_body"))return ESP_ERR_NO_MEM;
    c->body=b; c->body_len=b?n:0;
    return b?ESP_OK:esp_http_client_set_header(c,"Content-Type",NULL);
}
static int esp_http_client_set_url(esp_http_client_handle_t c,const char *u) {
    if(api_error("set_url"))return ESP_ERR_NO_MEM;snprintf(c->url,sizeof(c->url),"%s",u); return ESP_OK;
}
static int esp_http_client_set_method(esp_http_client_handle_t c,int m) { if(api_error("set_method"))return ESP_ERR_NO_MEM;c->method=m;return 0; }
static int esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int t) { if(api_error("set_timeout"))return ESP_ERR_NO_MEM;c->timeout_ms=t;return 0; }
static int esp_http_client_set_user_data(esp_http_client_handle_t c,void *d) { if(api_error("set_user_data"))return ESP_ERR_NO_MEM;c->user_data=d;return 0; }
static int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
static int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) {
    return (int64_t)c->content_length;
}
static bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { (void)c;return false; }

static int send_event(esp_http_client_handle_t c,int id,const char *key,const char *value,void *data,int size) {
    clock_us+=100;esp_http_client_event_t e={id,c->user_data,key,value,data,size};return c->event_handler(&e);
}
static int esp_http_client_perform(esp_http_client_handle_t c) {
    assert(plan_index<plan_count && c->user_data); reply_t r=plan[plan_index++];
    int i=perform_count++; sent_method[i]=c->method; sent_timeout[i]=c->timeout_ms;
    assert(header(c,"Content-Type") && !strcmp(header(c,"Content-Type"),"application/json"));
    snprintf(sent_nonce[i],sizeof(sent_nonce[i]),"%s",header(c,"X-Nonce"));
    snprintf(sent_body[i],sizeof(sent_body[i]),"%s",c->body?c->body:"");
    if(!c->connected) {
        ++connect_count;if(r.error==ESP_ERR_HTTP_CONNECT)return r.error;
        c->connected=true;send_event(c,HTTP_EVENT_ON_CONNECTED,NULL,NULL,NULL,0);
    }
    if(c->method==HTTP_METHOD_POST) ++posts;
    /* IDF preserves the previous status on write errors before fetch_headers. */
    if(r.error==ESP_ERR_HTTP_WRITE_DATA)return r.error;
    send_event(c,HTTP_EVENT_HEADERS_SENT,NULL,NULL,NULL,0);c->status=-1;
    if(r.status) {
        if(!r.no_headers)send_event(c,HTTP_EVENT_ON_HEADER,"Connection",r.server_close?"close":"keep-alive",NULL,0);
        c->status=r.status;
        /* A deliberately hostile stub would replay if the required flags were absent. */
        if((r.status>=300&&r.status<=308&&!c->redirects_disabled) ||
           (r.status==401&&c->auth_retries!=-1)) { ++posts;++perform_count; }
    }
    c->content_length=r.bytes;
    if(r.bytes) {
        char data[64];assert(r.bytes<sizeof(data));memset(data,'x',r.bytes);
        send_event(c,HTTP_EVENT_ON_DATA,NULL,NULL,data,(int)r.bytes);
    }
    if(!r.error)send_event(c,HTTP_EVENT_ON_FINISH,NULL,NULL,NULL,0);
    if(r.server_close) { c->connected=false;send_event(c,HTTP_EVENT_DISCONNECTED,NULL,NULL,NULL,0); }
    return r.error;
}
static int esp_http_client_cleanup(esp_http_client_handle_t c) {
    if(c->redirects_disabled)assert(!c->user_data&&!c->body);
    send_event(c,HTTP_EVENT_DISCONNECTED,NULL,NULL,NULL,0);++cleanup_count;free(c);return 0;
}
static int hub_http_response_append(http_response_t *r,const void *data,size_t n) {
    r->data=realloc(r->data,r->len+n+1);assert(r->data);memcpy(r->data+r->len,data,n);r->len+=n;r->data[r->len]=0;return ESP_OK;
}
static void hub_http_response_reset(http_response_t *r) { free(r->data);memset(r,0,sizeof(*r)); }
static int hub_http_response_finish(http_response_t *r,int64_t n,bool chunked) {
    (void)r;(void)n;(void)chunked;return fail_finish?ESP_ERR_INVALID_RESPONSE:ESP_OK;
}
static int inflate_http_response_if_needed(http_response_t *r) {
    (void)r;return fail_inflate?ESP_ERR_INVALID_RESPONSE:ESP_OK;
}

/* ACTUAL FUNCTIONS */

static const installation_key_t key={"SYNTHETIC"};
static const cloud_tokens_t tokens={"synthetic-token"};
static int run_get(esp_http_client_handle_t *s) {
    http_response_t r={0};int status=0;bool started=false;
    int err=cloud_http_request_no_retry("https://example.test/dashboard",HTTP_METHOD_GET,&key,
        tokens.access_token,NULL,&r,&status,4000,s,&started);
    hub_http_response_reset(&r);if(!err)assert(status==200&&started);return err;
}
static hub_cloud_command_attempt_t run_post(esp_http_client_handle_t *s,const hub_command_deadline_t *deadline) {
    return cloud_execute_url(&key,&tokens,"https://example.test/command","synthetic","{\"mode\":\"Dose1\"}",deadline,0,s);
}
static void fresh_get(esp_http_client_handle_t *s,bool close) {
    reply(0,200,close,10);assert(!run_get(s));
}
static void lifetime(void) {
    esp_http_client_handle_t s=NULL;reset();fresh_get(NULL,false);reply(0,200,false,2);
    hub_cloud_command_attempt_t a=run_post(NULL,NULL);
    assert(!a.err&&a.request_started&&init_count==2&&connect_count==2&&cleanup_count==2&&posts==1);
    assert(!command_timing.http[0].reused_handle&&!command_timing.http[1].reused_handle);
    reset();fresh_get(&s,false);assert(s&&!s->user_data&&!s->body);
    assert(s->redirects_disabled&&s->auth_retries==-1);reply(0,200,false,2);a=run_post(&s,NULL);
    assert(!a.err&&a.http_status==200&&a.request_started&&a.attempts==1);
    assert(init_count==1&&connect_count==1&&cleanup_count==0&&posts==1);
    assert(strcmp(sent_nonce[0],sent_nonce[1])&&!sent_body[0][0]&&sent_body[1][0]);
    assert(command_timing.http[0].connection_events==1&&!command_timing.http[0].reused_handle);
    assert(command_timing.http[1].reused_handle&&command_timing.http[1].connection_events==0);
    assert(command_timing.http[1].connect_us==-1&&command_timing.http[1].headers_us>=0&&command_timing.http[1].first_response_us>=0);
    cloud_http_session_cleanup(&s);assert(!s&&cleanup_count==1);
    reset();fresh_get(&s,true);reply(0,200,false,2);a=run_post(&s,NULL);
    assert(!a.err&&init_count==1&&connect_count==2&&posts==1);
    assert(command_timing.http[1].reused_handle&&command_timing.http[1].connection_events==1);
    cloud_http_session_cleanup(&s);
}
static void no_replay(void) {
    const int faults[]={ESP_ERR_HTTP_WRITE_DATA,ESP_ERR_HTTP_FETCH_HEADER,ESP_ERR_HTTP_CONNECTION_CLOSED,
        ESP_ERR_HTTP_EAGAIN,ESP_ERR_HTTP_READ_TIMEOUT,ESP_ERR_HTTP_INCOMPLETE_DATA,ESP_ERR_TIMEOUT};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);i++) {
        esp_http_client_handle_t s=NULL;reset();fresh_get(&s,false);reply(faults[i],0,false,0);
        hub_cloud_command_attempt_t a=run_post(&s,NULL);
        assert(a.err==faults[i]&&a.request_started&&a.attempts==1&&a.http_status==0);
        assert(!s&&posts==1&&perform_count==2&&cleanup_count==1&&delays==0);
        assert(command_timing.http[1].status==0);
    }
    /* A real 200 response followed by a truncated body remains an ambiguous single POST. */
    esp_http_client_handle_t s=NULL;reset();fresh_get(&s,false);reply(ESP_ERR_HTTP_INCOMPLETE_DATA,200,false,1);
    hub_cloud_command_attempt_t a=run_post(&s,NULL);
    assert(a.err&&a.http_status==200&&a.request_started&&posts==1&&!s);
    int redirects[]={301,302,303,307,308};
    for(unsigned i=0;i<sizeof(redirects)/sizeof(redirects[0]);i++) {
        reset();fresh_get(&s,false);reply(0,redirects[i],false,2);a=run_post(&s,NULL);
        assert(a.err==ESP_ERR_INVALID_RESPONSE&&a.http_status==redirects[i]&&posts==1&&perform_count==2);
        cloud_http_session_cleanup(&s);
    }
    for(int headerless=0;headerless<2;headerless++) {
        reset();fresh_get(&s,false);reply(ESP_FAIL,401,false,0);plan[plan_count-1].no_headers=headerless;
        a=run_post(&s,NULL);assert(a.err==ESP_FAIL&&a.http_status==401&&posts==1&&!s&&delays==0);
    }
    reset();fresh_get(&s,false);reply(0,403,false,2);a=run_post(&s,NULL);
    assert(a.err==ESP_ERR_INVALID_RESPONSE&&a.http_status==403&&posts==1);cloud_http_session_cleanup(&s);
}
static void connect_only_retry(void) {
    esp_http_client_handle_t s=NULL;reset();fresh_get(&s,true);
    reply(ESP_ERR_HTTP_CONNECT,0,false,0);reply(0,200,false,2);
    hub_cloud_command_attempt_t a=run_post(&s,NULL);
    assert(!a.err&&a.request_started&&a.attempts==2&&posts==1&&delays==1&&init_count==2);
    assert(command_timing.http[1].reused_handle&&command_timing.http[1].status==0);
    assert(!command_timing.http[2].reused_handle&&command_timing.http[2].connection_events==1);
    cloud_http_session_cleanup(&s);
    reset();fresh_get(&s,true);reply(ESP_ERR_HTTP_CONNECT,0,false,0);reply(ESP_ERR_HTTP_CONNECT,0,false,0);
    a=run_post(&s,NULL);assert(a.err==ESP_ERR_HTTP_CONNECT&&!a.request_started&&a.attempts==2&&posts==0&&!s&&delays==1);
    /* The existing deadline prevents a connect retry, and never grows. */
    reset();fresh_get(&s,true);reply(ESP_ERR_HTTP_CONNECT,0,false,0);
    hub_command_deadline_t d={.enforced=true,.deadline_at_ms=clock_us/1000+500};
    a=run_post(&s,&d);assert(a.err==ESP_ERR_TIMEOUT&&!a.request_started&&posts==0&&delays==0);cloud_http_session_cleanup(&s);
}
static void setup_and_cleanup(void) {
    const char *apis[]={"set_url","set_method","set_timeout","set_user_data","clear_body","delete_header","set_header","set_body"};
    for(unsigned i=0;i<sizeof(apis)/sizeof(apis[0]);i++) {
        esp_http_client_handle_t s=NULL;reset();fresh_get(&s,false);fail_api=apis[i];
        hub_cloud_command_attempt_t a=run_post(&s,NULL);
        assert(a.err==ESP_ERR_NO_MEM&&!a.request_started&&a.attempts==0&&posts==0&&!s&&cleanup_count==1);
    }
    esp_http_client_handle_t s=NULL;
    reset();fresh_get(&s,false);fail_sign=true;hub_cloud_command_attempt_t a=run_post(&s,NULL);
    assert(a.err==ESP_ERR_NO_MEM&&!a.request_started&&!a.attempts&&!s&&posts==0);
    reset();fail_init=true; a=run_post(&s,NULL);assert(a.err==ESP_ERR_NO_MEM&&!a.request_started&&!a.attempts&&!s);
    for(int inflate=0;inflate<2;inflate++) {
        reset();fresh_get(&s,false);reply(0,200,false,2);fail_inflate=inflate;fail_finish=!inflate;
        a=run_post(&s,NULL);assert(a.err&&a.request_started&&posts==1&&!s&&cleanup_count==1);
    }
    reset();fresh_get(&s,false);side_effect_allowed=false;a=run_post(&s,NULL);
    assert(a.err==ESP_ERR_INVALID_STATE&&!a.request_started&&posts==0);cloud_http_session_cleanup(&s);
    reset();reply(ESP_ERR_HTTP_FETCH_HEADER,0,false,0);assert(run_get(&s)==ESP_ERR_HTTP_FETCH_HEADER&&!s);
    fresh_get(&s,false);assert(init_count==2&&posts==0);cloud_http_session_cleanup(&s);
    reset();fresh_get(&s,false);reply(0,200,false,2);a=run_post(&s,NULL);assert(!a.err);
    reply(0,200,false,10);assert(!run_get(&s)&&!sent_body[2][0]);cloud_http_session_cleanup(&s);
    /* Legacy setup failure classification remains unchanged by the opt-in path. */
    reset();fail_sign=true;a=run_post(NULL,NULL);assert(a.err==ESP_ERR_NO_MEM&&a.request_started&&a.attempts==1&&posts==0);
}
int main(void) {
    lifetime();no_replay();connect_only_retry();setup_and_cleanup();
    puts("Scoped Cloud connection reuse PASS: actual helper lifetime, timing, setter failures, status isolation, bounded connect retry and no ambiguous POST replay");
    return 0;
}


'''


def main():
    source=(ROOT/'main/hub_cloud.c').read_text()
    header=(ROOT/'main/hub_cloud.h').read_text()
    names=['cloud_http_timing_t','cloud_http_event_context_t']
    types='\n'.join(re.search(r'typedef struct \{[^}]+\} '+name+r';',source).group() for name in names)
    types+='\n'+re.search(r'typedef struct \{[^}]+\} hub_cloud_command_attempt_t;',header).group()
    for name in ['CLOUD_COMMAND_HTTP_TIMEOUT_MS','CLOUD_COMMAND_CONNECT_RETRY_DELAY_MS']:
        types+='\n'+re.search(r'static const [^;\n]+\b'+name+r'[^;]+;',source).group()
    functions=[]
    for name in ['http_event_handler','cloud_http_request_once','cloud_http_request_no_retry',
                 'cloud_http_session_cleanup','http_response_first_bytes',
                 'cloud_command_connect_failed_before_request','cloud_execute_url']:
        signature=re.search(r'^(?:static )?[^\n;{}]+\b'+name+r'\([^;{]*\)\s*\{',source,re.M)
        assert signature,name
        functions.append(signature.group()+function_body(source,name)+'}')
    harness=HARNESS.replace('/* ACTUAL TYPES */',types).replace('/* ACTUAL FUNCTIONS */','\n'.join(functions))
    with tempfile.TemporaryDirectory(prefix='cloud-reuse-host-') as temp:
        compile_run(Path(temp),'reuse',harness,[ROOT/'main/hub_command_deadline.c','-fno-sanitize-recover=all'])

if __name__=='__main__':main()
