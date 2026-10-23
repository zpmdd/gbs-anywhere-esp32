#include "gbs_device_policy.h"
#include <math.h>
#include <string.h>

void gbs_rotation_policy_init(gbs_rotation_policy_t *p)
{
    memset(p, 0, sizeof(*p));
    p->current = GBS_ORIENTATION_USB_RIGHT;
}

gbs_orientation_t gbs_rotation_policy_update(gbs_rotation_policy_t *p,
    bool enabled, bool touching, bool valid, float x, float y, float z, int64_t now)
{
    if (!enabled || touching || !valid || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
        p->candidate_valid = p->previous_valid = false;
        return p->current;
    }
    float norm = x*x + y*y + z*z;
    bool stable = p->previous_valid && now >= p->last_sample_ms && now - p->last_sample_ms <= 150 &&
        fabsf(x-p->previous_x) < .12f && fabsf(y-p->previous_y) < .12f && fabsf(z-p->previous_z) < .12f;
    p->previous_x=x; p->previous_y=y; p->previous_z=z;
    p->previous_valid=true; p->last_sample_ms=now;
    float ax=fabsf(x), ay=fabsf(y);
    if (!stable || norm < .72f || norm > 1.28f || fabsf(z) > .65f ||
        (ax < .70f && ay < .70f) || fabsf(ax-ay) < .25f) {
        p->candidate_valid=false;
        return p->current;
    }
    gbs_orientation_t candidate = ay > ax ?
        (y > 0 ? GBS_ORIENTATION_USB_BOTTOM : GBS_ORIENTATION_USB_TOP) :
        (x < 0 ? GBS_ORIENTATION_USB_RIGHT : GBS_ORIENTATION_USB_LEFT);
    if (!p->candidate_valid || candidate != p->candidate) {
        p->candidate=candidate; p->candidate_since_ms=now; p->candidate_valid=true;
    } else if (now-p->candidate_since_ms >= 700) {
        p->current=candidate;
    }
    return p->current;
}

uint8_t gbs_device_panel_madctl(gbs_orientation_t orientation)
{
    /* Physical panel calibration: 0xA0 is USB-bottom and 0x00 is USB-right.
     * The CST9220 raw axes match USB-right, verified with four corner presses. */
    static const uint8_t commands[]={0x00,0xA0,0xC0,0x60};
    return (unsigned)orientation<4?commands[orientation]:commands[0];
}

bool gbs_device_map_touch(gbs_orientation_t orientation, uint16_t x, uint16_t y,
    uint16_t *screen_x, uint16_t *screen_y)
{
    if (x >= 480 || y >= 480 || !screen_x || !screen_y) return false;
    switch (orientation) {
    case GBS_ORIENTATION_USB_RIGHT: *screen_x=x; *screen_y=y; break;
    case GBS_ORIENTATION_USB_BOTTOM: *screen_x=479-y; *screen_y=x; break;
    case GBS_ORIENTATION_USB_LEFT: *screen_x=479-x; *screen_y=479-y; break;
    case GBS_ORIENTATION_USB_TOP: *screen_x=y; *screen_y=479-x; break;
    default: return false;
    }
    return true;
}

void gbs_device_decode_pmu(gbs_device_status_t *s, bool valid, uint8_t s1, uint8_t s2,
    bool percent_valid, uint8_t percent, int64_t now)
{
    gbs_orientation_t orientation=s->orientation;
    memset(s,0,sizeof(*s)); s->orientation=orientation;
    if (!valid) return;
    s->valid=true; s->observed_ms=now;
    s->battery_present_known=true; s->battery_present=(s1 & (1u<<3)) != 0;
    if (!s->battery_present) return;
    s->battery_percent_known=percent_valid && percent <= 100;
    if (s->battery_percent_known) s->battery_percent=percent;
    /* REG01[6:5] is current direction; bit7 is reserved. REG00[5]
     * must also report a valid VBUS source before displaying charging. */
    unsigned charge_state=(s2>>5)&3u;
    s->charging_known=charge_state <= 2;
    s->charging=s->charging_known && (s1 & (1u<<5)) && charge_state == 1;
}

void gbs_device_update_pmu(gbs_device_status_t *s, bool valid, uint8_t s1, uint8_t s2,
    bool percent_valid, uint8_t percent, int64_t now)
{
    if(valid||!s->valid||now<s->observed_ms||now-s->observed_ms>5000)
        gbs_device_decode_pmu(s,valid,s1,s2,percent_valid,percent,now);
}

bool gbs_device_touch_idle(const gbs_touch_policy_t *s, bool invalid_report, bool irq_high)
{
    return s && invalid_report && irq_high && !s->physical_down;
}

void gbs_device_touch_update(gbs_touch_policy_t *s, bool valid, bool down,
    uint16_t x, uint16_t y, bool blank)
{
    if (!valid) return;
    s->physical_down=down;
    if (down) { s->x=x; s->y=y; }
    if (down && blank) s->wake_gesture=true;
    s->visible_down=down && !s->wake_gesture;
    if (!down && !s->inhibited) s->wake_gesture=false;
}

void gbs_device_touch_inhibit(gbs_touch_policy_t *s, bool inhibited)
{
    s->inhibited=inhibited;
    if (inhibited) { s->wake_gesture=true; s->visible_down=false; }
}

gbs_button_event_t gbs_device_button_update(gbs_button_policy_t *s,
    bool down, bool blank, int64_t now)
{
    if (!s->initialized) {
        s->initialized=true; s->down=s->raw_down=down; s->raw_since_ms=now;
        return GBS_BUTTON_NONE;
    }
    if (down!=s->raw_down || now<s->raw_since_ms) {
        s->raw_down=down; s->raw_since_ms=now;
        return GBS_BUTTON_NONE;
    }
    if (now-s->raw_since_ms<30) return GBS_BUTTON_NONE;
    if (s->down!=down) {
        s->down=down;
        if (!down) {
            bool short_press=s->armed && !s->consumed;
            s->armed=true; s->consumed=false;
            return short_press ? GBS_BUTTON_SHORT : GBS_BUTTON_NONE;
        }
        s->pressed_since_ms=now; s->consumed=false;
        if (s->armed && blank) {
            s->consumed=true;
            return GBS_BUTTON_WAKE;
        }
    }
    if (!down) s->armed=true;
    if (down && s->armed && !s->consumed && now-s->pressed_since_ms>=1500) {
        s->consumed=true;
        return GBS_BUTTON_LONG;
    }
    return GBS_BUTTON_NONE;
}
