#include "gbs_steam_policy.h"

uint32_t gbs_steam_window(int64_t unix_seconds)
{
    if (unix_seconds < 1735689600 || unix_seconds > 4102444800) return 0;
    int64_t local = unix_seconds + 8 * 3600;
    unsigned minute = (unsigned)(local % 86400) / 60;
    unsigned mode = minute >= 420 && minute < 720 ? 1 : minute >= 840 && minute < 1200 ? 2 : 0;
    return mode ? (uint32_t)(local / 86400) * 3 + mode : 0;
}

bool gbs_steam_policy_step(gbs_steam_policy_t *p, const gbs_steam_input_t *in)
{
    if (!p->status.enabled) { p->status.phase = GBS_STEAM_DISABLED; return false; }
    if (!in->clock_ready) { p->status.phase = GBS_STEAM_CLOCK; return false; }
    if (!in->fresh || !in->online) {
        p->status.phase = GBS_STEAM_WAIT_MACHINE;
        if (!in->network_ready) p->offline_since = 0;
        else if (!p->offline_since) p->offline_since = in->now_ms;
        else if (in->now_ms - p->offline_since >= 60000) p->session = false;
        return false;
    }
    p->offline_since = 0;
    uint32_t window = gbs_steam_window(in->unix_seconds);
    if (!p->session) { p->session = true; p->session_window = window; }
    if (!window || window != p->session_window) {
        p->status.phase = GBS_STEAM_OUTSIDE;
        return false;
    }
    p->status.target = window % 3 == 2;
    // The journal survives sleep/reconnect; a manual choice owns the whole window.
    if (p->record.window == window) {
        // Late readback resolves the state without replaying the uncertain write.
        if (p->record.outcome == GBS_STEAM_UNKNOWN && in->steam_enabled == p->status.target)
            p->record.outcome = GBS_STEAM_READY;
        p->status.phase = (gbs_steam_phase_t)p->record.outcome;
        if (p->status.phase == GBS_STEAM_READY && in->steam_enabled != p->status.target)
            p->record.outcome = p->status.phase = GBS_STEAM_MANUAL;
        return false;
    }
    if (!in->idle) { p->status.phase = GBS_STEAM_WAIT_IDLE; return false; }
    if (in->steam_enabled == p->status.target) {
        p->record = (gbs_steam_record_t){window, GBS_STEAM_READY};
        p->status.phase = GBS_STEAM_READY;
        return false;
    }
    p->status.phase = GBS_STEAM_APPLYING;
    return true;
}
