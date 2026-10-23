#!/usr/bin/env python3
"""Exercise the board's actual PCM renderer without ESP-IDF or hardware."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / "display/gbs_board_amoled216.c").read_text()
# Compile the production function and sequence expression unchanged, replacing
# only the I2S transport. Keep this boundary explicit if the driver is refactored.
renderer = "static bool play_note(" + source.split("static bool play_note(", 1)[1].split(
    "\nstatic void audio_task(", 1
)[0]
sequence = re.search(r"if \(ok\) ok=tone==GBS_TONE_SUCCESS \? .*?;", source, re.S).group(0)
cleanup = "static void audio_cleanup(" + source.split("static void audio_cleanup(", 1)[1].split(
    "\nstatic bool audio_init(", 1
)[0]
worker = "static void audio_task(" + source.split("static void audio_task(", 1)[1].split(
    "\nstatic void audio_enqueue(", 1
)[0]
enqueue = "static void audio_enqueue(" + source.split("static void audio_enqueue(", 1)[1].split(
    "\nvoid gbs_board_amoled216_start_services(", 1
)[0]
assert "esp_codec_dev_set_out_vol(codec,100)" in source

harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#define ESP_OK 0
#define GBS_TONE_SUCCESS 1
#define GBS_TONE_FAILURE 2
#define GBS_TONE_NOTICE 3
#define pdTRUE 1
static atomic_bool sound_allowed;
static atomic_uint audio_played[4], audio_errors;
static void *audio_tx;
static int16_t captured[65536];
static unsigned frames, writes, cancel_after;
static int failure_mode;
static void *codec, *codec_if, *codec_ctrl, *codec_gpio, *codec_data;
static bool channel_enabled, codec_closed, muted;
static unsigned live_objects, init_calls, drain_calls, queued;
static int init_failure, open_failure, tone_queue;
static int64_t fake_time;
static int queued_tones[4];
static jmp_buf worker_done;
static const char *TAG="test";
#define ESP_CODEC_DEV_OK 0
#define portMAX_DELAY 999
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(tag,message) do { (void)(tag); (void)(message); } while (0)
typedef int gbs_tone_t;
typedef struct { int sample_rate,channel,bits_per_sample; } esp_codec_dev_sample_info_t;
static void *allocate(void) { void *p=malloc(1); assert(p); live_objects++; return p; }
static void release(void *p) { assert(p && live_objects); free(p); live_objects--; }
static void esp_codec_dev_close(void *p) { assert(p==codec); channel_enabled=false; codec_closed=true; }
static void esp_codec_dev_delete(void *p) { assert(codec_closed); release(p); }
static void audio_codec_delete_codec_if(void *p) { assert(!codec); release(p); }
static void audio_codec_delete_ctrl_if(void *p) { assert(!codec_if); release(p); }
static void audio_codec_delete_gpio_if(void *p) { assert(!codec_if); release(p); }
static void audio_codec_delete_data_if(void *p) { assert(!codec && !codec_if); release(p); }
static void i2s_channel_disable(void *p) { assert(p==audio_tx && channel_enabled); channel_enabled=false; }
static void i2s_del_channel(void *p) { assert(!channel_enabled && !codec && !codec_data); release(p); }
static void gpio_set_level(int pin,int value) { assert(pin==46 && value==0); }
static int esp_codec_dev_open(void *p,esp_codec_dev_sample_info_t *format) {
    assert(p==codec && channel_enabled); codec_closed=false;
    assert(format->sample_rate==16000 && format->channel==2 && format->bits_per_sample==16);
    return open_failure?-1:ESP_CODEC_DEV_OK;
}
static int esp_codec_dev_set_out_vol(void *p,int volume) { assert(p==codec && volume==100); return ESP_CODEC_DEV_OK; }
static int esp_codec_dev_set_out_mute(void *p,bool value) { assert(p==codec); muted=value; return ESP_CODEC_DEV_OK; }
static int64_t esp_timer_get_time(void) { return fake_time; }
static void vTaskDelay(unsigned delay) { assert(delay==50); drain_calls++; }
static int xQueueSend(int queue,const gbs_tone_t *tone,int timeout) {
    (void)queue; assert(timeout==0);
    if(queued==4) return 0;
    queued_tones[queued++]=*tone; return pdTRUE;
}
static int xQueueReceive(int queue,gbs_tone_t *tone,int timeout) {
    (void)queue;
    if(timeout==portMAX_DELAY) {
        assert(live_objects==0 && !audio_tx && !codec && !codec_if && !codec_ctrl && !codec_gpio && !codec_data);
        if(!queued) longjmp(worker_done,1);
        fake_time+=1000000;
    }
    if(!queued) return 0;
    *tone=queued_tones[0]; --queued;
    for(unsigned i=0;i<queued;++i) queued_tones[i]=queued_tones[i+1];
    return pdTRUE;
}

""" + cleanup + r"""
static bool audio_init(void) {
    init_calls++;
    audio_tx=allocate(); channel_enabled=true;
    if (init_failure==1) { audio_cleanup(); return false; }
    codec_ctrl=allocate(); codec_gpio=allocate(); codec_data=allocate(); codec_if=allocate();
    if (init_failure==2) { audio_cleanup(); return false; }
    codec=allocate(); return true;
}

static int i2s_channel_write(void *unused, const void *data, size_t bytes,
    size_t *written, unsigned timeout)
{
    (void)unused;
    assert(timeout==50 && bytes%4==0 && bytes<=160*4);
    writes++;
    if (failure_mode==1) return -1;
    *written=failure_mode==2?0:bytes;
    if (failure_mode==2) return ESP_OK;
    const int16_t *pcm=data;
    for (size_t i=0;i<bytes/sizeof(*pcm);i+=2) {
        assert(pcm[i]==pcm[i+1]);
        assert(pcm[i]>=-20000 && pcm[i]<=20000);
        assert(frames<sizeof(captured)/sizeof(captured[0]));
        captured[frames++]=pcm[i];
    }
    if (cancel_after && writes==cancel_after) atomic_store(&sound_allowed,false);
    return ESP_OK;
}
""" + renderer + worker + enqueue + r"""
static bool play_sequence(int tone)
{
    bool ok=true;
""" + sequence + r"""
    return ok;
}

static void reset(void)
{
    frames=writes=cancel_after=queued=0; failure_mode=0;
    init_failure=open_failure=0; init_calls=drain_calls=0;
    atomic_store(&sound_allowed,true);
    for(unsigned i=0;i<4;++i) atomic_store(&audio_played[i],0);
    atomic_store(&audio_errors,0);
}

static void run_worker(unsigned count,int tone)
{
    assert(count<=4); queued=count; for(unsigned i=0;i<count;++i) queued_tones[i]=tone;
    if (!setjmp(worker_done)) audio_task(NULL);
    assert(live_objects==0 && !audio_tx && !codec && !codec_if && !codec_ctrl && !codec_gpio && !codec_data);
    if(init_failure||open_failure||failure_mode) assert(atomic_load(&audio_errors)>0&&!atomic_load(&audio_played[tone]));
    else if(cancel_after||!atomic_load(&sound_allowed)) assert(!atomic_load(&audio_played[tone]));
    else assert(atomic_load(&audio_played[tone])==count&&!atomic_load(&audio_errors));
}

static void check_note(unsigned offset, unsigned frequency, unsigned milliseconds)
{
    unsigned count=milliseconds*16, peak=0;
    assert(captured[offset]==0 && captured[offset+count-1]==0);
    for (unsigned n=0;n<count;n++) {
        int value=captured[offset+n];
        unsigned magnitude=value<0?-value:value;
        if (!frequency) { assert(value==0); continue; }
        if (magnitude>peak) peak=magnitude;
        /* Independent double-precision reference checks frequency and the
         * 192-frame linear attack/release, including every chunk boundary. */
        double ramp=fmin(1.0,fmin(n/192.0,(count-1-n)/192.0));
        double expected=20000.0*ramp*sin(6.283185307179586*frequency*n/16000.0);
        assert(fabs(value-expected)<8.0);
    }
    if (frequency) assert(peak>=19990 && peak<=20000);
}

int main(void)
{
    reset(); assert(play_note(0,60)); assert(frames==960); check_note(0,0,60);
    reset(); assert(play_sequence(GBS_TONE_SUCCESS)); assert(frames==10400);
    check_note(0,880,240); check_note(3840,0,90); check_note(5280,1320,320);
    reset(); assert(play_sequence(GBS_TONE_FAILURE)); assert(frames==12160);
    check_note(0,784,260); check_note(4160,0,100); check_note(5760,523,400);
    reset(); assert(play_sequence(GBS_TONE_NOTICE)); assert(frames==3840); check_note(0,1046,240);
    reset(); atomic_store(&sound_allowed,false);
    assert(!play_note(880,180) && writes==0 && frames==0);
    reset(); cancel_after=1;
    assert(!play_note(880,180) && writes==1 && frames==160);
    reset(); cancel_after=1;
    assert(!play_note(0,80) && writes==1 && frames==160);
    reset(); failure_mode=1; assert(!play_note(880,180) && writes==1);
    reset(); failure_mode=2; assert(!play_note(880,180) && writes==1);
    puts("Actual AMOLED PCM: A sequence, silence, ramp, peak, duration, mute and transport failures passed");
    reset(); run_worker(2,GBS_TONE_SUCCESS); assert(init_calls==2 && drain_calls==2 && frames==20800 && muted);
    reset(); run_worker(1,GBS_TONE_FAILURE); assert(init_calls==1 && drain_calls==1 && frames==12160 && muted);
    reset(); cancel_after=1; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && frames==160 && muted);
    reset(); failure_mode=1; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && muted);
    reset(); failure_mode=2; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && muted);
    reset(); open_failure=1; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && writes==0 && muted);
    reset(); init_failure=1; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && writes==0);
    reset(); init_failure=2; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && writes==0);
    reset(); atomic_store(&sound_allowed,false); run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==0 && writes==0);
    reset(); audio_enqueue(0); assert(!queued);
    audio_enqueue(GBS_TONE_NOTICE); audio_enqueue(GBS_TONE_SUCCESS);
    audio_enqueue(GBS_TONE_NOTICE); audio_enqueue(GBS_TONE_SUCCESS);
    assert(queued==4&&queued_tones[0]==GBS_TONE_NOTICE&&queued_tones[3]==GBS_TONE_SUCCESS);
    audio_enqueue(GBS_TONE_FAILURE); assert(queued==4&&queued_tones[0]==GBS_TONE_SUCCESS&&queued_tones[3]==GBS_TONE_FAILURE);
    if(!setjmp(worker_done)) audio_task(NULL);
    assert(init_calls==4&&drain_calls==4&&frames==36800&&live_objects==0);
    assert(atomic_load(&audio_played[GBS_TONE_SUCCESS])==2&&atomic_load(&audio_played[GBS_TONE_FAILURE])==1&&
        atomic_load(&audio_played[GBS_TONE_NOTICE])==1&&!atomic_load(&audio_errors));
    puts("Actual audio worker/cleanup: FIFO node cues, bounded queue, mute and fault cleanup passed");
}
"""

with tempfile.TemporaryDirectory(prefix="gbs-audio-") as directory:
    c_file = Path(directory) / "test.c"
    executable = Path(directory) / "test"
    c_file.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        str(c_file), "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
