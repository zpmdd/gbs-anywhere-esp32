#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool ok;
    int duration_ms;
    char message[128];
} hub_ble_read_result_t;

typedef struct {
    bool ok;
    int duration_ms;
    int command_count;
    char commands_json[384];
    char message[128];
} hub_ble_write_validation_result_t;

typedef struct {
    bool ok;
    int duration_ms;
    bool request_started;
    bool guard_rejected;
    bool write_accepted;
    bool state_confirmed;
    bool state_confirmation_pending;
    bool late_side_effect_risk;
    /* Time spent before the machine-setting GATT write starts. */
    int pre_write_ms;
    /* Time spent sending the setting and reading the explicit Success status. */
    int write_status_ms;
    /* Elapsed target-confirmation time after write acceptance; 0 if not attempted. */
    int state_confirmation_ms;
    /* Successful or failed target-observation reads attempted after write acceptance. */
    int confirmation_read_count;
    /* Confirmation reads that first wrote the boilers selector. */
    int confirmation_selector_write_count;
    /* Confirmation reads that reused the already-primed READ characteristic. */
    int confirmation_direct_read_count;
    /* True when the first confirmation observation reused a proven boilers selector. */
    bool confirmation_direct_first;
    /* One-shot selector recovery attempts after an invalid direct response. */
    int confirmation_reprime_count;
    /* Last successful cache observation used for confirmation; 0 means none. */
    uint32_t observation_sequence;
    int attempts;
    char error_code[40];
    char message[128];
} hub_ble_command_result_t;

typedef esp_err_t (*hub_ble_worker_job_fn)(void *context);

esp_err_t hub_ble_init(void);
esp_err_t hub_ble_start_worker(void);
esp_err_t hub_ble_run_worker_job(
    hub_ble_worker_job_fn job,
    void *context,
    esp_err_t *job_result
);
uint32_t hub_ble_worker_stack_high_water_mark(void);
bool hub_ble_configured(void);
bool hub_ble_reads_validated(void);
bool hub_ble_writes_validated(void);
bool hub_ble_has_cached_state(void);
bool hub_ble_refresh_pending(void);
esp_err_t hub_ble_poll_brew_timer_mode(void);
esp_err_t hub_ble_validate_read(hub_ble_read_result_t *result);
esp_err_t hub_ble_full_read_probe_json(char *dest, size_t dest_len);
esp_err_t hub_ble_notify_probe_json(int duration_ms, char *dest, size_t dest_len);
esp_err_t hub_ble_protocol_probe_json(const char *profile, int duration_ms, int interval_ms, char *dest, size_t dest_len);
esp_err_t hub_ble_gatt_read_probe_json(const char *profile, int duration_ms, int interval_ms, char *dest, size_t dest_len);
esp_err_t hub_ble_validate_noop_write(bool confirm, bool include_power, hub_ble_write_validation_result_t *result);
esp_err_t hub_ble_set_bbw_dose(const char *dose, int grams, hub_ble_command_result_t *result);
esp_err_t hub_ble_set_bbw_mode(const char *mode, hub_ble_command_result_t *result);
esp_err_t hub_ble_set_prebrew(const char *field, const char *value, hub_ble_command_result_t *result);
esp_err_t hub_ble_set_coffee_temperature(double temperature, hub_ble_command_result_t *result);
esp_err_t hub_ble_set_steam_temperature(double temperature, hub_ble_command_result_t *result);
esp_err_t hub_ble_set_steam_enabled(bool enabled, hub_ble_command_result_t *result);
esp_err_t hub_ble_set_power(bool enabled, hub_ble_command_result_t *result);
esp_err_t hub_ble_set_smart_standby(bool enabled, int minutes, const char *mode, hub_ble_command_result_t *result);
esp_err_t hub_ble_start_backflush(hub_ble_command_result_t *result);
esp_err_t hub_ble_update_machine_firmware(hub_ble_command_result_t *result);
esp_err_t hub_ble_scan_diagnostics(int duration_ms, char *body, size_t body_len);
const char *hub_ble_state_json(void);
const char *hub_ble_diagnostics_json(void);
const char *hub_ble_last_error(void);
