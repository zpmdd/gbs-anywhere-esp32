#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hub_command_deadline.h"
#include "hub_config.h"
#include "hub_schedule.h"

typedef enum {
    HUB_CLOUD_STATUS_UNCONFIGURED = 0,
    HUB_CLOUD_STATUS_CONFIGURED,
    HUB_CLOUD_STATUS_NEEDS_LIVE_VALIDATION,
    HUB_CLOUD_STATUS_AUTHENTICATED,
    HUB_CLOUD_STATUS_ERROR,
} hub_cloud_status_t;

typedef struct {
    esp_err_t err;
    int http_status;
    bool request_started;
    int attempts;
    size_t response_bytes;
    uint32_t first_bytes;
} hub_cloud_command_attempt_t;

typedef struct {
    bool ok;
    int duration_ms;
    int upstream_attempts;
    int http_status;
    bool request_started;
    bool schedule_changed;
    bool preset_changed;
    char message[128];
} hub_cloud_command_result_t;

typedef struct {
    bool has_cached_snapshot;
    bool is_connected;
    bool has_complete_bbw;
    bool is_ready;
    bool refresh_pending;
    int64_t last_refresh_ms;
    int64_t scale_observed_ms;
    double dose1, dose2;
} hub_cloud_snapshot_status_t;

typedef struct {
    char access_token[HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1];
    char handshake_headers[512];
    char machine_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    uint32_t selection_generation;
} hub_cloud_websocket_auth_t;

esp_err_t hub_cloud_refresh(void);
// Bounded accessory read on the existing keeper/Cloud IO path.
esp_err_t hub_cloud_refresh_scale(bool *request_attempted);
bool hub_cloud_has_fresh_scale_battery(void);
esp_err_t hub_cloud_request_refresh(void);
uint32_t hub_cloud_refresh_request_generation(void);
void hub_cloud_complete_refresh_request(uint32_t generation);
bool hub_cloud_refresh_is_running(void);
int64_t hub_cloud_token_renewal_due_ms(void);
void hub_cloud_get_snapshot_status(hub_cloud_snapshot_status_t *out);
esp_err_t hub_cloud_get_websocket_auth(hub_cloud_websocket_auth_t *out);
void hub_cloud_clear_websocket_auth(hub_cloud_websocket_auth_t *auth);
bool hub_cloud_controls_available(void);
const char *hub_cloud_command_readiness(void);
// Bound Cloud timing to one coordinator execution without changing its result.
void hub_cloud_command_timing_begin(const char *request_id);
void hub_cloud_command_timing_end(void);
bool hub_cloud_readonly_available(void);
// Share outbound TLS capacity with background grinder traffic; controls take priority.
esp_err_t hub_cloud_background_io_begin(void);
void hub_cloud_background_io_end(void);
esp_err_t hub_cloud_statistics_json_alloc_for_serial(
    const char *expected_serial,
    int days,
    char **body_out
);
esp_err_t hub_cloud_statistics_json_alloc(int days, char **body_out);
// Fresh measured results for the integrated grinder; uses the existing session.
esp_err_t hub_cloud_latest_coffee_json(const char *expected_serial, char **body_out);
esp_err_t hub_cloud_statistics_json(int days, char *buffer, size_t buffer_len);
esp_err_t hub_cloud_schedule_json(char *buffer, size_t buffer_len);
esp_err_t hub_cloud_firmware_json(char *buffer, size_t buffer_len);
esp_err_t hub_cloud_set_bbw_dose(const char *dose, int grams, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_bbw_dose_with_deadline(
    const char *dose,
    int grams,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
);
esp_err_t hub_cloud_set_bbw_mode(const char *mode, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_bbw_mode_with_deadline(
    const char *mode,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
);
esp_err_t hub_cloud_set_prebrew(const char *field, const char *value, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_prebrew_with_deadline(
    const char *field,
    const char *value,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
);
esp_err_t hub_cloud_set_power(bool enabled, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_coffee_temperature(double temperature, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_steam_level(int level, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_steam_enabled(bool enabled, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_steam_temperature(double temperature, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_set_smart_standby(bool enabled, int minutes, const char *mode, hub_cloud_command_result_t *result);
esp_err_t hub_cloud_start_backflush(hub_cloud_command_result_t *result);
esp_err_t hub_cloud_update_machine_firmware(hub_cloud_command_result_t *result);
hub_cloud_status_t hub_cloud_status(void);
const char *hub_cloud_status_string(void);
bool hub_cloud_configured(void);
bool hub_cloud_authenticated(void);
bool hub_cloud_live_validation_required(void);
bool hub_cloud_refresh_pending(void);
bool hub_cloud_machine_selected(void);
bool hub_cloud_ble_token_cached(void);
bool hub_cloud_has_cached_snapshot(void);
const char *hub_cloud_machine_name(void);
const char *hub_cloud_machine_serial(void);
const char *hub_cloud_machine_state_json(void);
const char *hub_cloud_last_error(void);
int64_t hub_cloud_last_refresh_ms(void);
bool hub_cloud_installation_identity_sha256(char *buffer, size_t buffer_len);
uint32_t hub_cloud_auth_refresh_success_count(void);
uint32_t hub_cloud_auth_refresh_failure_count(void);
int64_t hub_cloud_auth_refresh_last_success_uptime_ms(void);
const char *hub_cloud_readonly_diagnostics_json(void);

esp_err_t hub_cloud_set_schedule(const hub_schedule_entry_t *expected, const hub_schedule_entry_t *requested, hub_cloud_command_result_t *result);

esp_err_t hub_cloud_set_bbw_mode_checked_with_deadline(const char *mode,int preset_grams,bool require_no_stored_match,const hub_command_deadline_t *deadline,uint32_t reserve_after_ms,hub_cloud_command_result_t *result);
