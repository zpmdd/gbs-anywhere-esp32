#include "hub_setup_mode.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "hub_config.h"

static const char *TAG = "hub_setup_mode";
static const int64_t SETUP_WINDOW_US = 10 * 60 * 1000000LL;

static bool s_enabled;
static bool s_completed;
static int64_t s_started_us;

esp_err_t hub_setup_mode_init(void)
{
    s_completed = false;
    s_enabled = !hub_config_has_wifi_credentials();
    s_started_us = s_enabled ? esp_timer_get_time() : 0;

    if (s_enabled) {
        ESP_LOGI(TAG, "setup mode enabled");
    }
    return ESP_OK;
}

bool hub_setup_mode_enabled(void)
{
    return s_enabled && !s_completed;
}

bool hub_setup_mode_window_active(void)
{
    if (!hub_setup_mode_enabled() || s_started_us <= 0) {
        return false;
    }

    int64_t elapsed_us = esp_timer_get_time() - s_started_us;
    return elapsed_us >= 0 && elapsed_us < SETUP_WINDOW_US;
}

void hub_setup_mode_mark_completed(void)
{
    if (!s_completed) {
        s_completed = true;
        s_enabled = false;
        ESP_LOGI(TAG, "setup mode completed");
    }
}
