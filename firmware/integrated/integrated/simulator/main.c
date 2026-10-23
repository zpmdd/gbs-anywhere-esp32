#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "gbs_ui.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HUB_AMOLED_216
enum { WIDTH = 480, HEIGHT = 480, SCALE = 2 };
#else
enum { WIDTH = 240, HEIGHT = 280, SCALE = 3 };
#endif
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t pixels[WIDTH * HEIGHT];
static bool pressed;
static lv_point_t pointer;
static lv_indev_drv_t input;
static lv_indev_t *pointer_device;
static gbs_ui_bridge_t bridge;
static gbs_ui_machine_t machine;
static gbs_ui_action_t command;
static double requested;
static unsigned requests, sequence;
static uint32_t submitted_at;
static uint32_t command_id;
static bool pending;
static bool reject_submit;
static unsigned scenario = 1;
static unsigned flush_count;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    ++flush_count;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * WIDTH + x] = lv_color_to32(*colors++);
    if (lv_disp_flush_is_last(driver)) {
        SDL_UpdateTexture(texture, NULL, pixels, WIDTH * sizeof(uint32_t));
        SDL_RenderCopy(renderer, texture, NULL, NULL); SDL_RenderPresent(renderer);
    }
    lv_disp_flush_ready(driver);
}

static void read_pointer(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver; data->point = pointer;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void pointer_event(int x, int y, bool down)
{
    if(!down&&pressed&&(pointer.x!=x||pointer.y!=y)) {
        pointer.x=x; pointer.y=y;
        lv_indev_read_timer_cb(input.read_timer);
    }
    pointer.x=x; pointer.y=y; pressed=down;
    lv_obj_update_layout(lv_scr_act());
    // SDL can deliver down and up in one poll; preserve both for LVGL.
    lv_indev_read_timer_cb(input.read_timer);
}

static void window_pointer(int x,int y,bool down)
{
    pointer_event(x/SCALE,y/SCALE,down);
}

static void mouse_event(const SDL_Event *e)
{
    if(e->type==SDL_MOUSEMOTION&&pressed) window_pointer(e->motion.x,e->motion.y,true);
    if((e->type==SDL_MOUSEBUTTONDOWN||e->type==SDL_MOUSEBUTTONUP)&&e->button.button==SDL_BUTTON_LEFT)
        window_pointer(e->button.x,e->button.y,e->type==SDL_MOUSEBUTTONDOWN);
}

static bool data_pending[GBS_DATA_COUNT];
static uint32_t data_started[GBS_DATA_COUNT];
static unsigned data_requests;
static bool schedule_pending;
static gbs_ui_schedule_t saved_plan, requested_plan;
static bool read_data(gbs_ui_data_kind_t kind,void *context)
{
    (void)context; data_pending[kind]=true; data_started[kind]=SDL_GetTicks(); ++data_requests; return true;
}
static gbs_ui_data_t sample_data(gbs_ui_data_kind_t kind)
{
    gbs_ui_data_t data={.kind=kind,.state=GBS_DATA_READY,.valid=true,.count=kind==GBS_DATA_SCHEDULE?4:5};
    data.total=data.count; data.editable=kind==GBS_DATA_SCHEDULE;
    if(kind==GBS_DATA_SCHEDULE) {
        data.items.schedules[0]=(gbs_ui_schedule_t){.on_minutes=420,.off_minutes=570,.days=31,.enabled=true,.steam=true};
        data.items.schedules[1]=(gbs_ui_schedule_t){.on_minutes=510,.off_minutes=660,.days=96,.enabled=true};
        data.items.schedules[2]=(gbs_ui_schedule_t){.on_minutes=1080,.off_minutes=1170,.days=16};
        data.items.schedules[3]=(gbs_ui_schedule_t){.on_minutes=1380,.off_minutes=60,.days=32,.enabled=true,.steam=true};
        for(unsigned i=0;i<data.count;++i) snprintf(data.items.schedules[i].id,sizeof(data.items.schedules[i].id),"synthetic-plan-%u",i);
        if(saved_plan.id[0]) data.items.schedules[0]=saved_plan;
    } else {
        const char *times[]={"2026-10-18T01:18:20Z","2026-10-18T01:02:00Z","2026-10-17T01:10:00Z","2026-10-16T09:20:00Z","2026-10-16T01:10:00Z"};
        for(unsigned i=0;i<data.count;++i) {
            gbs_ui_coffee_t *coffee=&data.items.coffees[i];
            snprintf(coffee->time,sizeof(coffee->time),"%s",times[i]);
            coffee->seconds=31.2-i*0.7; coffee->temperature=93; coffee->dose=30; coffee->mode=1;
        }
        data.items.coffees[3].seconds=data.items.coffees[3].dose=data.items.coffees[3].temperature=NAN;
    }
    return data;
}
static void finish_data(gbs_ui_data_kind_t kind)
{
    gbs_ui_data_t data=sample_data(kind); data_pending[kind]=false; gbs_ui_set_data(&data);
}

static bool submit_schedule(const gbs_ui_schedule_t *expected,const gbs_ui_schedule_t *plan,uint32_t id,void *context)
{
    (void)context;
    if(pending||!expected||!plan||!id||reject_submit) return false;
    requested_plan=*plan; command_id=id; pending=schedule_pending=true; submitted_at=SDL_GetTicks(); ++requests; return true;
}
static void finish_schedule(void)
{
    saved_plan=requested_plan; pending=schedule_pending=false;
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);
}

static bool submit(gbs_ui_action_t action, double value, uint32_t request_id, const gbs_ui_machine_t *expected, void *context)
{
    (void)context;
    if (reject_submit) return false;
    if (action == GBS_ACTION_BRIGHTNESS || action == GBS_ACTION_DIM || action==GBS_ACTION_ROTATION_AUTO || action==GBS_ACTION_SOUND) return true;
    if(gbs_ui_machine_action(action)&&(!expected||!gbs_ui_action_unchanged(action,expected,&machine))) return false;
    ++requests; command = action; requested = value; command_id=request_id; pending = true; submitted_at = SDL_GetTicks();
    return true;
}

static void finish_requested(void)
{
    switch(command) {
    case GBS_ACTION_WEIGHT:
        machine.dose_mode=machine.dose1==requested?1:machine.dose2==requested?2:1;
        if(machine.dose1!=requested&&machine.dose2!=requested) machine.dose1=requested;
        break;
    case GBS_ACTION_COFFEE: machine.coffee_target=requested; break;
    case GBS_ACTION_STEAM: machine.steam_target=requested; break;
    case GBS_ACTION_STEAM_ENABLED: machine.steam_enabled=requested!=0; break;
    case GBS_ACTION_DOSE1: machine.dose1=requested; break;
    case GBS_ACTION_DOSE2: machine.dose2=requested; break;
    case GBS_ACTION_BBW_MODE: machine.dose_mode=(unsigned)requested; break;
    case GBS_ACTION_PREBREW_MODE: machine.prebrew_mode=(unsigned)requested; break;
    case GBS_ACTION_PREBREW_ON: machine.prebrew_on=requested; break;
    case GBS_ACTION_PREBREW_OFF: machine.prebrew_off=requested; break;
    case GBS_ACTION_POWER: machine.machine_mode=(unsigned)requested; break;
    case GBS_ACTION_STANDBY_ENABLED: machine.standby_enabled=requested!=0; break;
    case GBS_ACTION_STANDBY_MINUTES: machine.standby_minutes=requested; break;
    case GBS_ACTION_STANDBY_MODE: machine.standby_mode=(unsigned)requested; break;
    case GBS_ACTION_MODE: bridge.active=requested!=0; break;
    default: assert(false);
    }
    machine.weight=machine.dose_mode==1?machine.dose1:machine.dose_mode==2?machine.dose2:NAN;
    pending=false; ++machine.revision; gbs_ui_set_machine(&machine); gbs_ui_set_bridge(&bridge);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);
}

static void scene(unsigned n)
{
    scenario = n;
    bridge = (gbs_ui_bridge_t){.valid=true, .wifi=true, .cloud=true, .grinder=true, .storage=true,
        .active=true, .free_internal=80000, .largest_internal=32000};
    strcpy(bridge.phase, "ready");
    machine = (gbs_ui_machine_t){.coffee=93, .coffee_target=93, .steam=128, .steam_target=128,
        .weight=30, .dose1=30, .dose2=40, .dose_mode=1, .available=true, .writable=GBS_UI_MACHINE_ACTIONS,
        .steam_enabled=true, .steam_enabled_known=true, .revision=++sequence,
        .prebrew_mode=1,.prebrew_on=2.5,.prebrew_off=3.5,.machine_mode=1,
        .standby_enabled=true,.standby_enabled_known=true,.standby_minutes=30,.standby_mode=0};
    if (n == 0) machine = (gbs_ui_machine_t){.coffee=NAN, .coffee_target=NAN, .steam=NAN, .steam_target=NAN, .weight=NAN};
    if (n == 2 || n == 3 || n == 7 || n == 8) {
        bridge.job = true; bridge.target_grams = 30; bridge.target_seconds = 35;
        bridge.job_deadline_ms = 10000 + (n == 3 ? 24000 : 156000);
        strcpy(bridge.phase, "waiting_for_coffee");
    }
    if (n == 4) { strcpy(bridge.outcome, "delivery_unknown"); bridge.unconfirmed_grams=30; bridge.unconfirmed_seconds=37; }
    if (n == 5) { bridge.grinder=false; bridge.retry_at_ms=15000; strcpy(bridge.phase, "grinder_waiting"); }
    if (n == 6) { strcpy(bridge.outcome, "success"); bridge.successes=12; bridge.result_grams=30; bridge.result_seconds=37; }
    if (n == 7 || n == 8) { bridge.pending=true; bridge.canceling=n==8; bridge.answer_deadline_ms=10000+(n==8?15000:90000); }
    #ifdef HUB_AMOLED_216
    machine.scale_connected_known=machine.scale_connected=machine.scale_battery_known=true;
    machine.scale_battery_percent=78; machine.scale_observed_ms=10000;
    gbs_device_status_t device={.valid=true,.battery_present_known=true,.battery_present=true,
        .battery_percent_known=true,.battery_percent=84,.charging_known=true,.observed_ms=10000};
    gbs_ui_set_device(&device);
#endif
    gbs_ui_set_bridge(&bridge); gbs_ui_set_machine(&machine); gbs_ui_tick(10000);
}

static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i=0; i<lv_obj_get_child_cnt(root); ++i) {
        lv_obj_t *found=find_text(lv_obj_get_child(root, i), text); if (found) return found;
    }
    return NULL;
}

static void click(const char *text)
{
#ifdef HUB_AMOLED_216
    if(gbs_ui_page()==GBS_PAGE_HOME){const char *presets[]={"20g","30g","35g","40g"};for(int i=0;i<4;++i)if(!strcmp(text,presets[i])){pointer_event(75+110*i,345,true);pointer_event(75+110*i,345,false);return;}}
#endif
    lv_obj_t *l = find_text(lv_scr_act(), text); assert(l);
    while (l && !lv_obj_has_flag(l, LV_OBJ_FLAG_CLICKABLE)) l = lv_obj_get_parent(l);
    assert(l); lv_event_send(l, LV_EVENT_CLICKED, NULL);
}

static void tap(int x, int y)
{
    pointer_event(x,y,true); pointer_event(x,y,false);
    lv_tick_inc(100); lv_timer_handler();
}

static bool self_testing;
static void check_glyphs(lv_obj_t *root)
{
    if(lv_obj_check_type(root,&lv_label_class)) {
        const char *text=lv_label_get_text(root); const lv_font_t *font=lv_obj_get_style_text_font(root,0);
        uint32_t offset=0,letter;
        while((letter=_lv_txt_encoded_next(text,&offset))) {
            if(letter<32) continue;
            lv_font_glyph_dsc_t glyph;
            if(!lv_font_get_glyph_dsc(font,&glyph,letter,0)||glyph.is_placeholder) {
                fprintf(stderr,"Missing glyph U+%04x in %s\n",letter,text); assert(false);
            }
        }
    }
    for(unsigned i=0;i<lv_obj_get_child_cnt(root);++i) check_glyphs(lv_obj_get_child(root,i));
}
static void paint(void)
{
    lv_obj_update_layout(lv_scr_act()); lv_refr_now(NULL);
    if(self_testing) check_glyphs(lv_scr_act());
}

static lv_obj_t *find_switch(lv_obj_t *root)
{
    if (lv_obj_check_type(root, &lv_switch_class)) return root;
    for (unsigned i=0; i<lv_obj_get_child_cnt(root); ++i) {
        lv_obj_t *found=find_switch(lv_obj_get_child(root,i)); if (found) return found;
    }
    return NULL;
}

static void save_screen(const char *directory, const char *name);

static void control_regressions(const char *directory)
{
    gbs_ui_init(submit,NULL); scene(1);
    click("35g"); unsigned submitted=requests;
    gbs_ui_tick(45000);
    assert(!gbs_ui_keep_awake() && find_text(lv_scr_act(),"请检查设置"));
    assert(requests==submitted); // An expired confirmation never resends a command.
    if(directory) save_screen(directory,"home-control-timeout");
    pending=false;
    uint32_t expired=command_id;
    click("40g");
    gbs_ui_control_result(expired,GBS_UI_CONTROL_CONFIRMED);
    assert(gbs_ui_keep_awake() && find_text(lv_scr_act(),"等待确认"));
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;

    gbs_ui_init(submit,NULL); scene(1); machine.stale=true;
    gbs_ui_set_machine(&machine); gbs_ui_show(GBS_PAGE_STEAM);
    lv_obj_t *toggle=find_switch(lv_scr_act()); assert(toggle);
    assert(lv_obj_has_state(toggle,LV_STATE_DISABLED));
    tap(199,147); assert(lv_obj_has_state(find_switch(lv_scr_act()),LV_STATE_CHECKED));
    if(directory) save_screen(directory,"steam-stale");
    machine.stale=false; gbs_ui_set_machine(&machine);
    reject_submit=true; tap(199,147); reject_submit=false;
    assert(lv_obj_has_state(find_switch(lv_scr_act()),LV_STATE_CHECKED));
    if(directory) save_screen(directory,"steam-rejected");
    tap(199,147); assert(pending&&lv_obj_has_state(find_switch(lv_scr_act()),LV_STATE_CHECKED));
    assert(lv_obj_has_state(find_switch(lv_scr_act()),LV_STATE_DISABLED));
    if(directory) save_screen(directory,"steam-pending");
    machine.steam_enabled=false; ++machine.revision; gbs_ui_set_machine(&machine);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED); pending=false;
    assert(!lv_obj_has_state(find_switch(lv_scr_act()),LV_STATE_CHECKED)&&find_text(lv_scr_act(),"已确认"));
    if(directory) save_screen(directory,"steam-confirmed");
    tap(199,147); gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;
    assert(!lv_obj_has_state(find_switch(lv_scr_act()),LV_STATE_CHECKED));
    if(directory) save_screen(directory,"steam-unknown");

    gbs_ui_init(submit,NULL); scene(1); bridge.cloud=false;
    gbs_ui_set_bridge(&bridge);
    lv_obj_t *status=find_text(lv_scr_act(),"云端异常"); assert(status);
    lv_obj_t *dot=lv_obj_get_child(lv_obj_get_parent(status),4);
    assert(lv_obj_get_style_bg_color(dot,0).full!=lv_color_hex(0x9fd2aa).full);
    if(directory) save_screen(directory,"home-cloud-error");
    gbs_ui_init(submit,NULL); scene(1);
    machine.coffee=91.5; machine.steam=127; gbs_ui_set_machine(&machine);
    if(directory) save_screen(directory,"home-telemetry");
}

static void save_screen(const char *directory, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.bmp", directory, name);
    paint();
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels, WIDTH, HEIGHT, 32, WIDTH*4, SDL_PIXELFORMAT_ARGB8888);
    assert(surface); assert(SDL_SaveBMP(surface,path)==0); SDL_FreeSurface(surface);
}

static void visible_labels(lv_obj_t *root)
{
    if(lv_obj_check_type(root,&lv_label_class)&&lv_label_get_text(root)[0]) {
        lv_area_t area; lv_obj_get_coords(root,&area);
        assert(area.x1>=0&&area.x2<WIDTH&&area.y1>=0&&area.y2<HEIGHT);
    }
    for(unsigned i=0;i<lv_obj_get_child_cnt(root);++i) visible_labels(lv_obj_get_child(root,i));
}

static void daily_control_regressions(const char *directory)
{
    gbs_ui_init(submit,NULL); scene(1); pending=false;
    gbs_ui_show(GBS_PAGE_MACHINE);
    pointer_event(120,245,true);
    for(int y=245;y>=65;y-=10) { pointer_event(120,y,true); lv_tick_inc(20); lv_timer_handler(); }
    pointer_event(120,65,false); lv_tick_inc(200); lv_timer_handler(); paint();
    assert(gbs_ui_page()==GBS_PAGE_MACHINE);
    lv_obj_t *power_row=find_text(lv_scr_act(),"机器电源"); assert(power_row);
    lv_area_t position; lv_obj_get_coords(power_row,&position);
    assert(position.y1>=44&&position.y2<HEIGHT);
    tap((position.x1+position.x2)/2,(position.y1+position.y2)/2); assert(gbs_ui_page()==GBS_PAGE_POWER);
    gbs_ui_show(GBS_PAGE_BBW); paint(); visible_labels(lv_scr_act());
    if(directory) save_screen(directory,"bbw-summary");
    tap(110,138); assert(gbs_ui_page()==GBS_PAGE_DOSE2);
    tap(193,204); assert(find_text(lv_scr_act(),"应用 41g"));
    if(directory) save_screen(directory,"bbw-dose-edit");
    unsigned count=requests; tap(120,254);
    assert(requests==count+1&&command==GBS_ACTION_DOSE2&&requested==41&&machine.dose2==40);
    if(directory) save_screen(directory,"bbw-dose-pending");
    tap(120,254); assert(requests==count+1);
    finish_requested(); assert(machine.dose1==30&&machine.dose2==41&&machine.dose_mode==1);
    if(directory) save_screen(directory,"bbw-dose-confirmed");
    gbs_ui_show(GBS_PAGE_BBW_MODE); tap(100,194); assert(find_text(lv_scr_act(),"脉冲模式"));
    if(directory) save_screen(directory,"bbw-modes");
    tap(120,254); assert(command==GBS_ACTION_BBW_MODE&&requested==3); finish_requested();
    assert(isnan(machine.weight)&&machine.dose_mode==3);

    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_DOSE1); tap(193,204);
    machine.dose2=50; ++machine.revision; gbs_ui_set_machine(&machine);
    assert(find_text(lv_scr_act(),"设置已变化，请返回")); count=requests; tap(120,254); assert(requests==count);
    if(directory) save_screen(directory,"bbw-conflict");
    tap(22,22); click("继续编辑"); tap(22,22); click("放弃修改");
    gbs_ui_show(GBS_PAGE_DOSE1); tap(193,204);
    machine.dose2=60; // A newer backend snapshot must not bypass the UI's edit baseline.
    count=requests; tap(120,254); assert(requests==count&&!pending);

    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_PREBREW);
    if(directory) save_screen(directory,"prebrew-summary");
    tap(110,78); assert(gbs_ui_page()==GBS_PAGE_PREBREW_MODE); tap(100,179);
    if(directory) save_screen(directory,"prebrew-modes");
    tap(120,254); assert(command==GBS_ACTION_PREBREW_MODE&&requested==2); finish_requested();
    assert(machine.prebrew_on==2.5&&machine.prebrew_off==3.5);
    gbs_ui_show(GBS_PAGE_PREBREW_ON); tap(193,204);
    assert(find_text(lv_scr_act(),"应用 2.6s"));
    if(directory) save_screen(directory,"prebrew-time");
    tap(120,254); assert(command==GBS_ACTION_PREBREW_ON&&requested==2.6); finish_requested();
    assert(machine.prebrew_off==3.5&&machine.prebrew_mode==2);
    gbs_ui_show(GBS_PAGE_PREBREW_OFF); tap(47,204); tap(120,254);
    assert(command==GBS_ACTION_PREBREW_OFF&&requested==3.4); finish_requested();
    assert(machine.prebrew_on==2.6);
    machine.prebrew_on=10; gbs_ui_set_machine(&machine); gbs_ui_show(GBS_PAGE_PREBREW_ON);
    count=requests; tap(193,204); tap(120,254); assert(requests==count);
    machine.prebrew_on=0; gbs_ui_set_machine(&machine); gbs_ui_show(GBS_PAGE_PREBREW_ON);
    tap(47,204); tap(120,254); assert(requests==count);

    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_POWER);
    if(directory) save_screen(directory,"power");
    count=requests; tap(110,160); tap(120,254);
    assert(requests==count&&find_text(lv_scr_act(),"确认机器设置"));
    if(directory) save_screen(directory,"power-confirm");
    click("返回编辑"); assert(requests==count&&gbs_ui_page()==GBS_PAGE_POWER);
    tap(120,254); click("确认应用");
    assert(requests==count+1&&command==GBS_ACTION_POWER&&requested==0&&machine.machine_mode==1);
    assert(find_text(lv_scr_act(),"加热开启"));
    if(directory) save_screen(directory,"power-pending");
    tap(110,160); tap(120,254); assert(requests==count+1); finish_requested();
    assert(find_text(lv_scr_act(),"待机中"));
    if(directory) save_screen(directory,"power-confirmed");
    tap(110,206); tap(120,254); click("确认应用"); finish_requested(); assert(machine.machine_mode==1);

    gbs_ui_show(GBS_PAGE_STANDBY);
    if(directory) save_screen(directory,"standby-summary");
    const gbs_ui_page_t pages[]={GBS_PAGE_STANDBY_ENABLED,GBS_PAGE_STANDBY_MINUTES,GBS_PAGE_STANDBY_MODE};
    const gbs_ui_action_t actions[]={GBS_ACTION_STANDBY_ENABLED,GBS_ACTION_STANDBY_MINUTES,GBS_ACTION_STANDBY_MODE};
    for(unsigned i=0;i<3;++i) {
        gbs_ui_show(pages[i]); if(i==1) tap(193,204); else tap(110,i==0?78:128);
        count=requests; tap(120,254); assert(requests==count&&find_text(lv_scr_act(),"确认机器设置"));
        if(directory&&i==1) save_screen(directory,"standby-confirm");
        click("确认应用"); assert(requests==count+1&&command==actions[i]); finish_requested();
    }
    assert(!machine.standby_enabled&&machine.standby_minutes==31&&machine.standby_mode==1);
    gbs_ui_show(GBS_PAGE_STANDBY_MINUTES); tap(193,204); tap(120,254);
    machine.standby_mode=0; ++machine.revision; gbs_ui_set_machine(&machine);
    count=requests; assert(find_text(lv_scr_act(),"设置已变化，请返回")); tap(176,254); assert(requests==count);
    if(directory) save_screen(directory,"standby-conflict");

    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_PREBREW_ON); tap(193,204); tap(120,254);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_PARTIAL); pending=false;
    assert(machine.prebrew_on==2.5&&find_text(lv_scr_act(),"部分应用"));
    if(directory) save_screen(directory,"prebrew-partial");
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_POWER); tap(110,160); tap(120,254); click("确认应用");
    count=requests; gbs_ui_tick(45000); pending=false;
    assert(requests==count&&machine.machine_mode==1&&find_text(lv_scr_act(),"结果未确认"));
    if(directory) save_screen(directory,"power-unknown");
    machine.stale=true; gbs_ui_set_machine(&machine); tap(110,160); tap(120,254); assert(requests==count);
    if(directory) save_screen(directory,"power-stale");
    machine.stale=false; machine.writable=0; gbs_ui_set_machine(&machine);
    gbs_ui_show(GBS_PAGE_PREBREW_MODE); tap(110,180); tap(120,254); assert(requests==count);
    if(directory) save_screen(directory,"prebrew-unavailable");

    gbs_ui_init(submit,NULL); scene(1);
    for(unsigned i=GBS_PAGE_DOSE1;i<GBS_PAGE_COUNT;++i) {
        gbs_ui_show((gbs_ui_page_t)i); paint(); visible_labels(lv_scr_act());
    }
    puts("daily UI: BBW/prebrew/power/standby navigation, preserved values, confirmation, conflicts, limits and unavailable states passed");
}

static void collection_regressions(const char *directory)
{
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_set_reader(read_data);
    unsigned before=requests,reads=data_requests;
    gbs_ui_show(GBS_PAGE_SCHEDULE); assert(data_requests==reads+1);
    tap(211,22); assert(data_requests==reads+1);
    if(directory) save_screen(directory,"schedule-loading");
    finish_data(GBS_DATA_SCHEDULE);
    assert(find_text(lv_scr_act(),"07:00 - 09:30"));
    if(directory) save_screen(directory,"schedule");
    tap(120,92); assert(gbs_ui_page()==GBS_PAGE_SCHEDULE_DETAIL);
    if(directory) save_screen(directory,"schedule-detail");
    tap(22,22); tap(207,255); assert(find_text(lv_scr_act(),"23:00 - 01:00"));
    tap(32,255); assert(find_text(lv_scr_act(),"07:00 - 09:30"));
    gbs_ui_show(GBS_PAGE_COFFEES); finish_data(GBS_DATA_HISTORY);
    if(directory) save_screen(directory,"history");
    tap(120,92); assert(gbs_ui_page()==GBS_PAGE_COFFEE_DETAIL&&find_text(lv_scr_act(),"31.2 s"));
    if(directory) save_screen(directory,"history-detail");
    tap(22,22); tap(207,255); tap(120,92); assert(gbs_ui_page()==GBS_PAGE_COFFEE_DETAIL&&find_text(lv_scr_act(),"—"));
    if(directory) save_screen(directory,"history-missing-values");
    tap(22,22);
    gbs_ui_data_t data=sample_data(GBS_DATA_HISTORY); data.state=GBS_DATA_STALE; gbs_ui_set_data(&data);
    if(directory) save_screen(directory,"history-stale");
    data.count=data.total=0; data.state=GBS_DATA_READY; gbs_ui_set_data(&data);
    assert(find_text(lv_scr_act(),"暂无出杯记录"));
    if(directory) save_screen(directory,"history-empty");
    data.valid=false; data.state=GBS_DATA_ERROR; gbs_ui_set_data(&data);
    if(directory) save_screen(directory,"history-error");
    assert(requests==before);
    puts("collections UI: on-demand refresh, pagination, details, missing metrics and stale/error/empty states passed");
}

static void schedule_edit_regressions(const char *directory)
{
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_set_reader(read_data); gbs_ui_set_schedule_submit(submit_schedule);
    gbs_ui_show(GBS_PAGE_SCHEDULE); finish_data(GBS_DATA_SCHEDULE); tap(120,92); click("编辑计划");
    assert(gbs_ui_page()==GBS_PAGE_SCHEDULE_EDIT); unsigned before=requests;
    tap(60,75); assert(gbs_ui_page()==GBS_PAGE_SCHEDULE_ON); tap(90,190); tap(206,190);
    if(directory) save_screen(directory,"schedule-time-edit");
    click("完成"); assert(gbs_ui_page()==GBS_PAGE_SCHEDULE_EDIT);
    tap(120,132); assert(gbs_ui_page()==GBS_PAGE_SCHEDULE_DAYS); tap(38,179);
    if(directory) save_screen(directory,"schedule-days-edit");
    click("完成"); tap(177,187); assert(requests==before);
    if(directory) save_screen(directory,"schedule-edit");
    click("保存计划"); assert(find_text(lv_scr_act(),"确认保存")&&requests==before);
    if(directory) save_screen(directory,"schedule-confirm");
    click("确认保存"); assert(requests==before+1&&pending&&requested_plan.on_minutes==481&&!requested_plan.steam);
    if(directory) save_screen(directory,"schedule-saving");
    finish_schedule(); assert(find_text(lv_scr_act(),"已确认保存"));
    if(directory) save_screen(directory,"schedule-saved");
    click("返回计划"); finish_data(GBS_DATA_SCHEDULE); tap(120,92); click("编辑计划"); tap(64,188);
    click("保存计划"); click("确认保存"); gbs_ui_tick(45000); pending=schedule_pending=false;
    assert(find_text(lv_scr_act(),"结果未确认")&&requests==before+2);
    if(directory) save_screen(directory,"schedule-unknown");
    click("返回计划"); finish_data(GBS_DATA_SCHEDULE); tap(120,92); click("编辑计划"); tap(64,188);
    tap(22,22); assert(find_text(lv_scr_act(),"放弃这次修改？")); click("放弃修改");
    assert(gbs_ui_page()==GBS_PAGE_SCHEDULE_DETAIL);
    click("编辑计划"); tap(64,188);
    gbs_ui_data_t changed=sample_data(GBS_DATA_SCHEDULE); ++changed.items.schedules[0].off_minutes;
    gbs_ui_set_data(&changed); assert(find_text(lv_scr_act(),"计划已变化，请重新打开"));
    click("保存计划"); assert(requests==before+2&&!find_text(lv_scr_act(),"确认保存"));
    if(directory) save_screen(directory,"schedule-conflict");
    memset(&saved_plan,0,sizeof(saved_plan));
    puts("schedule editor: draft-only time/day/toggle edits, explicit confirmation, save readback, conflict, timeout and discard passed");
}

static void pointer_regressions(void)
{
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_MACHINE);
    SDL_Event e={0}; e.type=SDL_MOUSEBUTTONDOWN; e.button.button=SDL_BUTTON_LEFT; e.button.x=360; e.button.y=735;
    mouse_event(&e);
    // Coalesced delivery may carry the entire drag in its release position.
    e.type=SDL_MOUSEBUTTONUP; e.button.y=195; mouse_event(&e); paint();
    assert(gbs_ui_page()==GBS_PAGE_MACHINE);
    lv_obj_t *power=find_text(lv_scr_act(),"机器电源"); assert(power);
    lv_area_t bounds; lv_obj_get_coords(power,&bounds); assert(bounds.y1>44&&bounds.y2<280);
    tap(120,(bounds.y1+bounds.y2)/2); assert(gbs_ui_page()==GBS_PAGE_POWER);
    gbs_ui_show(GBS_PAGE_MACHINE);
    e.type=SDL_MOUSEBUTTONDOWN; e.button.button=SDL_BUTTON_RIGHT; e.button.x=360; e.button.y=195; mouse_event(&e);
    e.type=SDL_MOUSEBUTTONUP; mouse_event(&e); assert(gbs_ui_page()==GBS_PAGE_MACHINE);
    puts("pointer: coalesced drag release does not click a row; subsequent tap and left-button filtering passed");
}

static void legacy_self_test(const char *directory)
{
    assert(gbs_ui_dim_enabled());
    scene(1);
    gbs_ui_show(GBS_PAGE_HOME); paint();
    assert(find_text(lv_scr_act(), "2") && find_text(lv_scr_act(), "当前 128°") && find_text(lv_scr_act(), "目标 128°"));
    if (directory) save_screen(directory, "home-steam-level");
    machine.steam_target = 131; machine.steam = 126; gbs_ui_set_machine(&machine); paint();
    assert(find_text(lv_scr_act(), "3") && find_text(lv_scr_act(), "当前 126°") && find_text(lv_scr_act(), "目标 131°"));
    bridge.invalid_coffee = bridge.job = true; gbs_ui_set_bridge(&bridge); gbs_ui_show(GBS_PAGE_LINK); paint();
    assert(find_text(lv_scr_act(), "萃取记录异常") && find_text(lv_scr_act(), "本杯未回传"));
    if (directory) save_screen(directory, "link-invalid-coffee");
    scene(1); gbs_ui_show(GBS_PAGE_HOME);
    tap(214,22); assert(gbs_ui_page()==GBS_PAGE_SETTINGS);
    scene(2); assert(gbs_ui_page()==GBS_PAGE_SETTINGS); // Background jobs never navigate.
    gbs_ui_show(GBS_PAGE_HOME);
    for (unsigned i=0; i<8; ++i) {
        tap(120,244); assert(gbs_ui_page()==GBS_PAGE_LINK);
        tap(22,22); assert(gbs_ui_page()==GBS_PAGE_HOME);
    }
    click("35g"); click("40g"); assert(requests==1 && requested==35);
    assert(machine.weight==30); // Accepted commands cannot fill a new selected preset.
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;
    scene(5); assert(find_text(lv_scr_act(),"请检查设置"));
    gbs_ui_show(GBS_PAGE_WEIGHT); tap(193,204);
    machine.weight=machine.dose1=40; ++machine.revision; gbs_ui_set_machine(&machine);
    assert(find_text(lv_scr_act(),"设置已变化，请返回"));
    tap(22,22); assert(find_text(lv_scr_act(),"放弃这次修改？"));
    click("继续编辑"); assert(gbs_ui_page()==GBS_PAGE_WEIGHT);
    tap(22,22); click("放弃修改");
    gbs_ui_show(GBS_PAGE_HOME); scene(0);
    lv_obj_t *preset=find_text(lv_scr_act(),"20g"); assert(preset);
    assert(lv_obj_has_state(lv_obj_get_parent(lv_obj_get_parent(preset)),LV_STATE_DISABLED));
    for (unsigned p=0; p<GBS_PAGE_COUNT; ++p) {
        gbs_ui_show((gbs_ui_page_t)p); paint();
        if(directory) { char name[32]; snprintf(name,sizeof(name),"page-%02u",p); save_screen(directory,name); }
    }
    gbs_ui_init(submit,NULL);
    for (unsigned n=0; n<=8; ++n) {
        scene(n); gbs_ui_show(GBS_PAGE_HOME); paint();
        if(directory) { char name[32]; snprintf(name,sizeof(name),"home-%u",n); save_screen(directory,name); }
        gbs_ui_show(GBS_PAGE_LINK);
        if(directory) { char name[32]; snprintf(name,sizeof(name),"link-%u",n); save_screen(directory,name); }
    }
    if(directory) {
        gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_SETTINGS); save_screen(directory,"settings");
        gbs_ui_show(GBS_PAGE_MACHINE); save_screen(directory,"machine");
        gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_WEIGHT); tap(193,204); tap(193,204);
        save_screen(directory,"quick-edit"); tap(120,253); save_screen(directory,"quick-pending");
        machine.weight=machine.dose1=32; ++machine.revision; gbs_ui_set_machine(&machine); gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);
        pending=false; save_screen(directory,"quick-confirmed");
        gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_COFFEE); tap(193,204); save_screen(directory,"boiler");
        machine.steam_target=126; gbs_ui_set_machine(&machine); gbs_ui_show(GBS_PAGE_STEAM); tap(120,210);
        assert(find_text(lv_scr_act(),"应用 2 档")); save_screen(directory,"steam");
        gbs_ui_set_preferences(70,true); gbs_ui_show(GBS_PAGE_DISPLAY); save_screen(directory,"display");
        bridge.active=false; gbs_ui_set_bridge(&bridge); gbs_ui_show(GBS_PAGE_MODE); tap(110,155); save_screen(directory,"mode");
        scene(5); gbs_ui_show(GBS_PAGE_CONNECTIONS); save_screen(directory,"connections");
        scene(6); gbs_ui_show(GBS_PAGE_RECORD); save_screen(directory,"record");
    }
    control_regressions(directory);
    daily_control_regressions(directory);
    pointer_regressions();
    collection_regressions(directory);
    schedule_edit_regressions(directory);
    puts("native UI: navigation, confirmation timeout, stale/rejected steam controls, link colors and all pages passed");
}

#ifdef HUB_AMOLED_216
static bool scale_pending;
static uint32_t scale_started;
static bool scale_read(void *context)
{
    (void)context; ++data_requests; scale_pending=true; scale_started=SDL_GetTicks(); return true;
}
static void assert_idle_refresh(void)
{
    for(unsigned i=0;i<4;++i) { lv_tick_inc(100); lv_timer_handler(); }
    paint();
    unsigned before=flush_count;
    gbs_device_status_t device={.valid=true,.battery_present_known=true,.battery_present=true,
        .battery_percent_known=true,.battery_percent=84,.charging_known=true};
    for(unsigned i=0;i<8;++i) {
        device.observed_ms=10000+i*50;
        gbs_ui_set_machine(&machine); gbs_ui_set_bridge(&bridge); gbs_ui_set_device(&device); gbs_ui_tick(10000+i*50);
        lv_tick_inc(50); lv_timer_handler(); paint();
    }
    if(flush_count!=before) fprintf(stderr,"Idle page %u produced %u flushes\n",gbs_ui_page(),flush_count-before);
    assert(flush_count==before);
}

static void amoled_refresh_test(void)
{
    gbs_ui_init(submit,NULL); scene(1); pending=false;
    const gbs_ui_page_t pages[]={GBS_PAGE_HOME,GBS_PAGE_DISPLAY,GBS_PAGE_WEIGHT,GBS_PAGE_COFFEE,
        GBS_PAGE_STEAM,GBS_PAGE_LINK,GBS_PAGE_RECORD,GBS_PAGE_SCALE,GBS_PAGE_DEVICE_BATTERY};
    for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);++i) { gbs_ui_show(pages[i]); assert_idle_refresh(); }
    gbs_ui_show(GBS_PAGE_HOME); paint();
    lv_obj_t *twenty=lv_obj_get_parent(find_text(lv_scr_act(),"20"));
    lv_obj_t *thirty_button=lv_obj_get_child(lv_scr_act(),lv_obj_get_index(lv_obj_get_parent(twenty))+1);
    lv_obj_t *thirty=lv_obj_get_parent(find_text(thirty_button,"30"));
    lv_color_t selected=lv_obj_get_style_bg_color(thirty,0),unselected=lv_obj_get_style_bg_color(twenty,0);
    assert(selected.full!=unselected.full);
    unsigned before=flush_count; machine.weight=20;machine.coffee=94.2;gbs_ui_set_machine(&machine);paint();
    assert(flush_count>before&&find_text(lv_scr_act(),"94.2°"));
    assert(lv_obj_get_style_bg_color(twenty,0).full==selected.full&&lv_obj_get_style_bg_color(thirty,0).full==unselected.full);
    assert_idle_refresh();
    const unsigned scenes[]={4,5,6,7};
    for(unsigned i=0;i<sizeof(scenes)/sizeof(*scenes);++i) {scene(scenes[i]);assert_idle_refresh();}
    scene(2);bridge.recipe_enabled=true;bridge.recipe_grams=30;strcpy(bridge.recipe_phase,"applying");gbs_ui_set_bridge(&bridge);assert_idle_refresh();
    scene(2);paint();assert(find_text(lv_scr_act(),"02:36"));before=flush_count;
    gbs_ui_tick(11000);paint();assert(flush_count>before&&find_text(lv_scr_act(),"02:35"));
    scene(1);machine.stale=true;gbs_ui_set_machine(&machine);assert_idle_refresh();
    scene(1);gbs_ui_show(GBS_PAGE_WEIGHT);tap(350,340);assert_idle_refresh();
    tap(240,432);gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);pending=false;assert_idle_refresh();
    puts("AMOLED refresh: unchanged native pages submit zero flushes; telemetry, selection and control changes still refresh");
}
static void assert_amoled_preset(unsigned selected)
{
    lv_obj_update_layout(lv_scr_act());
    unsigned found=0;
    for(unsigned i=0;i<lv_obj_get_child_cnt(lv_scr_act());++i) {
        lv_obj_t *button=lv_obj_get_child(lv_scr_act(),i);
        if(lv_obj_get_y(button)!=310||lv_obj_get_child_cnt(button)!=2) continue;
        int index=(lv_obj_get_x(button)-24)/110;
        if(index<0||index>3) continue;
        lv_obj_t *face=lv_obj_get_child(button,1);
        assert(lv_obj_get_style_bg_color(face,0).full==lv_color_hex((unsigned)index==selected?0xe3bb83:0x202420).full);
        ++found;
    }
    assert(found==4);
}
static void assert_scale_percentage(const char *text,uint32_t color)
{
    paint();
    lv_obj_t *reading=find_text(lv_scr_act(),text);
    assert(reading&&lv_obj_get_style_text_color(reading,0).full==lv_color_hex(color).full);
    const lv_font_t *font=lv_obj_get_style_text_font(reading,0);
    assert(lv_obj_get_height(reading)<=font->line_height);
    visible_labels(lv_scr_act());
}
static void assert_amoled_outline(int outlined)
{
    lv_obj_update_layout(lv_scr_act());
    unsigned found=0;
    for(unsigned i=0;i<lv_obj_get_child_cnt(lv_scr_act());++i) {
        lv_obj_t *button=lv_obj_get_child(lv_scr_act(),i);
        if(lv_obj_get_y(button)!=310||lv_obj_get_child_cnt(button)!=2) continue;
        int index=(lv_obj_get_x(button)-24)/110;
        if(index<0||index>3) continue;
        lv_obj_t *face=lv_obj_get_child(button,1);
        assert(lv_obj_get_style_border_width(face,0)==(index==outlined?2:1));
        if(index==outlined) assert(lv_obj_get_style_border_color(face,0).full==lv_color_hex(0xe3bb83).full);
        ++found;
    }
    assert(found==4);
}
static void amoled_manual_after_receipt_test(const char *directory)
{
    gbs_ui_init(submit,NULL); scene(6); pending=false;
    gbs_ui_set_device_preferences(false,true);
    unsigned count=requests,reads=data_requests;
    click("40g");
    if(directory) save_screen(directory,"manual-after-receipt-pending");
    assert(pending&&requests==count+1&&requested==40);
    assert(find_text(lv_scr_act(),"手动")&&find_text(lv_scr_act(),"设定中"));
    assert(!find_text(lv_scr_act(),"已接收")&&!find_text(lv_scr_act(),"30.0")&&!find_text(lv_scr_act(),"37.0s"));
    assert_amoled_preset(1); assert_amoled_outline(3);
    finish_requested();
    assert(find_text(lv_scr_act(),"已设定")&&!find_text(lv_scr_act(),"已接收"));
    assert_amoled_preset(3); assert_amoled_outline(-1);
    assert(gbs_ui_take_tone()==GBS_TONE_SUCCESS&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_set_bridge(&bridge); gbs_ui_set_machine(&machine); gbs_ui_tick(11000);
    assert_idle_refresh(); assert_amoled_preset(3); assert_amoled_outline(-1);
    if(directory) save_screen(directory,"manual-after-receipt-confirmed");
    gbs_ui_show(GBS_PAGE_LINK);
    assert(find_text(lv_scr_act(),"已设定")&&!find_text(lv_scr_act(),"已接收"));
    assert(find_text(lv_scr_act(),"设定已完成")&&!find_text(lv_scr_act(),"设定已完成，本次自动跟随已取消"));
    if(directory) save_screen(directory,"manual-after-receipt-detail");
    gbs_ui_show(GBS_PAGE_HOME);
    bridge.job=true; bridge.job_deadline_ms=190000; bridge.recipe_enabled=true; bridge.recipe_grams=30;
    strcpy(bridge.job_id,"next-cup"); strcpy(bridge.recipe_phase,"applying"); gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"配方")&&find_text(lv_scr_act(),"同步中")&&!find_text(lv_scr_act(),"手动"));
    assert_amoled_preset(3); assert_amoled_outline(1);
    if(directory) save_screen(directory,"next-cup-recipe-pending");
    strcpy(bridge.recipe_phase,"applied"); machine.weight=machine.dose1=30; machine.dose_mode=1;
    gbs_ui_set_machine(&machine); gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"已同步")); assert_amoled_preset(1); assert_amoled_outline(-1);
    if(directory) save_screen(directory,"next-cup-recipe-confirmed");
    click("40g"); finish_requested();
    gbs_ui_show(GBS_PAGE_LINK);
    assert(find_text(lv_scr_act(),"设定已完成，本次自动跟随已取消"));
    gbs_ui_show(GBS_PAGE_HOME);
    bridge.job=false; ++bridge.successes; bridge.result_grams=39.9; bridge.result_seconds=26.1;
    gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"已接收")&&find_text(lv_scr_act(),"39.9")&&!find_text(lv_scr_act(),"手动"));
    assert_amoled_preset(3); assert_amoled_outline(-1);
    click("35g"); gbs_ui_control_result(command_id,GBS_UI_CONTROL_FAILED); pending=false;
    assert(find_text(lv_scr_act(),"失败")&&!find_text(lv_scr_act(),"已接收"));
    assert_amoled_preset(3); assert_amoled_outline(2);
    click("20g"); gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;
    assert(find_text(lv_scr_act(),"未确认")&&!find_text(lv_scr_act(),"已设定"));
    assert_amoled_preset(3); assert_amoled_outline(0);
    assert(requests==count+4&&data_requests==reads);
    puts("AMOLED consecutive cups: completed receipt -> manual preset -> next recipe, exact outline state, failures, new receipt and no extra requests passed");
}
static void amoled_cup_tone_test(void)
{
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_set_device_preferences(false,true);
    unsigned controls=requests,reads=data_requests;
    bridge.job=true;bridge.job_deadline_ms=190000;strcpy(bridge.job_id,"cup-one");
    bridge.recipe_enabled=bridge.recipe_current=true;strcpy(bridge.recipe_phase,"waiting");
    gbs_ui_set_bridge(&bridge); assert(gbs_ui_take_tone()==GBS_TONE_NOTICE&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_set_bridge(&bridge); assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    bridge.recipe_current=false;strcpy(bridge.recipe_phase,"applied");gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE); // An old recipe is not this cup's success.
    bridge.recipe_current=true;strcpy(bridge.recipe_phase,"applying");gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    strcpy(bridge.recipe_phase,"applied");gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_SUCCESS&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_set_bridge(&bridge);assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    bridge.pending=true;bridge.unconfirmed_grams=29.7;bridge.unconfirmed_seconds=25.656;gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NOTICE&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_set_bridge(&bridge);assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    bridge.job=bridge.pending=false;bridge.successes=1;strcpy(bridge.outcome,"success");gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_SUCCESS&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_set_bridge(&bridge);assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    // Coalesced snapshots must not overwrite adjacent node notifications.
    bridge.job=true;bridge.job_deadline_ms+=100000;strcpy(bridge.job_id,"cup-two");gbs_ui_set_bridge(&bridge);
    bridge.job=false;bridge.successes=2;gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NOTICE&&gbs_ui_take_tone()==GBS_TONE_SUCCESS);
    assert(gbs_ui_take_tone()==GBS_TONE_NOTICE&&gbs_ui_take_tone()==GBS_TONE_SUCCESS);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    const char *phases[]={"failed","unconfirmed"};
    for(unsigned i=0;i<2;++i) {
        gbs_ui_init(submit,NULL);scene(1);gbs_ui_set_device_preferences(false,true);
        bridge.job=true;strcpy(bridge.job_id,"cup-fault");bridge.recipe_enabled=bridge.recipe_current=true;
        strcpy(bridge.recipe_phase,phases[i]);gbs_ui_set_bridge(&bridge);
        assert(gbs_ui_take_tone()==GBS_TONE_NOTICE);
        assert(gbs_ui_take_tone()==(i?GBS_TONE_NOTICE:GBS_TONE_FAILURE));
        gbs_ui_set_bridge(&bridge);assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    }
    // Booting into a cached live job is quiet; its next new measurement can still notify.
    gbs_ui_init(submit,NULL);gbs_ui_set_device_preferences(false,true);
    strcpy(bridge.recipe_phase,"applied");gbs_ui_set_bridge(&bridge);assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    bridge.pending=true;bridge.unconfirmed_grams=20;bridge.unconfirmed_seconds=25;gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NOTICE);
    gbs_ui_set_device_preferences(false,false);strcpy(bridge.job_id,"muted-cup");gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE);gbs_ui_set_device_preferences(false,true);gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE); // Enabling sound never replays muted stages.
    bridge.job=false;bridge.pending=false;gbs_ui_set_bridge(&bridge);
    bridge.job=true;gbs_ui_set_bridge(&bridge);assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    assert(controls==requests&&reads==data_requests);
}

static void amoled_boiler_warmup_test(const char *directory)
{
    gbs_ui_init(submit,NULL); scene(1); pending=false;
    unsigned controls=requests,reads=data_requests;
    int64_t now=time(NULL);
    machine.machine_mode=1; machine.boiler_observed_unix=now;
    machine.coffee_ready_unix=now+270; machine.steam_ready_unix=now+450;
    gbs_ui_set_machine(&machine); gbs_ui_show(GBS_PAGE_HOME); paint();
    lv_obj_t *coffee=find_text(lv_scr_act(),"~5 min"),*steam=find_text(lv_scr_act(),"~8 min");
    assert(coffee&&steam&&lv_obj_get_style_text_color(coffee,0).full==lv_color_hex(0xe3bb83).full);
    const lv_font_t *countdown_font=lv_obj_get_style_text_font(coffee,0);
    lv_area_t area; lv_obj_get_coords(steam,&area); assert(area.x2<WIDTH&&area.y2<HEIGHT);
    assert_idle_refresh(); if(directory) save_screen(directory,"boiler-warming-both");
    machine.coffee_ready_unix=now-1; gbs_ui_set_machine(&machine); paint();
    assert(find_text(lv_scr_act(),"93°")&&find_text(lv_scr_act(),"~8 min")&&!find_text(lv_scr_act(),"~5 min"));
    assert(lv_obj_get_style_text_font(find_text(lv_scr_act(),"93°"),0)!=countdown_font);
    if(directory) save_screen(directory,"boiler-steam-still-warming");
    machine.steam_enabled_known=true;machine.steam_enabled=false;gbs_ui_set_machine(&machine);paint();
    assert(!find_text(lv_scr_act(),"~8 min"));
    machine.steam_enabled=true;machine.boiler_observed_unix=now-300;gbs_ui_set_machine(&machine);paint();
    assert(!find_text(lv_scr_act(),"~8 min")); assert_idle_refresh();
    if(directory) save_screen(directory,"boiler-estimate-expired");
    machine.boiler_observed_unix=now;machine.stale=true;gbs_ui_set_machine(&machine);paint();
    assert(!find_text(lv_scr_act(),"~8 min"));
    machine.stale=false;machine.steam_ready_unix=0;gbs_ui_set_machine(&machine);paint();
    assert(!find_text(lv_scr_act(),"~8 min")&&requests==controls&&data_requests==reads);
}

static void amoled_scale_history_test(const char *directory)
{
    gbs_ui_init(submit,NULL);scene(1);pending=false;
    machine.scale_battery_last_known=true;machine.scale_battery_last_percent=28;
    machine.scale_battery_last_observed_unix=1792666020;
    const unsigned percentages[]={0,1,100};
    for(unsigned i=0;i<3;++i) {
        char text[16],name[48];snprintf(text,sizeof(text),"%u%%",percentages[i]);
        machine.scale_battery_percent=percentages[i];gbs_ui_set_machine(&machine);
        gbs_ui_show(GBS_PAGE_HOME);assert_scale_percentage(text,0x9fd2aa);
        assert(!find_text(lv_scr_act(),"28%"));
        if(directory) {snprintf(name,sizeof(name),"scale-current-%u-home",percentages[i]);save_screen(directory,name);}
        gbs_ui_show(GBS_PAGE_SCALE);assert_scale_percentage(text,0x9fd2aa);
        assert(!find_text(lv_scr_act(),"上次电量"));
    }
    machine.scale_connected=false;machine.scale_battery_known=false;
    for(unsigned i=0;i<3;++i) {
        char text[16],name[48];snprintf(text,sizeof(text),"%u%%",percentages[i]);
        machine.scale_battery_last_percent=percentages[i];gbs_ui_set_machine(&machine);
        gbs_ui_show(GBS_PAGE_HOME);assert_scale_percentage(text,0xb6beb3);
        if(directory) {snprintf(name,sizeof(name),"scale-history-%u-home",percentages[i]);save_screen(directory,name);}
        gbs_ui_show(GBS_PAGE_SCALE);assert_scale_percentage(text,0xb6beb3);
        assert(find_text(lv_scr_act(),"未连接")&&find_text(lv_scr_act(),"上次电量")&&
               find_text(lv_scr_act(),"上次读取 2026-10-22 10:47 UTC"));
        if(directory) {snprintf(name,sizeof(name),"scale-history-%u-detail",percentages[i]);save_screen(directory,name);}
    }
    machine.scale_battery_last_percent=28;machine.scale_connected=true;
    gbs_ui_set_machine(&machine);assert_scale_percentage("28%",0xb6beb3);
    assert(find_text(lv_scr_act(),"已连接")&&find_text(lv_scr_act(),"上次电量"));
    if(directory)save_screen(directory,"scale-online-history-detail");
    machine.scale_battery_known=true;machine.scale_battery_percent=78;machine.scale_stale=true;
    gbs_ui_set_machine(&machine);gbs_ui_tick(80000);assert_scale_percentage("28%",0xb6beb3);
    assert(find_text(lv_scr_act(),"状态已过期")&&!find_text(lv_scr_act(),"78%"));
    if(directory)save_screen(directory,"scale-stale-history-detail");
    gbs_ui_init(submit,NULL);scene(1);
    machine.scale_connected_known=machine.scale_connected=machine.scale_battery_known=false;
    machine.scale_stale=true;machine.scale_observed_ms=0;
    machine.scale_battery_last_known=true;machine.scale_battery_last_percent=28;
    machine.scale_battery_last_observed_unix=1792666020;
    gbs_ui_set_machine(&machine);gbs_ui_show(GBS_PAGE_HOME);assert_scale_percentage("28%",0xb6beb3);
    assert_idle_refresh();if(directory)save_screen(directory,"scale-coldboot-history-home");
    unsigned before=requests;click("35g");assert(pending&&requests==before+1&&requested==35);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN);pending=false;
    gbs_ui_show(GBS_PAGE_SCALE);assert_scale_percentage("28%",0xb6beb3);
    assert(find_text(lv_scr_act(),"状态未知")&&find_text(lv_scr_act(),"上次电量"));
    assert_idle_refresh();if(directory)save_screen(directory,"scale-coldboot-history-detail");
    machine.scale_battery_last_observed_unix=0;gbs_ui_set_machine(&machine);paint();
    assert(find_text(lv_scr_act(),"上次读取时间未知")&&!find_text(lv_scr_act(),"上次读取 2026-10-22 10:47 UTC"));
    if(directory)save_screen(directory,"scale-history-unknown-time-detail");
    machine.scale_battery_last_known=false;gbs_ui_set_machine(&machine);paint();
    assert(!find_text(lv_scr_act(),"28%")&&find_text(lv_scr_act(),"—")&&!find_text(lv_scr_act(),"上次电量"));
    if(directory)save_screen(directory,"scale-no-history-detail");
    machine.scale_battery_last_known=true;machine.scale_battery_last_percent=101;
    gbs_ui_set_machine(&machine);paint();assert(!find_text(lv_scr_act(),"101%")&&find_text(lv_scr_act(),"—"));
    puts("AMOLED scale history: current priority, gray retained battery, 0/1/100%, cold start, unknown time and unchanged refresh passed");
}
static void amoled_self_test(const char *directory)
{
    amoled_manual_after_receipt_test(directory);
    gbs_ui_init(submit,NULL); scene(1); pending=false;
    gbs_ui_set_scale_refresh(scale_read);
    assert(!gbs_ui_rotation_auto()&&!gbs_ui_sound_enabled()&&!gbs_ui_dim_enabled());
    paint(); visible_labels(lv_scr_act());
    assert(find_text(lv_scr_act(),"78%")&&find_text(lv_scr_act(),"84%")&&find_text(lv_scr_act(),"30"));
    if(directory) save_screen(directory,"amoled-home");
    tap(420,48); assert(gbs_ui_page()==GBS_PAGE_SETTINGS);
    gbs_ui_show(GBS_PAGE_HOME); tap(255,48); assert(gbs_ui_page()==GBS_PAGE_SCALE);
    unsigned reads=data_requests; click("刷新"); assert(data_requests==reads+1);
    if(directory) save_screen(directory,"amoled-scale");
    machine.scale_connected=false; gbs_ui_set_machine(&machine); paint();
    assert(find_text(lv_scr_act(),"未连接")&&find_text(lv_scr_act(),"—")&&!find_text(lv_scr_act(),"78%"));
    machine.scale_stale=true; gbs_ui_set_machine(&machine); assert(find_text(lv_scr_act(),"状态已过期"));
    if(directory) save_screen(directory,"amoled-scale-stale");
    machine.scale_observed_ms=0;machine.scale_connected_known=false;gbs_ui_set_machine(&machine);assert(find_text(lv_scr_act(),"状态未知"));
    machine.scale_observed_ms=10000;machine.scale_connected_known=true;
    machine.scale_stale=false; machine.scale_connected=true; machine.scale_battery_percent=1;
    gbs_ui_set_machine(&machine); assert(find_text(lv_scr_act(),"1%"));
    gbs_ui_show(GBS_PAGE_HOME); tap(315,48); assert(gbs_ui_page()==GBS_PAGE_DEVICE_BATTERY);
    if(directory) save_screen(directory,"amoled-battery");
    gbs_device_status_t missing={.valid=true,.battery_present_known=true}; gbs_ui_set_device(&missing);
    assert(find_text(lv_scr_act(),"未检测到")&&!find_text(lv_scr_act(),"84%"));
    gbs_device_status_t charging={.valid=true,.battery_present_known=true,.battery_present=true,.battery_percent_known=true,.battery_percent=50,.charging_known=true,.charging=true};
    gbs_ui_set_device(&charging);lv_obj_t *charge=find_text(lv_scr_act(),LV_SYMBOL_CHARGE);assert(charge&&!lv_obj_has_flag(charge,LV_OBJ_FLAG_HIDDEN));
    charging.charging_known=false;gbs_ui_set_device(&charging);charge=find_text(lv_scr_act(),LV_SYMBOL_CHARGE);assert(charge&&lv_obj_has_flag(charge,LV_OBJ_FLAG_HIDDEN));
    gbs_ui_show(GBS_PAGE_ORIENTATION); click("自动旋转"); assert(gbs_ui_rotation_auto());
    gbs_device_status_t rotated={.valid=true,.orientation=GBS_ORIENTATION_USB_TOP}; gbs_ui_set_device(&rotated);
    click("自动旋转"); assert(!gbs_ui_rotation_auto()&&find_text(lv_scr_act(),"USB-C 在上侧"));
    if(directory) save_screen(directory,"amoled-orientation");
    gbs_ui_show(GBS_PAGE_SOUND); click("成功与失败提示音"); assert(gbs_ui_sound_enabled());
    assert(gbs_ui_take_tone()==GBS_TONE_SUCCESS&&gbs_ui_take_tone()==GBS_TONE_NONE);
    if(directory) save_screen(directory,"amoled-sound");
    gbs_ui_tick(13000);
    gbs_ui_show(GBS_PAGE_HOME); scene(1); unsigned count=requests;
    machine.scale_connected=false; machine.scale_stale=true; gbs_ui_set_machine(&machine);
    click("35g"); click("40g"); assert(requests==count+1&&requested==35&&machine.weight==30);
    assert(find_text(lv_scr_act(),"设定中")&&!find_text(lv_scr_act(),"已设定"));
    assert(lv_obj_get_style_text_color(find_text(lv_scr_act(),"设定中"),0).full==lv_color_hex(0xf0c178).full);
    assert_amoled_preset(1);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_control_result(command_id+1,GBS_UI_CONTROL_CONFIRMED);
    assert(find_text(lv_scr_act(),"设定中")&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED); pending=false;
    gbs_ui_set_machine(&machine); gbs_ui_tick(13001); // Refresh must not consume the queued tone.
    assert(find_text(lv_scr_act(),"已设定")&&!find_text(lv_scr_act(),"待核对")&&gbs_ui_take_tone()==GBS_TONE_SUCCESS);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    uint32_t completed_id=command_id;
    gbs_ui_control_result(completed_id,GBS_UI_CONTROL_CONFIRMED);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    assert(lv_obj_get_style_text_color(find_text(lv_scr_act(),"已设定"),0).full==lv_color_hex(0x9fd2aa).full);
    assert_amoled_preset(2);
    if(directory) save_screen(directory,"amoled-manual-accepted");
    gbs_ui_show(GBS_PAGE_LINK); paint();
    assert(find_text(lv_scr_act(),"已设定")&&find_text(lv_scr_act(),"设定已完成"));
    assert(lv_obj_get_style_text_color(find_text(lv_scr_act(),"已设定"),0).full==lv_color_hex(0x9fd2aa).full);
    if(directory) save_screen(directory,"amoled-manual-detail");
    gbs_ui_show(GBS_PAGE_HOME);
    click("40g");
    gbs_ui_control_result(completed_id,GBS_UI_CONTROL_CONFIRMED);
    assert(find_text(lv_scr_act(),"设定中")&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_FAILED); pending=false;
    assert(find_text(lv_scr_act(),"失败")&&!find_text(lv_scr_act(),"已设定")&&gbs_ui_take_tone()==GBS_TONE_FAILURE);
    assert(lv_obj_get_style_text_color(find_text(lv_scr_act(),"失败"),0).full==lv_color_hex(0xeaa28d).full);
    assert_amoled_preset(1);
    if(directory) save_screen(directory,"amoled-manual-failed");
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_FAILED); assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    click("20g"); gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;
    assert(find_text(lv_scr_act(),"未确认")&&!find_text(lv_scr_act(),"失败")&&!find_text(lv_scr_act(),"已设定")&&gbs_ui_take_tone()==GBS_TONE_NOTICE);
    assert(lv_obj_get_style_text_color(find_text(lv_scr_act(),"未确认"),0).full==lv_color_hex(0xeaa28d).full);
    assert_amoled_preset(1);
    if(directory) save_screen(directory,"amoled-manual-unknown");
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);
    assert(find_text(lv_scr_act(),"未确认")&&gbs_ui_take_tone()==GBS_TONE_NONE);
    click("35g"); gbs_ui_control_result(command_id,GBS_UI_CONTROL_PARTIAL); pending=false;
    assert(gbs_ui_take_tone()==GBS_TONE_NOTICE);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_set_device_preferences(false,false);
    click("20g"); gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED); pending=false;
    assert(find_text(lv_scr_act(),"已设定")&&gbs_ui_take_tone()==GBS_TONE_NONE);
    gbs_ui_init(submit,NULL); gbs_ui_set_device_preferences(false,true); scene(6);
    assert(gbs_ui_take_tone()==GBS_TONE_NONE); // A startup snapshot is not a new event.
    ++bridge.successes; gbs_ui_set_bridge(&bridge); assert(gbs_ui_take_tone()==GBS_TONE_SUCCESS);
    gbs_ui_set_bridge(&bridge); assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    scene(2); bridge.job=false; strcpy(bridge.outcome,"job_disappeared"); gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_NOTICE);
    scene(2); bridge.job=false; strcpy(bridge.outcome,"delivery_refused"); gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_FAILURE);
    gbs_ui_set_bridge(&bridge); assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    scene(2); bridge.invalid_coffee=true; gbs_ui_set_bridge(&bridge);
    assert(gbs_ui_take_tone()==GBS_TONE_FAILURE);
    gbs_ui_set_bridge(&bridge); assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    scene(2); machine.weight=20;gbs_ui_set_machine(&machine);
    ++bridge.job_deadline_ms; bridge.recipe_enabled=true; bridge.recipe_grams=30; strcpy(bridge.recipe_phase,"applied");
    gbs_ui_set_bridge(&bridge); gbs_ui_show(GBS_PAGE_HOME); paint();
    assert(find_text(lv_scr_act(),"已同步")&&!find_text(lv_scr_act(),"待核对")&&gbs_ui_take_tone()==GBS_TONE_NONE);
    assert(lv_obj_get_style_text_color(find_text(lv_scr_act(),"已同步"),0).full==lv_color_hex(0x9fd2aa).full);
    assert_amoled_preset(1);
    if(directory) save_screen(directory,"amoled-recipe-accepted");
    gbs_ui_show(GBS_PAGE_LINK); paint();
    assert(find_text(lv_scr_act(),"已同步")&&find_text(lv_scr_act(),"配方设定已完成"));
    assert(lv_obj_get_style_text_color(find_text(lv_scr_act(),"已同步"),0).full==lv_color_hex(0x9fd2aa).full);
    if(directory) save_screen(directory,"amoled-recipe-detail");
    gbs_ui_show(GBS_PAGE_HOME);
    strcpy(bridge.recipe_phase,"unconfirmed"); gbs_ui_set_bridge(&bridge); paint();
    assert(find_text(lv_scr_act(),"未确认")&&!find_text(lv_scr_act(),"已同步")&&gbs_ui_take_tone()==GBS_TONE_NONE);
    assert_amoled_preset(0);
    strcpy(bridge.recipe_phase,"applying"); gbs_ui_set_bridge(&bridge); paint();
    assert(find_text(lv_scr_act(),"同步中")); if(directory) save_screen(directory,"amoled-recipe-applying");
    strcpy(bridge.recipe_phase,"failed"); gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"失败")&&gbs_ui_take_tone()==GBS_TONE_FAILURE);
    gbs_ui_set_bridge(&bridge); assert(gbs_ui_take_tone()==GBS_TONE_NONE);
    if(directory) save_screen(directory,"amoled-recipe-failed");
    strcpy(bridge.recipe_phase,"applying"); gbs_ui_set_bridge(&bridge);
    click("40g"); assert(pending&&requested==40&&bridge.job);
    assert(find_text(lv_scr_act(),"手动")&&find_text(lv_scr_act(),"40")&&find_text(lv_scr_act(),"40g"));
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED); pending=false;
    gbs_ui_show(GBS_PAGE_COFFEE);
    assert(find_text(lv_scr_act(),"93")&&!find_text(lv_scr_act(),"40.0")&&!find_text(lv_scr_act(),"已设定"));
    gbs_ui_show(GBS_PAGE_HOME); ++bridge.job_deadline_ms; gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"配方")&&find_text(lv_scr_act(),"30")&&!find_text(lv_scr_act(),"手动"));
    gbs_ui_init(submit,NULL); scene(4); machine.stale=true; gbs_ui_set_machine(&machine);
    assert(find_text(lv_scr_act(),"未知")&&!find_text(lv_scr_act(),"已过期"));
    if(directory) save_screen(directory,"amoled-unknown-stale-machine");
    gbs_ui_init(submit,NULL); scene(2); bridge.recipe_enabled=true; bridge.recipe_grams=30; strcpy(bridge.recipe_phase,"applied");
    gbs_ui_set_bridge(&bridge); gbs_ui_tick(bridge.job_deadline_ms);
    assert(find_text(lv_scr_act(),"收尾")&&find_text(lv_scr_act(),"00:00")&&find_text(lv_scr_act(),"—")&&!find_text(lv_scr_act(),"待核对"));
    machine.stale=true; gbs_ui_set_machine(&machine); assert(find_text(lv_scr_act(),"收尾"));
    if(directory) save_screen(directory,"amoled-wait-expired");
    bridge.pending=bridge.canceling=true; bridge.answer_deadline_ms=bridge.job_deadline_ms+20000; gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"收尾")&&find_text(lv_scr_act(),"00:00"));
    gbs_ui_init(submit,NULL); scene(1); machine.stale=true; gbs_ui_set_machine(&machine);
    count=requests; click("35g"); assert(requests==count); // Handler also rejects a stale source.
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_WEIGHT); tap(350,340);
    assert(find_text(lv_scr_act(),"应用 31g")); tap(240,432); assert(requested==31&&pending);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED); pending=false;
    assert(find_text(lv_scr_act(),"已设定")&&!find_text(lv_scr_act(),"请求已受理 · 待核对"));
    if(directory) save_screen(directory,"amoled-weight");
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_WEIGHT); tap(350,340);
    assert(find_text(lv_scr_act(),"应用 31g")); count=requests;
    gbs_ui_home_safe(); assert(find_text(lv_scr_act(),"放弃这次修改？")&&requests==count);
    click("继续编辑"); assert(gbs_ui_page()==GBS_PAGE_WEIGHT&&find_text(lv_scr_act(),"应用 31g"));
    gbs_ui_toggle_rotation(); assert(gbs_ui_rotation_auto()&&gbs_ui_page()==GBS_PAGE_WEIGHT&&requests==count);
    assert(find_text(lv_scr_act(),"应用 31g")&&find_text(lv_scr_act(),"自动旋转已开启"));
    reject_submit=true; gbs_ui_toggle_rotation(); reject_submit=false;
    assert(gbs_ui_rotation_auto()&&find_text(lv_scr_act(),"保存失败，请重试"));
    gbs_ui_tick(13000); assert(!find_text(lv_scr_act(),"保存失败，请重试"));
    gbs_ui_home_safe(); click("放弃修改"); assert(gbs_ui_page()==GBS_PAGE_HOME&&requests==count&&machine.weight==30);
    gbs_ui_home_safe(); assert(gbs_ui_page()==GBS_PAGE_HOME);
    click("35g"); assert(pending); gbs_ui_show(GBS_PAGE_LINK); gbs_ui_home_safe();
    assert(gbs_ui_page()==GBS_PAGE_HOME&&pending&&gbs_ui_keep_awake());
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_BBW);
    tap(220,236); assert(gbs_ui_page()==GBS_PAGE_DOSE2); tap(350,340);
    assert(find_text(lv_scr_act(),"应用 41g")); tap(240,432);
    assert(pending&&command==GBS_ACTION_DOSE2&&requested==41);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_STEAM);
    tap(420,48); assert(pending&&command==GBS_ACTION_STEAM_ENABLED&&requested==0);
    gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN); pending=false;
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_show(GBS_PAGE_SETTINGS);
    click("显示与声音");assert(gbs_ui_page()==GBS_PAGE_DISPLAY);
    click("短提示音");assert(gbs_ui_sound_enabled());gbs_ui_take_tone();
    click("自动调暗 / 熄屏");assert(gbs_ui_page()==GBS_PAGE_DISPLAY);
    click("屏幕亮度");assert(gbs_ui_page()==GBS_PAGE_BRIGHTNESS);
    lv_obj_t *brightness_slider=NULL;
    for(unsigned i=0;i<lv_obj_get_child_cnt(lv_scr_act());++i) {
        lv_obj_t *child=lv_obj_get_child(lv_scr_act(),i);
        if(lv_obj_check_type(child,&lv_slider_class)) brightness_slider=child;
    }
    assert(brightness_slider&&lv_obj_get_style_bg_opa(brightness_slider,LV_PART_MAIN)==LV_OPA_COVER&&
           lv_obj_get_style_bg_opa(brightness_slider,LV_PART_INDICATOR)==LV_OPA_COVER&&
           lv_obj_get_style_bg_opa(brightness_slider,LV_PART_KNOB)==LV_OPA_COVER);
    count=requests;tap(320,270);assert(requests==count&&lv_slider_get_value(brightness_slider)>60);
    if(directory)save_screen(directory,"amoled-brightness");
    gbs_ui_init(submit,NULL);scene(1);gbs_ui_show(GBS_PAGE_PREBREW_ON);
    assert(find_text(lv_scr_act(),"上次设定 2.5 s")&&!find_text(lv_scr_act(),"设置出杯量"));
    gbs_ui_show(GBS_PAGE_PREBREW_OFF);assert(find_text(lv_scr_act(),"上次设定 3.5 s"));
    gbs_ui_init(submit,NULL); scene(1); gbs_ui_set_reader(read_data); gbs_ui_set_schedule_submit(submit_schedule);
    for(unsigned p=0;p<GBS_PAGE_COUNT;++p) {
        gbs_ui_show((gbs_ui_page_t)p); paint();
        if(directory) {char name[40]; snprintf(name,sizeof(name),"amoled-page-%02u",p); save_screen(directory,name);}
    }
    gbs_ui_init(submit,NULL);scene(1);bridge.valid=false;gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"准备中")&&!find_text(lv_scr_act(),"就绪"));
    bridge.valid=true;bridge.baseline=true;gbs_ui_set_bridge(&bridge);assert(find_text(lv_scr_act(),"建立新任务基线"));
    strcpy(bridge.phase,"clock_waiting");gbs_ui_set_bridge(&bridge);assert(find_text(lv_scr_act(),"校时中"));
    scene(7);bridge.unconfirmed_grams=30.2;bridge.unconfirmed_seconds=27.7;gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"30.2")&&find_text(lv_scr_act(),"27.7s"));
    bridge.unconfirmed_grams=5000;gbs_ui_set_bridge(&bridge);paint();lv_obj_t *large=find_text(lv_scr_act(),"5000.0");assert(large);
    const lv_font_t *small_font=lv_obj_get_style_text_font(large,0);lv_area_t large_area;lv_obj_get_coords(large,&large_area);
    assert(large_area.x2<=310&&lv_obj_get_height(large)<=small_font->line_height);if(directory)save_screen(directory,"measurement-5000");
    bridge.unconfirmed_grams=30.2;gbs_ui_set_bridge(&bridge);paint();assert(lv_obj_get_style_text_font(find_text(lv_scr_act(),"30.2"),0)->line_height>small_font->line_height);
    if(directory)save_screen(directory,"measurement-return-30.2");
    gbs_ui_show(GBS_PAGE_RECORD);assert(find_text(lv_scr_act(),"30.2")&&find_text(lv_scr_act(),"27.7"));
    bridge.job=bridge.pending=false;bridge.successes=1;bridge.result_grams=30.2;bridge.result_seconds=27.7;strcpy(bridge.outcome,"success");gbs_ui_set_bridge(&bridge);
    assert(find_text(lv_scr_act(),"已接收")&&find_text(lv_scr_act(),"30.2"));
    amoled_cup_tone_test();
    amoled_boiler_warmup_test(directory);
    amoled_scale_history_test(directory);
    amoled_refresh_test();
    puts("AMOLED UI: 480 px pages, merged scale, separate battery, local preferences, stale guards, completed command feedback, event tones and glyph coverage passed");
}
#endif

#ifdef HUB_AMOLED_216
static void visual_review(const char *directory)
{
    gbs_ui_init(submit,NULL); scene(1); pending=false;
    machine.weight=machine.dose1=20; machine.coffee=92.8; machine.coffee_target=93;
    machine.steam=127; machine.steam_target=128; machine.scale_battery_percent=78;
    gbs_ui_set_machine(&machine); gbs_ui_show(GBS_PAGE_HOME); save_screen(directory,"ready");
    bridge.job=true; bridge.job_deadline_ms=173000; bridge.recipe_enabled=true; bridge.recipe_grams=30;
    strcpy(bridge.recipe_phase,"applying"); gbs_ui_set_bridge(&bridge); save_screen(directory,"setting");
    gbs_ui_show(GBS_PAGE_LINK);save_screen(directory,"link-setting");strcpy(bridge.recipe_phase,"applied");gbs_ui_set_bridge(&bridge);save_screen(directory,"link-accepted");
    gbs_ui_show(GBS_PAGE_HOME);save_screen(directory,"accepted");
    gbs_ui_show(GBS_PAGE_WEIGHT);for(int i=0;i<12;++i)tap(350,325);save_screen(directory,"quick");
    gbs_ui_show(GBS_PAGE_COFFEE);save_screen(directory,"coffee");
    gbs_ui_show(GBS_PAGE_STEAM);save_screen(directory,"steam");
    const gbs_ui_page_t pages[]={GBS_PAGE_SETTINGS,GBS_PAGE_DISPLAY,GBS_PAGE_ORIENTATION,GBS_PAGE_SCALE,GBS_PAGE_DEVICE_BATTERY,GBS_PAGE_CONNECTIONS};
    const char *names[]={"settings","display","orientation","scale","device-battery","connections"};
    for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);++i){gbs_ui_show(pages[i]);save_screen(directory,names[i]);}
    gbs_ui_show(GBS_PAGE_RECORD);save_screen(directory,"record-empty");
    bridge.pending=true;bridge.unconfirmed_grams=30.2;bridge.unconfirmed_seconds=27.7;bridge.answer_deadline_ms=100000;gbs_ui_set_bridge(&bridge);save_screen(directory,"record-pending");
    gbs_ui_show(GBS_PAGE_HOME);save_screen(directory,"sending");
    bridge.job=bridge.pending=false;bridge.successes=1;bridge.result_grams=30.2;bridge.result_seconds=27.7;strcpy(bridge.outcome,"success");gbs_ui_set_bridge(&bridge);save_screen(directory,"received");
    gbs_ui_show(GBS_PAGE_RECORD);save_screen(directory,"record-received");
    gbs_ui_set_reader(read_data);gbs_ui_set_schedule_submit(submit_schedule);
    gbs_ui_show(GBS_PAGE_COFFEES);finish_data(GBS_DATA_HISTORY);save_screen(directory,"history");tap(200,154);save_screen(directory,"history-detail");
    gbs_ui_show(GBS_PAGE_SCHEDULE);finish_data(GBS_DATA_SCHEDULE);save_screen(directory,"schedule");tap(200,154);save_screen(directory,"schedule-detail");
    tap(240,424);save_screen(directory,"schedule-edit");
    gbs_ui_show(GBS_PAGE_SCHEDULE_ON);save_screen(directory,"schedule-on");
    gbs_ui_show(GBS_PAGE_SCHEDULE_OFF);save_screen(directory,"schedule-off");
    gbs_ui_show(GBS_PAGE_SCHEDULE_DAYS);save_screen(directory,"schedule-days");
    const unsigned percentages[]={0,1,20,50,84,100};
    gbs_device_status_t device={.valid=true,.battery_present_known=true,.battery_present=true,.battery_percent_known=true,.charging_known=true};
    for(unsigned i=0;i<6;++i){char name[48];device.battery_percent=percentages[i];gbs_ui_set_device(&device);gbs_ui_show(GBS_PAGE_HOME);snprintf(name,sizeof(name),"battery-%u",percentages[i]);save_screen(directory,name);}
    device.battery_percent_known=false;gbs_ui_set_device(&device);save_screen(directory,"battery-unknown");
    device.battery_present=false;gbs_ui_set_device(&device);save_screen(directory,"battery-absent");
    device.battery_present=device.battery_percent_known=device.charging=true;device.battery_percent=50;gbs_ui_set_device(&device);save_screen(directory,"battery-charging");gbs_ui_show(GBS_PAGE_DEVICE_BATTERY);save_screen(directory,"battery-charging-detail");
    machine.scale_battery_percent=1;gbs_ui_set_machine(&machine);gbs_ui_show(GBS_PAGE_SCALE);save_screen(directory,"scale-1");machine.scale_battery_percent=100;gbs_ui_set_machine(&machine);save_screen(directory,"scale-100");
    machine.scale_stale=true;gbs_ui_set_machine(&machine);gbs_ui_tick(80000);save_screen(directory,"scale-stale");
    gbs_ui_init(submit,NULL);scene(1);bridge.valid=false;gbs_ui_set_bridge(&bridge);save_screen(directory,"coldboot");
    bridge.valid=true;bridge.baseline=true;gbs_ui_set_bridge(&bridge);save_screen(directory,"baseline");strcpy(bridge.phase,"clock_waiting");gbs_ui_set_bridge(&bridge);save_screen(directory,"clock");
    scene(4);save_screen(directory,"unknown-ready");gbs_ui_show(GBS_PAGE_LINK);save_screen(directory,"unknown-ready-detail");
    scene(1);strcpy(bridge.outcome,"interrupted_by_restart");gbs_ui_set_bridge(&bridge);gbs_ui_show(GBS_PAGE_HOME);save_screen(directory,"interrupted-ready");
    gbs_ui_init(submit,NULL);scene(1);pending=false;
    machine.weight=machine.dose1=20;machine.coffee=92.8;machine.steam=127;gbs_ui_set_machine(&machine);
    bridge.job=true;bridge.job_deadline_ms=173000;bridge.recipe_enabled=true;bridge.recipe_grams=30;
    strcpy(bridge.recipe_phase,"reading");gbs_ui_set_bridge(&bridge);save_screen(directory,"reading");
    strcpy(bridge.recipe_phase,"failed");gbs_ui_set_bridge(&bridge);save_screen(directory,"failed");
    bridge.recipe_enabled=false;gbs_ui_set_bridge(&bridge);save_screen(directory,"waiting");
    gbs_ui_tick(173000);save_screen(directory,"timeout");
    scene(5);save_screen(directory,"offline");
    scene(1);machine.stale=true;gbs_ui_set_machine(&machine);save_screen(directory,"stale");
    scene(1);machine.scale_connected=false;gbs_ui_set_machine(&machine);save_screen(directory,"scale-off-home");
    scene(1);click("35g");save_screen(directory,"manual");pending=false;
    gbs_ui_init(submit,NULL);scene(1);gbs_ui_set_device_preferences(true,false);gbs_ui_show(GBS_PAGE_ORIENTATION);save_screen(directory,"orientation-auto");
}
#endif

int main(int argc,char **argv)
{
    bool review=argc>2&&!strcmp(argv[1],"--review");
    bool test=review||(argc>1 && !strcmp(argv[1],"--self-test"));
    self_testing=test;
    SDL_SetMainReady(); assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)==0);
    SDL_Window *window=SDL_CreateWindow("GbS Touch - SYNTHETIC DATA - 0: unavailable, 1-8: scenarios",SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,WIDTH*SCALE,HEIGHT*SCALE,test?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN);
    assert(window); renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE); assert(renderer);
    texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,WIDTH,HEIGHT); assert(texture);
    lv_init();
    static lv_color_t buffer[WIDTH*24]; static lv_disp_draw_buf_t draw; static lv_disp_drv_t display;
    lv_disp_draw_buf_init(&draw,buffer,NULL,WIDTH*24); lv_disp_drv_init(&display);
    display.hor_res=WIDTH; display.ver_res=HEIGHT; display.flush_cb=flush; display.draw_buf=&draw; lv_disp_drv_register(&display);
    lv_indev_drv_init(&input); input.type=LV_INDEV_TYPE_POINTER; input.read_cb=read_pointer; pointer_device=lv_indev_drv_register(&input);
    gbs_ui_init(submit,NULL); scene(1);
    gbs_ui_set_reader(read_data); gbs_ui_set_schedule_submit(submit_schedule);
    if(test) {
#ifdef HUB_AMOLED_216
        if(review) visual_review(argv[2]); else if(WIDTH==480) amoled_self_test(argc>2?argv[2]:NULL); else legacy_self_test(NULL);
#else
        legacy_self_test(argc>2?argv[2]:NULL);
#endif
        SDL_Quit(); return 0;
    }
#ifdef HUB_AMOLED_216
    gbs_ui_set_scale_refresh(scale_read);
#endif
    bool native_pointer=false;
#ifdef __APPLE__
    bool gbs_cocoa_input(SDL_Window *,void (*)(int,int,bool));
    native_pointer=gbs_cocoa_input(window,window_pointer);
#endif
    uint32_t last=SDL_GetTicks(),scene_start=last; bool running=true;
    while(running) {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT) running=false;
            if(!native_pointer) mouse_event(&e);
            if(e.type==SDL_WINDOWEVENT&&e.window.event==SDL_WINDOWEVENT_FOCUS_LOST) {
                lv_indev_reset(pointer_device,NULL); pressed=false;
            }
            if(e.type==SDL_KEYDOWN) {
                if(e.key.keysym.sym==SDLK_ESCAPE) gbs_ui_show(GBS_PAGE_HOME);
                if(e.key.keysym.sym>=SDLK_0&&e.key.keysym.sym<=SDLK_8) { scene(e.key.keysym.sym-SDLK_0); scene_start=SDL_GetTicks(); }
            }
        }
        uint32_t now=SDL_GetTicks(); lv_tick_inc(now-last); last=now;
        if(pending&&now-submitted_at>1500) {
            pending=false;
            if(scenario==4) gbs_ui_control_result(command_id,GBS_UI_CONTROL_UNKNOWN);
            else if(schedule_pending) finish_schedule();
            else finish_requested();
        }
        for(unsigned i=0;i<GBS_DATA_COUNT;++i)
            if(data_pending[i]&&now-data_started[i]>700) finish_data(i);
#ifdef HUB_AMOLED_216
        if(scale_pending&&now-scale_started>=700) {
            scale_pending=false; machine.scale_stale=false; machine.scale_observed_ms=10000+now-scene_start;
            gbs_ui_set_machine(&machine);
        }
#endif
        gbs_ui_tick(10000+now-scene_start); lv_timer_handler(); SDL_Delay(10);
    }
#ifdef __APPLE__
    void gbs_cocoa_input_stop(void);
    gbs_cocoa_input_stop();
#endif
    SDL_Quit(); return 0;
}
