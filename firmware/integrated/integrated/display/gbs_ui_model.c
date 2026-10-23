#include "gbs_ui_model.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

unsigned gbs_ui_boiler_warmup_minutes(const gbs_ui_machine_t *machine, bool steam, int64_t now_unix)
{
    int64_t ready=steam?machine->steam_ready_unix:machine->coffee_ready_unix;
    if(!machine->available||machine->stale||machine->machine_mode==0||now_unix<1577836800||
        machine->boiler_observed_unix<=0||now_unix<machine->boiler_observed_unix||
        now_unix-machine->boiler_observed_unix>=300||ready<=now_unix||
        (steam&&machine->steam_enabled_known&&!machine->steam_enabled)) return 0;
    // Reject implausible dates instead of showing an unbounded countdown on the small screen.
    if(ready-now_unix>86400) return 0;
    return (unsigned)((ready-now_unix+59)/60);
}

bool gbs_ui_scale_is_stale(const gbs_ui_machine_t *machine, int64_t now_ms)
{
    return machine->scale_observed_ms<=0||now_ms<machine->scale_observed_ms||
        now_ms-machine->scale_observed_ms>=GBS_UI_SCALE_FRESH_MS;
}

bool gbs_ui_schedule_equal(const gbs_ui_schedule_t *a,const gbs_ui_schedule_t *b)
{
    return !strcmp(a->id,b->id)&&a->on_minutes==b->on_minutes&&a->off_minutes==b->off_minutes&&
        a->enabled==b->enabled&&a->steam==b->steam&&a->days==b->days;
}

static bool flag(const cJSON *j, const char *name)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, name));
}

static double number(const cJSON *j, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 &&
           v->valuedouble <= 9007199254740991.0 ? v->valuedouble : 0;
}

bool gbs_ui_parse_bridge(const char *json, gbs_ui_bridge_t *out)
{
    if (!json || !out) return false;
    cJSON *j = cJSON_Parse(json);
    const cJSON *phase = cJSON_GetObjectItemCaseSensitive(j, "phase");
    if (!cJSON_IsObject(j) || !cJSON_IsString(phase) || strlen(phase->valuestring) >= sizeof(out->phase)) {
        cJSON_Delete(j); return false;
    }
    gbs_ui_bridge_t next = {0};
    strcpy(next.phase, phase->valuestring);
    next.valid = strcmp(next.phase, "booting") != 0 && strcmp(next.phase, "configuration_required") != 0;
    next.wifi = flag(j, "wifi"); next.grinder = flag(j, "grinder");
    next.cloud = flag(j, "cloud"); next.storage = flag(j, "storage");
    next.pairing = flag(j, "pairing_required"); next.job = flag(j, "have_job");
    next.pending = flag(j, "pending"); next.canceling = flag(j, "canceling");
    next.baseline = flag(j, "baseline_pending");
    next.mode_failed = flag(j, "mode_change_failed");
    next.invalid_coffee = flag(j, "coffee_invalid");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(j, "mode");
    next.active = cJSON_IsString(mode) && !strcmp(mode->valuestring, "active");
    const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(j, "last_outcome");
    if (cJSON_IsString(outcome)) snprintf(next.outcome, sizeof(next.outcome), "%s", outcome->valuestring);
    next.observed_ms = (int64_t)number(j, "uptime_ms");
    next.job_deadline_ms = (int64_t)number(j, "job_deadline_ms");
    next.answer_deadline_ms = (int64_t)number(j, "answer_deadline_ms");
    next.retry_at_ms = (int64_t)number(j, "grinder_retry_at_ms");
    next.target_grams = number(j, "target_grams"); next.target_seconds = number(j, "target_seconds");
    const cJSON *recipe = cJSON_GetObjectItemCaseSensitive(j, "recipe_follow");
    const cJSON *recipe_phase = cJSON_GetObjectItemCaseSensitive(recipe, "phase");
    next.recipe_enabled = flag(recipe, "enabled");
    if (cJSON_IsString(recipe_phase)) snprintf(next.recipe_phase, sizeof(next.recipe_phase), "%s", recipe_phase->valuestring);
    next.recipe_grams = number(recipe, "target_grams");
    const cJSON *job_id = cJSON_GetObjectItemCaseSensitive(j, "grind_uuid");
    const cJSON *recipe_id = cJSON_GetObjectItemCaseSensitive(recipe, "grind_uuid");
    if(cJSON_IsString(job_id)&&strlen(job_id->valuestring)<sizeof(next.job_id)) strcpy(next.job_id,job_id->valuestring);
    next.recipe_current=next.job&&next.job_id[0]&&cJSON_IsString(recipe_id)&&!strcmp(next.job_id,recipe_id->valuestring);
    if (next.job && cJSON_IsString(job_id) && cJSON_IsString(recipe_id) &&
        !strcmp(job_id->valuestring, recipe_id->valuestring) && next.recipe_grams >= 1 && next.recipe_grams <= 200)
        next.target_grams = next.recipe_grams;
    next.result_grams = number(j, "last_success_grams"); next.result_seconds = number(j, "last_success_seconds");
    next.latest_grams = number(j, "latest_grams"); next.latest_seconds = number(j, "latest_seconds");
    next.unconfirmed_grams = number(j, "unconfirmed_grams"); next.unconfirmed_seconds = number(j, "unconfirmed_seconds");
    next.successes = (unsigned)fmin(number(j, "successes"), UINT32_MAX);
    next.free_internal = (unsigned)fmin(number(j, "free_internal"), UINT32_MAX);
    next.largest_internal = (unsigned)fmin(number(j, "largest_internal"), UINT32_MAX);
    *out = next;
    cJSON_Delete(j); return true;
}

gbs_ui_stage_t gbs_ui_stage(const gbs_ui_bridge_t *s)
{
    if (!s->valid) return GBS_UI_BOOT;
    if (!s->storage) return GBS_UI_STORAGE;
    if (s->pairing) return GBS_UI_PAIRING;
    if (!strcmp(s->phase, "clock_waiting")) return GBS_UI_CLOCK;
    if (s->job) return s->pending ? (s->canceling ? GBS_UI_CANCELING : GBS_UI_SENDING) : s->invalid_coffee ? GBS_UI_INVALID : GBS_UI_WAIT;
    // A connection recovery must not turn an ambiguous delivery into success.
    if (!strcmp(s->outcome, "delivery_unknown")) return GBS_UI_UNKNOWN;
    if (!strcmp(s->outcome, "invalid_coffee")) return GBS_UI_INVALID;
    if (!strcmp(s->outcome, "interrupted_by_restart") || !strcmp(s->outcome, "stopped_by_mode_change")) return GBS_UI_INTERRUPTED;
    if (!strcmp(s->outcome, "delivery_expired") || !strcmp(s->outcome, "canceled")) return GBS_UI_EXPIRED;
    if (!strcmp(s->outcome, "delivery_refused") || !strcmp(s->outcome, "job_disappeared")) return GBS_UI_REFUSED;
    if (!s->wifi) return GBS_UI_WIFI;
    if (!s->grinder) return GBS_UI_GRINDER;
    if (!s->cloud) return GBS_UI_CLOUD;
    if (s->baseline) return GBS_UI_BASELINE;
    if (!s->active) return GBS_UI_MONITOR;
    return !strcmp(s->outcome, "success") ? GBS_UI_RECEIVED : GBS_UI_READY;
}

unsigned gbs_ui_remaining(int64_t deadline_ms, int64_t now_ms)
{
    if (deadline_ms <= now_ms || deadline_ms <= 0 || now_ms < 0) return 0;
    int64_t seconds = (deadline_ms - now_ms + 999) / 1000;
    return (unsigned)(seconds > 5999 ? 5999 : seconds);
}

bool gbs_ui_busy(const gbs_ui_bridge_t *s, gbs_ui_control_t control, bool confirming)
{
    return s->job || control == GBS_UI_CONTROL_PENDING || confirming;
}

unsigned gbs_ui_backlight(unsigned brightness, bool dim_enabled, int64_t idle_ms, bool busy)
{
    if (brightness < 25) brightness = 25;
    if (brightness > 100) brightness = 100;
    if (busy || !dim_enabled) return brightness;
    if (idle_ms >= 120000) return 0;
    return idle_ms >= 60000 ? brightness / 4 : brightness;
}

bool gbs_ui_touch(bool down, bool blank, bool *wake_gesture)
{
    if (down && blank) *wake_gesture = true;
    if (*wake_gesture) { if (!down) *wake_gesture = false; return false; }
    return down;
}

bool gbs_ui_machine_action(gbs_ui_action_t action)
{
    return (unsigned)action < GBS_ACTION_COUNT && (GBS_UI_MACHINE_ACTIONS & (1u << action));
}

double gbs_ui_action_value(gbs_ui_action_t action, const gbs_ui_machine_t *m)
{
    if (!m->available) return NAN;
    switch (action) {
    case GBS_ACTION_WEIGHT: return m->weight;
    case GBS_ACTION_COFFEE: return m->coffee_target;
    case GBS_ACTION_STEAM: return m->steam_target;
    case GBS_ACTION_STEAM_ENABLED: return m->steam_enabled_known ? m->steam_enabled : NAN;
    case GBS_ACTION_DOSE1: return m->dose1;
    case GBS_ACTION_DOSE2: return m->dose2;
    case GBS_ACTION_BBW_MODE: return m->dose_mode <= 3 ? m->dose_mode : NAN;
    case GBS_ACTION_PREBREW_MODE: return m->prebrew_mode <= 2 ? m->prebrew_mode : NAN;
    case GBS_ACTION_PREBREW_ON: return m->prebrew_on;
    case GBS_ACTION_PREBREW_OFF: return m->prebrew_off;
    case GBS_ACTION_POWER: return m->machine_mode <= 2 ? m->machine_mode : NAN;
    case GBS_ACTION_STANDBY_ENABLED: return m->standby_enabled_known ? m->standby_enabled : NAN;
    case GBS_ACTION_STANDBY_MINUTES: return m->standby_minutes;
    case GBS_ACTION_STANDBY_MODE: return m->standby_mode <= 1 ? m->standby_mode : NAN;
    default: return NAN;
    }
}

static bool same(double a, double b)
{
    return (isnan(a) && isnan(b)) || (isfinite(a) && isfinite(b) && fabs(a - b) < 0.01);
}

bool gbs_ui_action_unchanged(gbs_ui_action_t action, const gbs_ui_machine_t *before,
                            const gbs_ui_machine_t *after)
{
    if (action == GBS_ACTION_WEIGHT || (action >= GBS_ACTION_DOSE1 && action <= GBS_ACTION_BBW_MODE))
        return before->dose_mode == after->dose_mode && same(before->dose1, after->dose1) && same(before->dose2, after->dose2);
    if (action >= GBS_ACTION_PREBREW_MODE && action <= GBS_ACTION_PREBREW_OFF)
        return before->prebrew_mode == after->prebrew_mode && same(before->prebrew_on, after->prebrew_on) && same(before->prebrew_off, after->prebrew_off);
    if (action >= GBS_ACTION_STANDBY_ENABLED && action <= GBS_ACTION_STANDBY_MODE)
        return before->standby_enabled_known == after->standby_enabled_known && before->standby_enabled == after->standby_enabled &&
            before->standby_mode == after->standby_mode && same(before->standby_minutes, after->standby_minutes);
    return gbs_ui_machine_action(action) && same(gbs_ui_action_value(action, before), gbs_ui_action_value(action, after));
}
