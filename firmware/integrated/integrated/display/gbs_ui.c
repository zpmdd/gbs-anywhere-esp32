#include "gbs_ui.h"
#include "icons/gbs_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(gbs_font_14);
LV_FONT_DECLARE(gbs_font_12);
LV_FONT_DECLARE(gbs_font_10);
LV_FONT_DECLARE(gbs_font_12_bold);
LV_FONT_DECLARE(gbs_font_14_bold);
LV_FONT_DECLARE(gbs_font_16_bold);
LV_FONT_DECLARE(gbs_font_20_bold);
LV_FONT_DECLARE(gbs_number_16);
LV_FONT_DECLARE(gbs_number_20);
LV_FONT_DECLARE(gbs_number_24);
LV_FONT_DECLARE(gbs_number_28);
LV_FONT_DECLARE(gbs_number_32);
LV_FONT_DECLARE(gbs_number_34);
LV_FONT_DECLARE(gbs_number_36);
LV_FONT_DECLARE(gbs_number_40);
#ifdef HUB_AMOLED_216
LV_FONT_DECLARE(gbs_font_18);
LV_FONT_DECLARE(gbs_font_20);
LV_FONT_DECLARE(gbs_font_24);
LV_FONT_DECLARE(gbs_font_24_bold);
LV_FONT_DECLARE(gbs_font_28_bold);
LV_FONT_DECLARE(gbs_font_34_bold);
LV_FONT_DECLARE(gbs_number_48);
LV_FONT_DECLARE(gbs_number_56);
LV_FONT_DECLARE(gbs_number_60);
LV_FONT_DECLARE(gbs_number_64);
LV_FONT_DECLARE(gbs_number_68);
LV_FONT_DECLARE(gbs_number_108);
LV_FONT_DECLARE(gbs_number_18);
LV_FONT_DECLARE(gbs_battery_18);
LV_FONT_DECLARE(gbs_number_27);
LV_FONT_DECLARE(gbs_number_30);
LV_FONT_DECLARE(gbs_number_52);
LV_FONT_DECLARE(gbs_number_55);
LV_FONT_DECLARE(gbs_number_70);
LV_FONT_DECLARE(gbs_number_76);
LV_FONT_DECLARE(gbs_font_19);
LV_FONT_DECLARE(gbs_font_23);
LV_FONT_DECLARE(gbs_font_25_bold);
LV_FONT_DECLARE(gbs_font_32);
LV_FONT_DECLARE(gbs_unit_24);
LV_FONT_DECLARE(gbs_unit_27);
LV_FONT_DECLARE(gbs_unit_36);
LV_FONT_DECLARE(gbs_font_15);
LV_FONT_DECLARE(gbs_font_16);
LV_FONT_DECLARE(gbs_font_17);
LV_FONT_DECLARE(gbs_font_16_semibold);
LV_FONT_DECLARE(gbs_font_23_semibold);
LV_FONT_DECLARE(gbs_font_28_semibold);
LV_FONT_DECLARE(gbs_font_30_semibold);
LV_FONT_DECLARE(gbs_number_29);
LV_FONT_DECLARE(gbs_number_31);
LV_FONT_DECLARE(gbs_number_45);
LV_FONT_DECLARE(gbs_number_72);
LV_FONT_DECLARE(gbs_battery_13);
LV_FONT_DECLARE(gbs_amoled_number_20);
LV_FONT_DECLARE(gbs_amoled_number_24);
#endif
// RGB565-friendly values preserve the neutral charcoal and warm ivory on the LCD.
#ifdef HUB_AMOLED_216
enum { DEFAULT_BRIGHTNESS = 60, DEFAULT_DIM = false };
enum { BG = 0x101410, SURFACE = 0x202820, LINE = 0x30382e, TEXT = 0xf4f0e4, MUTED = 0xc0c8bd,
       GOLD = 0xe3bb83, GREEN = 0xa6edc4, WARN = 0xffc179, RED = 0xe99d8b };
#else
enum { DEFAULT_BRIGHTNESS = 75, DEFAULT_DIM = true };
enum { BG = 0x101610, SURFACE = 0x202420, LINE = 0x343734, TEXT = 0xf4f0e4, MUTED = 0xbabdb6,
       GOLD = 0xe3bb83, GREEN = 0x9fd2aa, WARN = 0xf0c178, RED = 0xeaa28d };
#endif
enum { BACK = 100, MINUS, PLUS, APPLY, CONFIRM, DISCARD, KEEP,
       MODE_MONITOR, MODE_ACTIVE, BRIGHTNESS, DIM, DISPLAY_SAVE, STEAM_TOGGLE,
       ROTATION_TOGGLE, SOUND_VOLUME, SOUND_SAVE, SCALE_REFRESH, DIM_SAVE,
       DATA_REFRESH, DATA_PREVIOUS, DATA_NEXT, PLAN_ENABLED, PLAN_STEAM, PLAN_APPLY, PLAN_SEND,
       HOUR_MINUS, HOUR_PLUS, MINUTE_MINUS, MINUTE_PLUS, PRESET = 200, CHOICE = 400, DATA_ROW = 500, PLAN_DAY = 600 };
enum { F_COFFEE, F_COFFEE_TARGET, F_STEAM, F_STEAM_CURRENT, F_STEAM_TARGET, F_WEIGHT, F_STAGE,
       F_DETAIL, F_TIME, F_TIMER_CAPTION, F_TARGET, F_RESULT, F_CONTROL,
       F_DRAFT, F_CONFLICT, F_WIFI, F_GRINDER, F_CLOUD, F_MEMORY, F_MODE,
       F_SECOND_RESULT, F_HOME_HINT, F_HUB, F_BRIGHTNESS, F_SOUND, F_PARAMETER0, F_PARAMETER1, F_PARAMETER2,
       F_GLANCE_VALUE, F_GLANCE_UNIT, F_GLANCE_STAGE, F_GLANCE_CAPTION, F_GLANCE_PATH, F_SCALE, F_BATTERY, F_DEVICE_NOTE, F_PATH_OLD, F_PATH_NEW, F_PATH_HINT, F_BATTERY_DARK, F_BATTERY_LARGE, F_EDITOR_UNIT, F_EDITOR_CURRENT, F_COUNT };
static struct {
    gbs_ui_bridge_t bridge;
#ifdef HUB_AMOLED_216
    gbs_ui_flow_t flow;
    char saved_recipe_job_id[64];
#endif
    gbs_ui_machine_t machine, edit_snapshot;
    gbs_device_status_t device;
    bool auto_rotate, native_layout, manual_weight, home_requested, navigation_requested;
    unsigned sound_volume, sound_draft;
    unsigned variable_weight;
    char preference_notice[80];
    int64_t preference_notice_until;
    lv_obj_t *preference_toast;
    gbs_tone_t tones[4];
    unsigned tone_count, cup_tone_nodes;
    char tone_job_id[64];
    lv_obj_t *scale_shape, *battery_shape, *battery_fill, *battery_clip, *battery_terminal, *battery_charging, *phase_icon, *grinder_icon, *grinder_dot, *cloud_icon;
    lv_obj_t *path_icon, *path_arrow, *path_rule;
    lv_obj_t *preset_units[4];
    double previous_weight;
    gbs_ui_page_t page, navigation_page, stack[12];
    unsigned depth, brightness;
    bool dim, dirty, discard, confirm, conflict;
    double draft, initial, requested;
    uint32_t edit_revision, request_id;
    gbs_ui_control_t control;
    gbs_ui_action_t action;
    gbs_ui_submit_t submit;
    gbs_ui_read_t reader;
    bool (*scale_refresh)(void *);
    gbs_ui_data_t *data;
    gbs_ui_schedule_submit_t schedule_submit;
    bool schedule_submitted;
    unsigned data_page[GBS_DATA_COUNT];
    gbs_ui_schedule_t selected_schedule, schedule_draft;
    gbs_ui_coffee_t selected_coffee;
    bool selected_stale;
    void *context;
    int64_t now, submitted_ms, submitted_observation;
    lv_obj_t *fields[F_COUNT], *presets[4], *preset_faces[4], *apply, *body, *ring, *mode_radio[2];
    unsigned brightness_draft;
    bool mode_draft, dim_draft;
    lv_obj_t *connection_dots[4], *steam_toggle, *choices[4], *confirm_button;
} ui;
static uint32_t request_sequence;
static gbs_ui_data_t *collections;
#ifdef HUB_AMOLED_216
static const int presets[] = {20, 30, 36, 40};
#else
static const int presets[] = {20, 30, 35, 40};
#endif
static unsigned preset_value(unsigned index)
{
#ifdef HUB_AMOLED_216
    if(index==3) return ui.variable_weight;
#endif
    return presets[index];
}
static void render(void);
static void refresh(void);
static void queue_tone(gbs_tone_t tone)
{
    if(!ui.sound_volume) return;
    if(ui.tone_count==4) { memmove(ui.tones,ui.tones+1,3*sizeof(*ui.tones)); --ui.tone_count; }
    ui.tones[ui.tone_count++]=tone;
}
#ifdef HUB_AMOLED_216
static void amoled_refresh(void);
static lv_obj_t *fallback_heading, *fallback_back;
#endif
static void event(lv_event_t *event);
static void set(unsigned field_id, const char *text);
static void value(unsigned id,double number,const char *format);
static bool plan_editor(void) { return ui.page>=GBS_PAGE_SCHEDULE_EDIT&&ui.page<=GBS_PAGE_SCHEDULE_DAYS; }
static bool data_page(void) { return ui.page==GBS_PAGE_SCHEDULE||ui.page==GBS_PAGE_COFFEES; }
static gbs_ui_data_kind_t data_kind(void) { return ui.page==GBS_PAGE_SCHEDULE?GBS_DATA_SCHEDULE:GBS_DATA_HISTORY; }
static void request_data(void)
{
    gbs_ui_data_t *data=&ui.data[data_kind()];
    if(data->state==GBS_DATA_LOADING) return;
    data->state=ui.reader&&ui.reader(data_kind(),ui.context)?GBS_DATA_LOADING:GBS_DATA_UNAVAILABLE;
}

static const struct parameter {
    gbs_ui_page_t page;
    gbs_ui_action_t action;
    const char *title, *unit;
    double low, high, step;
} parameters[] = {
    {GBS_PAGE_WEIGHT,GBS_ACTION_WEIGHT,"出杯量","g",0,200,1},
    {GBS_PAGE_VARIABLE_WEIGHT,GBS_ACTION_WEIGHT,"可变液重","g",1,200,1},
    {GBS_PAGE_COFFEE,GBS_ACTION_COFFEE,"锅炉温度","°C",80,100,0.5},
    {GBS_PAGE_STEAM,GBS_ACTION_STEAM,"蒸汽","°C",126,131,1},
    {GBS_PAGE_DOSE1,GBS_ACTION_DOSE1,"剂量 1","g",0,200,1},
    {GBS_PAGE_DOSE2,GBS_ACTION_DOSE2,"剂量 2","g",0,200,1},
    {GBS_PAGE_BBW_MODE,GBS_ACTION_BBW_MODE,"BBW 模式","",0,3,1},
    {GBS_PAGE_PREBREW_MODE,GBS_ACTION_PREBREW_MODE,"预浸模式","",0,2,1},
    {GBS_PAGE_PREBREW_ON,GBS_ACTION_PREBREW_ON,"注水时间","s",0,10,0.1},
    {GBS_PAGE_PREBREW_OFF,GBS_ACTION_PREBREW_OFF,"停顿时间","s",0,10,0.1},
    {GBS_PAGE_POWER,GBS_ACTION_POWER,"机器电源","",0,1,1},
    {GBS_PAGE_STANDBY_ENABLED,GBS_ACTION_STANDBY_ENABLED,"自动待机开关","",0,1,1},
    {GBS_PAGE_STANDBY_MINUTES,GBS_ACTION_STANDBY_MINUTES,"待机时长","分钟",1,240,1},
    {GBS_PAGE_STANDBY_MODE,GBS_ACTION_STANDBY_MODE,"计时起点","",0,1,1}
};
static const struct parameter *parameter(void)
{
    for(unsigned i=0;i<sizeof(parameters)/sizeof(parameters[0]);++i)
        if(parameters[i].page==ui.page) return &parameters[i];
    return NULL;
}
static const char *choice_label(gbs_ui_action_t action,unsigned index)
{
    static const char *const bbw[]={"连续出杯","剂量 1","剂量 2","脉冲模式"};
    static const char *const prebrew[]={"关闭","预冲泡","预浸泡"};
    static const char *const power[]={"进入待机","开启加热","节能中"};
    static const char *const enabled[]={"关闭","开启"};
    static const char *const standby[]={"从开机计时","从上次萃取计时"};
    switch(action) {
    case GBS_ACTION_BBW_MODE: return index<4?bbw[index]:"—";
    case GBS_ACTION_PREBREW_MODE: return index<3?prebrew[index]:"—";
    case GBS_ACTION_POWER: return index<3?power[index]:"—";
    case GBS_ACTION_STANDBY_ENABLED: return index<2?enabled[index]:"—";
    case GBS_ACTION_STANDBY_MODE: return index<2?standby[index]:"—";
    default: return NULL;
    }
}
static void parameter_text(gbs_ui_action_t action,double value,char *out,size_t size)
{
    if(!isfinite(value)) { snprintf(out,size,"—"); return; }
    const char *choice=choice_label(action,(unsigned)value);
    if(choice) snprintf(out,size,"%s",choice);
    else if(action==GBS_ACTION_PREBREW_ON||action==GBS_ACTION_PREBREW_OFF) snprintf(out,size,"%.1f 秒",value);
    else if(action==GBS_ACTION_STANDBY_MINUTES) snprintf(out,size,"%.0f 分钟",value);
    else snprintf(out,size,"%.0fg",value);
}
static bool needs_confirmation(gbs_ui_action_t action)
{
    return action==GBS_ACTION_POWER||(action>=GBS_ACTION_STANDBY_ENABLED&&action<=GBS_ACTION_STANDBY_MODE);
}

/* LVGL 8.4 cannot transform A8 images. Resample once per size, then draw at 1:1. */
struct icon_mask { gbs_icon_t name; int width; lv_img_dsc_t image; uint8_t pixels[]; };
static void icon_delete(lv_event_t *e)
{
    struct icon_mask *mask=lv_obj_get_user_data(lv_event_get_target(e));
    if(mask) {lv_img_cache_invalidate_src(&mask->image);lv_mem_free(mask);}
}
static void icon_source(lv_obj_t *o,gbs_icon_t name,int width)
{
    struct icon_mask *old=lv_obj_get_user_data(o);
    if(old&&old->name==name&&old->width==width) return;
    const lv_img_dsc_t *src=gbs_icons[name];
    unsigned height=width*src->header.h/src->header.w;
    struct icon_mask *mask=lv_mem_alloc(sizeof(*mask)+width*height);
    if(!mask) return;
    memset(mask,0,sizeof(*mask)); mask->name=name;mask->width=width;
    mask->image.header.cf=LV_IMG_CF_ALPHA_8BIT;mask->image.header.w=width;mask->image.header.h=height;
    mask->image.data=mask->pixels;mask->image.data_size=width*height;
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<(unsigned)width;++x) {
        unsigned sx=x*(src->header.w-1)*256/(width>1?width-1:1), sy=y*(src->header.h-1)*256/(height>1?height-1:1);
        unsigned x0=sx/256,y0=sy/256,x1=x0+1<src->header.w?x0+1:x0,y1=y0+1<src->header.h?y0+1:y0;
        unsigned fx=sx%256,fy=sy%256;
        unsigned a=src->data[y0*src->header.w+x0]*(256-fx)+src->data[y0*src->header.w+x1]*fx;
        unsigned b=src->data[y1*src->header.w+x0]*(256-fx)+src->data[y1*src->header.w+x1]*fx;
        mask->pixels[y*width+x]=(a*(256-fy)+b*fy+32768)/65536;
    }
    lv_obj_set_user_data(o,mask);lv_img_set_zoom(o,LV_IMG_ZOOM_NONE);lv_img_set_src(o,&mask->image);
    if(old) {lv_img_cache_invalidate_src(&old->image);lv_mem_free(old);}
}
static lv_obj_t *icon(lv_obj_t *parent, gbs_icon_t name, int x, int y, int size, uint32_t color)
{
    lv_obj_t *o = lv_img_create(parent);
    icon_source(o,name,size);
    lv_obj_add_event_cb(o,icon_delete,LV_EVENT_DELETE,NULL);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_img_recolor(o, lv_color_hex(color), 0);
    lv_obj_set_style_img_recolor_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 7, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text); lv_obj_set_pos(o, x, y); lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(o, 2, 0);
    return o;
}

static lv_obj_t *field(lv_obj_t *parent, unsigned id, int x, int y, int w,
                       const lv_font_t *font, uint32_t color)
{
    return ui.fields[id] = label(parent, "", x, y, w, font, color);
}

static void centered(lv_obj_t *o) { lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0); }

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                        int action, bool filled)
{
    lv_obj_t *b = box(parent, x, y, w, h, filled ? GOLD : SURFACE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(filled ? GOLD : LINE), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(LINE), LV_STATE_PRESSED);
#ifdef HUB_AMOLED_216
    lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_DISABLED);
#else
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
#endif
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(filled ? 0xddb477 : 0x1a1e1a), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_t *t = label(b, text, 0, 0, w - 4, &gbs_font_14_bold, filled ? BG : TEXT);
    centered(t); lv_obj_center(t);
    lv_obj_add_event_cb(b, event, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    return b;
}

static lv_obj_t *icon_button(lv_obj_t *parent, gbs_icon_t name, int x, int y, int action)
{
    lv_obj_t *b = button(parent, "", x, y, 44, 44, action, false);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    icon(b, name, 12, 11, 22, TEXT);
    return b;
}

static void rule(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *line = box(parent, x, y, w, h, LINE);
    lv_obj_set_style_radius(line, 0, 0);
}

static lv_obj_t *ring(lv_obj_t *parent, int x, int y, int size, bool progress)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_pos(a, x, y); lv_obj_set_size(a, size, size);
    lv_arc_set_rotation(a, 270); lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 180); lv_arc_set_value(a, progress ? 156 : 0);
    lv_obj_set_style_arc_color(a, lv_color_hex(LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(GOLD), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    if (size >= 100) {
        lv_obj_t *dot = box(parent, x + size / 2 - 3, y - 1, 7, 7, GOLD);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    }
    return a;
}

static void header(const char *title, bool record)
{
#ifdef HUB_AMOLED_216
    fallback_back=icon_button(lv_scr_act(), GBS_ICON_CHEVRON_LEFT, 0, 0, BACK);
#else
    icon_button(lv_scr_act(), GBS_ICON_CHEVRON_LEFT, 0, 0, BACK);
#endif
    lv_obj_t *heading = label(lv_scr_act(), title, 44, 10, record ? 137 : 192, &gbs_font_16_bold, TEXT);
#ifdef HUB_AMOLED_216
    fallback_heading=heading;
#endif
    if (record) centered(heading);
    if (record) {
        lv_obj_t *b = button(lv_scr_act(), "记录", 188, 0, 48, 44, GBS_PAGE_RECORD, false);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(b, 0, 0);
    }
}

static void row(const char *title, const char *subtitle, int index, gbs_ui_page_t destination)
{
    (void)subtitle;
    int y = index * 44;
    lv_obj_t *b = button(ui.body, "", 0, y, 216, 44, destination, false);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(b, 0, 0);
    gbs_icon_t symbol = GBS_ICON_SETTINGS; uint32_t color = MUTED;
    switch (destination) {
    case GBS_PAGE_MACHINE: case GBS_PAGE_COFFEE: symbol = GBS_ICON_TEMPERATURE; color = GOLD; break;
    case GBS_PAGE_STEAM: symbol = GBS_ICON_CLOUD_RAIN; color = TEXT; break;
    case GBS_PAGE_LINK: symbol = GBS_ICON_LINK; color = GREEN; break;
    case GBS_PAGE_WEIGHT: symbol = GBS_ICON_COFFEE; color = GREEN; break;
    case GBS_PAGE_BBW: symbol = GBS_ICON_CIRCLE_DOTTED; break;
    case GBS_PAGE_PREBREW: symbol = GBS_ICON_DROPLET; break;
    case GBS_PAGE_COFFEES: symbol = GBS_ICON_LIST_DETAILS; break;
    case GBS_PAGE_DISPLAY: symbol = GBS_ICON_DEVICE_DESKTOP; break;
    case GBS_PAGE_POWER: symbol = GBS_ICON_POWER; break;
    case GBS_PAGE_STANDBY: symbol = GBS_ICON_CLOCK; break;
    case GBS_PAGE_SCHEDULE: symbol = GBS_ICON_CALENDAR; break;
    default: break;
    }
    icon(b, symbol, -1, 8, 28, color);
    label(b, title, 37, 12, 160, &gbs_font_14_bold, TEXT);
    icon(b, GBS_ICON_CHEVRON_RIGHT, 199, 15, 16, MUTED);
    if (ui.page == GBS_PAGE_MACHINE) {
        unsigned id = destination == GBS_PAGE_COFFEE ? F_COFFEE_TARGET : destination == GBS_PAGE_STEAM ? F_STEAM_TARGET : F_WEIGHT;
        if (destination == GBS_PAGE_COFFEE || destination == GBS_PAGE_STEAM || destination == GBS_PAGE_WEIGHT)
            field(b, id, 145, 13, 54, &gbs_number_16, TEXT);
    }
    rule(b, 0, 43, 216, 1);
}

static void list_body(void)
{
    ui.body = box(lv_scr_act(), 12, 44, 216, 228, BG);
    lv_obj_add_flag(ui.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.body, LV_SCROLLBAR_MODE_AUTO);
}

static void message(const char *title, const char *body)
{
    label(lv_scr_act(), title, 18, 72, 204, &gbs_font_16_bold, GOLD);
    label(lv_scr_act(), body, 18, 108, 204, &gbs_font_14, MUTED);
}

static bool editor(void)
{
    return parameter()!=NULL;
}

static double current_value(void)
{
    return parameter()?gbs_ui_action_value(parameter()->action,&ui.machine):NAN;
}

static void enter(gbs_ui_page_t page)
{
    ui.page = page;
    if(!plan_editor()) ui.dirty=ui.conflict=false;
    ui.discard=ui.confirm=ui.home_requested=ui.navigation_requested=false;
    if (page == GBS_PAGE_MODE) ui.mode_draft = ui.bridge.active;
    if (page == GBS_PAGE_DISPLAY || page == GBS_PAGE_BRIGHTNESS) { ui.brightness_draft = ui.brightness; ui.dim_draft = ui.dim; }
    if (page == GBS_PAGE_SOUND) ui.sound_draft = ui.sound_volume;
    if (editor()) {
        ui.initial = page==GBS_PAGE_VARIABLE_WEIGHT?ui.variable_weight:current_value();
        ui.draft = isfinite(ui.initial) ? ui.initial : parameter()->low;
        ui.edit_snapshot = ui.machine;
        ui.edit_revision = ui.machine.revision;
    }
    if(data_page()&&ui.data[data_kind()].state==GBS_DATA_IDLE) request_data();
    render();
}

void gbs_ui_show(gbs_ui_page_t page)
{
    if (page >= GBS_PAGE_COUNT) return;
    if (page != ui.page && ui.depth < sizeof(ui.stack) / sizeof(ui.stack[0])) ui.stack[ui.depth++] = ui.page;
    enter(page);
}

static void back(void)
{
    if (ui.discard || ui.confirm) { ui.discard = ui.confirm = ui.home_requested = ui.navigation_requested = false; render(); return; }
    if(plan_editor()&&ui.page!=GBS_PAGE_SCHEDULE_EDIT) { enter(ui.depth?ui.stack[--ui.depth]:GBS_PAGE_SCHEDULE_EDIT); return; }
    if (ui.dirty) { ui.discard = true; render(); return; }
    enter(ui.depth ? ui.stack[--ui.depth] : GBS_PAGE_HOME);
}

static void send(gbs_ui_action_t action, double value)
{
    if (ui.control == GBS_UI_CONTROL_PENDING || !ui.submit) return;
    if (gbs_ui_machine_action(action) &&
        (!gbs_ui_can_control(action,&ui.machine) || ui.conflict)) {
        refresh(); return;
    }
    if (!++request_sequence) ++request_sequence;
    ui.request_id = request_sequence; ui.action = action; ui.requested = value;
#ifdef HUB_AMOLED_216
    if(action==GBS_ACTION_WEIGHT) ui.manual_weight=true;
#endif
    const gbs_ui_machine_t *expected=editor()?&ui.edit_snapshot:&ui.machine;
    if (!ui.submit(action, value, ui.request_id, expected, ui.context)) { ui.control = GBS_UI_CONTROL_FAILED; queue_tone(GBS_TONE_FAILURE); ui.confirm=false; render(); return; }
    ui.schedule_submitted=false;
    if(action==GBS_ACTION_WEIGHT) ui.previous_weight=ui.machine.available&&!ui.machine.stale?ui.machine.weight:NAN;
    if(action==GBS_ACTION_WEIGHT) ui.manual_weight=true;
    ui.control = GBS_UI_CONTROL_PENDING; ui.submitted_ms = ui.now; ui.submitted_observation = ui.bridge.observed_ms;
    ui.dirty = ui.confirm = false; render();
}

static void toggle_rotation(void)
{
    bool requested=!ui.auto_rotate;
    bool ok=ui.submit&&ui.submit(GBS_ACTION_ROTATION_AUTO,requested,0,NULL,ui.context);
    if(ok) {
        ui.auto_rotate=requested;
        queue_tone(GBS_TONE_SUCCESS);
    } else queue_tone(GBS_TONE_FAILURE);
    snprintf(ui.preference_notice,sizeof(ui.preference_notice),"%s",!ok?"保存失败，请重试":
        requested?"自动旋转已开启":"已锁定当前方向");
    ui.preference_notice_until=ui.now+2500;
    render();
}

static void event(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
#ifdef HUB_AMOLED_216
    if(a==PRESET+3 && lv_event_get_code(e)==LV_EVENT_LONG_PRESSED) {
        if(ui.control==GBS_UI_CONTROL_PENDING || lv_obj_has_state(lv_event_get_target(e),LV_STATE_DISABLED)) return;
        lv_indev_t *input=lv_indev_get_act();
        if(input) lv_indev_wait_release(input);
        gbs_ui_show(GBS_PAGE_VARIABLE_WEIGHT); return;
    }
#endif
    if(a==GBS_PAGE_SCHEDULE_EDIT) {
        if(ui.control==GBS_UI_CONTROL_PENDING||!ui.data[GBS_DATA_SCHEDULE].editable||ui.data[GBS_DATA_SCHEDULE].state!=GBS_DATA_READY||ui.selected_stale||!ui.selected_schedule.id[0]) return;
        ui.schedule_draft=ui.selected_schedule; ui.schedule_submitted=false; ui.dirty=ui.conflict=false;
    }
    if(a==GBS_PAGE_SCHEDULE&&ui.schedule_submitted) {
        ui.data[GBS_DATA_SCHEDULE].state=GBS_DATA_IDLE; ui.schedule_submitted=false;
    }
    if (a < GBS_PAGE_COUNT) {
        if(editor()&&ui.dirty) {
            ui.navigation_page=(gbs_ui_page_t)a;ui.navigation_requested=ui.discard=true;render();
        } else gbs_ui_show((gbs_ui_page_t)a);
        return;
    }
    if (a == BACK) { back(); return; }
    if(a==SCALE_REFRESH) {
        bool ok=ui.scale_refresh&&ui.scale_refresh(ui.context);
        set(F_DEVICE_NOTE,ok?"正在刷新":"暂时无法刷新");
        if(!ok) queue_tone(GBS_TONE_FAILURE);
        return;
    }
    if(a==DIM_SAVE) {
        bool ok=ui.submit&&ui.submit(GBS_ACTION_DIM,!ui.dim,0,NULL,ui.context);
        if(ok) ui.dim=ui.dim_draft=!ui.dim;
        queue_tone(ok?GBS_TONE_SUCCESS:GBS_TONE_FAILURE);
        snprintf(ui.preference_notice,sizeof(ui.preference_notice),"%s",ok?"已保存":"保存失败，请重试");ui.preference_notice_until=ui.now+2500;render();return;
    }
    if(a==ROTATION_TOGGLE) { toggle_rotation(); return; }
    if(a==SOUND_VOLUME) {
        ui.sound_draft=lv_slider_get_value(lv_event_get_target(e));
        ui.dirty=ui.sound_draft!=ui.sound_volume;
        set(F_DETAIL,"0% 静音 · 保存后提示"); refresh(); return;
    }
    if(a==SOUND_SAVE) {
        bool ok=ui.submit&&ui.submit(GBS_ACTION_SOUND,ui.sound_draft,0,NULL,ui.context);
        if(ok) { ui.sound_volume=ui.sound_draft; ui.tone_count=0; }
        ui.dirty=ui.sound_draft!=ui.sound_volume;
        queue_tone(ok?GBS_TONE_SUCCESS:GBS_TONE_FAILURE);
        set(F_DETAIL,ok?(ui.sound_volume?"已保存":"已静音"):"保存失败，请重试");
        return;
    }
    if(data_page()) {
        gbs_ui_data_kind_t kind=data_kind(); gbs_ui_data_t *data=&ui.data[kind];
        if(a==DATA_REFRESH) { request_data(); render(); return; }
        if(a==DATA_PREVIOUS||a==DATA_NEXT) {
            unsigned *page=&ui.data_page[kind];
            if(a==DATA_PREVIOUS&&*page) --*page;
            if(a==DATA_NEXT&&(*page+1)*3<data->count) ++*page;
            render(); return;
        }
        if(a>=DATA_ROW&&a<DATA_ROW+3) {
            unsigned index=ui.data_page[kind]*3+a-DATA_ROW;
            if(!data->valid||index>=data->count) return;
            ui.selected_stale=data->state!=GBS_DATA_READY;
            if(kind==GBS_DATA_SCHEDULE) ui.selected_schedule=data->items.schedules[index];
            else ui.selected_coffee=data->items.coffees[index];
            gbs_ui_show(kind==GBS_DATA_SCHEDULE?GBS_PAGE_SCHEDULE_DETAIL:GBS_PAGE_COFFEE_DETAIL); return;
        }
    }
    if(plan_editor()) {
        bool writable=ui.data[GBS_DATA_SCHEDULE].editable&&ui.data[GBS_DATA_SCHEDULE].state==GBS_DATA_READY&&!ui.conflict;
        if(a==PLAN_SEND) {
            if(!ui.confirm||!writable||ui.control==GBS_UI_CONTROL_PENDING||!ui.schedule_submit) return;
            if(!++request_sequence) ++request_sequence;
            ui.request_id=request_sequence; ui.submitted_ms=ui.now;
            ui.control=ui.schedule_submit(&ui.selected_schedule,&ui.schedule_draft,ui.request_id,ui.context)?GBS_UI_CONTROL_PENDING:GBS_UI_CONTROL_FAILED;
            ui.action=GBS_ACTION_COUNT; ui.dirty=ui.confirm=false; ui.schedule_submitted=true; render(); return;
        }
        if(a==PLAN_APPLY) {
            if(!writable||!ui.dirty||ui.control==GBS_UI_CONTROL_PENDING||
                (ui.schedule_draft.enabled&&(!ui.schedule_draft.days||ui.schedule_draft.on_minutes==ui.schedule_draft.off_minutes))) return;
            ui.confirm=true; render(); return;
        }
        if(a>=PLAN_ENABLED&&a<=MINUTE_PLUS) {
            if(!writable||ui.control==GBS_UI_CONTROL_PENDING||ui.schedule_submitted) return;
            if(a==PLAN_ENABLED) ui.schedule_draft.enabled=!ui.schedule_draft.enabled;
            if(a==PLAN_STEAM) ui.schedule_draft.steam=!ui.schedule_draft.steam;
            uint16_t *minutes=ui.page==GBS_PAGE_SCHEDULE_ON?&ui.schedule_draft.on_minutes:&ui.schedule_draft.off_minutes;
            int hour=*minutes/60,minute=*minutes%60;
            if(a==HOUR_MINUS) hour=(hour+23)%24;
            if(a==HOUR_PLUS) hour=(hour+1)%24;
            if(a==MINUTE_MINUS) minute=(minute+59)%60;
            if(a==MINUTE_PLUS) minute=(minute+1)%60;
            if(a>=HOUR_MINUS) *minutes=hour*60+minute;
            ui.dirty=true; render(); return;
        }
        if(a>=PLAN_DAY&&a<PLAN_DAY+7&&writable&&ui.control!=GBS_UI_CONTROL_PENDING&&!ui.schedule_submitted) {
            ui.schedule_draft.days^=1u<<(a-PLAN_DAY); ui.dirty=true; render(); return;
        }
    }
    if (a == DISCARD) {
        bool home_requested=ui.home_requested,navigate=ui.navigation_requested;
        gbs_ui_page_t destination=ui.navigation_page;
        ui.dirty = ui.discard = ui.home_requested = ui.navigation_requested = false;
        if(home_requested) { ui.depth=0; enter(GBS_PAGE_HOME); }
        else if(navigate) gbs_ui_show(destination);
        else back();
        return;
    }
    if (a == KEEP) { ui.discard = ui.home_requested = ui.navigation_requested = false; render(); return; }
    if (a >= PRESET && a < PRESET + 4) { send(GBS_ACTION_WEIGHT, preset_value(a - PRESET)); return; }
    if (a == MINUS || a == PLUS) {
        if (ui.control == GBS_UI_CONTROL_PENDING || !parameter() || ui.conflict ||
            !gbs_ui_can_control(parameter()->action,&ui.machine)) return;
        const struct parameter *p=parameter();
        ui.draft = fmax(p->low, fmin(p->high, round((ui.draft + (a == PLUS ? p->step : -p->step))*10)/10));
        ui.dirty = !isfinite(ui.initial) || ui.draft != ui.initial; refresh();
    } else if (a == APPLY) {
        if(!parameter()||!ui.dirty||ui.control==GBS_UI_CONTROL_PENDING||ui.conflict) return;
        if(needs_confirmation(parameter()->action)) { ui.requested=ui.draft; ui.confirm=true; render(); }
        else send(parameter()->action,ui.draft);
    } else if (a == MODE_MONITOR || a == MODE_ACTIVE) {
        if (ui.control == GBS_UI_CONTROL_PENDING || !ui.bridge.valid || !ui.bridge.storage) return;
        ui.mode_draft = a == MODE_ACTIVE; render();
    } else if (a == CONFIRM) {
        if (ui.confirm) send(parameter()?parameter()->action:GBS_ACTION_MODE, ui.requested);
        else { ui.requested = ui.mode_draft; ui.confirm = true; render(); }
    } else if (a == BRIGHTNESS) {
        ui.brightness_draft = lv_slider_get_value(lv_event_get_target(e)); ui.dirty = true; refresh();
    } else if (a == DIM) {
        ui.dim_draft = !ui.dim_draft; ui.dirty = true;
        render();
    } else if (a == DISPLAY_SAVE && ui.submit) {
        bool brightness_ok = ui.submit(GBS_ACTION_BRIGHTNESS, ui.brightness_draft, 0, NULL, ui.context);
        if (brightness_ok) ui.brightness = ui.brightness_draft;
        bool dim_ok = ui.submit(GBS_ACTION_DIM, ui.dim_draft, 0, NULL, ui.context);
        if (dim_ok) ui.dim = ui.dim_draft;
        ui.dirty = !(brightness_ok && dim_ok);
        queue_tone(ui.dirty?GBS_TONE_FAILURE:GBS_TONE_SUCCESS);
        set(F_DETAIL, ui.dirty ? "保存未完成，请重试" : "已保存，首次触摸仅唤醒");
    } else if (a == STEAM_TOGGLE) {
        send(GBS_ACTION_STEAM_ENABLED, !ui.machine.steam_enabled);
    } else if(a>=CHOICE&&a<CHOICE+4&&parameter()&&ui.control!=GBS_UI_CONTROL_PENDING) {
        if(!gbs_ui_can_control(parameter()->action,&ui.machine)||a-CHOICE>parameter()->high) return;
        ui.draft=a-CHOICE; ui.dirty=!isfinite(ui.initial)||ui.draft!=ui.initial; render();
    } else if (a >= 300 && a <= 302 && ui.control != GBS_UI_CONTROL_PENDING) {
        const int levels[] = {126, 128, 131};
        ui.draft = levels[a - 300]; ui.dirty = ui.draft != ui.initial; render();
    }
}


#ifndef HUB_AMOLED_216
static void dial(int x, bool steam)
{
    ring(lv_scr_act(), x, 34, 108, false);
    icon(lv_scr_act(), steam ? GBS_ICON_CLOUD_RAIN : GBS_ICON_DROPLET, x + 46, 46, 17, TEXT);
    centered(label(lv_scr_act(), steam ? "蒸汽" : "锅炉", x + 2, 63, 104, &gbs_font_12, MUTED));
    centered(field(lv_scr_act(), steam ? F_STEAM : F_COFFEE, x, 77, 108, &gbs_number_34, TEXT));
    if (steam) centered(field(lv_scr_act(), F_STEAM_CURRENT, x + 2, 108, 104, &gbs_font_10, TEXT));
    else centered(label(lv_scr_act(), "°C", x + 2, 108, 104, &gbs_font_12, TEXT));
    rule(lv_scr_act(), x + 25, 122, 58, 1);
    centered(field(lv_scr_act(), steam ? F_STEAM_TARGET : F_COFFEE_TARGET, x + 2, 124, 104, &gbs_font_10, MUTED));
    // The visible ring starts above its hit area so it never overlaps the gear.
    lv_obj_t *hit = button(lv_scr_act(), "", x, 44, 108, 98, steam ? GBS_PAGE_STEAM : GBS_PAGE_COFFEE, false);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(hit, 0, 0);
}

static void home(void)
{
    label(lv_scr_act(), "咖啡机", 11, 8, 70, &gbs_font_16_bold, TEXT);
    if (ui.machine.available && !ui.machine.stale) {
        lv_obj_t *dot = box(lv_scr_act(), 70, 16, 7, 7, GREEN); lv_obj_set_style_radius(dot, 4, 0);
    } else field(lv_scr_act(), F_CONFLICT, 76, 13, 108, &gbs_font_12, MUTED);
    icon_button(lv_scr_act(), GBS_ICON_SETTINGS, 192, 0, GBS_PAGE_SETTINGS);
    dial(8, false); dial(124, true);
    label(lv_scr_act(), "快捷克重", 10, 155, 100, &gbs_font_12_bold, TEXT);
    field(lv_scr_act(), F_WEIGHT, 132, 154, 98, &gbs_font_12, MUTED);
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_t *b = button(lv_scr_act(), "", 10 + 56 * i, 168, 50, 44, PRESET + i, false);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(b, 0, 0);
        ui.presets[i] = b;
        lv_obj_t *face = box(b, 0, 5, 50, 34, SURFACE); ui.preset_faces[i] = face;
        lv_obj_set_style_border_width(face, 1, 0); lv_obj_set_style_border_color(face, lv_color_hex(LINE), 0);
        lv_obj_set_style_bg_grad_color(face, lv_color_hex(0x1a1e1a), 0); lv_obj_set_style_bg_grad_dir(face, LV_GRAD_DIR_VER, 0);
        char text[12]; snprintf(text, sizeof(text), "%dg", presets[i]);
        lv_obj_t *t = label(face, text, 0, 0, 50, &gbs_number_16, TEXT); centered(t); lv_obj_center(t);
    }
    lv_obj_t *card = button(lv_scr_act(), "", 10, 212, 220, 62, GBS_PAGE_LINK, false);
    gbs_ui_stage_t stage = gbs_ui_stage(&ui.bridge);
    uint32_t tone = stage == GBS_UI_UNKNOWN || stage == GBS_UI_INVALID || stage == GBS_UI_STORAGE || stage == GBS_UI_PAIRING ? RED :
                    stage == GBS_UI_READY || stage == GBS_UI_RECEIVED || stage == GBS_UI_MONITOR ? GREEN : WARN;
    if (stage == GBS_UI_GRINDER || stage == GBS_UI_WIFI || stage == GBS_UI_PAIRING || stage == GBS_UI_STORAGE) {
        icon(card, GBS_ICON_ALERT_CIRCLE, 9, 17, 25, tone);
        field(card, F_STAGE, 42, 8, 171, &gbs_font_14_bold, TEXT);
        field(card, F_TIMER_CAPTION, 42, 31, 100, &gbs_font_12, MUTED);
        field(card, F_TIME, 143, 28, 76, &gbs_number_20, WARN);
    } else {
        icon(card, GBS_ICON_LINK, 9, 7, 20, tone);
        label(card, "磨豆机联动", 36, 7, 111, &gbs_font_14_bold, TEXT);
        field(card, F_STAGE, 26, ui.bridge.job ? 25 : 31, 112, &gbs_font_12, MUTED);
        lv_obj_t *dot = box(card, 11, ui.bridge.job ? 32 : 38, 7, 7, tone); lv_obj_set_style_radius(dot, 4, 0);
        if (ui.bridge.job) {
            field(card, F_TARGET, 11, 43, 142, &gbs_font_10, MUTED);
            field(card, F_TIMER_CAPTION, 150, 8, 68, &gbs_font_12, MUTED);
            field(card, F_TIME, 140, 31, 79, &gbs_number_24, TEXT);
        } else {
            icon(card, GBS_ICON_CHEVRON_RIGHT, 201, 12, 17, MUTED);
            if (stage != GBS_UI_UNKNOWN && stage != GBS_UI_INTERRUPTED && stage != GBS_UI_INVALID) {
                rule(card, 77, 36, 1, 14);
                field(card, F_HOME_HINT, 88, 31, 119, &gbs_font_12, MUTED);
            }
        }
    }
}

#endif

static void bottom(const char *title, int action, bool filled)
{
    lv_obj_t *b = button(lv_scr_act(), "", 10, 232, 220, 44, action, false);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t *face = box(b, 0, 4, 220, 36, filled ? GOLD : SURFACE);
    lv_obj_set_style_bg_grad_color(face, lv_color_hex(filled ? 0xddb477 : 0x1a1e1a), 0);
    lv_obj_set_style_bg_grad_dir(face, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(face, 1, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(filled ? 0xf0cb91 : LINE), 0);
    lv_obj_t *t = label(face, title, 0, 0, 212, &gbs_font_16_bold, filled ? BG : TEXT);
    centered(t); lv_obj_center(t);
    if (action == GBS_PAGE_CONNECTIONS && !filled) {
        icon(face, GBS_ICON_LINK, 12, 7, 22, GREEN);
        icon(face, GBS_ICON_CHEVRON_RIGHT, 196, 10, 16, MUTED);
    }
    if (action == APPLY) ui.apply = b;
}

static void metric_pair(int y, bool compact)
{
    centered(field(lv_scr_act(), F_RESULT, 12, y, 94, compact ? &gbs_number_32 : &gbs_number_36, TEXT));
    centered(field(lv_scr_act(), F_SECOND_RESULT, 125, y, 94, compact ? &gbs_number_32 : &gbs_number_36, TEXT));
    label(lv_scr_act(), "s", 100, y + 15, 14, &gbs_font_12, TEXT);
    label(lv_scr_act(), "g", 213, y + 15, 14, &gbs_font_12, TEXT);
    rule(lv_scr_act(), 120, y + 3, 1, compact ? 39 : 44);
    centered(label(lv_scr_act(), "萃取用时", 12, y + 31, 102, &gbs_font_12, MUTED));
    centered(label(lv_scr_act(), "实测重量", 126, y + 31, 102, &gbs_font_12, MUTED));
}

static void parameter_summary(const char *title,const gbs_ui_page_t pages[3])
{
    header(title,false);
    for(unsigned i=0;i<3;++i) {
        const struct parameter *p=NULL;
        for(unsigned n=0;n<sizeof(parameters)/sizeof(parameters[0]);++n)
            if(parameters[n].page==pages[i]) p=&parameters[n];
        if(!p) continue;
        lv_obj_t *card=button(lv_scr_act(),"",10,50+60*i,220,56,p->page,false);
        label(card,p->title,12,7,185,&gbs_font_12,MUTED);
        field(card,F_PARAMETER0+i,12,27,185,&gbs_font_16_bold,TEXT);
        icon(card,GBS_ICON_CHEVRON_RIGHT,197,20,18,MUTED);
    }
    centered(field(lv_scr_act(),F_CONTROL,10,240,220,&gbs_font_12,MUTED));
}

static void choice_page(void)
{
    const struct parameter *p=parameter(); header(p->title,false);
    bool power=p->action==GBS_ACTION_POWER;
    if(power) {
        icon(lv_scr_act(),GBS_ICON_POWER,104,44,32,GOLD);
        centered(label(lv_scr_act(),"当前状态",12,80,216,&gbs_font_12,MUTED));
        centered(field(lv_scr_act(),F_PARAMETER0,12,100,216,&gbs_font_16_bold,TEXT));
    }
    for(unsigned i=0;i<=(unsigned)p->high;++i) {
        int y=power?144+44*i:p->high==3?42+44*i:58+50*i;
        lv_obj_t *b=ui.choices[i]=button(lv_scr_act(),"",10,y,220,42,CHOICE+i,false);
        label(b,choice_label(p->action,i),12,12,173,&gbs_font_14_bold,TEXT);
        if(current_value()==i) label(b,"当前",177,14,36,&gbs_font_10,MUTED);
    }
    centered(field(lv_scr_act(),F_CONFLICT,10,power?125:218,220,&gbs_font_10,MUTED));
    bottom(needs_confirmation(p->action)?"继续确认":"应用",APPLY,true);
}


#ifndef HUB_AMOLED_216
static void edit_page(void)
{
    const struct parameter *p=parameter();
    bool coffee = ui.page == GBS_PAGE_COFFEE;
    header(p->title, false);
    bool feedback = ui.control != GBS_UI_CONTROL_IDLE && ui.action == p->action && !ui.dirty;
    ui.ring = ring(lv_scr_act(), 46, 40, 148, feedback && ui.control == GBS_UI_CONTROL_PENDING);
    if (feedback && ui.control == GBS_UI_CONTROL_PENDING) lv_arc_set_value(ui.ring, 22);
    gbs_icon_t symbol=coffee||p->action==GBS_ACTION_PREBREW_ON?GBS_ICON_DROPLET:
        p->action==GBS_ACTION_PREBREW_OFF||p->action==GBS_ACTION_STANDBY_MINUTES?GBS_ICON_CLOCK:GBS_ICON_WEIGHT;
    icon(lv_scr_act(), feedback && ui.control == GBS_UI_CONTROL_CONFIRMED ? GBS_ICON_CIRCLE_CHECK_FILLED : symbol,
         110, 54, 21, feedback && ui.control == GBS_UI_CONTROL_CONFIRMED ? GREEN : TEXT);
    centered(label(lv_scr_act(), feedback ? (p->action>=GBS_ACTION_DOSE1&&ui.control!=GBS_UI_CONTROL_CONFIRMED?"请求值":p->title) : "待应用", 55, 81, 130, &gbs_font_12, MUTED));
    centered(field(lv_scr_act(), F_DRAFT, 48, 99, 144, &gbs_number_40, TEXT));
    centered(label(lv_scr_act(), p->unit, 85, 136, 70, &gbs_font_12, TEXT));
    rule(lv_scr_act(), 82, 152, 76, 1);
    centered(field(lv_scr_act(), F_CONFLICT, 46, 156, 148, &gbs_font_12, MUTED));
    if (!feedback || ui.control == GBS_UI_CONTROL_FAILED || ui.control == GBS_UI_CONTROL_PARTIAL || ui.control == GBS_UI_CONTROL_UNKNOWN) {
        lv_obj_t *minus = button(lv_scr_act(), "", 25, 182, 44, 44, MINUS, false);
        lv_obj_t *plus = button(lv_scr_act(), "", 171, 182, 44, 44, PLUS, false);
        lv_obj_set_style_radius(minus, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_radius(plus, LV_RADIUS_CIRCLE, 0);
        icon(minus, GBS_ICON_MINUS, 11, 11, 24, TEXT); icon(plus, GBS_ICON_PLUS, 11, 11, 24, TEXT);
        char step[32]; snprintf(step,sizeof(step),p->step<1?"每次 %.1f%s":"每次 %.0f%s",p->step,p->unit);
        centered(label(lv_scr_act(), step, 70, 196, 100, &gbs_font_12, TEXT));
        bottom(needs_confirmation(p->action)?"继续确认":"应用", APPLY, true);
    } else {
        if (ui.control == GBS_UI_CONTROL_PENDING) {
            centered(field(lv_scr_act(), F_HOME_HINT, 12, 188, 216, &gbs_font_12, MUTED));
            lv_obj_t *card = box(lv_scr_act(), 10, 211, 220, 23, SURFACE);
            lv_obj_set_style_border_width(card, 1, 0); lv_obj_set_style_border_color(card, lv_color_hex(LINE), 0);
            lv_obj_t *spinner = ring(card, 8, 4, 15, true); lv_arc_set_value(spinner, 135);
            field(card, F_DETAIL, 39, 3, 173, &gbs_font_12, TEXT);
        } else centered(field(lv_scr_act(), F_DETAIL, 12, 201, 216, &gbs_font_12, MUTED));
        bottom(ui.control == GBS_UI_CONTROL_CONFIRMED ? "完成" : "返回", p->action>=GBS_ACTION_DOSE1?BACK:GBS_PAGE_HOME, ui.control == GBS_UI_CONTROL_CONFIRMED);
    }
}


#endif

static void linked_page(void)
{
    gbs_ui_stage_t stage = gbs_ui_stage(&ui.bridge);
    header(stage == GBS_UI_RECEIVED ? "联动结果" : stage == GBS_UI_UNKNOWN || stage == GBS_UI_CANCELING ? "联动状态" : "联动详情", true);
    if (stage == GBS_UI_RECEIVED) {
        lv_obj_t *check = box(lv_scr_act(), 80, 50, 80, 80, BG);
        lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(check, 3, 0); lv_obj_set_style_border_color(check, lv_color_hex(GREEN), 0);
        icon(lv_scr_act(), GBS_ICON_CHECK, 90, 59, 60, GREEN);
        centered(field(lv_scr_act(), F_STAGE, 12, 130, 216, &gbs_font_20_bold, TEXT));
        centered(label(lv_scr_act(), "磨豆机已确认接收", 12, 164, 216, &gbs_font_12, MUTED));
        rule(lv_scr_act(), 12, 181, 216, 1); metric_pair(187, true);
        bottom("返回首页", GBS_PAGE_HOME, true);
    } else if (stage == GBS_UI_UNKNOWN) {
        icon(lv_scr_act(), GBS_ICON_HELP_CIRCLE, 104, 41, 32, WARN);
        centered(field(lv_scr_act(), F_STAGE, 12, 73, 216, &gbs_font_20_bold, TEXT));
        centered(label(lv_scr_act(), "结果可能已送达", 12, 106, 216, &gbs_font_14, MUTED));
        lv_obj_t *card = box(lv_scr_act(), 10, 133, 220, 68, SURFACE);
        lv_obj_set_style_border_color(card, lv_color_hex(LINE), 0); lv_obj_set_style_border_width(card, 1, 0);
        label(card, "本次测量", 10, 5, 180, &gbs_font_12, TEXT);
        centered(field(card, F_RESULT, 10, 25, 84, &gbs_number_32, TEXT));
        centered(field(card, F_SECOND_RESULT, 112, 25, 84, &gbs_number_32, TEXT));
        label(card, "s", 91, 40, 12, &gbs_font_12, MUTED);
        label(card, "g", 193, 40, 12, &gbs_font_12, MUTED);
        rule(card, 104, 26, 1, 27);
        centered(label(lv_scr_act(), "不会自动重发", 12, 212, 216, &gbs_font_12_bold, WARN));
        bottom("查看连接", GBS_PAGE_CONNECTIONS, false);
    } else if (stage == GBS_UI_CANCELING) {
        icon(lv_scr_act(), GBS_ICON_ALERT_CIRCLE, 106, 39, 28, WARN);
        centered(label(lv_scr_act(), "正在结束任务", 12, 71, 216, &gbs_font_20_bold, TEXT));
        ui.ring = ring(lv_scr_act(), 65, 105, 110, true);
        centered(field(lv_scr_act(), F_TIMER_CAPTION, 65, 129, 110, &gbs_font_12, MUTED));
        centered(field(lv_scr_act(), F_TIME, 60, 145, 120, &gbs_number_36, TEXT));
        centered(label(lv_scr_act(), "等待磨豆机确认", 12, 216, 216, &gbs_font_12, MUTED));
        bottom("查看连接", GBS_PAGE_CONNECTIONS, false);
    } else if (stage == GBS_UI_INVALID) {
        icon(lv_scr_act(), GBS_ICON_ALERT_CIRCLE, 96, 48, 48, WARN);
        centered(field(lv_scr_act(), F_STAGE, 12, 110, 216, &gbs_font_20_bold, TEXT));
        centered(field(lv_scr_act(), F_DETAIL, 12, 149, 216, &gbs_font_12, MUTED));
        centered(label(lv_scr_act(), "本杯未回传", 12, 180, 216, &gbs_font_14_bold, WARN));
        bottom("返回首页", GBS_PAGE_HOME, true);
    } else if (ui.bridge.job) {
        ui.ring = ring(lv_scr_act(), 62, 43, 116, true);
        centered(field(lv_scr_act(), F_TIMER_CAPTION, 62, 76, 116, &gbs_font_12, MUTED));
        centered(field(lv_scr_act(), F_TIME, 60, 92, 120, &gbs_number_36, TEXT));
        centered(field(lv_scr_act(), F_STAGE, 10, 165, 220, &gbs_font_20_bold, TEXT));
        centered(field(lv_scr_act(), F_TARGET, 12, 196, 216, &gbs_font_14, MUTED));
        centered(field(lv_scr_act(), F_DETAIL, 12, 217, 216, &gbs_font_12, MUTED));
        bottom("返回首页", GBS_PAGE_HOME, true);
    } else {
        icon(lv_scr_act(), stage == GBS_UI_READY ? GBS_ICON_LINK : GBS_ICON_ALERT_CIRCLE, 84, 54, 72, stage == GBS_UI_READY ? GREEN : WARN);
        centered(field(lv_scr_act(), F_STAGE, 12, 143, 216, &gbs_font_20_bold, TEXT));
        centered(field(lv_scr_act(), F_DETAIL, 12, 182, 216, &gbs_font_12, MUTED));
        button(lv_scr_act(), "连接", 12, 232, 104, 40, GBS_PAGE_CONNECTIONS, false);
        button(lv_scr_act(), "联动模式", 124, 232, 104, 40, GBS_PAGE_MODE, false);
    }
}

static const char *data_status(const gbs_ui_data_t *data)
{
    switch(data->state) {
    case GBS_DATA_LOADING: return data->valid?"旧记录 · 刷新中":"正在读取";
    case GBS_DATA_STALE: return "旧记录 · 请稍后刷新";
    case GBS_DATA_UNAVAILABLE: return "暂时无法读取";
    case GBS_DATA_UNSUPPORTED: return "机器不支持定时计划";
    case GBS_DATA_ERROR: return "读取失败";
    default: return data->kind==GBS_DATA_SCHEDULE?"计划时间 · 按机器设置":"云端记录 · UTC";
    }
}

static void days_text(uint8_t days,char *out,size_t size)
{
    if(days==127) { snprintf(out,size,"每天"); return; }
    if(days==31) { snprintf(out,size,"周一至周五"); return; }
    if(days==96) { snprintf(out,size,"周六、周日"); return; }
    if(!days) { snprintf(out,size,"未设置重复日期"); return; }
    const char *names[]={"一","二","三","四","五","六","日"};
    snprintf(out,size,"周");
    for(unsigned i=0;i<7;++i) if(days&(1u<<i)) {
        size_t used=strlen(out); snprintf(out+used,size-used," %s",names[i]);
    }
}

static void collection_page(void)
{
    gbs_ui_data_kind_t kind=data_kind(); const gbs_ui_data_t *data=&ui.data[kind];
    header(kind==GBS_DATA_SCHEDULE?"定时计划":"出杯记录",false);
    lv_obj_t *refresh_button=button(lv_scr_act(),"刷新",188,0,48,44,DATA_REFRESH,false);
    lv_obj_set_style_bg_opa(refresh_button,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(refresh_button,0,0);
    if(data->state==GBS_DATA_LOADING) lv_obj_add_state(refresh_button,LV_STATE_DISABLED);
    label(lv_scr_act(),data_status(data),12,46,216,&gbs_font_12,data->state==GBS_DATA_READY?MUTED:WARN);
    if(!data->valid||!data->count) {
        bool empty=data->valid&&data->state==GBS_DATA_READY;
        icon(lv_scr_act(),kind==GBS_DATA_SCHEDULE?GBS_ICON_CALENDAR:GBS_ICON_COFFEE,99,88,42,GOLD);
        centered(label(lv_scr_act(),empty?(kind==GBS_DATA_SCHEDULE?"暂无定时计划":"暂无出杯记录"):
            data->state==GBS_DATA_LOADING?"正在读取":data->state==GBS_DATA_UNSUPPORTED?"尚不支持":"暂时无法显示",
            12,145,216,&gbs_font_20_bold,TEXT));
        centered(label(lv_scr_act(),kind==GBS_DATA_SCHEDULE?"在咖啡机应用中管理计划":"记录由咖啡机云端提供",
            12,183,216,&gbs_font_12,MUTED));
        if(data->state==GBS_DATA_LOADING) centered(label(lv_scr_act(),"请稍候，完成后自动显示",12,241,216,&gbs_font_12,MUTED));
        else bottom("重新读取",DATA_REFRESH,false);
        return;
    }
    char text[96]; unsigned first=ui.data_page[kind]*3;
    for(unsigned row=0;row<3&&first+row<data->count;++row) {
        lv_obj_t *card=button(lv_scr_act(),"",10,70+53*row,220,49,DATA_ROW+row,false);
        if(kind==GBS_DATA_SCHEDULE) {
            const gbs_ui_schedule_t *plan=&data->items.schedules[first+row];
            snprintf(text,sizeof(text),"%02u:%02u - %02u:%02u",plan->on_minutes/60,plan->on_minutes%60,plan->off_minutes/60,plan->off_minutes%60);
            label(card,text,10,5,166,&gbs_number_20,TEXT);
            days_text(plan->days,text,sizeof(text)); label(card,text,10,30,166,&gbs_font_10,MUTED);
            label(card,plan->enabled?"启用":"关闭",181,17,33,&gbs_font_10,plan->enabled?GREEN:MUTED);
        } else {
            const gbs_ui_coffee_t *coffee=&data->items.coffees[first+row];
            snprintf(text,sizeof(text),"%.5s  %.5s",coffee->time+5,coffee->time+11);
            label(card,text,10,6,138,&gbs_number_16,TEXT);
            if(isfinite(coffee->temperature)) snprintf(text,sizeof(text),"目标 %.1f°C",coffee->temperature);
            else snprintf(text,sizeof(text),"目标温度 —");
            label(card,text,10,29,150,&gbs_font_10,MUTED);
            if(isfinite(coffee->seconds)) snprintf(text,sizeof(text),"%.1fs",coffee->seconds);
            else snprintf(text,sizeof(text),"—");
            lv_obj_t *value=label(card,text,142,13,65,coffee->seconds>=1000?&gbs_font_12:coffee->seconds>=100?&gbs_number_16:&gbs_number_20,GOLD);
            lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_RIGHT,0);
        }
    }
    lv_obj_t *previous=icon_button(lv_scr_act(),GBS_ICON_CHEVRON_LEFT,10,234,DATA_PREVIOUS);
    lv_obj_t *next=icon_button(lv_scr_act(),GBS_ICON_CHEVRON_RIGHT,186,234,DATA_NEXT);
    if(!ui.data_page[kind]) lv_obj_add_state(previous,LV_STATE_DISABLED);
    if(first+3>=data->count) lv_obj_add_state(next,LV_STATE_DISABLED);
    snprintf(text,sizeof(text),"%u / %u",ui.data_page[kind]+1,(data->count+2)/3);
    centered(label(lv_scr_act(),text,57,232,126,&gbs_number_16,TEXT));
    snprintf(text,sizeof(text),data->total>data->count?"最近 %u 条 / 共 %u 条":"共 %u 条",data->count,data->total);
    centered(label(lv_scr_act(),text,53,254,134,&gbs_font_10,MUTED));
}

static void schedule_detail(void)
{
    const gbs_ui_schedule_t *plan=&ui.selected_schedule;
    header("计划详情",false);
    label(lv_scr_act(),ui.selected_stale?"旧记录 · 返回列表刷新":"计划时间 · 按机器设置",12,46,216,&gbs_font_12,ui.selected_stale?WARN:MUTED);
    const unsigned minutes[]={plan->on_minutes,plan->off_minutes};
    for(unsigned i=0;i<2;++i) {
        lv_obj_t *card=box(lv_scr_act(),10+113*i,72,107,75,SURFACE);
        centered(label(card,i?"结束":"开始",3,9,101,&gbs_font_12,MUTED));
        char text[12]; snprintf(text,sizeof(text),"%02u:%02u",minutes[i]/60,minutes[i]%60);
        centered(label(card,text,3,31,101,&gbs_number_28,TEXT));
    }
    char days[64]; days_text(plan->days,days,sizeof(days));
    label(lv_scr_act(),days,12,160,216,&gbs_font_14_bold,TEXT);
    rule(lv_scr_act(),12,185,216,1);
    label(lv_scr_act(),"计划状态",12,197,126,&gbs_font_12,MUTED);
    label(lv_scr_act(),plan->enabled?"启用":"关闭",174,197,54,&gbs_font_12_bold,plan->enabled?GREEN:MUTED);
    label(lv_scr_act(),"蒸汽锅炉",12,221,126,&gbs_font_12,MUTED);
    label(lv_scr_act(),plan->steam?"开启":"关闭",174,221,54,&gbs_font_12_bold,TEXT);
    if(ui.data[GBS_DATA_SCHEDULE].editable&&!ui.selected_stale&&plan->id[0]) bottom("编辑计划",GBS_PAGE_SCHEDULE_EDIT,true);
    else centered(label(lv_scr_act(),"当前只读 · 刷新后重试",12,253,216,&gbs_font_12,MUTED));
}

static void coffee_detail(void)
{
    const gbs_ui_coffee_t *coffee=&ui.selected_coffee; char text[96];
    header("出杯详情",false);
    snprintf(text,sizeof(text),"%.10s  %.8s UTC",coffee->time,coffee->time+11);
    centered(label(lv_scr_act(),text,10,48,220,&gbs_font_10,MUTED));
    lv_obj_t *card=box(lv_scr_act(),10,74,220,88,SURFACE);
    centered(label(card,"萃取用时",10,10,200,&gbs_font_12,MUTED));
    if(isfinite(coffee->seconds)) snprintf(text,sizeof(text),"%.1f s",coffee->seconds);
    else snprintf(text,sizeof(text),"—");
    centered(label(card,text,5,33,210,&gbs_number_36,TEXT));
    label(lv_scr_act(),"目标温度",12,177,118,&gbs_font_12,MUTED);
    if(isfinite(coffee->temperature)) snprintf(text,sizeof(text),"%.1f°C",coffee->temperature);
    else snprintf(text,sizeof(text),"—");
    label(lv_scr_act(),text,142,173,86,&gbs_number_20,TEXT);
    label(lv_scr_act(),"设定剂量值",12,207,118,&gbs_font_12,MUTED);
    if(isfinite(coffee->dose)) snprintf(text,sizeof(text),"%.1f",coffee->dose);
    else snprintf(text,sizeof(text),"—");
    label(lv_scr_act(),text,142,203,86,coffee->dose>=10000?&gbs_font_14:&gbs_number_20,TEXT);
    snprintf(text,sizeof(text),"%s · %s",ui.selected_stale?"旧记录":"云端记录",choice_label(GBS_ACTION_BBW_MODE,coffee->mode));
    centered(label(lv_scr_act(),text,10,238,220,&gbs_font_10,ui.selected_stale?WARN:MUTED));
    centered(label(lv_scr_act(),"剂量值非实测重量 · 联动结果见记录",10,258,220,&gbs_font_10,MUTED));
}

static void plan_confirmation(void)
{
    const gbs_ui_schedule_t *plan=&ui.schedule_draft; char text[80];
    header("确认定时计划",false);
    icon(lv_scr_act(),GBS_ICON_CALENDAR,103,48,34,GOLD);
    snprintf(text,sizeof(text),"%02u:%02u - %02u:%02u",plan->on_minutes/60,plan->on_minutes%60,plan->off_minutes/60,plan->off_minutes%60);
    centered(label(lv_scr_act(),text,10,94,220,&gbs_number_28,TEXT));
    days_text(plan->days,text,sizeof(text)); centered(label(lv_scr_act(),text,12,135,216,&gbs_font_14,TEXT));
    snprintf(text,sizeof(text),"计划%s · 蒸汽%s",plan->enabled?"启用":"关闭",plan->steam?"开启":"关闭");
    centered(label(lv_scr_act(),text,12,164,216,&gbs_font_12,MUTED));
    centered(label(lv_scr_act(),"计划会在设定时间控制加热",12,195,216,&gbs_font_12,WARN));
    button(lv_scr_act(),"返回编辑",12,232,104,44,BACK,false);
    button(lv_scr_act(),"确认保存",124,232,104,44,PLAN_SEND,true);
}

static void schedule_editor(void)
{
    gbs_ui_schedule_t *plan=&ui.schedule_draft; char text[80];
    if(ui.schedule_submitted) {
        header("计划保存",false);
        icon(lv_scr_act(),ui.control==GBS_UI_CONTROL_CONFIRMED?GBS_ICON_CIRCLE_CHECK_FILLED:GBS_ICON_CLOCK,96,62,48,
            ui.control==GBS_UI_CONTROL_CONFIRMED?GREEN:GOLD);
        const char *status=ui.control==GBS_UI_CONTROL_PENDING?"正在保存":ui.control==GBS_UI_CONTROL_CONFIRMED?"已确认保存":
            ui.control==GBS_UI_CONTROL_FAILED?"保存未完成":"结果未确认";
        centered(label(lv_scr_act(),status,10,128,220,&gbs_font_20_bold,TEXT));
        centered(label(lv_scr_act(),ui.control==GBS_UI_CONTROL_CONFIRMED?"已读取并核对机器计划":
            ui.control==GBS_UI_CONTROL_PENDING?"正在等待计划回读":"请先刷新计划再继续编辑",12,173,216,&gbs_font_12,MUTED));
        bottom("返回计划",GBS_PAGE_SCHEDULE,false); return;
    }
    if(ui.page==GBS_PAGE_SCHEDULE_ON||ui.page==GBS_PAGE_SCHEDULE_OFF) {
        unsigned minutes=ui.page==GBS_PAGE_SCHEDULE_ON?plan->on_minutes:plan->off_minutes;
        header(ui.page==GBS_PAGE_SCHEDULE_ON?"开始时间":"结束时间",false);
        centered(label(lv_scr_act(),"按机器设置的时间",12,52,216,&gbs_font_12,MUTED));
        centered(label(lv_scr_act(),"小时",12,88,100,&gbs_font_12,MUTED));
        centered(label(lv_scr_act(),"分钟",128,88,100,&gbs_font_12,MUTED));
        snprintf(text,sizeof(text),"%02u",minutes/60); centered(label(lv_scr_act(),text,12,112,100,&gbs_number_40,TEXT));
        snprintf(text,sizeof(text),"%02u",minutes%60); centered(label(lv_scr_act(),text,128,112,100,&gbs_number_40,TEXT));
        centered(label(lv_scr_act(),":",109,112,22,&gbs_number_40,MUTED));
        button(lv_scr_act(),"−",12,170,44,44,HOUR_MINUS,false); button(lv_scr_act(),"+",68,170,44,44,HOUR_PLUS,false);
        button(lv_scr_act(),"−",128,170,44,44,MINUTE_MINUS,false); button(lv_scr_act(),"+",184,170,44,44,MINUTE_PLUS,false);
        bottom("完成",BACK,true); return;
    }
    if(ui.page==GBS_PAGE_SCHEDULE_DAYS) {
        header("重复星期",false);
        centered(label(lv_scr_act(),"选择计划生效的日期",12,56,216,&gbs_font_12,MUTED));
        const char *days[]={"周一","周二","周三","周四","周五","周六","周日"};
        for(unsigned i=0;i<7;++i) button(lv_scr_act(),days[i],10+56*(i%4),94+61*(i/4),52,50,PLAN_DAY+i,(plan->days&(1u<<i))!=0);
        bottom("完成",BACK,true); return;
    }
    header("编辑计划",false);
    unsigned minutes[]={plan->on_minutes,plan->off_minutes};
    for(unsigned i=0;i<2;++i) {
        lv_obj_t *card=button(lv_scr_act(),"",10+113*i,48,107,55,i?GBS_PAGE_SCHEDULE_OFF:GBS_PAGE_SCHEDULE_ON,false);
        label(card,i?"结束":"开始",10,5,87,&gbs_font_10,MUTED);
        snprintf(text,sizeof(text),"%02u:%02u",minutes[i]/60,minutes[i]%60);
        label(card,text,10,23,87,&gbs_number_24,TEXT);
    }
    lv_obj_t *days=button(lv_scr_act(),"",10,111,220,44,GBS_PAGE_SCHEDULE_DAYS,false);
    days_text(plan->days,text,sizeof(text)); label(days,text,10,13,188,&gbs_font_12_bold,TEXT);
    icon(days,GBS_ICON_CHEVRON_RIGHT,197,13,18,MUTED);
    for(unsigned i=0;i<2;++i) {
        bool enabled=i?plan->steam:plan->enabled;
        lv_obj_t *card=button(lv_scr_act(),"",10+113*i,163,107,52,i?PLAN_STEAM:PLAN_ENABLED,false);
        label(card,i?"蒸汽锅炉":"计划状态",10,5,87,&gbs_font_10,MUTED);
        label(card,enabled?"开启":"关闭",10,25,87,&gbs_font_14_bold,enabled?GREEN:MUTED);
    }
    bool valid=!plan->enabled||(plan->days&&plan->on_minutes!=plan->off_minutes);
    centered(label(lv_scr_act(),!valid?"请选择日期和不同的起止时间":ui.conflict?"计划已变化，请重新打开":"保存后按此计划运行",
        10,218,220,&gbs_font_10,valid?MUTED:WARN));
    bottom("保存计划",PLAN_APPLY,true);
    lv_obj_t *save=lv_obj_get_child(lv_scr_act(),lv_obj_get_child_cnt(lv_scr_act())-1);
    if(!ui.dirty||!valid||ui.conflict||!ui.data[GBS_DATA_SCHEDULE].editable||ui.data[GBS_DATA_SCHEDULE].state!=GBS_DATA_READY||ui.control==GBS_UI_CONTROL_PENDING)
        lv_obj_add_state(save,LV_STATE_DISABLED);
}

/* Refresh only the exact local selector; computed values may come from another state. */
static void refresh_color(lv_obj_t *obj,lv_style_prop_t prop,lv_color_t color,lv_style_selector_t selector)
{
#ifdef HUB_AMOLED_216
    lv_style_value_t old;
    if(lv_obj_get_local_style_prop(obj,prop,&old,selector)==LV_STYLE_RES_FOUND&&old.color.full==color.full) return;
#endif
    lv_obj_set_local_style_prop(obj,prop,(lv_style_value_t){.color=color},selector);
}

static void refresh_num(lv_obj_t *obj,lv_style_prop_t prop,int32_t number,lv_style_selector_t selector)
{
#ifdef HUB_AMOLED_216
    lv_style_value_t old;
    if(lv_obj_get_local_style_prop(obj,prop,&old,selector)==LV_STYLE_RES_FOUND&&old.num==number) return;
#endif
    lv_obj_set_local_style_prop(obj,prop,(lv_style_value_t){.num=number},selector);
}

#ifdef HUB_AMOLED_216
static void refresh_font(lv_obj_t *obj,const lv_font_t *font,lv_style_selector_t selector)
{
    lv_style_value_t old;
    if(lv_obj_get_local_style_prop(obj,LV_STYLE_TEXT_FONT,&old,selector)==LV_STYLE_RES_FOUND&&old.ptr==font) return;
    lv_obj_set_local_style_prop(obj,LV_STYLE_TEXT_FONT,(lv_style_value_t){.ptr=font},selector);
}

static void refresh_hidden(lv_obj_t *obj,bool hidden)
{
    if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)==hidden) return;
    if(hidden) lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(obj,LV_OBJ_FLAG_HIDDEN);
}
#endif

static void refresh_label(lv_obj_t *obj,const char *text)
{
#ifdef HUB_AMOLED_216
    if(!strcmp(lv_label_get_text(obj),text)) return;
#endif
    lv_label_set_text(obj,text);
}

#ifdef HUB_AMOLED_216
#include "gbs_ui_amoled.inc"
#endif

static void render_content(void)
{
    memset(ui.fields, 0, sizeof(ui.fields)); memset(ui.presets, 0, sizeof(ui.presets));
    ui.apply = ui.body = ui.ring = ui.steam_toggle = ui.confirm_button = NULL;
    ui.scale_shape=ui.battery_shape=ui.battery_fill=ui.phase_icon=ui.grinder_icon=ui.cloud_icon=ui.preference_toast=NULL;
    ui.battery_clip=ui.battery_terminal=ui.battery_charging=ui.path_icon=ui.path_arrow=ui.path_rule=ui.grinder_dot=NULL;
    memset(ui.preset_units,0,sizeof(ui.preset_units));
#ifdef HUB_AMOLED_216
    am_top=false;
#endif
    memset(ui.choices,0,sizeof(ui.choices));
    memset(ui.mode_radio, 0, sizeof(ui.mode_radio));
    memset(ui.connection_dots, 0, sizeof(ui.connection_dots));
    lv_obj_t *screen = lv_scr_act(); lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, &gbs_font_14, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    if (ui.discard) {
        header("未应用的修改", false);
        message("放弃这次修改？", "设备当前设置没有改变。\n已提交的请求仍会继续处理。");
        button(screen, "继续编辑", 12, 224, 104, 44, KEEP, false);
        button(screen, "放弃修改", 124, 224, 104, 44, DISCARD, true);
        return;
    }
    if (ui.confirm) {
        if(plan_editor()) { plan_confirmation(); return; }
        if(parameter()) {
            const struct parameter *p=parameter(); char text[128],selected[64];
            header("确认机器设置",false);
            parameter_text(p->action,ui.requested,selected,sizeof(selected));
            icon(screen,p->action==GBS_ACTION_POWER?GBS_ICON_POWER:GBS_ICON_CLOCK,103,52,34,GOLD);
            centered(label(screen,p->title,12,98,216,&gbs_font_12,MUTED));
            centered(label(screen,selected,12,121,216,&gbs_font_20_bold,TEXT));
            if(p->action==GBS_ACTION_POWER)
                snprintf(text,sizeof(text),"%s",ui.requested?"机器将开始加热，请确认已准备好。":"机器将进入待机，请确认萃取已结束。");
            else snprintf(text,sizeof(text),"确认后更新自动待机设置，\n其它待机参数保持当前值。");
            centered(label(screen,text,18,158,204,&gbs_font_12,MUTED));
            centered(field(screen,F_CONTROL,12,202,216,&gbs_font_12,WARN));
            button(screen,"返回编辑",12,232,104,44,BACK,false);
            ui.confirm_button=button(screen,"确认应用",124,232,104,44,CONFIRM,true);
            refresh(); return;
        }
        header("确认联动模式", false);
        message(ui.requested ? "启用联动" : "切换为仅监测", ui.requested ?
            "请确认电脑和其它联动客户端已停止。\n启用后接收新任务并回传结果。" :
            "停止接收联动任务。\n本杯会在本机结束，不代表磨豆机已收到取消。");
        button(screen, "返回", 12, 224, 104, 44, BACK, false);
        button(screen, "确认切换", 124, 224, 104, 44, CONFIRM, true);
        return;
    }
    if (ui.page == GBS_PAGE_HOME) {
#ifdef HUB_AMOLED_216
        amoled_home();
#else
        home();
#endif
        refresh(); return;
    }
#ifdef HUB_AMOLED_216
    if(amoled_settings_page()||amoled_details_build()){refresh();return;}
    if(ui.page==GBS_PAGE_STEAM){amoled_edit();refresh();return;}
    if(ui.page==GBS_PAGE_SCALE||ui.page==GBS_PAGE_DEVICE_BATTERY||ui.page==GBS_PAGE_ORIENTATION) {
        amoled_device_page(); refresh(); return;
    }
#endif
    if(parameter()&&choice_label(parameter()->action,0)) { choice_page(); refresh(); return; }
    if (editor() && ui.page != GBS_PAGE_STEAM) {
#ifdef HUB_AMOLED_216
        amoled_edit();
#else
        edit_page();
#endif
        refresh(); return;
    }
    switch (ui.page) {
    case GBS_PAGE_LINK:
    case GBS_PAGE_TASK:
        linked_page();
        break;
    case GBS_PAGE_RECORD: {
        header("联动记录", false);
        centered(label(screen, "最近一次", 12, 45, 216, &gbs_font_12, MUTED));
        icon(screen, ui.bridge.successes ? GBS_ICON_CIRCLE_CHECK_FILLED : GBS_ICON_CLOCK, 105, 62, 30, GREEN);
        centered(label(screen, ui.bridge.successes ? "已接收" : "暂无记录", 12, 97, 216, &gbs_font_14_bold, TEXT));
        metric_pair(122, false);
        lv_obj_t *card = box(screen, 12, 182, 216, 31, SURFACE);
        lv_obj_set_style_border_width(card, 1, 0); lv_obj_set_style_border_color(card, lv_color_hex(LINE), 0);
        icon(card, GBS_ICON_LINK, 12, 5, 21, GREEN);
        label(card, ui.bridge.successes ? "磨豆机已确认接收" : "等待第一杯确认结果", 43, 6, 168, &gbs_font_12_bold, TEXT);
        centered(label(screen, "仅展示最近结果", 12, 216, 216, &gbs_font_12, MUTED));
        bottom("查看详情", GBS_PAGE_CONNECTIONS, true);
        break;
    }
    case GBS_PAGE_SETTINGS:
        header("设置", false); list_body();
        row("咖啡机设置", NULL, 0, GBS_PAGE_MACHINE);
        row("联动与记录", NULL, 1, GBS_PAGE_LINK);
        row("连接与系统", NULL, 2, GBS_PAGE_CONNECTIONS);
        row("屏幕设置", NULL, 3, GBS_PAGE_DISPLAY);
        row("出杯记录", NULL, 4, GBS_PAGE_COFFEES);
        #ifdef HUB_AMOLED_216
        row("A 秤", NULL, 5, GBS_PAGE_SCALE);
        row("设备电量", NULL, 6, GBS_PAGE_DEVICE_BATTERY);
        row("屏幕方向", NULL, 7, GBS_PAGE_ORIENTATION);
        row("提示音", NULL, 8, GBS_PAGE_SOUND);
        row("关于", "版本和上游来源", 9, GBS_PAGE_ABOUT);
#else
        row("关于", "版本和上游来源", 5, GBS_PAGE_ABOUT);
#endif
        break;
    case GBS_PAGE_MACHINE:
        header("咖啡机设置", false); list_body();
        row("锅炉温度", NULL, 0, GBS_PAGE_COFFEE);
        row("蒸汽", NULL, 1, GBS_PAGE_STEAM);
        row("出杯量", NULL, 2, GBS_PAGE_WEIGHT);
        row("BBW 剂量与模式", NULL, 3, GBS_PAGE_BBW);
        row("预浸", NULL, 4, GBS_PAGE_PREBREW);
        row("机器电源", NULL, 5, GBS_PAGE_POWER);
        row("自动待机", NULL, 6, GBS_PAGE_STANDBY);
        row("清洗引导", NULL, 7, GBS_PAGE_CLEANING);
        row("定时计划", "只读", 8, GBS_PAGE_SCHEDULE);
        break;
    case GBS_PAGE_BBW: {
        const gbs_ui_page_t pages[]={GBS_PAGE_DOSE1,GBS_PAGE_DOSE2,GBS_PAGE_BBW_MODE};
        parameter_summary("BBW 剂量与模式",pages); break;
    }
    case GBS_PAGE_PREBREW: {
        const gbs_ui_page_t pages[]={GBS_PAGE_PREBREW_MODE,GBS_PAGE_PREBREW_ON,GBS_PAGE_PREBREW_OFF};
        parameter_summary("预浸",pages); break;
    }
    case GBS_PAGE_STANDBY: {
        const gbs_ui_page_t pages[]={GBS_PAGE_STANDBY_ENABLED,GBS_PAGE_STANDBY_MINUTES,GBS_PAGE_STANDBY_MODE};
        parameter_summary("自动待机",pages); break;
    }
    case GBS_PAGE_MODE:
        header("联动模式", false);
        field(screen, F_MODE, 12, 51, 216, &gbs_font_12, MUTED);
        for (int i = 0; i < 2; ++i) {
            lv_obj_t *card = button(screen, "", 10, 78 + i * 58, 220, 52, i ? MODE_ACTIVE : MODE_MONITOR, false);
            label(card, i ? "联动" : "仅监测", 12, 7, 166, &gbs_font_16_bold, TEXT);
            label(card, i ? "接收任务，回传萃取结果" : "读取状态，不回传结果", 12, 31, 193, &gbs_font_12, MUTED);
            ui.mode_radio[i] = box(card, 193, 17, 17, 17, SURFACE);
            lv_obj_set_style_radius(ui.mode_radio[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(ui.mode_radio[i], 1, 0);
            lv_obj_set_style_border_color(ui.mode_radio[i], lv_color_hex(ui.mode_draft == (i != 0) ? GOLD : MUTED), 0);
            if (ui.mode_draft == (i != 0)) {
                lv_obj_t *dot = box(ui.mode_radio[i], 4, 4, 9, 9, GOLD); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            }
        }
        icon(screen, GBS_ICON_ALERT_CIRCLE, 12, 205, 19, WARN);
        label(screen, "先停止其它 GbS 客户端", 39, 199, 190, &gbs_font_12, MUTED);
        field(screen, F_CONTROL, 39, 216, 190, &gbs_font_12, MUTED);
        bottom("确认切换", CONFIRM, true);
        break;
    case GBS_PAGE_CONNECTIONS: {
        header("连接与系统", false);
        const char *names[] = {"Wi-Fi", "本机服务", "厂商云端", "磨豆机"};
        const gbs_icon_t symbols[] = {GBS_ICON_WIFI, GBS_ICON_LINK, GBS_ICON_CLOUD, GBS_ICON_BLENDER};
        unsigned ids[] = {F_WIFI, F_HUB, F_CLOUD, F_GRINDER};
        for (int i = 0; i < 4; ++i) {
            lv_obj_t *card = button(screen, "", 10, 43 + 46 * i, 220, 43, GBS_PAGE_CONNECTION, false);
            icon(card, symbols[i], 9, 10, 23, TEXT);
            label(card, names[i], 41, 12, 106, &gbs_font_12_bold, TEXT);
            field(card, ids[i], 146, 13, 62, &gbs_font_12, MUTED);
            ui.connection_dots[i] = box(card, 132, 19, 7, 7, GREEN);
            lv_obj_set_style_radius(ui.connection_dots[i], LV_RADIUS_CIRCLE, 0);
            icon(card, GBS_ICON_CHEVRON_RIGHT, 203, 14, 15, MUTED);
        }
        rule(screen, 10, 237, 220, 1);
        label(screen, "下次尝试", 12, 252, 78, &gbs_font_12, MUTED);
        field(screen, F_TIME, 91, 247, 110, &gbs_number_20, WARN);
        icon_button(screen, GBS_ICON_SETTINGS, 191, 238, GBS_PAGE_DIAGNOSTICS);
        break;
    }
    case GBS_PAGE_CONNECTION:
        header("连接详情", false);
        field(screen, F_WIFI, 18, 66, 204, &gbs_font_14, TEXT);
        field(screen, F_GRINDER, 18, 102, 204, &gbs_font_14, TEXT);
        field(screen, F_CLOUD, 18, 138, 204, &gbs_font_14, TEXT);
        field(screen, F_DETAIL, 18, 180, 204, &gbs_font_12, MUTED);
        break;
    case GBS_PAGE_DIAGNOSTICS:
        header("诊断", false);
        field(screen, F_MEMORY, 18, 65, 204, &gbs_font_14, TEXT);
        field(screen, F_STAGE, 18, 134, 204, &gbs_font_14, GOLD);
        label(screen, "详细日志可通过局域网查看。\n此页不展示身份与凭据。", 18, 179, 204, &gbs_font_12, MUTED);
        break;
    case GBS_PAGE_DISPLAY: {
        header("屏幕设置", false);
        label(screen, "亮度", 12, 56, 110, &gbs_font_16_bold, TEXT);
        field(screen, F_BRIGHTNESS, 172, 52, 60, &gbs_number_24, TEXT);
        icon(screen, GBS_ICON_SUN, 12, 91, 18, MUTED); icon(screen, GBS_ICON_SUN, 209, 89, 22, MUTED);
        lv_obj_t *slider = lv_slider_create(screen);
        lv_obj_remove_style_all(slider); lv_obj_set_pos(slider, 42, 98); lv_obj_set_size(slider, 154, 7);
        lv_obj_set_ext_click_area(slider, 18); lv_slider_set_range(slider, 25, 100); lv_slider_set_value(slider, ui.brightness_draft, LV_ANIM_OFF);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN); lv_obj_set_style_bg_color(slider, lv_color_hex(LINE), LV_PART_MAIN);
        lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR); lv_obj_set_style_bg_color(slider, lv_color_hex(GOLD), LV_PART_INDICATOR);
        lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB); lv_obj_set_style_bg_color(slider, lv_color_hex(GOLD), LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB); lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB); lv_obj_set_style_border_color(slider, lv_color_hex(TEXT), LV_PART_KNOB);
        lv_obj_add_event_cb(slider, event, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)BRIGHTNESS);
        for (int i = 0; i < 2; ++i) {
            lv_obj_t *card = button(screen, "", 10, 120 + i * 48, 220, 44, DIM, false);
            label(card, i ? "空闲熄屏" : "空闲调暗", 12, 13, 126, &gbs_font_14_bold, TEXT);
            label(card, ui.dim_draft ? (i ? "120 秒" : "60 秒") : "关闭", 158, 14, 54, &gbs_font_12_bold, TEXT);
        }
        field(screen, F_DETAIL, 12, 218, 216, &gbs_font_12, MUTED);
        set(F_DETAIL, "首次触摸仅唤醒");
        bottom("保存", DISPLAY_SAVE, true);
        break;
    }
    case GBS_PAGE_STEAM: {
        header("蒸汽", false);
        icon(screen, GBS_ICON_CLOUD_RAIN, 51, 61, 25, TEXT);
        field(screen, F_STEAM, 84, 53, 82, &gbs_number_40, TEXT);
        label(screen, "°C", 172, 73, 28, &gbs_font_12, TEXT);
        centered(label(screen, "当前温度", 60, 94, 151, &gbs_font_12, MUTED));
        rule(screen, 10, 118, 220, 1);
        lv_obj_t *enabled = box(screen, 10, 127, 220, 40, SURFACE);
        label(enabled, "蒸汽锅炉", 12, 10, 135, &gbs_font_14_bold, TEXT);
        if (ui.machine.steam_enabled_known) {
            lv_obj_t *toggle = ui.steam_toggle = lv_switch_create(enabled);
            lv_obj_set_pos(toggle, 169, 8); lv_obj_set_size(toggle, 40, 24);
            lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
            lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_pad_all(toggle, -3, LV_PART_KNOB);
            lv_obj_set_style_bg_color(toggle, lv_color_hex(LINE), LV_PART_MAIN);
            lv_obj_set_style_bg_color(toggle, lv_color_hex(GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(toggle, lv_color_hex(TEXT), LV_PART_KNOB);
            if (ui.machine.steam_enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
            if (!ui.machine.writable || ui.control == GBS_UI_CONTROL_PENDING) lv_obj_add_state(toggle, LV_STATE_DISABLED);
            lv_obj_add_event_cb(toggle, event, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)STEAM_TOGGLE);
        } else label(enabled, "—", 176, 9, 30, &gbs_font_14, MUTED);
        label(screen, "蒸汽档位", 10, 175, 120, &gbs_font_12, MUTED);
        field(screen, F_CONTROL, 109, 175, 121, &gbs_font_10, WARN);
        for (int i = 0; i < 3; ++i) {
            const int levels[] = {126, 128, 131}; char caption[16]; snprintf(caption, sizeof(caption), "%d 档", i + 1);
            lv_obj_t *b = button(screen, caption, 10 + 75 * i, 190, 70, 38, 300 + i, ui.draft == levels[i]);
            lv_obj_set_ext_click_area(b, 3);
            if (!(ui.machine.writable & (1u << GBS_ACTION_STEAM)) || ui.machine.stale) {
                lv_obj_add_state(b, LV_STATE_DISABLED);
                lv_obj_set_style_opa(b,LV_OPA_COVER,LV_STATE_DISABLED);
                lv_obj_set_style_bg_color(b,lv_color_hex(SURFACE),LV_STATE_DISABLED);
                lv_obj_set_style_bg_grad_color(b,lv_color_hex(0x1a1e1a),LV_STATE_DISABLED);
                lv_obj_set_style_border_color(b,lv_color_hex(ui.draft==levels[i]?GOLD:LINE),LV_STATE_DISABLED);
                lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(ui.draft==levels[i]?GOLD:MUTED),0);
            }
        }
        bottom("应用档位", APPLY, true);
        break;
    }
    case GBS_PAGE_ABOUT:
        header("关于与更新", false);
        message("LamaBuddy / Integrated", "咖啡机与磨豆机联动\n升级与诊断可通过局域网。\n使用已授权的维护工具。\n此页不执行更新或重置。");
        break;
    case GBS_PAGE_SETUP:
        header("配置与恢复", false);
        message("需要电脑完成配置", "使用项目的配对导入工具。\n身份拒绝时先停止其它客户端，再检查原有配对。\n账本异常请保留原数据。\n此页不清除身份或重新配对。");
        break;
    case GBS_PAGE_SCHEDULE: case GBS_PAGE_COFFEES:
        collection_page(); break;
    case GBS_PAGE_SCHEDULE_DETAIL:
        schedule_detail(); break;
    case GBS_PAGE_SCHEDULE_EDIT: case GBS_PAGE_SCHEDULE_ON: case GBS_PAGE_SCHEDULE_OFF: case GBS_PAGE_SCHEDULE_DAYS:
        schedule_editor(); break;
    case GBS_PAGE_COFFEE_DETAIL:
        coffee_detail(); break;
    case GBS_PAGE_CLEANING:
        header("清洗引导", false);
        message("按照咖啡机说明操作", "开始前移开杯具，确认机器状态。\n当前屏幕不执行清洗命令，请在咖啡机上操作。");
        break;
    default: break;
    }
    refresh();
}

#ifdef HUB_AMOLED_216
static void amoled_fallback_style(lv_obj_t *parent,bool disabled)
{
    for(unsigned i=0;i<lv_obj_get_child_cnt(parent);++i) {
        lv_obj_t *child=lv_obj_get_child(parent,i);
        bool child_disabled=disabled || lv_obj_has_state(child,LV_STATE_DISABLED);
        lv_obj_set_style_bg_grad_dir(child,LV_GRAD_DIR_NONE,0);
        lv_obj_set_style_bg_grad_dir(child,LV_GRAD_DIR_NONE,LV_STATE_DISABLED);
        lv_coord_t radius=lv_obj_get_style_radius(child,0);
        if(radius>0 && radius!=LV_RADIUS_CIRCLE) lv_obj_set_style_radius(child,12,0);
        if(lv_obj_has_flag(child,LV_OBJ_FLAG_CLICKABLE))
            lv_obj_set_style_opa(child,LV_OPA_COVER,LV_STATE_DISABLED);
        if(lv_obj_check_type(child,&lv_label_class)) {
            const lv_font_t *font=lv_obj_get_style_text_font(child,0);
            if(font==&gbs_font_18 || font==&gbs_font_20 || font==&gbs_font_20_bold)
                lv_obj_set_style_text_font(child,&gbs_font_20,0);
            else if(font==&gbs_font_24 || font==&gbs_font_24_bold ||
                    font==&gbs_font_28_bold || font==&gbs_font_34_bold)
                lv_obj_set_style_text_font(child,&gbs_font_23,0);
            if(child_disabled) lv_obj_set_style_text_color(child,lv_color_hex(MUTED),0);
        } else if(lv_obj_check_type(child,&lv_img_class)) {
            struct icon_mask *mask=lv_obj_get_user_data(child);
            if(mask) {
                int before=mask->width;
                int size=mask->name==GBS_ICON_CHEVRON_LEFT || mask->name==GBS_ICON_CHEVRON_RIGHT ? 24 :
                    before<=48 && before>32 ? 32 : before;
                if(size!=before) {
                    lv_coord_t x=lv_obj_get_x(child),y=lv_obj_get_y(child);
                    icon_source(child,mask->name,size);
                    lv_obj_set_pos(child,x+(before-size)/2,y+(before-size)/2);
                }
            }
        } else if(child_disabled && lv_obj_get_style_bg_opa(child,0)>LV_OPA_TRANSP) {
            lv_obj_set_style_bg_color(child,lv_color_hex(SURFACE),0);
            lv_obj_set_style_border_color(child,lv_color_hex(LINE),0);
        }
        amoled_fallback_style(child,child_disabled);
    }
}
#endif

static void render(void)
{
    ui.native_layout=false;
#ifdef HUB_AMOLED_216
    fallback_heading=fallback_back=NULL;
#endif
    render_content();
#ifdef HUB_AMOLED_216
    if(!ui.native_layout) {
        lv_obj_update_layout(lv_scr_act());
        amoled_resize_children(lv_scr_act());
        lv_obj_update_layout(lv_scr_act());
        amoled_fallback_style(lv_scr_act(),false);
        if(fallback_heading) {
            lv_obj_set_pos(fallback_heading,92,29);
            lv_obj_set_style_text_font(fallback_heading,&gbs_font_25_bold,0);
            lv_obj_set_width(fallback_heading,lv_obj_get_width(fallback_heading)>300?350:274);
        }
        if(fallback_back) {
            lv_obj_set_pos(fallback_back,16,14);lv_obj_set_size(fallback_back,64,64);
            lv_obj_t *chevron=lv_obj_get_child(fallback_back,1);
            lv_obj_set_pos(chevron,20,20);
        }
        if(ui.body) {lv_obj_set_pos(ui.body,28,92);lv_obj_set_size(ui.body,424,372);}
    }
    if(ui.preference_notice_until>ui.now) {
        ui.preference_toast=box(lv_scr_act(),28,396,424,68,SURFACE);
        lv_obj_add_flag(ui.preference_toast,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(ui.preference_toast,1,0);
        lv_obj_set_style_border_color(ui.preference_toast,lv_color_hex(GOLD),0);
        lv_obj_t *notice=label(ui.preference_toast,ui.preference_notice,8,0,408,&gbs_font_24,TEXT);
        centered(notice); lv_obj_center(notice);
    }
#endif
}

static void set(unsigned field_id, const char *text)
{
    if (ui.fields[field_id] && strcmp(lv_label_get_text(ui.fields[field_id]), text)) lv_label_set_text(ui.fields[field_id], text);
}

static void value(unsigned id, double number, const char *format)
{
    char text[80];
    if (isfinite(number)) snprintf(text, sizeof(text), format, number); else strcpy(text, "—");
    set(id, text);
}

static void refresh(void)
{
    bool native_home=false,native_editor=false;
#ifdef HUB_AMOLED_216
    native_home=ui.page==GBS_PAGE_HOME&&ui.native_layout;
    native_editor=editor()&&ui.fields[F_EDITOR_CURRENT];
#endif
    static const char *titles[] = {"准备中", "账本需要处理", "身份需要处理", "正在校时", "恢复任务基线",
        "等待结果", "回传中", "取消处理中", "回传未确认", "联动已中断", "联动已结束", "任务已结束",
        "Wi-Fi 离线", "磨豆机离线", "云端异常", "仅监测", "已就绪", "已接收", "萃取记录异常"};
    static const char *details[] = {"恢复配置与连接", "请保留原数据，检查存储", "请检查原配对与其它客户端", "校时完成后恢复联动", "旧任务不会回放",
        "包含萃取与云端同步", "等待磨豆机明确接收", "等待取消结果确认", "可能已送达，不自动重发", "不会回放重启前的杯", "请查看连接与最近记录", "可能已拒绝或任务消失",
        "网络恢复后继续检查", "自动重连，结果保持不变", "云端恢复后继续检查", "仍读取状态，不回传结果", "等待新的磨豆任务", "磨豆机已确认接收结果", "请检查电子秤与萃取记录"};
    gbs_ui_stage_t stage = gbs_ui_stage(&ui.bridge);
    bool unconfigured=!strcmp(ui.bridge.phase,"configuration_required");
    set(F_STAGE, unconfigured?"联动待配置":titles[stage]);
    if (ui.fields[F_STAGE]) refresh_color(ui.fields[F_STAGE],LV_STYLE_TEXT_COLOR, lv_color_hex(ui.page == GBS_PAGE_HOME ? MUTED : TEXT), 0);
    if (ui.page != GBS_PAGE_DISPLAY && ui.page != GBS_PAGE_BRIGHTNESS && ui.page != GBS_PAGE_SOUND) set(F_DETAIL, unconfigured?"咖啡机功能可独立使用":details[stage]);
    const char *caption = "";
    int64_t deadline = 0;
    if (ui.bridge.job) { deadline = ui.bridge.pending ? ui.bridge.answer_deadline_ms : ui.bridge.job_deadline_ms;
        caption = ui.bridge.pending ? (ui.bridge.canceling ? "取消剩余" : "回传剩余") : "接收剩余"; }
    else if (stage == GBS_UI_GRINDER) { deadline = ui.bridge.retry_at_ms; caption = "下次尝试"; }
    char text[128];
    if (deadline) {
        unsigned seconds = gbs_ui_remaining(deadline, ui.now);
        snprintf(text, sizeof(text), "%02u:%02u", seconds / 60, seconds % 60);
        set(F_TIME, text);
        if (ui.ring && ui.bridge.job && (ui.page == GBS_PAGE_LINK || ui.page == GBS_PAGE_TASK)) {
            unsigned budget = ui.bridge.pending ? (ui.bridge.canceling ? 20 : 120) : 180;
            lv_arc_set_range(ui.ring, 0, budget); lv_arc_set_value(ui.ring, seconds > budget ? budget : seconds);
        }
        if (ui.bridge.job && !seconds) set(F_DETAIL, "期限已到，等待处理结果");
        else if (stage == GBS_UI_WAIT && seconds <= 30) set(F_STAGE, "即将超时");
    } else set(F_TIME, "—");
    set(F_TIMER_CAPTION, caption);
    if (ui.bridge.job) {
        if (ui.bridge.target_grams <= 0) snprintf(text, sizeof(text), "等待配方 · %.0fs", ui.bridge.target_seconds);
        else if (ui.page == GBS_PAGE_HOME) snprintf(text, sizeof(text), "目标 %.0fg · %.0fs", ui.bridge.target_grams, ui.bridge.target_seconds);
        else snprintf(text, sizeof(text), "目标 %.0fs · %.0fg", ui.bridge.target_seconds, ui.bridge.target_grams);
    }
    else snprintf(text, sizeof(text), "%s", ui.bridge.active ?
        (ui.bridge.recipe_enabled ? "配方自动同步" : "联动模式") : "仅监测模式");
    set(F_TARGET, text);
    if (stage == GBS_UI_WAIT && ui.bridge.recipe_enabled) {
        if (!strcmp(ui.bridge.recipe_phase, "applied")) {
            snprintf(text, sizeof(text), "配方 %.0fg 已同步", ui.bridge.recipe_grams);
            set(F_DETAIL, text);
        } else if (!strcmp(ui.bridge.recipe_phase, "waiting") || !strcmp(ui.bridge.recipe_phase, "reading") ||
                   !strcmp(ui.bridge.recipe_phase, "applying")) set(F_DETAIL, "正在同步配方，请稍等");
        else if (!strcmp(ui.bridge.recipe_phase, "unconfirmed")) set(F_DETAIL, "切重未确认，请检查克重");
        else set(F_DETAIL, "未自动切重，请检查克重");
    }
    if (stage == GBS_UI_UNKNOWN && (ui.page == GBS_PAGE_LINK || ui.page == GBS_PAGE_TASK)) {
        value(F_RESULT, ui.bridge.unconfirmed_seconds > 0 ? ui.bridge.unconfirmed_seconds : NAN, "%.1f");
        value(F_SECOND_RESULT, ui.bridge.unconfirmed_grams > 0 ? ui.bridge.unconfirmed_grams : NAN, "%.1f");
    } else {
        value(F_RESULT, ui.bridge.successes ? ui.bridge.result_seconds : NAN, "%.1f");
        value(F_SECOND_RESULT, ui.bridge.successes ? ui.bridge.result_grams : NAN, "%.1f");
    }
    set(F_HOME_HINT, stage == GBS_UI_READY ? "等待新任务" : stage == GBS_UI_RECEIVED ? "查看最近结果" : "查看详情");
    if(!native_home) {
        value(F_COFFEE, ui.machine.coffee, "%.1f");
        double steam_level = ui.machine.steam_target == 126 ? 1 : ui.machine.steam_target == 128 ? 2 : ui.machine.steam_target == 131 ? 3 : NAN;
        value(F_STEAM, ui.page == GBS_PAGE_HOME ? steam_level : ui.machine.steam, "%.0f");
        value(F_STEAM_CURRENT, ui.machine.steam, "当前 %.0f°");
        value(F_COFFEE_TARGET, ui.machine.coffee_target, ui.page == GBS_PAGE_MACHINE ? "%.1f°" : "目标 %.1f°");
        value(F_STEAM_TARGET, ui.machine.steam_target, ui.page == GBS_PAGE_MACHINE ? "%.0f°" : "目标 %.0f°");
    }
    value(F_WEIGHT, ui.machine.weight, ui.page == GBS_PAGE_MACHINE ? "%.0fg" : "当前 %.0fg");
    const char *controls[] = {"", "已提交，等待确认",
#ifdef HUB_AMOLED_216
        "已设定",
#else
        "已确认",
#endif
        "未能提交，请检查连接", "部分应用，请检查机器", "结果未确认，请检查机器"};
    if(!native_editor) set(F_CONTROL, ui.action == GBS_ACTION_MODE ? controls[ui.control] : "");
    if(ui.page==GBS_PAGE_BBW||ui.page==GBS_PAGE_PREBREW||ui.page==GBS_PAGE_STANDBY) {
        gbs_ui_action_t first=ui.page==GBS_PAGE_BBW?GBS_ACTION_DOSE1:ui.page==GBS_PAGE_PREBREW?GBS_ACTION_PREBREW_MODE:GBS_ACTION_STANDBY_ENABLED;
        for(unsigned i=0;i<3;++i) {
            parameter_text(first+i,gbs_ui_action_value(first+i,&ui.machine),text,sizeof(text)); set(F_PARAMETER0+i,text);
        }
        set(F_CONTROL,!ui.machine.available?"咖啡机未连接":ui.machine.stale?"数据已过期":
            (ui.action>=first&&ui.action<=first+2&&ui.control!=GBS_UI_CONTROL_IDLE)?controls[ui.control]:"点击参数进行编辑");
    }
    if(ui.page==GBS_PAGE_POWER) {
        double power=current_value();
        set(F_PARAMETER0,!isfinite(power)?"—":power==0?"待机中":power==1?"加热开启":"节能中");
    }
    if (ui.page == GBS_PAGE_HOME) {
        set(F_CONFLICT, !ui.machine.available ? "未接入" : ui.machine.stale ? "数据已过期" : "已连接");
        if (ui.control != GBS_UI_CONTROL_IDLE && ui.action == GBS_ACTION_WEIGHT)
            set(F_WEIGHT, ui.control == GBS_UI_CONTROL_PENDING ? "等待确认" : ui.control == GBS_UI_CONTROL_CONFIRMED ? "已确认" : "请检查设置");
    }
    if (editor()) {
        const struct parameter *p=parameter(); gbs_ui_action_t field_action=p->action;
        bool unresolved=field_action>=GBS_ACTION_DOSE1&&ui.action==field_action&&!ui.dirty&&
            ui.control!=GBS_UI_CONTROL_IDLE&&ui.control!=GBS_UI_CONTROL_CONFIRMED;
        if(!native_editor) value(F_DRAFT, unresolved?ui.requested:!isfinite(ui.initial)&&!ui.dirty?NAN:ui.draft, p->step<1 ? "%.1f" : "%.0f");
        const char *short_control[] = {"", "已提交 · 待确认",
#ifdef HUB_AMOLED_216
            "已设定",
#else
            "已确认",
#endif
            "应用失败", "部分应用", "结果未确认"};
        const char *warning=!ui.machine.available?"咖啡机未接入":ui.machine.stale&&!gbs_ui_can_control(field_action,&ui.machine)?"数据已过期":
            ui.conflict?"设置已变化，请返回":!(ui.machine.writable&(1u<<field_action))?"当前控制不可用":NULL;
        bool native_caption=native_editor&&
            ((gbs_ui_can_control(field_action,&ui.machine)&&!ui.conflict&&(ui.control==GBS_UI_CONTROL_IDLE||ui.action!=field_action||ui.dirty))||
             (ui.action==field_action&&ui.control==GBS_UI_CONTROL_CONFIRMED&&!ui.dirty));
        if(warning) { if(!native_caption) set(F_CONFLICT,warning); }
        else if (ui.control != GBS_UI_CONTROL_IDLE && ui.action == field_action && !ui.dirty) {
            if(!native_editor||ui.control!=GBS_UI_CONTROL_CONFIRMED) set(F_CONFLICT, short_control[ui.control]);
            if(field_action>=GBS_ACTION_DOSE1) {
                char current[64]; parameter_text(field_action,current_value(),current,sizeof(current));
                snprintf(text,sizeof(text),"当前确认值 %s",current); set(F_HOME_HINT,text);
            } else value(F_HOME_HINT, current_value(), ui.page == GBS_PAGE_WEIGHT ? "原确认值 %.0fg" : "原确认值 %.1f°");
            set(F_DETAIL, ui.control == GBS_UI_CONTROL_CONFIRMED ? "当前目标已更新" : "正在核对机器状态");
            if (!native_editor&&ui.fields[F_CONFLICT]) refresh_color(ui.fields[F_CONFLICT],LV_STYLE_TEXT_COLOR, lv_color_hex(ui.control == GBS_UI_CONTROL_CONFIRMED ? GREEN : WARN), 0);
        } else if(!native_editor&&field_action>=GBS_ACTION_DOSE1) {
            char current[64]; parameter_text(field_action,current_value(),current,sizeof(current));
            snprintf(text,sizeof(text),"当前 %s",current); set(F_CONFLICT,field_action==GBS_ACTION_POWER?"选择后确认":text);
        } else if(!native_editor) value(F_CONFLICT, current_value(), ui.page == GBS_PAGE_WEIGHT ? "当前 %.0fg" : "当前 %.1f°");
        if(ui.confirm_button) {
            set(F_CONTROL,warning?warning:"确认后生效");
            if(warning||ui.control==GBS_UI_CONTROL_PENDING) lv_obj_add_state(ui.confirm_button,LV_STATE_DISABLED);
            else lv_obj_clear_state(ui.confirm_button,LV_STATE_DISABLED);
            refresh_num(ui.confirm_button,LV_STYLE_OPA,LV_OPA_COVER,LV_STATE_DISABLED);
            refresh_color(ui.confirm_button,LV_STYLE_BG_COLOR,lv_color_hex(SURFACE),LV_STATE_DISABLED);
            refresh_color(ui.confirm_button,LV_STYLE_BG_GRAD_COLOR,lv_color_hex(SURFACE),LV_STATE_DISABLED);
            refresh_color(ui.confirm_button,LV_STYLE_BORDER_COLOR,lv_color_hex(LINE),LV_STATE_DISABLED);
            refresh_color(lv_obj_get_child(ui.confirm_button,0),LV_STYLE_TEXT_COLOR,lv_color_hex(warning?MUTED:BG),0);
        }
        if (ui.page == GBS_PAGE_STEAM) {
            set(F_CONTROL, ui.machine.stale ? "数据已过期" :
                (ui.action == GBS_ACTION_STEAM || ui.action == GBS_ACTION_STEAM_ENABLED) ? short_control[ui.control] : "");
            refresh_color(ui.fields[F_CONTROL],LV_STYLE_TEXT_COLOR,lv_color_hex(!ui.machine.stale&&ui.control==GBS_UI_CONTROL_CONFIRMED?GREEN:WARN),0);
        }
        if (ui.apply&&!native_editor) {
            if (ui.page == GBS_PAGE_STEAM) snprintf(text, sizeof(text), "应用 %d 档", ui.draft == 126 ? 1 : ui.draft == 128 ? 2 : 3);
            else if(needs_confirmation(field_action)) snprintf(text,sizeof(text),"继续确认");
            else if(choice_label(field_action,0)) snprintf(text,sizeof(text),"应用模式");
            else snprintf(text,sizeof(text),p->step<1?"应用 %.1f%s":"应用 %.0f%s",ui.draft,p->unit);
            refresh_label(lv_obj_get_child(lv_obj_get_child(ui.apply, 1), 0), text);
        }
    }

    bool blocked = ui.control == GBS_UI_CONTROL_PENDING || ui.conflict;
    if (ui.steam_toggle) {
        if (blocked || !gbs_ui_can_control(GBS_ACTION_STEAM_ENABLED,&ui.machine)) lv_obj_add_state(ui.steam_toggle, LV_STATE_DISABLED); else lv_obj_clear_state(ui.steam_toggle, LV_STATE_DISABLED);
        if (ui.machine.steam_enabled) lv_obj_add_state(ui.steam_toggle, LV_STATE_CHECKED); else lv_obj_clear_state(ui.steam_toggle, LV_STATE_CHECKED);
    }
    if (ui.apply) {
        gbs_ui_action_t action = parameter()->action;
        bool disabled = blocked || !ui.dirty || !isfinite(ui.initial) || !gbs_ui_can_control(action,&ui.machine);
        if (disabled) lv_obj_add_state(ui.apply, LV_STATE_DISABLED); else lv_obj_clear_state(ui.apply, LV_STATE_DISABLED);
        refresh_num(ui.apply,LV_STYLE_OPA, LV_OPA_COVER, LV_STATE_DISABLED);
        lv_obj_t *face = lv_obj_get_child(ui.apply, 1);
        refresh_color(face,LV_STYLE_BG_COLOR, lv_color_hex(disabled ? SURFACE : GOLD), 0);
        refresh_color(face,LV_STYLE_BG_GRAD_COLOR, lv_color_hex(disabled ? 0x1a1e1a : 0xddb477), 0);
        refresh_color(face,LV_STYLE_BORDER_COLOR, lv_color_hex(disabled ? LINE : 0xf0cb91), 0);
        refresh_color(lv_obj_get_child(face, 0),LV_STYLE_TEXT_COLOR, lv_color_hex(disabled ? MUTED : BG), 0);
    }
    for(unsigned i=0;i<4;++i) if(ui.choices[i]) {
        lv_obj_t *b=ui.choices[i]; bool selected=isfinite(ui.initial)&&ui.draft==i;
        bool disabled=blocked||!gbs_ui_can_control(parameter()->action,&ui.machine);
        if(disabled) lv_obj_add_state(b,LV_STATE_DISABLED); else lv_obj_clear_state(b,LV_STATE_DISABLED);
        refresh_num(b,LV_STYLE_OPA,LV_OPA_COVER,LV_STATE_DISABLED);
        refresh_color(b,LV_STYLE_BORDER_COLOR,lv_color_hex(selected?GOLD:LINE),0);
        refresh_color(lv_obj_get_child(b,1),LV_STYLE_TEXT_COLOR,lv_color_hex(selected?GOLD:disabled?MUTED:TEXT),0);
    }
    for (unsigned i = 0; i < 4; ++i) if (ui.presets[i]) {
        if(!native_home) {
            bool selected = ui.machine.available && ui.machine.weight == presets[i];
            refresh_color(ui.preset_faces[i],LV_STYLE_BG_COLOR, lv_color_hex(selected ? GOLD : SURFACE), 0);
            refresh_color(ui.preset_faces[i],LV_STYLE_BG_GRAD_COLOR, lv_color_hex(selected ? 0xddb477 : 0x1a1e1a), 0);
            refresh_color(ui.preset_faces[i],LV_STYLE_BORDER_COLOR, lv_color_hex(selected ? 0xf2cc91 : LINE), 0);
            refresh_color(lv_obj_get_child(ui.preset_faces[i], 0),LV_STYLE_TEXT_COLOR, lv_color_hex(selected ? BG : TEXT), 0);
        }
        if (blocked || !gbs_ui_can_control(GBS_ACTION_WEIGHT,&ui.machine)) lv_obj_add_state(ui.presets[i], LV_STATE_DISABLED); else lv_obj_clear_state(ui.presets[i], LV_STATE_DISABLED);
    }
    set(F_MODE, ui.bridge.active ? "当前：联动" : "当前：仅监测");
    if (ui.page == GBS_PAGE_MODE && !(ui.action == GBS_ACTION_MODE && ui.control != GBS_UI_CONTROL_IDLE)) set(F_CONTROL, "确认后生效");
    bool connection_cards = ui.page == GBS_PAGE_CONNECTIONS;
    set(F_WIFI, connection_cards ? (ui.bridge.wifi ? "已连接" : "离线") : (ui.bridge.wifi ? "Wi-Fi        已连接" : "Wi-Fi        离线"));
    set(F_GRINDER, connection_cards ? (ui.bridge.grinder ? "已连接" : "重连中") : (ui.bridge.grinder ? "磨豆机       已连接" : "磨豆机       离线"));
    set(F_CLOUD, connection_cards ? (ui.bridge.cloud ? "可用" : "不可用") : (ui.bridge.cloud ? "云端         已连接" : "云端         不可用"));
    set(F_HUB, !ui.machine.available ? "未接入" : ui.machine.stale ? "已过期" : "已接入");
    const bool connected[] = {ui.bridge.wifi, ui.machine.available && !ui.machine.stale, ui.bridge.cloud, ui.bridge.grinder};
    for (unsigned i = 0; i < 4; ++i) if (ui.connection_dots[i])
        refresh_color(ui.connection_dots[i],LV_STYLE_BG_COLOR, lv_color_hex(connected[i] ? GREEN : WARN), 0);
    if (ui.page == GBS_PAGE_DISPLAY || ui.page == GBS_PAGE_BRIGHTNESS) {
        snprintf(text, sizeof(text), "%u%%", ui.brightness_draft); set(F_BRIGHTNESS, text);
    }
    if (ui.page == GBS_PAGE_DISPLAY || ui.page == GBS_PAGE_SOUND) {
        snprintf(text, sizeof(text), "%u%%", ui.page==GBS_PAGE_SOUND?ui.sound_draft:ui.sound_volume); set(F_SOUND, text);
    }
    snprintf(text, sizeof(text), "内部空闲  %u KiB\n最大连续块  %u KiB", ui.bridge.free_internal / 1024, ui.bridge.largest_internal / 1024);
    set(F_MEMORY, text);
#ifdef HUB_AMOLED_216
    amoled_refresh();
    amoled_details_refresh();
#endif
}

void gbs_ui_init(gbs_ui_submit_t submit, void *context)
{
    if(!collections) collections=lv_mem_alloc(sizeof(*collections)*GBS_DATA_COUNT);
    LV_ASSERT_MALLOC(collections);
    memset(collections,0,sizeof(*collections)*GBS_DATA_COUNT);
    memset(&ui, 0, sizeof(ui)); ui.data=collections; ui.submit = submit; ui.context = context;
    ui.machine = (gbs_ui_machine_t){.coffee=NAN, .coffee_target=NAN, .steam=NAN, .steam_target=NAN, .weight=NAN};
    ui.previous_weight=NAN; ui.variable_weight=40; ui.brightness = DEFAULT_BRIGHTNESS; ui.dim = DEFAULT_DIM; render();
}
void gbs_ui_set_variable_weight(unsigned grams)
{
    ui.variable_weight=gbs_ui_variable_weight_valid(grams)?grams:40;
    refresh();
}
unsigned gbs_ui_variable_weight(void) { return ui.variable_weight; }
void gbs_ui_set_reader(gbs_ui_read_t reader) { ui.reader=reader; }
void gbs_ui_set_schedule_submit(gbs_ui_schedule_submit_t submit) { ui.schedule_submit=submit; }
void gbs_ui_set_data(const gbs_ui_data_t *data)
{
    if(!data||data->kind>=GBS_DATA_COUNT||data->count>(data->kind==GBS_DATA_SCHEDULE?GBS_SCHEDULE_LIMIT:GBS_HISTORY_LIMIT)) return;
    if(data->kind==GBS_DATA_SCHEDULE&&plan_editor()&&!ui.schedule_submitted&&data->valid) {
        unsigned index=0;
        while(index<data->count&&strcmp(data->items.schedules[index].id,ui.selected_schedule.id)) ++index;
        if(data->binding_generation!=ui.data[data->kind].binding_generation||index==data->count||
            !gbs_ui_schedule_equal(&data->items.schedules[index],&ui.selected_schedule)) ui.conflict=true;
    }
    ui.data[data->kind]=*data;
    unsigned pages=data->count?(data->count+2)/3:1;
    if(ui.data_page[data->kind]>=pages) ui.data_page[data->kind]=pages-1;
    if((data_page()&&data_kind()==data->kind)||plan_editor()) render();
}
#ifdef HUB_AMOLED_216
static void remember_weight(double grams)
{
    if(!gbs_ui_variable_weight_valid(grams) || grams==ui.variable_weight) return;
    if(ui.submit && ui.submit(GBS_ACTION_WEIGHT_PRESET,grams,0,NULL,ui.context)) ui.variable_weight=(unsigned)grams;
    else {
        snprintf(ui.preference_notice,sizeof(ui.preference_notice),"保存失败，请重试");
        ui.preference_notice_until=ui.now+2500;
    }
}

static void cup_tone_once(unsigned bit,gbs_tone_t tone,bool audible)
{
    if(ui.cup_tone_nodes&bit) return;
    ui.cup_tone_nodes|=bit;
    if(audible) queue_tone(tone);
}

static void cup_progress_tones(const gbs_ui_bridge_t *s)
{
    if(!s->valid) return;
    bool audible=ui.bridge.valid;
    if(s->job&&s->job_id[0]) {
        if(strcmp(ui.tone_job_id,s->job_id)) {
            strcpy(ui.tone_job_id,s->job_id); ui.cup_tone_nodes=0;
        }
        if(s->recipe_enabled&&s->recipe_current) {
            if(!strcmp(s->recipe_phase,"applied")) cup_tone_once(2,GBS_TONE_SUCCESS,audible);
            else if(!strcmp(s->recipe_phase,"failed")) cup_tone_once(2,GBS_TONE_FAILURE,audible);
            else if(!strcmp(s->recipe_phase,"unconfirmed")) cup_tone_once(2,GBS_TONE_FAILURE,audible);
        }
    }
}
#endif

void gbs_ui_set_bridge(const gbs_ui_bridge_t *s)
{
    gbs_ui_stage_t old_stage = gbs_ui_stage(&ui.bridge);
#ifdef HUB_AMOLED_216
    cup_progress_tones(s);
    if(s->successes>ui.bridge.successes) ui.manual_weight=false;
#endif
    if(ui.bridge.valid&&s->valid) {
        if(s->successes>ui.bridge.successes&&gbs_ui_stage(s)==GBS_UI_RECEIVED) queue_tone(GBS_TONE_SUCCESS);
        else if((ui.bridge.job||ui.bridge.pending)&&old_stage!=gbs_ui_stage(s)&&
                (gbs_ui_stage(s)==GBS_UI_INVALID||!strcmp(s->outcome,"delivery_refused"))) queue_tone(GBS_TONE_FAILURE);
#ifdef HUB_AMOLED_216
        else if((ui.bridge.job||ui.bridge.pending)&&old_stage!=gbs_ui_stage(s)&&
                (gbs_ui_stage(s)==GBS_UI_UNKNOWN||gbs_ui_stage(s)==GBS_UI_EXPIRED||gbs_ui_stage(s)==GBS_UI_INTERRUPTED||gbs_ui_stage(s)==GBS_UI_REFUSED))
            queue_tone(GBS_TONE_FAILURE);
#endif
    }
    bool recipe_changed=strcmp(ui.bridge.recipe_phase,s->recipe_phase)!=0;
    if(recipe_changed&&ui.bridge.valid&&ui.bridge.job&&s->job&&ui.bridge.job_deadline_ms==s->job_deadline_ms&&
       (!strcmp(ui.bridge.recipe_phase,"reading")||!strcmp(ui.bridge.recipe_phase,"applying"))&&
       !strcmp(s->recipe_phase,"failed")) {
#ifdef HUB_AMOLED_216
        if(!s->job_id[0]) queue_tone(GBS_TONE_FAILURE);
#else
        queue_tone(GBS_TONE_FAILURE);
#endif
    }
    if(s->job&&(!ui.bridge.job||s->job_deadline_ms!=ui.bridge.job_deadline_ms)) {
        ui.manual_weight=false; ui.previous_weight=ui.machine.available&&!ui.machine.stale?ui.machine.weight:NAN;
    }
#ifdef HUB_AMOLED_216
    if(s->valid && s->job && s->recipe_enabled && s->recipe_current && s->recipe_log_valid &&
       s->job_id[0] && !strcmp(s->job_id,s->recipe_job_id) && !strcmp(s->recipe_phase,"applied") &&
       strcmp(ui.saved_recipe_job_id,s->job_id)) {
        strcpy(ui.saved_recipe_job_id,s->job_id);
        // Repeated cup snapshots must not replace a later manual override.
        if(!ui.manual_weight) remember_weight(s->recipe_grams);
    }
#endif
    ui.bridge = *s;
#ifdef HUB_AMOLED_216
    gbs_ui_flow_update(&ui.flow,s);
#endif
    if (ui.control == GBS_UI_CONTROL_PENDING && ui.action == GBS_ACTION_MODE &&
        s->observed_ms > ui.submitted_ms && s->observed_ms > ui.submitted_observation) {
        if (s->active == (ui.requested != 0)) ui.control = GBS_UI_CONTROL_CONFIRMED;
        else if (s->mode_failed) ui.control = GBS_UI_CONTROL_FAILED;
#ifdef HUB_AMOLED_216
        if(ui.control==GBS_UI_CONTROL_CONFIRMED) queue_tone(GBS_TONE_SUCCESS);
        else if(ui.control==GBS_UI_CONTROL_FAILED) queue_tone(GBS_TONE_FAILURE);
#endif
    }
    if ((old_stage != gbs_ui_stage(s) || recipe_changed) && (ui.page == GBS_PAGE_HOME || ui.page == GBS_PAGE_LINK || ui.page == GBS_PAGE_TASK)) render();
    else refresh();
}
void gbs_ui_set_machine(const gbs_ui_machine_t *s)
{
    double old = current_value();
    bool indicator_changed = ui.machine.available != s->available || ui.machine.stale != s->stale;
    bool steam_changed = ui.machine.steam_enabled_known != s->steam_enabled_known || ui.machine.writable != s->writable || ui.machine.stale != s->stale;
    ui.machine = *s;
    if (editor() && ui.dirty && !gbs_ui_action_unchanged(parameter()->action,&ui.edit_snapshot,s)) ui.conflict = true;
    if (editor() && !ui.dirty && ui.control != GBS_UI_CONTROL_PENDING && isfinite(current_value())) {
        if(ui.page!=GBS_PAGE_VARIABLE_WEIGHT) ui.draft = ui.initial = current_value();
        ui.edit_revision = s->revision;
        ui.edit_snapshot = *s;
    }
    bool choices_changed=parameter()&&choice_label(parameter()->action,0)&&old!=current_value();
    if ((indicator_changed && ui.page == GBS_PAGE_HOME) || (steam_changed && ui.page == GBS_PAGE_STEAM)||choices_changed) render(); else refresh();
}
void gbs_ui_control_result(uint32_t request_id, gbs_ui_control_t result)
{
    if (ui.control != GBS_UI_CONTROL_PENDING || request_id != ui.request_id) return;
    ui.control = result;
    if(result==GBS_UI_CONTROL_FAILED) queue_tone(GBS_TONE_FAILURE);
#ifdef HUB_AMOLED_216
    // A completed control sounds once, matching the displayed completion status.
    if(result==GBS_UI_CONTROL_CONFIRMED) queue_tone(GBS_TONE_SUCCESS);
    else if(result==GBS_UI_CONTROL_UNKNOWN||result==GBS_UI_CONTROL_PARTIAL) queue_tone(GBS_TONE_FAILURE);
#else
    // Machine acknowledgements may contain optimistic cache values, not physical readback.
    if(result==GBS_UI_CONTROL_CONFIRMED&&!gbs_ui_machine_action(ui.action)&&!ui.schedule_submitted) queue_tone(GBS_TONE_SUCCESS);
#endif
    if (result == GBS_UI_CONTROL_CONFIRMED) {
#ifdef HUB_AMOLED_216
        if(ui.action==GBS_ACTION_WEIGHT) remember_weight(ui.requested);
#endif
        if(ui.schedule_submitted) { ui.selected_schedule=ui.schedule_draft; ui.selected_stale=false; }
        ui.conflict=false;
        if(editor()) { ui.initial=ui.draft=ui.page==GBS_PAGE_VARIABLE_WEIGHT?ui.requested:current_value(); ui.edit_snapshot=ui.machine; }
    }
    render();
}
void gbs_ui_tick(int64_t now_ms)
{
    ui.now = now_ms;
    if(ui.preference_toast&&now_ms>=ui.preference_notice_until) {
        lv_obj_del(ui.preference_toast); ui.preference_toast=NULL; ui.preference_notice_until=0;
    }
    // This bounds the UI wait, not execution: a missing reply is never a safe retry.
    if (ui.control == GBS_UI_CONTROL_PENDING && now_ms - ui.submitted_ms >= GBS_UI_CONTROL_WAIT_MS) {
        ui.control = GBS_UI_CONTROL_UNKNOWN;
#ifdef HUB_AMOLED_216
        queue_tone(GBS_TONE_FAILURE);
#endif
        render();
    } else refresh();
}
gbs_ui_page_t gbs_ui_page(void) { return ui.page; }
bool gbs_ui_keep_awake(void) { return gbs_ui_busy(&ui.bridge, ui.control, ui.confirm || ui.discard); }
void gbs_ui_set_preferences(unsigned brightness, bool dim) { ui.brightness = brightness >= 25 && brightness <= 100 ? brightness : DEFAULT_BRIGHTNESS; ui.dim = dim; }
unsigned gbs_ui_brightness(void) { return ui.brightness; }
bool gbs_ui_dim_enabled(void) { return ui.dim; }

void gbs_ui_set_device(const gbs_device_status_t *device)
{
    if(!device) return;
    ui.device=*device;
    refresh();
}
void gbs_ui_set_device_preferences(bool auto_rotate,unsigned volume_percent)
{
    ui.auto_rotate=auto_rotate; ui.sound_volume=volume_percent<=100?volume_percent:0;
    if(!ui.sound_volume) ui.tone_count=0;
}
bool gbs_ui_rotation_auto(void) { return ui.auto_rotate; }
unsigned gbs_ui_sound_volume(void) { return ui.sound_volume; }
bool gbs_ui_sound_enabled(void) { return ui.sound_volume>0; }
gbs_tone_t gbs_ui_take_tone(void)
{
    if(!ui.tone_count) return GBS_TONE_NONE;
    gbs_tone_t tone=ui.tones[0];
    --ui.tone_count; memmove(ui.tones,ui.tones+1,ui.tone_count*sizeof(*ui.tones));
    return tone;
}

void gbs_ui_set_scale_refresh(bool (*refresh)(void *context)) { ui.scale_refresh=refresh; }

void gbs_ui_home_safe(void)
{
    if(ui.page==GBS_PAGE_HOME) return;
    if(ui.dirty) {
        ui.home_requested=ui.discard=true; ui.confirm=false; render(); return;
    }
    ui.depth=0; enter(GBS_PAGE_HOME);
}
void gbs_ui_toggle_rotation(void) { toggle_rotation(); }
