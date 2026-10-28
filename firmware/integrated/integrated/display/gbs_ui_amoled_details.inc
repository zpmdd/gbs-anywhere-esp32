/* Native 480 px information pages; data ownership and navigation remain shared. */
enum { AD_OLD, AD_NEW, AD_STATE, AD_NOTICE, AD_TIMER_TITLE, AD_TIMER_VALUE, AD_HINT,
       AD_MASS, AD_SECONDS, AD_CLOUD, AD_RECEIPT, AD_TITLE, AD_READY,
       AD_WIFI, AD_GRINDER, AD_CONNECTION_CLOUD, AD_SCALE, AD_COUNT };
static struct {
    gbs_ui_page_t page;
    lv_obj_t *fields[AD_COUNT], *phase, *empty, *measurement, *unit[2], *edit;
    const lv_font_t *metric_font[2];
    int metric_x[2], metric_y[2];
} am_details;

static lv_obj_t *am_detail_label(lv_obj_t *parent,unsigned id,int x,int y,int width,
                                 const lv_font_t *font,uint32_t color)
{
    return am_details.fields[id]=label(parent,"",x,y,width,font,color);
}

static void am_detail_set(unsigned id,const char *text)
{
    if(am_details.fields[id]) refresh_label(am_details.fields[id],text);
}

static void am_detail_color(unsigned id,uint32_t color)
{
    if(am_details.fields[id]) refresh_color(am_details.fields[id],LV_STYLE_TEXT_COLOR,lv_color_hex(color),0);
}

static lv_obj_t *am_detail_button(lv_obj_t *parent,const char *text,int x,int y,int width,int action,bool filled)
{
    lv_obj_t *b=button(parent,text,x,y,width,64,action,filled);
    lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_NONE,0);
    lv_obj_set_style_radius(b,12,0);
    lv_obj_set_style_text_font(lv_obj_get_child(b,0),&gbs_font_23,0);
    return b;
}

static lv_obj_t *am_detail_row(lv_obj_t *parent,const char *title,const char *subtitle,
                               const char *right,int y,int height,int action)
{
    lv_obj_t *row=button(parent,"",28,y,424,height,action,false);
    lv_obj_set_style_bg_opa(row,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(row,0,0);
    lv_obj_set_style_bg_grad_dir(row,LV_GRAD_DIR_NONE,0);
    if(action<0) lv_obj_clear_flag(row,LV_OBJ_FLAG_CLICKABLE);
    label(row,title,0,subtitle?9:19,right&&right[0]?280:390,&gbs_font_23,TEXT);
    if(subtitle) label(row,subtitle,0,38,330,&gbs_font_18,MUTED);
    if(right) {
        lv_obj_t *value=label(row,right,288,20,action<0?136:106,&gbs_font_23,MUTED);
        lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_RIGHT,0);
    }
    if(action>=0) icon(row,GBS_ICON_CHEVRON_RIGHT,402,(height-22)/2,22,MUTED);
    rule(row,0,height-1,424,1);
    return row;
}

static void am_detail_metric(lv_obj_t *parent,unsigned id,unsigned unit,const char *caption,
                             const char *suffix,int x,int y,const lv_font_t *font,uint32_t color)
{
    am_detail_label(parent,id,x,y,194,font,color);
    am_details.metric_font[unit]=font;
    am_details.metric_x[unit]=x;
    am_details.metric_y[unit]=y;
    am_details.unit[unit]=label(parent,suffix,x+150,y,32,&gbs_unit_24,color);
    if(caption) label(parent,caption,x,y+69,194,&gbs_font_18,MUTED);
}

static void am_detail_number(unsigned id,unsigned unit,double number,const char *format)
{
    char text[32];
    if(isfinite(number)) snprintf(text,sizeof(text),format,number); else strcpy(text,"—");
    lv_obj_t *value=am_details.fields[id];
    refresh_label(value,text);
    const lv_font_t *font=am_details.metric_font[unit];
    int width=lv_txt_get_width(text,strlen(text),font,0,LV_TEXT_FLAG_NONE);
    if(width>164) { font=&gbs_number_40;width=lv_txt_get_width(text,strlen(text),font,0,LV_TEXT_FLAG_NONE); }
    if(width>164) { font=&gbs_number_28;width=lv_txt_get_width(text,strlen(text),font,0,LV_TEXT_FLAG_NONE); }
    refresh_font(value,font,0);
    lv_obj_set_x(am_details.unit[unit],am_details.metric_x[unit]+width+5);
    lv_obj_set_y(am_details.unit[unit],am_details.metric_y[unit]+font->line_height-font->base_line-
                 gbs_unit_24.line_height+gbs_unit_24.base_line);
    if(isfinite(number)) refresh_hidden(am_details.unit[unit],false);
    else refresh_hidden(am_details.unit[unit],true);
}

static bool am_detail_ready(void)
{
    return ui.bridge.valid&&ui.bridge.storage&&!ui.bridge.pairing&&!ui.bridge.baseline&&
           ui.bridge.wifi&&ui.bridge.grinder&&ui.bridge.cloud&&strcmp(ui.bridge.phase,"clock_waiting");
}

static const char *am_detail_preparation(void)
{
    if(!ui.bridge.valid) return "恢复配置与连接";
    if(!ui.bridge.storage) return "请保留原数据，检查存储";
    if(ui.bridge.pairing) return "请检查配对与其它客户端";
    if(!strcmp(ui.bridge.phase,"clock_waiting")) return "正在校时，稍后恢复联动";
    if(!ui.bridge.wifi) return "等待 Wi-Fi 连接";
    if(!ui.bridge.grinder) return "等待磨豆机重新连接";
    if(!ui.bridge.cloud) return "等待 Cloud 状态更新";
    if(ui.bridge.baseline) return "恢复任务基线，旧任务不重放";
    return ui.bridge.active?"准备完成，可开始新杯":"仅监测，不回传结果";
}

static void am_detail_link(void)
{
    amoled_header("本杯联动");
    label(lv_scr_act(),"机器上次目标",28,110,210,&gbs_font_18,MUTED);
    am_detail_label(lv_scr_act(),AD_TITLE,272,110,180,&gbs_font_18,MUTED);
    am_detail_metric(lv_scr_act(),AD_OLD,0,NULL,"g",28,148,&gbs_number_52,TEXT);
    am_detail_metric(lv_scr_act(),AD_NEW,1,NULL,"g",272,148,&gbs_number_52,GOLD);
    icon(lv_scr_act(),GBS_ICON_ARROW_RIGHT,228,143,28,0x818a7b);
    am_details.phase=icon(lv_scr_act(),GBS_ICON_CLOCK,28,222,36,GOLD);
    am_detail_label(lv_scr_act(),AD_STATE,78,218,374,&gbs_font_32,GOLD);
    am_detail_label(lv_scr_act(),AD_NOTICE,28,272,424,&gbs_font_19,MUTED);
    rule(lv_scr_act(),28,307,424,1);
    am_detail_label(lv_scr_act(),AD_TIMER_TITLE,28,338,268,&gbs_font_20,TEXT);
    lv_obj_t *time=am_detail_label(lv_scr_act(),AD_TIMER_VALUE,282,333,170,&gbs_number_30,TEXT);
    lv_obj_set_style_text_align(time,LV_TEXT_ALIGN_RIGHT,0);
    am_detail_label(lv_scr_act(),AD_HINT,28,365,424,&gbs_font_18,MUTED);
    const char *captions[]={"本杯记录","连接"};
    const gbs_icon_t glyphs[]={GBS_ICON_HISTORY,GBS_ICON_LINK};
    for(unsigned i=0;i<2;++i) {
        lv_obj_t *b=am_detail_button(lv_scr_act(),captions[i],28+218*i,392,206,i?GBS_PAGE_CONNECTIONS:GBS_PAGE_RECORD,false);
        icon(b,glyphs[i],i?64:40,21,23,TEXT);
        lv_obj_t *text=lv_obj_get_child(b,0);
        lv_obj_set_align(text,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(text,i?95:72,18);
        lv_obj_set_width(text,i?90:126);lv_obj_set_style_text_align(text,LV_TEXT_ALIGN_LEFT,0);
    }
}

static void am_detail_record(void)
{
    amoled_header("本杯记录");
    am_details.empty=box(lv_scr_act(),28,100,424,282,BG);
    icon(am_details.empty,GBS_ICON_COFFEE,180,48,64,MUTED);
    centered(label(am_details.empty,"尚无测量",0,142,424,&gbs_font_32,TEXT));
    centered(am_detail_label(am_details.empty,AD_NOTICE,0,208,424,&gbs_font_19,MUTED));
    am_details.measurement=box(lv_scr_act(),0,94,480,286,BG);
    am_detail_label(am_details.measurement,AD_TITLE,28,0,424,&gbs_font_18,MUTED);
    am_detail_metric(am_details.measurement,AD_MASS,0,"实测液重","g",28,27,&gbs_number_55,TEXT);
    am_detail_metric(am_details.measurement,AD_SECONDS,1,"萃取时长","s",254,27,&gbs_number_55,TEXT);
    label(am_details.measurement,"Cloud 结果",28,164,250,&gbs_font_23,TEXT);
    lv_obj_t *cloud=am_detail_label(am_details.measurement,AD_CLOUD,282,164,170,&gbs_font_23,GREEN);
    lv_obj_set_style_text_align(cloud,LV_TEXT_ALIGN_RIGHT,0);
    rule(am_details.measurement,28,206,424,1);
    label(am_details.measurement,"磨豆机回执",28,232,250,&gbs_font_23,TEXT);
    lv_obj_t *receipt=am_detail_label(am_details.measurement,AD_RECEIPT,282,232,170,&gbs_font_23,GOLD);
    lv_obj_set_style_text_align(receipt,LV_TEXT_ALIGN_RIGHT,0);
    rule(am_details.measurement,28,274,424,1);
    am_detail_button(lv_scr_act(),"云端历史",28,392,424,GBS_PAGE_COFFEES,false);
}

static void am_detail_collection(void)
{
    gbs_ui_data_kind_t kind=data_kind();
    const gbs_ui_data_t *data=&ui.data[kind];
    bool plans=kind==GBS_DATA_SCHEDULE;
    amoled_header(plans?"定时计划":"云端历史");
    lv_obj_t *refresh=button(lv_scr_act(),"",388,14,64,64,DATA_REFRESH,false);
    lv_obj_set_style_bg_opa(refresh,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(refresh,0,0);
    icon(refresh,GBS_ICON_HISTORY,19,19,26,TEXT);
    if(data->state==GBS_DATA_LOADING) lv_obj_add_state(refresh,LV_STATE_DISABLED);
    label(lv_scr_act(),data_status(data),28,94,424,&gbs_font_18,data->state==GBS_DATA_READY?MUTED:WARN);
    if(!data->valid||!data->count) {
        bool empty=data->valid&&data->state==GBS_DATA_READY;
        icon(lv_scr_act(),plans?GBS_ICON_CALENDAR:GBS_ICON_COFFEE,208,157,64,MUTED);
        centered(label(lv_scr_act(),empty?(plans?"暂无定时计划":"暂无出杯记录"):
                       data->state==GBS_DATA_LOADING?"正在读取":data->state==GBS_DATA_UNSUPPORTED?"尚不支持":"暂时无法显示",
                       28,248,424,&gbs_font_32,TEXT));
        centered(label(lv_scr_act(),plans?"在咖啡机应用中管理计划":"记录由咖啡机 Cloud 提供",28,306,424,&gbs_font_19,MUTED));
        if(data->state!=GBS_DATA_LOADING) am_detail_button(lv_scr_act(),"重新读取",28,392,424,DATA_REFRESH,false);
        return;
    }
    unsigned first=ui.data_page[kind]*3;
    char title[80],subtitle[96],right[32];
    for(unsigned row=0;row<3&&first+row<data->count;++row) {
        if(plans) {
            const gbs_ui_schedule_t *p=&data->items.schedules[first+row];
            snprintf(title,sizeof(title),"%02u:%02u — %02u:%02u",p->on_minutes/60,p->on_minutes%60,p->off_minutes/60,p->off_minutes%60);
            days_text(p->days,subtitle,sizeof(subtitle));
            snprintf(right,sizeof(right),"%s",p->enabled?"已开启":"已关闭");
        } else {
            const gbs_ui_coffee_t *c=&data->items.coffees[first+row];
            snprintf(title,sizeof(title),"%.10s %.5s",c->time,c->time+11);
            if(isfinite(c->dose)) snprintf(subtitle,sizeof(subtitle),"设定剂量 %.1f",c->dose);
            else snprintf(subtitle,sizeof(subtitle),"测量字段不完整");
            if(isfinite(c->seconds)) snprintf(right,sizeof(right),"%.1f s",c->seconds);
            else strcpy(right,"—");
        }
        lv_obj_t *row_obj=am_detail_row(lv_scr_act(),title,subtitle,right,121+76*row,76,DATA_ROW+row);
        if(!plans) lv_obj_set_style_text_font(lv_obj_get_child(row_obj,3),&gbs_amoled_number_20,0);
    }
    label(lv_scr_act(),plans?"仅编辑已有计划，不新增或删除。":"剂量是设定值，不代替实测液重。",28,357,424,&gbs_font_18,MUTED);
    if(data->count>3) {
        lv_obj_t *previous=am_detail_button(lv_scr_act(),"‹",28,392,64,DATA_PREVIOUS,false);
        lv_obj_t *next=am_detail_button(lv_scr_act(),"›",388,392,64,DATA_NEXT,false);
        if(!first) lv_obj_add_state(previous,LV_STATE_DISABLED);
        if(first+3>=data->count) lv_obj_add_state(next,LV_STATE_DISABLED);
        snprintf(title,sizeof(title),"%u / %u",ui.data_page[kind]+1,(data->count+2)/3);
        centered(label(lv_scr_act(),title,106,413,268,&gbs_amoled_number_24,MUTED));
    }
}

static void am_detail_schedule(void)
{
    const gbs_ui_schedule_t *p=&ui.selected_schedule;
    amoled_header("计划详情");
    label(lv_scr_act(),ui.selected_stale?"旧记录 · 返回列表刷新":"计划时间 · 按机器设置",28,94,424,&gbs_font_18,ui.selected_stale?WARN:MUTED);
    char text[96];
    snprintf(text,sizeof(text),"%02u:%02u",p->on_minutes/60,p->on_minutes%60);
    am_detail_row(lv_scr_act(),"开启时间",NULL,text,124,64,-1);
    snprintf(text,sizeof(text),"%02u:%02u",p->off_minutes/60,p->off_minutes%60);
    am_detail_row(lv_scr_act(),"关闭时间",NULL,text,188,64,-1);
    days_text(p->days,text,sizeof(text));
    label(lv_scr_act(),"重复",28,273,80,&gbs_font_23,TEXT);
    lv_obj_t *days=label(lv_scr_act(),text,110,273,342,&gbs_font_20,MUTED);
    lv_obj_set_style_text_align(days,LV_TEXT_ALIGN_RIGHT,0);
    rule(lv_scr_act(),28,315,424,1);
    snprintf(text,sizeof(text),"计划%s · 蒸汽%s",p->enabled?"开启":"关闭",p->steam?"开启":"关闭");
    label(lv_scr_act(),text,28,334,424,&gbs_font_19,MUTED);
    am_details.edit=am_detail_button(lv_scr_act(),"编辑计划",28,392,424,GBS_PAGE_SCHEDULE_EDIT,true);
}

static void am_detail_coffee(void)
{
    const gbs_ui_coffee_t *c=&ui.selected_coffee;
    amoled_header("历史记录详情");
    char text[96]; snprintf(text,sizeof(text),"%.10s %.5s UTC%s",c->time,c->time+11,ui.selected_stale?" · 旧记录":"");
    label(lv_scr_act(),text,28,94,424,&gbs_font_18,ui.selected_stale?WARN:MUTED);
    am_detail_metric(lv_scr_act(),AD_MASS,0,"设定剂量值","",28,148,&gbs_number_55,TEXT);
    am_detail_metric(lv_scr_act(),AD_SECONDS,1,"云端时长","s",254,148,&gbs_number_55,TEXT);
    am_detail_number(AD_MASS,0,c->dose,"%.1f");
    am_detail_number(AD_SECONDS,1,c->seconds,"%.1f");
    if(isfinite(c->temperature)) snprintf(text,sizeof(text),"%.1f°C",c->temperature); else strcpy(text,"—");
    am_detail_row(lv_scr_act(),"目标温度",NULL,text,270,64,-1);
    am_detail_row(lv_scr_act(),"BBW 模式",NULL,choice_label(GBS_ACTION_BBW_MODE,c->mode),334,64,-1);
    label(lv_scr_act(),"设定剂量不代替出杯测量。",28,416,424,&gbs_font_19,MUTED);
}

static void am_detail_connections(bool detail)
{
    amoled_header(detail?"连接详情":"连接状态");
    if(detail) {
        label(lv_scr_act(),"当前连接与本杯交付分开显示。",28,100,424,&gbs_font_19,MUTED);
        label(lv_scr_act(),"恢复连接不会重放未知结果。",28,131,424,&gbs_font_19,MUTED);
    }
    const char *names[]={"磨豆机","咖啡机 Cloud","A 秤","Wi-Fi"};
    const unsigned ids[]={AD_GRINDER,AD_CONNECTION_CLOUD,AD_SCALE,AD_WIFI};
    const char *notes[]={"本机维护连接","控制与结果仍需要云端",NULL,NULL};
    int y=detail?175:90;
    for(unsigned i=0;i<4;++i) {
        int height=detail?54:i<2?76:64;
        lv_obj_t *row=am_detail_row(lv_scr_act(),names[i],detail?NULL:notes[i],NULL,y,height,detail?-1:i==2?GBS_PAGE_SCALE:GBS_PAGE_CONNECTION);
        lv_obj_t *value=am_detail_label(row,ids[i],280,detail?16:22,detail?144:114,&gbs_font_23,MUTED);
        lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_RIGHT,0);
        y+=height;
    }
    if(detail) am_detail_label(lv_scr_act(),AD_READY,28,410,424,&gbs_font_19,MUTED);
    else am_detail_row(lv_scr_act(),"诊断与设备信息",NULL,NULL,y,64,GBS_PAGE_DIAGNOSTICS);
}

static bool amoled_details_build(void)
{
    memset(&am_details,0,sizeof(am_details)); am_details.page=ui.page;
    switch(ui.page) {
    case GBS_PAGE_LINK: case GBS_PAGE_TASK: am_detail_link(); break;
    case GBS_PAGE_RECORD: am_detail_record(); break;
    case GBS_PAGE_COFFEES: case GBS_PAGE_SCHEDULE: am_detail_collection(); break;
    case GBS_PAGE_COFFEE_DETAIL: am_detail_coffee(); break;
    case GBS_PAGE_SCHEDULE_DETAIL: am_detail_schedule(); break;
    case GBS_PAGE_CONNECTIONS: am_detail_connections(false); break;
    case GBS_PAGE_CONNECTION: am_detail_connections(true); break;
    default: am_details.page=GBS_PAGE_COUNT; return false;
    }
    ui.native_layout=true; return true;
}

static void am_detail_link_refresh(void)
{
    gbs_ui_stage_t stage=gbs_ui_stage(&ui.bridge);
    bool manual=amoled_manual_weight_view(stage);
    bool recipe=ui.bridge.job&&ui.bridge.recipe_enabled&&stage==GBS_UI_WAIT;
    double old=(ui.bridge.job||manual)?ui.previous_weight:NAN;
    double target=manual?ui.requested:ui.bridge.job&&ui.bridge.recipe_grams>0?ui.bridge.recipe_grams:NAN;
    am_detail_number(AD_OLD,0,old,"%.0f"); am_detail_number(AD_NEW,1,target,"%.0f");
    am_detail_set(AD_TITLE,manual?"手动请求":"本杯配方");
    const char *state="就绪",*notice=am_detail_preparation();
    gbs_icon_t glyph=GBS_ICON_GRINDER; uint32_t color=GREEN;
    if(stage==GBS_UI_WAIT) { state="等结果";notice="等待本杯 Cloud 测量结果";glyph=GBS_ICON_CLOCK;color=GOLD; }
    else if(stage==GBS_UI_SENDING) { state="回传中";notice="等待磨豆机明确接收";glyph=GBS_ICON_ARROW_RIGHT;color=GOLD; }
    else if(stage==GBS_UI_RECEIVED) { state="已接收";notice="磨豆机已接收；是否调整由它决定";glyph=GBS_ICON_CHECK; }
    else if(stage==GBS_UI_UNKNOWN) { state="未知";notice="可能已送达，不自动重发";glyph=GBS_ICON_HELP_CIRCLE;color=RED; }
    else if(stage==GBS_UI_INTERRUPTED) { state="已中断";notice="不会重放重启前的杯";glyph=GBS_ICON_ALERT_TRIANGLE;color=GOLD; }
    else if(stage==GBS_UI_CANCELING||stage==GBS_UI_EXPIRED||stage==GBS_UI_REFUSED) { state=ui.bridge.job?"收尾中":"已结束";notice="取消与交付结果请分别核对";glyph=GBS_ICON_CLOCK;color=GOLD; }
    else if(stage==GBS_UI_INVALID) { state="结果异常";notice="请检查电子秤与萃取记录";glyph=GBS_ICON_ALERT_TRIANGLE;color=RED; }
    else if(stage==GBS_UI_MONITOR) { state="仅监测";glyph=GBS_ICON_EYE;color=MUTED; }
    else if(stage!=GBS_UI_READY) { state=stage==GBS_UI_WIFI||stage==GBS_UI_GRINDER||stage==GBS_UI_CLOUD?"离线":stage==GBS_UI_CLOCK?"校时中":"准备中";glyph=GBS_ICON_CLOCK;color=GOLD; }
    if(recipe) {
        notice="拉杆前，请核对机器克重";
        if(!strcmp(ui.bridge.recipe_phase,"waiting")||!strcmp(ui.bridge.recipe_phase,"reading")) { state="读取";glyph=GBS_ICON_FILE_SEARCH; }
        else if(!strcmp(ui.bridge.recipe_phase,"applying")) { state="同步中";glyph=GBS_ICON_REFRESH; }
        else if(!strcmp(ui.bridge.recipe_phase,"applied")) { state="已同步";glyph=GBS_ICON_CHECK;color=GREEN;notice="配方设定已完成"; }
        else if(!strcmp(ui.bridge.recipe_phase,"unconfirmed")) { state="未知";glyph=GBS_ICON_HELP_CIRCLE;color=RED;notice="切重结果未知，不自动重放"; }
        else if(!strcmp(ui.bridge.recipe_phase,"failed")||!strcmp(ui.bridge.recipe_phase,"probe_failed")) { state="失败";glyph=GBS_ICON_ALERT_TRIANGLE;color=RED;notice="未自动切重，请检查机器克重"; }
        else if(!strcmp(ui.bridge.recipe_phase,"machine_unavailable")) { state="未接入";glyph=GBS_ICON_CLOUD_OFF;notice="机器状态未准备，未自动切重"; }
        else if(!strcmp(ui.bridge.recipe_phase,"skipped")) { state="未切重";glyph=GBS_ICON_HELP_CIRCLE;notice="本次跟随已跳过，请核对克重"; }
    }
    if(manual) {
        state=ui.control==GBS_UI_CONTROL_PENDING?"同步中":ui.control==GBS_UI_CONTROL_CONFIRMED?"已设定":ui.control==GBS_UI_CONTROL_FAILED?"失败":"未知";
        glyph=ui.control==GBS_UI_CONTROL_CONFIRMED?GBS_ICON_CHECK:GBS_ICON_HAND_STOP;
        color=ui.control==GBS_UI_CONTROL_CONFIRMED?GREEN:ui.control==GBS_UI_CONTROL_FAILED||ui.control==GBS_UI_CONTROL_UNKNOWN?RED:GOLD;
        notice=ui.control==GBS_UI_CONTROL_CONFIRMED?"设定已完成，本次自动跟随已取消":"本次自动跟随已取消，请核对克重";
        if(!ui.bridge.job) notice=ui.control==GBS_UI_CONTROL_CONFIRMED?"设定已完成":"请核对克重";
    }
    am_detail_set(AD_STATE,state);am_detail_color(AD_STATE,color);am_detail_set(AD_NOTICE,notice);
    icon_source(am_details.phase,glyph,36);
    refresh_color(am_details.phase,LV_STYLE_IMG_RECOLOR,lv_color_hex(color),0);
    char text[64];
    if(stage==GBS_UI_SENDING&&ui.bridge.unconfirmed_grams>0) {
        am_detail_set(AD_TIMER_TITLE,"测量已收到");
        snprintf(text,sizeof(text),"%.1f g",ui.bridge.unconfirmed_grams);am_detail_set(AD_TIMER_VALUE,text);
        am_detail_set(AD_HINT,"回传中 · 等待磨豆机回执");
    } else if(ui.bridge.job) {
        unsigned seconds=gbs_ui_remaining(ui.bridge.pending?ui.bridge.answer_deadline_ms:ui.bridge.job_deadline_ms,ui.now);
        am_detail_set(AD_TIMER_TITLE,ui.bridge.pending?(ui.bridge.canceling?"等待取消确认":"等待回传确认"):"等待本杯结果");
        snprintf(text,sizeof(text),"%02u:%02u",seconds/60,seconds%60);am_detail_set(AD_TIMER_VALUE,text);
        am_detail_set(AD_HINT,ui.bridge.pending?(ui.bridge.canceling?"20 秒取消窗口":"120 秒回传窗口"):"不是萃取计时");
    } else {
        am_detail_set(AD_TIMER_TITLE,stage==GBS_UI_RECEIVED?(manual?"最近结果":"测量已收到"):"当前没有进行中的任务");
        if(stage==GBS_UI_RECEIVED) snprintf(text,sizeof(text),"%.1f g",ui.bridge.result_grams); else strcpy(text,"—");
        am_detail_set(AD_TIMER_VALUE,text);
        am_detail_set(AD_HINT,am_detail_ready()?am_detail_preparation():"连接与任务基线准备中");
    }
}

static void am_detail_record_refresh(void)
{
    gbs_ui_stage_t stage=gbs_ui_stage(&ui.bridge);
    bool unconfirmed=(stage==GBS_UI_SENDING||stage==GBS_UI_UNKNOWN)&&ui.bridge.unconfirmed_grams>0&&ui.bridge.unconfirmed_seconds>0;
    bool received=stage==GBS_UI_RECEIVED&&ui.bridge.successes>0;
    bool history=!ui.bridge.job&&!ui.bridge.outcome[0]&&ui.bridge.successes>0;
    bool measured=unconfirmed||received||history;
    if(measured) { refresh_hidden(am_details.empty,true);refresh_hidden(am_details.measurement,false); }
    else { refresh_hidden(am_details.empty,false);refresh_hidden(am_details.measurement,true); }
    am_detail_set(AD_NOTICE,stage==GBS_UI_INTERRUPTED?"重启前的杯不会重放":stage==GBS_UI_SENDING?"正在回传，测量详情尚不可读":"尚未收到本杯有效结果");
    if(!measured) return;
    am_detail_number(AD_MASS,0,unconfirmed?ui.bridge.unconfirmed_grams:ui.bridge.result_grams,"%.1f");
    am_detail_number(AD_SECONDS,1,unconfirmed?ui.bridge.unconfirmed_seconds:ui.bridge.result_seconds,"%.1f");
    am_detail_set(AD_TITLE,history?"最近成功记录 · 非当前杯":"是否调整由磨豆机决定");
    am_detail_set(AD_CLOUD,"已收到");
    am_detail_set(AD_RECEIPT,stage==GBS_UI_UNKNOWN?"未知":unconfirmed?"等待中":"已接收");
    am_detail_color(AD_RECEIPT,stage==GBS_UI_UNKNOWN?RED:unconfirmed?GOLD:GREEN);
}

static void amoled_details_refresh(void)
{
    if(ui.page!=am_details.page||ui.confirm||ui.discard) return;
    if(ui.page==GBS_PAGE_LINK||ui.page==GBS_PAGE_TASK) am_detail_link_refresh();
    else if(ui.page==GBS_PAGE_RECORD) am_detail_record_refresh();
    else if(ui.page==GBS_PAGE_CONNECTIONS||ui.page==GBS_PAGE_CONNECTION) {
        am_detail_set(AD_GRINDER,ui.bridge.grinder?"已连接":"重连中");
        am_detail_set(AD_CONNECTION_CLOUD,ui.bridge.cloud?"正常":"离线");
        am_detail_set(AD_WIFI,ui.bridge.wifi?"已连接":"离线");
        am_detail_set(AD_SCALE,ui.machine.scale_stale&&ui.machine.scale_observed_ms>0?"已过期":
                      !ui.machine.scale_connected_known||ui.machine.scale_stale?"未知":ui.machine.scale_connected?"已连接":"未连接");
        am_detail_set(AD_READY,am_detail_preparation());
    } else if(ui.page==GBS_PAGE_SCHEDULE_DETAIL&&am_details.edit) {
        bool editable=ui.data[GBS_DATA_SCHEDULE].editable&&ui.data[GBS_DATA_SCHEDULE].state==GBS_DATA_READY&&
                      !ui.selected_stale&&ui.selected_schedule.id[0]&&ui.control!=GBS_UI_CONTROL_PENDING;
        if(editable) lv_obj_clear_state(am_details.edit,LV_STATE_DISABLED);else lv_obj_add_state(am_details.edit,LV_STATE_DISABLED);
    }
}
