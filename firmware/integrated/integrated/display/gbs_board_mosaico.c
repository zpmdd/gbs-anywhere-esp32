#include "gbs_board_amoled216.h"
#include "gbs_mosaico_battery.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#ifdef GBS_CUE_PCM_HEADER
#include GBS_CUE_PCM_HEADER
#endif

static const char *TAG="mosaico";
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_touch_handle_t touch;
static i2c_master_dev_handle_t gauge;
static esp_codec_dev_handle_t codec;
static gbs_rotation_policy_t rotation;
static gbs_device_status_t battery;
static uint16_t battery_flags;
static float acceleration[3];
static int64_t acceleration_ms;
static bool acceleration_valid;
static SemaphoreHandle_t sensor_mutex;
static StaticSemaphore_t sensor_mutex_storage;
static StaticTask_t sensor_task_storage,tone_task_storage;
static StackType_t sensor_stack[4096/sizeof(StackType_t)],tone_stack[4096/sizeof(StackType_t)];
static QueueHandle_t tone_queue;
static StaticQueue_t tone_queue_storage;
static uint8_t tone_queue_buffer[4*sizeof(gbs_tone_t)];
static atomic_uint audio_volume_percent,audio_played[4],audio_errors;

static esp_err_t command(uint8_t reg,uint8_t value)
{
    return esp_lcd_panel_io_tx_param(lcd_io,(0x02u<<24)|((uint32_t)reg<<8),&value,1);
}

static gbs_orientation_t render_orientation(gbs_orientation_t physical)
{
    /* With USB right, the Mosaico native panel is 90 degrees counterclockwise
     * from the UI. Keep gravity/status physical and correct panel/touch together. */
    return (gbs_orientation_t)(((unsigned)physical+3u)%4u);
}

static esp_err_t set_panel_orientation(gbs_orientation_t physical)
{
    return command(0x36,gbs_device_panel_madctl(render_orientation(physical)));
}

esp_err_t gbs_board_amoled216_brightness(unsigned percent)
{
    if (percent>100) return ESP_ERR_INVALID_ARG;
    return bsp_display_brightness_set((int)percent);
}

/* This getter is used only by the display's raw-touch coordinate mapping. */
gbs_orientation_t gbs_board_amoled216_orientation(void) { return render_orientation(rotation.current); }

bool gbs_board_amoled216_touch_active(void)
{
    return gpio_get_level(BSP_LCD_TOUCH_INT)==0;
}

bool gbs_board_amoled216_acceleration(float xyz[3])
{
    if (!xyz || !sensor_mutex || xSemaphoreTake(sensor_mutex,0)!=pdTRUE) return false;
    int64_t now=esp_timer_get_time()/1000;
    bool valid=acceleration_valid && acceleration_ms>0 && now>=acceleration_ms && now-acceleration_ms<=150;
    if (valid) memcpy(xyz,acceleration,sizeof(acceleration));
    xSemaphoreGive(sensor_mutex);
    return valid;
}

bool gbs_board_amoled216_pmu_diagnostics(gbs_pmu_diagnostics_t *snapshot)
{
    if (!snapshot || !sensor_mutex || xSemaphoreTake(sensor_mutex,pdMS_TO_TICKS(20))!=pdTRUE) return false;
    /* Legacy raw00/raw01 fields contain BQ27220 BatteryStatus low/high bytes. */
    snapshot->raw00=(uint8_t)battery_flags;
    snapshot->raw01=(uint8_t)(battery_flags>>8);
    snapshot->decoded=battery;
    xSemaphoreGive(sensor_mutex);
    gbs_mosaico_battery_update(&snapshot->decoded,false,0,0,0,0,esp_timer_get_time()/1000);
    return true;
}

gbs_audio_diagnostics_t gbs_board_amoled216_audio_diagnostics(void)
{
    return (gbs_audio_diagnostics_t){.success=atomic_load(&audio_played[GBS_TONE_SUCCESS]),
        .failure=atomic_load(&audio_played[GBS_TONE_FAILURE]),
        .notice=atomic_load(&audio_played[GBS_TONE_NOTICE]),.errors=atomic_load(&audio_errors),
        .volume_percent=atomic_load(&audio_volume_percent)};
}

bool gbs_board_amoled216_buttons(int64_t now,bool blank,
    gbs_button_event_t *user_key,gbs_button_event_t *boot_key)
{
    static gbs_button_policy_t user_policy,boot_policy;
    *user_key=gbs_device_button_update(&user_policy,gpio_get_level(BSP_BUTTON_AI_GPIO)==0,blank,now);
    *boot_key=gbs_device_button_update(&boot_policy,gpio_get_level(BSP_BUTTON_BOOT_GPIO)==0,blank,now);
    return user_policy.raw_down || user_policy.down || boot_policy.raw_down || boot_policy.down;
}

void gbs_board_amoled216_deinit(void)
{
    /* BSP owns its static handles and has no display release API. Keep them
     * valid until reboot when startup fails; do not free borrowed handles. */
    if (panel) {
        (void)bsp_display_brightness_set(0);
        (void)bsp_display_off();
    }
    (void)gpio_set_level(BSP_AUDIO_PA_CTRL,0);
}

esp_err_t gbs_board_amoled216_init(size_t transfer_bytes,
    esp_lcd_panel_io_color_trans_done_cb_t done,
    esp_lcd_panel_handle_t *out_panel,esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(out_panel && out_touch && done && transfer_bytes>0 &&
        transfer_bytes<=480u*8u*2u,ESP_ERR_INVALID_ARG,TAG,"invalid display buffers");
    *out_panel=NULL; *out_touch=NULL;
    esp_err_t ret;
    gbs_rotation_policy_init(&rotation);
    bsp_board_variant_t variant;
    ESP_RETURN_ON_ERROR(bsp_board_variant_get(&variant),TAG,"detect board revision");
    ESP_RETURN_ON_ERROR(bsp_power_init(),TAG,"board power");
    const gpio_config_t amp={.pin_bit_mask=1ULL<<BSP_AUDIO_PA_CTRL,.mode=GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_AUDIO_PA_CTRL,0),TAG,"mute amplifier");
    ESP_RETURN_ON_ERROR(gpio_config(&amp),TAG,"amplifier GPIO");
    const gpio_config_t keys={.pin_bit_mask=(1ULL<<BSP_BUTTON_AI_GPIO)|(1ULL<<BSP_BUTTON_BOOT_GPIO),
        .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&keys),TAG,"button GPIO");
    ESP_GOTO_ON_ERROR(bsp_display_new(NULL,&panel),failed,TAG,"CO5300 panel");
    lcd_io=bsp_display_get_panel_io();
    const esp_lcd_panel_io_callbacks_t callbacks={.on_color_trans_done=done};
    ESP_GOTO_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(lcd_io,&callbacks,NULL),failed,TAG,"LCD completion");
    ESP_GOTO_ON_ERROR(bsp_display_brightness_set(0),failed,TAG,"initial brightness");
    ESP_GOTO_ON_ERROR(set_panel_orientation(rotation.current),failed,TAG,"initial panel orientation");
    /* Keep touch in raw panel coordinates; the shared UI maps it once. */
    ESP_GOTO_ON_ERROR(bsp_touch_new(BSP_DISPLAY_ROTATE_0,&touch),failed,TAG,"CST9220 touch");
    *out_panel=panel; *out_touch=touch;
    return ESP_OK;
failed:
    gbs_board_amoled216_deinit();
    return ret;
}

static bool read_gauge(uint8_t reg,uint16_t *value)
{
    uint8_t data[2];
    if (!gauge || i2c_master_transmit_receive(gauge,&reg,1,data,sizeof(data),25)!=ESP_OK) return false;
    *value=(uint16_t)data[0]|((uint16_t)data[1]<<8);
    return true;
}

static bool init_imu(void)
{
    if (bsp_imu_deinit()!=ESP_OK) return false;
    return bsp_imu_init()==ESP_OK && bsp_imu_start(NULL)==ESP_OK;
}

static void sensors_task(void *unused)
{
    (void)unused;
    const i2c_device_config_t config={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=BSP_BATTERY_I2C_ADDR,.scl_speed_hz=400000};
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(),&config,&gauge)!=ESP_OK)
        ESP_LOGW(TAG,"BQ27220 unavailable; battery status is unknown");
    /* bsp_battery_init writes an 80 mAh profile. Read only, preserving calibration. */
    bool imu_ready=init_imu();
    if (!imu_ready) ESP_LOGW(TAG,"BMI270 unavailable; keeping the locked orientation");
    int64_t next_battery=0,next_imu_retry=0;
    for (;;) {
        int64_t now=esp_timer_get_time()/1000;
        if (!imu_ready && now>=next_imu_retry) {
            imu_ready=init_imu(); next_imu_retry=now+5000;
        }
        if (now>=next_battery) {
            uint16_t flags=0,current=0,operation=0,percent=0;
            bool valid=read_gauge(0x0A,&flags) && read_gauge(0x0C,&current) &&
                read_gauge(0x3A,&operation) && read_gauge(0x2C,&percent);
            xSemaphoreTake(sensor_mutex,portMAX_DELAY);
            gbs_mosaico_battery_update(&battery,valid,flags,(int16_t)current,operation,percent,now);
            if (valid) battery_flags=flags;
            xSemaphoreGive(sensor_mutex);
            next_battery=now+2000;
        }
        float xyz[3]={0};
        bool valid=imu_ready && bsp_imu_get_accel(&xyz[0],&xyz[1],&xyz[2])==ESP_OK;
        xSemaphoreTake(sensor_mutex,portMAX_DELAY);
        /* Expose the mapped axes in diagnostics for four-orientation calibration. */
        acceleration[0]=-xyz[0]; acceleration[1]=xyz[1]; acceleration[2]=xyz[2];
        acceleration_ms=esp_timer_get_time()/1000; acceleration_valid=valid;
        xSemaphoreGive(sensor_mutex);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool audio_init(void)
{
    if (!codec) codec=bsp_audio_codec_speaker_init();
    return codec!=NULL;
}

static void audio_cleanup(void)
{
    /* BSP retains its codec and I2S handles; close playback without deleting them. */
    if (codec) {
        esp_codec_dev_set_out_mute(codec,true);
        esp_codec_dev_close(codec);
    }
    gpio_set_level(BSP_AUDIO_PA_CTRL,0);
}

static bool play_note(unsigned frequency, unsigned milliseconds)
{
    int16_t pcm[160*2];
    unsigned samples=milliseconds*16;
    const float fade_samples=192.0f; /* 12 ms at 16 kHz. */
    for (unsigned offset=0;offset<samples;offset+=160) {
        unsigned volume=atomic_load(&audio_volume_percent);
        if (!volume) return false;
        float amplitude=25000.0f*volume/100.0f;
        unsigned count=samples-offset<160?samples-offset:160;
        for (unsigned i=0;i<count;i++) {
            unsigned n=offset+i;
            float envelope=fminf(1.0f,fminf(n/fade_samples,(samples-1-n)/fade_samples));
            int16_t sample=(int16_t)(amplitude*envelope*sinf(6.28318530718f*frequency*n/16000.0f));
            pcm[i*2]=pcm[i*2+1]=sample;
        }
        size_t bytes=count*2*sizeof(int16_t);
        if (esp_codec_dev_write(codec,pcm,(int)bytes)!=ESP_CODEC_DEV_OK) return false;
    }
    return true;
}

#ifdef GBS_CUE_PCM_HEADER
/* Optional build-local 16 kHz signed mono PCM; no heap allocation or decoding. */
static bool play_pcm(const int16_t *samples, size_t sample_count)
{
    int16_t pcm[160*2];
    for (size_t offset=0;offset<sample_count;offset+=160) {
        unsigned volume=atomic_load(&audio_volume_percent);
        if (!volume) return false;
        size_t count=sample_count-offset<160?sample_count-offset:160;
        for (size_t i=0;i<count;i++) {
            int16_t sample=(int16_t)((int32_t)samples[offset+i]*(int32_t)volume/100);
            pcm[i*2]=pcm[i*2+1]=sample;
        }
        size_t bytes=count*2*sizeof(int16_t);
        if (esp_codec_dev_write(codec,pcm,(int)bytes)!=ESP_CODEC_DEV_OK) return false;
    }
    return true;
}
#endif

static bool play_success(void)
{
#ifdef GBS_CUE_PCM_HEADER
    return play_pcm(gbs_success_pcm,sizeof(gbs_success_pcm)/sizeof(gbs_success_pcm[0]));
#else
    return play_note(880,240)&&play_note(0,90)&&play_note(1320,320);
#endif
}

static bool play_failure(void)
{
#ifdef GBS_CUE_PCM_HEADER
    return play_pcm(gbs_failure_pcm,sizeof(gbs_failure_pcm)/sizeof(gbs_failure_pcm[0]));
#else
    return play_note(784,260)&&play_note(0,100)&&play_note(523,400);
#endif
}

static void audio_task(void *unused)
{
    (void)unused;
    int64_t retry_after=0;
    for (;;) {
        gbs_tone_t tone;
        xQueueReceive(tone_queue,&tone,portMAX_DELAY);
        if (!atomic_load(&audio_volume_percent) || esp_timer_get_time()/1000<retry_after) continue;
        if (!codec && !audio_init()) {
            atomic_fetch_add(&audio_errors,1);
            ESP_LOGW(TAG,"ES8311 output unavailable; skipping tone");
            retry_after=esp_timer_get_time()/1000+30000; continue;
        }
        esp_codec_dev_sample_info_t format={.sample_rate=16000,.channel=2,.bits_per_sample=16};
        /* Keep unity codec gain. 80% reproduces the previous 20000 PCM peak;
         * the 100% peak of 25000 still leaves headroom below int16 clipping. */
        bool ok=esp_codec_dev_open(codec,&format)==ESP_CODEC_DEV_OK &&
            esp_codec_dev_set_out_vol(codec,100)==ESP_CODEC_DEV_OK &&
            esp_codec_dev_set_out_mute(codec,false)==ESP_CODEC_DEV_OK;
        /* Write silence through DMA between notes; a task delay could let
         * the previous DMA buffer repeat instead of producing a quiet gap. */
        if (ok) ok=tone==GBS_TONE_SUCCESS ? play_success() :
            tone==GBS_TONE_NOTICE ? play_note(1046,240) : play_failure();
        /* Drain the BSP's default DMA queue before closing the DAC. */
        if (ok && atomic_load(&audio_volume_percent)) vTaskDelay(pdMS_TO_TICKS(150));
        esp_codec_dev_set_out_mute(codec,true);
        /* Keep the BSP-owned interfaces reusable for the next cue. */
        audio_cleanup();
        if(atomic_load(&audio_volume_percent)) {
            if(ok) atomic_fetch_add(&audio_played[tone],1);
            else { atomic_fetch_add(&audio_errors,1); ESP_LOGW(TAG,"Tone output interrupted"); }
        }
    }
}

static void audio_enqueue(gbs_tone_t tone)
{
    if(tone<GBS_TONE_SUCCESS||tone>GBS_TONE_NOTICE) return;
    if(xQueueSend(tone_queue,&tone,0)!=pdTRUE) {
        gbs_tone_t oldest;
        xQueueReceive(tone_queue,&oldest,0);
        xQueueSend(tone_queue,&tone,0);
    }
}

void gbs_board_amoled216_start_services(void)
{
    if (sensor_mutex) return;
    sensor_mutex=xSemaphoreCreateMutexStatic(&sensor_mutex_storage);
    tone_queue=xQueueCreateStatic(4,sizeof(gbs_tone_t),tone_queue_buffer,&tone_queue_storage);
    configASSERT(sensor_mutex && tone_queue);
    TaskHandle_t sensors=xTaskCreateStatic(sensors_task,"gbs_sensors",sizeof(sensor_stack),NULL,1,sensor_stack,&sensor_task_storage);
    TaskHandle_t audio=xTaskCreateStatic(audio_task,"gbs_tone",sizeof(tone_stack),NULL,1,tone_stack,&tone_task_storage);
    configASSERT(sensors && audio);
}

bool gbs_board_amoled216_tick(int64_t now, bool touching, bool auto_rotate,
    unsigned volume_percent, gbs_tone_t tone, gbs_device_status_t *status)
{
    /* The active-low touch IRQ also covers a press before LVGL's next poll. */
    touching=touching || gbs_board_amoled216_touch_active();
    if (volume_percent>100) volume_percent=100;
    atomic_store(&audio_volume_percent,volume_percent);
    if (!volume_percent) xQueueReset(tone_queue);
    else audio_enqueue(tone);
    float xyz[3]; int64_t sampled; bool valid;
    xSemaphoreTake(sensor_mutex,portMAX_DELAY);
    *status=battery; memcpy(xyz,acceleration,sizeof(xyz)); sampled=acceleration_ms; valid=acceleration_valid;
    xSemaphoreGive(sensor_mutex);
    // A sensor sample can be newer than the display loop's entry timestamp.
    now=esp_timer_get_time()/1000;
    gbs_mosaico_battery_update(status,false,0,0,0,0,now);
    static int64_t processed_sample;
    bool changed=false;
    /* Process one gravity decision per fresh sample. Touch/lock cancel any
     * candidate immediately, including between sensor samples. */
    if (sampled!=processed_sample || touching || !auto_rotate || now-sampled>150) {
        gbs_orientation_t before=rotation.current;
        gbs_orientation_t next=gbs_rotation_policy_update(&rotation,auto_rotate,touching,
            valid && sampled>0 && now>=sampled && now-sampled<=150,xyz[0],xyz[1],xyz[2],sampled);
        processed_sample=sampled;
        if (next!=before) {
            if (set_panel_orientation(next)==ESP_OK) changed=true;
            else { rotation.current=before; rotation.candidate_valid=false; }
        }
    }
    status->orientation=rotation.current;
    return changed;
}
