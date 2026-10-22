#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hub_api_v2.h"
#include "hub_command_deadline.h"

typedef enum {
    HUB_CONTROL_RISK_LOW = 0,
    HUB_CONTROL_RISK_MEDIUM,
    HUB_CONTROL_RISK_HIGH,
} hub_control_risk_t;

typedef enum {
    HUB_CONTROL_TRANSPORT_NONE = 0,
    HUB_CONTROL_TRANSPORT_BLE,
    HUB_CONTROL_TRANSPORT_CLOUD,
    HUB_CONTROL_TRANSPORT_LOCAL,
} hub_control_transport_t;

typedef struct {
    bool ok;
    int api_version;
    hub_api_v2_command_status_t v2_status;
    int accepted_ttl_ms;
    int remaining_ms;
    char request_id[37];
    bool client_request_id;
    bool pending;
    bool deduplicated;
    int upstream_attempts;
    int response_http_status;
    int queue_delay_ms;
    char error_code[40];
    char control[48];
    hub_control_transport_t transport;
    hub_control_transport_t preferred_transport;
    hub_control_transport_t fallback_transport;
    bool fallback_used;
    hub_control_risk_t risk;
    bool requires_confirmation;
    bool write_accepted;
    bool state_confirmed;
    bool state_confirmation_pending;
    bool late_side_effect_risk;
    /* BLE command admission and hot-session preparation before the setting write. */
    int ble_pre_write_ms;
    /* BLE setting write plus explicit command-status read. */
    int ble_write_status_ms;
    /* Elapsed target-confirmation time after write acceptance; 0 if not attempted. */
    int state_confirmation_ms;
    int confirmation_read_count;
    int confirmation_selector_write_count;
    int confirmation_direct_read_count;
    bool confirmation_direct_first;
    int confirmation_reprime_count;
    /* Last successful BLE cache observation; 0 means no successful observation. */
    uint32_t ble_observation_sequence;
    int duration_ms;
    bool ble_attempted;
    int ble_duration_ms;
    char ble_error[128];
    bool cloud_attempted;
    int cloud_duration_ms;
    char fallback_reason[128];
    char message[192];
} hub_control_result_t;

struct hub_command_request;

const char *hub_control_transport_string(hub_control_transport_t transport);
const char *hub_control_risk_string(hub_control_risk_t risk);
void hub_control_result_json(const hub_control_result_t *result, char *buffer, size_t buffer_len);
esp_err_t hub_control_execute_request(
    const struct hub_command_request *request,
    hub_control_result_t *result
);

esp_err_t hub_control_execute_quick_weight(int grams, hub_control_result_t *result);
esp_err_t hub_control_execute_quick_weight_with_deadline(
    int grams,
    const hub_command_deadline_t *deadline,
    hub_control_result_t *result
);
esp_err_t hub_control_execute_bbw_dose(const char *dose, int grams, hub_control_result_t *result);
esp_err_t hub_control_execute_bbw_mode(const char *mode, hub_control_result_t *result);
esp_err_t hub_control_execute_prebrew(const char *field, const char *value, hub_control_result_t *result);
esp_err_t hub_control_execute_prebrew_with_deadline(
    const char *field,
    const char *value,
    const hub_command_deadline_t *deadline,
    hub_control_result_t *result
);
esp_err_t hub_control_execute_coffee_temperature(double temperature, hub_control_result_t *result);
esp_err_t hub_control_execute_steam_level(int level, hub_control_result_t *result);
esp_err_t hub_control_execute_steam_enabled(bool enabled, hub_control_result_t *result);
esp_err_t hub_control_execute_steam_temperature(double temperature, hub_control_result_t *result);
esp_err_t hub_control_execute_smart_standby(bool enabled, int minutes, const char *mode, bool confirm, hub_control_result_t *result);
esp_err_t hub_control_execute_power(bool enabled, bool confirm, hub_control_result_t *result);
esp_err_t hub_control_execute_backflush(bool confirm, hub_control_result_t *result);
esp_err_t hub_control_execute_machine_firmware_update(bool confirm, hub_control_result_t *result);

esp_err_t hub_control_execute_quick_weight_checked_with_deadline(int grams,bool require_no_stored_match,const hub_command_deadline_t *deadline,hub_control_result_t *result);
esp_err_t hub_control_execute_bbw_mode_checked_with_deadline(const char *mode,int preset_grams,const hub_command_deadline_t *deadline,hub_control_result_t *result);
