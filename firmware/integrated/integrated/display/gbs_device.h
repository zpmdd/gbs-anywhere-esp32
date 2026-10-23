#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GBS_ORIENTATION_USB_RIGHT = 0,
    GBS_ORIENTATION_USB_BOTTOM = 1,
    GBS_ORIENTATION_USB_LEFT = 2,
    GBS_ORIENTATION_USB_TOP = 3,
} gbs_orientation_t;

typedef struct {
    bool valid;
    bool battery_present_known, battery_present;
    bool battery_percent_known;
    unsigned battery_percent;
    bool charging_known, charging;
    int64_t observed_ms;
    gbs_orientation_t orientation;
} gbs_device_status_t;

typedef enum { GBS_TONE_NONE, GBS_TONE_SUCCESS, GBS_TONE_FAILURE, GBS_TONE_NOTICE } gbs_tone_t;
