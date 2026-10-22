#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool merge_enabled;
    uint32_t field_mask;
} hub_cloud_websocket_config_t;

esp_err_t hub_cloud_websocket_start(void);
esp_err_t hub_cloud_websocket_apply_config(const hub_cloud_websocket_config_t *config);
const char *hub_cloud_websocket_diagnostics_json(void);
const char *hub_cloud_websocket_state_json(void);
bool hub_cloud_websocket_merge_active(void);
