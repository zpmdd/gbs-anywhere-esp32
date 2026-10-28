/* Native 480 px layouts use the approved assets and retain the shared control handlers. */
#include <time.h>
/* The RGB565 surface uses the nearest representable shade to the approved #20271e. */
enum { AM_OFFLINE=0x858c86, AM_ERROR=0xf08f83, AM_FUTURE=0xb9b7ba };
static bool am_top;
static lv_obj_t *am_machine,*am_ble,*am_detail,*am_phase_question,*am_boiler_clock[2],*am_boiler_title[2];
static lv_obj_t *am_nodes[GBS_FLOW_COUNT],*am_node_rings[GBS_FLOW_COUNT],*am_node_icons[GBS_FLOW_COUNT],*am_node_labels[GBS_FLOW_COUNT],*am_node_questions[GBS_FLOW_COUNT];
static bool amoled_manual_weight_view(gbs_ui_stage_t stage)
{
    return ui.manual_weight&&ui.action==GBS_ACTION_WEIGHT&&ui.control!=GBS_UI_CONTROL_IDLE&&
        (stage==GBS_UI_WAIT||stage==GBS_UI_READY||stage==GBS_UI_MONITOR||stage==GBS_UI_RECEIVED||
         stage==GBS_UI_GRINDER||stage==GBS_UI_BASELINE);
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

/* Match SVG text baselines rather than font-dependent label top edges. */
static int amoled_text_y(const lv_font_t *font,int baseline)
{
    return baseline-font->line_height+font->base_line;
}
static lv_obj_t *amoled_text(lv_obj_t *parent,const char *text,int x,int baseline,int width,const lv_font_t *font,uint32_t color)
{
    return label(parent,text,x,amoled_text_y(font,baseline),width,font,color);
}
static lv_obj_t *amoled_field(unsigned id,int x,int baseline,int width,const lv_font_t *font,uint32_t color)
{
    return ui.fields[id]=amoled_text(lv_scr_act(),"",x,baseline,width,font,color);
}
static void amoled_top(void)
{
    am_top=true;
    ui.grinder_icon=icon(lv_scr_act(),GBS_ICON_FLOW_GRINDER,36,23,48,AM_OFFLINE);
    am_machine=icon(lv_scr_act(),GBS_ICON_FLOW_MACHINE,108,23,48,AM_OFFLINE);
    am_ble=icon(lv_scr_act(),GBS_ICON_FLOW_BLUETOOTH,147,22,15,AM_OFFLINE);
    ui.cloud_icon=icon(lv_scr_act(),GBS_ICON_FLOW_CLOUD,180,23,48,AM_OFFLINE);
    ui.scale_shape=icon(lv_scr_act(),GBS_ICON_FLOW_SCALE,252,23,48,AM_OFFLINE);
    centered(amoled_field(F_SCALE,252,53,48,&gbs_battery_13,AM_OFFLINE));
    ui.battery_shape=icon(lv_scr_act(),GBS_ICON_FLOW_BATTERY,322,23,52,AM_OFFLINE);
    centered(amoled_field(F_BATTERY,325,48,43,&gbs_battery_13,AM_OFFLINE));
    ui.battery_charging=label(lv_scr_act(),LV_SYMBOL_CHARGE,378,35,12,&lv_font_montserrat_14,GREEN);
    icon(lv_scr_act(),GBS_ICON_FLOW_SETTINGS,396,23,48,MUTED);
    const int pages[]={GBS_PAGE_LINK,GBS_PAGE_MACHINE,GBS_PAGE_CONNECTIONS,GBS_PAGE_SCALE,GBS_PAGE_DEVICE_BATTERY,GBS_PAGE_SETTINGS};
    for(unsigned i=0;i<6;++i) amoled_hit(28+72*i,12,64,64,pages[i]);
}
static void amoled_home(void)
{
    ui.native_layout=true;
    amoled_top();
    const char *names[]={"磨粉","配方","设定","结果","回传"};
    for(unsigned i=0;i<4;++i) {
        lv_obj_t *line=box(lv_scr_act(),76+91*i,122,55,3,AM_FUTURE);
        lv_obj_set_style_radius(line,0,0);
    }
    for(unsigned i=0;i<GBS_FLOW_COUNT;++i) {
        am_nodes[i]=box(lv_scr_act(),40+91*i,105,36,36,BG);
        lv_obj_set_style_radius(am_nodes[i],LV_RADIUS_CIRCLE,0);
        am_node_rings[i]=icon(lv_scr_act(),GBS_ICON_FLOW_NODE_RING,38+91*i,103,40,AM_FUTURE);
        am_node_icons[i]=icon(lv_scr_act(),GBS_ICON_FLOW_CLOCK,48+91*i,113,20,AM_FUTURE);
        am_node_questions[i]=amoled_text(lv_scr_act(),"?",40+91*i,129,36,&gbs_font_16_semibold,WARN);
        centered(am_node_questions[i]);
        am_node_labels[i]=amoled_text(lv_scr_act(),names[i],22+91*i,166,72,&gbs_font_16_semibold,AM_FUTURE);
        centered(am_node_labels[i]);
    }
    amoled_field(F_GLANCE_STAGE,28,234,276,&gbs_font_30_semibold,TEXT);
    amoled_field(F_GLANCE_PATH,30,270,274,&gbs_font_16,MUTED);
    lv_obj_set_style_text_letter_space(ui.fields[F_GLANCE_STAGE],-1,0);
    lv_label_set_long_mode(ui.fields[F_GLANCE_STAGE],LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(ui.fields[F_GLANCE_PATH],LV_LABEL_LONG_CLIP);
    rule(lv_scr_act(),307,207,1,65);
    centered(amoled_field(F_GLANCE_VALUE,315,242,140,&gbs_number_45,TEXT));
    lv_obj_set_style_text_letter_space(ui.fields[F_GLANCE_VALUE],-1,0);
    centered(amoled_field(F_GLANCE_CAPTION,315,270,140,&gbs_font_16,MUTED));
    ui.phase_icon=icon(lv_scr_act(),GBS_ICON_FLOW_WAIT_GRINDER,365,205,40,MUTED);
    am_phase_question=amoled_text(lv_scr_act(),"?",365,237,40,&gbs_font_30_semibold,GOLD);centered(am_phase_question);
    am_detail=icon(lv_scr_act(),GBS_ICON_FLOW_CHEVRON_RIGHT,281,252,16,GOLD);
    amoled_hit(24,94,432,187,GBS_PAGE_LINK);
    for(unsigned i=0;i<4;++i) {
        lv_obj_t *b=ui.presets[i]=button(lv_scr_act(),"",24+110*i,299,102,72,PRESET+i,false);
        lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(b,0,0);
        lv_obj_t *face=ui.preset_faces[i]=box(b,0,0,102,72,SURFACE);
        lv_obj_set_style_radius(face,15,0);
        char text[8];snprintf(text,sizeof(text),"%u",preset_value(i));
        centered(amoled_text(face,text,0,47,102,&gbs_number_31,TEXT));
        if(i==3) {
            label(face,LV_SYMBOL_EDIT,82,5,16,&lv_font_montserrat_14,MUTED);
            lv_obj_add_event_cb(b,event,LV_EVENT_LONG_PRESSED,(void*)(intptr_t)(PRESET+3));
        }
    }
    icon(lv_scr_act(),GBS_ICON_FLOW_DROP,36,403,21,MUTED);
    am_boiler_title[0]=amoled_text(lv_scr_act(),"咖啡",69,419,116,&gbs_font_18,MUTED);
    icon(lv_scr_act(),GBS_ICON_FLOW_CHEVRON_RIGHT,203,404,16,MUTED);
    amoled_field(F_COFFEE,68,457,91,&gbs_number_29,TEXT);
    amoled_field(F_COFFEE_TARGET,159,457,72,&gbs_number_18,MUTED);
    am_boiler_clock[0]=icon(lv_scr_act(),GBS_ICON_FLOW_CLOCK,38,438,21,GOLD);
    amoled_hit(24,391,210,77,GBS_PAGE_COFFEE);
    rule(lv_scr_act(),240,395,1,61);
    icon(lv_scr_act(),GBS_ICON_FLOW_STEAM,270,403,23,MUTED);
    am_boiler_title[1]=amoled_text(lv_scr_act(),"蒸汽",306,419,116,&gbs_font_18,MUTED);
    icon(lv_scr_act(),GBS_ICON_FLOW_CHEVRON_RIGHT,433,404,16,MUTED);
    amoled_field(F_STEAM,305,457,32,&gbs_number_29,TEXT);
    amoled_field(F_STEAM_TARGET,339,457,114,&gbs_font_16,MUTED);
    am_boiler_clock[1]=icon(lv_scr_act(),GBS_ICON_FLOW_CLOCK,271,438,21,GOLD);
    amoled_hit(256,391,200,77,GBS_PAGE_STEAM);
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
    amoled_top();
    lv_obj_t *back=amoled_hit(12,80,60,60,BACK);
    icon(back,GBS_ICON_FLOW_BACK,5,15,32,MUTED);
    centered(amoled_text(lv_scr_act(),p->title,80,121,320,&gbs_font_25_bold,TEXT));
    centered(amoled_field(F_CONFLICT,24,166,432,&gbs_font_17,MUTED));
    amoled_field(F_DRAFT,28,244,424,&gbs_number_72,TEXT);
    amoled_field(F_EDITOR_UNIT,288,243,66,&gbs_unit_24,MUTED);
    centered(amoled_field(F_EDITOR_CURRENT,24,277,432,&gbs_font_17,MUTED));
    if(steam) {
        for(int i=0;i<3;++i){char t[8];snprintf(t,sizeof(t),"%d",i+1);
            lv_obj_t *b=amoled_flat(lv_scr_act(),t,24+148*i,308,136,64,300+i,ui.draft==(i==0?126:i==1?128:131));
            lv_obj_set_style_text_font(lv_obj_get_child(b,0),&gbs_number_31,0);
            if(ui.draft==(i==0?126:i==1?128:131)) {
                lv_obj_set_style_bg_color(b,lv_color_hex(GOLD),0);
                lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(BG),0);
            }
            if(!gbs_ui_can_control(p->action,&ui.machine)||ui.conflict||ui.control==GBS_UI_CONTROL_PENDING)lv_obj_add_state(b,LV_STATE_DISABLED);
        }
        if(ui.machine.steam_enabled_known) {
            ui.steam_toggle=amoled_hit(396,80,60,60,STEAM_TOGGLE);
            icon(ui.steam_toggle,GBS_ICON_POWER,18,18,24,ui.machine.steam_enabled?GREEN:MUTED);
        }
        centered(amoled_field(F_CONTROL,24,393,432,&gbs_font_15,WARN));
    } else {
        lv_obj_t *minus=amoled_flat(lv_scr_act(),"",24,308,166,64,MINUS,false);
        lv_obj_t *plus=amoled_flat(lv_scr_act(),"",290,308,166,64,PLUS,false);
        lv_obj_set_style_border_width(minus,0,0);lv_obj_set_style_border_width(plus,0,0);
        lv_obj_set_style_radius(minus,15,0);lv_obj_set_style_radius(plus,15,0);
        icon(minus,GBS_ICON_FLOW_EDIT_MINUS,67,16,32,TEXT);icon(plus,GBS_ICON_FLOW_EDIT_PLUS,67,16,32,TEXT);
        char step[32];snprintf(step,sizeof(step),p->step<1?"%.1f%s / 步":"%.0f%s / 步",p->step,p->unit);
        centered(amoled_text(lv_scr_act(),step,190,346,100,&gbs_font_15,MUTED));
        if(!gbs_ui_can_control(p->action,&ui.machine)||ui.conflict||ui.control==GBS_UI_CONTROL_PENDING) {lv_obj_add_state(minus,LV_STATE_DISABLED);lv_obj_add_state(plus,LV_STATE_DISABLED);}
    }
    ui.apply=button(lv_scr_act(),"",24,399,432,62,APPLY,false);
    lv_obj_set_style_bg_opa(ui.apply,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(ui.apply,0,0);
    lv_obj_t *face=box(ui.apply,0,0,432,62,GOLD);lv_obj_set_style_radius(face,15,0);
    centered(amoled_text(face,"应用",0,41,432,&gbs_font_23_semibold,BG));
}
static void amoled_device_page(void)
{
    ui.native_layout=true;
    const char *title=ui.page==GBS_PAGE_SCALE?"A 秤":ui.page==GBS_PAGE_DEVICE_BATTERY?"设备电量":"屏幕方向";
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
    }
}
static bool amoled_settings_page(void)
{
    if(ui.page!=GBS_PAGE_SETTINGS&&ui.page!=GBS_PAGE_DISPLAY&&ui.page!=GBS_PAGE_BRIGHTNESS&&ui.page!=GBS_PAGE_SOUND)return false;
    bool volume=ui.page==GBS_PAGE_SOUND;
    ui.native_layout=true;amoled_header(ui.page==GBS_PAGE_SETTINGS?"设置":ui.page==GBS_PAGE_DISPLAY?"显示与声音":volume?"音量":"屏幕亮度");
    if(ui.page==GBS_PAGE_BRIGHTNESS||volume) {
        centered(field(lv_scr_act(),volume?F_SOUND:F_BRIGHTNESS,28,125,424,&gbs_number_76,TEXT));
        lv_obj_t *slider=lv_slider_create(lv_scr_act());lv_obj_set_pos(slider,80,264);lv_obj_set_size(slider,320,12);
        lv_obj_set_ext_click_area(slider,26);lv_slider_set_range(slider,volume?0:25,100);lv_slider_set_value(slider,volume?ui.sound_draft:ui.brightness_draft,LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider,lv_color_hex(LINE),LV_PART_MAIN);lv_obj_set_style_bg_color(slider,lv_color_hex(GOLD),LV_PART_INDICATOR);lv_obj_set_style_bg_color(slider,lv_color_hex(TEXT),LV_PART_KNOB);
        lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_MAIN);lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_INDICATOR);lv_obj_set_style_bg_opa(slider,LV_OPA_COVER,LV_PART_KNOB);
        lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_MAIN);lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);lv_obj_set_style_radius(slider,LV_RADIUS_CIRCLE,LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider,9,LV_PART_KNOB);
        lv_obj_add_event_cb(slider,event,LV_EVENT_VALUE_CHANGED,(void *)(intptr_t)(volume?SOUND_VOLUME:BRIGHTNESS));
        centered(field(lv_scr_act(),F_DETAIL,28,329,424,&gbs_font_20,MUTED));set(F_DETAIL,volume?"0% 静音 · 保存后提示":"首次触摸仅唤醒");amoled_bottom("保存",volume?SOUND_SAVE:DISPLAY_SAVE);return true;
    }
    ui.body=box(lv_scr_act(),28,90,424,340,BG);lv_obj_add_flag(ui.body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(ui.body,LV_DIR_VER);
    if(ui.page==GBS_PAGE_SETTINGS) {
        const char *names[]={"咖啡机","历史记录","定时计划","显示与声音","联动模式","连接与系统","关于与更新"};
        const int pages[]={GBS_PAGE_MACHINE,GBS_PAGE_COFFEES,GBS_PAGE_SCHEDULE,GBS_PAGE_DISPLAY,GBS_PAGE_MODE,GBS_PAGE_CONNECTIONS,GBS_PAGE_ABOUT};
        const gbs_icon_t symbols[]={GBS_ICON_COFFEE,GBS_ICON_HISTORY,GBS_ICON_CLOCK,GBS_ICON_SUN,GBS_ICON_LINK,GBS_ICON_WIFI,GBS_ICON_HELP_CIRCLE};
        for(int i=0;i<7;++i){lv_obj_t *r=amoled_row(ui.body,"",NULL,i*68,68,pages[i]);icon(r,symbols[i],0,23,22,i==4?GREEN:MUTED);label(r,names[i],34,19,280,&gbs_font_23,TEXT);icon(r,GBS_ICON_CHEVRON_RIGHT,402,23,22,MUTED);if(i==4){lv_obj_t *v=label(r,ui.bridge.active?"运行":"监测",314,22,75,&gbs_font_20,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);}}
    } else {
        lv_obj_t *r=amoled_row(ui.body,"屏幕亮度",NULL,0,68,GBS_PAGE_BRIGHTNESS);lv_obj_t *v=field(r,F_BRIGHTNESS,302,20,122,&gbs_amoled_number_24,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);
        r=amoled_row(ui.body,"自动调暗 / 熄屏","60s 调暗 · 120s 熄屏",68,68,DIM_SAVE);amoled_toggle(r,371,19,ui.dim);
        const char *sides[]={"右侧","下侧","左侧","上侧"};char direction[32];
        snprintf(direction,sizeof(direction),"C口%s",sides[(unsigned)ui.device.orientation<4?(unsigned)ui.device.orientation:0]);
        r=amoled_row(ui.body,"屏幕方向",NULL,136,68,GBS_PAGE_ORIENTATION);v=label(r,ui.auto_rotate?"自动":direction,282,22,108,&gbs_font_20,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);icon(r,GBS_ICON_CHEVRON_RIGHT,402,23,22,MUTED);
        r=amoled_row(ui.body,"音量",NULL,204,68,GBS_PAGE_SOUND);v=field(r,F_SOUND,272,20,118,&gbs_amoled_number_24,MUTED);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_RIGHT,0);icon(r,GBS_ICON_CHEVRON_RIGHT,402,23,22,MUTED);
        amoled_row(ui.body,"麦克风","未启用",272,68,-1);
    }
    return true;
}
#include "gbs_ui_amoled_details.inc"

static void amoled_refresh(void)
{
    if(am_top) {
        bool known=ui.bridge.valid&&ui.bridge.observed_ms>0&&ui.now>=ui.bridge.observed_ms&&ui.now-ui.bridge.observed_ms<GBS_UI_MACHINE_FRESH_MS;
        uint32_t grinder=!known?AM_OFFLINE:ui.bridge.pairing?AM_ERROR:ui.bridge.grinder?GREEN:ui.bridge.wifi&&ui.bridge.retry_at_ms>ui.now?WARN:AM_OFFLINE;
        uint32_t cloud=!known||!ui.bridge.wifi?AM_OFFLINE:ui.bridge.cloud?GREEN:WARN;
        refresh_color(ui.grinder_icon,LV_STYLE_IMG_RECOLOR,lv_color_hex(grinder),0);
        refresh_color(ui.cloud_icon,LV_STYLE_IMG_RECOLOR,lv_color_hex(cloud),0);
        const gbs_ui_ble_t *ble=&ui.machine.ble;
        uint32_t machine=AM_OFFLINE;
        if(!gbs_ui_ble_is_stale(ble,ui.now)&&ble->configured) {
            if(ble->connected)machine=ble->control_ready?GREEN:WARN;
            else if(!strcmp(ble->status,"scanning")||!strcmp(ble->status,"connecting")||!strcmp(ble->status,"authenticating")||!strcmp(ble->status,"confirmation_pending"))machine=WARN;
        }
        refresh_color(am_machine,LV_STYLE_IMG_RECOLOR,lv_color_hex(machine),0);
        refresh_color(am_ble,LV_STYLE_IMG_RECOLOR,lv_color_hex(machine),0);
    }
    bool scale_fresh=ui.machine.scale_connected_known&&!ui.machine.scale_stale&&!gbs_ui_scale_is_stale(&ui.machine,ui.now);
    bool scale_online=scale_fresh&&ui.machine.scale_connected;
    bool scale_current=scale_online&&ui.machine.scale_battery_known&&ui.machine.scale_battery_percent<=100;
    bool scale_history=!scale_current&&ui.machine.scale_battery_last_known&&ui.machine.scale_battery_last_percent<=100;
    uint32_t scale_color=scale_online?(scale_current&&ui.machine.scale_battery_percent<=20?WARN:GREEN):AM_OFFLINE;
    int64_t battery_age=scale_current?(ui.now-ui.machine.scale_observed_ms)/1000:-1;
    if(!scale_current&&scale_history&&ui.machine.scale_battery_last_observed_unix>0) {
        int64_t now=time(NULL);
        if(now>=ui.machine.scale_battery_last_observed_unix)
            battery_age=now-ui.machine.scale_battery_last_observed_unix;
    }
    unsigned battery_percent=scale_current?ui.machine.scale_battery_percent:ui.machine.scale_battery_last_percent;
    uint32_t battery_color=battery_age>=0&&battery_age<3600?(battery_percent<=20?WARN:GREEN):AM_OFFLINE;
    char text[96];
    if(ui.scale_shape) {
        refresh_color(ui.scale_shape,LV_STYLE_IMG_RECOLOR,lv_color_hex(scale_color),0);
        refresh_color(ui.fields[F_SCALE],LV_STYLE_TEXT_COLOR,lv_color_hex(battery_color),0);
        if(scale_current) snprintf(text,sizeof(text),"%u%%",ui.machine.scale_battery_percent);
        else if(scale_history) snprintf(text,sizeof(text),"%u%%",ui.machine.scale_battery_last_percent);
        else snprintf(text,sizeof(text),"—");
        set(F_SCALE,text);
    }
    if(ui.battery_shape) {
        bool known=ui.device.valid&&ui.device.battery_present_known&&ui.device.battery_present&&ui.device.battery_percent_known;
        if(ui.device.valid&&ui.device.battery_present_known&&ui.device.battery_present&&ui.device.charging_known&&ui.device.charging)refresh_hidden(ui.battery_charging,false);else refresh_hidden(ui.battery_charging,true);
        uint32_t color=known?(ui.device.battery_percent<=20?WARN:GREEN):AM_OFFLINE;
        if(am_top) {
            refresh_color(ui.battery_shape,LV_STYLE_IMG_RECOLOR,lv_color_hex(color),0);
            refresh_color(ui.fields[F_BATTERY],LV_STYLE_TEXT_COLOR,lv_color_hex(color),0);
        } else {
        refresh_color(ui.battery_shape,LV_STYLE_BORDER_COLOR,lv_color_hex(color),0);
        refresh_color(ui.battery_terminal,LV_STYLE_BG_COLOR,lv_color_hex(color),0);
        refresh_color(ui.battery_fill,LV_STYLE_BG_COLOR,lv_color_hex(color),0);
        unsigned width=known?50*ui.device.battery_percent/100:0;
        if(width>46) width=46;
        if(width) {refresh_hidden(ui.battery_fill,false);refresh_hidden(ui.battery_clip,false);}
        else {refresh_hidden(ui.battery_fill,true);refresh_hidden(ui.battery_clip,true);}
        lv_obj_set_width(ui.battery_fill,width); lv_obj_set_width(ui.battery_clip,width);
        }
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
            bool fresh=gbs_ui_can_control(p->action,&ui.machine);
            bool duration=p->action==GBS_ACTION_PREBREW_ON||p->action==GBS_ACTION_PREBREW_OFF||p->action==GBS_ACTION_STANDBY_MINUTES;
            bool previous=ui.page==GBS_PAGE_VARIABLE_WEIGHT&&ui.draft!=ui.requested;
            bool confirmed=ui.action==p->action&&ui.control==GBS_UI_CONTROL_CONFIRMED&&!ui.dirty&&!previous;
            const char *caption=steam?"选择蒸汽级别":ui.page==GBS_PAGE_COFFEE?"目标锅炉温度":ui.dirty?"待应用目标":duration?p->title:"设置出杯量";
            if(fresh&&!ui.conflict&&(ui.control==GBS_UI_CONTROL_IDLE||ui.action!=p->action||ui.dirty||previous))set(F_CONFLICT,caption);
            double number=!isfinite(ui.initial)&&!ui.dirty?NAN:ui.draft;
            if(confirmed){set(F_CONFLICT,"已设定");number=ui.requested;}
            if(steam)number=number==126?1:number==128?2:number==131?3:NAN;
            value(F_DRAFT,number,steam||p->step>=1||floor(number)==number?"%.0f":"%.1f");
            const char *unit=steam||!isfinite(number)?"":p->unit;set(F_EDITOR_UNIT,unit);
            int width=lv_txt_get_width(lv_label_get_text(ui.fields[F_DRAFT]),strlen(lv_label_get_text(ui.fields[F_DRAFT])),&gbs_number_72,0,LV_TEXT_FLAG_NONE);
            const lv_font_t *unit_font=strlen(unit)>3?&gbs_font_24:&gbs_unit_24;
            refresh_font(ui.fields[F_EDITOR_UNIT],unit_font,0);
            lv_obj_set_y(ui.fields[F_EDITOR_UNIT],amoled_text_y(unit_font,243));
            int uw=lv_txt_get_width(unit,strlen(unit),unit_font,0,LV_TEXT_FLAG_NONE);
            int left=uw?233-width/2:(480-width)/2;lv_obj_set_x(ui.fields[F_DRAFT],left);lv_obj_set_width(ui.fields[F_DRAFT],width+3);lv_obj_set_x(ui.fields[F_EDITOR_UNIT],left+width+12);
            if(!fresh)snprintf(text,sizeof(text),"机器状态暂不可用");
            else if(ui.page==GBS_PAGE_COFFEE)snprintf(text,sizeof(text),"当前 %.1f°C · 原目标 %.1f°C",ui.machine.coffee,ui.initial);
            else if(steam)snprintf(text,sizeof(text),"当前 %.0f° / 目标 %.0f°",ui.machine.steam,ui.draft);
            else if(duration)snprintf(text,sizeof(text),p->step<1?"上次设定 %.1f %s":"上次设定 %.0f %s",ui.initial,p->unit);
            else if(ui.page==GBS_PAGE_VARIABLE_WEIGHT)snprintf(text,sizeof(text),"已保存 %ug",ui.variable_weight);
            else snprintf(text,sizeof(text),"原目标 %.0f%s",ui.initial,p->unit);
            set(F_EDITOR_CURRENT,text);
            refresh_color(ui.fields[F_CONFLICT],LV_STYLE_TEXT_COLOR,lv_color_hex(fresh&&!ui.conflict?MUTED:WARN),0);
            if(ui.apply){lv_obj_t *face=lv_obj_get_child(ui.apply,1);refresh_num(face,LV_STYLE_BG_GRAD_DIR,LV_GRAD_DIR_NONE,0);refresh_num(face,LV_STYLE_RADIUS,15,0);
                if(confirmed)strcpy(text,"已设定");
                else if(ui.page==GBS_PAGE_VARIABLE_WEIGHT&&gbs_ui_variable_weight_valid(ui.draft))strcpy(text,"应用并记住");
                else if(steam)snprintf(text,sizeof(text),"应用 %d",ui.draft==126?1:ui.draft==128?2:3);
                else snprintf(text,sizeof(text),floor(ui.draft)==ui.draft?"应用 %.0f%s":"应用 %.1f%s",ui.draft,p->unit);
                refresh_label(lv_obj_get_child(face,0),text);
            }
        }
        return;
    }
    gbs_ui_stage_t stage=gbs_ui_stage(&ui.bridge);
    bool manual=amoled_manual_weight_view(stage);
    bool closing=stage==GBS_UI_CANCELING||(stage==GBS_UI_WAIT&&ui.bridge.job_deadline_ms>0&&ui.now>=ui.bridge.job_deadline_ms);
    bool timed_out=ui.flow.result_timed_out||(stage!=GBS_UI_CANCELING&&ui.bridge.job&&!ui.flow.result_grams&&ui.bridge.job_deadline_ms>0&&ui.now>=ui.bridge.job_deadline_ms);
    bool recipe=ui.bridge.recipe_enabled&&ui.bridge.recipe_current&&ui.bridge.job;
    const char *title="等待磨粉",*subtitle="联动就绪 · 等待新任务",*caption="等待开始";
    gbs_icon_t glyph=GBS_ICON_FLOW_WAIT_GRINDER;uint32_t color=MUTED;
    bool detail=false;
    if(stage==GBS_UI_WAIT) {
        title="等待本杯结果";subtitle="等待新的有效萃取记录";
        if(recipe) {
            if(!strcmp(ui.bridge.recipe_phase,"waiting")||!strcmp(ui.bridge.recipe_phase,"reading")) {title="读取配方中";subtitle="已收到新磨粉任务";}
            else if(!strcmp(ui.bridge.recipe_phase,"applying")) {title="同步咖啡机";snprintf(text,sizeof(text),"磨粉完成 · 配方 %.0fg",ui.bridge.recipe_grams);subtitle=text;}
            else if(!strcmp(ui.bridge.recipe_phase,"applied")) {snprintf(text,sizeof(text),"目标 %.0fg · 咖啡机已设定",ui.bridge.recipe_grams);subtitle=text;}
            else if(!strcmp(ui.bridge.recipe_phase,"unconfirmed")) {title="设定结果未确认";snprintf(text,sizeof(text),"目标 %.0fg · 请核对机器",ui.bridge.recipe_grams);subtitle=text;detail=true;}
            else if(!strcmp(ui.bridge.recipe_phase,"failed")||!strcmp(ui.bridge.recipe_phase,"probe_failed")) {
                title=ui.flow.nodes[GBS_FLOW_RECIPE]==GBS_FLOW_DONE?"咖啡机设定失败":"配方读取失败";subtitle="请核对机器目标液重";detail=true;color=RED;
            } else if(!strcmp(ui.bridge.recipe_phase,"machine_unavailable")) {title="咖啡机未接入";subtitle="请核对机器目标液重";detail=true;}
            else if(!strcmp(ui.bridge.recipe_phase,"skipped")) subtitle="本杯未自动设定液重";
        }
    } else if(stage==GBS_UI_SENDING) {
        title="回传磨豆机";snprintf(text,sizeof(text),"实测 %.1fg · %.1fs",ui.flow.result_grams,ui.flow.result_seconds);subtitle=text;
    } else if(stage==GBS_UI_RECEIVED) {
        title="本杯已完成";snprintf(text,sizeof(text),"磨豆机已接收 · %.1fg / %.1fs",ui.bridge.result_grams,ui.bridge.result_seconds);subtitle=text;
        caption="回传成功";glyph=GBS_ICON_CHECK;color=GREEN;
    } else if(stage==GBS_UI_UNKNOWN) {
        title="回传结果未确认";
        if(ui.bridge.unconfirmed_grams>0&&ui.bridge.unconfirmed_seconds>0) {
            snprintf(text,sizeof(text),"实测 %.1fg · %.1fs",ui.bridge.unconfirmed_grams,ui.bridge.unconfirmed_seconds);subtitle=text;
        } else subtitle="结果未确认 · 请查看详情";
        caption="查看详情";glyph=GBS_ICON_HELP_CIRCLE;color=WARN;detail=true;
    } else if(stage==GBS_UI_EXPIRED) {
        title="本杯已结束";subtitle=timed_out?"等待超时 · 本杯未回传":!strcmp(ui.bridge.outcome,"delivery_expired")?"回传超时 · 请查看详情":"本杯已结束 · 本杯未回传";caption=timed_out||!strcmp(ui.bridge.outcome,"delivery_expired")?"已超时":"已结束";glyph=GBS_ICON_FLOW_CLOCK;color=GOLD;
    } else if(stage==GBS_UI_INTERRUPTED) {
        title="本杯已中断";subtitle="已结束联动 · 不重放本杯";caption="查看详情";glyph=GBS_ICON_FLOW_CLOCK;color=WARN;detail=true;
    } else if(stage==GBS_UI_REFUSED) {
        title="本杯已结束";subtitle="任务已结束 · 请查看详情";caption="查看详情";glyph=GBS_ICON_FLOW_ALERT;color=RED;detail=true;
    } else if(stage==GBS_UI_INVALID) {
        title="萃取记录异常";subtitle="请检查电子秤与萃取记录";caption="查看详情";glyph=GBS_ICON_FLOW_ALERT;color=RED;detail=true;
    } else if(stage==GBS_UI_MONITOR) {title="仅监测模式";subtitle="读取机器状态 · 暂不联动";caption="仅监测";glyph=GBS_ICON_EYE;}
    else if(stage==GBS_UI_BOOT) {title="正在准备联动";subtitle="恢复配置与连接";caption="准备中";glyph=GBS_ICON_FLOW_CLOCK;}
    else if(stage==GBS_UI_BASELINE) {title="正在建立基线";subtitle="等待新任务 · 旧任务不回放";caption="准备中";glyph=GBS_ICON_FLOW_CLOCK;}
    else if(stage==GBS_UI_CLOCK) {title="正在同步时间";subtitle="校时完成后恢复联动";caption="校时中";glyph=GBS_ICON_FLOW_CLOCK;}
    else if(stage==GBS_UI_WIFI||stage==GBS_UI_GRINDER||stage==GBS_UI_CLOUD) {
        title="联动连接中断";subtitle=stage==GBS_UI_WIFI?"Wi-Fi 离线 · 等待恢复":stage==GBS_UI_GRINDER?"磨豆机离线 · 等待重连":"云端暂不可用 · 等待恢复";caption="查看连接";glyph=GBS_ICON_FLOW_CLOUD;
    } else if(stage==GBS_UI_STORAGE||stage==GBS_UI_PAIRING) {
        title=stage==GBS_UI_STORAGE?"账本需要处理":"身份需要处理";subtitle=stage==GBS_UI_STORAGE?"请保留原数据 · 检查存储":"请检查配对与其它客户端";caption="查看详情";glyph=GBS_ICON_FLOW_ALERT;color=RED;detail=true;
    }
    if(manual) {
        title=ui.control==GBS_UI_CONTROL_PENDING?"正在设定液重":ui.control==GBS_UI_CONTROL_CONFIRMED?"液重已设定":ui.control==GBS_UI_CONTROL_FAILED?"液重设定失败":"设定结果未确认";
        snprintf(text,sizeof(text),"手动目标 %.0fg%s",ui.requested,ui.control==GBS_UI_CONTROL_CONFIRMED?
            (ui.machine.stale?" · Cloud 已设定":" · 咖啡机已设定"):" · 请核对机器");subtitle=text;
        color=ui.control==GBS_UI_CONTROL_CONFIRMED?GREEN:ui.control==GBS_UI_CONTROL_FAILED?RED:WARN;
        glyph=ui.control==GBS_UI_CONTROL_CONFIRMED?GBS_ICON_CHECK:GBS_ICON_HAND_STOP;
        caption=ui.control==GBS_UI_CONTROL_PENDING?"待确认":ui.control==GBS_UI_CONTROL_CONFIRMED?"已设定":"查看详情";
        detail=ui.control>=GBS_UI_CONTROL_FAILED;
    }
    if(closing) {title="正在结束本杯";subtitle=timed_out?"本杯超时 · 尚无有效结果":"正在结束任务 · 等待确认";caption=timed_out?"本杯剩余":"取消剩余";color=GOLD;}
    const lv_font_t *headline=strlen(title)>18?&gbs_font_28_semibold:&gbs_font_30_semibold;
    refresh_font(ui.fields[F_GLANCE_STAGE],headline,0);
    lv_obj_set_y(ui.fields[F_GLANCE_STAGE],amoled_text_y(headline,234));
    set(F_GLANCE_STAGE,title);set(F_GLANCE_PATH,subtitle);
    const lv_font_t *subfont=stage==GBS_UI_RECEIVED&&!manual?&gbs_font_15:&gbs_font_16;
    refresh_font(ui.fields[F_GLANCE_PATH],subfont,0);lv_obj_set_y(ui.fields[F_GLANCE_PATH],amoled_text_y(subfont,270));
    bool timer=ui.bridge.job;
    bool question=stage==GBS_UI_UNKNOWN&&!manual&&!timer;
    refresh_hidden(am_phase_question,!question);
    refresh_hidden(ui.phase_icon,timer||question);refresh_hidden(ui.fields[F_GLANCE_VALUE],!timer);
    if(timer) {
        unsigned seconds=closing&&timed_out?0:gbs_ui_remaining(ui.bridge.pending?ui.bridge.answer_deadline_ms:ui.bridge.job_deadline_ms,ui.now);
        snprintf(text,sizeof(text),"%02u:%02u",seconds/60,seconds%60);set(F_GLANCE_VALUE,text);
        caption=closing&&!timed_out?"取消剩余":ui.bridge.pending&&!closing?"回传剩余":"本杯剩余";
        refresh_color(ui.fields[F_GLANCE_VALUE],LV_STYLE_TEXT_COLOR,lv_color_hex(closing?GOLD:TEXT),0);
    } else {icon_source(ui.phase_icon,glyph,40);refresh_color(ui.phase_icon,LV_STYLE_IMG_RECOLOR,lv_color_hex(color),0);}
    set(F_GLANCE_CAPTION,caption);
    refresh_color(ui.fields[F_GLANCE_CAPTION],LV_STYLE_TEXT_COLOR,lv_color_hex(stage==GBS_UI_RECEIVED&&!manual?GREEN:MUTED),0);
    refresh_hidden(am_detail,!detail);refresh_color(am_detail,LV_STYLE_IMG_RECOLOR,lv_color_hex(color==RED?RED:GOLD),0);
    const gbs_icon_t waiting[]={GBS_ICON_FLOW_WAIT_GRINDER,GBS_ICON_FLOW_FILE,GBS_ICON_FLOW_UPLOAD,GBS_ICON_FLOW_CLOCK,GBS_ICON_FLOW_UPLOAD};
    for(unsigned i=0;i<GBS_FLOW_COUNT;++i) {
        gbs_ui_flow_node_t node=ui.flow.nodes[i];
        bool timeout=timed_out&&i==GBS_FLOW_RESULT&&node!=GBS_FLOW_DONE;
        if(timed_out&&i==GBS_FLOW_DELIVERY&&node!=GBS_FLOW_DONE)node=GBS_FLOW_SKIPPED;
        uint32_t tint=node==GBS_FLOW_DONE?GREEN:timeout||node==GBS_FLOW_ACTIVE||node==GBS_FLOW_UNKNOWN?WARN:node==GBS_FLOW_FAILED?RED:AM_FUTURE;
        refresh_color(am_nodes[i],LV_STYLE_BG_COLOR,lv_color_hex(node==GBS_FLOW_DONE?GREEN:BG),0);
        refresh_hidden(am_node_rings[i],node==GBS_FLOW_DONE);
        refresh_color(am_node_rings[i],LV_STYLE_IMG_RECOLOR,lv_color_hex(tint),0);
        refresh_color(am_node_labels[i],LV_STYLE_TEXT_COLOR,lv_color_hex(tint),0);
        bool question=node==GBS_FLOW_UNKNOWN&&!timeout;
        refresh_hidden(am_node_questions[i],!question);refresh_hidden(am_node_icons[i],question);
        gbs_icon_t node_icon=node==GBS_FLOW_DONE?GBS_ICON_FLOW_DONE:timeout?GBS_ICON_FLOW_CLOCK:node==GBS_FLOW_FAILED?GBS_ICON_FLOW_ALERT:node==GBS_FLOW_SKIPPED?GBS_ICON_FLOW_MINUS:node==GBS_FLOW_ACTIVE&&i!=GBS_FLOW_RESULT?GBS_ICON_FLOW_REFRESH:waiting[i];
        int size=node==GBS_FLOW_DONE?22:20;
        icon_source(am_node_icons[i],node_icon,size);lv_obj_set_pos(am_node_icons[i],58+91*i-size/2,123-size/2);
        refresh_color(am_node_icons[i],LV_STYLE_IMG_RECOLOR,lv_color_hex(node==GBS_FLOW_DONE?BG:tint),0);
    }
    bool fresh=ui.machine.available&&!ui.machine.stale;
    for(unsigned i=0;i<2;++i) {
        unsigned minutes=gbs_ui_boiler_warmup_minutes(&ui.machine,i!=0,time(NULL));
        unsigned primary=i?F_STEAM:F_COFFEE,id=i?F_STEAM_TARGET:F_COFFEE_TARGET;
        refresh_hidden(am_boiler_clock[i],minutes==0);
        refresh_label(am_boiler_title[i],i?(minutes?"蒸汽预热":"蒸汽"):(minutes?"咖啡预热":"咖啡"));
        refresh_color(am_boiler_title[i],LV_STYLE_TEXT_COLOR,lv_color_hex(minutes?GOLD:MUTED),0);
        const lv_font_t *primary_font=minutes&&minutes<100?&gbs_number_45:&gbs_number_29;
        refresh_font(ui.fields[primary],primary_font,0);
        lv_obj_set_y(ui.fields[primary],amoled_text_y(primary_font,minutes?462:457));
        lv_obj_set_width(ui.fields[primary],minutes?94:(i?32:91));
        refresh_color(ui.fields[primary],LV_STYLE_TEXT_COLOR,lv_color_hex(minutes?GOLD:TEXT),0);
        if(minutes) {snprintf(text,sizeof(text),"%u",minutes);set(primary,text);}
        else if(!i)value(primary,fresh?ui.machine.coffee:NAN,"%.1f°");
        else value(primary,fresh&&!(ui.machine.steam_enabled_known&&!ui.machine.steam_enabled)?(ui.machine.steam_target==126?1:ui.machine.steam_target==128?2:ui.machine.steam_target==131?3:NAN):NAN,"%.0f");
        const lv_font_t *font=minutes?&gbs_font_18:i?&gbs_font_16:&gbs_number_18;
        refresh_font(ui.fields[id],font,0);lv_obj_set_y(ui.fields[id],amoled_text_y(font,minutes?462:457));
        const char *number=lv_label_get_text(ui.fields[primary]);
        int primary_x=i?305:68,unit_x=i?339:159;
        if(minutes) {
            int number_width=lv_txt_get_width(number,strlen(number),primary_font,0,LV_TEXT_FLAG_NONE);
            int unit_width=lv_txt_get_width("分钟",strlen("分钟"),font,0,LV_TEXT_FLAG_NONE);
            int left=(i?356:129)-(21+9+number_width+8+unit_width)/2;
            lv_obj_set_x(am_boiler_clock[i],left);primary_x=left+30;unit_x=primary_x+number_width+8;
        }
        lv_obj_set_x(ui.fields[primary],primary_x);
        lv_obj_set_x(ui.fields[id],unit_x);lv_obj_set_width(ui.fields[id],minutes?54:(i?114:80));
        refresh_color(ui.fields[id],LV_STYLE_TEXT_COLOR,lv_color_hex(minutes?GOLD:MUTED),0);
        if(minutes)strcpy(text,"分钟");
        else if(!fresh)strcpy(text,"—");
        else if(!i&&isfinite(ui.machine.coffee_target))snprintf(text,sizeof(text),"/ %.0f°",ui.machine.coffee_target);
        else if(i&&isfinite(ui.machine.steam)&&isfinite(ui.machine.steam_target))snprintf(text,sizeof(text),"%.0f° / %.0f°",ui.machine.steam,ui.machine.steam_target);
        else strcpy(text,"—");
        set(id,text);
    }
    // A retained selection is independent of whether Cloud currently accepts controls.
    double selected=ui.machine.weight,target=NAN;
    bool awaiting=false;
    if(recipe) {
        target=ui.bridge.recipe_grams;
        selected=!strcmp(ui.bridge.recipe_phase,"applied")?target:NAN;
        awaiting=!strcmp(ui.bridge.recipe_phase,"applying");
    }
    if(manual) {target=ui.requested;selected=ui.control==GBS_UI_CONTROL_CONFIRMED?target:NAN;awaiting=ui.control==GBS_UI_CONTROL_PENDING;}
    for(unsigned i=0;i<4;++i) {
        bool preview=i==3&&awaiting&&gbs_ui_variable_weight_valid(target);
        unsigned grams=preview?(unsigned)target:preset_value(i);
        char weight[8];snprintf(weight,sizeof(weight),"%u",grams);
        lv_obj_t *number=lv_obj_get_child(ui.preset_faces[i],0);
        if(strcmp(lv_label_get_text(number),weight))lv_label_set_text(number,weight);
        bool chosen=ui.machine.available&&isfinite(selected)&&selected==grams;
        bool pending=gbs_ui_can_control(GBS_ACTION_WEIGHT,&ui.machine)&&awaiting&&isfinite(target)&&target==grams&&!chosen;
        refresh_color(ui.preset_faces[i],LV_STYLE_BG_COLOR,lv_color_hex(chosen?GOLD:SURFACE),0);
        refresh_num(ui.preset_faces[i],LV_STYLE_BG_GRAD_DIR,LV_GRAD_DIR_NONE,0);
        refresh_color(ui.preset_faces[i],LV_STYLE_BORDER_COLOR,lv_color_hex(GOLD),0);
        refresh_num(ui.preset_faces[i],LV_STYLE_BORDER_WIDTH,pending?2:0,0);
        refresh_color(number,LV_STYLE_TEXT_COLOR,lv_color_hex(chosen?0x161c13:pending?GOLD:TEXT),0);
        if(i==3)refresh_color(lv_obj_get_child(ui.preset_faces[i],1),LV_STYLE_TEXT_COLOR,lv_color_hex(chosen?BG:MUTED),0);
        if(preview)lv_obj_add_state(ui.presets[i],LV_STATE_DISABLED);
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
