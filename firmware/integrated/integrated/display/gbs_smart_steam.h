#pragma once
#include "gbs_ui_model.h"
#include "esp_err.h"
#include <stddef.h>

esp_err_t gbs_smart_steam_init(void);
void gbs_smart_steam_tick(const gbs_ui_machine_t *machine);
esp_err_t gbs_smart_steam_enable(bool enabled);
gbs_steam_status_t gbs_smart_steam_status(void);
bool gbs_smart_steam_json(char *buffer, size_t size);
void gbs_smart_steam_manual_override(void);
bool gbs_smart_steam_write_allowed(void);
