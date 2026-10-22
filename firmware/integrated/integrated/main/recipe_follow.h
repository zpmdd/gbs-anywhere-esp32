#pragma once
#include <stdint.h>
typedef struct cJSON cJSON;
#include "esp_err.h"
#include <stdbool.h>
esp_err_t gbs_recipe_follow_init(void);
void gbs_recipe_follow_begin(const char *uuid);
void gbs_recipe_follow_cancel(void);
void gbs_recipe_follow_tick(bool accepted);
void gbs_recipe_follow_diagnostics(cJSON *parent);
bool gbs_recipe_follow_can_write(void);
bool gbs_recipe_follow_generation_can_write(uint32_t generation);
