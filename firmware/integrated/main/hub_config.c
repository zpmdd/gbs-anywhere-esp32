#include "hub_config.h"

#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_check.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hub_command_coordinator.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *HUB_NAMESPACE = "lamabuddy";
static const char *WIFI_SSID_KEY = "wifi_ssid";
static const char *WIFI_PASSWORD_KEY = "wifi_pass";
static const char *LOCAL_TOKEN_KEY = "local_token";
static const char *CLOUD_USERNAME_KEY = "cloud_username";
static const char *CLOUD_PASSWORD_KEY = "cloud_password";
static const char *CLOUD_REFRESH_TOKEN_KEY = "cloud_refresh";
static const char *CLOUD_INSTALLATION_KEY_KEY = "cloud_inst_key";
static const char *CLOUD_SERIAL_NUMBER_KEY = "cloud_serial";
static const char *MACHINE_SERIAL_KEY = "machine_serial";
static const char *MACHINE_NAME_KEY = "machine_name";
static const char *BLE_AUTH_TOKEN_KEY = "ble_auth_token";
static const char *BLE_DEVICE_NAME_KEY = "ble_device_name";
static const char *BLE_DEVICE_ADDRESS_KEY = "ble_dev_addr";
static const char *CLOUD_WS_ENABLED_KEY = "ws_enabled";
static const char *CLOUD_WS_MERGE_KEY = "ws_merge";
static const char *CLOUD_WS_FIELD_MASK_KEY = "ws_mask";
static const char *CLOUD_WS_BOOT_GUARD_KEY = "ws_guard";
static SemaphoreHandle_t machine_selection_mutex;
static portMUX_TYPE machine_selection_mutex_lock = portMUX_INITIALIZER_UNLOCKED;
static hub_machine_selection_identity_t machine_selection_identity;
static bool machine_selection_identity_loaded;

static esp_err_t ensure_machine_selection_mutex(void)
{
    if (machine_selection_mutex != NULL) {
        return ESP_OK;
    }
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&machine_selection_mutex_lock);
    if (machine_selection_mutex == NULL) {
        machine_selection_mutex = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&machine_selection_mutex_lock);
    if (created != NULL) {
        vSemaphoreDelete(created);
    }
    return ESP_OK;
}

static esp_err_t open_config(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(HUB_NAMESPACE, mode, handle);
}

static bool has_nvs_string(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required_len = 0;
    err = nvs_get_str(handle, key, NULL, &required_len);
    nvs_close(handle);
    return err == ESP_OK && required_len > 1;
}

static esp_err_t set_required_string(nvs_handle_t handle, const char *key, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_str(handle, key, value);
}

static esp_err_t set_optional_string(nvs_handle_t handle, const char *key, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        return nvs_set_str(handle, key, value);
    }

    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t get_optional_string(nvs_handle_t handle, const char *key, char *value, size_t value_len)
{
    if (value == NULL || value_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    value[0] = '\0';
    size_t required_len = value_len;
    esp_err_t err = nvs_get_str(handle, key, value, &required_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_machine_selection_identity_locked(void)
{
    if (machine_selection_identity_loaded) {
        return ESP_OK;
    }
    memset(&machine_selection_identity, 0, sizeof(machine_selection_identity));
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        machine_selection_identity_loaded = true;
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = get_optional_string(
            handle,
            MACHINE_SERIAL_KEY,
            machine_selection_identity.serial_number,
            sizeof(machine_selection_identity.serial_number)
        );
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        machine_selection_identity.generation = machine_selection_identity.serial_number[0] != '\0' ? 1 : 0;
        machine_selection_identity_loaded = true;
    }
    return err;
}

static bool bbw_selection_valid(const hub_bbw_selection_t *value)
{
    return memchr(value->serial_number, 0, sizeof(value->serial_number))
        && value->serial_number[0] && memchr(value->mode, 0, sizeof(value->mode))
        && (!strcmp(value->mode, "Dose1") || !strcmp(value->mode, "Dose2")
            || !strcmp(value->mode, "Continuous") || !strcmp(value->mode, "PulsesType"))
        && isfinite(value->confirmed_at) && value->confirmed_at >= 1704067200.0;
}

esp_err_t hub_config_get_bbw_selection(const hub_machine_selection_identity_t *target, hub_bbw_selection_t *out)
{
    if (!target || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(*out);
    err = nvs_get_blob(handle, "bbw_select_v1", out, &size);
    nvs_close(handle);
    if (err == ESP_OK && (size != sizeof(*out) || !bbw_selection_valid(out)
            || strcmp(out->serial_number, target->serial_number)))
        err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) memset(out, 0, sizeof(*out));
    return err;
}

esp_err_t hub_config_save_bbw_selection(const hub_machine_selection_identity_t *target, const char *mode, double confirmed_at)
{
    if (!target || !mode || strlen(mode) >= sizeof(((hub_bbw_selection_t *)0)->mode))
        return ESP_ERR_INVALID_ARG;
    hub_bbw_selection_t next = {0}, previous = {0};
    memcpy(next.serial_number, target->serial_number, sizeof(next.serial_number));
    strcpy(next.mode, mode);
    next.confirmed_at = confirmed_at;
    if (!bbw_selection_valid(&next) || confirmed_at > (double)time(NULL) + 8.0)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_machine_selection_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(machine_selection_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    err = load_machine_selection_identity_locked();
    if (err == ESP_OK && (target->generation != machine_selection_identity.generation
            || strcmp(target->serial_number, machine_selection_identity.serial_number)))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        esp_err_t read_err = hub_config_get_bbw_selection(target, &previous);
        if (read_err == ESP_OK && confirmed_at <= previous.confirmed_at) {
            xSemaphoreGive(machine_selection_mutex);
            return confirmed_at == previous.confirmed_at && !strcmp(mode, previous.mode)
                ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
        if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND && read_err != ESP_ERR_INVALID_STATE)
            err = read_err;
    }
    if (err == ESP_OK) {
        nvs_handle_t handle;
        err = open_config(NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, "bbw_select_v1", &next, sizeof(next));
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    xSemaphoreGive(machine_selection_mutex);
    return err;
}

static bool bbw_doses_valid(const hub_bbw_doses_t *value)
{
    return memchr(value->serial_number, 0, sizeof(value->serial_number)) && value->serial_number[0]
        && memchr(value->mode, 0, sizeof(value->mode))
        && (!strcmp(value->mode, "Dose1") || !strcmp(value->mode, "Dose2")
            || !strcmp(value->mode, "Continuous") || !strcmp(value->mode, "PulsesType"))
        && isfinite(value->dose1) && value->dose1 > 0 && value->dose1 <= 200
        && isfinite(value->dose2) && value->dose2 > 0 && value->dose2 <= 200;
}

esp_err_t hub_config_get_bbw_doses(const hub_machine_selection_identity_t *target, hub_bbw_doses_t *out)
{
    if (!target || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(*out);
    err = nvs_get_blob(handle, "bbw_doses_v1", out, &size);
    nvs_close(handle);
    if (err == ESP_OK && (size != sizeof(*out) || !bbw_doses_valid(out)
            || strcmp(out->serial_number, target->serial_number)))
        err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) memset(out, 0, sizeof(*out));
    return err;
}

esp_err_t hub_config_save_bbw_doses(const hub_machine_selection_identity_t *target, const char *mode, double dose1, double dose2)
{
    if (!target || !mode || strlen(mode) >= sizeof(((hub_bbw_doses_t *)0)->mode)) return ESP_ERR_INVALID_ARG;
    hub_bbw_doses_t next = {0}, previous = {0};
    memcpy(next.serial_number, target->serial_number, sizeof(next.serial_number));
    strcpy(next.mode, mode);
    next.dose1 = dose1; next.dose2 = dose2;
    if (!bbw_doses_valid(&next)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_machine_selection_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(machine_selection_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    err = load_machine_selection_identity_locked();
    if (err == ESP_OK && (target->generation != machine_selection_identity.generation
            || strcmp(target->serial_number, machine_selection_identity.serial_number)))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        esp_err_t read_err = hub_config_get_bbw_doses(target, &previous);
        if (read_err == ESP_OK && dose1 == previous.dose1 && dose2 == previous.dose2 && !strcmp(mode, previous.mode)) {
            xSemaphoreGive(machine_selection_mutex);
            return ESP_OK;
        }
        if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND && read_err != ESP_ERR_INVALID_STATE)
            err = read_err;
    }
    if (err == ESP_OK) {
        nvs_handle_t handle;
        err = open_config(NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, "bbw_doses_v1", &next, sizeof(next));
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    xSemaphoreGive(machine_selection_mutex);
    return err;
}

esp_err_t hub_config_init(void)
{
    /*
     * NVS contains credentials, the local bearer token, and the persistent Hub
     * identity. Never turn a recoverable storage/version error into an
     * implicit factory reset during boot. Recovery must be an explicit,
     * authenticated factory-reset/provisioning action.
     */
    return nvs_flash_init();
}

bool hub_config_has_wifi_credentials(void)
{
    char ssid[HUB_CONFIG_MAX_SSID_LEN + 1] = {0};
    char password[HUB_CONFIG_MAX_PASSWORD_LEN + 1] = {0};
    return hub_config_get_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK;
}

esp_err_t hub_config_get_wifi_credentials(
    char *ssid,
    size_t ssid_len,
    char *password,
    size_t password_len
)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_ssid_len = ssid_len;
    size_t required_password_len = password_len;
    err = nvs_get_str(handle, WIFI_SSID_KEY, ssid, &required_ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, WIFI_PASSWORD_KEY, password, &required_password_len);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_PASSWORD_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_get_local_token(char *token, size_t token_len)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_token_len = token_len;
    err = nvs_get_str(handle, LOCAL_TOKEN_KEY, token, &required_token_len);
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_save_local_token(const char *token)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, LOCAL_TOKEN_KEY, token);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_save_cloud_settings(
    const char *username,
    const char *password,
    const char *refresh_token,
    const char *installation_key,
    const char *cloud_serial_number
)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = set_required_string(handle, CLOUD_USERNAME_KEY, username);
    if (err == ESP_OK) {
        err = set_optional_string(handle, CLOUD_PASSWORD_KEY, password);
    }
    if (err == ESP_OK) {
        err = set_optional_string(handle, CLOUD_REFRESH_TOKEN_KEY, refresh_token);
    }
    if (err == ESP_OK) {
        err = set_required_string(handle, CLOUD_INSTALLATION_KEY_KEY, installation_key);
    }
    if (err == ESP_OK) {
        err = set_optional_string(handle, CLOUD_SERIAL_NUMBER_KEY, cloud_serial_number);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_get_cloud_settings(hub_cloud_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(settings, 0, sizeof(*settings));

    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = get_optional_string(handle, CLOUD_USERNAME_KEY, settings->username, sizeof(settings->username));
    if (err == ESP_OK) {
        err = get_optional_string(handle, CLOUD_PASSWORD_KEY, settings->password, sizeof(settings->password));
    }
    if (err == ESP_OK) {
        err = get_optional_string(handle, CLOUD_REFRESH_TOKEN_KEY, settings->refresh_token, sizeof(settings->refresh_token));
    }
    if (err == ESP_OK) {
        err = get_optional_string(handle, CLOUD_INSTALLATION_KEY_KEY, settings->installation_key, sizeof(settings->installation_key));
    }
    if (err == ESP_OK) {
        err = get_optional_string(handle, CLOUD_SERIAL_NUMBER_KEY, settings->cloud_serial_number, sizeof(settings->cloud_serial_number));
    }
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_save_cloud_tokens(const char *refresh_token)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = set_optional_string(handle, CLOUD_REFRESH_TOKEN_KEY, refresh_token);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_machine_selection(
    const hub_machine_selection_identity_t *expected,
    bool require_unchanged,
    const char *serial_number,
    const char *machine_name,
    const char *ble_auth_token,
    const char *ble_device_name,
    const char *ble_device_address
)
{
    if (serial_number == NULL || serial_number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_machine_selection_mutex(), "hub_config", "machine selection mutex unavailable");
    if (xSemaphoreTake(machine_selection_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t identity_err = load_machine_selection_identity_locked();
    if (identity_err != ESP_OK) {
        xSemaphoreGive(machine_selection_mutex);
        return identity_err;
    }
    if (require_unchanged
        && (expected == NULL
            || expected->generation != machine_selection_identity.generation
            || strcmp(expected->serial_number, machine_selection_identity.serial_number) != 0)) {
        xSemaphoreGive(machine_selection_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    bool selection_changed = strcmp(machine_selection_identity.serial_number, serial_number) != 0;
    hub_machine_selection_identity_t command_target = {0};
    if (selection_changed && hub_command_target_guard_get(&command_target)) {
        xSemaphoreGive(machine_selection_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(machine_selection_mutex);
        return err;
    }

    err = set_required_string(handle, MACHINE_SERIAL_KEY, serial_number);
    if (err == ESP_OK) {
        err = set_optional_string(handle, MACHINE_NAME_KEY, machine_name);
    }
    if (err == ESP_OK) {
        err = set_optional_string(handle, BLE_AUTH_TOKEN_KEY, ble_auth_token);
    }
    if (err == ESP_OK) {
        err = set_optional_string(handle, BLE_DEVICE_NAME_KEY, ble_device_name);
    }
    if (err == ESP_OK) {
        err = set_optional_string(handle, BLE_DEVICE_ADDRESS_KEY, ble_device_address);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        if (selection_changed) {
            machine_selection_identity.generation++;
            if (machine_selection_identity.generation == 0) {
                machine_selection_identity.generation = 1;
            }
        }
        snprintf(
            machine_selection_identity.serial_number,
            sizeof(machine_selection_identity.serial_number),
            "%s",
            serial_number
        );
    }
    xSemaphoreGive(machine_selection_mutex);
    return err;
}

esp_err_t hub_config_save_machine_selection(
    const char *serial_number,
    const char *machine_name,
    const char *ble_auth_token,
    const char *ble_device_name,
    const char *ble_device_address
)
{
    return save_machine_selection(
        NULL,
        false,
        serial_number,
        machine_name,
        ble_auth_token,
        ble_device_name,
        ble_device_address
    );
}

esp_err_t hub_config_save_machine_selection_if_unchanged(
    const hub_machine_selection_identity_t *expected,
    const char *serial_number,
    const char *machine_name,
    const char *ble_auth_token,
    const char *ble_device_name,
    const char *ble_device_address
)
{
    return save_machine_selection(
        expected,
        true,
        serial_number,
        machine_name,
        ble_auth_token,
        ble_device_name,
        ble_device_address
    );
}

esp_err_t hub_config_save_ble_device_address(const char *ble_device_address)
{
    hub_machine_selection_identity_t identity = {0};
    ESP_RETURN_ON_ERROR(
        hub_config_get_machine_selection_identity(&identity),
        "hub_config",
        "machine selection unavailable"
    );
    return hub_config_save_ble_device_address_for_serial(identity.serial_number, ble_device_address);
}

esp_err_t hub_config_save_ble_device_address_for_serial(
    const char *expected_serial_number,
    const char *ble_device_address
)
{
    if (expected_serial_number == NULL || expected_serial_number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (ble_device_address == NULL || ble_device_address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_machine_selection_mutex(), "hub_config", "machine selection mutex unavailable");
    if (xSemaphoreTake(machine_selection_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t identity_err = load_machine_selection_identity_locked();
    if (identity_err != ESP_OK
        || strcmp(machine_selection_identity.serial_number, expected_serial_number) != 0) {
        xSemaphoreGive(machine_selection_mutex);
        return identity_err == ESP_OK ? ESP_ERR_INVALID_STATE : identity_err;
    }

    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(machine_selection_mutex);
        return err;
    }

    err = nvs_set_str(handle, BLE_DEVICE_ADDRESS_KEY, ble_device_address);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    xSemaphoreGive(machine_selection_mutex);
    return err;
}

esp_err_t hub_config_get_machine_selection(hub_machine_selection_t *selection)
{
    if (selection == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(selection, 0, sizeof(*selection));
    ESP_RETURN_ON_ERROR(ensure_machine_selection_mutex(), "hub_config", "machine selection mutex unavailable");
    if (xSemaphoreTake(machine_selection_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(machine_selection_mutex);
        return err;
    }

    err = get_optional_string(handle, MACHINE_SERIAL_KEY, selection->serial_number, sizeof(selection->serial_number));
    if (err == ESP_OK) {
        err = get_optional_string(handle, MACHINE_NAME_KEY, selection->machine_name, sizeof(selection->machine_name));
    }
    if (err == ESP_OK) {
        err = get_optional_string(handle, BLE_AUTH_TOKEN_KEY, selection->ble_auth_token, sizeof(selection->ble_auth_token));
    }
    if (err == ESP_OK) {
        err = get_optional_string(handle, BLE_DEVICE_NAME_KEY, selection->ble_device_name, sizeof(selection->ble_device_name));
    }
    if (err == ESP_OK) {
        err = get_optional_string(handle, BLE_DEVICE_ADDRESS_KEY, selection->ble_device_address, sizeof(selection->ble_device_address));
    }
    nvs_close(handle);
    xSemaphoreGive(machine_selection_mutex);
    return err;
}

esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *identity)
{
    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(identity, 0, sizeof(*identity));
    ESP_RETURN_ON_ERROR(ensure_machine_selection_mutex(), "hub_config", "machine selection mutex unavailable");
    if (xSemaphoreTake(machine_selection_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = load_machine_selection_identity_locked();
    if (err == ESP_OK && machine_selection_identity.serial_number[0] != '\0') {
        *identity = machine_selection_identity;
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(machine_selection_mutex);
    return err;
}

bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *expected)
{
    if (expected == NULL || expected->serial_number[0] == '\0') {
        return false;
    }
    hub_machine_selection_identity_t current = {0};
    return hub_config_get_machine_selection_identity(&current) == ESP_OK
        && current.generation == expected->generation
        && strcmp(current.serial_number, expected->serial_number) == 0;
}

bool hub_config_has_cloud_credentials(void)
{
    return has_nvs_string(CLOUD_USERNAME_KEY)
        && has_nvs_string(CLOUD_INSTALLATION_KEY_KEY)
        && (has_nvs_string(CLOUD_PASSWORD_KEY) || has_nvs_string(CLOUD_REFRESH_TOKEN_KEY));
}

bool hub_config_has_machine_selection(void)
{
    return has_nvs_string(MACHINE_SERIAL_KEY);
}

bool hub_config_has_ble_auth_token(void)
{
    return has_nvs_string(BLE_AUTH_TOKEN_KEY);
}

bool hub_config_has_ble_device(void)
{
    return has_nvs_string(BLE_DEVICE_NAME_KEY) || has_nvs_string(BLE_DEVICE_ADDRESS_KEY);
}

esp_err_t hub_config_get_cloud_websocket_settings(hub_cloud_websocket_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    settings->enabled = false;
    settings->merge_enabled = false;
    settings->field_mask = UINT32_MAX;

    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t enabled = 0;
    uint8_t merge_enabled = 0;
    uint32_t field_mask = UINT32_MAX;
    esp_err_t enabled_err = nvs_get_u8(handle, CLOUD_WS_ENABLED_KEY, &enabled);
    esp_err_t merge_err = nvs_get_u8(handle, CLOUD_WS_MERGE_KEY, &merge_enabled);
    esp_err_t mask_err = nvs_get_u32(handle, CLOUD_WS_FIELD_MASK_KEY, &field_mask);
    nvs_close(handle);

    if (enabled_err != ESP_OK && enabled_err != ESP_ERR_NVS_NOT_FOUND) {
        return enabled_err;
    }
    if (merge_err != ESP_OK && merge_err != ESP_ERR_NVS_NOT_FOUND) {
        return merge_err;
    }
    if (mask_err != ESP_OK && mask_err != ESP_ERR_NVS_NOT_FOUND) {
        return mask_err;
    }
    settings->enabled = enabled != 0;
    settings->merge_enabled = merge_enabled != 0;
    settings->field_mask = field_mask;
    return ESP_OK;
}

esp_err_t hub_config_save_cloud_websocket_settings(
    const hub_cloud_websocket_settings_t *settings
)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, CLOUD_WS_ENABLED_KEY, settings->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, CLOUD_WS_MERGE_KEY, settings->merge_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, CLOUD_WS_FIELD_MASK_KEY, settings->field_mask);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_get_cloud_websocket_boot_guard(bool *armed, bool *present)
{
    if (armed == NULL || present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *armed = false;
    *present = false;
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    uint8_t value = 0;
    err = nvs_get_u8(handle, CLOUD_WS_BOOT_GUARD_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        *armed = value != 0;
        *present = true;
    }
    return err;
}

esp_err_t hub_config_set_cloud_websocket_boot_guard(bool armed)
{
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, CLOUD_WS_BOOT_GUARD_KEY, armed ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hub_config_factory_reset(void)
{
    hub_machine_selection_identity_t command_target = {0};
    if (hub_command_target_guard_get(&command_target)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ensure_machine_selection_mutex(), "hub_config", "machine selection mutex unavailable");
    if (xSemaphoreTake(machine_selection_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    (void)load_machine_selection_identity_locked();
    nvs_handle_t handle;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(machine_selection_mutex);
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        machine_selection_identity.generation++;
        if (machine_selection_identity.generation == 0) {
            machine_selection_identity.generation = 1;
        }
        machine_selection_identity.serial_number[0] = '\0';
        machine_selection_identity_loaded = true;
    }
    xSemaphoreGive(machine_selection_mutex);
    return err;
}
