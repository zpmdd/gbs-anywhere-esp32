#!/usr/bin/env python3
"""Exercise production history/statistics reads with failed trends and busy Cloud IO."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

from host_cjson import cjson_source_dir
from test_quick_weight_control import ESP_ERR_STUB
from validate_stability_foundation import function_body

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "hub_cloud.h"
#include "cJSON.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static const char *CUSTOMER_APP_URL="https://synthetic.invalid";
enum { READONLY_DIAG_COUNTER, READONLY_DIAG_RECENT, READONLY_DIAG_TREND,
       HTTP_METHOD_GET, CLOUD_STATISTICS_RETRY_BASE_DELAY_MS=400 };
typedef int readonly_diag_kind_t;
typedef struct { int unused; } installation_key_t;
typedef struct { char access_token[16]; } cloud_tokens_t;
typedef struct { char *data; size_t len,capacity; } http_response_t;
static const char *payloads[3];
static esp_err_t errors[3];
static unsigned locks,reads,delays,lock_failures;
static bool locked,pending;
static esp_err_t load_cloud_context(hub_cloud_settings_t *s,installation_key_t *k,cloud_tokens_t *t)
{ (void)s;(void)k;(void)t;return ESP_OK; }
static void free_cloud_context(hub_cloud_settings_t *s,installation_key_t *k,cloud_tokens_t *t)
{ free(s);free(k);free(t); }
esp_err_t hub_cloud_request_refresh(void) { return ESP_OK; }
static bool selected_machine_serial_for_request(const hub_cloud_settings_t *s,char *serial,size_t size)
{ (void)s;snprintf(serial,size,"SYNTHETIC");return true; }
static int statistics_http_max_attempts(readonly_diag_kind_t kind) { (void)kind;return 3; }
static int statistics_http_timeout_ms(readonly_diag_kind_t kind) { (void)kind;return 6500; }
static int64_t esp_timer_get_time(void) { return 1000000; }
static void vTaskDelay(int ms) { assert(ms>0);++delays; }
static bool hub_command_interactive_pending(void) { return pending; }
static bool cloud_command_is_waiting(void) { return pending; }
static esp_err_t cloud_readonly_wait_for_internal_heap(void) { return ESP_OK; }
static esp_err_t cloud_readonly_io_lock(void)
{
    ++locks;
    if(pending)return ESP_ERR_INVALID_STATE;
    if(lock_failures) { --lock_failures;return ESP_ERR_TIMEOUT; }
    assert(!locked);locked=true;return ESP_OK;
}
static void cloud_readonly_io_unlock(void) { assert(locked);locked=false; }
static esp_err_t cloud_readonly_http_request(const char *url,int method,const installation_key_t *key,
    const char *token,const char *body,http_response_t *response,int *status,int timeout)
{
    (void)key;(void)token;assert(method==HTTP_METHOD_GET&&!body&&locked&&timeout==6500);++reads;
    unsigned index=strstr(url,"LAST_COFFEE")?READONLY_DIAG_RECENT:
        strstr(url,"COFFEE_AND_FLUSH_TREND")?READONLY_DIAG_TREND:READONLY_DIAG_COUNTER;
    if(errors[index])return errors[index];
    *status=200;response->data=strdup(payloads[index]);assert(response->data);
    response->len=response->capacity=strlen(response->data);return ESP_OK;
}
static uint32_t http_response_first_bytes(const http_response_t *r) { (void)r;return 0; }
static void hub_http_response_reset(http_response_t *r) { free(r->data);memset(r,0,sizeof(*r)); }
static void record_readonly_http_diag(readonly_diag_kind_t k,esp_err_t e,int status,int ms,size_t bytes,uint32_t first)
{ (void)k;(void)e;(void)status;(void)ms;(void)bytes;(void)first; }
static bool cloud_readonly_retryable_error(esp_err_t e) { return e==ESP_ERR_TIMEOUT; }
static esp_err_t cloud_statistics_pause_between_endpoints(void) { return ESP_OK; }
static void update_counter_snapshot(const char *serial,cJSON *counter)
{ assert(!strcmp(serial,"SYNTHETIC")&&cJSON_IsObject(counter)); }
static bool counter_indicates_recent_coffees(cJSON *counter)
{ return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(counter,"totalCoffee"))>0; }
/* PRODUCTION */
static void reset(void)
{
    locks=reads=delays=lock_failures=0;locked=pending=false;memset(errors,0,sizeof(errors));
    payloads[0]="{\"output\":{\"totalCoffee\":15,\"totalFlush\":20}}";
    payloads[1]="{\"output\":{\"lastCoffees\":[{\"time\":1793235600000,\"extractionSeconds\":29.2,\"doseValue\":30.2,\"targetTemperature\":92}]}}";
    payloads[2]="{\"output\":{\"coffees\":[{\"timestamp\":1793235600000,\"value\":1}],\"flushes\":[{\"timestamp\":1793235600000,\"value\":0}]}}";
}
static void check(bool success,bool trend,unsigned count)
{
    char *body=NULL;
    esp_err_t err=hub_cloud_statistics_json_alloc_for_serial("SYNTHETIC",7,&body);
    assert(!locked);
    if(!success) { assert(err!=ESP_OK&&!body);return; }
    assert(err==ESP_OK&&body);
    cJSON *j=cJSON_Parse(body),*recent=cJSON_GetObjectItemCaseSensitive(j,"recentCoffees");
    assert(cJSON_IsArray(recent)&&(unsigned)cJSON_GetArraySize(recent)==count);
    cJSON *t=cJSON_GetObjectItemCaseSensitive(j,"trend");
    assert(trend?cJSON_IsObject(t):cJSON_IsNull(t));
    if(count) {
        cJSON *row=cJSON_GetArrayItem(recent,0);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(row,"time")->valuestring,"2026-10-29T01:00:00Z"));
        assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row,"extractionSeconds"))==29.2);
    }
    cJSON_Delete(j);free(body);
}
int main(void)
{
    reset();check(true,true,1);
    reset();errors[2]=ESP_ERR_TIMEOUT;check(true,false,1);
    reset();payloads[2]="{\"output\":{\"coffees\":[],\"flushes\":[]}}";check(true,false,1);
    reset();payloads[2]="invalid json";check(true,false,1);
    reset();errors[1]=ESP_ERR_TIMEOUT;check(false,false,0);
    reset();payloads[1]="{\"output\":{\"lastCoffees\":[]}}";check(false,false,0);
    reset();errors[0]=ESP_ERR_TIMEOUT;check(false,false,0);
    reset();payloads[0]="{}";check(false,false,0);
    reset();payloads[0]="{\"output\":{\"totalCoffee\":0,\"totalFlush\":0}}";
    payloads[1]="{\"output\":{\"lastCoffees\":[]}}";check(true,true,0);
    reset();payloads[0]="{\"output\":{\"totalCoffee\":0}}";
    payloads[1]="{\"output\":{\"lastCoffees\":[]}}";errors[2]=ESP_ERR_TIMEOUT;check(false,false,0);
    reset();lock_failures=1;check(true,true,1);assert(locks==4&&reads==3&&delays==1);
    reset();lock_failures=10;check(false,false,0);assert(locks==3&&!reads&&delays==2);
    reset();pending=true;check(false,false,0);assert(locks==1&&!reads&&!delays);
    reset();char *body=NULL;
    assert(hub_cloud_statistics_json_alloc_for_serial("OTHER",7,&body)!=ESP_OK&&!body&&!reads);
    puts("Cloud history PASS: trend failures preserve real records, absent data stays unavailable, bounded lock retry and control priority");
}
'''


def main():
    source = (ROOT / "main/hub_cloud.c").read_text()
    names = [
        "parse_double_string", "json_item_to_double", "find_key_recursive", "find_number_recursive",
        "cloud_response_payload", "cloud_response_output", "cloud_parse_json_text",
        "cloud_fetch_json_text", "add_number_or_null", "add_counter_number_or_null",
        "add_string_or_null", "millis_to_iso8601", "statistics_recent_coffees_json",
        "statistics_trend_points_json", "statistics_output_has_counter", "statistics_array_has_rows",
        "statistics_json_has_meaningful_data", "statistics_missing_required_recent",
        "statistics_missing_required_trend", "statistics_best_error_for_missing_recent",
        "statistics_best_error_for_empty_outputs", "hub_cloud_statistics_json_alloc_for_serial",
    ]
    functions = []
    for name in names:
        signature = re.search(r"(?m)^(?:static )?[\w* ]+\b" + name + r"\([^;{]*\)\s*\{", source)
        assert signature, name
        functions.append(signature.group(0) + function_body(source, name) + "}\n")
    json_dir = cjson_source_dir()
    with tempfile.TemporaryDirectory(prefix="cloud-history-") as temp:
        d = Path(temp)
        (d / "esp_err.h").write_text(ESP_ERR_STUB)
        (d / "test.c").write_text(HARNESS.replace("/* PRODUCTION */", "\n".join(functions)))
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
            "-I", str(d), "-I", str(ROOT / "main"), "-I", str(json_dir),
            str(d / "test.c"), str(json_dir / "cJSON.c"), "-lm", "-o", str(d / "test")], check=True)
        subprocess.run([str(d / "test")], check=True)


if __name__ == "__main__":
    main()
