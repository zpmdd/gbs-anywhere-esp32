#include "gbs_ui_model.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(j, "mode");
    next.active = cJSON_IsString(mode) && !strcmp(mode->valuestring, "active");
    const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(j, "last_outcome");
    if (cJSON_IsString(outcome)) snprintf(next.outcome, sizeof(next.outcome), "%s", outcome->valuestring);
    next.observed_ms = (int64_t)number(j, "uptime_ms");
    next.job_deadline_ms = (int64_t)number(j, "job_deadline_ms");
    next.answer_deadline_ms = (int64_t)number(j, "answer_deadline_ms");
    next.retry_at_ms = (int64_t)number(j, "grinder_retry_at_ms");
    next.target_grams = number(j, "target_grams"); next.target_seconds = number(j, "target_seconds");
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
    if (s->job) return s->pending ? (s->canceling ? GBS_UI_CANCELING : GBS_UI_SENDING) : GBS_UI_WAIT;
    // A connection recovery must not turn an ambiguous delivery into success.
    if (!strcmp(s->outcome, "delivery_unknown")) return GBS_UI_UNKNOWN;
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
