#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HUB_CONFIG_MAX_SSID_LEN 32
#define HUB_CONFIG_MAX_PASSWORD_LEN 64
#define HUB_CONFIG_TOKEN_LEN 35
#define HUB_CONFIG_MAX_CLOUD_USERNAME_LEN 96
#define HUB_CONFIG_MAX_CLOUD_PASSWORD_LEN 128
#define HUB_CONFIG_MAX_CLOUD_TOKEN_LEN 1024
#define HUB_CONFIG_MAX_INSTALLATION_KEY_LEN 768
#define HUB_CONFIG_MAX_MACHINE_SERIAL_LEN 64
#define HUB_CONFIG_MAX_MACHINE_NAME_LEN 96
#define HUB_CONFIG_MAX_BLE_TOKEN_LEN 256
#define HUB_CONFIG_MAX_BLE_DEVICE_NAME_LEN 64
#define HUB_CONFIG_MAX_BLE_DEVICE_ADDRESS_LEN 32

typedef struct {
    char username[HUB_CONFIG_MAX_CLOUD_USERNAME_LEN + 1];
    char password[HUB_CONFIG_MAX_CLOUD_PASSWORD_LEN + 1];
    char refresh_token[HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1];
    char installation_key[HUB_CONFIG_MAX_INSTALLATION_KEY_LEN + 1];
    char cloud_serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
} hub_cloud_settings_t;

typedef struct {
    char serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    char machine_name[HUB_CONFIG_MAX_MACHINE_NAME_LEN + 1];
    char ble_auth_token[HUB_CONFIG_MAX_BLE_TOKEN_LEN + 1];
    char ble_device_name[HUB_CONFIG_MAX_BLE_DEVICE_NAME_LEN + 1];
    char ble_device_address[HUB_CONFIG_MAX_BLE_DEVICE_ADDRESS_LEN + 1];
} hub_machine_selection_t;

typedef struct {
    char serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    uint32_t generation;
} hub_machine_selection_identity_t;

typedef struct {
    char serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    char mode[16];
    double confirmed_at;
} hub_bbw_selection_t;

esp_err_t hub_config_get_bbw_selection(const hub_machine_selection_identity_t *target, hub_bbw_selection_t *out);
esp_err_t hub_config_save_bbw_selection(const hub_machine_selection_identity_t *target, const char *mode, double confirmed_at);

typedef struct {
    char serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    char mode[16];
    double dose1;
    double dose2;
} hub_bbw_doses_t;

esp_err_t hub_config_get_bbw_doses(const hub_machine_selection_identity_t *target, hub_bbw_doses_t *out);
esp_err_t hub_config_save_bbw_doses(const hub_machine_selection_identity_t *target, const char *mode, double dose1, double dose2);

typedef struct {
    bool enabled;
    bool merge_enabled;
    uint32_t field_mask;
} hub_cloud_websocket_settings_t;

esp_err_t hub_config_init(void);
bool hub_config_has_wifi_credentials(void);
esp_err_t hub_config_get_wifi_credentials(
    char *ssid,
    size_t ssid_len,
    char *password,
    size_t password_len
);
esp_err_t hub_config_save_wifi_credentials(const char *ssid, const char *password);
esp_err_t hub_config_get_local_token(char *token, size_t token_len);
esp_err_t hub_config_save_local_token(const char *token);
esp_err_t hub_config_save_cloud_settings(
    const char *username,
    const char *password,
    const char *refresh_token,
    const char *installation_key,
    const char *cloud_serial_number
);
esp_err_t hub_config_get_cloud_settings(hub_cloud_settings_t *settings);
esp_err_t hub_config_save_cloud_tokens(const char *refresh_token);
esp_err_t hub_config_save_machine_selection(
    const char *serial_number,
    const char *machine_name,
    const char *ble_auth_token,
    const char *ble_device_name,
    const char *ble_device_address
);
esp_err_t hub_config_save_machine_selection_if_unchanged(
    const hub_machine_selection_identity_t *expected,
    const char *serial_number,
    const char *machine_name,
    const char *ble_auth_token,
    const char *ble_device_name,
    const char *ble_device_address
);
esp_err_t hub_config_save_ble_device_address(const char *ble_device_address);
esp_err_t hub_config_save_ble_device_address_for_serial(
    const char *expected_serial_number,
    const char *ble_device_address
);
esp_err_t hub_config_get_machine_selection(hub_machine_selection_t *selection);
esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *identity);
bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *expected);
bool hub_config_has_cloud_credentials(void);
bool hub_config_has_machine_selection(void);
bool hub_config_has_ble_auth_token(void);
bool hub_config_has_ble_device(void);
esp_err_t hub_config_get_cloud_websocket_settings(hub_cloud_websocket_settings_t *settings);
esp_err_t hub_config_save_cloud_websocket_settings(
    const hub_cloud_websocket_settings_t *settings
);
esp_err_t hub_config_get_cloud_websocket_boot_guard(bool *armed, bool *present);
esp_err_t hub_config_set_cloud_websocket_boot_guard(bool armed);
esp_err_t hub_config_factory_reset(void);
