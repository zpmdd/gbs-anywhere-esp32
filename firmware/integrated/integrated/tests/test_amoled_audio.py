#!/usr/bin/env python3
"""Exercise the board's actual PCM renderer and volume path without hardware."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile


display = Path(__file__).resolve().parents[1] / "display"
source = (display / "gbs_board_amoled216.c").read_text()
header = (display / "gbs_board_amoled216.h").read_text()
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
tick = "bool gbs_board_amoled216_tick(" + source.split("bool gbs_board_amoled216_tick(", 1)[1]
diagnostics_type = re.search(r"typedef struct \{[^{}]*\} gbs_audio_diagnostics_t;", header).group(0)
diagnostics = "gbs_audio_diagnostics_t gbs_board_amoled216_audio_diagnostics(" + source.split(
    "gbs_audio_diagnostics_t gbs_board_amoled216_audio_diagnostics(", 1
)[1].split("\nbool gbs_board_amoled216_buttons(", 1)[0]
pcm_include = re.search(r"#ifdef GBS_CUE_PCM_HEADER\n#include GBS_CUE_PCM_HEADER\n#endif", source).group(0)
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
#include <string.h>
#include "gbs_device_policy.h"
""" + pcm_include + r"""

#define ESP_OK 0
#define pdTRUE 1
static atomic_uint audio_volume_percent;
static atomic_uint audio_played[4], audio_errors;
static void *audio_tx;
static int16_t captured[65536];
static unsigned frames, writes, cancel_after, change_after, next_volume;
static int failure_mode;
static void *codec, *codec_if, *codec_ctrl, *codec_gpio, *codec_data;
static bool channel_enabled, codec_closed, muted;
static unsigned live_objects, init_calls, drain_calls, queued;
static int init_failure, open_failure, tone_queue, sensor_mutex;
static bool transient_init_failure;
static int64_t fake_time, receive_step_us;
static gbs_tone_t queued_tones[4];
static gbs_rotation_policy_t rotation;
static gbs_device_status_t battery;
static float acceleration[3];
static int64_t acceleration_ms;
static bool acceleration_valid;
static jmp_buf worker_done;
static const char *TAG="test";
#define ESP_CODEC_DEV_OK 0
#define portMAX_DELAY 999
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(tag,message) do { (void)(tag); (void)(message); } while (0)
typedef struct { int sample_rate,channel,bits_per_sample; } esp_codec_dev_sample_info_t;
static void set_volume(unsigned volume,gbs_tone_t tone);
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
static int gpio_get_level(int pin) { assert(pin==11); return 1; }
static int command(uint8_t reg,uint8_t value) { assert(reg==0x36); (void)value; return ESP_OK; }
static void xSemaphoreTake(int mutex,int timeout) { assert(mutex==sensor_mutex && timeout==portMAX_DELAY); }
static void xSemaphoreGive(int mutex) { assert(mutex==sensor_mutex); }
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
static void xQueueReset(int queue) { assert(queue==tone_queue); queued=0; }
static int xQueueReceive(int queue,gbs_tone_t *tone,int timeout) {
    (void)queue;
    if(timeout==portMAX_DELAY) {
        assert(live_objects==0 && !audio_tx && !codec && !codec_if && !codec_ctrl && !codec_gpio && !codec_data);
        if(!queued) longjmp(worker_done,1);
        fake_time+=receive_step_us;
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
    if (init_failure==1) {
        if(transient_init_failure) init_failure=0;
        audio_cleanup(); return false;
    }
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
        assert(pcm[i]>=-25000 && pcm[i]<=25000);
        assert(frames<sizeof(captured)/sizeof(captured[0]));
        captured[frames++]=pcm[i];
    }
    if (change_after && writes==change_after) set_volume(next_volume,GBS_TONE_NONE);
    if (cancel_after && writes==cancel_after) set_volume(0,GBS_TONE_NONE);
    return ESP_OK;
}
""" + diagnostics_type + diagnostics + renderer + worker + enqueue + tick + r"""
static void set_volume(unsigned volume,gbs_tone_t tone)
{
    gbs_device_status_t status;
    assert(!gbs_board_amoled216_tick(fake_time/1000,false,false,volume,tone,&status));
    unsigned expected=volume>100?100:volume;
    assert(atomic_load(&audio_volume_percent)==expected);
    assert(gbs_board_amoled216_audio_diagnostics().volume_percent==expected);
}

static bool play_sequence(int tone)
{
    bool ok=true;
""" + sequence + r"""
    return ok;
}

static void reset(void)
{
    assert(!live_objects);
    frames=writes=cancel_after=queued=change_after=next_volume=0; failure_mode=0;
    init_failure=open_failure=0; init_calls=drain_calls=0;
    transient_init_failure=false; receive_step_us=1000000; fake_time=1000000;
    set_volume(80,GBS_TONE_NONE);
    for(unsigned i=0;i<4;++i) atomic_store(&audio_played[i],0);
    atomic_store(&audio_errors,0);
}

static void run_worker(unsigned count,int tone)
{
    assert(count<=4); queued=count; for(unsigned i=0;i<count;++i) queued_tones[i]=tone;
    if (!setjmp(worker_done)) audio_task(NULL);
    assert(live_objects==0 && !audio_tx && !codec && !codec_if && !codec_ctrl && !codec_gpio && !codec_data);
    if(init_failure||open_failure||failure_mode) assert(atomic_load(&audio_errors)>0&&!atomic_load(&audio_played[tone]));
    else if(cancel_after||!atomic_load(&audio_volume_percent)) assert(!atomic_load(&audio_played[tone]));
    else assert(atomic_load(&audio_played[tone])==count&&!atomic_load(&audio_errors));
}

static void check_sample(unsigned offset,unsigned frequency,unsigned count,unsigned n,unsigned volume)
{
    int value=captured[offset+n];
    double ramp=fmin(1.0,fmin(n/192.0,(count-1-n)/192.0));
    double expected=25000.0*volume/100.0*ramp*sin(6.283185307179586*frequency*n/16000.0);
    assert(fabs(value-expected)<10.0);
    if(volume==80) {
        /* Exact pre-volume float expression, rather than an approximate
         * loudness comparison: every old waveform sample must match. */
        float envelope=fminf(1.0f,fminf(n/192.0f,(count-1-n)/192.0f));
        int16_t legacy=(int16_t)(20000.0f*envelope*sinf(6.28318530718f*frequency*n/16000.0f));
        assert(value==legacy);
    }
}

static void check_note(unsigned offset, unsigned frequency, unsigned milliseconds,unsigned volume)
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
        check_sample(offset,frequency,count,n,volume);
    }
    unsigned amplitude=25000*volume/100;
    if (frequency) assert(peak>=amplitude-10 && peak<=amplitude);
}

int main(void)
{
#ifdef GBS_CUE_PCM_HEADER
    const unsigned sample_count=sizeof(gbs_success_pcm)/sizeof(gbs_success_pcm[0]);
    const unsigned failure_count=sizeof(gbs_failure_pcm)/sizeof(gbs_failure_pcm[0]);
    assert(sample_count==321&&failure_count==177);
    const unsigned volumes[]={0,30,80,100};
    for(unsigned i=0;i<sizeof(volumes)/sizeof(volumes[0]);i++) {
        unsigned volume=volumes[i];
        for(gbs_tone_t tone=GBS_TONE_SUCCESS;tone<=GBS_TONE_FAILURE;tone++) {
            const int16_t *samples=tone==GBS_TONE_SUCCESS?gbs_success_pcm:gbs_failure_pcm;
            unsigned count=tone==GBS_TONE_SUCCESS?sample_count:failure_count;
            reset(); set_volume(volume,GBS_TONE_NONE);
            assert(play_sequence(tone)==(volume!=0));
            assert(frames==(volume?count:0));
            assert(writes==(volume?(count+159)/160:0));
            for(unsigned n=0;n<frames;n++) {
                /* Signed division must truncate negative fractional samples
                 * toward zero, without an unsigned multiplication conversion. */
                assert(captured[n]==(int32_t)samples[n]*(int32_t)volume/100);
            }
        }
    }
    reset(); set_volume(30,GBS_TONE_NONE); change_after=1; next_volume=80;
    assert(play_sequence(GBS_TONE_SUCCESS)&&frames==sample_count&&writes==3);
    for(unsigned n=0;n<frames;n++) {
        int32_t volume=n<160?30:80;
        assert(captured[n]==(int32_t)gbs_success_pcm[n]*volume/100);
    }
    reset(); cancel_after=1;
    assert(!play_sequence(GBS_TONE_SUCCESS)&&frames==160&&writes==1);
    reset(); failure_mode=1;
    assert(!play_sequence(GBS_TONE_SUCCESS)&&writes==1&&!frames);
    reset(); failure_mode=2;
    assert(!play_sequence(GBS_TONE_SUCCESS)&&writes==1&&!frames);
    reset(); set_volume(30,GBS_TONE_NONE); run_worker(2,GBS_TONE_SUCCESS);
    assert(frames==2*sample_count&&init_calls==2&&drain_calls==2&&muted);
    reset(); set_volume(30,GBS_TONE_NONE); run_worker(2,GBS_TONE_FAILURE);
    assert(frames==2*failure_count&&init_calls==2&&drain_calls==2&&muted);
    reset(); cancel_after=1; run_worker(2,GBS_TONE_FAILURE);
    assert(frames==160&&init_calls==1&&!drain_calls&&!queued&&muted);
    reset(); cancel_after=1; run_worker(4,GBS_TONE_SUCCESS);
    assert(frames==160&&init_calls==1&&!drain_calls&&!queued&&muted);
    assert(!atomic_load(&audio_errors));
    reset(); failure_mode=1; run_worker(1,GBS_TONE_SUCCESS);
    assert(init_calls==1&&!drain_calls&&muted);
    reset(); failure_mode=2; run_worker(1,GBS_TONE_SUCCESS);
    assert(init_calls==1&&!drain_calls&&muted);
    reset(); failure_mode=2; run_worker(1,GBS_TONE_FAILURE);
    assert(init_calls==1&&!drain_calls&&muted);
    reset(); open_failure=1; run_worker(1,GBS_TONE_SUCCESS);
    assert(init_calls==1&&!writes&&muted);
    reset(); init_failure=1; run_worker(2,GBS_TONE_SUCCESS);
    assert(init_calls==1&&!writes);
    reset(); set_volume(0,GBS_TONE_NONE); run_worker(1,GBS_TONE_SUCCESS);
    assert(!init_calls&&!writes);
    /* Optional success/failure samples leave the neutral cue intact. */
    reset(); assert(play_sequence(GBS_TONE_NOTICE)&&frames==3840); check_note(0,1046,240,80);
    puts("Actual optional PCM: distinct success/failure, signed 0/30/80/100 scaling, tail blocks, cancellation, faults and neutral cue passed");
#else
    reset(); assert(play_note(0,60)); assert(frames==960); check_note(0,0,60,80);
    const unsigned volumes[]={40,80,100};
    for(unsigned i=0;i<sizeof(volumes)/sizeof(volumes[0]);i++) {
        unsigned volume=volumes[i];
        reset(); set_volume(volume,GBS_TONE_NONE);
        assert(play_sequence(GBS_TONE_SUCCESS)); assert(frames==10400);
        check_note(0,880,240,volume); check_note(3840,0,90,volume); check_note(5280,1320,320,volume);
        reset(); set_volume(volume,GBS_TONE_NONE);
        assert(play_sequence(GBS_TONE_FAILURE)); assert(frames==12160);
        check_note(0,784,260,volume); check_note(4160,0,100,volume); check_note(5760,523,400,volume);
        reset(); set_volume(volume,GBS_TONE_NONE);
        assert(play_sequence(GBS_TONE_NOTICE)); assert(frames==3840); check_note(0,1046,240,volume);
    }
    reset(); set_volume(0,GBS_TONE_NONE);
    assert(!play_note(880,180) && writes==0 && frames==0);
    assert(!play_sequence(GBS_TONE_SUCCESS) && !play_sequence(GBS_TONE_FAILURE) &&
        !play_sequence(GBS_TONE_NOTICE) && writes==0);
    reset(); change_after=1; next_volume=40;
    assert(play_note(880,180) && frames==2880);
    for(unsigned n=0;n<frames;n++) check_sample(0,880,frames,n,n<160?80:40);
    reset(); cancel_after=1;
    assert(!play_note(880,180) && writes==1 && frames==160);
    reset(); cancel_after=1;
    assert(!play_note(0,80) && writes==1 && frames==160);
    reset(); failure_mode=1; assert(!play_note(880,180) && writes==1);
    reset(); failure_mode=2; assert(!play_note(880,180) && writes==1);
    puts("Actual AMOLED PCM: 0/40/80/100 volume, exact legacy 80% waveform, live volume, mute and failures passed");
    reset(); run_worker(2,GBS_TONE_SUCCESS); assert(init_calls==2 && drain_calls==2 && frames==20800 && muted);
    reset(); run_worker(1,GBS_TONE_FAILURE); assert(init_calls==1 && drain_calls==1 && frames==12160 && muted);
    reset(); cancel_after=1; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && frames==160 && muted);
    reset(); failure_mode=1; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && muted);
    reset(); failure_mode=2; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && muted);
    reset(); open_failure=1; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && !drain_calls && writes==0 && muted);
    reset(); init_failure=1; run_worker(2,GBS_TONE_SUCCESS); assert(init_calls==1 && writes==0);
    reset(); init_failure=2; run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==1 && writes==0);
    reset(); set_volume(0,GBS_TONE_NONE); run_worker(1,GBS_TONE_SUCCESS); assert(init_calls==0 && writes==0);
    /* A failed initialization releases all objects, suppresses immediate
     * retries and can play the next cue after the existing 30 s cooldown. */
    reset(); init_failure=1; transient_init_failure=true; receive_step_us=31000000;
    queued=2; queued_tones[0]=queued_tones[1]=GBS_TONE_SUCCESS;
    if(!setjmp(worker_done)) audio_task(NULL);
    assert(init_calls==2&&drain_calls==1&&frames==10400&&live_objects==0);
    assert(atomic_load(&audio_errors)==1&&atomic_load(&audio_played[GBS_TONE_SUCCESS])==1);
    reset(); set_volume(101,GBS_TONE_SUCCESS); set_volume(UINT32_MAX,GBS_TONE_FAILURE);
    assert(queued==2); set_volume(0,GBS_TONE_NOTICE); assert(!queued);
    set_volume(40,GBS_TONE_NOTICE); assert(queued==1&&queued_tones[0]==GBS_TONE_NOTICE);
    if(!setjmp(worker_done)) audio_task(NULL);
    assert(init_calls==1&&frames==3840&&live_objects==0);
    check_note(0,1046,240,40);
    reset(); cancel_after=1; run_worker(4,GBS_TONE_SUCCESS);
    assert(!queued&&init_calls==1&&frames==160&&muted&&live_objects==0&&!atomic_load(&audio_errors));
    reset(); audio_enqueue(0); assert(!queued);
    audio_enqueue(GBS_TONE_NOTICE); audio_enqueue(GBS_TONE_SUCCESS);
    audio_enqueue(GBS_TONE_NOTICE); audio_enqueue(GBS_TONE_SUCCESS);
    assert(queued==4&&queued_tones[0]==GBS_TONE_NOTICE&&queued_tones[3]==GBS_TONE_SUCCESS);
    audio_enqueue(GBS_TONE_FAILURE); assert(queued==4&&queued_tones[0]==GBS_TONE_SUCCESS&&queued_tones[3]==GBS_TONE_FAILURE);
    if(!setjmp(worker_done)) audio_task(NULL);
    assert(init_calls==4&&drain_calls==4&&frames==36800&&live_objects==0);
    assert(atomic_load(&audio_played[GBS_TONE_SUCCESS])==2&&atomic_load(&audio_played[GBS_TONE_FAILURE])==1&&
        atomic_load(&audio_played[GBS_TONE_NOTICE])==1&&!atomic_load(&audio_errors));
    gbs_audio_diagnostics_t stats=gbs_board_amoled216_audio_diagnostics();
    assert(stats.success==2&&stats.failure==1&&stats.notice==1&&!stats.errors&&stats.volume_percent==80);
    puts("Actual board tick/audio worker: clamp, diagnostics, queue mute, resource cleanup and 30 s retry passed");
#endif
}
"""

with tempfile.TemporaryDirectory(prefix="gbs-audio-") as directory:
    c_file = Path(directory) / "test.c"
    c_file.write_text(harness)
    pcm_header = Path(directory) / "synthetic_cue_pcm.h"
    values = [-25000, -18001, -3, -1, 0, 1, 3, 18001, 25000]
    pcm_header.write_text("static const int16_t gbs_success_pcm[]={" +
                          ",".join(str(values[i % len(values)]) for i in range(321)) + "};\n" +
                          "static const int16_t gbs_failure_pcm[]={" +
                          ",".join(str(-values[(i+3) % len(values)]) for i in range(177)) + "};\n")
    for name, options in [
        ("default", []),
        ("optional-pcm", [f'-DGBS_CUE_PCM_HEADER="{pcm_header}"']),
    ]:
        executable = Path(directory) / name
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            *options, "-I", str(display), str(c_file), str(display / "gbs_device_policy.c"),
            "-lm", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
