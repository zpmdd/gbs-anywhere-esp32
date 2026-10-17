#pragma once
#include "gbs_ui_model.h"
#include "esp_err.h"

esp_err_t gbs_hub_start(const char *machine_serial);
bool gbs_hub_submit(gbs_ui_action_t action,double value,uint32_t request_id);
bool gbs_hub_read(gbs_ui_machine_t *machine,uint32_t *request_id,gbs_ui_control_t *result);
