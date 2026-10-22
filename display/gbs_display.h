#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef bool (*gbs_display_state_fn)(char *buffer, size_t size);
typedef void (*gbs_display_mode_fn)(bool active);
esp_err_t gbs_display_start(gbs_display_state_fn state, gbs_display_mode_fn mode);
