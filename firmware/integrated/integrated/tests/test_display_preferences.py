#!/usr/bin/env python3
"""Run the display's actual preference loader/save callback against NVS faults."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "display/gbs_display.c").read_text()
submit = "static bool submit(" + source.split("static bool submit(", 1)[1].split("\nstatic bool read_data(", 1)[0]
loader = "static void load_device_preferences(" + source.split("static void load_device_preferences(", 1)[1].split("\n#endif", 1)[0]
model = (Path(__file__).resolve().parents[1] / "display/gbs_ui_model.c").read_text()
validator = "bool gbs_ui_variable_weight_valid(" + model.split("bool gbs_ui_variable_weight_valid(", 1)[1].split("\n}", 1)[0] + "\n}\n"
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#define HUB_AMOLED_216 1
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef int gbs_ui_machine_t;
typedef enum { GBS_ACTION_MODE, GBS_ACTION_BRIGHTNESS, GBS_ACTION_DIM,
    GBS_ACTION_ROTATION_AUTO, GBS_ACTION_SOUND, GBS_ACTION_WEIGHT_PRESET } gbs_ui_action_t;
static int old_sound=-1, saved_volume=-1, staged=-1;
static int saved_weight=-1;
static bool staging_weight;
static unsigned actual_weight;
static atomic_uint variable_weight=40;
bool gbs_ui_variable_weight_valid(double grams);
static bool rotate, actual_rotate, open_fail, set_fail, commit_fail;
static unsigned actual_volume, opens, sets, commits, cloud_calls;
static int nvs_open(const char *name,int mode,nvs_handle_t *handle) {
    assert(!strcmp(name,"gbs_display")); opens++; *handle=mode;
    return open_fail?-1:ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { (void)handle; staged=-1; }
static int nvs_get_u8(nvs_handle_t handle,const char *key,uint8_t *value) {
    (void)handle;
    int result=!strcmp(key,"weight_preset")?saved_weight:!strcmp(key,"volume")?saved_volume:!strcmp(key,"sound")?old_sound:rotate;
    if(result<0) return -1;
    *value=(uint8_t)result; return ESP_OK;
}
static int nvs_set_u8(nvs_handle_t handle,const char *key,uint8_t value) {
    assert(handle==NVS_READWRITE && (!strcmp(key,"volume")||!strcmp(key,"weight_preset"))); sets++;
    staging_weight=!strcmp(key,"weight_preset");
    if(set_fail) return -1;
    staged=value; return ESP_OK;
}
static int nvs_commit(nvs_handle_t handle) {
    assert(handle==NVS_READWRITE && staged>=0); commits++;
    if(commit_fail) return -1;
    if(staging_weight) saved_weight=staged; else saved_volume=staged;
    return ESP_OK;
}
static void gbs_ui_set_device_preferences(bool rotation,unsigned volume) {
    actual_rotate=rotation; actual_volume=volume;
}
static void gbs_ui_set_variable_weight(unsigned grams) {
    actual_weight=gbs_ui_variable_weight_valid(grams)?grams:40;
}
static unsigned gbs_ui_variable_weight(void) { return actual_weight; }
static bool request_mode(bool mode) { (void)mode; assert(false); return false; }
static bool gbs_hub_submit(gbs_ui_action_t action,double value,uint32_t id,const gbs_ui_machine_t *expected) {
    (void)action; (void)value; (void)id; (void)expected; cloud_calls++; return false;
}
''' + validator + submit + loader + r'''
static bool save(double value) { return submit(GBS_ACTION_SOUND,value,0,NULL,NULL); }
int main(void) {
    load_device_preferences(); assert(actual_volume==0 && !actual_rotate && !sets);
    old_sound=1; rotate=true;
    load_device_preferences(); assert(actual_volume==80 && actual_rotate && !sets);
    old_sound=0; load_device_preferences(); assert(actual_volume==0);
    old_sound=2; load_device_preferences(); assert(actual_volume==0);
    old_sound=1;
    unsigned levels[]={0,40,80,100};
    for(unsigned i=0;i<4;i++) {
        assert(save(levels[i])); actual_volume=255;
        load_device_preferences(); assert(actual_volume==levels[i] && actual_rotate);
        unsigned before=sets;
        assert(save(levels[i]) && sets==before);
    }
    saved_volume=255; load_device_preferences(); assert(actual_volume==80);
    saved_volume=40;
    open_fail=true; assert(!save(100)); load_device_preferences(); assert(actual_volume==0);
    open_fail=false;
    set_fail=true; assert(!save(100)); set_fail=false;
    load_device_preferences(); assert(actual_volume==40);
    commit_fail=true; assert(!save(100)); commit_fail=false;
    load_device_preferences(); assert(actual_volume==40);
    unsigned before=opens;
    assert(!save(-1) && !save(101) && !save(0.5) && !save(NAN) && !save(INFINITY));
    assert(opens==before && cloud_calls==0);
    assert(save(0)); load_device_preferences(); assert(actual_volume==0 && old_sound==1);
    assert(actual_weight==40 && saved_weight==-1);
    unsigned invalid[]={0,20,30,36,201,255};
    before=sets;
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        saved_weight=invalid[i]; load_device_preferences();
        assert(actual_weight==40 && atomic_load(&variable_weight)==40 && sets==before);
    }
    unsigned weights[]={1,35,40,42,200};
    for(unsigned i=0;i<sizeof(weights)/sizeof(weights[0]);i++) {
        assert(submit(GBS_ACTION_WEIGHT_PRESET,weights[i],0,NULL,NULL));
        assert(atomic_load(&variable_weight)==weights[i]);
        actual_weight=0; load_device_preferences();
        assert(actual_weight==weights[i] && actual_volume==0 && actual_rotate);
        before=sets;
        atomic_store(&variable_weight,0);
        assert(submit(GBS_ACTION_WEIGHT_PRESET,weights[i],0,NULL,NULL) && sets==before);
        assert(atomic_load(&variable_weight)==weights[i]);
    }
    open_fail=true; assert(!submit(GBS_ACTION_WEIGHT_PRESET,42,0,NULL,NULL)); open_fail=false;
    set_fail=true; assert(!submit(GBS_ACTION_WEIGHT_PRESET,42,0,NULL,NULL)); set_fail=false;
    commit_fail=true; assert(!submit(GBS_ACTION_WEIGHT_PRESET,42,0,NULL,NULL)); commit_fail=false;
    load_device_preferences(); assert(actual_weight==200 && saved_weight==200);
    double rejected[]={0,20,30,36,201,1.5,NAN,INFINITY}; before=opens;
    for(unsigned i=0;i<sizeof(rejected)/sizeof(rejected[0]);i++)
        assert(!submit(GBS_ACTION_WEIGHT_PRESET,rejected[i],0,NULL,NULL));
    assert(opens==before && cloud_calls==0 && saved_volume==0);
    puts("Display preferences: legacy migration, mute precedence, restart, NVS faults and bounds PASS");
    puts("Variable preset: validated NVS save, restart recall, no duplicate writes, failed commits preserve value, no Cloud calls PASS");
}
'''
with tempfile.TemporaryDirectory(prefix="gbs-preferences-") as directory:
    c_file = Path(directory) / "test.c"
    executable = Path(directory) / "test"
    c_file.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        str(c_file), "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
