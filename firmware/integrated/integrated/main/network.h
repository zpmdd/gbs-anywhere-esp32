#pragma once
#include "esp_err.h"
#include "logic.h"
#include "recipe_log.h"

typedef struct {
    char serial[65], grinder_host[16], device_id[37], grinder_cert[4096];
    uint32_t pairing_id;
} gbs_config_t;

extern gbs_config_t gbs_config;
typedef struct {
    char operation[24];
    int http_status;
    esp_err_t transport;
} gbs_request_info_t;
extern gbs_request_info_t gbs_grinder_request, gbs_cloud_request;
esp_err_t gbs_load_config(void);
void gbs_uuid(char output[37]);
int64_t gbs_now_ms(void);
esp_err_t gbs_cloud_connect(void);
esp_err_t gbs_cloud_coffee(gbs_coffee_t *coffee);
// Response ownership passes to caller. API status is checked separately.
esp_err_t gbs_grinder(const char *endpoint, const cJSON *body, cJSON **response,
                      char status[80]);
esp_err_t gbs_grinder_recipe(const char *uuid, gbs_recipe_match_t *match,
                             unsigned *bytes, unsigned *elapsed_ms);
