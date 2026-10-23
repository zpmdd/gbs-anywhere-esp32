#include "gbs_display.h"
#include "gbs_ui.h"
#include "gbs_hub_client.h"
#include "driver/gpio.h"
#ifdef HUB_AMOLED_216
#include "gbs_board_amoled216.h"
#include "gbs_device_policy.h"
#else
#include "driver/i2c.h"
#endif
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>

#ifdef HUB_AMOLED_216
// One larger tile reduces repeated drawing within the same 7680-byte DMA budget.
enum { WIDTH=GBS_DISPLAY_WIDTH, HEIGHT=GBS_DISPLAY_HEIGHT, ROWS=8 };
#else
enum { WIDTH=GBS_DISPLAY_WIDTH, HEIGHT=GBS_DISPLAY_HEIGHT, ROWS=12 };
#endif
static const char *TAG="gbs_display";
static gbs_display_state_fn read_state;
static gbs_display_mode_fn request_mode;
static esp_lcd_panel_handle_t panel;
#ifndef HUB_AMOLED_216
static esp_lcd_panel_io_handle_t lcd_io, touch_io;
#endif
static esp_lcd_touch_handle_t touch;
static lv_disp_drv_t display;
// Reserve DMA buffers before networking fragments internal RAM.
static DMA_ATTR lv_color_t buffer1[WIDTH*ROWS];
#ifndef HUB_AMOLED_216
static DMA_ATTR lv_color_t buffer2[WIDTH*ROWS];
#endif
static char *snapshot;
static unsigned backlight;
static int64_t last_input;
#ifdef HUB_AMOLED_216
static bool touch_down;
static bool manual_sleep;
static gbs_touch_policy_t touch_policy;
static lv_indev_t *pointer_input;
static atomic_int touch_last_error;
static atomic_bool physical_touch_down;
static atomic_uint touch_idle_reports;
#else
static bool wake_gesture;
#endif
static atomic_uint touch_presses, current_page, stack_free, rendered_at;
static atomic_uint touch_raw, touch_position, touch_errors;
static atomic_uint loop_max_gap, touch_max_gap;
static atomic_int startup_error=ESP_ERR_INVALID_STATE;
static uint16_t *frame;
static SemaphoreHandle_t frame_mutex;
static StackType_t display_stack[8192 / sizeof(StackType_t)];
static StaticTask_t display_task_storage;

enum { PERF_LVGL, PERF_FLUSH, PERF_TOUCH, PERF_SNAPSHOT, PERF_MODEL, PERF_COUNT };
typedef struct { unsigned last_us, max_us; } display_timing_t;
typedef struct {
    display_timing_t timing[PERF_COUNT];
    uint64_t flush_total_us;
    unsigned flush_count;
} display_performance_t;
static display_performance_t performance;
static portMUX_TYPE performance_lock=portMUX_INITIALIZER_UNLOCKED;

static void record_performance(unsigned kind,int64_t elapsed_us)
{
    unsigned elapsed=elapsed_us>UINT_MAX?UINT_MAX:(unsigned)elapsed_us;
    portENTER_CRITICAL(&performance_lock);
    performance.timing[kind].last_us=elapsed;
    if(elapsed>performance.timing[kind].max_us) performance.timing[kind].max_us=elapsed;
    if(kind==PERF_FLUSH) {
        ++performance.flush_count;
        performance.flush_total_us+=(uint64_t)elapsed_us;
    }
    portEXIT_CRITICAL(&performance_lock);
}

bool gbs_display_copy_frame(void *buffer,size_t size)
{
    if(!buffer||size!=GBS_DISPLAY_FRAME_BYTES||!frame||!atomic_load(&rendered_at)||
       xSemaphoreTake(frame_mutex,pdMS_TO_TICKS(30))!=pdTRUE) return false;
    memcpy(buffer,frame,size);
    xSemaphoreGive(frame_mutex);
    return true;
}

bool gbs_display_diagnostics_json(char *buffer,size_t size)
{
    unsigned raw=atomic_load(&touch_raw),position=atomic_load(&touch_position);
    display_performance_t perf;
    portENTER_CRITICAL(&performance_lock); perf=performance; portEXIT_CRITICAL(&performance_lock);
#ifdef HUB_AMOLED_216
    char board[768]="";
    gbs_audio_diagnostics_t audio=gbs_board_amoled216_audio_diagnostics();
    float xyz[3]={0};
    bool imu_valid=gbs_board_amoled216_acceleration(xyz);
    gbs_pmu_diagnostics_t pmu={0};
    (void)gbs_board_amoled216_pmu_diagnostics(&pmu);
    int board_count=snprintf(board,sizeof(board),",\"lastTouchError\":\"%s\",\"touchIrqLevel\":%d,\"touchPhysicalDown\":%s,\"touchIdleReports\":%u,\"imuValid\":%s,\"imuXMilliG\":%d,\"imuYMilliG\":%d,\"imuZMilliG\":%d,"
        "\"pmuRaw00\":%u,\"pmuRaw01\":%u,\"pmuValid\":%s,\"pmuChargingKnown\":%s,\"pmuCharging\":%s,\"pmuPercentKnown\":%s,\"pmuPercent\":%u,\"pmuObservedMs\":%" PRId64 ",\"audioSuccessPlayed\":%u,\"audioFailurePlayed\":%u,\"audioNoticePlayed\":%u,\"audioOutputErrors\":%u",
        esp_err_to_name(atomic_load(&touch_last_error)),gpio_get_level(11),
        atomic_load(&physical_touch_down)?"true":"false",atomic_load(&touch_idle_reports),imu_valid?"true":"false",
        (int)(xyz[0]*1000),(int)(xyz[1]*1000),(int)(xyz[2]*1000),
        (unsigned)pmu.raw00,(unsigned)pmu.raw01,pmu.decoded.valid?"true":"false",
        pmu.decoded.charging_known?"true":"false",pmu.decoded.charging?"true":"false",
        pmu.decoded.battery_percent_known?"true":"false",pmu.decoded.battery_percent,pmu.decoded.observed_ms,
        audio.success,audio.failure,audio.notice,audio.errors);
    if (board_count<0 || (size_t)board_count>=sizeof(board)) return false;
#else
    const char *board="";
#endif
    int count=snprintf(buffer,size,"{\"ready\":%s,\"width\":%u,\"height\":%u,\"page\":%u,\"touchPresses\":%u,\"stackFreeBytes\":%u,\"renderedAtMs\":%u,\"dataTaskReady\":%s,\"startupError\":\"%s\",\"touchRawX\":%u,\"touchRawY\":%u,\"touchX\":%u,\"touchY\":%u,\"touchReadErrors\":%u,\"loopMaxGapMs\":%u,\"touchMaxGapMs\":%u,"
        "\"lvglLastUs\":%u,\"lvglMaxUs\":%u,\"flushLastUs\":%u,\"flushMaxUs\":%u,\"flushCount\":%u,\"flushTotalUs\":%" PRIu64 ","
        "\"touchReadLastUs\":%u,\"touchReadMaxUs\":%u,\"snapshotReadLastUs\":%u,\"snapshotReadMaxUs\":%u,\"uiModelLastUs\":%u,\"uiModelMaxUs\":%u%s}",
        atomic_load(&rendered_at)?"true":"false",(unsigned)WIDTH,(unsigned)HEIGHT,atomic_load(&current_page),atomic_load(&touch_presses),
        atomic_load(&stack_free),atomic_load(&rendered_at),gbs_hub_running()?"true":"false",
        esp_err_to_name(atomic_load(&startup_error)),raw>>16,raw&65535,position>>16,position&65535,atomic_load(&touch_errors),
        atomic_load(&loop_max_gap),atomic_load(&touch_max_gap),
        perf.timing[PERF_LVGL].last_us,perf.timing[PERF_LVGL].max_us,
        perf.timing[PERF_FLUSH].last_us,perf.timing[PERF_FLUSH].max_us,perf.flush_count,perf.flush_total_us,
        perf.timing[PERF_TOUCH].last_us,perf.timing[PERF_TOUCH].max_us,
        perf.timing[PERF_SNAPSHOT].last_us,perf.timing[PERF_SNAPSHOT].max_us,
        perf.timing[PERF_MODEL].last_us,perf.timing[PERF_MODEL].max_us,board);
    return count>=0&&(size_t)count<size;
}

static bool flushed(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io; (void)event; (void)context; lv_disp_flush_ready(&display); return false;
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    int64_t started=esp_timer_get_time();
    xSemaphoreTake(frame_mutex,portMAX_DELAY);
    for(int y=area->y1;y<=area->y2;++y)
        memcpy(frame+y*WIDTH+area->x1,colors+(y-area->y1)*(area->x2-area->x1+1),(area->x2-area->x1+1)*sizeof(lv_color_t));
    xSemaphoreGive(frame_mutex);
    size_t count=(area->x2-area->x1+1)*(area->y2-area->y1+1);
    for(size_t i=0;i<count;++i) colors[i].full=(colors[i].full>>8)|(colors[i].full<<8);
    esp_err_t err=esp_lcd_panel_draw_bitmap(panel,area->x1,area->y1,area->x2+1,area->y2+1,colors);
    if(err!=ESP_OK) { ESP_LOGW(TAG,"LCD transfer failed: %s",esp_err_to_name(err)); lv_disp_flush_ready(driver); }
    record_performance(PERF_FLUSH,esp_timer_get_time()-started);
}

static bool map_touch(uint16_t x,uint16_t y,lv_point_t *point)
{
    if(x>=WIDTH||y>=HEIGHT) return false;
#ifdef HUB_AMOLED_216
    uint16_t sx,sy;
    if(!gbs_device_map_touch(gbs_board_amoled216_orientation(),x,y,&sx,&sy)) return false;
    point->x=sx; point->y=sy;
#else
    // USB-C on the right: the touch sensor must follow the panel's 180-degree orientation.
    point->x=WIDTH-1-x; point->y=HEIGHT-1-y;
#endif
    return true;
}

static void read_touch(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    int64_t started=esp_timer_get_time();
    static unsigned last_poll;
    unsigned now_ms = (unsigned)(esp_timer_get_time() / 1000);
    if (last_poll && now_ms - last_poll > atomic_load(&touch_max_gap)) atomic_store(&touch_max_gap, now_ms - last_poll);
    last_poll = now_ms;
    (void)driver;
    esp_lcd_touch_point_data_t point={0}; uint8_t count=0;
    static lv_point_t position;
    esp_err_t err=esp_lcd_touch_read_data(touch);
#ifdef HUB_AMOLED_216
    bool idle=gbs_device_touch_idle(&touch_policy,err==ESP_ERR_INVALID_RESPONSE,gpio_get_level(11)!=0);
    if(idle) {
        /* CST9220 may have no report before its first touch. Do not consume
         * cached coordinates or manufacture release of a held gesture. */
        atomic_fetch_add(&touch_idle_reports,1);
        err=ESP_OK;
    } else
#endif
    if(err==ESP_OK) err=esp_lcd_touch_get_data(touch,&point,&count,1);
    if(err!=ESP_OK) atomic_fetch_add(&touch_errors,1);
    bool down=err==ESP_OK&&count&&map_touch(point.x,point.y,&position);
#ifdef HUB_AMOLED_216
    bool trusted=err==ESP_OK && (!count || down);
    atomic_store(&touch_last_error,err==ESP_OK && !trusted?ESP_ERR_INVALID_RESPONSE:err);
    if(err==ESP_OK && !trusted) atomic_fetch_add(&touch_errors,1);
    bool may_wake=!touch_policy.wake_gesture && !touch_policy.inhibited;
    gbs_device_touch_update(&touch_policy,trusted,down,position.x,position.y,manual_sleep || backlight<gbs_ui_brightness());
    down=touch_policy.physical_down;
    atomic_store(&physical_touch_down,down);
    touch_down=down || !trusted;
    position.x=touch_policy.x; position.y=touch_policy.y;
#endif
    if(down && err==ESP_OK) {
        atomic_store(&touch_raw,((unsigned)point.x<<16)|point.y);
        atomic_store(&touch_position,((unsigned)position.x<<16)|position.y);
    }
    static bool previous_down;
    if(down&&!previous_down) {
        atomic_fetch_add(&touch_presses,1);
#ifdef HUB_AMOLED_216
        if(may_wake) manual_sleep=false;
#endif
    }
    previous_down=down;
#ifdef HUB_AMOLED_216
    bool visible=touch_policy.visible_down;
#else
    bool visible=gbs_ui_touch(down,backlight<gbs_ui_brightness(),&wake_gesture);
#endif
    if(down) last_input=esp_timer_get_time()/1000;
    data->point=position;
    data->state=visible?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    record_performance(PERF_TOUCH,esp_timer_get_time()-started);
}

static bool submit(gbs_ui_action_t action,double value,uint32_t request_id,const gbs_ui_machine_t *expected,void *context)
{
    (void)context;
    if(action==GBS_ACTION_MODE) return request_mode(value!=0);
    const char *key=NULL;
    if(action==GBS_ACTION_BRIGHTNESS) key="brightness";
    else if(action==GBS_ACTION_DIM) key="dim";
#ifdef HUB_AMOLED_216
    else if(action==GBS_ACTION_ROTATION_AUTO) key="rotate_auto";
    else if(action==GBS_ACTION_SOUND) key="sound";
#endif
    if(!key) return gbs_hub_submit(action,value,request_id,expected);
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("gbs_display",NVS_READWRITE,&nvs);
    if(err!=ESP_OK) return false;
#ifdef HUB_AMOLED_216
    uint8_t stored;
    if(nvs_get_u8(nvs,key,&stored)==ESP_OK && stored==(uint8_t)value) {
        nvs_close(nvs); return true;
    }
#endif
    err=nvs_set_u8(nvs,key,(uint8_t)value);
    if(err==ESP_OK) err=nvs_commit(nvs);
    nvs_close(nvs); return err==ESP_OK;
}

static bool read_data(gbs_ui_data_kind_t kind,void *context)
{
    (void)context; return gbs_hub_request_data(kind);
}

#ifdef HUB_AMOLED_216
static bool refresh_scale(void *context)
{
    (void)context; return gbs_hub_request_scale_refresh();
}
#endif

static bool submit_schedule(const gbs_ui_schedule_t *expected,const gbs_ui_schedule_t *requested,uint32_t request_id,void *context)
{
    (void)context; return gbs_hub_submit_schedule(expected,requested,request_id);
}

#ifdef HUB_AMOLED_216
static void round_area(lv_disp_drv_t *driver,lv_area_t *area)
{
    (void)driver;
    area->x1&=~1; area->y1&=~1;
    area->x2|=1; area->y2|=1;
}
#endif

static void task(void *unused)
{
    (void)unused;
    lv_init();
    static lv_disp_draw_buf_t draw;
#ifdef HUB_AMOLED_216
    lv_disp_draw_buf_init(&draw,buffer1,NULL,WIDTH*ROWS);
#else
    lv_disp_draw_buf_init(&draw,buffer1,buffer2,WIDTH*ROWS);
#endif
    lv_disp_drv_init(&display); display.hor_res=WIDTH; display.ver_res=HEIGHT;
    display.flush_cb=flush; display.draw_buf=&draw;
#ifdef HUB_AMOLED_216
    display.rounder_cb=round_area;
#endif
    lv_disp_drv_register(&display);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input); input.type=LV_INDEV_TYPE_POINTER; input.read_cb=read_touch;
#ifdef HUB_AMOLED_216
    pointer_input=lv_indev_drv_register(&input);
#else
    lv_indev_drv_register(&input);
#endif
    gbs_ui_init(submit,NULL);
    gbs_ui_set_reader(read_data);
    gbs_ui_set_schedule_submit(submit_schedule);
    gbs_ui_data_t *data=heap_caps_calloc(GBS_DATA_COUNT,sizeof(*data),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(data?ESP_OK:ESP_ERR_NO_MEM);
    nvs_handle_t nvs;
#ifdef HUB_AMOLED_216
    uint8_t brightness=60,dim=0;
#else
    uint8_t brightness=75,dim=1;
#endif
    if(nvs_open("gbs_display",NVS_READONLY,&nvs)==ESP_OK) {
        nvs_get_u8(nvs,"brightness",&brightness); nvs_get_u8(nvs,"dim",&dim); nvs_close(nvs);
    }
    gbs_ui_set_preferences(brightness,dim!=0);
#ifdef HUB_AMOLED_216
    uint8_t auto_rotate=0,sound=0;
    if(nvs_open("gbs_display",NVS_READONLY,&nvs)==ESP_OK) {
        nvs_get_u8(nvs,"rotate_auto",&auto_rotate); nvs_get_u8(nvs,"sound",&sound); nvs_close(nvs);
    }
    gbs_ui_set_device_preferences(auto_rotate==1,sound==1);
    gbs_ui_set_scale_refresh(refresh_scale);
    gbs_board_amoled216_start_services();
    gbs_device_status_t previous_device={0};
    bool have_device=false;
#endif
    // Large status buffers and LVGL allocations stay out of the networking heap.
    int64_t previous=esp_timer_get_time()/1000,next_poll=0,next_render=0;
    last_input=previous;
    for(;;) {
        int64_t now=esp_timer_get_time()/1000;
        int64_t model_us=0;
        bool model_updated=false;
        if ((unsigned)(now-previous) > atomic_load(&loop_max_gap)) atomic_store(&loop_max_gap, (unsigned)(now-previous));
        lv_tick_inc((uint32_t)(now-previous)); previous=now;
        if(now>=next_poll) {
            gbs_ui_bridge_t state;
            int64_t started=esp_timer_get_time();
            bool valid=read_state(snapshot,6144)&&gbs_ui_parse_bridge(snapshot,&state);
            record_performance(PERF_SNAPSHOT,esp_timer_get_time()-started);
            if(valid) {
                started=esp_timer_get_time();
                gbs_ui_set_bridge(&state);
                model_us+=esp_timer_get_time()-started; model_updated=true;
            }
            next_poll=now+500;
        }
        if(now>=next_render) {
            int64_t started=esp_timer_get_time();
            gbs_ui_machine_t machine; uint32_t request_id; gbs_ui_control_t result;
            if(gbs_hub_read(&machine,&request_id,&result)) {
                gbs_ui_set_machine(&machine);
                if(request_id) gbs_ui_control_result(request_id,result);
            }
            for(unsigned i=0;i<GBS_DATA_COUNT;++i)
                if(gbs_hub_read_data(i,&data[i])) gbs_ui_set_data(&data[i]);
            gbs_ui_tick(now); next_render=now+250;
            model_us+=esp_timer_get_time()-started; model_updated=true;
        }
#ifdef HUB_AMOLED_216
        gbs_button_event_t user_key,boot_key;
        bool keys_down=gbs_board_amoled216_buttons(now,manual_sleep || backlight==0,&user_key,&boot_key);
        if(keys_down && !touch_policy.inhibited) lv_indev_wait_release(pointer_input);
        gbs_device_touch_inhibit(&touch_policy,keys_down);
        if(user_key==GBS_BUTTON_WAKE || boot_key==GBS_BUTTON_WAKE) {
            manual_sleep=false; last_input=now;
        }
        if(user_key==GBS_BUTTON_SHORT) {
            last_input=now;
            if(manual_sleep || backlight==0) manual_sleep=false;
            else gbs_ui_home_safe();
        } else if(user_key==GBS_BUTTON_LONG) manual_sleep=true;
        if(boot_key==GBS_BUTTON_SHORT) {
            last_input=now;
            if(manual_sleep || backlight==0) manual_sleep=false;
            else gbs_ui_toggle_rotation();
        }
        gbs_device_status_t device;
        if(gbs_board_amoled216_tick(now,touch_down || keys_down,gbs_ui_rotation_auto(),gbs_ui_sound_enabled(),gbs_ui_take_tone(),&device))
            lv_obj_invalidate(lv_scr_act());
        if(!have_device || memcmp(&device,&previous_device,sizeof(device))) {
            int64_t started=esp_timer_get_time();
            gbs_ui_set_device(&device); previous_device=device; have_device=true;
            model_us+=esp_timer_get_time()-started; model_updated=true;
        }
#endif
        if(model_updated) record_performance(PERF_MODEL,model_us);
        int64_t lvgl_started=esp_timer_get_time();
        lv_timer_handler();
        record_performance(PERF_LVGL,esp_timer_get_time()-lvgl_started);
        atomic_store(&current_page,gbs_ui_page());
        atomic_store(&stack_free,uxTaskGetStackHighWaterMark(NULL));
        atomic_store(&rendered_at,(unsigned)now);
        if(gbs_ui_keep_awake()) last_input=now;
        unsigned level=gbs_ui_backlight(gbs_ui_brightness(),gbs_ui_dim_enabled(),now-last_input,gbs_ui_keep_awake());
#ifdef HUB_AMOLED_216
        if(manual_sleep) level=0;
#endif
        if(level!=backlight) {
#ifdef HUB_AMOLED_216
            if(gbs_board_amoled216_brightness(level)==ESP_OK) backlight=level;
#else
            ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,level*1023/100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0); backlight=level;
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t gbs_display_start(gbs_display_state_fn state,gbs_display_mode_fn mode)
{
    esp_err_t ret=ESP_OK;
#ifndef HUB_AMOLED_216
    bool spi_ready=false,i2c_ready=false;
#endif
    read_state=state; request_mode=mode;
    size_t required_psram=256*1024;
#ifdef HUB_AMOLED_216
    required_psram+=GBS_DISPLAY_FRAME_BYTES;
#endif
    if(!state||!mode||heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<required_psram) {
        atomic_store(&startup_error,ESP_ERR_NO_MEM); return ESP_ERR_NO_MEM;
    }
#ifdef HUB_AMOLED_216
    ESP_GOTO_ON_ERROR(gbs_board_amoled216_init(WIDTH*ROWS*sizeof(lv_color_t),flushed,&panel,&touch),failed,TAG,"AMOLED board");
#else
    const i2c_config_t i2c={.mode=I2C_MODE_MASTER,.sda_io_num=11,.scl_io_num=10,
        .sda_pullup_en=GPIO_PULLUP_ENABLE,.scl_pullup_en=GPIO_PULLUP_ENABLE,.master.clk_speed=100000};
    ESP_GOTO_ON_ERROR(i2c_param_config(I2C_NUM_0,&i2c),failed,TAG,"I2C config");
    ESP_GOTO_ON_ERROR(i2c_driver_install(I2C_NUM_0,I2C_MODE_MASTER,0,0,0),failed,TAG,"I2C"); i2c_ready=true;
    const esp_lcd_panel_io_i2c_config_t touch_cfg=ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_NUM_0,&touch_cfg,&touch_io),failed,TAG,"touch IO");
    const esp_lcd_touch_config_t touch_config={.x_max=WIDTH,.y_max=HEIGHT,.rst_gpio_num=13,.int_gpio_num=14};
    ESP_GOTO_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(touch_io,&touch_config,&touch),failed,TAG,"CST816");
    const spi_bus_config_t spi={.sclk_io_num=6,.mosi_io_num=7,.miso_io_num=-1,.quadwp_io_num=-1,.quadhd_io_num=-1,
        .max_transfer_sz=WIDTH*ROWS*sizeof(lv_color_t)};
    ESP_GOTO_ON_ERROR(spi_bus_initialize(SPI2_HOST,&spi,SPI_DMA_CH_AUTO),failed,TAG,"SPI"); spi_ready=true;
    const esp_lcd_panel_io_spi_config_t io={.dc_gpio_num=4,.cs_gpio_num=5,.pclk_hz=40000000,.lcd_cmd_bits=8,
        .lcd_param_bits=8,.trans_queue_depth=2,.on_color_trans_done=flushed};
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&io,&lcd_io),failed,TAG,"LCD IO");
    const esp_lcd_panel_dev_config_t lcd={.reset_gpio_num=8,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io,&lcd,&panel),failed,TAG,"ST7789");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel),failed,TAG,"reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel),failed,TAG,"init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(panel,true,true),failed,TAG,"mirror");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(panel,0,20),failed,TAG,"gap");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(panel,true),failed,TAG,"invert");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel,true),failed,TAG,"enable");
    const ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_10_BIT,
        .timer_num=LEDC_TIMER_0,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
    ESP_GOTO_ON_ERROR(ledc_timer_config(&timer),failed,TAG,"backlight timer");
    const ledc_channel_config_t channel={.gpio_num=15,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,
        .timer_sel=LEDC_TIMER_0,.duty=0};
    ESP_GOTO_ON_ERROR(ledc_channel_config(&channel),failed,TAG,"backlight");
#endif
    snapshot=heap_caps_malloc(6144,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    frame=heap_caps_calloc(1,GBS_DISPLAY_FRAME_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    frame_mutex=xSemaphoreCreateMutex();
    if(!snapshot||!frame||!frame_mutex) { ESP_LOGE(TAG,"Display buffers unavailable"); ret=ESP_ERR_NO_MEM; goto failed; }
    if(!xTaskCreateStatic(task,"gbs_display",sizeof(display_stack),NULL,2,display_stack,&display_task_storage)) {
        ESP_LOGE(TAG,"Display task stack unavailable; internal largest=%u",(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        ret=ESP_ERR_NO_MEM; goto failed;
    }
#ifdef HUB_AMOLED_216
    ESP_LOGI(TAG,"CO5300 480x480 and CST9220 ready; display task started");
#else
    ESP_LOGI(TAG,"ST7789 240x280 and CST816 ready; display task started");
#endif
    atomic_store(&startup_error,ESP_OK);
    return ESP_OK;
failed:
    atomic_store(&startup_error,ret);
    free(snapshot); snapshot=NULL;
    free(frame); frame=NULL;
    if(frame_mutex) { vSemaphoreDelete(frame_mutex); frame_mutex=NULL; }
#ifdef HUB_AMOLED_216
    gbs_board_amoled216_deinit();
#else
    if(touch) esp_lcd_touch_del(touch);
    if(touch_io) esp_lcd_panel_io_del(touch_io);
    if(i2c_ready) i2c_driver_delete(I2C_NUM_0);
    if(panel) esp_lcd_panel_del(panel);
    if(lcd_io) esp_lcd_panel_io_del(lcd_io);
    if(spi_ready) spi_bus_free(SPI2_HOST);
#endif
    return ret;
}
