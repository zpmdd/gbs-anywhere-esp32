#include "gbs_ui_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    gbs_ui_bridge_t s = {0};
    assert(gbs_ui_stage(&s) == GBS_UI_BOOT);
    assert(gbs_ui_parse_bridge("{\"phase\":\"waiting_for_coffee\",\"have_job\":true,\"grind_uuid\":\"current\","
        "\"recipe_follow\":{\"enabled\":true,\"phase\":\"applied\",\"grind_uuid\":\"current\",\"target_grams\":30}}", &s));
    assert(s.target_grams == 30 && s.recipe_enabled && !strcmp(s.recipe_phase, "applied"));
    assert(gbs_ui_parse_bridge("{\"phase\":\"waiting_for_coffee\",\"have_job\":true,\"grind_uuid\":\"current\","
        "\"recipe_follow\":{\"grind_uuid\":\"old\",\"target_grams\":30}}", &s));
    assert(s.target_grams == 0);
    assert(!gbs_ui_parse_bridge("{broken", &s));
    assert(!gbs_ui_parse_bridge("{\"phase\":2}", &s));
    assert(gbs_ui_parse_bridge("{\"phase\":\"ready\",\"mode\":\"active\",\"wifi\":true,"
        "\"grinder\":true,\"cloud\":true,\"storage\":true,\"uptime_ms\":10000,"
        "\"job_deadline_ms\":190000,\"have_job\":true}", &s));
    assert(gbs_ui_stage(&s) == GBS_UI_WAIT);
    s.invalid_coffee = true; assert(gbs_ui_stage(&s) == GBS_UI_INVALID);
    s.pending = true; assert(gbs_ui_stage(&s) == GBS_UI_SENDING);
    s.job = false; strcpy(s.outcome, "invalid_coffee"); assert(gbs_ui_stage(&s) == GBS_UI_INVALID);
    s.job = true; s.pending = s.invalid_coffee = false; s.outcome[0] = 0;
    assert(gbs_ui_remaining(s.job_deadline_ms, 10000) == 180);
    assert(gbs_ui_remaining(s.job_deadline_ms, 189001) == 1);
    assert(gbs_ui_remaining(s.job_deadline_ms, 200000) == 0);
    // Re-reading a stale snapshot cannot restart the budget or declare completion.
    assert(gbs_ui_remaining(s.job_deadline_ms, 400000) == 0);
    assert(gbs_ui_stage(&s) == GBS_UI_WAIT);
    s.pending = true; assert(gbs_ui_stage(&s) == GBS_UI_SENDING);
    s.canceling = true; assert(gbs_ui_stage(&s) == GBS_UI_CANCELING);
    s.job = false; strcpy(s.outcome, "delivery_unknown");
    s.grinder = false; assert(gbs_ui_stage(&s) == GBS_UI_UNKNOWN);
    s.grinder = true; assert(gbs_ui_stage(&s) == GBS_UI_UNKNOWN);
    s.job = true; assert(gbs_ui_stage(&s) == GBS_UI_CANCELING);
    s.storage = false; assert(gbs_ui_stage(&s) == GBS_UI_STORAGE);
    s.storage = true; s.pairing = true; assert(gbs_ui_stage(&s) == GBS_UI_PAIRING);
    s.pairing = false; s.job = false; s.outcome[0] = 0; s.baseline = true;
    assert(gbs_ui_stage(&s) == GBS_UI_BASELINE);
    s.baseline = false; s.active = false; assert(gbs_ui_stage(&s) == GBS_UI_MONITOR);
    assert(gbs_ui_backlight(80, true, 61000, false) == 20);
    assert(gbs_ui_backlight(80, true, 121000, false) == 0);
    assert(gbs_ui_backlight(80, true, 121000, true) == 80);
    assert(gbs_ui_backlight(80, false, 121000, false) == 80);
    bool wake = false;
    assert(!gbs_ui_touch(true, true, &wake));
    assert(!gbs_ui_touch(true, false, &wake));
    assert(!gbs_ui_touch(false, false, &wake));
    assert(gbs_ui_touch(true, false, &wake));
    assert(!gbs_ui_busy(&s, GBS_UI_CONTROL_UNKNOWN, false));
    assert(gbs_ui_busy(&s, GBS_UI_CONTROL_PENDING, false));
    puts("display model: deadlines, precedence, missing data and wake gesture passed");
    return 0;
}
