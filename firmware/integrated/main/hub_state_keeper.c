#include "hub_state_keeper.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_ble.h"
#include "hub_cloud.h"
#ifdef HUB_AMOLED_216
#include "hub_command_coordinator.h"
#include "hub_grinder.h"
#endif
#include "hub_state.h"
#include "hub_wifi.h"

static const char *TAG = "hub_state_keeper";

static const uint32_t STARTUP_DELAY_MS = 1000;
static const uint32_t STARTUP_NETWORK_WAIT_MS = 30000;
static const uint32_t STARTUP_POLL_MS = 250;
static const uint32_t COLD_INTERVALS_MS[] = {5000, 15000, 30000, 60000, 300000, 600000, 1800000, 3600000};
static const uint32_t ACTIVE_WINDOW_MS = 300000;
static const uint32_t ACTIVE_INTERVAL_MS = 600000;
static const uint32_t STABLE_INTERVAL_MS = 1800000;
static const uint32_t IDLE_THRESHOLD_MS = 1800000;
static const uint32_t IDLE_INTERVAL_MS = 3600000;
static const uint32_t LOCAL_STATE_POLL_MS = 30000;
static const uint32_t CONNECTION_RECOVERY_INTERVAL_MS = 60000;

#define HUB_STATE_KEEPER_NOTIFY_TOUCH BIT0
#define HUB_STATE_KEEPER_NOTIFY_RECOVERY BIT1

static TaskHandle_t keeper_task_handle = NULL;
static portMUX_TYPE keeper_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t last_activity_ms = 0;
static int64_t active_until_ms = 0;
static bool recovery_requested = false;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void notify_keeper(uint32_t bits)
{
    TaskHandle_t handle = keeper_task_handle;
    if (handle != NULL) {
        (void)xTaskNotify(handle, bits, eSetBits);
    }
}

void hub_state_keeper_touch(void)
{
    int64_t now = now_ms();
    portENTER_CRITICAL(&keeper_lock);
    last_activity_ms = now;
    active_until_ms = now + ACTIVE_WINDOW_MS;
    portEXIT_CRITICAL(&keeper_lock);
    notify_keeper(HUB_STATE_KEEPER_NOTIFY_TOUCH);
}

void hub_state_keeper_request_recovery(void)
{
    int64_t now = now_ms();
    portENTER_CRITICAL(&keeper_lock);
    last_activity_ms = now;
    active_until_ms = now + ACTIVE_WINDOW_MS;
    recovery_requested = true;
    portEXIT_CRITICAL(&keeper_lock);
    notify_keeper(HUB_STATE_KEEPER_NOTIFY_RECOVERY);
}

static void read_keeper_state(int64_t *activity_ms, int64_t *active_ms, bool *recovery)
{
    portENTER_CRITICAL(&keeper_lock);
    *activity_ms = last_activity_ms;
    *active_ms = active_until_ms;
    *recovery = recovery_requested;
    portEXIT_CRITICAL(&keeper_lock);
}

static bool consume_recovery_request(void)
{
    bool requested = false;
    portENTER_CRITICAL(&keeper_lock);
    requested = recovery_requested;
    recovery_requested = false;
    portEXIT_CRITICAL(&keeper_lock);
    return requested;
}

static void extend_active_window(void)
{
    int64_t now = now_ms();
    portENTER_CRITICAL(&keeper_lock);
    active_until_ms = now + ACTIVE_WINDOW_MS;
    portEXIT_CRITICAL(&keeper_lock);
}

static uint32_t cold_interval_ms(size_t attempt)
{
    size_t max_index = sizeof(COLD_INTERVALS_MS) / sizeof(COLD_INTERVALS_MS[0]) - 1;
    return COLD_INTERVALS_MS[attempt < max_index ? attempt : max_index];
}

static uint32_t ready_interval_ms(int64_t now, int64_t activity_ms, int64_t active_ms, bool recover_connection)
{
    if (recover_connection) {
        return CONNECTION_RECOVERY_INTERVAL_MS;
    }
    if (activity_ms > 0 && now - activity_ms >= IDLE_THRESHOLD_MS) {
        return IDLE_INTERVAL_MS;
    }
    if (now < active_ms) {
        return ACTIVE_INTERVAL_MS;
    }
    return STABLE_INTERVAL_MS;
}

#ifdef HUB_AMOLED_216
typedef struct {
    hub_machine_selection_identity_t binding;
    bool observed;
    unsigned attempts;
    int64_t next_ms;
} scale_battery_reads_t;

static void scale_battery_tick(scale_battery_reads_t *reads,
                               const hub_cloud_snapshot_status_t *status, bool cloud_due)
{
    hub_machine_selection_identity_t binding = {0};
    if (hub_config_get_machine_selection_identity(&binding) != ESP_OK) return;
    int64_t now = now_ms();
    if (reads->binding.generation != binding.generation
        || strcmp(reads->binding.serial_number, binding.serial_number)) {
        *reads = (scale_battery_reads_t){.binding = binding, .next_ms = now + 90000};
    }
    // Latch ordinary Dashboard observations before their 60-second freshness expires.
    if (hub_cloud_has_fresh_scale_battery()
        && hub_config_machine_selection_matches(&binding)) reads->observed = true;
    if (cloud_due || reads->observed || reads->attempts >= 2 || now < reads->next_ms) return;
    if (!status->is_ready || status->refresh_pending || !hub_wifi_station_has_ip()
        || hub_command_idle_background_deferred() || hub_grinder_busy()) return;
    bool attempted = false;
    (void)hub_cloud_refresh_scale(&attempted);
    if (attempted) ++reads->attempts;
    reads->next_ms = now_ms() + (attempted ? 90000 : 30000);
    // An in-flight TLS read cannot be preempted; do not reserve grinder admission.
    if (hub_cloud_has_fresh_scale_battery()
        && hub_config_machine_selection_matches(&binding)) reads->observed = true;
}
#endif

static void hub_state_keeper_task(void *arg)
{
    (void)arg;
    int64_t startup_started_ms = now_ms();
    while (true) {
        int64_t elapsed_ms = now_ms() - startup_started_ms;
        if (elapsed_ms >= STARTUP_DELAY_MS && hub_wifi_station_has_ip()) {
            break;
        }
        if (elapsed_ms >= STARTUP_NETWORK_WAIT_MS) {
            ESP_LOGW(TAG, "Wi-Fi did not obtain an IP before the Cloud startup deadline");
            break;
        }

        uint32_t startup_bits = 0;
        (void)xTaskNotifyWait(
            0,
            UINT32_MAX,
            &startup_bits,
            pdMS_TO_TICKS(STARTUP_POLL_MS)
        );
    }

    size_t cold_attempt = 0;
    int64_t last_attempt_ms = 0;
    bool was_snapshot_ready = false;
#ifdef HUB_AMOLED_216
    scale_battery_reads_t scale_reads = {0};
#endif

    while (true) {
        if (hub_state_process_pending_slow_cloud_read()) {
            continue;
        }

        hub_cloud_snapshot_status_t status = {0};
        hub_cloud_get_snapshot_status(&status);

        int64_t activity_ms = 0;
        int64_t active_ms = 0;
        bool recovery = false;
        read_keeper_state(&activity_ms, &active_ms, &recovery);

        int64_t now = now_ms();
        /*
         * Cloud's connected flag may lag the local BLE/machine state.  Once
         * the selected-machine snapshot contains the complete control fields,
         * keep it on the normal cadence instead of hammering things/dashboard
         * as if startup had never completed.
         */
        bool snapshot_ready = status.has_cached_snapshot && status.has_complete_bbw;
        bool cold = !snapshot_ready || recovery;
        uint32_t interval_ms = cold
            ? cold_interval_ms(cold_attempt)
            : ready_interval_ms(now, activity_ms, active_ms,
                hub_ble_has_cached_state() && !status.is_connected);

        bool initial_attempt = last_attempt_ms == 0;
        int64_t cadence_due_ms = initial_attempt ? now : last_attempt_ms + interval_ms;
        int64_t token_due_ms = hub_cloud_token_renewal_due_ms();
        int64_t next_due_ms = cadence_due_ms;
        /*
         * Token renewal is independent from machine snapshot readiness.
         * A powered-off/unknown machine can keep the keeper on the cold
         * cadence for hours; ignoring token_due_ms in that state creates a
         * gap where Cloud controls fail until the next cold refresh.
         */
        if (token_due_ms > 0 && token_due_ms < next_due_ms) {
            next_due_ms = token_due_ms;
        }
        bool due = initial_attempt || recovery || now >= next_due_ms;
#ifdef HUB_AMOLED_216
        scale_battery_tick(&scale_reads, &status,
            due || (token_due_ms > 0 && token_due_ms <= now + 5000));
        now = now_ms();
        due = due || now >= next_due_ms;
#endif
        if (!due) {
            uint32_t wait_ms = (uint32_t)(next_due_ms - now);
            if (wait_ms > LOCAL_STATE_POLL_MS) {
                wait_ms = LOCAL_STATE_POLL_MS;
            }
            uint32_t bits = 0;
            (void)xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(wait_ms));
            // Recheck local reconnection without turning each wake into a Cloud read.
            continue;
        }

        bool recovery_wake = consume_recovery_request();
        last_attempt_ms = now_ms();
        uint32_t refresh_ticket = hub_cloud_refresh_request_generation();
        esp_err_t err = hub_cloud_refresh();
        if (err == ESP_ERR_INVALID_STATE && hub_cloud_refresh_is_running()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            hub_state_keeper_request_recovery();
        } else {
            hub_cloud_complete_refresh_request(refresh_ticket);
        }
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Cloud refresh already running or unavailable");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "Background Cloud refresh failed: %s", esp_err_to_name(err));
        }

        hub_cloud_get_snapshot_status(&status);
        snapshot_ready = status.has_cached_snapshot && status.has_complete_bbw;
        if (snapshot_ready) {
            cold_attempt = 0;
            if (recovery_wake || !was_snapshot_ready) {
                extend_active_window();
            }
        } else if (!initial_attempt && !recovery_wake
            && cold_attempt < sizeof(COLD_INTERVALS_MS) / sizeof(COLD_INTERVALS_MS[0])) {
            cold_attempt++;
        }
        was_snapshot_ready = snapshot_ready;
    }
}

esp_err_t hub_state_keeper_start(void)
{
    portENTER_CRITICAL(&keeper_lock);
    bool already_started = keeper_task_handle != NULL;
    if (!already_started && last_activity_ms == 0) {
        last_activity_ms = now_ms();
        active_until_ms = last_activity_ms + ACTIVE_WINDOW_MS;
    }
    portEXIT_CRITICAL(&keeper_lock);

    if (already_started) {
        return ESP_OK;
    }

    TaskHandle_t handle = NULL;
    BaseType_t created = xTaskCreate(
        hub_state_keeper_task,
        "hub_state_keeper",
        8192,
        NULL,
        4,
        &handle
    );
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&keeper_lock);
    if (keeper_task_handle == NULL) {
        keeper_task_handle = handle;
        handle = NULL;
    }
    portEXIT_CRITICAL(&keeper_lock);

    if (handle != NULL) {
        vTaskDelete(handle);
    }
    return ESP_OK;
}
