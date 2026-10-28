#!/usr/bin/env python3
"""Exercise the production compound weight command with bounded, offline HTTP steps."""
import re
import tempfile
from pathlib import Path

from test_integrated_support import ERRORS, ROOT, compile_run, function_body

HARNESS = r'''
#include "hub_cloud.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
/* TYPES */
typedef void *esp_http_client_handle_t;
static cloud_snapshot_t fresh,snapshot;
static bool locked,automatic,identity_valid;
static unsigned locks,unlocks,reads,posts,cleanups,saves,updates;
static int64_t clock_ms,read_ms,mode_ms,dose_ms,lock_ms;
static int read_error,lock_error,mode_error,dose_error,mode_status,dose_status;
static bool mode_started,dose_started,change_identity_after_mode;
static bool history_available;
static unsigned history_reads;
static char dose_body[96];
static int64_t esp_timer_get_time(void) { return clock_ms*1000; }
static const char *esp_err_to_name(esp_err_t e) { return e?"error":"ok"; }
static bool active_command_target(hub_machine_selection_identity_t *target) {
    strcpy(target->serial_number,"SYNTHETIC");target->generation=7;return identity_valid;
}
esp_err_t hub_config_get_bbw_doses(const hub_machine_selection_identity_t *target,hub_bbw_doses_t *out) {
    assert(target->generation==7&&!strcmp(target->serial_number,"SYNTHETIC"));++history_reads;
    if(!history_available)return ESP_ERR_NOT_FOUND;
    *out=(hub_bbw_doses_t){"SYNTHETIC","Dose2",40,30.5};return ESP_OK;
}
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
static void set_command_result(hub_cloud_command_result_t *r,bool ok,int64_t started,const char *m) {
    r->ok=ok;r->duration_ms=(int)(clock_ms-started/1000);snprintf(r->message,sizeof(r->message),"%s",m);
}
static void set_command_attempt(hub_cloud_command_result_t *r,const hub_cloud_command_attempt_t *a) {
    r->request_started=a->request_started;r->upstream_attempts=a->attempts;r->http_status=a->http_status;
}
static esp_err_t ensure_cloud_controls_ready_or_fail(hub_cloud_command_result_t *r,int64_t t) {(void)r;(void)t;return 0;}
static bool hub_command_requires_online_machine(void) {return automatic;}
static esp_err_t cloud_command_io_lock(const hub_command_deadline_t *d,uint32_t reserve) {
    (void)d;(void)reserve;assert(!locked);++locks;clock_ms+=lock_ms;locked=!lock_error;return lock_error;
}
static void cloud_command_io_unlock(void) {assert(locked);locked=false;++unlocks;}
static void cloud_http_session_cleanup(esp_http_client_handle_t *session) {assert(locked);*session=NULL;++cleanups;}
static esp_err_t read_control_snapshot_locked(const hub_command_deadline_t *d,uint32_t reserve,
    cloud_snapshot_t *out,esp_http_client_handle_t *session) {
    (void)d;(void)reserve;assert(locked&&!*session);++reads;*out=fresh;*session=&fresh;clock_ms+=read_ms;return read_error;
}
static hub_cloud_command_attempt_t execute_command_with_auth_locked(const char *name,const char *body,
    const hub_command_deadline_t *d,uint32_t reserve,esp_http_client_handle_t *session) {
    assert(locked&&session&&*session==&fresh);
    if(!identity_valid)return (hub_cloud_command_attempt_t){.err=ESP_ERR_INVALID_STATE};
    if(!hub_command_deadline_can_start_step(d,clock_ms,reserve+1000))
        return (hub_cloud_command_attempt_t){.err=ESP_ERR_TIMEOUT};
    bool mode=strstr(name,"ChangeMode")!=NULL;
    int error=mode?mode_error:dose_error,status=mode?mode_status:dose_status;
    bool started=mode?mode_started:dose_started;
    ++posts;clock_ms+=mode?mode_ms:dose_ms;
    if(mode) {assert(!strcmp(body,"{\"mode\":\"Dose1\"}"));if(change_identity_after_mode)identity_valid=false;}
    else {assert(strstr(name,"SettingDoses"));snprintf(dose_body,sizeof(dose_body),"%s",body);}
    return (hub_cloud_command_attempt_t){.err=error,.http_status=status,.request_started=started,.attempts=started};
}
static hub_cloud_command_attempt_t execute_setting_command_with_auth(const char *name,const char *body,
    const hub_command_deadline_t *d,uint32_t reserve) {
    (void)name;(void)body;(void)d;(void)reserve;assert(false);return (hub_cloud_command_attempt_t){0};
}
static void schedule_command_auth_recovery(const hub_cloud_command_attempt_t *a) {(void)a;assert(!locked);}
static void update_snapshot_bbw_mode(const char *mode) {if(identity_valid){++saves;strcpy(snapshot.bbw_mode,mode);clock_ms+=11;}}
static void update_snapshot_bbw_dose(double first,double second) {if(identity_valid){++updates;snapshot.dose1=first;snapshot.dose2=second;}}
static void cloud_command_failure_message(char *out,size_t n,const char *label,int err,const hub_cloud_command_attempt_t *a) {
    (void)label;(void)err;snprintf(out,n,"Dose failed %s upstream",a->request_started?"after":"before");
}
/* FUNCTIONS */
static void reset(void) {
    fresh=(cloud_snapshot_t){.selection_generation=7,.connected=true,.dose1_set=true,.dose1=40,
        .dose2_set=true,.dose2=30.5};strcpy(fresh.serial_number,"SYNTHETIC");strcpy(fresh.bbw_mode,"Dose2");
    snapshot=fresh;strcpy(snapshot.last_updated,"unchanged");
    locked=automatic=change_identity_after_mode=false;identity_valid=true;
    locks=unlocks=reads=posts=cleanups=saves=updates=0;clock_ms=0;
    lock_ms=478+969;read_ms=1862;mode_ms=783;dose_ms=783;read_error=lock_error=mode_error=dose_error=0;
    mode_status=dose_status=200;mode_started=dose_started=true;dose_body[0]=0;
    history_available=false;history_reads=0;
}
static esp_err_t run(hub_cloud_command_result_t *mode,hub_cloud_command_result_t *dose) {
    hub_command_deadline_t d={.enforced=true,.deadline_at_ms=8000};
    int err=hub_cloud_set_quick_weight_with_deadline(20,true,&d,mode,dose);
    assert(!locked&&locks==1&&unlocks==(lock_error==0)&&cleanups==unlocks);
    assert(!strcmp(snapshot.last_updated,"unchanged"));return err;
}
int main(void) {
    hub_cloud_command_result_t mode,dose;
    hub_command_deadline_t deadline={.enforced=true,.deadline_at_ms=8000};
    // Recorded two-session execution used 6820 ms before the final POST: less than its 1000+250 ms reserve.
    assert(!hub_command_deadline_can_start_step(&deadline,6820,1000));
    reset();assert(run(&mode,&dose)==0&&mode.ok&&dose.ok&&reads==1&&posts==2&&saves==1&&updates==1);
    assert(clock_ms<5000&&!strcmp(dose_body,"{\"doses\":{\"Dose1\":20.0,\"Dose2\":30.5}}"));
    printf("Compound weight replay: recorded old path cannot admit final POST at 6820 ms; one-session fixture completes at %lld ms\n",(long long)clock_ms);
    for(int scenario=0;scenario<8;++scenario) {
        reset();if(scenario==0)lock_error=ESP_ERR_TIMEOUT;
        if(scenario==1)read_error=ESP_ERR_TIMEOUT;
        if(scenario==2){automatic=true;fresh.connected=false;}
        if(scenario==3)fresh.dose2_set=false;
        if(scenario==4)fresh.dose2=NAN;
        if(scenario==5)fresh.dose2=200.1;
        if(scenario==6)fresh.selection_generation=8;
        if(scenario==7)strcpy(fresh.serial_number,"OTHER");
        assert(run(&mode,&dose)!=0&&!mode.ok&&!dose.ok&&!posts&&!updates&&!saves);
    }
    reset();fresh.connected=false;assert(run(&mode,&dose)==0&&posts==2); // Manual controls keep their existing offline policy.
    reset();fresh.connected=false;fresh.explicitly_offline=true;fresh.dose1_set=fresh.dose2_set=false;history_available=true;
    assert(run(&mode,&dose)==ESP_OK&&posts==2&&history_reads==1&&mode.ok&&dose.ok);
    assert(!strcmp(dose_body,"{\"doses\":{\"Dose1\":20.0,\"Dose2\":30.5}}"));
    for(int scenario=0;scenario<8;++scenario) {
        reset();fresh.connected=false;fresh.explicitly_offline=true;fresh.dose1_set=fresh.dose2_set=false;history_available=true;
        if(scenario==0)automatic=true;
        if(scenario==1)fresh.explicitly_offline=false;
        if(scenario==2)fresh.connected=true;
        if(scenario==3)history_available=false;
        if(scenario==4)read_error=ESP_ERR_TIMEOUT;
        if(scenario==5)fresh.selection_generation=8;
        if(scenario==6)strcpy(fresh.serial_number,"OTHER");
        if(scenario==7)fresh.bbw_doses_invalid=true;
        assert(run(&mode,&dose)!=ESP_OK&&posts==0&&!mode.ok&&!dose.ok);
    }
    reset();fresh.connected=false;fresh.explicitly_offline=true;fresh.dose1_set=false;fresh.dose2=36;history_available=true;
    assert(run(&mode,&dose)==ESP_OK&&strstr(dose_body,"36.0")); // Preserve any freshly returned dose over history.
    reset();fresh.connected=false;fresh.explicitly_offline=true;fresh.dose1_set=false;fresh.dose2=NAN;history_available=true;
    assert(run(&mode,&dose)!=ESP_OK&&posts==0); // Invalid explicit values are never repaired by history.
    reset();fresh.connected=false;fresh.explicitly_offline=true;fresh.dose1_set=fresh.dose2_set=false;history_available=true;
    assert(hub_cloud_set_bbw_mode_checked_with_deadline("Dose1",40,false,&deadline,0,&mode)==ESP_OK&&mode.ok&&posts==1&&history_reads==1);
    reset();fresh.dose1=20;assert(run(&mode,&dose)==ESP_ERR_INVALID_STATE&&mode.preset_changed&&!posts);
    assert(snapshot.dose1==20&&snapshot.dose2==30.5&&!strcmp(snapshot.bbw_mode,"Dose2")&&!saves);
    locks=unlocks=reads=cleanups=0;clock_ms=0;
    assert(hub_cloud_set_bbw_mode_checked_with_deadline("Dose1",20,false,&deadline,0,&mode)==0&&mode.ok&&posts==1);
    assert(reads==1&&snapshot.dose1==20); // The corrected cache routes the next press to its stored dose.
    reset();fresh.dose2=20;assert(hub_cloud_set_bbw_mode_checked_with_deadline("Dose2",30,false,&deadline,0,&mode)!=0);
    assert(mode.preset_changed&&snapshot.dose2==20&&!posts&&!strcmp(snapshot.last_updated,"unchanged"));
    for(int scenario=0;scenario<3;++scenario) {
        reset();mode_error=scenario==2?ESP_ERR_INVALID_RESPONSE:ESP_ERR_TIMEOUT;
        mode_status=scenario==2?401:0;mode_started=scenario!=0;
        assert(run(&mode,&dose)!=0&&!mode.ok&&!dose.ok&&posts==1&&!updates&&!saves);
        assert(mode.request_started==mode_started&&!dose.request_started);
    }
    reset();mode_ms=4000;assert(run(&mode,&dose)==ESP_ERR_TIMEOUT&&mode.ok&&!dose.ok&&posts==1&&saves==1&&!updates);
    reset();change_identity_after_mode=true;assert(run(&mode,&dose)==ESP_ERR_INVALID_STATE&&mode.ok&&!dose.ok&&posts==1&&!updates);
    for(int scenario=0;scenario<2;++scenario) {
        reset();dose_error=ESP_ERR_TIMEOUT;dose_status=0;dose_started=scenario!=0;
        assert(run(&mode,&dose)==ESP_ERR_TIMEOUT&&mode.ok&&!dose.ok&&posts==2&&saves==1&&!updates);
        assert(dose.request_started==dose_started&&snapshot.dose1==40&&snapshot.dose2==30.5);
    }
    // Replay observed pre-write delays; the HTTP calls themselves remain synthetic.
    const int prewrite_ms[]={4479,5967,7381};
    for(unsigned i=0;i<sizeof(prewrite_ms)/sizeof(prewrite_ms[0]);++i) {
        for(unsigned ttl=8000;ttl<=12000;ttl+=4000) {
            reset();lock_ms=0;read_ms=prewrite_ms[i];
            fresh.dose1=20;fresh.dose2=30;strcpy(fresh.bbw_mode,"Dose1");snapshot=fresh;
            hub_command_deadline_t extended={.enforced=true,.client_ttl_present=true,.client_ttl_ms=ttl};
            assert(hub_command_deadline_accept(&extended,0));
            int err=hub_cloud_set_quick_weight_with_deadline(40,true,&extended,&mode,&dose);
            assert(!locked&&locks==1&&unlocks==1&&reads==1&&cleanups==1);
            if(ttl==8000) assert(err==ESP_ERR_TIMEOUT&&!posts&&!mode.ok&&!dose.ok&&snapshot.dose1==20);
            else {
                assert(err==ESP_OK&&mode.ok&&dose.ok&&posts==2&&clock_ms<12000);
                assert(snapshot.dose1==40&&snapshot.dose2==30);
                printf("Network margin replay: prewrite=%d ms, total=%lld ms, posts=%u\n",prewrite_ms[i],(long long)clock_ms,posts);
            }
        }
    }
    reset();lock_ms=0;read_ms=7751;
    hub_command_deadline_t extended={.enforced=true,.client_ttl_present=true,.client_ttl_ms=12000};
    assert(hub_command_deadline_accept(&extended,0));
    assert(hub_cloud_set_quick_weight_with_deadline(20,true,&extended,&mode,&dose)==ESP_ERR_TIMEOUT&&!posts);
    puts("Compound Cloud weight: one lock/read/session, stale preset repair, identity, offline guard, decimal Dose2, deadline and no repeated POST passed");
}
'''


def main():
    source = (ROOT / 'main/hub_cloud.c').read_text()
    types = re.search(r'typedef struct\s*\{[^}]+\}\s*cloud_snapshot_t;', source).group()
    functions = []
    for name in ['control_decimal_value_is_valid', 'restore_offline_bbw_doses', 'bbw_mode_is_valid', 'publish_control_bbw_snapshot',
                 'hub_cloud_set_quick_weight_with_deadline', 'hub_cloud_set_bbw_mode_checked_with_deadline']:
        signature = re.search(r'^(?:static )?[^\n;{}]+\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
        assert signature, name
        functions.append(signature.group() + function_body(source, name) + '}')
    harness = HARNESS.replace('/* TYPES */', types).replace('/* FUNCTIONS */', '\n'.join(functions))
    with tempfile.TemporaryDirectory(prefix='cloud-quick-weight-') as tmp:
        directory = Path(tmp)
        (directory / 'esp_err.h').write_text(ERRORS.replace('const char *esp_err_to_name(esp_err_t error);', ''))
        compile_run(directory, 'compound', harness, [ROOT / 'main/hub_command_deadline.c'])


if __name__ == '__main__':
    main()
