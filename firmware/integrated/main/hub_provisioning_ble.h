#pragma once

#include "esp_err.h"

esp_err_t hub_provisioning_ble_init(void);
esp_err_t hub_provisioning_ble_start(void);
void hub_provisioning_ble_stop(void);
