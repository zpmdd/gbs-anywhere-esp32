#include "gbs_board_amoled216.h"
#include "gbs_device_policy.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst9217.h"
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

static const char *TAG="amoled216";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t pmu, imu;
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t lcd_io, touch_io;
static esp_lcd_touch_handle_t touch;
static bool spi_ready;
static gbs_rotation_policy_t rotation;
static gbs_device_status_t battery;
static uint8_t pmu_raw00, pmu_raw01;
static float acceleration[3];
static int64_t acceleration_ms;
static bool acceleration_valid;
static SemaphoreHandle_t sensor_mutex;
static StaticSemaphore_t sensor_mutex_storage;
static StaticTask_t sensor_task_storage, tone_task_storage;
static StackType_t sensor_stack[4096 / sizeof(StackType_t)], tone_stack[4096 / sizeof(StackType_t)];
static QueueHandle_t tone_queue;
static StaticQueue_t tone_queue_storage;
static uint8_t tone_queue_buffer[4*sizeof(gbs_tone_t)];
static atomic_uint audio_volume_percent;
static atomic_uint audio_played[4], audio_errors;
static bool add_sensor(unsigned address, i2c_master_dev_handle_t *device);

/* Panel sequence and pins follow the Waveshare BSP 2.0.1 (Apache-2.0).
 * https://github.com/waveshareteam/Waveshare-ESP32-components/tree/bfeb6e6d5737178cdde78b630c8118074da0a657/bsp/esp32_s3_touch_amoled_2_16
 * Keep the board's initialization sequence; use only its panel/touch drivers,
 * so the application remains on LVGL 8.4 rather than importing the whole BSP. */
static const co5300_lcd_init_cmd_t panel_commands[]={
    {0x11,NULL,0,600},
    {0xFE,(uint8_t[]){0x20},1,0}, {0x19,(uint8_t[]){0x10},1,0},
    {0x1C,(uint8_t[]){0xA0},1,0}, {0xFE,(uint8_t[]){0x00},1,0},
    {0xC4,(uint8_t[]){0x80},1,0}, {0x3A,(uint8_t[]){0x55},1,0},
    {0x35,(uint8_t[]){0x00},1,0}, {0x53,(uint8_t[]){0x20},1,0},
    {0x51,(uint8_t[]){0x00},1,0}, {0x63,(uint8_t[]){0xFF},1,0},
    {0x2A,(uint8_t[]){0x00,0x00,0x01,0xDF},4,0},
    {0x2B,(uint8_t[]){0x00,0x00,0x01,0xDF},4,0},
    {0x36,(uint8_t[]){0x00},1,0}, {0x29,NULL,0,600},
};

static esp_err_t command(uint8_t reg, uint8_t value)
{
    /* tx_param drains queued pixel transfers before changing panel registers. */
    return esp_lcd_panel_io_tx_param(lcd_io, (0x02u<<24)|((uint32_t)reg<<8), &value, 1);
}

esp_err_t gbs_board_amoled216_brightness(unsigned percent)
{
    if (percent>100) return ESP_ERR_INVALID_ARG;
    return command(0x51,(uint8_t)(percent*255/100));
}

gbs_orientation_t gbs_board_amoled216_orientation(void) { return rotation.current; }
bool gbs_board_amoled216_touch_active(void) { return gpio_get_level(11)==0; }

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
    snapshot->raw00=pmu_raw00; snapshot->raw01=pmu_raw01; snapshot->decoded=battery;
    xSemaphoreGive(sensor_mutex);
    int64_t now=esp_timer_get_time()/1000;
    gbs_device_update_pmu(&snapshot->decoded,false,0,0,false,0,now);
    return true;
}

gbs_audio_diagnostics_t gbs_board_amoled216_audio_diagnostics(void)
{
    return (gbs_audio_diagnostics_t){.success=atomic_load(&audio_played[GBS_TONE_SUCCESS]),
        .failure=atomic_load(&audio_played[GBS_TONE_FAILURE]),
        .notice=atomic_load(&audio_played[GBS_TONE_NOTICE]),.errors=atomic_load(&audio_errors),
        .volume_percent=atomic_load(&audio_volume_percent)};
}

bool gbs_board_amoled216_buttons(int64_t now, bool blank,
    gbs_button_event_t *user_key, gbs_button_event_t *boot_key)
{
    static gbs_button_policy_t user_policy,boot_policy;
    *user_key=gbs_device_button_update(&user_policy,gpio_get_level(18)==0,blank,now);
    *boot_key=gbs_device_button_update(&boot_policy,gpio_get_level(0)==0,blank,now);
    return user_policy.raw_down || user_policy.down || boot_policy.raw_down || boot_policy.down;
}

void gbs_board_amoled216_deinit(void)
{
    /* Only used before service tasks start, on display initialization failure. */
    if (touch) { esp_lcd_touch_del(touch); touch=NULL; }
    if (touch_io) { esp_lcd_panel_io_del(touch_io); touch_io=NULL; }
    if (panel) { esp_lcd_panel_del(panel); panel=NULL; }
    if (lcd_io) { esp_lcd_panel_io_del(lcd_io); lcd_io=NULL; }
    if (spi_ready) { spi_bus_free(SPI2_HOST); spi_ready=false; }
    if (pmu) { i2c_master_bus_rm_device(pmu); pmu=NULL; }
    if (bus) { i2c_del_master_bus(bus); bus=NULL; }
}

esp_err_t gbs_board_amoled216_init(size_t transfer_bytes,
    esp_lcd_panel_io_color_trans_done_cb_t done,
    esp_lcd_panel_handle_t *out_panel, esp_lcd_touch_handle_t *out_touch)
{
    esp_err_t ret;
    gbs_rotation_policy_init(&rotation);
    const gpio_config_t amp={.pin_bit_mask=1ULL<<46,.mode=GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&amp),TAG,"amplifier GPIO");
    gpio_set_level(46,0);
    /* GPIO0 remains the ROM boot strap. Read it only after normal boot; do
     * not touch the separate AXP2101 PWRON key or its long-press settings. */
    const gpio_config_t keys={.pin_bit_mask=(1ULL<<18)|(1ULL<<0),.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&keys),TAG,"button GPIO");
    const i2c_master_bus_config_t i2c={.i2c_port=I2C_NUM_0,.sda_io_num=15,.scl_io_num=14,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    ESP_GOTO_ON_ERROR(i2c_new_master_bus(&i2c,&bus),failed,TAG,"I2C");
    if (!add_sensor(0x34,&pmu)) ESP_LOGW(TAG,"PMU interface unavailable");
    /* LCD, touch and IMU use DCDC1/VCC3V3, the same rail already powering
     * the CPU. The schematic shows no separate display rail to enable. */
    esp_lcd_panel_io_i2c_config_t ti=ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    ti.scl_speed_hz=400000;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(bus,&ti,&touch_io),failed,TAG,"touch IO");
    /* CST9220 is supported by the CST9217 driver used in Waveshare's BSP. */
    const esp_lcd_touch_config_t tc={.x_max=480,.y_max=480,.rst_gpio_num=40,.int_gpio_num=11};
    ESP_GOTO_ON_ERROR(esp_lcd_touch_new_i2c_cst9217(touch_io,&tc,&touch),failed,TAG,"CST9220");
    const spi_bus_config_t spi=CO5300_PANEL_BUS_QSPI_CONFIG(38,4,5,6,7,transfer_bytes);
    ESP_GOTO_ON_ERROR(spi_bus_initialize(SPI2_HOST,&spi,SPI_DMA_CH_AUTO),failed,TAG,"QSPI");
    spi_ready=true;
    esp_lcd_panel_io_spi_config_t io=CO5300_PANEL_IO_QSPI_CONFIG(12,done,NULL);
    io.trans_queue_depth=2;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&io,&lcd_io),failed,TAG,"LCD IO");
    co5300_vendor_config_t vendor={.init_cmds=panel_commands,
        .init_cmds_size=sizeof(panel_commands)/sizeof(panel_commands[0]),.flags.use_qspi_interface=1};
    const esp_lcd_panel_dev_config_t pc={.reset_gpio_num=39,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel=16,.vendor_config=&vendor};
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_co5300(lcd_io,&pc,&panel),failed,TAG,"CO5300");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel),failed,TAG,"panel reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel),failed,TAG,"panel init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel,true),failed,TAG,"panel enable");
    *out_panel=panel; *out_touch=touch;
    return ESP_OK;
failed:
    gbs_board_amoled216_deinit();
    return ret;
}

static bool read_register(i2c_master_dev_handle_t device, uint8_t reg, uint8_t *data, size_t size)
{
    return device && i2c_master_transmit_receive(device,&reg,1,data,size,25)==ESP_OK;
}

static bool write_register(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    uint8_t data[]={reg,value};
    return device && i2c_master_transmit(device,data,sizeof(data),25)==ESP_OK;
}

static bool add_sensor(unsigned address, i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=address,.scl_speed_hz=400000};
    return i2c_master_bus_add_device(bus,&config,device)==ESP_OK;
}

static bool update_register(i2c_master_dev_handle_t device, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t before=0,after=0;
    if (!read_register(device,reg,&before,1)) return false;
    uint8_t requested=(before & (uint8_t)~mask)|(value & mask);
    if (requested!=before && !write_register(device,reg,requested)) return false;
    return read_register(device,reg,&after,1) && (after & mask)==(value & mask);
}

static bool pmu_identified(void)
{
    uint8_t id=0;
    return read_register(pmu,0x03,&id,1) && (id & 0xCF)==0x4A;
}

static bool init_accelerometer(void)
{
    uint8_t id=0;
    return read_register(imu,0x00,&id,1) && id==0x05 &&
        write_register(imu,0x08,0x00) && write_register(imu,0x02,0x60) &&
        write_register(imu,0x03,0x06) && write_register(imu,0x06,0x03) && write_register(imu,0x08,0x01);
}

static void sensors_task(void *unused)
{
    (void)unused;
    bool pmu_ready=pmu_identified(), gauge_ready=false;
    /* Enable measurement/detection and the gauge only. Preserve charger
     * currents, target voltage and temperature protection. */
    /* QMI8658: auto-increment, +/-2 g, 125 Hz, LPF, accelerometer only.
     * Register settings match Waveshare qmi8658 2.0.2 / QMI8658 datasheet. */
    bool imu_ready=add_sensor(0x6B,&imu) && init_accelerometer();
    if (!pmu_ready) ESP_LOGW(TAG,"AXP2101 unavailable; battery status is unknown");
    if (!imu_ready) ESP_LOGW(TAG,"QMI8658 unavailable; keeping the locked orientation");
    int64_t next_battery=0,next_imu_retry=5000;
    for (;;) {
        int64_t now=esp_timer_get_time()/1000;
        if (!imu_ready && now>=next_imu_retry) {
            imu_ready=init_accelerometer(); next_imu_retry=now+5000;
        }
        if (now>=next_battery) {
            if (!pmu_ready) pmu_ready=pmu_identified();
            if (pmu_ready && !gauge_ready) gauge_ready=update_register(pmu,0x68,0x01,0x01) &&
                update_register(pmu,0x30,0x01,0x01) && update_register(pmu,0x18,0x08,0x08);
            uint8_t state[2]={0},percent=0;
            bool valid=pmu_ready && gauge_ready && read_register(pmu,0x00,state,sizeof(state));
            bool percent_valid=valid && gauge_ready && (state[0]&8) && read_register(pmu,0xA4,&percent,1);
            xSemaphoreTake(sensor_mutex,portMAX_DELAY);
            gbs_device_update_pmu(&battery,valid,state[0],state[1],percent_valid,percent,now);
            pmu_raw00=state[0]; pmu_raw01=state[1];
            xSemaphoreGive(sensor_mutex);
            next_battery=now+2000;
        }
        uint8_t ready=0,raw[6]={0};
        bool valid=imu_ready && read_register(imu,0x2E,&ready,1) && (ready&1) && read_register(imu,0x35,raw,sizeof(raw));
        float xyz[3]={0};
        if (valid) for (unsigned i=0;i<3;i++) xyz[i]=(int16_t)((unsigned)raw[i*2]|((unsigned)raw[i*2+1]<<8))/16384.0f;
        /* Same screen-axis signs as the official Immersive_block example. */
        xSemaphoreTake(sensor_mutex,portMAX_DELAY);
        acceleration[0]=-xyz[0]; acceleration[1]=xyz[1]; acceleration[2]=xyz[2];
        acceleration_ms=now; acceleration_valid=valid;
        xSemaphoreGive(sensor_mutex);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static i2s_chan_handle_t audio_tx;
static esp_codec_dev_handle_t codec;
static const audio_codec_ctrl_if_t *codec_ctrl;
static const audio_codec_gpio_if_t *codec_gpio;
static const audio_codec_data_if_t *codec_data;
static const audio_codec_if_t *codec_if;

static void audio_cleanup(void)
{
    /* The codec closes its I2S channel before its interfaces are deleted.
     * Initialization failures before codec creation need a direct stop. */
    if (codec) { esp_codec_dev_close(codec); esp_codec_dev_delete(codec); codec=NULL; }
    else if (audio_tx) i2s_channel_disable(audio_tx);
    if (codec_if) { audio_codec_delete_codec_if(codec_if); codec_if=NULL; }
    if (codec_ctrl) { audio_codec_delete_ctrl_if(codec_ctrl); codec_ctrl=NULL; }
    if (codec_gpio) { audio_codec_delete_gpio_if(codec_gpio); codec_gpio=NULL; }
    if (codec_data) { audio_codec_delete_data_if(codec_data); codec_data=NULL; }
    if (audio_tx) { i2s_del_channel(audio_tx); audio_tx=NULL; }
    gpio_set_level(46,0);
}

static bool audio_init(void)
{
    /* Schematic: ALDO1 supplies ES8311 AVDD/A3V3. Configure it explicitly
     * before playback instead of relying on previous factory firmware.
     * REG92: (3300-500)/100=28; REG90 bit0 enables ALDO1. */
    if (!pmu_identified() || !update_register(pmu,0x92,0x1F,28) ||
        !update_register(pmu,0x90,0x01,0x01)) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    channel.dma_desc_num=4; channel.dma_frame_num=160;
    channel.auto_clear_after_cb=true;
    if (i2s_new_channel(&channel,&audio_tx,NULL)!=ESP_OK) goto failed;
    const i2s_std_config_t i2s={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=42,.bclk=9,.ws=45,.dout=8,.din=I2S_GPIO_UNUSED}};
    if (i2s_channel_init_std_mode(audio_tx,&i2s)!=ESP_OK) goto failed;
    /* Supply MCLK before codec initialization, as in the Waveshare BSP. */
    if (i2s_channel_enable(audio_tx)!=ESP_OK) goto failed;
    audio_codec_i2c_cfg_t control={.port=I2C_NUM_0,.addr=ES8311_CODEC_DEFAULT_ADDR,.bus_handle=bus};
    audio_codec_i2s_cfg_t data={.port=I2S_NUM_0,.tx_handle=audio_tx};
    codec_ctrl=audio_codec_new_i2c_ctrl(&control); codec_gpio=audio_codec_new_gpio();
    codec_data=audio_codec_new_i2s_data(&data);
    if (!codec_ctrl || !codec_gpio || !codec_data) goto failed;
    es8311_codec_cfg_t output={.ctrl_if=codec_ctrl,.gpio_if=codec_gpio,
        .codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.pa_pin=46,.use_mclk=true,
        .hw_gain={.pa_voltage=3.3f,.codec_dac_voltage=3.3f}};
    codec_if=es8311_codec_new(&output);
    if (!codec_if) goto failed;
    esp_codec_dev_cfg_t device={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=codec_if,.data_if=codec_data};
    codec=esp_codec_dev_new(&device);
    if (!codec) goto failed;
    return true;
failed:
    audio_cleanup(); return false;
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
        size_t written=0,bytes=count*2*sizeof(int16_t);
        if (i2s_channel_write(audio_tx,pcm,bytes,&written,50)!=ESP_OK || written!=bytes) return false;
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
        size_t written=0,bytes=count*2*sizeof(int16_t);
        if (i2s_channel_write(audio_tx,pcm,bytes,&written,50)!=ESP_OK || written!=bytes) return false;
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
        /* Drain the <=40 ms DMA queue before closing the DAC. Only this
         * worker waits; the UI and sensors continue running. */
        if (ok && atomic_load(&audio_volume_percent)) vTaskDelay(pdMS_TO_TICKS(50));
        esp_codec_dev_set_out_mute(codec,true);
        /* Release internal-memory DMA and codec objects between cues. */
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
    sensor_mutex=xSemaphoreCreateMutexStatic(&sensor_mutex_storage);
    tone_queue=xQueueCreateStatic(4,sizeof(gbs_tone_t),tone_queue_buffer,&tone_queue_storage);
    xTaskCreateStatic(sensors_task,"gbs_sensors",sizeof(sensor_stack),NULL,1,sensor_stack,&sensor_task_storage);
    xTaskCreateStatic(audio_task,"gbs_tone",sizeof(tone_stack),NULL,1,tone_stack,&tone_task_storage);
}

bool gbs_board_amoled216_tick(int64_t now, bool touching, bool auto_rotate,
    unsigned volume_percent, gbs_tone_t tone, gbs_device_status_t *status)
{
    /* The active-low touch IRQ also covers a press before LVGL's next poll. */
    touching=touching || gpio_get_level(11)==0;
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
    gbs_device_update_pmu(status,false,0,0,false,0,now);
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
            if (command(0x36,gbs_device_panel_madctl(next))==ESP_OK) changed=true;
            else { rotation.current=before; rotation.candidate_valid=false; }
        }
    }
    status->orientation=rotation.current;
    return changed;
}
