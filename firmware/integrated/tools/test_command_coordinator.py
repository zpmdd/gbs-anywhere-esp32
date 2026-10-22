#!/usr/bin/env python3
"""Compile and run host tests for the bounded command coordinator."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"


ESP_ERR_STUB = r"""
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_RESPONSE 0x105
#define ESP_ERR_TIMEOUT 0x107
"""


FREERTOS_STUB = r"""
#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
"""


QUEUE_STUB = r"""
#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t queue);
"""


SEMPHR_STUB = r"""
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
"""


TASK_STUB = r"""
#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreate(
    void (*task)(void *),
    const char *name,
    uint32_t stack_depth,
    void *parameter,
    UBaseType_t priority,
    TaskHandle_t *handle
);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
"""


TIMER_STUB = r"""
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
"""


RANDOM_STUB = r"""
#pragma once
#include <stdint.h>
uint32_t esp_random(void);
"""


LOG_STUB = r"""
#pragma once
#define ESP_LOGI(tag, format, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, format, ...) do { (void)(tag); } while (0)
"""


HEAP_CAPS_STUB = r"""
#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1U << 0)
#define MALLOC_CAP_8BIT (1U << 1)
static inline void *heap_caps_calloc(size_t count, size_t size, unsigned int caps)
{
    (void)caps;
    return calloc(count, size);
}
"""


HARNESS = r"""
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub_command_coordinator.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t length;
    size_t item_size;
    size_t count;
    size_t head;
    size_t tail;
    unsigned char *items;
} fake_queue_t;

static int64_t fake_now_ms;
static uint32_t fake_random_value = 1;
static int executor_calls;
static unsigned recipe_cancels;
static bool recipe_permitted = true, observed_permission, observed_online_required;
bool gbs_recipe_follow_generation_can_write(uint32_t generation) { return generation == 7 && recipe_permitted; }
void gbs_recipe_follow_cancel(void) { ++recipe_cancels; recipe_permitted = false; }
static int64_t executor_advance_ms;
static bool executor_unknown;
static hub_command_kind_t executed_kinds[HUB_COMMAND_QUEUE_CAPACITY];
static hub_machine_selection_identity_t fake_identity = {
    .serial_number = "MACHINE-A",
    .generation = 1,
};

esp_err_t hub_config_get_machine_selection_identity(hub_machine_selection_identity_t *identity)
{
    if (identity == NULL || fake_identity.serial_number[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    *identity = fake_identity;
    return ESP_OK;
}

bool hub_config_machine_selection_matches(const hub_machine_selection_identity_t *expected)
{
    return expected != NULL
        && expected->generation == fake_identity.generation
        && strcmp(expected->serial_number, fake_identity.serial_number) == 0;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    fake_queue_t *queue = calloc(1, sizeof(*queue));
    if (queue == NULL) {
        return NULL;
    }
    queue->items = calloc(length, item_size);
    if (queue->items == NULL) {
        free(queue);
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t ticks)
{
    (void)ticks;
    fake_queue_t *queue = handle;
    if (queue == NULL || queue->count >= queue->length) {
        return pdFALSE;
    }
    memcpy(queue->items + queue->tail * queue->item_size, item, queue->item_size);
    queue->tail = (queue->tail + 1) % queue->length;
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t ticks)
{
    (void)ticks;
    fake_queue_t *queue = handle;
    if (queue == NULL || queue->count == 0) {
        return pdFALSE;
    }
    memcpy(item, queue->items + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    queue->count--;
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t handle)
{
    fake_queue_t *queue = handle;
    if (queue == NULL) {
        return pdFALSE;
    }
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
    return pdTRUE;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)semaphore;
    (void)ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
}

BaseType_t xTaskCreate(
    void (*task)(void *),
    const char *name,
    uint32_t stack_depth,
    void *parameter,
    UBaseType_t priority,
    TaskHandle_t *handle
)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)parameter;
    (void)priority;
    if (handle != NULL) {
        *handle = (TaskHandle_t)1;
    }
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks)
{
    (void)clear_on_exit;
    (void)ticks;
    return 1;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

int64_t esp_timer_get_time(void)
{
    return fake_now_ms * 1000;
}

uint32_t esp_random(void)
{
    return fake_random_value++;
}

esp_err_t hub_control_execute_request(
    const struct hub_command_request *request,
    hub_control_result_t *result
)
{
    if (executor_calls < HUB_COMMAND_QUEUE_CAPACITY) {
        executed_kinds[executor_calls] = request->kind;
    }
    observed_permission = hub_command_side_effect_allowed();
    observed_online_required = hub_command_requires_online_machine();
    executor_calls++;
    fake_now_ms += executor_advance_ms;
    memset(result, 0, sizeof(*result));
    result->ok = true;
    result->upstream_attempts = 1;
    snprintf(result->control, sizeof(result->control), "%s",
             request->kind == HUB_COMMAND_POWER ? "power" : "brewByWeightDose");
    snprintf(result->message, sizeof(result->message), "completed");
    if (executor_unknown) {
        result->ok = false; result->late_side_effect_risk = true;
        snprintf(result->error_code, sizeof(result->error_code), "unknownOutcome");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void hub_control_result_json(const hub_control_result_t *result, char *buffer, size_t buffer_len)
{
    snprintf(
        buffer,
        buffer_len,
        "{\"requestId\":\"%s\",\"pending\":%s}",
        result->request_id,
        result->pending ? "true" : "false"
    );
}

const char *hub_control_transport_string(hub_control_transport_t transport)
{
    (void)transport;
    return "none";
}

typedef struct {
    int calls;
    esp_err_t error;
    hub_control_result_t result;
} completion_capture_t;

static void capture_completion(
    const char *request_id,
    esp_err_t error,
    const hub_control_result_t *result,
    bool deduplicated,
    void *context
)
{
    completion_capture_t *capture = context;
    capture->calls++;
    capture->error = error;
    capture->result = *result;
    if (strcmp(request_id, result->request_id) != 0 || deduplicated) {
        fprintf(stderr, "completion metadata mismatch\n");
        exit(1);
    }
}

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static hub_command_request_t request(const char *request_id, hub_command_kind_t kind)
{
    hub_command_request_t output = {0};
    snprintf(output.request_id, sizeof(output.request_id), "%s", request_id);
    output.kind = kind;
    if (kind == HUB_COMMAND_QUICK_WEIGHT) {
        output.value.quick_weight.grams = 30;
    } else if (kind == HUB_COMMAND_BBW_DOSE) {
        snprintf(output.value.bbw_dose.dose, sizeof(output.value.bbw_dose.dose), "dose1");
        output.value.bbw_dose.grams = 30;
    } else if (kind == HUB_COMMAND_BBW_MODE) {
        snprintf(output.value.bbw_mode.mode, sizeof(output.value.bbw_mode.mode), "Continuous");
    } else if (kind == HUB_COMMAND_PREBREW) {
        snprintf(output.value.prebrew.field, sizeof(output.value.prebrew.field), "mode");
        snprintf(output.value.prebrew.value, sizeof(output.value.prebrew.value), "PreBrewing");
    } else if (kind == HUB_COMMAND_COFFEE_TEMPERATURE
        || kind == HUB_COMMAND_STEAM_TEMPERATURE) {
        output.value.temperature.temperature = 93;
    } else if (kind == HUB_COMMAND_STEAM_LEVEL) {
        output.value.steam_level.level = 2;
    } else if (kind == HUB_COMMAND_STEAM_ENABLED) {
        output.value.enabled.enabled = true;
    } else if (kind == HUB_COMMAND_POWER) {
        output.value.power.enabled = true;
        output.value.power.confirm = true;
    }
    return output;
}

static hub_command_request_t v2_request(
    const char *request_id,
    hub_command_kind_t kind,
    uint32_t ttl_ms
)
{
    hub_command_request_t output = request(request_id, kind);
    output.deadline.enforced = true;
    output.deadline.client_ttl_present = true;
    output.deadline.client_ttl_ms = ttl_ms;
    output.client_surface = HUB_API_V2_SURFACE_DIAGNOSTIC;
    return output;
}

static hub_command_submit_status_t submit(
    const hub_command_request_t *command,
    completion_capture_t *capture,
    hub_command_submission_t *submission
)
{
    return hub_command_submit(command, capture_completion, capture, submission);
}

static void reset(void)
{
    hub_command_coordinator_reset();
    executor_calls = 0; recipe_cancels = 0; recipe_permitted = true;
    executor_advance_ms = 0;
    executor_unknown = false;
    memset(executed_kinds, 0, sizeof(executed_kinds));
    fake_now_ms = 0;
    snprintf(fake_identity.serial_number, sizeof(fake_identity.serial_number), "MACHINE-A");
    fake_identity.generation = 1;
}

static void test_submit_returns_without_running_executor_inline(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000001", HUB_COMMAND_BBW_DOSE);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "submit not accepted");
    require_true(executor_calls == 0 && capture.calls == 0, "executor ran inline on HTTP submit path");
    require_true(hub_command_coordinator_process_next_for_test(), "queued command was not processed");
    require_true(executor_calls == 1 && capture.calls == 1, "worker did not complete command");
}

static void test_same_completed_request_id_returns_cached_result(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000002", HUB_COMMAND_BBW_DOSE);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "submit not accepted");
    require_true(hub_command_coordinator_process_next_for_test(), "command not processed");
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_DUPLICATE_COMPLETE,
                 "completed duplicate was not served from cache");
    require_true(executor_calls == 1, "completed duplicate executed twice");
    require_true(submission.result.ok && submission.result.deduplicated,
                 "completed duplicate did not return cached result metadata");
}

static void test_same_inflight_request_id_does_not_execute_twice(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000003", HUB_COMMAND_BBW_DOSE);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "submit not accepted");
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_DUPLICATE_INFLIGHT,
                 "in-flight duplicate was not detected");
    require_true(submission.result.pending, "in-flight duplicate did not report pending");
    require_true(hub_command_coordinator_process_next_for_test(), "command not processed");
    require_true(executor_calls == 1, "in-flight duplicate executed twice");
}

static void test_queue_depth_is_bounded_at_four(void)
{
    reset();
    completion_capture_t captures[5] = {0};
    hub_command_submission_t submission = {0};
    const char *ids[] = {
        "00000000-0000-4000-8000-000000000010",
        "00000000-0000-4000-8000-000000000011",
        "00000000-0000-4000-8000-000000000012",
        "00000000-0000-4000-8000-000000000013",
        "00000000-0000-4000-8000-000000000014",
    };
    for (int index = 0; index < 4; index++) {
        hub_command_request_t command = request(ids[index], HUB_COMMAND_BBW_DOSE);
        require_true(submit(&command, &captures[index], &submission) == HUB_COMMAND_ACCEPTED,
                     "queue rejected one of first four commands");
    }
    hub_command_request_t fifth = request(ids[4], HUB_COMMAND_BBW_DOSE);
    require_true(submit(&fifth, &captures[4], &submission) == HUB_COMMAND_QUEUE_FULL,
                 "queue accepted a fifth outstanding command");
    require_true(executor_calls == 0, "bounded queue executed inline");
}

static void test_daily_interactive_controls_jump_ahead_of_queued_normal_commands(void)
{
    const hub_command_kind_t interactive_kinds[] = {
        HUB_COMMAND_QUICK_WEIGHT,
        HUB_COMMAND_BBW_DOSE,
        HUB_COMMAND_BBW_MODE,
        HUB_COMMAND_COFFEE_TEMPERATURE,
        HUB_COMMAND_STEAM_LEVEL,
        HUB_COMMAND_STEAM_ENABLED,
        HUB_COMMAND_STEAM_TEMPERATURE,
    };
    for (size_t index = 0; index < sizeof(interactive_kinds) / sizeof(interactive_kinds[0]); index++) {
        reset();
        completion_capture_t low_capture = {0};
        completion_capture_t priority_capture = {0};
        hub_command_submission_t submission = {0};
        char low_id[HUB_COMMAND_REQUEST_ID_CAPACITY] = {0};
        char priority_id[HUB_COMMAND_REQUEST_ID_CAPACITY] = {0};
        snprintf(low_id, sizeof(low_id), "00000000-0000-4000-8000-%012lu", (unsigned long)(100 + index));
        snprintf(priority_id, sizeof(priority_id), "00000000-0000-4000-8000-%012lu", (unsigned long)(200 + index));
        hub_command_request_t low = request(low_id, HUB_COMMAND_POWER);
        hub_command_request_t priority = request(priority_id, interactive_kinds[index]);

        require_true(submit(&low, &low_capture, &submission) == HUB_COMMAND_ACCEPTED,
                     "normal command was not accepted before priority test");
        require_true(submit(&priority, &priority_capture, &submission) == HUB_COMMAND_ACCEPTED,
                     "daily interactive command was not accepted");
        require_true(hub_command_coordinator_process_next_for_test(),
                     "daily interactive command was not processed");
        require_true(executed_kinds[0] == interactive_kinds[index],
                     "daily interactive command did not jump ahead of queued normal work");
        require_true(hub_command_coordinator_process_next_for_test(),
                     "queued normal command was not processed after priority work");
        require_true(executed_kinds[1] == HUB_COMMAND_POWER,
                     "normal command order was corrupted after priority work");
    }
}

static void test_normal_queue_reserves_one_outstanding_slot_for_daily_controls(void)
{
    reset();
    completion_capture_t captures[5] = {0};
    hub_command_submission_t submission = {0};
    const char *ids[] = {
        "00000000-0000-4000-8000-000000000110",
        "00000000-0000-4000-8000-000000000111",
        "00000000-0000-4000-8000-000000000112",
        "00000000-0000-4000-8000-000000000113",
        "00000000-0000-4000-8000-000000000114",
    };
    for (int index = 0; index < HUB_COMMAND_NORMAL_QUEUE_CAPACITY; index++) {
        hub_command_request_t normal = request(ids[index], HUB_COMMAND_POWER);
        require_true(submit(&normal, &captures[index], &submission) == HUB_COMMAND_ACCEPTED,
                     "normal queue rejected an available non-reserved slot");
    }
    hub_command_request_t extra_normal = request(ids[3], HUB_COMMAND_POWER);
    require_true(submit(&extra_normal, &captures[3], &submission) == HUB_COMMAND_QUEUE_FULL,
                 "normal work consumed the reserved daily-control slot");

    hub_command_request_t priority = request(ids[4], HUB_COMMAND_COFFEE_TEMPERATURE);
    require_true(submit(&priority, &captures[4], &submission) == HUB_COMMAND_ACCEPTED,
                 "reserved slot did not accept a daily interactive control");
    require_true(hub_command_coordinator_process_next_for_test(),
                 "reserved daily interactive control was not processed");
    require_true(executed_kinds[0] == HUB_COMMAND_COFFEE_TEMPERATURE,
                 "reserved daily interactive control did not execute first");
}

static void test_normal_work_cannot_consume_any_interactive_capacity(void)
{
    reset();
    completion_capture_t captures[5] = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t first_priority = request(
        "00000000-0000-4000-8000-000000000115",
        HUB_COMMAND_COFFEE_TEMPERATURE
    );
    require_true(submit(&first_priority, &captures[0], &submission) == HUB_COMMAND_ACCEPTED,
                 "first daily interactive command was not accepted");

    for (int index = 0; index < HUB_COMMAND_NORMAL_QUEUE_CAPACITY; index++) {
        char request_id[HUB_COMMAND_REQUEST_ID_CAPACITY] = {0};
        snprintf(request_id, sizeof(request_id), "00000000-0000-4000-8000-%012d", 116 + index);
        hub_command_request_t normal = request(request_id, HUB_COMMAND_POWER);
        require_true(submit(&normal, &captures[index + 1], &submission) == HUB_COMMAND_ACCEPTED,
                     "normal command was not accepted within its own capacity");
    }

    hub_command_request_t second_priority = request(
        "00000000-0000-4000-8000-000000000119",
        HUB_COMMAND_QUICK_WEIGHT
    );
    require_true(submit(&second_priority, &captures[4], &submission) == HUB_COMMAND_ACCEPTED,
                 "normal work consumed capacity reserved exclusively for daily controls");
    require_true(hub_command_coordinator_process_next_for_test(),
                 "first daily control was not processed");
    require_true(hub_command_coordinator_process_next_for_test(),
                 "second daily control was not processed");
    require_true(executed_kinds[0] == HUB_COMMAND_COFFEE_TEMPERATURE
                     && executed_kinds[1] == HUB_COMMAND_QUICK_WEIGHT,
                 "daily controls lost FIFO priority when normal capacity was full");
}

static void test_daily_interactive_controls_keep_fifo_and_pending_count(void)
{
    reset();
    completion_capture_t captures[2] = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t coffee = request(
        "00000000-0000-4000-8000-000000000120",
        HUB_COMMAND_COFFEE_TEMPERATURE
    );
    hub_command_request_t steam = request(
        "00000000-0000-4000-8000-000000000121",
        HUB_COMMAND_STEAM_LEVEL
    );
    require_true(submit(&coffee, &captures[0], &submission) == HUB_COMMAND_ACCEPTED,
                 "first daily interactive command was not accepted");
    require_true(submit(&steam, &captures[1], &submission) == HUB_COMMAND_ACCEPTED,
                 "second daily interactive command was not accepted");
    require_true(hub_command_interactive_pending(),
                 "daily interactive pending signal was not raised at admission");

    require_true(hub_command_coordinator_process_next_for_test(),
                 "first daily interactive command was not processed");
    require_true(executed_kinds[0] == HUB_COMMAND_COFFEE_TEMPERATURE,
                 "daily interactive FIFO reversed the first command");
    require_true(hub_command_interactive_pending(),
                 "pending signal cleared while another daily command remained");
    require_true(hub_command_coordinator_process_next_for_test(),
                 "second daily interactive command was not processed");
    require_true(executed_kinds[1] == HUB_COMMAND_STEAM_LEVEL,
                 "daily interactive FIFO reversed the second command");
    require_true(!hub_command_interactive_pending(),
                 "pending signal leaked after all daily commands completed");
}

static void test_daily_interactive_burst_survives_cloud_queue_delay(void)
{
    reset();
    completion_capture_t captures[3] = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t commands[] = {
        request("00000000-0000-4000-8000-000000000122", HUB_COMMAND_QUICK_WEIGHT),
        request("00000000-0000-4000-8000-000000000123", HUB_COMMAND_COFFEE_TEMPERATURE),
        request("00000000-0000-4000-8000-000000000124", HUB_COMMAND_STEAM_LEVEL),
    };
    for (size_t index = 0; index < 3; index++) {
        require_true(submit(&commands[index], &captures[index], &submission) == HUB_COMMAND_ACCEPTED,
                     "daily interactive burst command was not accepted");
    }

    fake_now_ms = HUB_COMMAND_START_DEADLINE_MS + 1000;
    for (size_t index = 0; index < 3; index++) {
        require_true(hub_command_coordinator_process_next_for_test(),
                     "daily interactive burst command was not processed");
        require_true(captures[index].error == ESP_OK,
                     "daily interactive burst expired at a realistic Cloud queue delay");
    }
    require_true(executor_calls == 3, "daily interactive burst did not execute all commands");
}

static void test_entries_expire_after_ten_minutes(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000020", HUB_COMMAND_BBW_DOSE);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "submit not accepted");
    require_true(hub_command_coordinator_process_next_for_test(), "command not processed");
    fake_now_ms = 600001;
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "expired request ID remained deduplicated");
    require_true(hub_command_coordinator_process_next_for_test(), "expired ID command not reprocessed");
    require_true(executor_calls == 2, "expired request ID did not execute again");
}

static void test_high_risk_command_has_one_attempt(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000030", HUB_COMMAND_POWER);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "power submit not accepted");
    require_true(hub_command_coordinator_process_next_for_test(), "power command not processed");
    require_true(executor_calls == 1, "high-risk command executor ran more than once");
    require_true(capture.result.upstream_attempts == 1, "high-risk command reported multiple attempts");
}

static void test_same_inflight_id_with_different_payload_is_conflict(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t first = request("00000000-0000-4000-8000-000000000040", HUB_COMMAND_BBW_DOSE);
    hub_command_request_t changed = first;
    changed.value.bbw_dose.grams = 31;
    require_true(submit(&first, &capture, &submission) == HUB_COMMAND_ACCEPTED, "first submit not accepted");
    require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "different in-flight payload reused a request ID");
    require_true(strcmp(submission.result.error_code, "requestIdPayloadConflict") == 0,
                 "payload conflict did not expose an error code");
    require_true(executor_calls == 0, "conflicting payload executed");
}

static void test_same_completed_id_with_different_kind_is_conflict(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t first = request("00000000-0000-4000-8000-000000000041", HUB_COMMAND_BBW_DOSE);
    hub_command_request_t changed = request("00000000-0000-4000-8000-000000000041", HUB_COMMAND_POWER);
    require_true(submit(&first, &capture, &submission) == HUB_COMMAND_ACCEPTED, "first submit not accepted");
    require_true(hub_command_coordinator_process_next_for_test(), "first command not processed");
    require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "different completed payload reused a request ID");
    require_true(executor_calls == 1, "completed conflict executed");
}

static void test_same_inflight_id_for_different_target_is_conflict(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000044", HUB_COMMAND_BBW_DOSE);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "first submit not accepted");
    snprintf(fake_identity.serial_number, sizeof(fake_identity.serial_number), "MACHINE-B");
    fake_identity.generation = 2;
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "different target reused an in-flight request ID");
    require_true(strcmp(submission.result.error_code, "requestIdTargetConflict") == 0,
                 "target conflict did not expose an error code");
    require_true(executor_calls == 0, "target conflict executed");
}

static void test_missing_machine_selection_fails_closed(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000045", HUB_COMMAND_BBW_DOSE);
    fake_identity.serial_number[0] = '\0';
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "command without a selected machine did not fail closed");
    require_true(strcmp(submission.result.error_code, "machineNotSelected") == 0,
                 "missing selection did not expose an error code");
    require_true(submission.result.response_http_status == 409,
                 "missing selection did not map to HTTP 409");
    require_true(executor_calls == 0, "command without a target executed");
}

static void test_explicit_machine_target_mismatch_fails_closed(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000046", HUB_COMMAND_BBW_DOSE);
    snprintf(
        command.expected_serial_number,
        sizeof(command.expected_serial_number),
        "MACHINE-B"
    );
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "command for a different explicit machine target did not fail closed");
    require_true(strcmp(submission.result.error_code, "machineTargetMismatch") == 0,
                 "explicit target mismatch did not expose an error code");
    require_true(submission.result.response_http_status == 409,
                 "explicit target mismatch did not map to HTTP 409");
    require_true(executor_calls == 0, "mismatched explicit target executed");
}

static void test_matching_explicit_machine_target_is_accepted(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000047", HUB_COMMAND_BBW_DOSE);
    snprintf(
        command.expected_serial_number,
        sizeof(command.expected_serial_number),
        "MACHINE-A"
    );
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "matching explicit machine target was rejected");
    require_true(hub_command_coordinator_process_next_for_test(),
                 "matching explicit target command was not processed");
    require_true(executor_calls == 1, "matching explicit target did not execute once");
}

static void test_selection_change_rejects_before_executor(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000042", HUB_COMMAND_BBW_DOSE);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "submit not accepted");
    snprintf(fake_identity.serial_number, sizeof(fake_identity.serial_number), "MACHINE-B");
    fake_identity.generation = 2;
    require_true(hub_command_coordinator_process_next_for_test(), "changed-target command not processed");
    require_true(executor_calls == 0, "changed-target command reached executor");
    require_true(capture.error == ESP_ERR_INVALID_STATE, "changed target did not fail");
    require_true(strcmp(capture.result.error_code, "machineSelectionChanged") == 0,
                 "changed target did not expose an error code");
    require_true(capture.result.upstream_attempts == 0, "changed target reported an upstream attempt");
}

static void test_command_expires_before_executor(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000043", HUB_COMMAND_BBW_DOSE);
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "submit not accepted");
    fake_now_ms = HUB_COMMAND_INTERACTIVE_START_DEADLINE_MS;
    require_true(hub_command_coordinator_process_next_for_test(), "expired command not processed");
    require_true(executor_calls == 0, "expired command reached executor");
    require_true(capture.error == ESP_ERR_TIMEOUT, "expired command did not timeout");
    require_true(strcmp(capture.result.error_code, "commandExpired") == 0,
                 "expired command did not expose an error code");
}

static void test_v2_invalid_ttl_fails_before_queueing(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = v2_request(
        "00000000-0000-4000-8000-000000000201",
        HUB_COMMAND_QUICK_WEIGHT,
        0
    );
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_INVALID,
                 "v2 zero TTL was accepted");
    require_true(strcmp(submission.result.error_code, "invalidTtl") == 0,
                 "v2 invalid TTL did not expose invalidTtl");
    require_true(submission.result.response_http_status == 400,
                 "v2 invalid TTL did not map to HTTP 400");
    require_true(executor_calls == 0 && submission.result.upstream_attempts == 0,
                 "v2 invalid TTL reached an upstream executor");
}

static void test_v2_capacity_rejects_before_side_effect(void)
{
    reset();
    completion_capture_t legacy_capture = {0};
    completion_capture_t v2_capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t legacy = request(
        "00000000-0000-4000-8000-000000000202",
        HUB_COMMAND_BBW_DOSE
    );
    require_true(submit(&legacy, &legacy_capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "legacy setup command was not queued");
    hub_command_request_t command = v2_request(
        "00000000-0000-4000-8000-000000000203",
        HUB_COMMAND_QUICK_WEIGHT,
        8000
    );
    require_true(submit(&command, &v2_capture, &submission)
                     == HUB_COMMAND_DEADLINE_CAPACITY,
                 "v2 command was accepted without enough serialized capacity");
    require_true(strcmp(submission.result.error_code, "commandDeadlineCapacity") == 0,
                 "capacity rejection did not expose commandDeadlineCapacity");
    require_true(submission.result.response_http_status == 503,
                 "capacity rejection did not map to HTTP 503");
    require_true(executor_calls == 0 && submission.result.upstream_attempts == 0,
                 "capacity rejection started an upstream side effect");
}

static void test_v2_worker_reserve_expires_before_executor(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = v2_request(
        "00000000-0000-4000-8000-000000000204",
        HUB_COMMAND_QUICK_WEIGHT,
        8000
    );
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "v2 quick weight was not accepted");
    fake_now_ms = 2000;
    require_true(hub_command_coordinator_process_next_for_test(),
                 "v2 expired command was not terminalized");
    require_true(executor_calls == 0, "v2 expired command reached executor");
    require_true(capture.error == ESP_ERR_TIMEOUT,
                 "v2 expired command did not return timeout");
    require_true(strcmp(capture.result.error_code, "commandExpired") == 0,
                 "v2 expired command did not expose commandExpired");
    require_true(capture.result.api_version == 2
                     && capture.result.v2_status == HUB_API_V2_COMMAND_STATUS_COMPLETED,
                 "v2 expired command lost terminal metadata");
}

static void test_v2_duplicate_does_not_extend_original_deadline(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t original = v2_request(
        "00000000-0000-4000-8000-000000000205",
        HUB_COMMAND_QUICK_WEIGHT,
        7000
    );
    require_true(submit(&original, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "v2 original request was not accepted");
    require_true(submission.result.accepted_ttl_ms == 7000,
                 "v2 original accepted TTL changed");
    fake_now_ms = 250;
    hub_command_request_t duplicate = v2_request(
        "00000000-0000-4000-8000-000000000205",
        HUB_COMMAND_QUICK_WEIGHT,
        8000
    );
    require_true(submit(&duplicate, &capture, &submission)
                     == HUB_COMMAND_DUPLICATE_INFLIGHT,
                 "v2 duplicate was not deduplicated");
    require_true(submission.result.accepted_ttl_ms == 7000,
                 "v2 duplicate extended accepted TTL");
    require_true(submission.result.remaining_ms == 6750,
                 "v2 duplicate did not retain original absolute deadline");
    require_true(executor_calls == 0, "v2 duplicate executed inline");
}

static void test_v2_late_completion_becomes_unknown_outcome(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t command = v2_request(
        "00000000-0000-4000-8000-000000000206",
        HUB_COMMAND_PREBREW,
        8000
    );
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "v2 prebrew was not accepted");
    executor_advance_ms = 8000;
    require_true(hub_command_coordinator_process_next_for_test(),
                 "late v2 command was not processed");
    require_true(executor_calls == 1, "late v2 command did not execute exactly once");
    require_true(capture.error == ESP_ERR_TIMEOUT && !capture.result.ok,
                 "late v2 command reported success");
    require_true(capture.result.v2_status == HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME,
                 "late v2 command did not become unknownOutcome");
    require_true(strcmp(capture.result.error_code, "unknownOutcome") == 0,
                 "late v2 command lost unknownOutcome error code");
}

static void test_v2_prebrew_cannot_be_displaced_after_acceptance(void)
{
    reset();
    completion_capture_t prebrew_capture = {0};
    completion_capture_t later_capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t prebrew = v2_request(
        "00000000-0000-4000-8000-000000000207",
        HUB_COMMAND_PREBREW,
        8000
    );
    hub_command_request_t later_quick_weight = request(
        "00000000-0000-4000-8000-000000000208",
        HUB_COMMAND_QUICK_WEIGHT
    );
    require_true(submit(&prebrew, &prebrew_capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "v2 prebrew was not accepted");
    require_true(submit(&later_quick_weight, &later_capture, &submission)
                     == HUB_COMMAND_ACCEPTED,
                 "later legacy quick weight was not accepted");
    require_true(hub_command_coordinator_process_next_for_test(),
                 "v2 prebrew was not processed");
    require_true(executed_kinds[0] == HUB_COMMAND_PREBREW,
                 "later priority work displaced accepted v2 prebrew");
}

static void test_mode_preset_guard_is_part_of_request_identity(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t original = request(
        "00000000-0000-4000-8000-000000000209", HUB_COMMAND_BBW_MODE);
    snprintf(original.value.bbw_mode.mode, sizeof(original.value.bbw_mode.mode), "Dose1");
    original.value.bbw_mode.has_preset_grams = true;
    original.value.bbw_mode.preset_grams = 20;
    require_true(submit(&original, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "guarded mode was not accepted");
    require_true(submit(&original, &capture, &submission) == HUB_COMMAND_DUPLICATE_INFLIGHT,
                 "same guarded mode was not deduplicated");
    hub_command_request_t changed = original;
    changed.value.bbw_mode.preset_grams = 30;
    require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "changing preset grams reused an inflight mode request");
    changed = original;
    changed.value.bbw_mode.has_preset_grams = false;
    require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "removing preset validation reused an inflight mode request");
    require_true(hub_command_coordinator_process_next_for_test(), "guarded mode was not processed");
    require_true(submit(&original, &capture, &submission) == HUB_COMMAND_DUPLICATE_COMPLETE,
                 "completed guarded mode was not deduplicated");
    require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_CONFLICT,
                 "removing validation reused a completed mode request");
    require_true(executor_calls == 1, "guarded duplicate mode executed again");

    reset();
    original.value.bbw_mode.has_preset_grams = false;
    require_true(submit(&original, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "legacy mode was not accepted");
    changed = original;
    changed.value.bbw_mode.preset_grams = 199;
    require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_DUPLICATE_INFLIGHT,
                 "inactive preset storage changed legacy mode identity");
}

static void test_quick_weight_preset_guard_is_part_of_request_identity(void)
{
    for (int guarded = 0; guarded < 2; guarded++) {
        reset();
        completion_capture_t capture = {0};
        hub_command_submission_t submission = {0};
        hub_command_request_t original = request(
            "00000000-0000-4000-8000-000000000210", HUB_COMMAND_QUICK_WEIGHT);
        original.value.quick_weight.require_no_stored_match = guarded != 0;
        require_true(submit(&original, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                     "quick weight was not accepted");
        require_true(submit(&original, &capture, &submission) == HUB_COMMAND_DUPLICATE_INFLIGHT,
                     "same quick-weight guard was not deduplicated");
        hub_command_request_t changed = original;
        changed.value.quick_weight.require_no_stored_match = !original.value.quick_weight.require_no_stored_match;
        require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_CONFLICT,
                     "changing the unmatched guard reused an inflight quick weight");
        require_true(hub_command_coordinator_process_next_for_test(), "quick weight was not processed");
        require_true(submit(&changed, &capture, &submission) == HUB_COMMAND_CONFLICT,
                     "changing the unmatched guard reused a completed quick weight");
        require_true(executor_calls == 1, "changed quick-weight guard executed again");
    }
}

static void test_only_guarded_mode_accepts_v2_deadline(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t mode = v2_request(
        "00000000-0000-4000-8000-000000000211", HUB_COMMAND_BBW_MODE, 8000);
    snprintf(mode.value.bbw_mode.mode, sizeof(mode.value.bbw_mode.mode), "Dose2");
    require_true(submit(&mode, &capture, &submission) == HUB_COMMAND_INVALID,
                 "ordinary mode unexpectedly gained a v2 deadline");
    require_true(strcmp(submission.result.error_code, "unsupportedDeadlineKind") == 0,
                 "ordinary mode lost the existing deadline rejection");
    mode.value.bbw_mode.has_preset_grams = true;
    mode.value.bbw_mode.preset_grams = 30;
    require_true(submit(&mode, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                 "guarded mode did not accept a bounded deadline");
    require_true(submission.result.accepted_ttl_ms == 8000,
                 "guarded mode did not preserve its TTL");
    require_true(hub_command_coordinator_process_next_for_test(), "guarded mode was not processed");
    require_true(executor_calls == 1 && capture.result.api_version == 2,
                 "guarded mode lost its executor or deadline metadata");
}

static void test_schedule_identity_and_deduplication(void)
{
    reset();
    completion_capture_t capture = {0};
    hub_command_submission_t submission = {0};
    hub_command_request_t original = request(
        "00000000-0000-4000-8000-000000000212", HUB_COMMAND_SCHEDULE);
    original.value.schedule.expected = (hub_schedule_entry_t){
        .id="plan-a", .on_minutes=420, .off_minutes=570, .days=31, .enabled=true, .steam=true};
    original.value.schedule.requested = original.value.schedule.expected;
    original.value.schedule.requested.on_minutes=480;
    original.value.schedule.confirm=true;
    require_true(submit(&original,&capture,&submission)==HUB_COMMAND_ACCEPTED,"schedule was not accepted");
    require_true(submit(&original,&capture,&submission)==HUB_COMMAND_DUPLICATE_INFLIGHT,"schedule duplicate was not coalesced");
    for(unsigned i=0;i<13;++i) {
        hub_command_request_t changed=original;
        hub_schedule_entry_t *plan=i<6?&changed.value.schedule.expected:&changed.value.schedule.requested;
        switch(i%6) {
        case 0: plan->id[0]='b'; break;
        case 1: ++plan->on_minutes; break;
        case 2: ++plan->off_minutes; break;
        case 3: plan->days=63; break;
        case 4: plan->enabled=false; break;
        case 5: plan->steam=false; break;
        }
        if(i==12) { changed=original; changed.value.schedule.confirm=false; }
        require_true(submit(&changed,&capture,&submission)==HUB_COMMAND_CONFLICT,"changed schedule reused request identity");
    }
    require_true(hub_command_coordinator_process_next_for_test(),"schedule was not processed");
    require_true(submit(&original,&capture,&submission)==HUB_COMMAND_DUPLICATE_COMPLETE,"completed schedule was not deduplicated");
    require_true(executor_calls==1,"duplicate schedule executed twice");
}

static void test_v2_status_without_deadline(void)
{
    const hub_command_kind_t kinds[] = {HUB_COMMAND_SCHEDULE, HUB_COMMAND_BBW_DOSE,
        HUB_COMMAND_BBW_MODE, HUB_COMMAND_COFFEE_TEMPERATURE, HUB_COMMAND_STEAM_LEVEL,
        HUB_COMMAND_STEAM_ENABLED, HUB_COMMAND_STEAM_TEMPERATURE, HUB_COMMAND_SMART_STANDBY,
        HUB_COMMAND_POWER, HUB_COMMAND_BACKFLUSH, HUB_COMMAND_MACHINE_FIRMWARE_UPDATE};
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        reset();
        completion_capture_t capture = {0}; hub_command_submission_t submission = {0};
        hub_command_request_t command = request("00000000-0000-4000-8000-000000000213", kinds[i]);
        command.client_surface = HUB_API_V2_SURFACE_DIAGNOSTIC;
        require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED,
                     "v2 non-deadline command rejected");
        require_true(submission.result.api_version == 2 && submission.result.v2_status == HUB_API_V2_COMMAND_STATUS_ACCEPTED
                     && submission.result.accepted_ttl_ms == 0, "v2 acceptance missing or invented a deadline");
        hub_control_result_t pending = {0};
        require_true(hub_command_lookup(command.request_id, NULL, &pending) == HUB_COMMAND_LOOKUP_INFLIGHT
                     && pending.api_version == 2, "v2 pending lookup lost version");
        require_true(hub_command_coordinator_process_next_for_test(), "v2 command did not execute");
        require_true(capture.result.api_version == 2 && capture.result.v2_status == HUB_API_V2_COMMAND_STATUS_COMPLETED,
                     "v2 non-deadline completion lost status");
        require_true(submit(&command, &capture, &submission) == HUB_COMMAND_DUPLICATE_COMPLETE
                     && submission.result.api_version == 2 && executor_calls == 1, "v2 deduplication lost metadata");
    }
    reset();
    completion_capture_t capture = {0}; hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000214", HUB_COMMAND_SCHEDULE);
    command.client_surface = HUB_API_V2_SURFACE_DIAGNOSTIC; executor_unknown = true;
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "unknown case rejected");
    require_true(hub_command_coordinator_process_next_for_test(), "unknown case not executed");
    require_true(capture.result.api_version == 2 && capture.result.v2_status == HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME,
                 "v2 unknown result became completed");
    reset(); command.client_surface = HUB_API_V2_SURFACE_UNKNOWN;
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED && submission.result.api_version == 0,
                 "legacy acceptance changed wire version");
    require_true(hub_command_coordinator_process_next_for_test() && capture.result.api_version == 0,
                 "legacy completion changed wire version");
}

static void test_recipe_guard_and_manual_override(void)
{
    reset();
    completion_capture_t capture = {0}; hub_command_submission_t submission = {0};
    hub_command_request_t command = request("00000000-0000-4000-8000-000000000301", HUB_COMMAND_BBW_DOSE);
    command.grinder_recipe_generation = 7;
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED, "recipe rejected");
    require_true(recipe_cancels == 0, "automatic request canceled itself");
    command.grinder_recipe_generation = 8;
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_CONFLICT, "recipe generation not deduplicated");
    recipe_permitted = false;
    require_true(hub_command_coordinator_process_next_for_test() && !observed_permission, "expired recipe allowed side effect");
    require_true(observed_online_required, "automatic command skipped live connectivity check");
    require_true(hub_command_side_effect_allowed(), "automatic guard leaked after completion");
    require_true(!hub_command_requires_online_machine(), "automatic connectivity guard leaked after completion");
    reset(); command.grinder_recipe_generation = 0;
    require_true(submit(&command, &capture, &submission) == HUB_COMMAND_ACCEPTED && recipe_cancels == 1, "manual weight did not cancel automation");
    require_true(hub_command_coordinator_process_next_for_test() && observed_permission, "manual action blocked by recipe guard");
    require_true(!observed_online_required, "manual command inherited automatic connectivity policy");
}

int main(void)
{
    test_recipe_guard_and_manual_override();
    test_v2_status_without_deadline();
    require_true(hub_command_coordinator_start() == ESP_OK, "coordinator failed to start");
    test_submit_returns_without_running_executor_inline();
    test_same_completed_request_id_returns_cached_result();
    test_same_inflight_request_id_does_not_execute_twice();
    test_queue_depth_is_bounded_at_four();
    test_daily_interactive_controls_jump_ahead_of_queued_normal_commands();
    test_normal_queue_reserves_one_outstanding_slot_for_daily_controls();
    test_normal_work_cannot_consume_any_interactive_capacity();
    test_daily_interactive_controls_keep_fifo_and_pending_count();
    test_daily_interactive_burst_survives_cloud_queue_delay();
    test_entries_expire_after_ten_minutes();
    test_high_risk_command_has_one_attempt();
    test_same_inflight_id_with_different_payload_is_conflict();
    test_same_completed_id_with_different_kind_is_conflict();
    test_same_inflight_id_for_different_target_is_conflict();
    test_missing_machine_selection_fails_closed();
    test_explicit_machine_target_mismatch_fails_closed();
    test_matching_explicit_machine_target_is_accepted();
    test_selection_change_rejects_before_executor();
    test_command_expires_before_executor();
    test_v2_invalid_ttl_fails_before_queueing();
    test_v2_capacity_rejects_before_side_effect();
    test_v2_worker_reserve_expires_before_executor();
    test_v2_duplicate_does_not_extend_original_deadline();
    test_v2_late_completion_becomes_unknown_outcome();
    test_v2_prebrew_cannot_be_displaced_after_acceptance();
    test_mode_preset_guard_is_part_of_request_identity();
    test_quick_weight_preset_guard_is_part_of_request_identity();
    test_only_guarded_mode_accepts_v2_deadline();
    test_schedule_identity_and_deduplication();
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="command-coordinator-host-") as temporary:
        temporary_path = pathlib.Path(temporary)
        include_path = temporary_path / "include"
        freertos_path = include_path / "freertos"
        freertos_path.mkdir(parents=True)
        (include_path / "esp_err.h").write_text(textwrap.dedent(ESP_ERR_STUB), encoding="utf-8")
        (include_path / "esp_timer.h").write_text(textwrap.dedent(TIMER_STUB), encoding="utf-8")
        (include_path / "esp_random.h").write_text(textwrap.dedent(RANDOM_STUB), encoding="utf-8")
        (include_path / "esp_log.h").write_text(textwrap.dedent(LOG_STUB), encoding="utf-8")
        (include_path / "esp_heap_caps.h").write_text(textwrap.dedent(HEAP_CAPS_STUB), encoding="utf-8")
        (freertos_path / "FreeRTOS.h").write_text(textwrap.dedent(FREERTOS_STUB), encoding="utf-8")
        (freertos_path / "queue.h").write_text(textwrap.dedent(QUEUE_STUB), encoding="utf-8")
        (freertos_path / "semphr.h").write_text(textwrap.dedent(SEMPHR_STUB), encoding="utf-8")
        (freertos_path / "task.h").write_text(textwrap.dedent(TASK_STUB), encoding="utf-8")
        harness_path = temporary_path / "harness.c"
        harness_path.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
        binary_path = temporary_path / "harness"

        command = [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DHUB_GRINDER=1",
            "-I", str(ROOT / "integrated/main"),
            "-I",
            str(include_path),
            "-I",
            str(MAIN_DIR),
            str(harness_path),
            str(MAIN_DIR / "hub_command_coordinator.c"),
            str(MAIN_DIR / "hub_command_deadline.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)

    print("command coordinator host tests passed (31 cases including automatic recipe guard)")


if __name__ == "__main__":
    main()
