#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t hub_https_start(void);
bool hub_https_ready(void);
