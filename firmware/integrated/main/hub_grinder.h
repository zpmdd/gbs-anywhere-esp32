#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t hub_grinder_start(void);
bool hub_grinder_state(char *buffer, size_t size);
bool hub_grinder_set_active(bool enabled);
// Read-only guard for optional work; never reserves or delays job admission.
bool hub_grinder_busy(void);

bool hub_grinder_begin_maintenance(void);
void hub_grinder_end_maintenance(void);
bool hub_grinder_configure_recipe(bool enabled, const char *owner_password);
bool hub_grinder_probe_recipe(const char *uuid);
bool hub_grinder_begin_recipe_probe(void);
void hub_grinder_end_recipe_probe(void);
