#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define LAMABUDDY_SETUP_AP_PREFIX "LamaBuddy-Hub-"

esp_err_t hub_wifi_init(void);
esp_err_t hub_wifi_refresh_mdns_services(void);
const char *hub_wifi_mode(void);
const char *hub_wifi_ap_ssid(void);
const char *hub_wifi_ip_address(void);
bool hub_wifi_station_has_ip(void);
