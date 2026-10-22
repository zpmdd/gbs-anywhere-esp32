#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_ota_ops.h"

typedef struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    size_t expected_size;
    size_t written;
    bool active;
    bool grinder_paused;
    char message[128];
} hub_ota_update_t;

esp_err_t hub_ota_status_json(char *body, size_t body_len);
esp_err_t hub_ota_begin_update(size_t expected_size, hub_ota_update_t *update);
esp_err_t hub_ota_write(hub_ota_update_t *update, const void *data, size_t data_len);
esp_err_t hub_ota_finish(hub_ota_update_t *update);
void hub_ota_abort(hub_ota_update_t *update);
