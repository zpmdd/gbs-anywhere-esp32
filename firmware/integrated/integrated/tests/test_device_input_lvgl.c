#include "gbs_device_policy.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>

static gbs_touch_policy_t touch;
static lv_indev_t *pointer;
static bool sample_valid, sample_down, blank;
static unsigned clicks, lost;

static void flush(lv_disp_drv_t *display, const lv_area_t *area, lv_color_t *color)
{
    (void)area; (void)color;
    lv_disp_flush_ready(display);
}

static void read_pointer(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    gbs_device_touch_update(&touch,sample_valid,sample_down,40,40,blank);
    data->point.x=touch.x; data->point.y=touch.y;
    data->state=touch.visible_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void event(lv_event_t *e)
{
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) clicks++;
    if(lv_event_get_code(e)==LV_EVENT_PRESS_LOST) lost++;
}

static void poll(bool valid, bool down, bool screen_blank)
{
    sample_valid=valid; sample_down=down; blank=screen_blank;
    lv_tick_inc(40); lv_timer_handler();
}

static void keys(bool down)
{
    if(down && !touch.inhibited) lv_indev_wait_release(pointer);
    gbs_device_touch_inhibit(&touch,down);
}

int main(void)
{
    lv_init();
    static lv_color_t pixels[1000];
    static lv_disp_draw_buf_t draw;
    static lv_disp_drv_t display;
    lv_disp_draw_buf_init(&draw,pixels,NULL,1000);
    lv_disp_drv_init(&display);
    display.hor_res=100; display.ver_res=100; display.draw_buf=&draw; display.flush_cb=flush;
    lv_disp_drv_register(&display);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input); input.type=LV_INDEV_TYPE_POINTER; input.read_cb=read_pointer;
    pointer=lv_indev_drv_register(&input);
    lv_obj_t *button=lv_btn_create(lv_scr_act());
    lv_obj_set_pos(button,10,10); lv_obj_set_size(button,80,80);
    lv_obj_add_event_cb(button,event,LV_EVENT_ALL,NULL);
    poll(true,false,false);

    /* A failed read cannot release a command button. */
    poll(true,true,false); poll(false,false,false); poll(true,true,false);
    assert(clicks==0 && lost==0);
    poll(true,false,false); assert(clicks==1 && lost==0);

    /* A physical key cancels the active gesture with PRESS_LOST, never CLICKED.
     * Touch bounce, transport failure and key release cannot re-target it. */
    poll(true,true,false); keys(true); poll(false,false,false);
    assert(clicks==1 && lost==1);
    poll(true,false,false); poll(true,true,false); poll(true,false,false);
    poll(true,true,false); keys(false); poll(false,false,false); poll(true,true,false);
    assert(clicks==1 && lost==1);
    poll(true,false,false); poll(true,true,false); poll(true,false,false);
    assert(clicks==2 && lost==1);

    /* Long-key screen-off cancels a held touch; neither that finger nor the
     * first fresh wake gesture submits a control after the screen relights. */
    poll(true,true,false); keys(true); poll(true,true,true);
    assert(clicks==2 && lost==2);
    keys(false); poll(true,true,true); poll(true,false,true);
    poll(true,true,true); poll(false,false,false); poll(true,true,false); poll(true,false,false);
    assert(clicks==2 && lost==2);
    poll(true,true,false); poll(true,false,false); assert(clicks==3 && lost==2);
    puts("LVGL input: failed reads, key cancellation and first-wake gestures passed");
    return 0;
}
