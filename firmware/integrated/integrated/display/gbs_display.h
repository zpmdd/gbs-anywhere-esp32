#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef HUB_AMOLED_216
#define GBS_DISPLAY_WIDTH 480
#define GBS_DISPLAY_HEIGHT 480
#else
#define GBS_DISPLAY_WIDTH 240
#define GBS_DISPLAY_HEIGHT 280
#endif
#define GBS_DISPLAY_FRAME_BYTES (GBS_DISPLAY_WIDTH*GBS_DISPLAY_HEIGHT*2)

typedef bool (*gbs_display_state_fn)(char *buffer, size_t size);
typedef bool (*gbs_display_mode_fn)(bool active);
esp_err_t gbs_display_start(gbs_display_state_fn state, gbs_display_mode_fn mode);
bool gbs_display_diagnostics_json(char *buffer, size_t size);
bool gbs_display_copy_frame(void *buffer, size_t size);
