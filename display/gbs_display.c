#include "gbs_display.h"
#include "gbs_ui.h"
#include "gbs_hub_client.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

enum { WIDTH=240, HEIGHT=280, ROWS=24 };
static const char *TAG="gbs_display";
static gbs_display_state_fn read_state;
static gbs_display_mode_fn request_mode;
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t lcd_io, touch_io;
static esp_lcd_touch_handle_t touch;
static lv_disp_drv_t display;
static lv_color_t *buffer1, *buffer2;
static char *snapshot;
static unsigned backlight;
static int64_t last_input;
static bool wake_gesture;

static bool flushed(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io; (void)event; (void)context; lv_disp_flush_ready(&display); return false;
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    size_t count=(area->x2-area->x1+1)*(area->y2-area->y1+1);
    for(size_t i=0;i<count;++i) colors[i].full=(colors[i].full>>8)|(colors[i].full<<8);
    esp_err_t err=esp_lcd_panel_draw_bitmap(panel,area->x1,area->y1,area->x2+1,area->y2+1,colors);
    if(err!=ESP_OK) { ESP_LOGW(TAG,"LCD transfer failed: %s",esp_err_to_name(err)); lv_disp_flush_ready(driver); }
}

static void read_touch(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    esp_lcd_touch_point_data_t point={0}; uint8_t count=0;
    esp_err_t err=esp_lcd_touch_read_data(touch);
    bool down=err==ESP_OK&&esp_lcd_touch_get_data(touch,&point,&count,1)==ESP_OK&&count&&point.x<WIDTH&&point.y<HEIGHT;
    bool visible=gbs_ui_touch(down,backlight<gbs_ui_brightness(),&wake_gesture);
    if(down) last_input=esp_timer_get_time()/1000;
    data->point.x=point.x; data->point.y=point.y;
    data->state=visible?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}

static bool submit(gbs_ui_action_t action,double value,uint32_t request_id,const gbs_ui_machine_t *expected,void *context)
{
    (void)context;
    if(action==GBS_ACTION_MODE) { request_mode(value!=0); return true; }
    if(action!=GBS_ACTION_BRIGHTNESS&&action!=GBS_ACTION_DIM) return gbs_hub_submit(action,value,request_id,expected);
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("gbs_display",NVS_READWRITE,&nvs);
    if(err!=ESP_OK) return false;
    err=nvs_set_u8(nvs,action==GBS_ACTION_DIM?"dim":"brightness",(uint8_t)value);
    if(err==ESP_OK) err=nvs_commit(nvs);
    nvs_close(nvs); return err==ESP_OK;
}

static void task(void *unused)
{
    (void)unused;
    lv_init();
    static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw,buffer1,buffer2,WIDTH*ROWS);
    lv_disp_drv_init(&display); display.hor_res=WIDTH; display.ver_res=HEIGHT;
    display.flush_cb=flush; display.draw_buf=&draw; lv_disp_drv_register(&display);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input); input.type=LV_INDEV_TYPE_POINTER; input.read_cb=read_touch; lv_indev_drv_register(&input);
    gbs_ui_init(submit,NULL);
    nvs_handle_t nvs; uint8_t brightness=75,dim=1;
    if(nvs_open("gbs_display",NVS_READONLY,&nvs)==ESP_OK) {
        nvs_get_u8(nvs,"brightness",&brightness); nvs_get_u8(nvs,"dim",&dim); nvs_close(nvs);
    }
    gbs_ui_set_preferences(brightness,dim!=0);
    // Large status buffers and LVGL allocations stay out of the networking heap.
    int64_t previous=esp_timer_get_time()/1000,next_poll=0,next_render=0;
    last_input=previous;
    bool setup_shown=false;
    for(;;) {
        int64_t now=esp_timer_get_time()/1000;
        lv_tick_inc((uint32_t)(now-previous)); previous=now;
        if(now>=next_poll) {
            gbs_ui_bridge_t state;
            if(read_state(snapshot,6144)&&gbs_ui_parse_bridge(snapshot,&state)) {
                gbs_ui_set_bridge(&state);
                if(!setup_shown&&!strcmp(state.phase,"configuration_required")) {
                    gbs_ui_show(GBS_PAGE_SETUP); setup_shown=true;
                }
            }
            next_poll=now+500;
        }
        if(now>=next_render) {
            gbs_ui_machine_t machine; uint32_t request_id; gbs_ui_control_t result;
            if(gbs_hub_read(&machine,&request_id,&result)) {
                gbs_ui_set_machine(&machine);
                if(request_id) gbs_ui_control_result(request_id,result);
            }
            gbs_ui_tick(now); next_render=now+250;
        }
        lv_timer_handler();
        if(gbs_ui_keep_awake()) last_input=now;
        unsigned level=gbs_ui_backlight(gbs_ui_brightness(),gbs_ui_dim_enabled(),now-last_input,gbs_ui_keep_awake());
        if(level!=backlight) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,level*1023/100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0); backlight=level;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t gbs_display_start(gbs_display_state_fn state,gbs_display_mode_fn mode)
{
    esp_err_t ret=ESP_OK;
    bool spi_ready=false,i2c_ready=false;
    read_state=state; request_mode=mode;
    if(!state||!mode||heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<256*1024) return ESP_ERR_NO_MEM;
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
    const i2c_config_t i2c={.mode=I2C_MODE_MASTER,.sda_io_num=11,.scl_io_num=10,
        .sda_pullup_en=GPIO_PULLUP_ENABLE,.scl_pullup_en=GPIO_PULLUP_ENABLE,.master.clk_speed=100000};
    ESP_GOTO_ON_ERROR(i2c_param_config(I2C_NUM_0,&i2c),failed,TAG,"I2C config");
    ESP_GOTO_ON_ERROR(i2c_driver_install(I2C_NUM_0,I2C_MODE_MASTER,0,0,0),failed,TAG,"I2C"); i2c_ready=true;
    const esp_lcd_panel_io_i2c_config_t touch_cfg=ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_NUM_0,&touch_cfg,&touch_io),failed,TAG,"touch IO");
    const esp_lcd_touch_config_t touch_config={.x_max=WIDTH,.y_max=HEIGHT,.rst_gpio_num=13,.int_gpio_num=14};
    ESP_GOTO_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(touch_io,&touch_config,&touch),failed,TAG,"CST816");
    const ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_10_BIT,
        .timer_num=LEDC_TIMER_0,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
    ESP_GOTO_ON_ERROR(ledc_timer_config(&timer),failed,TAG,"backlight timer");
    const ledc_channel_config_t channel={.gpio_num=15,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,
        .timer_sel=LEDC_TIMER_0,.duty=0};
    ESP_GOTO_ON_ERROR(ledc_channel_config(&channel),failed,TAG,"backlight");
    buffer1=heap_caps_malloc(WIDTH*ROWS*sizeof(lv_color_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    buffer2=heap_caps_malloc(WIDTH*ROWS*sizeof(lv_color_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    snapshot=heap_caps_malloc(6144,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!buffer1||!buffer2||!snapshot||xTaskCreate(task,"gbs_display",8192,NULL,2,NULL)!=pdPASS) { ret=ESP_ERR_NO_MEM; goto failed; }
    return ESP_OK;
failed:
    free(buffer1); free(buffer2); buffer1=buffer2=NULL;
    free(snapshot); snapshot=NULL;
    if(touch) esp_lcd_touch_del(touch);
    if(touch_io) esp_lcd_panel_io_del(touch_io);
    if(i2c_ready) i2c_driver_delete(I2C_NUM_0);
    if(panel) esp_lcd_panel_del(panel);
    if(lcd_io) esp_lcd_panel_io_del(lcd_io);
    if(spi_ready) spi_bus_free(SPI2_HOST);
    return ret;
}
