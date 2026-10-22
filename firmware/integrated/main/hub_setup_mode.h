#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t hub_setup_mode_init(void);
bool hub_setup_mode_enabled(void);
bool hub_setup_mode_window_active(void);
void hub_setup_mode_mark_completed(void);
