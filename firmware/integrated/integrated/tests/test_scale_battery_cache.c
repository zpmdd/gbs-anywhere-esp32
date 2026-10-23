#include "gbs_scale_battery_cache.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char durable[128], staged[128];
static size_t durable_size, staged_size;
static unsigned writes, commits, closes;
static bool fail_open, fail_read, fail_set, fail_commit;
static const int64_t WALL = INT64_C(1792700000);

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name,"gbs_display"));
    assert(mode==NVS_READONLY || mode==NVS_READWRITE);
    if(fail_open) return ESP_FAIL;
    *handle=1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle==1 && !strcmp(key,"scale_batt_v1"));
    if(fail_read || !durable_size || *size<durable_size) return ESP_FAIL;
    memcpy(value,durable,durable_size); *size=durable_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle==1 && !strcmp(key,"scale_batt_v1") && size<=sizeof(staged));
    writes++;
    if(fail_set) return ESP_FAIL;
    memcpy(staged,value,size); staged_size=size; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle==1); commits++;
    if(fail_commit) return ESP_FAIL;
    memcpy(durable,staged,staged_size); durable_size=staged_size; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle==1); closes++; }

static void reset(void)
{
    memset(durable,0,sizeof(durable)); memset(staged,0,sizeof(staged));
    durable_size=staged_size=writes=commits=closes=0;
    fail_open=fail_read=fail_set=fail_commit=false;
}
static gbs_ui_machine_t observation(unsigned percent, int64_t observed_ms)
{
    return (gbs_ui_machine_t){.available=true,.scale_connected_known=true,
        .scale_connected=true,.scale_battery_known=true,.scale_battery_percent=percent,
        .scale_observed_ms=observed_ms};
}
static void observe(gbs_scale_battery_cache_t *cache, unsigned percent, int64_t observed_ms)
{
    gbs_ui_machine_t machine=observation(percent,observed_ms);
    gbs_scale_battery_cache_observe(cache,"synthetic-a",1,&machine,observed_ms,WALL);
}
static gbs_scale_battery_cache_t fresh(void)
{
    gbs_scale_battery_cache_t cache={0};
    gbs_scale_battery_cache_bind(&cache,"synthetic-a",1,WALL);
    return cache;
}

static void accepted_sources(void)
{
    reset(); gbs_scale_battery_cache_t cache=fresh();
    assert(!cache.known);
    observe(&cache,0,1000); assert(cache.known && cache.percent==0);
    observe(&cache,1,2000); assert(cache.percent==1);
    observe(&cache,100,3000); assert(cache.percent==100);
    gbs_ui_machine_t machine=observation(28,4000);
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,15500,WALL);
    assert(cache.percent==28 && cache.observed_unix==WALL-11);
    machine.scale_battery_percent=99;
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,16000,WALL+10);
    assert(cache.percent==28 && cache.observed_unix==WALL-11);
    machine.scale_observed_ms=3999;
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,16000,WALL);
    assert(cache.percent==28);
    machine=observation(20,5000);
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,5000,0);
    assert(cache.percent==20 && cache.observed_unix==0);
    machine=observation(19,6000);
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,6000,INT64_MAX);
    assert(cache.percent==19 && cache.observed_unix==0);
}

static void rejected_sources(void)
{
    reset(); gbs_scale_battery_cache_t cache=fresh(); observe(&cache,28,1000);
    for(unsigned bad=0;bad<10;bad++) {
        gbs_ui_machine_t machine=observation(99,2000+bad*1000);
        int64_t now_ms=machine.scale_observed_ms;
        switch(bad) {
        case 0: machine.available=false; break;
        case 1: machine.stale=true; break;
        case 2: machine.scale_connected_known=false; break;
        case 3: machine.scale_connected=false; break;
        case 4: machine.scale_battery_known=false; break;
        case 5: machine.scale_battery_percent=101; break;
        case 6: machine.scale_stale=true; break;
        case 7: now_ms+=60000; break;
        case 8: now_ms--; break;
        case 9: machine.scale_observed_ms=0; break;
        }
        gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,now_ms,WALL);
        assert(cache.known && cache.percent==28 && cache.observed_unix==WALL);
    }
    gbs_ui_machine_t machine=observation(1,20000);
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,79999,WALL);
    assert(cache.percent==1 && cache.observed_unix==WALL-59);
    machine=observation(30,30000); machine.stale=true;
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,30000,WALL);
    machine.stale=false;
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,30001,WALL);
    assert(cache.percent==30); /* Failed local transport did not consume a real observation. */
}

static void persistence_and_restore(void)
{
    reset(); gbs_scale_battery_cache_t cache=fresh(); observe(&cache,1,1000);
    assert(gbs_scale_battery_cache_save(&cache,1000,false)==ESP_OK && !writes);
    assert(gbs_scale_battery_cache_save(&cache,1000,true)==ESP_OK && writes==1 && commits==1);
    assert(durable_size==88);
    observe(&cache,1,2000);
    assert(gbs_scale_battery_cache_save(&cache,1000000,true)==ESP_OK && writes==1);
    observe(&cache,25,3000);
    observe(&cache,23,4000);
    assert(gbs_scale_battery_cache_save(&cache,60999,true)==ESP_OK && writes==1);
    assert(gbs_scale_battery_cache_save(&cache,61000,true)==ESP_OK && writes==2);
    observe(&cache,20,62000); observe(&cache,23,63000);
    assert(gbs_scale_battery_cache_save(&cache,121000,true)==ESP_OK && writes==2);

    gbs_scale_battery_cache_t reboot={0};
    gbs_scale_battery_cache_bind(&reboot,"",0,WALL);
    assert(!reboot.bound && !reboot.known);
    gbs_scale_battery_cache_bind(&reboot,"synthetic-a",1,WALL);
    assert(reboot.known && reboot.percent==23 && reboot.observed_ms==0);
    gbs_ui_machine_t machine={.stale=true,.scale_stale=true};
    gbs_scale_battery_cache_apply(&reboot,&machine);
    assert(machine.scale_battery_last_known && machine.scale_battery_last_percent==23);
    assert(machine.scale_battery_last_observed_unix==WALL);
    assert(!machine.available && !machine.scale_battery_known && !machine.scale_connected_known);
    assert(!machine.scale_connected && !machine.scale_observed_ms && machine.scale_stale);
    assert(gbs_scale_battery_cache_save(&reboot,0,true)==ESP_OK && writes==2);
    observe(&reboot,23,1);
    assert(gbs_scale_battery_cache_save(&reboot,1,true)==ESP_OK && writes==2);
    observe(&reboot,0,2);
    assert(gbs_scale_battery_cache_save(&reboot,2,true)==ESP_OK && writes==3);
}

static void binding_changes(void)
{
    reset(); gbs_scale_battery_cache_t cache=fresh(); observe(&cache,28,1000);
    assert(gbs_scale_battery_cache_save(&cache,1000,true)==ESP_OK);
    gbs_scale_battery_cache_t other={0};
    gbs_scale_battery_cache_bind(&other,"synthetic-b",1,WALL);
    assert(!other.known);
    gbs_scale_battery_cache_bind(&cache,"synthetic-a",2,WALL);
    assert(!cache.known && cache.clear_pending);
    gbs_ui_machine_t machine=observation(10,2000);
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",1,&machine,2000,WALL);
    assert(!cache.known);
    gbs_scale_battery_cache_observe(&cache,"synthetic-b",2,&machine,2000,WALL);
    assert(!cache.known);
    /* Power loss before the invalidation commit can retain the prior historical value. */
    gbs_scale_battery_cache_t early_reboot=fresh();
    assert(early_reboot.known && early_reboot.percent==28 && !early_reboot.observed_ms);
    assert(gbs_scale_battery_cache_save(&cache,2000,true)==ESP_OK);
    gbs_scale_battery_cache_t reboot=fresh(); assert(!reboot.known);
    gbs_scale_battery_cache_observe(&cache,"synthetic-a",2,&machine,2000,WALL);
    assert(cache.known && cache.percent==10);
    gbs_scale_battery_cache_bind(&cache,"",3,WALL);
    assert(!cache.known);
    gbs_scale_battery_cache_bind(&cache,"synthetic-a",4,WALL);
    assert(!cache.known); /* Runtime A -> unbound -> A never reloads the old NVS value. */
}

static void write_failures(void)
{
    for(unsigned failure=0;failure<3;failure++) {
        reset(); gbs_scale_battery_cache_t cache=fresh(); observe(&cache,1,1);
        fail_open=failure==0; fail_set=failure==1; fail_commit=failure==2;
        assert(gbs_scale_battery_cache_save(&cache,0,true)==ESP_FAIL);
        assert(cache.known && cache.percent==1 && !cache.saved_known && !durable_size);
        unsigned attempts=writes;
        fail_open=fail_set=fail_commit=false;
        assert(gbs_scale_battery_cache_save(&cache,59999,true)==ESP_OK && writes==attempts);
        assert(gbs_scale_battery_cache_save(&cache,-1,true)==ESP_OK && writes==attempts);
        observe(&cache,0,60000);
        assert(gbs_scale_battery_cache_save(&cache,60000,true)==ESP_OK && writes==attempts+1);
        gbs_scale_battery_cache_t reboot=fresh(); assert(reboot.known && reboot.percent==0);
    }
    reset(); gbs_scale_battery_cache_t cache=fresh(); observe(&cache,28,1);
    assert(gbs_scale_battery_cache_save(&cache,1,true)==ESP_OK);
    observe(&cache,20,60001); fail_commit=true;
    assert(gbs_scale_battery_cache_save(&cache,60001,true)==ESP_FAIL);
    fail_commit=false;
    gbs_scale_battery_cache_t reboot=fresh();
    assert(reboot.percent==28 && cache.percent==20);
}

static void invalid_records(void)
{
    for(unsigned invalid=0;invalid<8;invalid++) {
        reset(); gbs_scale_battery_cache_t cache=fresh(); observe(&cache,28,1);
        assert(gbs_scale_battery_cache_save(&cache,1,true)==ESP_OK);
        uint32_t bad;
        switch(invalid) {
        case 0: fail_open=true; break;
        case 1: fail_read=true; break;
        case 2: durable_size--; break;
        case 3: bad=2; memcpy(durable,&bad,4); break;
        case 4: bad=101; memcpy(durable+4,&bad,4); break;
        case 5: memset(durable+16,'x',GBS_SCALE_BATTERY_SERIAL_SIZE); break;
        case 6: durable[16]='z'; break;
        case 7: { int64_t bad_time=-1; memcpy(durable+8,&bad_time,8); break; }
        }
        gbs_scale_battery_cache_t reboot=fresh(); assert(!reboot.known);
    }
    reset(); gbs_scale_battery_cache_t cache=fresh(); observe(&cache,100,1);
    assert(gbs_scale_battery_cache_save(&cache,1,true)==ESP_OK);
    gbs_scale_battery_cache_t reboot={0};
    gbs_scale_battery_cache_bind(&reboot,"synthetic-a",1,WALL-1);
    assert(reboot.known && reboot.percent==100 && !reboot.observed_unix);
}

int main(void)
{
    accepted_sources(); rejected_sources(); persistence_and_restore(); binding_changes();
    write_failures(); invalid_records();
    puts("Scale battery history cache PASS (real cache + NVS fault stubs)");
    return 0;
}
