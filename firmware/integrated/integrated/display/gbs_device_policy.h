#pragma once
#include "gbs_device.h"

typedef struct {
    gbs_orientation_t current, candidate;
    int64_t candidate_since_ms, last_sample_ms;
    float previous_x, previous_y, previous_z;
    bool candidate_valid, previous_valid;
} gbs_rotation_policy_t;

void gbs_rotation_policy_init(gbs_rotation_policy_t *policy);
/* Board-calibrated acceleration in g: -X USB-right, +Y USB-bottom,
 * +X USB-left, -Y USB-top. Z measures tilt out of the screen plane. */
gbs_orientation_t gbs_rotation_policy_update(gbs_rotation_policy_t *policy,
    bool enabled, bool touching, bool sample_valid, float x, float y, float z, int64_t now_ms);
bool gbs_device_map_touch(gbs_orientation_t orientation, uint16_t x, uint16_t y,
    uint16_t *screen_x, uint16_t *screen_y);
uint8_t gbs_device_panel_madctl(gbs_orientation_t orientation);
void gbs_device_decode_pmu(gbs_device_status_t *status, bool status_valid,
    uint8_t status1, uint8_t status2, bool percent_valid, uint8_t percent, int64_t now_ms);

// Invalid reads preserve the last valid sample only within the existing 5 s freshness window.
void gbs_device_update_pmu(gbs_device_status_t *status, bool status_valid,
    uint8_t status1, uint8_t status2, bool percent_valid, uint8_t percent, int64_t now_ms);

typedef struct {
    bool physical_down, visible_down, wake_gesture, inhibited;
    uint16_t x, y;
} gbs_touch_policy_t;

/* Only an idle report marker with inactive IRQ and an already released
 * gesture is harmless. Transport failures and held gestures are not idle. */
bool gbs_device_touch_idle(const gbs_touch_policy_t *state,
    bool invalid_report, bool irq_high);
/* Invalid polls retain the entire gesture, including the LVGL state and wake
 * suppression, until a trusted sample arrives. Coordinates are screen-local. */
void gbs_device_touch_update(gbs_touch_policy_t *state, bool sample_valid,
    bool down, uint16_t x, uint16_t y, bool blank);
/* Cancel the LVGL gesture before inhibiting; rearm only after all keys and a
 * trusted touch sample are released. */
void gbs_device_touch_inhibit(gbs_touch_policy_t *state, bool inhibited);

typedef enum { GBS_BUTTON_NONE, GBS_BUTTON_WAKE, GBS_BUTTON_SHORT, GBS_BUTTON_LONG } gbs_button_event_t;
typedef struct {
    bool initialized, raw_down, down, armed, consumed;
    int64_t raw_since_ms, pressed_since_ms;
} gbs_button_policy_t;

/* Zero-init; a boot-held key must be released before it can emit any event. */
gbs_button_event_t gbs_device_button_update(gbs_button_policy_t *state,
    bool down, bool blank, int64_t now_ms);
