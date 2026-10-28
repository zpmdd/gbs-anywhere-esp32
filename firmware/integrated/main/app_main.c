#include <stdlib.h>

#include "bootloader_random.h"
#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hub_ble.h"
#include "hub_cloud_websocket.h"
#include "hub_command_coordinator.h"
#include "hub_config.h"
#include "hub_event_log.h"
#include "hub_http.h"
#include "hub_https.h"
#include "hub_identity.h"
#include "hub_provisioning_ble.h"
#include "hub_runtime_resources.h"
#include "hub_security.h"
#include "hub_setup_mode.h"
#include "hub_state_keeper.h"
#include "hub_version.h"
#include "hub_wifi.h"

#ifdef HUB_GRINDER
#include "hub_grinder.h"
#endif
#ifdef HUB_TOUCHSCREEN
#include "gbs_display.h"
#include "gbs_hub_client.h"
#endif

static const char *TAG = "app_main";

static void *json_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        return ptr;
    }
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void configure_json_allocator(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = json_malloc,
        .free_fn = free,
    };
    cJSON_InitHooks(&hooks);
}

void app_main(void)
{
    configure_json_allocator();

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    hub_runtime_resources_t resources;
    hub_runtime_resources_snapshot(&resources);

    ESP_LOGI(TAG, "%s booting", LAMABUDDY_MDNS_INSTANCE);
    ESP_LOGI(TAG, "chip=" LAMABUDDY_CHIP_NAME " cores=%d", chip_info.cores);
    uint32_t flash_bytes=0;
    esp_flash_get_size(NULL,&flash_bytes);
    ESP_LOGI(TAG, "flash_bytes=%lu", (unsigned long)flash_bytes);
    ESP_LOGI(
        TAG,
        "psram_initialized=%s psram_bytes=%lu psram_free=%lu",
        resources.psram_initialized ? "true" : "false",
        (unsigned long)resources.psram_bytes,
        (unsigned long)resources.psram_free_bytes
    );
    ESP_LOGI(TAG, "firmware=%s", LAMABUDDY_FIRMWARE_VERSION);
    ESP_LOGI(
        TAG,
        "internal_free_heap=%lu internal_largest_block=%lu",
        (unsigned long)resources.internal_free_bytes,
        (unsigned long)resources.internal_largest_block_bytes
    );
    hub_event_log_record("boot", "appStart", true, LAMABUDDY_FIRMWARE_VERSION);

    ESP_ERROR_CHECK(hub_config_init());
    /*
     * Neither Wi-Fi nor Bluetooth RF entropy is available yet. Keep the
     * bootloader hardware entropy source active across both first-run bearer
     * token creation and Hub identity/key generation, and close it before any
     * RF/ADC subsystem is initialized. Do not ESP_ERROR_CHECK inside this
     * window, so every error path disables the source first.
     */
    bootloader_random_enable();
    esp_err_t security_err = hub_security_init();
    esp_err_t identity_err = security_err == ESP_OK
        ? hub_identity_init()
        : ESP_ERR_INVALID_STATE;
    bootloader_random_disable();
    ESP_ERROR_CHECK(security_err);
    if (identity_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "HTTPS v2 identity disabled: release security is not ready");
    } else if (identity_err != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS v2 identity unavailable: %s", esp_err_to_name(identity_err));
    }
    ESP_ERROR_CHECK(hub_setup_mode_init());
    ESP_ERROR_CHECK(hub_command_coordinator_start());
    if (hub_setup_mode_enabled()) {
        hub_event_log_record("setup", "setupModeEnabled", true, "setup mode enabled");
    }
#ifdef HUB_TOUCHSCREEN
    // The controller needs a contiguous 30 KiB block before display/task allocations.
    esp_err_t early_ble_err=hub_setup_mode_enabled()?hub_provisioning_ble_init():hub_ble_init();
#endif
    ESP_ERROR_CHECK(hub_wifi_init());
    ESP_ERROR_CHECK(hub_http_start());
    esp_err_t https_err = hub_https_start();
    if (https_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "HTTPS v2 server disabled: Hub identity is not ready");
    } else if (https_err != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS v2 server skipped: %s", esp_err_to_name(https_err));
    } else {
        ESP_LOGI(TAG, "HTTPS v2 server listening on port %d", LAMABUDDY_HTTPS_PORT);
        esp_err_t mdns_err = hub_wifi_refresh_mdns_services();
        if (mdns_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTPS v2 Bonjour refresh failed: %s", esp_err_to_name(mdns_err));
        }
    }

    if (hub_setup_mode_enabled()) {
#ifdef HUB_TOUCHSCREEN
        esp_err_t provisioning_err = early_ble_err;
#else
        esp_err_t provisioning_err = hub_provisioning_ble_init();
#endif
        if (provisioning_err == ESP_OK) {
            provisioning_err = hub_provisioning_ble_start();
        }
        if (provisioning_err != ESP_OK) {
            ESP_LOGW(TAG, "Setup provisioning BLE skipped: %s", esp_err_to_name(provisioning_err));
        }
    } else {
        /* Reserve the larger internal BLE stack before Cloud work can fragment heap. */
#ifdef HUB_TOUCHSCREEN
        esp_err_t ble_err = early_ble_err;
#else
        esp_err_t ble_err = hub_ble_init();
#endif
        if (ble_err == ESP_OK) {
            ble_err = hub_ble_start_worker();
        }
        if (ble_err != ESP_OK) {
            ESP_LOGW(TAG, "BLE init skipped: %s", esp_err_to_name(ble_err));
        }
        ESP_ERROR_CHECK(hub_state_keeper_start());
        esp_err_t websocket_err = hub_cloud_websocket_start();
        if (websocket_err != ESP_OK) {
            ESP_LOGW(TAG, "Cloud WebSocket supervisor skipped: %s", esp_err_to_name(websocket_err));
        }
    }
#ifdef HUB_GRINDER
    esp_err_t grinder_err=hub_grinder_start();
    if(grinder_err!=ESP_OK) ESP_LOGW(TAG,"Grinder unavailable: %s",esp_err_to_name(grinder_err));
#endif
#ifdef HUB_TOUCHSCREEN
    esp_err_t display_err=gbs_display_start(hub_grinder_state,hub_grinder_set_active);
    ESP_LOGI(TAG,"touchscreen=%s",esp_err_to_name(display_err));
    if(display_err==ESP_OK) {
        esp_err_t surface_err=gbs_hub_start(NULL);
        if(surface_err!=ESP_OK) ESP_LOGW(TAG,"Touch data unavailable: %s",esp_err_to_name(surface_err));
    }
    hub_runtime_resources_snapshot(&resources);
    ESP_LOGI(TAG,"integrated internal_free=%lu largest=%lu psram_free=%lu",
        (unsigned long)resources.internal_free_bytes,(unsigned long)resources.internal_largest_block_bytes,
        (unsigned long)resources.psram_free_bytes);
#endif
}
