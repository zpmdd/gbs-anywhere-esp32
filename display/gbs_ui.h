#pragma once
#include "gbs_ui_model.h"
#include "lvgl.h"

typedef enum {
    GBS_PAGE_HOME, GBS_PAGE_LINK, GBS_PAGE_RECORD, GBS_PAGE_TASK,
    GBS_PAGE_SETTINGS, GBS_PAGE_MACHINE, GBS_PAGE_WEIGHT, GBS_PAGE_COFFEE,
    GBS_PAGE_STEAM, GBS_PAGE_BBW, GBS_PAGE_PREBREW, GBS_PAGE_POWER,
    GBS_PAGE_STANDBY, GBS_PAGE_CLEANING, GBS_PAGE_SCHEDULE, GBS_PAGE_COFFEES,
    GBS_PAGE_MODE, GBS_PAGE_CONNECTIONS, GBS_PAGE_CONNECTION,
    GBS_PAGE_DIAGNOSTICS, GBS_PAGE_DISPLAY, GBS_PAGE_ABOUT, GBS_PAGE_SETUP,
    GBS_PAGE_DOSE1, GBS_PAGE_DOSE2, GBS_PAGE_BBW_MODE,
    GBS_PAGE_PREBREW_MODE, GBS_PAGE_PREBREW_ON, GBS_PAGE_PREBREW_OFF,
    GBS_PAGE_STANDBY_ENABLED, GBS_PAGE_STANDBY_MINUTES, GBS_PAGE_STANDBY_MODE,
    GBS_PAGE_SCHEDULE_DETAIL, GBS_PAGE_COFFEE_DETAIL,
    GBS_PAGE_SCHEDULE_EDIT, GBS_PAGE_SCHEDULE_ON, GBS_PAGE_SCHEDULE_OFF, GBS_PAGE_SCHEDULE_DAYS, GBS_PAGE_COUNT
} gbs_ui_page_t;
typedef bool (*gbs_ui_submit_t)(gbs_ui_action_t action, double value, uint32_t request_id,
                                const gbs_ui_machine_t *expected, void *context);

void gbs_ui_init(gbs_ui_submit_t submit, void *context);
typedef bool (*gbs_ui_read_t)(gbs_ui_data_kind_t kind,void *context);
void gbs_ui_set_reader(gbs_ui_read_t reader);
void gbs_ui_set_data(const gbs_ui_data_t *data);
void gbs_ui_set_bridge(const gbs_ui_bridge_t *bridge);
void gbs_ui_set_machine(const gbs_ui_machine_t *machine);
void gbs_ui_control_result(uint32_t request_id, gbs_ui_control_t result);
void gbs_ui_tick(int64_t now_ms);
void gbs_ui_show(gbs_ui_page_t page);
gbs_ui_page_t gbs_ui_page(void);
bool gbs_ui_keep_awake(void);
void gbs_ui_set_preferences(unsigned brightness, bool dim);
unsigned gbs_ui_brightness(void);
bool gbs_ui_dim_enabled(void);

typedef bool (*gbs_ui_schedule_submit_t)(const gbs_ui_schedule_t *expected,const gbs_ui_schedule_t *requested,uint32_t request_id,void *context);
void gbs_ui_set_schedule_submit(gbs_ui_schedule_submit_t submit);
