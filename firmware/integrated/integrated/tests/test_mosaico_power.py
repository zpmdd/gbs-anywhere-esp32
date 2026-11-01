#!/usr/bin/env python3
"""Exercise production sleep/profile paths with offline peripheral faults."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
board = (root / "integrated/display/gbs_board_mosaico.c").read_text()
surface = (root / "main/hub_touchscreen.c").read_text()


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|void|esp_err_t) " + name + r"\([^)]*\)\s*\{", source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


harness = r'''
#include "gbs_mosaico_battery.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define BSP_BUTTON_AI_GPIO 7
#define ESP_EXT1_WAKEUP_ANY_LOW 0
#define ESP_EXT1_WAKEUP_ANY_HIGH 1
#define ESP_SLEEP_WAKEUP_EXT1 3
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
#define ESP_RETURN_ON_ERROR(x,...) do { int e=(x); if(e) return e; } while(0)
typedef struct { uint64_t pin_bit_mask; int mode,pull_up_en; } gpio_config_t;
static int tone_queue,setup_error,rail_error,wake_error,step,cleanup_calls,init_calls;
static unsigned battery_profile_attempts,init_failures,read_failures;
static int64_t now_ms,release_ms;
static uint32_t causes;
static uint16_t design;
static bool read_ok,grinder_busy,commands_busy,brewing,grinder_reserved,commands_reserved;
static atomic_bool sleep_requested,sensors_parked,audio_busy;
static atomic_uint audio_volume_percent;
static atomic_uint audio_played[4],audio_errors;
static bool sleep_waiting_release,wake_tone_pending;
static int wake_level,blank_calls,tone_calls,last_tone,codec=1;
static bool sound_ok=true;
#define ESP_CODEC_DEV_OK 0
typedef struct { int sample_rate,channel,bits_per_sample; } esp_codec_dev_sample_info_t;
static jmp_buf escaped;
static int64_t esp_timer_get_time(void) { return now_ms*1000; }
static void vTaskDelay(int ms) { now_ms+=ms; }
static int gpio_get_level(int pin) { assert(pin==7); return now_ms>=release_ms; }
static int gpio_config(const gpio_config_t *key) { assert(key->pin_bit_mask==(1u<<7)); return 0; }
static int rtc_gpio_hold_dis(int pin) { assert(pin==7); return 0; }
static int rtc_gpio_deinit(int pin) { assert(pin==7); return 0; }
static int rtc_gpio_init(int pin) { assert(pin==7); return 0; }
static int rtc_gpio_pulldown_dis(int pin) { assert(pin==7); return 0; }
static int rtc_gpio_pullup_en(int pin) { assert(pin==7); return 0; }
static uint32_t esp_sleep_get_wakeup_causes(void) { return causes; }
static int esp_sleep_enable_ext1_wakeup_io(uint64_t mask,int mode) { assert(mask==(1u<<7)); wake_level=mode; return wake_error; }
static int esp_sleep_disable_ext1_wakeup_io(uint64_t mask) { assert(mask==(1u<<7)); return 0; }
static void xQueueReset(int queue) { (void)queue; }
static void audio_cleanup(void) { ++cleanup_calls; }
static int bsp_display_brightness_set(int percent) { assert(percent==0); ++blank_calls; return 0; }
static bool audio_init(void) { codec=1; return true; }
static int esp_codec_dev_open(int handle,const esp_codec_dev_sample_info_t *format) { assert(handle && format->sample_rate==16000); return 0; }
static int esp_codec_dev_set_out_vol(int handle,int volume) { assert(handle && volume==100); return 0; }
static int esp_codec_dev_set_out_mute(int handle,bool mute) { assert(handle && !mute); return 0; }
static bool play_success(void) { ++tone_calls; last_tone=GBS_TONE_SUCCESS; return sound_ok; }
static bool play_failure(void) { assert(blank_calls); ++tone_calls; last_tone=GBS_TONE_FAILURE; return sound_ok; }
static bool play_note(unsigned hz,unsigned ms) { assert(hz==1046 && ms==240); ++tone_calls; last_tone=GBS_TONE_NOTICE; return sound_ok; }
static int bsp_imu_deinit(void) { assert(atomic_load(&sensors_parked)); return 0; }
static int bsp_power_prepare_sleep(void) { assert(step++==0); return rail_error; }
static int bsp_power_set_codec_3v3(bool on) { assert(!on && step++==1); return 0; }
static int bsp_power_set_vcc_3v3(bool on) { assert(!on && step++==2); return 0; }
static _Noreturn void bsp_power_enter_deep_sleep(void) { longjmp(escaped,1); }
static _Noreturn void esp_restart(void) { longjmp(escaped,2); }
static bool read_gauge(uint8_t reg,uint16_t *out) {
    assert(reg==0x3C); *out=design;
    if(read_failures) { --read_failures; return false; }
    return read_ok;
}
static int bsp_battery_init(void) {
    ++init_calls; design=80;
    if(init_failures) { --init_failures; return ESP_ERR_TIMEOUT; }
    return setup_error;
}
static bool hub_grinder_begin_maintenance(void) { if(grinder_busy) return false; grinder_reserved=true; return true; }
static void hub_grinder_end_maintenance(void) { assert(grinder_reserved); grinder_reserved=false; }
static bool hub_command_begin_sleep(void) { if(commands_busy) return false; commands_reserved=true; return true; }
static void hub_command_end_sleep(void) { assert(commands_reserved); commands_reserved=false; }
static bool hub_brew_timer_is_brewing(void) { return brewing; }
void hub_touchscreen_end_sleep(void);
'''
harness += "\n".join(function(board, name) for name in
                     ("ensure_battery_profile", "play_tone", "arm_sleep_button", "gbs_board_mosaico_check_wake", "gbs_board_mosaico_sleep"))
harness += "\n".join(function(surface, name) for name in
                     ("hub_touchscreen_begin_sleep", "hub_touchscreen_end_sleep"))
harness += r'''
int main(void)
{
    read_ok=true; design=3000;
    assert(ensure_battery_profile() && init_calls==1 && design==80);
    assert(ensure_battery_profile() && init_calls==2);
    design=65; assert(ensure_battery_profile() && init_calls==2);
    design=3000; setup_error=1; assert(!ensure_battery_profile() && init_calls==5 && battery_profile_attempts==3);
    assert(!ensure_battery_profile() && init_calls==8);
    design=0; assert(!ensure_battery_profile() && init_calls==8);
    read_ok=false; assert(!ensure_battery_profile());
    read_ok=true; design=80; setup_error=0; init_failures=1;
    int64_t before_retry=now_ms;
    assert(ensure_battery_profile() && init_calls==10 && battery_profile_attempts==2);
    assert(now_ms==before_retry+1000);
    read_failures=1;
    assert(ensure_battery_profile() && init_calls==11 && battery_profile_attempts==2);
    assert(ensure_battery_profile() && battery_profile_attempts==1);

    grinder_busy=true; assert(!hub_touchscreen_begin_sleep() && !commands_reserved);
    grinder_busy=false; commands_busy=true;
    assert(!hub_touchscreen_begin_sleep() && !grinder_reserved);
    commands_busy=false; brewing=true;
    assert(!hub_touchscreen_begin_sleep() && !grinder_reserved && !commands_reserved);
    brewing=false; assert(hub_touchscreen_begin_sleep());
    assert(grinder_reserved && commands_reserved); hub_touchscreen_end_sleep();

    release_ms=0; wake_error=3; assert(gbs_board_mosaico_sleep()==3 && !sleep_requested);
    wake_error=0; assert(gbs_board_mosaico_sleep()==ESP_ERR_TIMEOUT && !sleep_requested && !cleanup_calls);
    sensors_parked=true; audio_busy=true; assert(gbs_board_mosaico_sleep()==ESP_ERR_TIMEOUT && !cleanup_calls);
    audio_busy=false;
    release_ms=now_ms+10000; audio_volume_percent=53;
    int outcome=setjmp(escaped);
    if(!outcome) { gbs_board_mosaico_sleep(); assert(0); }
    assert(outcome==1 && step==3 && cleanup_calls==2);
    assert(sleep_waiting_release && wake_level==ESP_EXT1_WAKEUP_ANY_HIGH);
    assert(now_ms<release_ms && tone_calls==1 && last_tone==GBS_TONE_FAILURE && audio_played[GBS_TONE_FAILURE]==1);
    /* Releasing the sleep press must never boot the UI or play the wake cue. */
    causes=1u<<ESP_SLEEP_WAKEUP_EXT1; now_ms=release_ms;
    outcome=setjmp(escaped);
    if(!outcome) { gbs_board_mosaico_check_wake(); assert(0); }
    assert(outcome==1 && !sleep_waiting_release && !wake_tone_pending && wake_level==ESP_EXT1_WAKEUP_ANY_LOW);
    /* A separate short tap wakes without the old 1.5 s gate. */
    now_ms=0; release_ms=0; gbs_board_mosaico_check_wake();
    assert(wake_tone_pending && now_ms==0); wake_tone_pending=false;
    causes=0; gbs_board_mosaico_check_wake(); assert(!wake_tone_pending);
    /* Mute skips the sleep cue; preparation failure still restarts safely. */
    step=0; rail_error=1;
    outcome=setjmp(escaped);
    if(!outcome) { gbs_board_mosaico_sleep(); assert(0); }
    assert(outcome==2 && step==1 && tone_calls==1);

    audio_volume_percent=53; play_tone(GBS_TONE_SUCCESS);
    assert(audio_played[GBS_TONE_SUCCESS]==1 && last_tone==GBS_TONE_SUCCESS);
    sound_ok=false; play_tone(GBS_TONE_SUCCESS);
    assert(audio_played[GBS_TONE_SUCCESS]==1 && audio_errors==1);
    puts("Mosaico power: profile, admission, held-button sleep, silent release, short wake and cues passed");
    return 0;
}
'''

# Consume release-only wakes before any network or display initialization.
app = (root / "main/app_main.c").read_text()
assert app.index("gbs_board_mosaico_check_wake();") < app.index("configure_json_allocator();")
with tempfile.TemporaryDirectory(prefix="gbs-mosaico-power-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(root / "integrated/display"), str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
