/* Native 480 px layouts use the approved assets and retain the shared control handlers. */
#include <time.h>
static lv_obj_t *am_path_separator,*am_path_grinder,*am_path_receipt,*am_boiler_clock[2];
static bool amoled_manual_weight_view(gbs_ui_stage_t stage)
{
    return ui.manual_weight&&ui.action==GBS_ACTION_WEIGHT&&ui.control!=GBS_UI_CONTROL_IDLE&&
        (stage==GBS_UI_WAIT||stage==GBS_UI_READY||stage==GBS_UI_MONITOR||stage==GBS_UI_RECEIVED);
}
static lv_obj_t *amoled_hit(int x,int y,int w,int h,int action)
{
    lv_obj_t *hit=button(lv_scr_act(),"",x,y,w,h,action,false);
    lv_obj_set_style_bg_opa(hit,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(hit,0,0);
    return hit;
}

static void amoled_header(const char *title)
{
    lv_obj_t *back=amoled_hit(16,14,64,64,BACK);
    icon(back,GBS_ICON_CHEVRON_LEFT,20,20,24,TEXT);
    label(lv_scr_act(),title,92,29,350,&gbs_font_25_bold,TEXT);
}

static void amoled_scale(int x,int y,int scale)
{
    int width=scale==1?82:172;
    ui.scale_shape=icon(lv_scr_act(),GBS_ICON_SCALE_FRAME,x,y,width,MUTED);
    centered(field(lv_scr_act(),F_SCALE,x,y+(scale==1?34:68),width,scale==1?&gbs_number_18:&gbs_number_40,MUTED));
    if(scale==1) lv_obj_set_style_text_letter_space(ui.fields[F_SCALE],-1,0);
}

static void amoled_battery(int x,int y,int scale)
{
    (void)scale;
    ui.battery_shape=box(lv_scr_act(),x,y,54,28,BG);
    lv_obj_set_style_border_width(ui.battery_shape,2,0);
    lv_obj_set_style_radius(ui.battery_shape,7,0);
    ui.battery_terminal=box(lv_scr_act(),x+56,y+9,3,10,MUTED);
    ui.battery_charging=label(lv_scr_act(),LV_SYMBOL_CHARGE,x+62,y+6,14,&lv_font_montserrat_14,GREEN);
    ui.battery_fill=box(ui.battery_shape,2,2,46,20,GREEN);
    lv_obj_set_style_radius(ui.battery_fill,4,0);
    centered(field(ui.battery_shape,F_BATTERY,0,6,50,&gbs_battery_18,TEXT));
    lv_obj_set_style_text_letter_space(ui.fields[F_BATTERY],-1,0);
    ui.battery_clip=box(ui.battery_shape,2,2,46,20,BG);
    lv_obj_set_style_bg_opa(ui.battery_clip,LV_OPA_TRANSP,0);
    lv_obj_set_style_radius(ui.battery_clip,0,0);
    centered(field(ui.battery_clip,F_BATTERY_DARK,-2,4,50,&gbs_battery_18,BG));
    lv_obj_set_style_text_letter_space(ui.fields[F_BATTERY_DARK],-1,0);
}

static void amoled_home(void)
{
    ui.native_layout=true;
    lv_obj_t *grinder=amoled_hit(28,18,64,64,GBS_PAGE_LINK);
    ui.grinder_icon=icon(grinder,GBS_ICON_GRINDER,15,15,34,GREEN);
    ui.grinder_dot=box(grinder,51,31,7,7,GREEN); lv_obj_set_style_radius(ui.grinder_dot,4,0);
    lv_obj_t *cloud=amoled_hit(100,18,64,64,GBS_PAGE_CONNECTIONS);
    ui.cloud_icon=icon(cloud,GBS_ICON_CLOUD_CHECK,15,15,34,GREEN);
    amoled_scale(216,20,1); amoled_hit(214,18,86,64,GBS_PAGE_SCALE);
    amoled_battery(317,36,1); amoled_hit(308,18,72,64,GBS_PAGE_DEVICE_BATTERY);
    lv_obj_t *gear=amoled_hit(388,18,64,64,GBS_PAGE_SETTINGS);
    icon(gear,GBS_ICON_SETTINGS,17,17,30,TEXT);
    field(lv_scr_act(),F_GLANCE_CAPTION,30,100,270,&gbs_font_20,MUTED);
    field(lv_scr_act(),F_GLANCE_VALUE,25,141,290,&gbs_number_108,TEXT);
    lv_obj_set_style_text_letter_space(ui.fields[F_GLANCE_VALUE],-6,0);
    lv_label_set_long_mode(ui.fields[F_GLANCE_VALUE],LV_LABEL_LONG_CLIP);
    field(lv_scr_act(),F_GLANCE_UNIT,160,196,52,&gbs_unit_36,TEXT);
    ui.phase_icon=icon(lv_scr_act(),GBS_ICON_GRINDER,350,126,62,GREEN);
    centered(field(lv_scr_act(),F_GLANCE_STAGE,326,200,110,&gbs_font_23,GREEN));
    amoled_hit(24,96,432,146,GBS_PAGE_LINK);
    ui.path_icon=icon(lv_scr_act(),GBS_ICON_COFFEE,28,260,28,MUTED);
    field(lv_scr_act(),F_PATH_HINT,68,263,44,&gbs_font_18,MUTED);
    field(lv_scr_act(),F_PATH_OLD,68,265,160,&gbs_number_24,GOLD);
    ui.path_arrow=icon(lv_scr_act(),GBS_ICON_ARROW_RIGHT,172,260,28,GOLD);
    field(lv_scr_act(),F_PATH_NEW,208,265,112,&gbs_number_24,GOLD);
    field(lv_scr_act(),F_GLANCE_PATH,70,260,380,&gbs_font_19,MUTED);
    am_path_separator=box(lv_scr_act(),151,264,1,20,LINE);
    am_path_grinder=icon(lv_scr_act(),GBS_ICON_GRINDER,172,260,28,GOLD);
    am_path_receipt=icon(lv_scr_act(),GBS_ICON_ARROW_RIGHT,212,260,28,GOLD);
    for(unsigned i=0;i<4;++i) {
        lv_obj_t *b=ui.presets[i]=button(lv_scr_act(),"",24+110*i,310,102,70,PRESET+i,false);
        lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(b,0,0);
        lv_obj_t *face=ui.preset_faces[i]=box(b,0,0,102,70,SURFACE);
        lv_obj_set_style_border_width(face,1,0); lv_obj_set_style_border_color(face,lv_color_hex(LINE),0);
        lv_obj_set_style_radius(face,12,0);
        char text[12]; snprintf(text,sizeof(text),"%d",presets[i]);
        label(face,text,24,22,50,&gbs_number_30,TEXT);
        ui.preset_units[i]=label(face,"g",64,30,22,&gbs_number_18,TEXT);
    }
    icon(lv_scr_act(),GBS_ICON_DROPLET,28,416,32,0xc4ccbf);
    field(lv_scr_act(),F_COFFEE,76,409,160,&gbs_number_27,TEXT);
    field(lv_scr_act(),F_COFFEE_TARGET,76,444,152,&gbs_number_18,0x9fa99a);
    am_boiler_clock[0]=icon(lv_scr_act(),GBS_ICON_CLOCK,76,444,20,GOLD);
    amoled_hit(24,402,210,64,GBS_PAGE_COFFEE);
    rule(lv_scr_act(),250,402,1,64);
    icon(lv_scr_act(),GBS_ICON_STEAM_STATUS,274,416,32,0xc4ccbf);
    field(lv_scr_act(),F_STEAM,322,409,130,&gbs_number_27,TEXT);
    field(lv_scr_act(),F_STEAM_TARGET,322,444,140,&gbs_number_18,0x9fa99a);
    am_boiler_clock[1]=icon(lv_scr_act(),GBS_ICON_CLOCK,322,444,20,GOLD);
    amoled_hit(256,402,200,64,GBS_PAGE_STEAM);
}

static void amoled_bottom(const char *text,int action)
{
    lv_obj_t *b=button(lv_scr_act(),"",28,392,424,64,action,false);
    lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(b,0,0);
    lv_obj_t *face=box(b,0,0,424,64,GOLD);lv_obj_set_style_radius(face,12,0);
    lv_obj_t *t=label(face,text,0,0,424,&gbs_font_24,BG); centered(t); lv_obj_center(t);
    if(action==APPLY) ui.apply=b;
}

static lv_obj_t *amoled_flat(lv_obj_t *parent,const char *text,int x,int y,int w,int h,int action,bool selected)
{
    lv_obj_t *b=button(parent,text,x,y,w,h,action,false);
    lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_NONE,0);lv_obj_set_style_bg_color(b,lv_color_hex(SURFACE),0);
    lv_obj_set_style_border_color(b,lv_color_hex(selected?GOLD:LINE),0);lv_obj_set_style_border_width(b,selected?2:1,0);
    lv_obj_set_style_radius(b,12,0);lv_obj_t *t=lv_obj_get_child(b,0);
    lv_obj_set_style_text_font(t,&gbs_font_24,0);lv_obj_set_style_text_color(t,lv_color_hex(selected?GOLD:TEXT),0);
    return b;
}
static void amoled_toggle(lv_obj_t *parent,int x,int y,bool on)
{
    lv_obj_t *track=box(parent,x,y,53,30,on?GREEN:0x4b544b);lv_obj_set_style_radius(track,15,0);
    lv_obj_t *knob=box(track,on?26:3,3,24,24,TEXT);lv_obj_set_style_radius(knob,12,0);
}
static lv_obj_t *amoled_row(lv_obj_t *parent,const char *title,const char *sub,int y,int h,int action)
{
    lv_obj_t *row=button(parent,"",0,y,424,h,action,false);
    lv_obj_set_style_bg_opa(row,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(row,0,0);
    if(action<0)lv_obj_clear_flag(row,LV_OBJ_FLAG_CLICKABLE);
    label(row,title,0,sub?8:20,340,&gbs_font_23,TEXT);
    if(sub)label(row,sub,0,40,350,&gbs_font_18,MUTED);
    rule(row,0,h-1,424,1);return row;
}
static void amoled_edit(void)
{
    ui.native_layout=true;
    const struct parameter *p=parameter();bool steam=ui.page==GBS_PAGE_STEAM;
    amoled_header(p->title);
    centered(field(lv_scr_act(),F_CONFLICT,28,109,424,&gbs_font_20,MUTED));
    field(lv_scr_act(),F_DRAFT,28,158,424,&gbs_number_76,TEXT);
    field(lv_scr_act(),F_EDITOR_UNIT,290,201,66,&gbs_unit_27,TEXT);
    centered(field(lv_scr_act(),F_EDITOR_CURRENT,28,249,424,&gbs_font_20,MUTED));
    if(steam) {
        for(int i=0;i<3;++i){char t[8];snprintf(t,sizeof(t),"%d",i+1);
            lv_obj_t *b=amoled_flat(lv_scr_act(),t,28+146*i,292,132,68,300+i,ui.draft==(i==0?126:i==1?128:131));
            lv_obj_set_style_text_font(lv_obj_get_child(b,0),&gbs_number_30,0);
            if(ui.draft==(i==0?126:i==1?128:131)) {
                lv_obj_set_style_bg_color(b,lv_color_hex(GOLD),0);
                lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(BG),0);
            }
            if(ui.machine.stale||ui.conflict||ui.control==GBS_UI_CONTROL_PENDING||!(ui.machine.writable&(1u<<p->action)))lv_obj_add_state(b,LV_STATE_DISABLED);
        }
        if(ui.machine.steam_enabled_known) {
            ui.steam_toggle=amoled_hit(388,14,64,64,STEAM_TOGGLE);
            icon(ui.steam_toggle,GBS_ICON_POWER,18,18,28,ui.machine.steam_enabled?GREEN:MUTED);
        }
        field(lv_scr_act(),F_CONTROL,28,365,424,&gbs_font_19,WARN);
    } else {
        lv_obj_t *minus=amoled_flat(lv_scr_act(),"",28,292,202,68,MINUS,false);
        lv_obj_t *plus=amoled_flat(lv_scr_act(),"",250,292,202,68,PLUS,false);
        icon(minus,GBS_ICON_MINUS,86,19,30,TEXT);icon(plus,GBS_ICON_PLUS,86,19,30,TEXT);
        if(ui.machine.stale||ui.conflict||ui.control==GBS_UI_CONTROL_PENDING||!(ui.machine.writable&(1u<<p->action))) {lv_obj_add_state(minus,LV_STATE_DISABLED);lv_obj_add_state(plus,LV_STATE_DISABLED);}
    }
    amoled_bottom("应用",APPLY);
}
static void amoled_device_page(void)
{
    ui.native_layout=true;
    const char *title=ui.page==GBS_PAGE_SCALE?"A 秤":ui.page==GBS_PAGE_DEVICE_BATTERY?"设备电量":ui.page==GBS_PAGE_ORIENTATION?"屏幕方向":"提示音";
    amoled_header(title);
    if(ui.page==GBS_PAGE_SCALE) {
        amoled_scale(154,104,2);
        centered(field(lv_scr_act(),F_GLANCE_STAGE,28,252,424,&gbs_font_32,GREEN));
        centered(field(lv_scr_act(),F_DEVICE_NOTE,28,302,424,&gbs_font_20,MUTED));
        centered(field(lv_scr_act(),F_GLANCE_CAPTION,28,341,424,&gbs_font_18,MUTED));
        amoled_flat(lv_scr_act(),"刷新",28,392,424,64,SCALE_REFRESH,false);
    } else if(ui.page==GBS_PAGE_DEVICE_BATTERY) {
        amoled_battery(28,148,1);
        field(lv_scr_act(),F_BATTERY_LARGE,116,133,210,&gbs_number_70,TEXT);
        field(lv_scr_act(),F_EDITOR_UNIT,240,161,70,&gbs_number_30,TEXT);
        label(lv_scr_act(),"设备电池",28,239,240,&gbs_font_23,TEXT);
        lv_obj_t *v=field(lv_scr_act(),F_GLANCE_STAGE,28,276,424,&gbs_font_18,MUTED);
        rule(lv_scr_act(),28,299,424,1);label(lv_scr_act(),"充电状态",28,323,250,&gbs_font_23,TEXT);
        v=field(lv_scr_act(),F_DEVICE_NOTE,270,323,182,&gbs_font_23,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);
        rule(lv_scr_act(),28,367,424,1);
    } else if(ui.page==GBS_PAGE_ORIENTATION) {
        lv_obj_t *body=box(lv_scr_act(),28,90,424,340,BG);
        lv_obj_t *r=amoled_row(body,"自动旋转","放稳后调整，触摸时保持",0,68,ROTATION_TOGGLE);amoled_toggle(r,371,19,ui.auto_rotate);
        r=amoled_row(body,"固定方向","",68,68,-1);
        field(r,F_DEVICE_NOTE,0,40,424,&gbs_font_18,MUTED);
        label(body,"平放时保持上一方向\n画面和触摸坐标同步旋转",0,150,424,&gbs_font_20,MUTED);
    } else {
        lv_obj_t *body=box(lv_scr_act(),28,90,424,320,BG);
        lv_obj_t *r=amoled_row(body,"成功与失败提示音",NULL,0,94,SOUND_TOGGLE);amoled_toggle(r,371,31,ui.sound);
        label(body,"成功与明确失败时提示\n请求受理与未知结果保持安静",0,129,424,&gbs_font_20,MUTED);
    }
}
static bool amoled_settings_page(void)
{
    if(ui.page!=GBS_PAGE_SETTINGS&&ui.page!=GBS_PAGE_DISPLAY&&ui.page!=GBS_PAGE_BRIGHTNESS)return false;
    ui.native_layout=true;amoled_header(ui.page==GBS_PAGE_SETTINGS?"设置":ui.page==GBS_PAGE_DISPLAY?"显示与声音":"屏幕亮度");
    if(ui.page==GBS_PAGE_BRIGHTNESS) {
        centered(field(lv_scr_act(),F_BRIGHTNESS,28,125,424,&gbs_number_76,TEXT));
        lv_obj_t *slider=lv_slider_create(lv_scr_act());lv_obj_set_pos(slider,80,264);lv_obj_set_size(slider,320,12);
        lv_obj_set_ext_click_area(slider,26);lv_slider_set_range(slider,25,100);lv_slider_set_value(slider,ui.brightness_draft,LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider,lv_color_hex(LINE),LV_PART_MAIN);lv_obj_set_style_bg_color(slider,lv_color_hex(GOLD),LV_PART_INDICATOR);lv_obj_set_style_bg_color(slider,lv_color_hex(TEXT),LV_PART_KNOB);
        lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_MAIN);lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_INDICATOR);lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_KNOB);
        lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_MAIN);lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider,9,LV_PART_KNOB);
        lv_obj_add_event_cb(slider,event,LV_EVENT_VALUE_CHANGED,(void *)(intptr_t)BRIGHTNESS);
        centered(field(lv_scr_act(),F_DETAIL,28,329,424,&gbs_font_20,MUTED));set(F_DETAIL,"首次触摸仅唤醒");amoled_bottom("保存",DISPLAY_SAVE);return true;
    }
    ui.body=box(lv_scr_act(),28,90,424,340,BG);lv_obj_add_flag(ui.body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(ui.body,LV_DIR_VER);
    if(ui.page==GBS_PAGE_SETTINGS) {
        const char *names[]={"咖啡机","历史记录","定时计划","显示与声音","联动模式","连接与系统","关于与更新"};
        const int pages[]={GBS_PAGE_MACHINE,GBS_PAGE_COFFEES,GBS_PAGE_SCHEDULE,GBS_PAGE_DISPLAY,GBS_PAGE_MODE,GBS_PAGE_CONNECTIONS,GBS_PAGE_ABOUT};
        const gbs_icon_t symbols[]={GBS_ICON_COFFEE,GBS_ICON_HISTORY,GBS_ICON_CLOCK,GBS_ICON_SUN,GBS_ICON_LINK,GBS_ICON_WIFI,GBS_ICON_HELP_CIRCLE};
        for(int i=0;i<7;++i){lv_obj_t *r=amoled_row(ui.body,"",NULL,i*68,68,pages[i]);icon(r,symbols[i],0,23,22,i==4?GREEN:MUTED);label(r,names[i],34,19,280,&gbs_font_23,TEXT);icon(r,GBS_ICON_CHEVRON_RIGHT,402,23,22,MUTED);if(i==4){lv_obj_t *v=label(r,ui.bridge.active?"运行":"监测",314,22,75,&gbs_font_20,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);}}
    } else {
        lv_obj_t *r=amoled_row(ui.body,"屏幕亮度",NULL,0,68,GBS_PAGE_BRIGHTNESS);lv_obj_t *v=field(r,F_BRIGHTNESS,302,20,122,&gbs_number_24,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);
        r=amoled_row(ui.body,"自动调暗 / 熄屏","60s 调暗 · 120s 熄屏",68,68,DIM_SAVE);amoled_toggle(r,371,19,ui.dim);
        const char *sides[]={"右侧","下侧","左侧","上侧"};char direction[32];
        snprintf(direction,sizeof(direction),"C口%s",sides[(unsigned)ui.device.orientation<4?(unsigned)ui.device.orientation:0]);
        r=amoled_row(ui.body,"屏幕方向",NULL,136,68,GBS_PAGE_ORIENTATION);v=label(r,ui.auto_rotate?"自动":direction,282,22,108,&gbs_font_20,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);icon(r,GBS_ICON_CHEVRON_RIGHT,402,23,22,MUTED);
        r=amoled_row(ui.body,"短提示音","默认静音",204,68,SOUND_TOGGLE);amoled_toggle(r,371,19,ui.sound);
        amoled_row(ui.body,"麦克风","未启用",272,68,-1);
    }
    return true;
}
#include "gbs_ui_amoled_details.inc"

static void amoled_refresh(void)
{
    bool scale_fresh=ui.machine.scale_connected_known&&!ui.machine.scale_stale&&ui.machine.scale_observed_ms>0;
    bool scale_online=scale_fresh&&ui.machine.scale_connected;
    bool scale_current=scale_online&&ui.machine.scale_battery_known&&ui.machine.scale_battery_percent<=100;
    bool scale_history=!scale_current&&ui.machine.scale_battery_last_known&&ui.machine.scale_battery_last_percent<=100;
    uint32_t scale_color=scale_online&&!scale_history?GREEN:MUTED;
    char text[96];
    if(ui.scale_shape) {
        refresh_color(ui.scale_shape,LV_STYLE_IMG_RECOLOR,lv_color_hex(scale_color),0);
        refresh_color(ui.fields[F_SCALE],LV_STYLE_TEXT_COLOR,lv_color_hex(scale_color),0);
        if(scale_current) snprintf(text,sizeof(text),"%u%%",ui.machine.scale_battery_percent);
        else if(scale_history) snprintf(text,sizeof(text),"%u%%",ui.machine.scale_battery_last_percent);
        else snprintf(text,sizeof(text),"—");
        set(F_SCALE,text);
    }
    if(ui.battery_shape) {
        bool known=ui.device.valid&&ui.device.battery_present_known&&ui.device.battery_present&&ui.device.battery_percent_known;
        if(ui.device.valid&&ui.device.battery_present_known&&ui.device.battery_present&&ui.device.charging_known&&ui.device.charging)refresh_hidden(ui.battery_charging,false);else refresh_hidden(ui.battery_charging,true);
        uint32_t color=known?(ui.device.battery_percent<=20?WARN:GREEN):MUTED;
        refresh_color(ui.battery_shape,LV_STYLE_BORDER_COLOR,lv_color_hex(color),0);
        refresh_color(ui.battery_terminal,LV_STYLE_BG_COLOR,lv_color_hex(color),0);
        refresh_color(ui.battery_fill,LV_STYLE_BG_COLOR,lv_color_hex(color),0);
        unsigned width=known?50*ui.device.battery_percent/100:0;
        if(width>46) width=46;
        if(width) {refresh_hidden(ui.battery_fill,false);refresh_hidden(ui.battery_clip,false);}
        else {refresh_hidden(ui.battery_fill,true);refresh_hidden(ui.battery_clip,true);}
        lv_obj_set_width(ui.battery_fill,width); lv_obj_set_width(ui.battery_clip,width);
        if(known) snprintf(text,sizeof(text),"%u%%",ui.device.battery_percent); else snprintf(text,sizeof(text),"—");
        set(F_BATTERY,text); set(F_BATTERY_DARK,text);
    }
    if(ui.page==GBS_PAGE_SCALE) {
        refresh_color(ui.fields[F_GLANCE_STAGE],LV_STYLE_TEXT_COLOR,lv_color_hex(scale_online?GREEN:MUTED),0);
        set(F_GLANCE_STAGE,ui.machine.scale_stale&&ui.machine.scale_observed_ms>0?"状态已过期":!ui.machine.scale_connected_known||ui.machine.scale_observed_ms<=0?"状态未知":scale_online?"已连接":"未连接");
        if(scale_history) {
            set(F_DEVICE_NOTE,"上次电量");
            time_t observed=(time_t)ui.machine.scale_battery_last_observed_unix;
            struct tm date;
            if(observed>0&&(int64_t)observed==ui.machine.scale_battery_last_observed_unix&&gmtime_r(&observed,&date)&&date.tm_year>=100&&date.tm_year<=8099)
                snprintf(text,sizeof(text),"上次读取 %04d-%02d-%02d %02d:%02d UTC",date.tm_year+1900,date.tm_mon+1,date.tm_mday,date.tm_hour,date.tm_min);
            else snprintf(text,sizeof(text),"上次读取时间未知");
            set(F_GLANCE_CAPTION,text);
        } else {
            if(ui.machine.scale_observed_ms>0&&ui.now>=ui.machine.scale_observed_ms)
                snprintf(text,sizeof(text),"%lld 秒前更新",(long long)((ui.now-ui.machine.scale_observed_ms)/1000));
            else snprintf(text,sizeof(text),"等待状态更新");
            set(F_DEVICE_NOTE,text);
            set(F_GLANCE_CAPTION,"来自咖啡机 Cloud 状态");
        }
        return;
    }
    if(ui.page==GBS_PAGE_DEVICE_BATTERY) {
        bool present=ui.device.valid&&ui.device.battery_present_known&&ui.device.battery_present;
        bool known=present&&ui.device.battery_percent_known;
        if(known)snprintf(text,sizeof(text),"%u",ui.device.battery_percent);else strcpy(text,"—");set(F_BATTERY_LARGE,text);
        int width=lv_txt_get_width(text,strlen(text),&gbs_number_70,0,LV_TEXT_FLAG_NONE);
        set(F_EDITOR_UNIT,known?"%":"");lv_obj_set_x(ui.fields[F_EDITOR_UNIT],116+width);
        lv_obj_set_y(ui.fields[F_EDITOR_UNIT],133+gbs_number_70.line_height-gbs_number_70.base_line-gbs_number_30.line_height+gbs_number_30.base_line);
        set(F_GLANCE_STAGE,!ui.device.valid?"暂不可读":!ui.device.battery_present_known?"未知":present?"已连接":"未检测到");
        set(F_DEVICE_NOTE,!present||!ui.device.charging_known?"未知":ui.device.charging?"充电中":"未充电");return;
    }
    if(ui.page==GBS_PAGE_ORIENTATION) {
        const char *sides[]={"右侧","下侧","左侧","上侧"};
        unsigned direction=(unsigned)ui.device.orientation;
        snprintf(text,sizeof(text),"USB-C 在%s%s",sides[direction<4?direction:0],ui.auto_rotate?" · 自动":"");
        set(F_DEVICE_NOTE,text); return;
    }
    if(ui.page!=GBS_PAGE_HOME) {
        if(editor()&&ui.fields[F_EDITOR_CURRENT]) {
            const struct parameter *p=parameter();bool steam=ui.page==GBS_PAGE_STEAM;
            bool fresh=ui.machine.available&&!ui.machine.stale;
            bool duration=p->action==GBS_ACTION_PREBREW_ON||p->action==GBS_ACTION_PREBREW_OFF||p->action==GBS_ACTION_STANDBY_MINUTES;
            const char *caption=steam?"选择蒸汽级别":ui.page==GBS_PAGE_COFFEE?"目标锅炉温度":ui.dirty?"待应用目标":duration?p->title:"设置出杯量";
            if(fresh&&!ui.conflict&&(ui.control==GBS_UI_CONTROL_IDLE||ui.action!=p->action||ui.dirty))set(F_CONFLICT,caption);
            double number=!isfinite(ui.initial)&&!ui.dirty?NAN:ui.draft;
            if(ui.action==p->action&&ui.control==GBS_UI_CONTROL_CONFIRMED&&!ui.dirty){set(F_CONFLICT,"已设定");number=ui.requested;}
            if(steam)number=number==126?1:number==128?2:number==131?3:NAN;
            value(F_DRAFT,number,steam||p->step>=1||floor(number)==number?"%.0f":"%.1f");
            const char *unit=steam||!isfinite(number)?"":p->unit;set(F_EDITOR_UNIT,unit);
            int width=lv_txt_get_width(lv_label_get_text(ui.fields[F_DRAFT]),strlen(lv_label_get_text(ui.fields[F_DRAFT])),&gbs_number_76,0,LV_TEXT_FLAG_NONE);
            const lv_font_t *unit_font=strlen(unit)>3?&gbs_font_24:&gbs_unit_27;
            refresh_font(ui.fields[F_EDITOR_UNIT],unit_font,0);
            lv_obj_set_y(ui.fields[F_EDITOR_UNIT],158+gbs_number_76.line_height-gbs_number_76.base_line-unit_font->line_height+unit_font->base_line);
            int uw=lv_txt_get_width(unit,strlen(unit),unit_font,0,LV_TEXT_FLAG_NONE);
            int left=(480-width-uw-(uw?5:0))/2;lv_obj_set_x(ui.fields[F_DRAFT],left);lv_obj_set_width(ui.fields[F_DRAFT],width+3);lv_obj_set_x(ui.fields[F_EDITOR_UNIT],left+width+5);
            if(!fresh)snprintf(text,sizeof(text),"机器状态暂不可用");
            else if(ui.page==GBS_PAGE_COFFEE)snprintf(text,sizeof(text),"当前 %.1f°C · 原目标 %.1f°C",ui.machine.coffee,ui.initial);
            else if(steam)snprintf(text,sizeof(text),"当前 %.0f° / 目标 %.0f°",ui.machine.steam,ui.draft);
            else if(duration)snprintf(text,sizeof(text),p->step<1?"上次设定 %.1f %s":"上次设定 %.0f %s",ui.initial,p->unit);
            else snprintf(text,sizeof(text),"咖啡机上次目标 %.0f %s",ui.initial,p->unit);
            set(F_EDITOR_CURRENT,text);
            refresh_color(ui.fields[F_CONFLICT],LV_STYLE_TEXT_COLOR,lv_color_hex(fresh&&!ui.conflict?MUTED:WARN),0);
            if(ui.apply){lv_obj_t *face=lv_obj_get_child(ui.apply,1);refresh_num(face,LV_STYLE_BG_GRAD_DIR,LV_GRAD_DIR_NONE,0);refresh_num(face,LV_STYLE_RADIUS,12,0);
                if(ui.action==p->action&&ui.control==GBS_UI_CONTROL_CONFIRMED&&!ui.dirty)strcpy(text,"已设定");
                else if(steam)snprintf(text,sizeof(text),"应用 %d",ui.draft==126?1:ui.draft==128?2:3);
                else snprintf(text,sizeof(text),floor(ui.draft)==ui.draft?"应用 %.0f%s":"应用 %.1f%s",ui.draft,p->unit);
                refresh_label(lv_obj_get_child(face,0),text);
            }
        }
        return;
    }
    gbs_ui_stage_t stage=gbs_ui_stage(&ui.bridge);
    const char *caption="当前",*state="就绪"; double grams=ui.machine.weight;
    gbs_icon_t glyph=GBS_ICON_GRINDER; uint32_t color=GREEN;
    bool recipe=ui.bridge.recipe_enabled&&ui.bridge.job&&stage==GBS_UI_WAIT;
    if(stage==GBS_UI_WAIT) { state="等结果"; glyph=GBS_ICON_CLOCK; color=WARN; }
    else if(stage==GBS_UI_SENDING) { caption="本杯"; state="回传中"; glyph=GBS_ICON_ARROW_RIGHT; color=GOLD; grams=ui.bridge.unconfirmed_grams>0?ui.bridge.unconfirmed_grams:NAN; }
    else if(stage==GBS_UI_RECEIVED) { caption="本杯"; state="已接收"; glyph=GBS_ICON_CHECK; grams=ui.bridge.result_grams; }
    else if(stage==GBS_UI_UNKNOWN) { caption="本杯"; state="未知"; glyph=GBS_ICON_HELP_CIRCLE; color=RED; grams=ui.bridge.unconfirmed_grams>0?ui.bridge.unconfirmed_grams:NAN; }
    else if(stage==GBS_UI_INVALID) { caption="本杯"; state="异常"; glyph=GBS_ICON_ALERT_TRIANGLE; color=RED; grams=NAN; }
    else if(stage==GBS_UI_EXPIRED||stage==GBS_UI_INTERRUPTED||stage==GBS_UI_REFUSED) { caption="本杯"; state=stage==GBS_UI_INTERRUPTED?"已中断":"已结束"; glyph=GBS_ICON_ALERT_TRIANGLE; color=WARN; grams=NAN; }
    else if(stage==GBS_UI_MONITOR) { state="仅监测"; glyph=GBS_ICON_EYE; color=MUTED; }
    else if(stage==GBS_UI_WIFI||stage==GBS_UI_GRINDER||stage==GBS_UI_CLOUD) { caption="联动"; state="离线"; glyph=GBS_ICON_CLOUD; color=MUTED; grams=NAN; glyph=GBS_ICON_CLOUD_OFF; }
    else if(stage==GBS_UI_BOOT||stage==GBS_UI_BASELINE||stage==GBS_UI_CLOCK) { caption="联动"; state=stage==GBS_UI_CLOCK?"校时中":"准备中"; grams=NAN; glyph=GBS_ICON_CLOCK; color=GOLD; }
    else if(stage!=GBS_UI_READY) { state=stage==GBS_UI_CANCELING?"收尾中":"待检查"; glyph=GBS_ICON_ALERT_CIRCLE; color=GOLD; }
    bool manual_view=amoled_manual_weight_view(stage);
    if(recipe&&!manual_view) {
        caption="配方"; grams=ui.bridge.recipe_grams>0?ui.bridge.recipe_grams:NAN;
        if(!strcmp(ui.bridge.recipe_phase,"waiting")||!strcmp(ui.bridge.recipe_phase,"reading")) {state="读取";glyph=GBS_ICON_FILE_SEARCH;}
        else if(!strcmp(ui.bridge.recipe_phase,"applying")) {state="同步中";glyph=GBS_ICON_REFRESH;}
        else if(!strcmp(ui.bridge.recipe_phase,"applied")) {state="已同步";glyph=GBS_ICON_CHECK;}
        else if(!strcmp(ui.bridge.recipe_phase,"unconfirmed")) {state="未确认";glyph=GBS_ICON_HELP_CIRCLE;}
        else if(!strcmp(ui.bridge.recipe_phase,"failed")||!strcmp(ui.bridge.recipe_phase,"probe_failed")) {state="失败";glyph=GBS_ICON_ALERT_TRIANGLE;grams=NAN;}
        else if(!strcmp(ui.bridge.recipe_phase,"machine_unavailable")) {state="未接入";glyph=GBS_ICON_ALERT_CIRCLE;grams=NAN;}
        else if(!strcmp(ui.bridge.recipe_phase,"skipped")) {state="未切重";glyph=GBS_ICON_HELP_CIRCLE;grams=NAN;}
        color=!strcmp(ui.bridge.recipe_phase,"applied")?GREEN:!strcmp(ui.bridge.recipe_phase,"failed")||!strcmp(ui.bridge.recipe_phase,"probe_failed")?RED:WARN;
    }
    if(manual_view&&ui.action==GBS_ACTION_WEIGHT&&ui.control!=GBS_UI_CONTROL_IDLE) {
        caption="手动"; grams=ui.requested; color=ui.control==GBS_UI_CONTROL_CONFIRMED?GREEN:ui.control==GBS_UI_CONTROL_FAILED||ui.control==GBS_UI_CONTROL_UNKNOWN?RED:WARN;
        state=ui.control==GBS_UI_CONTROL_PENDING?"设定中":ui.control==GBS_UI_CONTROL_CONFIRMED?"已设定":ui.control==GBS_UI_CONTROL_FAILED?"失败":"未确认";
        glyph=ui.control==GBS_UI_CONTROL_CONFIRMED?GBS_ICON_CHECK:ui.control==GBS_UI_CONTROL_FAILED?GBS_ICON_ALERT_TRIANGLE:GBS_ICON_HAND_STOP;
    }
    bool closing=stage==GBS_UI_CANCELING||(stage==GBS_UI_WAIT&&ui.bridge.job_deadline_ms>0&&ui.now>=ui.bridge.job_deadline_ms);
    if(closing) { caption="本杯"; state="收尾"; grams=NAN; color=WARN; glyph=GBS_ICON_CLOCK; }
    bool idle_stage=stage==GBS_UI_READY||stage==GBS_UI_MONITOR;
    if(idle_stage&&!ui.manual_weight&&(!ui.machine.available||ui.machine.stale)) { caption="机器"; state=ui.machine.stale?"已过期":"未接入"; grams=NAN; color=MUTED; glyph=GBS_ICON_ALERT_CIRCLE; }
    value(F_GLANCE_VALUE,grams,!manual_view&&(stage==GBS_UI_SENDING||stage==GBS_UI_RECEIVED||stage==GBS_UI_UNKNOWN)?"%.1f":"%.0f"); set(F_GLANCE_UNIT,isfinite(grams)?"g":"");
    const char *number_text=lv_label_get_text(ui.fields[F_GLANCE_VALUE]);
    const lv_font_t *number_font=&gbs_number_108;
    int letter_space=-6;
    int number_width=lv_txt_get_width(number_text,strlen(number_text),number_font,letter_space,LV_TEXT_FLAG_NONE);
    if(number_width>256){number_font=&gbs_number_76;letter_space=-2;number_width=lv_txt_get_width(number_text,strlen(number_text),number_font,letter_space,LV_TEXT_FLAG_NONE);}
    if(number_width>256){number_font=&gbs_number_64;letter_space=-2;number_width=lv_txt_get_width(number_text,strlen(number_text),number_font,letter_space,LV_TEXT_FLAG_NONE);}
    refresh_font(ui.fields[F_GLANCE_VALUE],number_font,0);
    refresh_num(ui.fields[F_GLANCE_VALUE],LV_STYLE_TEXT_LETTER_SPACE,letter_space,0);
    lv_obj_set_width(ui.fields[F_GLANCE_VALUE],number_width+3);
    lv_obj_set_y(ui.fields[F_GLANCE_VALUE],141+gbs_number_108.line_height-gbs_number_108.base_line-number_font->line_height+number_font->base_line);
    lv_obj_set_x(ui.fields[F_GLANCE_UNIT],25+number_width+2);
    lv_obj_set_y(ui.fields[F_GLANCE_UNIT],141+gbs_number_108.line_height-gbs_number_108.base_line-gbs_unit_36.line_height+gbs_unit_36.base_line);
    set(F_GLANCE_CAPTION,caption); set(F_GLANCE_STAGE,state);
    icon_source(ui.phase_icon,glyph,62);
    refresh_color(ui.phase_icon,LV_STYLE_IMG_RECOLOR,lv_color_hex(color),0);
    refresh_color(ui.fields[F_GLANCE_STAGE],LV_STYLE_TEXT_COLOR,lv_color_hex(color),0);
    refresh_color(ui.grinder_icon,LV_STYLE_IMG_RECOLOR,lv_color_hex(ui.bridge.valid&&ui.bridge.grinder?GREEN:MUTED),0);
    refresh_color(ui.cloud_icon,LV_STYLE_IMG_RECOLOR,lv_color_hex(ui.bridge.valid&&ui.bridge.cloud?GREEN:MUTED),0);
    refresh_color(ui.grinder_dot,LV_STYLE_BG_COLOR,lv_color_hex(ui.bridge.valid&&ui.bridge.grinder?GREEN:MUTED),0);
    const gbs_icon_t cloud_glyph=ui.bridge.valid&&ui.bridge.cloud?GBS_ICON_CLOUD_CHECK:GBS_ICON_CLOUD_OFF;
    icon_source(ui.cloud_icon,cloud_glyph,34);
    const char *path_hint="",*path_caption="";
    char path_old[32]="",path_new[32]="";
    int path_x=68,caption_x=70;
    uint32_t path_color=GOLD;
    bool path_arrow=false;
    gbs_icon_t path_glyph=GBS_ICON_COFFEE;
    if(closing) { path_glyph=GBS_ICON_CLOCK; strcpy(path_old,"00:00"); path_caption="等待结束确认"; caption_x=178; }
    else if(stage==GBS_UI_BOOT||stage==GBS_UI_BASELINE||stage==GBS_UI_CLOCK) {
        path_glyph=GBS_ICON_CLOCK; path_caption=stage==GBS_UI_BASELINE?"建立新任务基线":"连接准备中";
    } else if(manual_view||(recipe&&(!strcmp(ui.bridge.recipe_phase,"applying")||!strcmp(ui.bridge.recipe_phase,"applied")||!strcmp(ui.bridge.recipe_phase,"reading")||!strcmp(ui.bridge.recipe_phase,"waiting")))) {
        path_hint="上次"; path_x=112; path_color=0x8f998d; path_arrow=true;
        if(isfinite(ui.previous_weight)) snprintf(path_old,sizeof(path_old),"%.0fg",ui.previous_weight); else strcpy(path_old,"—");
        double target=manual_view?ui.requested:ui.bridge.recipe_grams>0?ui.bridge.recipe_grams:NAN;
        if(isfinite(target)) snprintf(path_new,sizeof(path_new),"%.0fg",target); else strcpy(path_new,"—");
    } else if(stage==GBS_UI_WAIT&&ui.bridge.job) {
        unsigned sec=gbs_ui_remaining(ui.bridge.job_deadline_ms,ui.now);path_glyph=GBS_ICON_CLOCK;
        snprintf(path_old,sizeof(path_old),"%02u:%02u",sec/60,sec%60);
        path_caption=ui.manual_weight?"本次手动":"待核对"; caption_x=178;
    } else if(stage==GBS_UI_SENDING||stage==GBS_UI_RECEIVED) {
        path_glyph=GBS_ICON_CLOCK;
        double seconds=stage==GBS_UI_SENDING?ui.bridge.unconfirmed_seconds:ui.bridge.result_seconds;
        if(isfinite(seconds)) snprintf(path_old,sizeof(path_old),"%.1fs",seconds); else strcpy(path_old,"—");
    } else if(stage==GBS_UI_UNKNOWN||!idle_stage||ui.machine.stale) {
        strcpy(path_old,"—"); path_color=0x8f998d;
    } else {
        double weight=ui.manual_weight?ui.requested:ui.machine.weight;
        if(isfinite(weight)) snprintf(path_old,sizeof(path_old),"%.0fg",weight); else strcpy(path_old,"—");
    }
    set(F_GLANCE_PATH,path_caption); set(F_PATH_HINT,path_hint); set(F_PATH_OLD,path_old); set(F_PATH_NEW,path_new);
    refresh_hidden(ui.path_arrow,!path_arrow);
    lv_obj_set_x(ui.fields[F_PATH_OLD],path_x); lv_obj_set_x(ui.fields[F_GLANCE_PATH],caption_x);
    refresh_color(ui.fields[F_PATH_OLD],LV_STYLE_TEXT_COLOR,lv_color_hex(path_color),0);
    icon_source(ui.path_icon,path_glyph,28);
    bool receipt=!manual_view&&(stage==GBS_UI_SENDING||stage==GBS_UI_RECEIVED);
    lv_obj_t *receipt_parts[]={am_path_separator,am_path_grinder,am_path_receipt};
    for(unsigned i=0;i<3;++i) {
        if(receipt) refresh_hidden(receipt_parts[i],false);
        else refresh_hidden(receipt_parts[i],true);
    }
    if(receipt) {
        const char *duration=lv_label_get_text(ui.fields[F_PATH_OLD]);
        int end=68+lv_txt_get_width(duration,strlen(duration),&gbs_number_24,0,LV_TEXT_FLAG_NONE);
        lv_obj_set_x(am_path_separator,end+18);lv_obj_set_x(am_path_grinder,end+39);lv_obj_set_x(am_path_receipt,end+79);
        icon_source(am_path_receipt,stage==GBS_UI_RECEIVED?GBS_ICON_CHECK:GBS_ICON_ARROW_RIGHT,28);
    }
    bool machine_fresh=ui.machine.available&&!ui.machine.stale;
    value(F_COFFEE,machine_fresh?ui.machine.coffee:NAN,"%.1f°");
    if(!machine_fresh||(ui.machine.steam_enabled_known&&!ui.machine.steam_enabled)) set(F_STEAM,"—");
    else value(F_STEAM,ui.machine.steam_target==126?1:ui.machine.steam_target==128?2:ui.machine.steam_target==131?3:NAN,"%.0f");
    int64_t now_unix=time(NULL);
    for(unsigned i=0;i<2;++i) {
        unsigned minutes=gbs_ui_boiler_warmup_minutes(&ui.machine,i!=0,now_unix);
        unsigned field_id=i?F_STEAM_TARGET:F_COFFEE_TARGET;
        lv_obj_t *subtitle=ui.fields[field_id];
        refresh_hidden(am_boiler_clock[i],minutes==0);
        lv_obj_set_x(subtitle,(i?322:76)+(minutes?26:0));
        lv_obj_set_width(subtitle,(i?140:152)-(minutes?26:0));
        refresh_font(subtitle,minutes?&gbs_font_18:&gbs_number_18,0);
        refresh_color(subtitle,LV_STYLE_TEXT_COLOR,lv_color_hex(minutes?GOLD:0x9fa99a),0);
        if(minutes) { snprintf(text,sizeof(text),"~%u min",minutes); set(field_id,text); }
        else if(!i) value(field_id,machine_fresh?ui.machine.coffee_target:NAN,"%.0f°");
        else {
            if(machine_fresh&&isfinite(ui.machine.steam)&&isfinite(ui.machine.steam_target))
                snprintf(text,sizeof(text),"%.0f° / %.0f°",ui.machine.steam,ui.machine.steam_target);
            else snprintf(text,sizeof(text),"—");
            set(field_id,text);
        }
    }
    double selected_weight=(recipe||manual_view)?ui.previous_weight:ui.machine.weight;
    if(manual_view&&ui.control==GBS_UI_CONTROL_CONFIRMED) selected_weight=ui.requested;
    else if(recipe&&!manual_view&&!strcmp(ui.bridge.recipe_phase,"applied")) selected_weight=ui.bridge.recipe_grams;
    for(unsigned i=0;i<4;++i) {
        bool selected=!ui.machine.stale&&ui.machine.available&&isfinite(selected_weight)&&selected_weight==presets[i];
        bool target=!ui.machine.stale&&(recipe||manual_view)&&isfinite(grams)&&grams==presets[i]&&!selected;
        refresh_color(ui.preset_faces[i],LV_STYLE_BG_COLOR,lv_color_hex(selected?GOLD:SURFACE),0);
        refresh_num(ui.preset_faces[i],LV_STYLE_BG_GRAD_DIR,LV_GRAD_DIR_NONE,0);
        refresh_color(ui.preset_faces[i],LV_STYLE_BORDER_COLOR,lv_color_hex(selected||target?GOLD:LINE),0);
        refresh_num(ui.preset_faces[i],LV_STYLE_BORDER_WIDTH,target?2:1,0);
        uint32_t foreground=selected?BG:target?GOLD:TEXT;
        refresh_color(lv_obj_get_child(ui.preset_faces[i],0),LV_STYLE_TEXT_COLOR,lv_color_hex(foreground),0);
        refresh_color(ui.preset_units[i],LV_STYLE_TEXT_COLOR,lv_color_hex(foreground),0);
    }

}

static const lv_font_t *amoled_font(const lv_font_t *font)
{
    if(font==&gbs_font_10) return &gbs_font_18;
    if(font==&gbs_font_12) return &gbs_font_20;
    if(font==&gbs_font_14) return &gbs_font_24;
    if(font==&gbs_font_12_bold) return &gbs_font_20_bold;
    if(font==&gbs_font_14_bold) return &gbs_font_24_bold;
    if(font==&gbs_font_16_bold) return &gbs_font_28_bold;
    if(font==&gbs_font_20_bold) return &gbs_font_34_bold;
    if(font==&gbs_number_16) return &gbs_number_28;
    if(font==&gbs_number_20) return &gbs_number_34;
    if(font==&gbs_number_24) return &gbs_number_40;
    if(font==&gbs_number_28) return &gbs_number_48;
    if(font==&gbs_number_32) return &gbs_number_56;
    if(font==&gbs_number_34) return &gbs_number_60;
    if(font==&gbs_number_36) return &gbs_number_64;
    if(font==&gbs_number_40) return &gbs_number_68;
    return font;
}

static void amoled_resize_children(lv_obj_t *parent)
{
    for(unsigned i=0;i<lv_obj_get_child_cnt(parent);++i) {
        lv_obj_t *child=lv_obj_get_child(parent,i);
        lv_coord_t x=lv_obj_get_x(child),y=lv_obj_get_y(child),w=lv_obj_get_width(child),h=lv_obj_get_height(child);
        amoled_resize_children(child);
        lv_obj_set_align(child,LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(child,x*2,y*12/7);
        if(lv_obj_check_type(child,&lv_img_class)) { struct icon_mask *mask=lv_obj_get_user_data(child); if(mask) icon_source(child,mask->name,mask->width*12/7); }
        else {
            lv_obj_set_size(child,w*2,h*12/7);
            if(lv_obj_has_flag(child,LV_OBJ_FLAG_CLICKABLE)) {
                int smallest=w*2<h*12/7?w*2:h*12/7;
                if(smallest<64) lv_obj_set_ext_click_area(child,(64-smallest+1)/2);
            }
            if(lv_obj_check_type(child,&lv_label_class)) {
                lv_obj_set_style_text_font(child,amoled_font(lv_obj_get_style_text_font(child,0)),0);
                lv_obj_set_height(child,LV_SIZE_CONTENT);
            }
        }
    }
}
