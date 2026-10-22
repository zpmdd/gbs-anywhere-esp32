#include "hub_cloud_websocket.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hub_cloud.h"
#include "hub_cloud_websocket_policy.h"
#include "hub_command_coordinator.h"
#include "hub_config.h"
#include "hub_runtime_resources.h"
#include "hub_stomp.h"
#include "hub_ws_reassembly.h"

#define HUB_CLOUD_WS_START_INTERNAL_FREE_BYTES 32768
#define HUB_CLOUD_WS_START_LARGEST_BLOCK_BYTES 12288
#define HUB_CLOUD_WS_LOW_INTERNAL_FREE_BYTES 24576
#define HUB_CLOUD_WS_LOW_LARGEST_BLOCK_BYTES 8192
#define HUB_CLOUD_WS_FRAME_CAPACITY 16384
#define HUB_CLOUD_WS_COMPONENT_BUFFER_BYTES 2048
#define HUB_CLOUD_WS_SUPERVISOR_STACK_BYTES 8192
#define HUB_CLOUD_WS_DIAGNOSTICS_BYTES 2048
#define HUB_CLOUD_WS_STATE_BYTES 768
#define HUB_CLOUD_WS_RESOURCE_SAMPLE_MS 10000LL
#define HUB_CLOUD_WS_AUTH_RETRY_MS 15000LL
#define HUB_CLOUD_WS_CONNECT_TIMEOUT_MS 30000LL
#define HUB_CLOUD_WS_BOOT_GUARD_CLEAR_MS 60000LL
#define HUB_CLOUD_WS_STARTUP_GRACE_MS 15000LL
#define HUB_CLOUD_WS_RUNTIME_ENABLE_GRACE_MS 5000LL

static const char *TAG = "hub_cloud_ws";
static const char *CLOUD_WEBSOCKET_URI = "wss://lion.lamarzocco.io/ws/connect";

typedef enum {
    WS_RUNTIME_DISABLED = 0,
    WS_RUNTIME_WAITING_AUTH,
    WS_RUNTIME_RESOURCE_BLOCKED,
    WS_RUNTIME_CONNECTING,
    WS_RUNTIME_TRANSPORT_CONNECTED,
    WS_RUNTIME_SUBSCRIBED,
    WS_RUNTIME_BACKOFF,
    WS_RUNTIME_CIRCUIT_OPEN,
    WS_RUNTIME_STOPPING,
} websocket_runtime_state_t;

typedef struct {
    bool valid;
    int64_t observed_at_ms;
    uint32_t selection_generation;
    uint32_t widget_mask;
    uint32_t body_hash;
    bool connected_set;
    bool connected;
    char serial_number[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
} websocket_observation_t;

typedef struct {
    unsigned slot;
    size_t frame_len;
    bool dashboard;
} websocket_observation_queue_item_t;

typedef struct {
    bool started;
    TaskHandle_t supervisor_task;
    esp_websocket_client_handle_t client;
    hub_cloud_websocket_config_t config;
    hub_cloud_websocket_policy_t policy;
    websocket_runtime_state_t state;
    bool transport_connected;
    bool stomp_connected;
    bool disconnect_pending;
    bool machine_selection_changed;
    bool boot_guard_armed;
    bool boot_guard_recovered;
    QueueHandle_t observation_queue;
    uint8_t pending_observation_slots;
    int64_t connect_started_ms;
    int64_t connected_at_ms;
    int64_t last_disconnect_ms;
    int64_t last_event_ms;
    int64_t next_attempt_ms;
    int64_t last_resource_sample_ms;
    uint32_t reconnect_attempt;
    uint32_t reconnect_count;
    uint32_t transport_connect_count;
    uint32_t stomp_connect_count;
    uint32_t frame_count;
    uint32_t dashboard_frame_count;
    uint32_t scheduling_frame_count;
    uint32_t receipt_count;
    uint32_t command_success_count;
    uint32_t command_failure_count;
    uint32_t parse_error_count;
    uint32_t parser_drop_count;
    uint32_t oversized_drop_count;
    uint32_t wrong_target_drop_count;
    uint32_t last_frame_bytes;
    uint32_t max_frame_bytes;
    uint32_t task_stack_high_water_bytes;
    uint32_t supervisor_stack_high_water_bytes;
    int last_error_type;
    int last_handshake_status;
    int last_transport_errno;
    esp_err_t last_esp_error;
    char last_error_code[48];
    char expected_serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1];
    uint32_t expected_generation;
    char *pending_access_token;
    char *frame_slots[2];
    hub_ws_reassembly_t reassembly;
    websocket_observation_t observation;
    char *diagnostics_body;
    char *state_body;
} websocket_runtime_t;

static websocket_runtime_t runtime;
static portMUX_TYPE runtime_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * NVS writes temporarily disable the flash/PSRAM cache. A task whose stack is
 * in PSRAM cannot safely enter that path and will trip IDF's cache safety
 * assertion. Keep this runtime guard even though both WebSocket tasks are
 * intentionally created with the normal internal-RAM FreeRTOS allocator.
 */
static bool current_stack_flash_safe(void)
{
    volatile uint8_t stack_marker = 0;
    return esp_ptr_in_dram((const void *)&stack_marker);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *runtime_state_name(websocket_runtime_state_t state)
{
    switch (state) {
    case WS_RUNTIME_DISABLED:
        return "disabled";
    case WS_RUNTIME_WAITING_AUTH:
        return "waiting_auth";
    case WS_RUNTIME_RESOURCE_BLOCKED:
        return "resource_blocked";
    case WS_RUNTIME_CONNECTING:
        return "connecting";
    case WS_RUNTIME_TRANSPORT_CONNECTED:
        return "transport_connected";
    case WS_RUNTIME_SUBSCRIBED:
        return "subscribed";
    case WS_RUNTIME_BACKOFF:
        return "backoff";
    case WS_RUNTIME_CIRCUIT_OPEN:
        return "circuit_open";
    case WS_RUNTIME_STOPPING:
        return "stopping";
    }
    return "unknown";
}

static void secure_clear(void *value, size_t length)
{
    if (value == NULL) {
        return;
    }
    volatile unsigned char *cursor = (volatile unsigned char *)value;
    for (size_t index = 0; index < length; index++) {
        cursor[index] = 0;
    }
}

static void set_state(websocket_runtime_state_t state)
{
    portENTER_CRITICAL(&runtime_lock);
    runtime.state = state;
    portEXIT_CRITICAL(&runtime_lock);
}

static void set_error_code(const char *code)
{
    portENTER_CRITICAL(&runtime_lock);
    snprintf(runtime.last_error_code, sizeof(runtime.last_error_code), "%s", code != NULL ? code : "");
    portEXIT_CRITICAL(&runtime_lock);
}

static void update_websocket_stack_watermark(void)
{
    uint32_t watermark = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&runtime_lock);
    if (runtime.task_stack_high_water_bytes == 0
        || watermark < runtime.task_stack_high_water_bytes) {
        runtime.task_stack_high_water_bytes = watermark;
    }
    portEXIT_CRITICAL(&runtime_lock);
}

static void update_supervisor_stack_watermark(void)
{
    uint32_t watermark = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&runtime_lock);
    if (runtime.supervisor_stack_high_water_bytes == 0
        || watermark < runtime.supervisor_stack_high_water_bytes) {
        runtime.supervisor_stack_high_water_bytes = watermark;
    }
    portEXIT_CRITICAL(&runtime_lock);
}

static void free_frame_buffers(void)
{
    for (size_t index = 0; index < 2; index++) {
        if (runtime.frame_slots[index] != NULL) {
            secure_clear(runtime.frame_slots[index], HUB_CLOUD_WS_FRAME_CAPACITY + 1);
            free(runtime.frame_slots[index]);
            runtime.frame_slots[index] = NULL;
        }
    }
    if (runtime.pending_access_token != NULL) {
        secure_clear(runtime.pending_access_token, HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1);
        free(runtime.pending_access_token);
        runtime.pending_access_token = NULL;
    }
    hub_ws_reassembly_reset(&runtime.reassembly);
}

static esp_err_t allocate_frame_buffers(void)
{
    if (runtime.frame_slots[0] != NULL
        && runtime.frame_slots[1] != NULL
        && runtime.pending_access_token != NULL) {
        return ESP_OK;
    }
    free_frame_buffers();
    for (size_t index = 0; index < 2; index++) {
        runtime.frame_slots[index] = heap_caps_calloc(
            1,
            HUB_CLOUD_WS_FRAME_CAPACITY + 1,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (runtime.frame_slots[index] == NULL) {
            free_frame_buffers();
            return ESP_ERR_NO_MEM;
        }
    }
    runtime.pending_access_token = heap_caps_calloc(
        1,
        HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (runtime.pending_access_token == NULL) {
        free_frame_buffers();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static uint32_t fnv1a(const char *data, size_t length)
{
    uint32_t value = 2166136261U;
    for (size_t index = 0; index < length; index++) {
        value ^= (unsigned char)data[index];
        value *= 16777619U;
    }
    return value;
}

static uint32_t dashboard_widget_bit(const char *code)
{
    static const char *codes[] = {
        "CMMachineStatus",
        "CMCoffeeBoiler",
        "CMSteamBoilerLevel",
        "CMPreBrewing",
        "CMBrewByWeightDoses",
        "CMRinseFlush",
        "CMBackFlush",
        "ThingScale",
    };
    if (code == NULL) {
        return 0;
    }
    for (size_t index = 0; index < sizeof(codes) / sizeof(codes[0]); index++) {
        if (strcmp(code, codes[index]) == 0) {
            return 1U << index;
        }
    }
    return 0;
}

static cJSON *find_named_item(cJSON *value, const char *name, unsigned depth)
{
    if (value == NULL || name == NULL || depth > 8) {
        return NULL;
    }
    if (cJSON_IsObject(value)) {
        cJSON *direct = cJSON_GetObjectItemCaseSensitive(value, name);
        if (direct != NULL) {
            return direct;
        }
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, value) {
        cJSON *found = find_named_item(child, name, depth + 1);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void observe_command_receipts(cJSON *root)
{
    cJSON *commands = find_named_item(root, "commands", 0);
    if (!cJSON_IsArray(commands)) {
        return;
    }
    uint32_t success = 0;
    uint32_t failure = 0;
    cJSON *command = NULL;
    cJSON_ArrayForEach(command, commands) {
        cJSON *status = cJSON_GetObjectItemCaseSensitive(command, "status");
        if (!cJSON_IsString(status) || status->valuestring == NULL) {
            continue;
        }
        if (strcasecmp(status->valuestring, "Success") == 0) {
            success++;
        } else if (strcasecmp(status->valuestring, "Failed") == 0
                   || strcasecmp(status->valuestring, "Failure") == 0
                   || strcasecmp(status->valuestring, "Error") == 0) {
            failure++;
        }
    }
    portENTER_CRITICAL(&runtime_lock);
    runtime.receipt_count += success + failure;
    runtime.command_success_count += success;
    runtime.command_failure_count += failure;
    portEXIT_CRITICAL(&runtime_lock);
}

static bool destination_matches_expected(const char *destination, const char *suffix)
{
    char expected[160] = {0};
    portENTER_CRITICAL(&runtime_lock);
    snprintf(expected, sizeof(expected), "/ws/sn/%s/%s", runtime.expected_serial, suffix);
    portEXIT_CRITICAL(&runtime_lock);
    return destination != NULL && strcmp(destination, expected) == 0;
}

static void render_observation(const websocket_observation_t *observation)
{
    if (runtime.state_body == NULL || observation == NULL) {
        return;
    }
    snprintf(
        runtime.state_body,
        HUB_CLOUD_WS_STATE_BYTES,
        "{"
        "\"valid\":%s,"
        "\"mergeActive\":false,"
        "\"observedAtMs\":%" PRId64 ","
        "\"serialNumber\":\"%s\","
        "\"selectionGeneration\":%" PRIu32 ","
        "\"connected\":%s,"
        "\"widgetMask\":%" PRIu32 ","
        "\"bodyHash\":\"%08" PRIx32 "\""
        "}",
        observation->valid ? "true" : "false",
        observation->observed_at_ms,
        observation->serial_number,
        observation->selection_generation,
        observation->connected_set
            ? (observation->connected ? "true" : "false")
            : "null",
        observation->widget_mask,
        observation->body_hash
    );
}

static void observe_json_message(const hub_stomp_frame_t *frame, bool dashboard)
{
    if (frame == NULL || frame->body == NULL || frame->body_len == 0) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.parse_error_count++;
        portEXIT_CRITICAL(&runtime_lock);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(frame->body, frame->body_len);
    if (root == NULL) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.parse_error_count++;
        portEXIT_CRITICAL(&runtime_lock);
        set_error_code("stomp_json_invalid");
        return;
    }

    uint32_t widget_mask = 0;
    if (dashboard) {
        cJSON *widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
        cJSON *widget = NULL;
        cJSON_ArrayForEach(widget, widgets) {
            cJSON *code = cJSON_GetObjectItemCaseSensitive(widget, "code");
            if (cJSON_IsString(code) && code->valuestring != NULL) {
                widget_mask |= dashboard_widget_bit(code->valuestring);
            }
        }
    }
    observe_command_receipts(root);
    cJSON *connected = cJSON_GetObjectItemCaseSensitive(root, "connected");
    uint32_t hash = fnv1a(frame->body, frame->body_len);
    int64_t observed_ms = now_ms();
    websocket_observation_t observation = {0};

    portENTER_CRITICAL(&runtime_lock);
    runtime.observation.valid = true;
    runtime.observation.observed_at_ms = observed_ms;
    runtime.observation.selection_generation = runtime.expected_generation;
    snprintf(
        runtime.observation.serial_number,
        sizeof(runtime.observation.serial_number),
        "%s",
        runtime.expected_serial
    );
    runtime.observation.body_hash = hash;
    if (dashboard) {
        runtime.observation.widget_mask = widget_mask;
    }
    if (cJSON_IsBool(connected)) {
        runtime.observation.connected_set = true;
        runtime.observation.connected = cJSON_IsTrue(connected);
    }
    observation = runtime.observation;
    portEXIT_CRITICAL(&runtime_lock);
    render_observation(&observation);
    cJSON_Delete(root);
}

static void release_observation_slot(unsigned slot)
{
    if (slot >= HUB_WS_REASSEMBLY_SLOT_COUNT) {
        return;
    }
    portENTER_CRITICAL(&runtime_lock);
    runtime.pending_observation_slots &= (uint8_t)~(1U << slot);
    portEXIT_CRITICAL(&runtime_lock);
}

static bool observation_slot_busy(unsigned slot)
{
    if (slot >= HUB_WS_REASSEMBLY_SLOT_COUNT) {
        return true;
    }
    portENTER_CRITICAL(&runtime_lock);
    bool busy = (runtime.pending_observation_slots & (1U << slot)) != 0;
    portEXIT_CRITICAL(&runtime_lock);
    return busy;
}

static bool queue_observation(unsigned slot, size_t frame_len, bool dashboard)
{
    QueueHandle_t queue = runtime.observation_queue;
    if (queue == NULL || slot >= HUB_WS_REASSEMBLY_SLOT_COUNT) {
        return false;
    }
    websocket_observation_queue_item_t item = {
        .slot = slot,
        .frame_len = frame_len,
        .dashboard = dashboard,
    };
    portENTER_CRITICAL(&runtime_lock);
    bool already_busy = (runtime.pending_observation_slots & (1U << slot)) != 0;
    if (!already_busy) {
        runtime.pending_observation_slots |= (uint8_t)(1U << slot);
    }
    portEXIT_CRITICAL(&runtime_lock);
    if (already_busy || xQueueSend(queue, &item, 0) != pdTRUE) {
        if (!already_busy) {
            release_observation_slot(slot);
        }
        portENTER_CRITICAL(&runtime_lock);
        runtime.parser_drop_count++;
        portEXIT_CRITICAL(&runtime_lock);
        return false;
    }
    TaskHandle_t supervisor = runtime.supervisor_task;
    if (supervisor != NULL) {
        xTaskNotifyGive(supervisor);
    }
    return true;
}

static void process_pending_observations(void)
{
    QueueHandle_t queue = runtime.observation_queue;
    if (queue == NULL) {
        return;
    }
    websocket_observation_queue_item_t item = {0};
    while (xQueueReceive(queue, &item, 0) == pdTRUE) {
        bool valid_slot = item.slot < HUB_WS_REASSEMBLY_SLOT_COUNT
            && runtime.frame_slots[item.slot] != NULL;
        hub_stomp_frame_t frame = {0};
        if (valid_slot
            && hub_stomp_parse(runtime.frame_slots[item.slot], item.frame_len, &frame)
            && frame.type == HUB_STOMP_MESSAGE) {
            observe_json_message(&frame, item.dashboard);
        } else {
            portENTER_CRITICAL(&runtime_lock);
            runtime.parse_error_count++;
            portEXIT_CRITICAL(&runtime_lock);
            set_error_code("stomp_deferred_parse_invalid");
        }
        release_observation_slot(item.slot);
        update_supervisor_stack_watermark();
    }
}

static void clear_pending_observations(void)
{
    if (runtime.observation_queue != NULL) {
        (void)xQueueReset(runtime.observation_queue);
    }
    portENTER_CRITICAL(&runtime_lock);
    runtime.pending_observation_slots = 0;
    portEXIT_CRITICAL(&runtime_lock);
}

static bool send_text_frame(
    esp_websocket_client_handle_t client,
    const char *frame,
    size_t frame_len
)
{
    if (client == NULL || frame == NULL || frame_len == 0 || frame_len > INT32_MAX) {
        return false;
    }
    int sent = esp_websocket_client_send_text(
        client,
        frame,
        (int)frame_len,
        pdMS_TO_TICKS(1000)
    );
    return sent == (int)frame_len;
}

static bool send_subscriptions(esp_websocket_client_handle_t client)
{
    char serial[HUB_CONFIG_MAX_MACHINE_SERIAL_LEN + 1] = {0};
    portENTER_CRITICAL(&runtime_lock);
    snprintf(serial, sizeof(serial), "%s", runtime.expected_serial);
    portEXIT_CRITICAL(&runtime_lock);

    char frame[320] = {0};
    char destination[160] = {0};
    uint32_t nonce = esp_random();
    char subscription_id[48] = {0};

    snprintf(destination, sizeof(destination), "/ws/sn/%s/dashboard", serial);
    snprintf(subscription_id, sizeof(subscription_id), "lb-dashboard-%08" PRIx32, nonce);
    size_t length = hub_stomp_write_subscribe(
        frame,
        sizeof(frame),
        subscription_id,
        destination
    );
    if (length == 0 || !send_text_frame(client, frame, length)) {
        return false;
    }

    memset(frame, 0, sizeof(frame));
    snprintf(destination, sizeof(destination), "/ws/sn/%s/scheduling", serial);
    snprintf(subscription_id, sizeof(subscription_id), "lb-schedule-%08" PRIx32, nonce);
    length = hub_stomp_write_subscribe(frame, sizeof(frame), subscription_id, destination);
    return length > 0 && send_text_frame(client, frame, length);
}

static void request_disconnect(const char *error_code)
{
    portENTER_CRITICAL(&runtime_lock);
    runtime.disconnect_pending = true;
    if (error_code != NULL) {
        snprintf(runtime.last_error_code, sizeof(runtime.last_error_code), "%s", error_code);
    }
    TaskHandle_t supervisor = runtime.supervisor_task;
    portEXIT_CRITICAL(&runtime_lock);
    if (supervisor != NULL) {
        xTaskNotifyGive(supervisor);
    }
}

static void process_stomp_frame(
    char *data,
    size_t length,
    esp_websocket_client_handle_t client,
    unsigned frame_slot
)
{
    hub_stomp_frame_t frame = {0};
    if (!hub_stomp_parse(data, length, &frame)) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.parse_error_count++;
        portEXIT_CRITICAL(&runtime_lock);
        set_error_code("stomp_frame_invalid");
        return;
    }
    int64_t observed_ms = now_ms();
    portENTER_CRITICAL(&runtime_lock);
    runtime.frame_count++;
    runtime.last_event_ms = observed_ms;
    runtime.last_frame_bytes = (uint32_t)length;
    if (length > runtime.max_frame_bytes) {
        runtime.max_frame_bytes = (uint32_t)length;
    }
    portEXIT_CRITICAL(&runtime_lock);

    if (frame.type == HUB_STOMP_HEARTBEAT) {
        return;
    }
    if (frame.type == HUB_STOMP_CONNECTED) {
        if (!send_subscriptions(client)) {
            request_disconnect("stomp_subscribe_failed");
            return;
        }
        portENTER_CRITICAL(&runtime_lock);
        runtime.stomp_connected = true;
        runtime.stomp_connect_count++;
        runtime.reconnect_attempt = 0;
        runtime.state = WS_RUNTIME_SUBSCRIBED;
        runtime.last_error_code[0] = '\0';
        portEXIT_CRITICAL(&runtime_lock);
        return;
    }
    if (frame.type == HUB_STOMP_MESSAGE) {
        if (destination_matches_expected(frame.destination, "dashboard")) {
            portENTER_CRITICAL(&runtime_lock);
            runtime.dashboard_frame_count++;
            portEXIT_CRITICAL(&runtime_lock);
            if (!queue_observation(frame_slot, length, true)) {
                request_disconnect("websocket_parser_backpressure");
            }
        } else if (destination_matches_expected(frame.destination, "scheduling")) {
            portENTER_CRITICAL(&runtime_lock);
            runtime.scheduling_frame_count++;
            portEXIT_CRITICAL(&runtime_lock);
            if (!queue_observation(frame_slot, length, false)) {
                request_disconnect("websocket_parser_backpressure");
            }
        } else {
            portENTER_CRITICAL(&runtime_lock);
            runtime.wrong_target_drop_count++;
            portEXIT_CRITICAL(&runtime_lock);
        }
        return;
    }
    if (frame.type == HUB_STOMP_RECEIPT) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.receipt_count++;
        portEXIT_CRITICAL(&runtime_lock);
        return;
    }
    if (frame.type == HUB_STOMP_ERROR) {
        request_disconnect("stomp_server_error");
    }
}

static void process_data_event(esp_websocket_event_data_t *event)
{
    if (event == NULL || event->data_len < 0 || event->payload_len < 0 || event->payload_offset < 0) {
        request_disconnect("websocket_data_invalid");
        return;
    }
    /*
     * esp_websocket_client dispatches PING/PONG/CLOSE as DATA events before it
     * handles those control frames internally. They are transport traffic,
     * not STOMP payloads, so observing them would create false parse errors.
     */
    if (event->op_code == 0x08 || event->op_code == 0x09 || event->op_code == 0x0A) {
        return;
    }
    if (event->op_code == 0x02) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.parse_error_count++;
        portEXIT_CRITICAL(&runtime_lock);
        set_error_code("websocket_binary_unsupported");
        return;
    }
    if (event->op_code != 0x00 && event->op_code != 0x01) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.parse_error_count++;
        portEXIT_CRITICAL(&runtime_lock);
        set_error_code("websocket_opcode_unsupported");
        return;
    }
    if (event->op_code == 0x01 && event->payload_offset == 0) {
        unsigned next_slot = (runtime.reassembly.active_slot + 1U)
            % HUB_WS_REASSEMBLY_SLOT_COUNT;
        if (observation_slot_busy(next_slot)) {
            portENTER_CRITICAL(&runtime_lock);
            runtime.parser_drop_count++;
            portEXIT_CRITICAL(&runtime_lock);
            request_disconnect("websocket_parser_backpressure");
            return;
        }
    }
    char *completed_data = NULL;
    size_t completed_len = 0;
    hub_ws_reassembly_result_t result = hub_ws_reassembly_feed(
        &runtime.reassembly,
        runtime.frame_slots,
        HUB_CLOUD_WS_FRAME_CAPACITY,
        event->op_code,
        event->fin,
        (size_t)event->payload_len,
        (size_t)event->payload_offset,
        event->data_ptr,
        (size_t)event->data_len,
        &completed_data,
        &completed_len
    );
    switch (result) {
    case HUB_WS_REASSEMBLY_MORE:
        return;
    case HUB_WS_REASSEMBLY_COMPLETE:
        process_stomp_frame(
            completed_data,
            completed_len,
            event->client,
            runtime.reassembly.active_slot
        );
        return;
    case HUB_WS_REASSEMBLY_UNEXPECTED_CONTINUATION:
        request_disconnect("websocket_continuation_unexpected");
        return;
    case HUB_WS_REASSEMBLY_UNEXPECTED_TEXT:
        request_disconnect("websocket_text_during_fragment");
        return;
    case HUB_WS_REASSEMBLY_OVERSIZED:
        portENTER_CRITICAL(&runtime_lock);
        runtime.oversized_drop_count++;
        portEXIT_CRITICAL(&runtime_lock);
        request_disconnect("websocket_frame_oversized");
        return;
    case HUB_WS_REASSEMBLY_INVALID:
        request_disconnect("websocket_fragment_invalid");
        return;
    }
}

static void websocket_event_handler(
    void *handler_args,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_args;
    (void)event_base;
    esp_websocket_event_data_t *event = event_data;
    update_websocket_stack_watermark();

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        char *connect_frame = runtime.frame_slots[0];
        size_t frame_len = hub_stomp_write_connect(
            connect_frame,
            HUB_CLOUD_WS_FRAME_CAPACITY + 1,
            runtime.pending_access_token
        );
        secure_clear(
            runtime.pending_access_token,
            runtime.pending_access_token != NULL ? HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1 : 0
        );
        portENTER_CRITICAL(&runtime_lock);
        runtime.transport_connected = true;
        runtime.transport_connect_count++;
        runtime.connected_at_ms = now_ms();
        runtime.state = WS_RUNTIME_TRANSPORT_CONNECTED;
        portEXIT_CRITICAL(&runtime_lock);
        if (frame_len == 0 || !send_text_frame(event != NULL ? event->client : NULL, connect_frame, frame_len)) {
            secure_clear(connect_frame, frame_len);
            request_disconnect("stomp_connect_send_failed");
            return;
        }
        secure_clear(connect_frame, frame_len);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DATA) {
        process_data_event(event);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR && event != NULL) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.last_error_type = event->error_handle.error_type;
        runtime.last_handshake_status = event->error_handle.esp_ws_handshake_status_code;
        runtime.last_transport_errno = event->error_handle.esp_transport_sock_errno;
        runtime.last_esp_error = event->error_handle.esp_tls_last_esp_err;
        snprintf(runtime.last_error_code, sizeof(runtime.last_error_code), "websocket_transport_error");
        portEXIT_CRITICAL(&runtime_lock);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED
        || event_id == WEBSOCKET_EVENT_CLOSED
        || event_id == WEBSOCKET_EVENT_FINISH) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.transport_connected = false;
        runtime.stomp_connected = false;
        runtime.disconnect_pending = true;
        runtime.last_disconnect_ms = now_ms();
        if (runtime.last_error_code[0] == '\0') {
            snprintf(runtime.last_error_code, sizeof(runtime.last_error_code), "websocket_disconnected");
        }
        TaskHandle_t supervisor = runtime.supervisor_task;
        portEXIT_CRITICAL(&runtime_lock);
        if (supervisor != NULL) {
            xTaskNotifyGive(supervisor);
        }
    }
}

static void stop_client(void)
{
    esp_websocket_client_handle_t client = NULL;
    portENTER_CRITICAL(&runtime_lock);
    client = runtime.client;
    runtime.client = NULL;
    runtime.transport_connected = false;
    runtime.stomp_connected = false;
    runtime.state = WS_RUNTIME_STOPPING;
    portEXIT_CRITICAL(&runtime_lock);
    if (client != NULL) {
        (void)esp_websocket_client_stop(client);
        (void)esp_websocket_unregister_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        (void)esp_websocket_client_destroy(client);
    }
    clear_pending_observations();
    free_frame_buffers();
}

static esp_err_t start_client(void)
{
    hub_runtime_resources_t resources = {0};
    hub_runtime_resources_snapshot(&resources);
    int64_t current_ms = now_ms();
    hub_cloud_websocket_policy_t policy = {0};
    portENTER_CRITICAL(&runtime_lock);
    policy = runtime.policy;
    portEXIT_CRITICAL(&runtime_lock);
    if (!hub_cloud_websocket_policy_start_eligible(
            &policy,
            resources.psram_initialized,
            resources.internal_free_bytes,
            resources.internal_largest_block_bytes,
            current_ms
        )) {
        set_state(
            policy.circuit_open_until_ms > current_ms
                ? WS_RUNTIME_CIRCUIT_OPEN
                : WS_RUNTIME_RESOURCE_BLOCKED
        );
        set_error_code("websocket_resource_gate");
        return ESP_ERR_NO_MEM;
    }
    if (hub_command_interactive_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(allocate_frame_buffers(), TAG, "frame buffer allocation failed");

    hub_cloud_websocket_auth_t *auth = heap_caps_calloc(
        1,
        sizeof(*auth),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (auth == NULL) {
        free_frame_buffers();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t auth_err = hub_cloud_get_websocket_auth(auth);
    if (auth_err != ESP_OK) {
        hub_cloud_clear_websocket_auth(auth);
        free(auth);
        free_frame_buffers();
        set_state(WS_RUNTIME_WAITING_AUTH);
        set_error_code("websocket_auth_not_ready");
        return auth_err;
    }
    snprintf(
        runtime.pending_access_token,
        HUB_CONFIG_MAX_CLOUD_TOKEN_LEN + 1,
        "%s",
        auth->access_token
    );
    portENTER_CRITICAL(&runtime_lock);
    snprintf(runtime.expected_serial, sizeof(runtime.expected_serial), "%s", auth->machine_serial);
    runtime.expected_generation = auth->selection_generation;
    runtime.disconnect_pending = false;
    runtime.machine_selection_changed = false;
    runtime.connect_started_ms = current_ms;
    runtime.state = WS_RUNTIME_CONNECTING;
    portEXIT_CRITICAL(&runtime_lock);

    esp_websocket_client_config_t config = {
        .uri = CLOUD_WEBSOCKET_URI,
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .task_core_id_set = true,
        .task_core_id = 1,
        .task_prio = 3,
        .task_name = "hub_cloud_ws",
        .task_stack = 8192,
        .buffer_size = HUB_CLOUD_WS_COMPONENT_BUFFER_BYTES,
        .headers = auth->handshake_headers,
        .pingpong_timeout_sec = 30,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .keep_alive_idle = 15,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    hub_cloud_clear_websocket_auth(auth);
    free(auth);
    if (client == NULL) {
        free_frame_buffers();
        set_error_code("websocket_init_failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(
        client,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        NULL
    );
    if (err == ESP_OK) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.client = client;
        portEXIT_CRITICAL(&runtime_lock);
        err = esp_websocket_client_start(client);
    }
    if (err != ESP_OK) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.client = NULL;
        portEXIT_CRITICAL(&runtime_lock);
        (void)esp_websocket_client_destroy(client);
        free_frame_buffers();
        set_error_code("websocket_start_failed");
    }
    return err;
}

static void schedule_backoff(int64_t current_ms)
{
    uint32_t reconnect_attempt = 0;
    portENTER_CRITICAL(&runtime_lock);
    reconnect_attempt = runtime.reconnect_attempt;
    portEXIT_CRITICAL(&runtime_lock);
    uint32_t delay_ms = hub_cloud_websocket_policy_backoff_ms(
        reconnect_attempt,
        esp_random() % 1001U
    );
    portENTER_CRITICAL(&runtime_lock);
    if (runtime.reconnect_attempt < UINT32_MAX) {
        runtime.reconnect_attempt++;
    }
    runtime.reconnect_count++;
    runtime.next_attempt_ms = current_ms + delay_ms;
    runtime.state = WS_RUNTIME_BACKOFF;
    portEXIT_CRITICAL(&runtime_lock);
}

static bool selection_still_matches(void)
{
    hub_machine_selection_identity_t expected = {0};
    portENTER_CRITICAL(&runtime_lock);
    snprintf(expected.serial_number, sizeof(expected.serial_number), "%s", runtime.expected_serial);
    expected.generation = runtime.expected_generation;
    portEXIT_CRITICAL(&runtime_lock);
    return hub_config_machine_selection_matches(&expected);
}

static void supervisor_task(void *parameter)
{
    (void)parameter;
    while (true) {
        update_supervisor_stack_watermark();
        process_pending_observations();
        int64_t current_ms = now_ms();
        hub_cloud_websocket_config_t config = {0};
        esp_websocket_client_handle_t client = NULL;
        bool disconnect_pending = false;
        int64_t next_attempt_ms = 0;
        int64_t connect_started_ms = 0;
        int64_t connected_at_ms = 0;
        bool stomp_connected = false;
        bool boot_guard_armed = false;
        portENTER_CRITICAL(&runtime_lock);
        config = runtime.config;
        client = runtime.client;
        disconnect_pending = runtime.disconnect_pending;
        next_attempt_ms = runtime.next_attempt_ms;
        connect_started_ms = runtime.connect_started_ms;
        connected_at_ms = runtime.connected_at_ms;
        stomp_connected = runtime.stomp_connected;
        boot_guard_armed = runtime.boot_guard_armed;
        portEXIT_CRITICAL(&runtime_lock);

        if (!config.enabled) {
            if (client != NULL) {
                stop_client();
            }
            set_state(WS_RUNTIME_DISABLED);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
            continue;
        }

        if (client != NULL && !selection_still_matches()) {
            portENTER_CRITICAL(&runtime_lock);
            runtime.machine_selection_changed = true;
            runtime.disconnect_pending = true;
            runtime.next_attempt_ms = current_ms + 5000;
            portEXIT_CRITICAL(&runtime_lock);
            disconnect_pending = true;
            set_error_code("websocket_machine_selection_changed");
        }

        if (client != NULL && !stomp_connected && connect_started_ms > 0
            && current_ms - connect_started_ms >= HUB_CLOUD_WS_CONNECT_TIMEOUT_MS) {
            disconnect_pending = true;
            request_disconnect("websocket_connect_timeout");
        }

        if (client != NULL && disconnect_pending) {
            bool changed = false;
            portENTER_CRITICAL(&runtime_lock);
            changed = runtime.machine_selection_changed;
            runtime.disconnect_pending = false;
            runtime.machine_selection_changed = false;
            portEXIT_CRITICAL(&runtime_lock);
            stop_client();
            portENTER_CRITICAL(&runtime_lock);
            int64_t circuit_open_until_ms = runtime.policy.circuit_open_until_ms;
            portEXIT_CRITICAL(&runtime_lock);
            if (circuit_open_until_ms > current_ms) {
                portENTER_CRITICAL(&runtime_lock);
                runtime.next_attempt_ms = circuit_open_until_ms;
                runtime.state = WS_RUNTIME_CIRCUIT_OPEN;
                portEXIT_CRITICAL(&runtime_lock);
            } else if (!changed) {
                schedule_backoff(current_ms);
            } else {
                portENTER_CRITICAL(&runtime_lock);
                runtime.state = WS_RUNTIME_BACKOFF;
                portEXIT_CRITICAL(&runtime_lock);
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        if (client != NULL) {
            if (stomp_connected && boot_guard_armed && connected_at_ms > 0
                && current_ms - connected_at_ms >= HUB_CLOUD_WS_BOOT_GUARD_CLEAR_MS) {
                if (!current_stack_flash_safe()) {
                    request_disconnect("websocket_nvs_stack_unsafe");
                } else {
                    esp_err_t guard_err = hub_config_set_cloud_websocket_boot_guard(false);
                    if (guard_err == ESP_OK) {
                        portENTER_CRITICAL(&runtime_lock);
                        runtime.boot_guard_armed = false;
                        portEXIT_CRITICAL(&runtime_lock);
                    } else {
                        set_error_code("websocket_boot_guard_clear_failed");
                    }
                }
            }
            bool sample_resources = false;
            portENTER_CRITICAL(&runtime_lock);
            sample_resources = runtime.last_resource_sample_ms == 0
                || current_ms - runtime.last_resource_sample_ms >= HUB_CLOUD_WS_RESOURCE_SAMPLE_MS;
            if (sample_resources) {
                runtime.last_resource_sample_ms = current_ms;
            }
            portEXIT_CRITICAL(&runtime_lock);
            if (sample_resources) {
                hub_runtime_resources_t resources = {0};
                hub_runtime_resources_snapshot(&resources);
                portENTER_CRITICAL(&runtime_lock);
                bool circuit_opened = hub_cloud_websocket_policy_sample_low_resources(
                        &runtime.policy,
                        resources.internal_free_bytes,
                        resources.internal_largest_block_bytes,
                        current_ms
                    );
                portEXIT_CRITICAL(&runtime_lock);
                if (circuit_opened) {
                    request_disconnect("websocket_resource_circuit_open");
                }
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        if (next_attempt_ms > current_ms || hub_command_interactive_pending()) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        esp_err_t err = start_client();
        update_supervisor_stack_watermark();
        if (err != ESP_OK) {
            int64_t retry_at = now_ms() + HUB_CLOUD_WS_AUTH_RETRY_MS;
            portENTER_CRITICAL(&runtime_lock);
            bool waiting_auth = runtime.state == WS_RUNTIME_WAITING_AUTH;
            bool resource_blocked = runtime.state == WS_RUNTIME_RESOURCE_BLOCKED
                || runtime.state == WS_RUNTIME_CIRCUIT_OPEN;
            if (waiting_auth || resource_blocked || err == ESP_ERR_INVALID_STATE) {
                runtime.next_attempt_ms = runtime.policy.circuit_open_until_ms > retry_at
                    ? runtime.policy.circuit_open_until_ms
                    : retry_at;
            }
            portEXIT_CRITICAL(&runtime_lock);
            if (!waiting_auth && !resource_blocked && err != ESP_ERR_INVALID_STATE) {
                schedule_backoff(now_ms());
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

esp_err_t hub_cloud_websocket_start(void)
{
    if (runtime.started) {
        return ESP_OK;
    }
    if (!current_stack_flash_safe()) {
        ESP_LOGE(TAG, "Refusing WebSocket startup NVS access from external task stack");
        return ESP_ERR_INVALID_STATE;
    }
    hub_cloud_websocket_settings_t stored = {0};
    ESP_RETURN_ON_ERROR(
        hub_config_get_cloud_websocket_settings(&stored),
        TAG,
        "failed to load WebSocket settings"
    );
    bool boot_guard_armed = false;
    bool boot_guard_present = false;
    esp_err_t guard_err = hub_config_get_cloud_websocket_boot_guard(
        &boot_guard_armed,
        &boot_guard_present
    );
    if (guard_err != ESP_OK) {
        stored.enabled = false;
        stored.merge_enabled = false;
        (void)hub_config_save_cloud_websocket_settings(&stored);
    } else if (stored.enabled && !boot_guard_present) {
        /*
         * Builds predating the boot guard may have persisted ws_enabled=true.
         * Never carry that unsafe state into the guarded implementation: make
         * the first upgraded boot self-recover before a socket task can start.
         */
        stored.enabled = false;
        stored.merge_enabled = false;
        ESP_RETURN_ON_ERROR(
            hub_config_save_cloud_websocket_settings(&stored),
            TAG,
            "failed to migrate legacy WebSocket enable state"
        );
        (void)hub_config_set_cloud_websocket_boot_guard(false);
        runtime.boot_guard_recovered = true;
        snprintf(
            runtime.last_error_code,
            sizeof(runtime.last_error_code),
            "websocket_legacy_enable_recovered"
        );
    } else if (stored.enabled && boot_guard_armed) {
        stored.enabled = false;
        stored.merge_enabled = false;
        ESP_RETURN_ON_ERROR(
            hub_config_save_cloud_websocket_settings(&stored),
            TAG,
            "failed to recover WebSocket boot guard"
        );
        (void)hub_config_set_cloud_websocket_boot_guard(false);
        runtime.boot_guard_recovered = true;
        snprintf(
            runtime.last_error_code,
            sizeof(runtime.last_error_code),
            "websocket_boot_guard_recovered"
        );
    } else if (stored.enabled) {
        guard_err = hub_config_set_cloud_websocket_boot_guard(true);
        if (guard_err != ESP_OK) {
            stored.enabled = false;
            stored.merge_enabled = false;
            (void)hub_config_save_cloud_websocket_settings(&stored);
            snprintf(
                runtime.last_error_code,
                sizeof(runtime.last_error_code),
                "websocket_boot_guard_arm_failed"
            );
        } else {
            runtime.boot_guard_armed = true;
        }
    }
    runtime.config.enabled = stored.enabled;
    runtime.config.merge_enabled = stored.merge_enabled;
    runtime.config.field_mask = stored.field_mask;
    runtime.state = stored.enabled ? WS_RUNTIME_WAITING_AUTH : WS_RUNTIME_DISABLED;
    runtime.next_attempt_ms = stored.enabled
        ? now_ms() + HUB_CLOUD_WS_STARTUP_GRACE_MS
        : 0;
    runtime.diagnostics_body = heap_caps_calloc(
        1,
        HUB_CLOUD_WS_DIAGNOSTICS_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    runtime.state_body = heap_caps_calloc(
        1,
        HUB_CLOUD_WS_STATE_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (runtime.diagnostics_body == NULL || runtime.state_body == NULL) {
        free(runtime.diagnostics_body);
        free(runtime.state_body);
        runtime.diagnostics_body = NULL;
        runtime.state_body = NULL;
        return ESP_ERR_NO_MEM;
    }
    runtime.observation_queue = xQueueCreate(
        HUB_WS_REASSEMBLY_SLOT_COUNT,
        sizeof(websocket_observation_queue_item_t)
    );
    if (runtime.observation_queue == NULL) {
        free(runtime.diagnostics_body);
        free(runtime.state_body);
        runtime.diagnostics_body = NULL;
        runtime.state_body = NULL;
        return ESP_ERR_NO_MEM;
    }
    snprintf(runtime.state_body, HUB_CLOUD_WS_STATE_BYTES, "{\"valid\":false,\"mergeActive\":false}");
    hub_cloud_websocket_policy_reset(&runtime.policy);
    runtime.started = true;
    if (xTaskCreatePinnedToCore(
            supervisor_task,
            "hub_ws_supervisor",
            HUB_CLOUD_WS_SUPERVISOR_STACK_BYTES,
            NULL,
            2,
            &runtime.supervisor_task,
            1
        ) != pdPASS) {
        runtime.started = false;
        free(runtime.diagnostics_body);
        free(runtime.state_body);
        runtime.diagnostics_body = NULL;
        runtime.state_body = NULL;
        vQueueDelete(runtime.observation_queue);
        runtime.observation_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t hub_cloud_websocket_apply_config(const hub_cloud_websocket_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!current_stack_flash_safe()) {
        ESP_LOGE(TAG, "Refusing WebSocket config NVS access from external task stack");
        return ESP_ERR_INVALID_STATE;
    }
    /* Merge is intentionally held closed until observe-only hardware gates pass. */
    if (config->merge_enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    hub_cloud_websocket_settings_t stored = {
        .enabled = config->enabled,
        .merge_enabled = false,
        .field_mask = config->field_mask,
    };
    if (config->enabled) {
        ESP_RETURN_ON_ERROR(
            hub_config_set_cloud_websocket_boot_guard(true),
            TAG,
            "failed to arm WebSocket boot guard"
        );
    }
    esp_err_t save_err = hub_config_save_cloud_websocket_settings(&stored);
    if (save_err != ESP_OK) {
        if (config->enabled) {
            (void)hub_config_set_cloud_websocket_boot_guard(false);
        }
        return save_err;
    }
    if (!config->enabled) {
        ESP_RETURN_ON_ERROR(
            hub_config_set_cloud_websocket_boot_guard(false),
            TAG,
            "failed to clear WebSocket boot guard"
        );
    }
    portENTER_CRITICAL(&runtime_lock);
    runtime.config = *config;
    runtime.config.merge_enabled = false;
    runtime.boot_guard_armed = config->enabled;
    runtime.next_attempt_ms = config->enabled
        ? now_ms() + HUB_CLOUD_WS_RUNTIME_ENABLE_GRACE_MS
        : 0;
    TaskHandle_t supervisor = runtime.supervisor_task;
    portEXIT_CRITICAL(&runtime_lock);
    if (supervisor != NULL) {
        xTaskNotifyGive(supervisor);
    }
    return ESP_OK;
}

const char *hub_cloud_websocket_diagnostics_json(void)
{
    if (runtime.diagnostics_body == NULL) {
        return "{\"enabled\":false,\"state\":\"not_started\",\"mergeActive\":false}";
    }
    hub_runtime_resources_t resources = {0};
    hub_runtime_resources_snapshot(&resources);
    int64_t current_ms = now_ms();

    portENTER_CRITICAL(&runtime_lock);
    websocket_runtime_t snapshot = runtime;
    portEXIT_CRITICAL(&runtime_lock);
    int64_t event_age_ms = snapshot.last_event_ms > 0 ? current_ms - snapshot.last_event_ms : -1;
    int64_t observation_age_ms = snapshot.observation.valid
        ? current_ms - snapshot.observation.observed_at_ms
        : -1;
    int64_t backoff_ms = snapshot.next_attempt_ms > current_ms ? snapshot.next_attempt_ms - current_ms : 0;
    bool resource_eligible = hub_cloud_websocket_policy_start_eligible(
        &snapshot.policy,
        resources.psram_initialized,
        resources.internal_free_bytes,
        resources.internal_largest_block_bytes,
        current_ms
    );
    snprintf(
        runtime.diagnostics_body,
        HUB_CLOUD_WS_DIAGNOSTICS_BYTES,
        "{"
        "\"enabled\":%s,"
        "\"mergeRequested\":%s,"
        "\"mergeActive\":false,"
        "\"bootGuardArmed\":%s,"
        "\"bootGuardRecovered\":%s,"
        "\"fieldMask\":%" PRIu32 ","
        "\"state\":\"%s\","
        "\"transportConnected\":%s,"
        "\"stompConnected\":%s,"
        "\"resourceEligible\":%s,"
        "\"resourceCircuitOpen\":%s,"
        "\"backoffSeconds\":%" PRId64 ","
        "\"reconnectAttempt\":%" PRIu32 ","
        "\"reconnectCount\":%" PRIu32 ","
        "\"transportConnectCount\":%" PRIu32 ","
        "\"stompConnectCount\":%" PRIu32 ","
        "\"frameCount\":%" PRIu32 ","
        "\"frameCapacityBytes\":%u,"
        "\"componentBufferBytes\":%u,"
        "\"dashboardFrameCount\":%" PRIu32 ","
        "\"schedulingFrameCount\":%" PRIu32 ","
        "\"receiptCount\":%" PRIu32 ","
        "\"commandSuccessCount\":%" PRIu32 ","
        "\"commandFailureCount\":%" PRIu32 ","
        "\"parseErrorCount\":%" PRIu32 ","
        "\"parserDropCount\":%" PRIu32 ","
        "\"oversizedDropCount\":%" PRIu32 ","
        "\"wrongTargetDropCount\":%" PRIu32 ","
        "\"lastFrameBytes\":%" PRIu32 ","
        "\"maxFrameBytes\":%" PRIu32 ","
        "\"lastEventAgeMs\":%" PRId64 ","
        "\"observationValid\":%s,"
        "\"lastObservationAgeMs\":%" PRId64 ","
        "\"observedWidgetMask\":%" PRIu32 ","
        "\"observedBodyHash\":\"%08" PRIx32 "\","
        "\"taskStackHighWaterBytes\":%" PRIu32 ","
        "\"supervisorStackHighWaterBytes\":%" PRIu32 ","
        "\"lastErrorCode\":\"%s\","
        "\"lastErrorType\":%d,"
        "\"lastHandshakeStatus\":%d,"
        "\"lastTransportErrno\":%d,"
        "\"lastEspError\":%d,"
        "\"psramFreeBytes\":%lu,"
        "\"internalFreeHeap\":%lu,"
        "\"internalLargestFreeBlock\":%lu"
        "}",
        snapshot.config.enabled ? "true" : "false",
        snapshot.config.merge_enabled ? "true" : "false",
        snapshot.boot_guard_armed ? "true" : "false",
        snapshot.boot_guard_recovered ? "true" : "false",
        snapshot.config.field_mask,
        runtime_state_name(snapshot.state),
        snapshot.transport_connected ? "true" : "false",
        snapshot.stomp_connected ? "true" : "false",
        resource_eligible ? "true" : "false",
        snapshot.policy.circuit_open_until_ms > current_ms ? "true" : "false",
        backoff_ms / 1000,
        snapshot.reconnect_attempt,
        snapshot.reconnect_count,
        snapshot.transport_connect_count,
        snapshot.stomp_connect_count,
        snapshot.frame_count,
        HUB_CLOUD_WS_FRAME_CAPACITY,
        HUB_CLOUD_WS_COMPONENT_BUFFER_BYTES,
        snapshot.dashboard_frame_count,
        snapshot.scheduling_frame_count,
        snapshot.receipt_count,
        snapshot.command_success_count,
        snapshot.command_failure_count,
        snapshot.parse_error_count,
        snapshot.parser_drop_count,
        snapshot.oversized_drop_count,
        snapshot.wrong_target_drop_count,
        snapshot.last_frame_bytes,
        snapshot.max_frame_bytes,
        event_age_ms,
        snapshot.observation.valid ? "true" : "false",
        observation_age_ms,
        snapshot.observation.widget_mask,
        snapshot.observation.body_hash,
        snapshot.task_stack_high_water_bytes,
        snapshot.supervisor_stack_high_water_bytes,
        snapshot.last_error_code,
        snapshot.last_error_type,
        snapshot.last_handshake_status,
        snapshot.last_transport_errno,
        snapshot.last_esp_error,
        (unsigned long)resources.psram_free_bytes,
        (unsigned long)resources.internal_free_bytes,
        (unsigned long)resources.internal_largest_block_bytes
    );
    return runtime.diagnostics_body;
}

const char *hub_cloud_websocket_state_json(void)
{
    return runtime.state_body != NULL
        ? runtime.state_body
        : "{\"valid\":false,\"mergeActive\":false}";
}

bool hub_cloud_websocket_merge_active(void)
{
    return false;
}
