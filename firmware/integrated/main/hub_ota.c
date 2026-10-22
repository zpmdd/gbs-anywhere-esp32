#include "hub_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "hub_version.h"
#ifdef HUB_GRINDER
#include "hub_grinder.h"
#endif

static const char *TAG = "hub_ota";

static void resume_grinder(hub_ota_update_t *update)
{
#ifdef HUB_GRINDER
    if(update->grinder_paused) hub_grinder_end_maintenance();
#endif
    update->grinder_paused=false;
}

static const char *partition_label(const esp_partition_t *partition)
{
    return partition != NULL ? partition->label : "";
}

static void set_message(hub_ota_update_t *update, const char *message)
{
    if (update != NULL) {
        snprintf(update->message, sizeof(update->message), "%s", message);
    }
}

esp_err_t hub_ota_status_json(char *body, size_t body_len)
{
    if (body == NULL || body_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    esp_app_desc_t running_desc = {0};
    const char *version = LAMABUDDY_FIRMWARE_VERSION;
    if (running != NULL && esp_ota_get_partition_description(running, &running_desc) == ESP_OK && running_desc.version[0] != '\0') {
        version = running_desc.version;
    }

    snprintf(
        body,
        body_len,
        "{"
        "\"ok\":true,"
        "\"enabled\":%s,"
        "\"firmwareVersion\":\"%s\","
        "\"runningPartition\":\"%s\","
        "\"bootPartition\":\"%s\","
        "\"nextUpdatePartition\":\"%s\","
        "\"nextUpdateSize\":%lu,"
        "\"restartRequired\":false"
        "}",
        next != NULL ? "true" : "false",
        version,
        partition_label(running),
        partition_label(boot),
        partition_label(next),
        (unsigned long)(next != NULL ? next->size : 0)
    );
    return ESP_OK;
}

esp_err_t hub_ota_begin_update(size_t expected_size, hub_ota_update_t *update)
{
    if (update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(update, 0, sizeof(*update));

    if (expected_size == 0) {
        set_message(update, "OTA image is empty.");
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        set_message(update, "No OTA update partition is available.");
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > partition->size) {
        snprintf(
            update->message,
            sizeof(update->message),
            "OTA image is larger than partition %s.",
            partition->label
        );
        return ESP_ERR_INVALID_SIZE;
    }

#ifdef HUB_GRINDER
    if(!hub_grinder_begin_maintenance()) {
        set_message(update,"Grinder job or maintenance is active; retry after it completes.");
        return ESP_ERR_INVALID_STATE;
    }
    update->grinder_paused=true;
#endif
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, expected_size, &handle);
    if (err != ESP_OK) {
        resume_grinder(update);
        snprintf(update->message, sizeof(update->message), "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    update->partition = partition;
    update->handle = handle;
    update->expected_size = expected_size;
    update->written = 0;
    update->active = true;
    snprintf(update->message, sizeof(update->message), "OTA writing to %s.", partition->label);
    ESP_LOGI(TAG, "OTA update started partition=%s size=%lu", partition->label, (unsigned long)expected_size);
    return ESP_OK;
}

esp_err_t hub_ota_write(hub_ota_update_t *update, const void *data, size_t data_len)
{
    if (update == NULL || !update->active || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len == 0) {
        return ESP_OK;
    }
    if (update->written + data_len > update->expected_size) {
        set_message(update, "OTA upload exceeded expected content length.");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_write(update->handle, data, data_len);
    if (err != ESP_OK) {
        snprintf(update->message, sizeof(update->message), "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }
    update->written += data_len;
    return ESP_OK;
}

esp_err_t hub_ota_finish(hub_ota_update_t *update)
{
    if (update == NULL || !update->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (update->written != update->expected_size) {
        set_message(update, "OTA upload ended before content length was fully received.");
        hub_ota_abort(update);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_end(update->handle);
    update->active = false;
    if (err != ESP_OK) {
        resume_grinder(update);
        snprintf(update->message, sizeof(update->message), "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update->partition);
    if (err != ESP_OK) {
        resume_grinder(update);
        snprintf(update->message, sizeof(update->message), "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_app_desc_t staged_desc = {0};
    const char *version = "unknown";
    if (esp_ota_get_partition_description(update->partition, &staged_desc) == ESP_OK && staged_desc.version[0] != '\0') {
        version = staged_desc.version;
    }

    snprintf(
        update->message,
        sizeof(update->message),
        "OTA image staged on %s version %s. Restart required.",
        partition_label(update->partition),
        version
    );
    ESP_LOGI(TAG, "%s", update->message);
    return ESP_OK;
}

void hub_ota_abort(hub_ota_update_t *update)
{
    if (update == NULL || !update->active) {
        return;
    }
    resume_grinder(update);
    esp_err_t err = esp_ota_abort(update->handle);
    update->active = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_abort failed: %s", esp_err_to_name(err));
    }
}
