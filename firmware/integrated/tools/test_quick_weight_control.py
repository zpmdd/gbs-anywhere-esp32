#!/usr/bin/env python3
"""Compile and run host tests for quick-weight partial-apply truth."""

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
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t error);
"""


TIMER_STUB = r"""
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
"""


HARNESS = r"""
#include "hub_ble.h"
#include "hub_cloud.h"
#include "hub_control.h"
#include "hub_command_coordinator.h"
#include "hub_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool mode_request_started;
static bool coffee_confirmation_timeout;
static int cloud_coffee_call_count;
static int mode_cloud_call_count;
static int dose_cloud_call_count;
static int64_t fake_now_ms = 1000;
static int64_t mode_advance_ms;
static int guarded_calls;

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

int64_t esp_timer_get_time(void)
{
    return fake_now_ms * 1000;
}

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

static unsigned schedule_calls,schedule_invalidations;
static esp_err_t schedule_error;
static hub_cloud_command_result_t schedule_result;
void hub_state_invalidate_schedule(void) { ++schedule_invalidations; }
esp_err_t hub_cloud_set_schedule(const hub_schedule_entry_t *expected,const hub_schedule_entry_t *requested,hub_cloud_command_result_t *result)
{ (void)expected; (void)requested; ++schedule_calls; *result=schedule_result; return schedule_error; }

void hub_state_record_command(
    const char *command,
    const char *transport,
    bool ok,
    int duration_ms,
    const char *message
)
{
    (void)command;
    (void)transport;
    (void)ok;
    (void)duration_ms;
    (void)message;
}

esp_err_t hub_cloud_set_bbw_mode(const char *mode, hub_cloud_command_result_t *result)
{
    mode_cloud_call_count++;
    require_true(strcmp(mode, "Dose1") == 0, "quick weight did not restore Dose1 mode");
    memset(result, 0, sizeof(*result));
    result->ok = true;
    result->duration_ms = 11;
    result->upstream_attempts = mode_request_started ? 1 : 0;
    result->http_status = mode_request_started ? 200 : 0;
    result->request_started = mode_request_started;
    snprintf(
        result->message,
        sizeof(result->message),
        "%s",
        mode_request_started ? "Mode changed to Dose1." : "Mode already matched cached snapshot."
    );
    return ESP_OK;
}

esp_err_t hub_cloud_set_bbw_dose(const char *dose, int grams, hub_cloud_command_result_t *result)
{
    dose_cloud_call_count++;
    require_true(strcmp(dose, "dose1") == 0, "quick weight did not target Dose1");
    require_true(grams == 30, "quick weight changed the requested grams");
    memset(result, 0, sizeof(*result));
    result->ok = false;
    result->duration_ms = 17;
    result->upstream_attempts = 1;
    result->http_status = 503;
    result->request_started = true;
    snprintf(result->message, sizeof(result->message), "Dose1 update failed.");
    return ESP_FAIL;
}

esp_err_t hub_cloud_set_bbw_mode_with_deadline(
    const char *mode,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
)
{
    (void)deadline;
    (void)reserve_after_ms;
    esp_err_t error = hub_cloud_set_bbw_mode(mode, result);
    fake_now_ms += mode_advance_ms;
    return error;
}

esp_err_t hub_cloud_set_bbw_dose_with_deadline(
    const char *dose,
    int grams,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
)
{
    (void)deadline;
    (void)reserve_after_ms;
    return hub_cloud_set_bbw_dose(dose, grams, result);
}

esp_err_t hub_cloud_set_bbw_mode_checked_with_deadline(
    const char *mode, int preset_grams, bool require_no_stored_match,
    const hub_command_deadline_t *deadline, uint32_t reserve, hub_cloud_command_result_t *result)
{
    require_true(!strcmp(mode,"Dose1") && preset_grams==30 && deadline!=NULL,
                 "preset decision or deadline was lost");
    (void)require_no_stored_match; (void)reserve;
    ++guarded_calls; memset(result,0,sizeof(*result)); result->preset_changed=true;
    return ESP_ERR_INVALID_STATE;
}

#define CLOUD_FAILURE_STUB(name, signature) \
    esp_err_t name signature \
    { \
        (void)result; \
        return ESP_FAIL; \
    }

CLOUD_FAILURE_STUB(hub_cloud_set_prebrew,
                   (const char *field, const char *value, hub_cloud_command_result_t *result))
CLOUD_FAILURE_STUB(hub_cloud_set_power,
                   (bool enabled, hub_cloud_command_result_t *result))
CLOUD_FAILURE_STUB(hub_cloud_set_steam_level,
                   (int level, hub_cloud_command_result_t *result))
CLOUD_FAILURE_STUB(hub_cloud_set_steam_enabled,
                   (bool enabled, hub_cloud_command_result_t *result))
CLOUD_FAILURE_STUB(hub_cloud_set_steam_temperature,
                   (double temperature, hub_cloud_command_result_t *result))
CLOUD_FAILURE_STUB(hub_cloud_set_smart_standby,
                   (bool enabled, int minutes, const char *mode, hub_cloud_command_result_t *result))
CLOUD_FAILURE_STUB(hub_cloud_start_backflush,
                   (hub_cloud_command_result_t *result))
CLOUD_FAILURE_STUB(hub_cloud_update_machine_firmware,
                   (hub_cloud_command_result_t *result))

#undef CLOUD_FAILURE_STUB

esp_err_t hub_cloud_set_prebrew_with_deadline(
    const char *field,
    const char *value,
    const hub_command_deadline_t *deadline,
    uint32_t reserve_after_ms,
    hub_cloud_command_result_t *result
)
{
    (void)deadline;
    (void)reserve_after_ms;
    return hub_cloud_set_prebrew(field, value, result);
}

esp_err_t hub_cloud_set_coffee_temperature(
    double temperature,
    hub_cloud_command_result_t *result
)
{
    (void)temperature;
    (void)result;
    cloud_coffee_call_count++;
    return ESP_FAIL;
}

#define BLE_FAILURE_STUB(name, signature) \
    esp_err_t name signature \
    { \
        (void)result; \
        return ESP_FAIL; \
    }

BLE_FAILURE_STUB(hub_ble_set_steam_temperature,
                 (double temperature, hub_ble_command_result_t *result))
BLE_FAILURE_STUB(hub_ble_set_steam_enabled,
                 (bool enabled, hub_ble_command_result_t *result))
BLE_FAILURE_STUB(hub_ble_set_power,
                 (bool enabled, hub_ble_command_result_t *result))
BLE_FAILURE_STUB(hub_ble_set_smart_standby,
                 (bool enabled, int minutes, const char *mode, hub_ble_command_result_t *result))
BLE_FAILURE_STUB(hub_ble_update_machine_firmware,
                 (hub_ble_command_result_t *result))

#undef BLE_FAILURE_STUB

esp_err_t hub_ble_set_coffee_temperature(
    double temperature,
    hub_ble_command_result_t *result
)
{
    require_true(temperature == 93.5, "coffee temperature target changed");
    memset(result, 0, sizeof(*result));
    result->attempts = 1;
    result->request_started = true;
    result->write_accepted = true;
    result->observation_sequence = coffee_confirmation_timeout ? 41 : 42;
    result->state_confirmation_ms = coffee_confirmation_timeout ? 1500 : 187;
    if (coffee_confirmation_timeout) {
        result->ok = false;
        result->state_confirmation_pending = true;
        result->late_side_effect_risk = true;
        snprintf(result->error_code, sizeof(result->error_code), "stateConfirmationTimeout");
        snprintf(result->message, sizeof(result->message), "state confirmation timed out");
        return ESP_ERR_TIMEOUT;
    }
    result->ok = true;
    result->state_confirmed = true;
    snprintf(result->message, sizeof(result->message), "state confirmed");
    return ESP_OK;
}

static void test_real_mode_change_followed_by_dose_failure_is_partial_apply(void)
{
    mode_request_started = true;
    hub_control_result_t result = {0};

    esp_err_t error = hub_control_execute_quick_weight(30, &result);

    require_true(error == ESP_FAIL, "dose failure did not fail quick weight");
    require_true(!result.ok, "partial quick weight reported success");
    require_true(result.transport == HUB_CONTROL_TRANSPORT_CLOUD,
                 "confirmed Cloud mode change was hidden as transport=none");
    require_true(strcmp(result.error_code, "partialApply") == 0,
                 "partial quick weight did not expose partialApply");
    require_true(result.upstream_attempts == 2,
                 "partial quick weight did not aggregate both upstream attempts");
    require_true(result.cloud_duration_ms == 28,
                 "partial quick weight did not aggregate Cloud duration");
}

static void test_cached_mode_noop_followed_by_dose_failure_is_not_partial_apply(void)
{
    mode_request_started = false;
    hub_control_result_t result = {0};

    esp_err_t error = hub_control_execute_quick_weight(30, &result);

    require_true(error == ESP_FAIL, "dose failure did not fail quick weight");
    require_true(!result.ok, "failed quick weight reported success");
    require_true(result.transport == HUB_CONTROL_TRANSPORT_CLOUD &&
                 !strcmp(result.error_code,"unknownOutcome") && result.state_confirmation_pending,
                 "a sent dose request with a 503 reply must retain unknown outcome");
    require_true(strcmp(result.error_code, "partialApply") != 0,
                 "cached mode no-op incorrectly exposed partialApply");
    require_true(result.upstream_attempts == 1,
                 "cached mode no-op invented an upstream attempt");
}

static void test_coffee_confirmation_timeout_stays_terminal_ble_failure(void)
{
    coffee_confirmation_timeout = true;
    cloud_coffee_call_count = 0;
    hub_control_result_t result = {0};

    esp_err_t error = hub_control_execute_coffee_temperature(93.5, &result);

    require_true(error == ESP_ERR_TIMEOUT, "confirmation timeout error was hidden");
    require_true(!result.ok && !result.pending, "confirmation timeout was not terminal failure");
    require_true(result.transport == HUB_CONTROL_TRANSPORT_BLE,
                 "accepted BLE write was hidden as transport=none");
    require_true(result.preferred_transport == HUB_CONTROL_TRANSPORT_BLE,
                 "confirmation timeout changed preferred transport");
    require_true(!result.fallback_used && !result.cloud_attempted,
                 "confirmation timeout attempted Cloud fallback");
    require_true(result.upstream_attempts == 1, "confirmation timeout invented another attempt");
    require_true(!result.requires_confirmation,
                 "coffee state pending leaked into requiresConfirmation");
    require_true(result.write_accepted && !result.state_confirmed,
                 "accepted unconfirmed write semantics were lost");
    require_true(result.state_confirmation_pending && result.late_side_effect_risk,
                 "late side effect risk was not exposed");
    require_true(result.state_confirmation_ms == 1500,
                 "confirmation deadline duration was not propagated");
    require_true(result.ble_observation_sequence == 41,
                 "BLE observation sequence was not propagated");
    require_true(strcmp(result.error_code, "stateConfirmationTimeout") == 0,
                 "confirmation timeout error code was not propagated");
    require_true(cloud_coffee_call_count == 0,
                 "Cloud fallback ran after the BLE write was accepted");

    char json[2048] = {0};
    hub_control_result_json(&result, json, sizeof(json));
    require_true(strstr(json, "\"transport\":\"ble\"") != NULL,
                 "terminal JSON hid BLE transport");
    require_true(strstr(json, "\"writeAccepted\":true") != NULL,
                 "terminal JSON omitted writeAccepted");
    require_true(strstr(json, "\"stateConfirmed\":false") != NULL,
                 "terminal JSON omitted stateConfirmed=false");
    require_true(strstr(json, "\"stateConfirmationPending\":true") != NULL,
                 "terminal JSON omitted stateConfirmationPending");
    require_true(strstr(json, "\"lateSideEffectRisk\":true") != NULL,
                 "terminal JSON omitted lateSideEffectRisk");
}

static void test_coffee_confirmation_success_is_strict_ble_success(void)
{
    coffee_confirmation_timeout = false;
    cloud_coffee_call_count = 0;
    hub_control_result_t result = {0};

    esp_err_t error = hub_control_execute_coffee_temperature(93.5, &result);

    require_true(error == ESP_OK && result.ok, "confirmed coffee write did not succeed");
    require_true(result.transport == HUB_CONTROL_TRANSPORT_BLE,
                 "confirmed coffee write did not report BLE");
    require_true(result.write_accepted && result.state_confirmed,
                 "confirmed coffee write semantics were lost");
    require_true(!result.state_confirmation_pending && !result.late_side_effect_risk,
                 "confirmed coffee write retained pending risk");
    require_true(result.state_confirmation_ms == 187,
                 "confirmation latency was not propagated");
    require_true(!result.requires_confirmation,
                 "coffee success incorrectly requires manual confirmation");
    require_true(cloud_coffee_call_count == 0,
                 "confirmed BLE success attempted Cloud fallback");
}

static void reset_deadline_test_state(void)
{
    fake_now_ms = 0;
    mode_advance_ms = 0;
    mode_cloud_call_count = 0;
    dose_cloud_call_count = 0;
}

static hub_command_deadline_t accepted_deadline(uint32_t ttl_ms)
{
    hub_command_deadline_t deadline = {
        .enforced = true,
        .client_ttl_present = true,
        .client_ttl_ms = ttl_ms,
    };
    require_true(hub_command_deadline_accept(&deadline, fake_now_ms),
                 "test deadline was not accepted");
    return deadline;
}

static void test_v2_quick_weight_rejects_before_any_cloud_step(void)
{
    reset_deadline_test_state();
    hub_command_deadline_t deadline = accepted_deadline(5000);
    hub_control_result_t result = {0};

    esp_err_t error = hub_control_execute_quick_weight_with_deadline(
        30,
        &deadline,
        &result
    );

    require_true(error == ESP_ERR_TIMEOUT, "insufficient v2 deadline did not timeout");
    require_true(mode_cloud_call_count == 0 && dose_cloud_call_count == 0,
                 "insufficient v2 deadline started a Cloud step");
    require_true(result.upstream_attempts == 0,
                 "insufficient v2 deadline invented an upstream attempt");
    require_true(strcmp(result.error_code, "commandExpired") == 0,
                 "insufficient v2 deadline lost commandExpired");
}

static void test_v2_mode_apply_then_dose_budget_exhaustion_is_partial(void)
{
    reset_deadline_test_state();
    mode_request_started = true;
    mode_advance_ms = 6000;
    hub_command_deadline_t deadline = accepted_deadline(8000);
    hub_control_result_t result = {0};

    esp_err_t error = hub_control_execute_quick_weight_with_deadline(
        30,
        &deadline,
        &result
    );

    require_true(error == ESP_ERR_TIMEOUT, "dose budget exhaustion did not timeout");
    require_true(mode_cloud_call_count == 1 && dose_cloud_call_count == 0,
                 "dose budget exhaustion started the forbidden dose step");
    require_true(result.v2_status == HUB_API_V2_COMMAND_STATUS_PARTIAL_APPLY,
                 "applied mode plus skipped dose was not partialApply");
    require_true(strcmp(result.error_code, "partialApply") == 0,
                 "partial deadline result lost partialApply error code");
    require_true(result.upstream_attempts == 1,
                 "partial deadline result lost the mode attempt");
}

static void test_v2_unconfirmed_dose_without_mode_side_effect_is_unknown(void)
{
    reset_deadline_test_state();
    mode_request_started = false;
    hub_command_deadline_t deadline = accepted_deadline(8000);
    hub_control_result_t result = {0};

    esp_err_t error = hub_control_execute_quick_weight_with_deadline(
        30,
        &deadline,
        &result
    );

    require_true(error == ESP_FAIL, "unconfirmed v2 dose failure was hidden");
    require_true(result.v2_status == HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME,
                 "unconfirmed v2 dose did not become unknownOutcome");
    require_true(strcmp(result.error_code, "unknownOutcome") == 0,
                 "unconfirmed v2 dose lost unknownOutcome error code");
    require_true(result.transport == HUB_CONTROL_TRANSPORT_CLOUD,
                 "unconfirmed v2 dose hid its Cloud side effect");
}

static void test_schedule_confirmation_and_unknown_result(void)
{
    hub_command_request_t request={.kind=HUB_COMMAND_SCHEDULE};
    hub_control_result_t result={0};
    schedule_calls=schedule_invalidations=0;
    require_true(hub_control_execute_request(&request,&result)!=ESP_OK&&!schedule_calls,"unconfirmed schedule reached Cloud");
    request.value.schedule.confirm=true;
    schedule_error=ESP_OK;
    schedule_result=(hub_cloud_command_result_t){.ok=true,.request_started=true,.upstream_attempts=1,.http_status=200};
    require_true(hub_control_execute_request(&request,&result)==ESP_OK&&result.state_confirmed&&result.write_accepted,
                 "successful schedule lost readback confirmation");
    require_true(schedule_calls==1&&schedule_invalidations==1,"schedule write did not invalidate the cache");
    schedule_error=ESP_ERR_TIMEOUT; schedule_result.ok=false; schedule_result.http_status=0;
    require_true(hub_control_execute_request(&request,&result)!=ESP_OK&&!result.state_confirmed&&result.late_side_effect_risk,
                 "lost schedule reply became a definite failure or success");
    require_true(!strcmp(result.error_code,"unknownOutcome")&&schedule_invalidations==2,"unknown schedule outcome or invalidation missing");
    schedule_error=ESP_ERR_INVALID_STATE;
    schedule_result=(hub_cloud_command_result_t){.schedule_changed=true};
    require_true(hub_control_execute_request(&request,&result)!=ESP_OK&&!strcmp(result.error_code,"scheduleChanged")
                 &&!result.late_side_effect_risk&&schedule_invalidations==2,"schedule conflict was not a no-write refusal");
}

int main(void)
{
    hub_command_deadline_t guarded_deadline={.enforced=true,.deadline_at_ms=20000};
    for(int quick=0;quick<2;++quick) {
        hub_command_request_t request={.kind=quick?HUB_COMMAND_QUICK_WEIGHT:HUB_COMMAND_BBW_MODE};
        request.deadline=guarded_deadline;
        if(quick) { request.value.quick_weight.grams=30; request.value.quick_weight.require_no_stored_match=true; }
        else { strcpy(request.value.bbw_mode.mode,"Dose1"); request.value.bbw_mode.has_preset_grams=true; request.value.bbw_mode.preset_grams=30; }
        hub_control_result_t result={0};
        int dose_before=dose_cloud_call_count;
        require_true(hub_control_execute_request(&request,&result)!=ESP_OK && !result.ok &&
                     !strcmp(result.error_code,"presetChanged") && dose_cloud_call_count==dose_before,
                     "changed presets must stop before any dose write");
    }
    require_true(guarded_calls==2,"guarded dispatch missing");
    test_schedule_confirmation_and_unknown_result();
    test_real_mode_change_followed_by_dose_failure_is_partial_apply();
    test_cached_mode_noop_followed_by_dose_failure_is_not_partial_apply();
    test_coffee_confirmation_timeout_stays_terminal_ble_failure();
    test_coffee_confirmation_success_is_strict_ble_success();
    test_v2_quick_weight_rejects_before_any_cloud_step();
    test_v2_mode_apply_then_dose_budget_exhaustion_is_partial();
    test_v2_unconfirmed_dose_without_mode_side_effect_is_unknown();
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="quick-weight-control-host-") as temporary:
        temporary_path = pathlib.Path(temporary)
        include_path = temporary_path / "include"
        include_path.mkdir(parents=True)
        (include_path / "esp_err.h").write_text(textwrap.dedent(ESP_ERR_STUB), encoding="utf-8")
        (include_path / "esp_timer.h").write_text(textwrap.dedent(TIMER_STUB), encoding="utf-8")
        harness_path = temporary_path / "harness.c"
        harness_path.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
        binary_path = temporary_path / "harness"

        command = [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-I",
            str(include_path),
            "-I",
            str(MAIN_DIR),
            str(harness_path),
            str(MAIN_DIR / "hub_control.c"),
            str(MAIN_DIR / "hub_command_deadline.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)

    print("quick-weight/control contract host tests passed (10 cases including guarded dispatch)")


if __name__ == "__main__":
    main()
