#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "gbs_ui.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 240, HEIGHT = 280, SCALE = 3 };
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t pixels[WIDTH * HEIGHT];
static bool pressed;
static lv_point_t pointer;
static lv_indev_drv_t input;
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

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
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
    pointer.x=x; pointer.y=y; pressed=down;
    lv_obj_update_layout(lv_scr_act());
    // SDL can deliver down and up in one poll; preserve both for LVGL.
    lv_indev_read_timer_cb(input.read_timer);
}

static bool submit(gbs_ui_action_t action, double value, uint32_t request_id, void *context)
{
    (void)context;
    if (reject_submit) return false;
    if (action == GBS_ACTION_BRIGHTNESS || action == GBS_ACTION_DIM) return true;
    ++requests; command = action; requested = value; command_id=request_id; pending = true; submitted_at = SDL_GetTicks();
    return true;
}

static void scene(unsigned n)
{
    scenario = n;
    bridge = (gbs_ui_bridge_t){.valid=true, .wifi=true, .cloud=true, .grinder=true, .storage=true,
        .active=true, .free_internal=80000, .largest_internal=32000};
    strcpy(bridge.phase, "ready");
    machine = (gbs_ui_machine_t){.coffee=93, .coffee_target=93, .steam=128, .steam_target=128,
        .weight=30, .dose1=30, .dose2=40, .dose_mode=1, .available=true, .writable=GBS_UI_MACHINE_ACTIONS,
        .steam_enabled=true, .steam_enabled_known=true, .revision=++sequence};
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
    lv_obj_t *l = find_text(lv_scr_act(), text); assert(l);
    while (l && !lv_obj_has_flag(l, LV_OBJ_FLAG_CLICKABLE)) l = lv_obj_get_parent(l);
    assert(l); lv_event_send(l, LV_EVENT_CLICKED, NULL);
}

static void tap(int x, int y)
{
    pointer_event(x,y,true); pointer_event(x,y,false);
    lv_tick_inc(100); lv_timer_handler();
}

static void paint(void)
{
    lv_obj_update_layout(lv_scr_act()); lv_refr_now(NULL);
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

static void self_test(const char *directory)
{
    scene(1);
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
    machine.weight=40; ++machine.revision; gbs_ui_set_machine(&machine);
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
        machine.weight=32; ++machine.revision; gbs_ui_set_machine(&machine); gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);
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
    puts("native UI: navigation, confirmation timeout, stale/rejected steam controls, link colors and all pages passed");
}

int main(int argc,char **argv)
{
    bool test=argc>1 && !strcmp(argv[1],"--self-test");
    SDL_SetMainReady(); assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)==0);
    SDL_Window *window=SDL_CreateWindow("GbS Touch - SYNTHETIC DATA - 0: unavailable, 1-8: scenarios",SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,WIDTH*SCALE,HEIGHT*SCALE,test?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN);
    assert(window); renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE); assert(renderer);
    texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,WIDTH,HEIGHT); assert(texture);
    lv_init();
    static lv_color_t buffer[WIDTH*24]; static lv_disp_draw_buf_t draw; static lv_disp_drv_t display;
    lv_disp_draw_buf_init(&draw,buffer,NULL,WIDTH*24); lv_disp_drv_init(&display);
    display.hor_res=WIDTH; display.ver_res=HEIGHT; display.flush_cb=flush; display.draw_buf=&draw; lv_disp_drv_register(&display);
    lv_indev_drv_init(&input); input.type=LV_INDEV_TYPE_POINTER; input.read_cb=read_pointer; lv_indev_drv_register(&input);
    gbs_ui_init(submit,NULL); scene(1);
    if(test) { self_test(argc>2?argv[2]:NULL); SDL_Quit(); return 0; }
    uint32_t last=SDL_GetTicks(),scene_start=last; bool running=true;
    while(running) {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT) running=false;
            if(e.type==SDL_MOUSEMOTION) { pointer.x=e.motion.x/SCALE; pointer.y=e.motion.y/SCALE; }
            if(e.type==SDL_MOUSEBUTTONDOWN||e.type==SDL_MOUSEBUTTONUP) {
                pointer_event(e.button.x/SCALE,e.button.y/SCALE,e.type==SDL_MOUSEBUTTONDOWN);
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
            else {
                if(command==GBS_ACTION_WEIGHT) machine.weight=requested;
                if(command==GBS_ACTION_COFFEE) machine.coffee_target=requested;
                if(command==GBS_ACTION_STEAM) machine.steam_target=requested;
                if(command==GBS_ACTION_STEAM_ENABLED) machine.steam_enabled=requested!=0;
                if(command==GBS_ACTION_MODE) bridge.active=requested!=0;
                ++machine.revision; gbs_ui_set_machine(&machine); gbs_ui_set_bridge(&bridge);
                gbs_ui_control_result(command_id,GBS_UI_CONTROL_CONFIRMED);
            }
        }
        gbs_ui_tick(10000+now-scene_start); lv_timer_handler(); SDL_Delay(10);
    }
    SDL_Quit(); return 0;
}
