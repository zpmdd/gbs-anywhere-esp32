#pragma once

#include "esp_err.h"

esp_err_t hub_state_keeper_start(void);
void hub_state_keeper_touch(void);
void hub_state_keeper_request_recovery(void);
