#!/usr/bin/env python3
"""Exercise the production official-app hint detector with synthetic HTTP evidence."""
from pathlib import Path
import re
import tempfile

from test_integrated_support import ERRORS, ROOT, compile_run, function_body
from host_cjson import cjson_source_dir

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "cJSON.h"
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
typedef int portMUX_TYPE;
static bool locked, wifi=true, synchronized=true;
static int64_t clock_ms=5000;
static int64_t esp_timer_get_time(void) { return clock_ms*1000; }
#define portENTER_CRITICAL(lock) do { (void)(lock);assert(!locked);locked=true; } while(0)
#define portEXIT_CRITICAL(lock) do { (void)(lock);assert(locked);locked=false; } while(0)
static uint32_t esp_get_free_heap_size(void) { assert(!locked);return 50000; }
static size_t heap_caps_get_largest_free_block(int caps) { (void)caps;assert(!locked);return 16000; }
static bool hub_wifi_station_has_ip(void) { return wifi; }
static const char *hub_cloud_time_status(void) { return synchronized?"synchronized":"syncing"; }
/* TYPES */
static readonly_http_diag_t readonly_http_diags[READONLY_DIAG_COUNT];
static portMUX_TYPE readonly_http_diag_lock,auth_diag_lock;
static uint32_t auth_refresh_success_count,auth_refresh_failure_count;
static int64_t auth_refresh_last_success_uptime_ms;
static int64_t policy_required_ms;
typedef struct { const char *data; size_t len; } http_response_t;
/* FUNCTIONS */
static void observation(readonly_diag_kind_t kind,int status)
{
    clock_ms+=1000;
    record_readonly_http_diag(kind,status==200?ESP_OK:ESP_ERR_INVALID_RESPONSE,status,1000,0,0);
}
static void pair(int things,int dashboard)
{ observation(READONLY_DIAG_THINGS,things);observation(READONLY_DIAG_DASHBOARD,dashboard); }
static void reset(void)
{
    memset(readonly_http_diags,0,sizeof(readonly_http_diags));
    clock_ms=5000;wifi=synchronized=true;
    auth_refresh_last_success_uptime_ms=0;auth_refresh_success_count=auth_refresh_failure_count=0;
    record_auth_refresh_result(true);
}
static void response(int status, const char *body)
{
    clock_ms+=1000;
    http_response_t r={.data=body,.len=body?strlen(body):0};
    record_cloud_policy_response(status,&r);
}
static void policy_code(void)
{
    const char *policy="{\"success\":false,\"message\":\"User must accept latest policy\",\"code\":\"USER_MUST_ACCEPT_POLICY\"}";
    reset();response(403,policy);
    assert(hub_cloud_policy_acceptance_required()&&hub_cloud_app_check_suggested());
    assert(!readonly_http_diags[READONLY_DIAG_THINGS].access_rejections);
    wifi=false;assert(!hub_cloud_policy_acceptance_required());wifi=true;
    synchronized=false;assert(!hub_cloud_app_check_suggested());synchronized=true;
    assert(hub_cloud_policy_acceptance_required());
    clock_ms+=120001;assert(!hub_cloud_app_check_suggested());
    response(403,policy);record_auth_refresh_result(true);assert(!hub_cloud_app_check_suggested());
    response(403,policy);record_auth_refresh_result(false);assert(!hub_cloud_app_check_suggested());
    response(403,policy);auth_refresh_last_success_uptime_ms=0;assert(!hub_cloud_app_check_suggested());
    reset();response(403,policy);clock_ms=1000;assert(!hub_cloud_app_check_suggested());

    const char *other[]={NULL,"","<html>Forbidden</html>","{", "null", "[]",
        "{\"code\":7}","{\"code\":null}","{\"code\":\"OTHER\"}",
        "{\"message\":\"USER_MUST_ACCEPT_POLICY\"}",
        "{\"nested\":{\"code\":\"USER_MUST_ACCEPT_POLICY\"}}",
        "{\"code\":\"USER_MUST_ACCEPT_POLICY_EXTRA\"}",
        "{\"code\":\"USER_MUST_ACCEPT_POLICY\"}trailing"};
    for(unsigned i=0;i<sizeof(other)/sizeof(other[0]);++i) {
        reset();response(403,policy);response(403,other[i]);
        assert(!hub_cloud_policy_acceptance_required()&&!hub_cloud_app_check_suggested());
    }
    char oversized[1100];memset(oversized,' ',sizeof(oversized));
    memcpy(oversized,policy,strlen(policy));oversized[sizeof(oversized)-1]=0;
    response(403,oversized);assert(!hub_cloud_policy_acceptance_required());
    char embedded[128];size_t len=strlen(policy);memcpy(embedded,policy,len+1);embedded[len+1]='x';embedded[len+2]=0;
    http_response_t r={.data=embedded,.len=len+2};
    record_cloud_policy_response(403,&r);assert(!hub_cloud_policy_acceptance_required());
    record_cloud_policy_response(403,NULL);assert(!hub_cloud_policy_acceptance_required());
    const int unrelated[]={200,204,302,500,503};
    for(unsigned i=0;i<sizeof(unrelated)/sizeof(unrelated[0]);++i) {
        reset();response(403,policy);response(unrelated[i],policy);
        assert(!hub_cloud_policy_acceptance_required());
    }
    reset();response(403,policy);response(401,"{}");assert(!hub_cloud_app_check_suggested());
    reset();pair(403,412);pair(403,403);
    assert(hub_cloud_app_check_suggested()&&!hub_cloud_policy_acceptance_required());
}
int main(void)
{
    policy_code();
    reset();pair(403,412);assert(!hub_cloud_app_check_suggested());
    pair(403,403);assert(hub_cloud_app_check_suggested());
    wifi=false;assert(!hub_cloud_app_check_suggested());wifi=true;
    synchronized=false;assert(!hub_cloud_app_check_suggested());synchronized=true;
    assert(hub_cloud_app_check_suggested());
    observation(READONLY_DIAG_DASHBOARD,200);assert(!hub_cloud_app_check_suggested());
    pair(403,412);assert(!hub_cloud_app_check_suggested());pair(403,412);assert(hub_cloud_app_check_suggested());
    observation(READONLY_DIAG_THINGS,200);assert(!hub_cloud_app_check_suggested());
    reset();pair(412,412);pair(412,412);assert(!hub_cloud_app_check_suggested());
    reset();pair(403,200);pair(403,200);assert(!hub_cloud_app_check_suggested());
    reset();pair(200,403);pair(200,403);assert(!hub_cloud_app_check_suggested());
    const int unrelated[]={0,401,500,503};
    for(unsigned i=0;i<sizeof(unrelated)/sizeof(unrelated[0]);++i) {
        reset();pair(403,403);pair(403,403);assert(hub_cloud_app_check_suggested());
        pair(unrelated[i],unrelated[i]);assert(!hub_cloud_app_check_suggested());
    }
    reset();pair(403,403);pair(403,403);auth_refresh_last_success_uptime_ms=0;
    assert(!hub_cloud_app_check_suggested());
    reset();pair(403,403);pair(403,403);clock_ms+=120001;assert(!hub_cloud_app_check_suggested());
    pair(403,403);assert(!hub_cloud_app_check_suggested());pair(403,403);assert(hub_cloud_app_check_suggested());
    record_auth_refresh_result(true);assert(!hub_cloud_app_check_suggested());
    pair(403,403);assert(!hub_cloud_app_check_suggested());pair(403,403);assert(hub_cloud_app_check_suggested());
    record_auth_refresh_result(false);assert(!hub_cloud_app_check_suggested());
    reset();for(int i=0;i<100;++i)pair(403,412);
    assert(hub_cloud_app_check_suggested()&&readonly_http_diags[READONLY_DIAG_DASHBOARD].access_rejections==2);
    clock_ms=1000;assert(!hub_cloud_app_check_suggested());
    reset();observation(READONLY_DIAG_COUNT,403);assert(!hub_cloud_app_check_suggested()&&!locked);
    puts("Official app hint PASS: exact policy code, malformed/unrelated replies, paired rejection fallback, auth/clock/Wi-Fi guards, expiry and recovery");
}
'''


def main():
    source = (ROOT / "main/hub_cloud.c").read_text()
    types = source[source.index("typedef enum {\n    READONLY_DIAG_THINGS"):source.index("typedef struct {\n    char access_token")]
    functions = []
    for name in ("hub_cloud_auth_refresh_last_success_uptime_ms", "record_auth_refresh_result",
                 "record_readonly_http_diag", "record_cloud_policy_response",
                 "hub_cloud_policy_acceptance_required", "hub_cloud_app_check_suggested"):
        signature = re.search(r"(?m)^[\w* ]+\b" + name + r"\([^;{]*\)\s*\{", source)
        assert signature, name
        functions.append(signature.group() + function_body(source, name) + "}\n")
    with tempfile.TemporaryDirectory(prefix="cloud-app-check-") as temp:
        directory = Path(temp)
        (directory / "esp_err.h").write_text(ERRORS)
        json_dir = cjson_source_dir()
        (directory / "cJSON.h").write_text((json_dir / "cJSON.h").read_text())
        compile_run(directory, "app_check", HARNESS.replace("/* TYPES */", types)
                    .replace("/* FUNCTIONS */", "\n".join(functions)),
                    [json_dir / "cJSON.c", "-Wno-deprecated-declarations"])


if __name__ == "__main__":
    main()
