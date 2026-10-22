#!/usr/bin/env python3
"""Source-level guards for the LamaBuddy stability foundation."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class SourceTree:
    root: Path

    def read(self, relative_path: str) -> str:
        return (self.root / relative_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"^[\w\s\*]+{re.escape(name)}\s*\([^)]*\)\s*\{{", source, re.M)
    if match is None:
        raise AssertionError(f"missing function {name}")
    depth = 0
    start = match.end() - 1
    for index in range(start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : index]
    raise AssertionError(f"unterminated function {name}")


def test_runtime_resource_truth(source: SourceTree) -> None:
    app = source.read("main/app_main.c")
    http = source.read("main/hub_http.c")
    resources = source.read("main/hub_runtime_resources.c")
    cmake = source.read("CMakeLists.txt")
    defaults = source.read("sdkconfig.defaults")

    assert 'ESP_LOGI(TAG, "psram=8MB")' not in app
    assert '\"psramMB\":8' not in http
    assert "hub_runtime_resources_snapshot" in app
    assert "hub_runtime_resources_snapshot" in http
    assert "CONFIG_SPIRAM=y" in defaults
    assert "CONFIG_SPIRAM_MODE_OCT=y" in defaults
    assert "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096" in defaults
    assert "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y" in defaults
    assert 'set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")' in cmake
    assert "#ifndef CONFIG_SPIRAM" in resources
    assert "LamaBuddy Hub requires PSRAM" in resources


def test_state_reads_are_pure(source: SourceTree) -> None:
    http = source.read("main/hub_http.c")
    keeper = source.read("main/hub_state_keeper.c")
    wifi = source.read("main/hub_wifi.c")
    wifi_h = source.read("main/hub_wifi.h")
    machine_state_body = function_body(http, "machine_state_handler")

    for forbidden in (
        "hub_state_keeper_touch",
        "hub_state_keeper_request_recovery",
        "hub_cloud_refresh",
        "hub_ble_validate_read",
    ):
        assert forbidden not in machine_state_body
    assert "hub_wifi_station_has_ip()" in keeper
    assert "bool hub_wifi_station_has_ip(void)" in wifi_h
    assert "atomic_store(&s_station_has_ip, true)" in wifi
    assert "{5000, 15000, 30000, 60000, 300000, 600000, 1800000, 3600000}" in keeper


def test_command_history_uses_psram(source: SourceTree) -> None:
    coordinator = source.read("main/hub_command_coordinator.c")

    assert "static command_completed_entry_t *completed_entries;" in coordinator
    assert "static command_completed_entry_t completed_entries[" not in coordinator
    assert "heap_caps_calloc" in coordinator
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in coordinator


def test_cloud_refresh_requests_use_the_keeper(source: SourceTree) -> None:
    cloud = source.read("main/hub_cloud.c")
    keeper = source.read("main/hub_state_keeper.c")
    request_body = function_body(cloud, "hub_cloud_request_refresh")

    assert "hub_state_keeper_request_recovery()" in request_body
    assert "xTaskCreate" not in request_body
    assert "command_refresh_task" not in cloud
    assert "CLOUD_REFRESH_TASK_STACK_BYTES" not in cloud
    assert "hub_cloud_refresh_is_running()" in keeper
    assert "hub_state_keeper_request_recovery()" in keeper
    assert "hub_cloud_complete_refresh_request(refresh_ticket)" in keeper


def test_commands_are_accepted_without_holding_http_connections(source: SourceTree) -> None:
    cmake = source.read("main/CMakeLists.txt")
    app = source.read("main/app_main.c")
    http = source.read("main/hub_http.c")
    coordinator = source.read("main/hub_command_coordinator.c")
    cloud = source.read("main/hub_cloud.c")
    cloud_h = source.read("main/hub_cloud.h")
    keeper = source.read("main/hub_state_keeper.c")
    control = source.read("main/hub_control.c")
    ble_h = source.read("main/hub_ble.h")

    assert "hub_command_coordinator.c" in cmake
    assert "hub_command_coordinator_start()" in app
    submit_body = function_body(http, "submit_control_command")
    assert "httpd_req_async_handler_begin" not in submit_body
    assert "httpd_req_async_handler_complete" not in submit_body
    assert "hub_command_submit" in http
    assert "HUB_COMMAND_ACCEPTED" in submit_body
    assert "submission.result.response_http_status = 202" in submit_body
    assert "send_control_response_without_state_effect" in submit_body
    completion_body = function_body(http, "command_state_completion")
    assert "httpd_req" not in completion_body
    assert "apply_control_completion_state_effect" in completion_body
    state_effect_body = function_body(http, "apply_control_completion_state_effect")
    assert "hub_state_keeper_touch()" in state_effect_body
    assert "hub_state_keeper_request_recovery()" in state_effect_body
    assert '"/diagnostics/commands"' in http
    coordinator_header = source.read("main/hub_command_coordinator.h")
    assert "HUB_COMMAND_QUEUE_CAPACITY 7" in coordinator_header
    assert "HUB_COMMAND_INTERACTIVE_QUEUE_CAPACITY 4" in coordinator_header
    assert "HUB_COMMAND_NORMAL_QUEUE_CAPACITY 3" in coordinator_header
    assert "HUB_COMMAND_COMPLETED_CAPACITY 32" in source.read("main/hub_command_coordinator.h")
    assert "HUB_COMMAND_COMPLETED_TTL_MS (10LL * 60LL * 1000LL)" in coordinator
    assert "result.upstream_attempts = 1" not in coordinator

    assert "hub_cloud_command_attempt_t" in cloud_h
    assert "size_t response_bytes;" in cloud_h
    assert "uint32_t first_bytes;" in cloud_h
    assert "CLOUD_COMMAND_AFTER_IO_COOLDOWN_MS = 500" in cloud
    assert "CLOUD_COMMAND_MIN_INTERNAL_BLOCK_BYTES = 6144" in cloud
    assert "CLOUD_COMMAND_HEAP_SETTLE_MAX_MS = 3000" in cloud
    assert "CLOUD_READONLY_HTTP_TIMEOUT_MS = 4000" in cloud
    assert "CLOUD_COMMAND_HTTP_TIMEOUT_MS = 12000" in cloud
    assert "CLOUD_COMMAND_REFRESH_WAIT_MS = 12000" in cloud

    context_body = function_body(cloud, "load_cloud_context")
    assert "copy_cached_tokens_if_valid(tokens)" in context_body
    assert "get_access_tokens" not in context_body
    assert "ensure_time_synced" not in context_body
    assert "hub_config_save_cloud_tokens" not in context_body

    ready_body = function_body(cloud, "ensure_cloud_controls_ready")
    assert "hub_cloud_request_refresh()" in ready_body
    assert "hub_cloud_refresh()" not in ready_body
    assert "wait_for_cloud_controls_ready" not in cloud

    assert "static SemaphoreHandle_t token_mutex" in cloud
    assert "ensure_token_mutex" in cloud
    assert "copy_cached_tokens_if_valid" in cloud
    assert "ACCESS_TOKEN_PROACTIVE_RENEWAL_MS = 40LL * 60LL * 1000LL" in cloud
    assert "copy_cached_tokens_for_refresh(tokens)" in cloud
    assert "hub_cloud_token_renewal_due_ms()" in keeper
    assert "if (token_due_ms > 0 && token_due_ms < next_due_ms)" in keeper
    assert "if (!cold && token_due_ms > 0 && token_due_ms < next_due_ms)" not in keeper

    locked_body = function_body(cloud, "execute_command_with_auth_locked")
    assert "attempt.http_status == 401 || attempt.http_status == 403" in locked_body
    assert "invalidate_cached_tokens()" in locked_body
    assert "hub_cloud_request_refresh()" not in locked_body
    assert "execute_command_once_with_auth" in locked_body
    assert "hub_cloud_command_attempt_t retry" not in locked_body
    assert "cloud_command_retryable_error" not in locked_body

    execute_body = function_body(cloud, "execute_command_with_auth")
    assert execute_body.index("cloud_command_io_unlock()") < execute_body.index("hub_cloud_request_refresh()")
    assert "cloud_http_retryable_error(attempt.err)" not in execute_body
    assert "!attempt.request_started && attempt.err == ESP_ERR_INVALID_STATE" in execute_body

    command_lock_body = function_body(cloud, "cloud_command_io_lock")
    command_unlock_body = function_body(cloud, "cloud_command_io_unlock")
    command_heap_body = function_body(cloud, "cloud_command_wait_for_internal_heap")
    assert "hub_command_deadline_timeout_ms(" in command_lock_body
    assert "pdMS_TO_TICKS((uint32_t)lock_wait_ms)" in command_lock_body
    assert "cloud_command_wait_for_internal_heap(" in command_lock_body
    assert "heap_caps_get_largest_free_block" in command_heap_body
    assert "mark_cloud_io_completed()" in command_unlock_body

    connect_guard_body = function_body(cloud, "cloud_command_connect_failed_before_request")
    assert "attempt->err == ESP_ERR_HTTP_CONNECT" in connect_guard_body
    assert "attempt->http_status == 0" in connect_guard_body
    assert "attempt->response_bytes == 0" in connect_guard_body
    command_url_body = function_body(cloud, "cloud_execute_url")
    assert "const int max_connect_attempts = 2" in command_url_body
    assert "attempt.request_started = !failed_before_request" in command_url_body
    assert "CLOUD_COMMAND_CONNECT_RETRY_DELAY_MS" in command_url_body
    assert "ESP_ERR_HTTP_CONNECT is raised before the HTTP request is written" in command_url_body
    assert "request_timeout_ms = hub_command_deadline_timeout_ms(" in command_url_body
    assert "CLOUD_COMMAND_HTTP_TIMEOUT_MS" in command_url_body
    assert "request_timeout_ms" in command_url_body
    assert command_url_body.count("cloud_http_request_no_retry(") == 1
    assert "cloud_http_request(" not in command_url_body

    refresh_pending_snapshot_body = function_body(cloud, "cloud_refresh_pending_snapshot")
    assert "portENTER_CRITICAL(&refresh_mutex_lock)" in refresh_pending_snapshot_body
    assert "command_refresh_pending || refresh_running" in refresh_pending_snapshot_body
    assert "portEXIT_CRITICAL(&refresh_mutex_lock)" in refresh_pending_snapshot_body
    assert "out->refresh_pending = cloud_refresh_pending_snapshot()" in cloud
    assert "return cloud_refresh_pending_snapshot()" in function_body(cloud, "hub_cloud_refresh_pending")

    retryable_body = function_body(cloud, "cloud_http_retryable_error")
    for error_name in (
        "ESP_ERR_HTTP_WRITE_DATA",
        "ESP_ERR_HTTP_EAGAIN",
        "ESP_ERR_HTTP_FETCH_HEADER",
        "ESP_ERR_HTTP_CONNECTION_CLOSED",
        "ESP_ERR_HTTP_READ_TIMEOUT",
        "ESP_ERR_HTTP_INCOMPLETE_DATA",
    ):
        assert error_name in retryable_body

    failure_body = function_body(cloud, "cloud_command_failure_message")
    assert "attempt->request_started" in failure_body
    assert "attempt->response_bytes" in failure_body
    assert "attempt->first_bytes" in failure_body
    assert "last_cloud_command_status_code" not in failure_body
    assert "failed before upstream request" in failure_body

    setting_body = function_body(cloud, "execute_setting_command_with_auth")
    assert "execute_command_with_auth(" in setting_body
    assert "deadline" in setting_body
    assert "reserve_after_ms" in setting_body
    assert "max_attempts" not in setting_body
    assert "vTaskDelay" not in setting_body
    assert "setting_command_retry_delay_ms" not in cloud

    for wrapper_name, implementation_name in (
        ("hub_cloud_set_bbw_dose", "hub_cloud_set_bbw_dose_with_deadline"),
        ("hub_cloud_set_bbw_mode", "hub_cloud_set_bbw_mode_with_deadline"),
        ("hub_cloud_set_prebrew", "hub_cloud_set_prebrew_with_deadline"),
    ):
        wrapper_body = function_body(cloud, wrapper_name)
        assert implementation_name in wrapper_body
        assert "NULL" in wrapper_body
        assert "0" in wrapper_body

    auth_body = function_body(cloud, "cloud_http_request")
    assert "const int max_attempts = 3" in auth_body
    assert "CLOUD_READONLY_HTTP_TIMEOUT_MS" in auth_body
    assert "cloud_command_is_waiting()" in auth_body

    firmware_body = function_body(cloud, "hub_cloud_update_machine_firmware")
    assert "cloud_http_request(" not in firmware_body
    assert "for (int attempt" not in firmware_body

    assert "bool request_started;" in ble_h
    assert "bool guard_rejected;" in ble_h
    assert "bool write_accepted;" in ble_h
    assert "bool state_confirmed;" in ble_h
    assert "bool state_confirmation_pending;" in ble_h
    assert "bool late_side_effect_risk;" in ble_h
    assert "int pre_write_ms;" in ble_h
    assert "int write_status_ms;" in ble_h
    assert "int state_confirmation_ms;" in ble_h
    assert "int confirmation_read_count;" in ble_h
    assert "int confirmation_selector_write_count;" in ble_h
    assert "int confirmation_direct_read_count;" in ble_h
    assert "bool confirmation_direct_first;" in ble_h
    assert "int confirmation_reprime_count;" in ble_h
    assert "uint32_t observation_sequence;" in ble_h
    assert "char error_code[40];" in ble_h
    assert "bool requires_confirmation;" not in ble_h
    assert "int attempts;" in ble_h
    fallback_body = function_body(control, "should_try_cloud_after_ble")
    normalized_fallback_body = " ".join(fallback_body.split())
    assert (
        "return err != ESP_OK && ble_result != NULL && !ble_result->ok "
        "&& !ble_result->guard_rejected && !ble_result->request_started;"
    ) in normalized_fallback_body
    assert "!ble_result->guard_rejected" in fallback_body
    assert "result->upstream_attempts" in control


def test_commands_are_bound_to_one_machine_selection(source: SourceTree) -> None:
    coordinator = source.read("main/hub_command_coordinator.c")
    coordinator_h = source.read("main/hub_command_coordinator.h")
    config = source.read("main/hub_config.c")
    config_h = source.read("main/hub_config.h")
    cloud = source.read("main/hub_cloud.c")
    ble = source.read("main/hub_ble.c")
    control = source.read("main/hub_control.c")
    http = source.read("main/hub_http.c")
    api_v2 = source.read("main/hub_api_v2.h")

    assert "HUB_COMMAND_START_DEADLINE_MS 4000" in coordinator_h
    assert "HUB_COMMAND_INTERACTIVE_START_DEADLINE_MS 10000" in coordinator_h
    request_priority_body = function_body(coordinator, "request_is_interactive_priority")
    assert "command_is_interactive_priority(request->kind)" in request_priority_body
    assert "request->deadline.enforced" in request_priority_body
    assert "request->kind == HUB_COMMAND_PREBREW" in request_priority_body
    assert "request_is_interactive_priority(&item.request)" in coordinator
    assert "hub_machine_selection_identity_t target;" in coordinator_h
    assert "payload_fingerprint" in coordinator_h
    assert "expected_serial_number" in coordinator_h
    assert "requestIdPayloadConflict" in coordinator
    assert "requestIdTargetConflict" in coordinator
    assert "machineSelectionChanged" in coordinator
    assert "machineTargetMismatch" in coordinator
    assert "commandExpired" in coordinator

    assert "hub_config_save_machine_selection_if_unchanged" in config_h
    assert "hub_command_target_guard_get" in config
    assert "require_unchanged" in config
    assert "hub_config_machine_selection_matches(&target)" in cloud
    assert "target.serial_number" in function_body(cloud, "cloud_execute_command")
    assert "hub_config_machine_selection_matches(&target)" in ble
    assert "hub_config_save_ble_device_address_for_serial" in ble
    assert "hub_machine_selection_identity_t selection_identity;" in ble
    assert "cached_selection_identity" in ble
    assert "cached_ble_auth_fingerprint" in ble
    cached_match_body = function_body(ble, "cached_session_matches_operation")
    assert "cached_selection_identity.generation != op->selection_identity.generation" in cached_match_body
    assert "cached_ble_auth_fingerprint != ble_auth_fingerprint" in cached_match_body
    restore_body = function_body(ble, "restore_cached_session")
    assert "ensure_operation_selection_identity(op)" in restore_body
    assert "terminate_cached_session()" in restore_body
    noop_body = function_body(ble, "run_ble_noop_write")
    assert noop_body.count("hub_config_machine_selection_matches(&op->selection_identity)") >= 2
    realtime_body = function_body(ble, "read_realtime_state")
    worker_lease_call = 'update_realtime_state_json(op, boilers, "workerRealtime")'
    assert worker_lease_call in realtime_body
    assert ble.count(worker_lease_call) == 1
    assert 'build_state_json(op, boilers, "", "", "")' not in realtime_body
    assert "HUB_BLE_STATE_CACHE_TTL_MS = 45000" in ble
    assert "ble_cache_is_fresh()" in function_body(ble, "hub_ble_has_cached_state")
    execute_ble_body = function_body(ble, "execute_ble_write_json")
    assert "restore_cached_session(&op, false)" in execute_ble_body
    assert "op.write_handle == 0" in execute_ble_body
    assert "BLE control session is not prewarmed; use Cloud fallback." in execute_ble_body
    assert "restore_cached_session(&op, true)" not in execute_ble_body
    assert "prepare_authenticated_connection(&op, true)" not in execute_ble_body
    assert "discover_handle(" not in execute_ble_body
    assert "confirmation_target_from_command" in execute_ble_body
    assert "confirm_command_target_until_deadline" in execute_ble_body
    assert "command_deadline_us = started_us" in execute_ble_body
    assert "HUB_BLE_COMMAND_DEADLINE_MS" in execute_ble_body
    assert "write_json_command_until_deadline" in execute_ble_body
    assert "write_json_command(&op" not in execute_ble_body
    assert "write_accepted = err == ESP_OK" in execute_ble_body
    assert "result->write_accepted = write_accepted" in execute_ble_body
    assert "result->state_confirmed = state_confirmed" in execute_ble_body
    assert "result->state_confirmation_pending = confirmation_pending" in execute_ble_body
    assert "result->late_side_effect_risk = request_started && err != ESP_OK" in execute_ble_body
    assert "result->late_side_effect_risk = confirmation_pending && write_accepted" not in execute_ble_body
    assert "result->pre_write_ms = pre_write_ms" in execute_ble_body
    assert "result->write_status_ms = write_status_ms" in execute_ble_body
    assert "result->confirmation_read_count = confirmation_read_count" in execute_ble_body
    assert (
        "result->confirmation_selector_write_count = confirmation_selector_write_count"
        in execute_ble_body
    )
    assert (
        "result->confirmation_direct_read_count = confirmation_direct_read_count"
        in execute_ble_body
    )
    assert (
        "result->confirmation_direct_first = confirmation_can_read_direct_first"
        in execute_ble_body
    )
    assert "&& confirmation_read_count > 0;" in execute_ble_body
    assert "result->confirmation_reprime_count = confirmation_reprime_count" in execute_ble_body
    assert 'copy_safe(result->error_code, sizeof(result->error_code), "stateConfirmationTimeout")' in execute_ble_body
    assert "read_realtime_state(&op)" not in execute_ble_body
    assert "xTaskNotifyGive(ble_worker_handle)" in execute_ble_body
    assert "HUB_BLE_COMMAND_DEADLINE_MS = 1500" in ble
    deadline_initialization = (
        "int64_t command_deadline_us = started_us\n"
        "        + (int64_t)HUB_BLE_COMMAND_DEADLINE_MS * 1000;"
    )
    assert deadline_initialization in execute_ble_body
    assert execute_ble_body.index("command_deadline_us = started_us") < execute_ble_body.index(
        "write_json_command_until_deadline"
    )

    direct_first_snapshot = (
        "bool confirmation_can_read_direct_first = confirmation_target.kind != HUB_BLE_CONFIRM_NONE\n"
        "        && command_confirmation_direct_read_ready(&op);"
    )
    assert direct_first_snapshot in execute_ble_body
    assert "confirmation_can_read_direct_first," in execute_ble_body
    assert execute_ble_body.index("restore_cached_session(&op, false)") < execute_ble_body.index(
        "command_confirmation_direct_read_ready(&op)"
    )
    assert execute_ble_body.index(
        "command_confirmation_direct_read_ready(&op)"
    ) < execute_ble_body.index("write_json_command_until_deadline(")
    direct_first_snapshot_index = execute_ble_body.index(
        "command_confirmation_direct_read_ready(&op)"
    )
    setting_write_index = execute_ble_body.index("write_json_command_until_deadline(")
    assert direct_first_snapshot_index < setting_write_index
    assert "cached_read_selector_proof.observation_sequence = 0;" not in execute_ble_body
    assert "hub_ble_read_selector_proof_t confirmation_proof_candidate" in execute_ble_body
    retained = execute_ble_body.index(
        "finish_active_connection(&op, write_accepted && connection_healthy);"
    )
    confirmed_guard = execute_ble_body.index("if (state_confirmed)", retained)
    committed = execute_ble_body.index("commit_boilers_selector_proof_candidate(", confirmed_guard)
    mutex_release = execute_ble_body.index("xSemaphoreGive(ble_op_mutex);", committed)
    assert retained < confirmed_guard < committed < mutex_release

    payload_validator_body = function_body(ble, "realtime_boilers_payload_is_valid")
    for required in (
        "cJSON_Parse(boilers_raw)",
        "cJSON_IsArray(root)",
        "cJSON_GetArraySize(root) <= 0",
        '"CoffeeBoiler1"',
        '"SteamBoiler"',
        "json_number_any(",
        "json_bool_any(",
        "cJSON_Delete(root)",
        "return coffee_valid && steam_valid;",
    ):
        assert required in payload_validator_body
    confirmation_payload_validator = function_body(
        ble, "confirmation_boilers_payload_is_valid"
    )
    assert "target != NULL" in confirmation_payload_validator
    assert "target->kind != HUB_BLE_CONFIRM_NONE" in confirmation_payload_validator
    assert (
        "realtime_boilers_payload_is_valid(boilers_raw)"
        in confirmation_payload_validator
    )

    assert "hub_ble_read_selector_proof_t" in ble
    assert "static hub_ble_read_selector_proof_t cached_read_selector_proof;" in ble
    assert ble.count("uint32_t connection_epoch;") >= 2
    assert "static uint32_t cached_connection_epoch;" in ble
    assert "static uint32_t connection_epoch_counter;" in ble
    assert "cached_read_selector_boilers" not in ble
    assert "cached_read_selector_observation_sequence" not in ble

    proof_owner_body = function_body(ble, "read_selector_proof_matches_operation")
    for required in (
        "proof->boilers",
        "proof->conn_handle == op->conn_handle",
        "op->connection_epoch != 0",
        "proof->connection_epoch == op->connection_epoch",
        "proof->read_handle == op->read_handle",
        "proof->write_handle != 0",
        "proof->write_handle != proof->read_handle",
        "op->write_handle == proof->write_handle",
        "proof->selection_generation == op->selection_identity.generation",
        "proof->selection_serial",
        "op->selection_identity.serial_number",
        "proof->auth_fingerprint",
        "ble_auth_fingerprint(op->selection.ble_auth_token)",
        "hub_config_machine_selection_matches(&op->selection_identity)",
    ):
        assert required in proof_owner_body

    proof_candidate_body = function_body(ble, "prepare_boilers_selector_proof_candidate")
    for required in (
        "memset(candidate, 0, sizeof(*candidate));",
        "candidate->boilers = true;",
        "op->connection_epoch == 0",
        "op->write_handle == 0",
        "op->write_handle == op->read_handle",
        "observation_sequence == 0",
        "candidate->write_handle = op->write_handle;",
        "candidate->observation_sequence = observation_sequence;",
        "op->selection_identity.serial_number",
    ):
        assert required in proof_candidate_body

    proof_commit_body = function_body(ble, "commit_boilers_selector_proof_candidate")
    for required in (
        "strcmp(ble_state.cache_origin, required_cache_origin) != 0",
        "ble_cache_is_fresh()",
        "candidate->observation_sequence != ble_state.cache_sequence",
        "read_selector_proof_matches_operation(candidate, op)",
        "cached_session_matches_operation(op)",
        "op->conn_handle != cached_conn_handle",
        "op->connection_epoch != cached_connection_epoch",
        "op->read_handle != cached_read_handle",
        "op->write_handle != cached_write_handle",
        "cached_write_handle == cached_read_handle",
        "cached_read_selector_proof = *candidate;",
    ):
        assert required in proof_commit_body

    proof_consume_body = function_body(
        ble, "consume_read_selector_proof_before_setting_write"
    )
    assert "cached_read_selector_proof.observation_sequence = 0;" in proof_consume_body
    for forbidden in ("hub_config_", "nvs_", "xSemaphore", "if ("):
        assert forbidden not in proof_consume_body

    direct_ready_body = function_body(ble, "command_confirmation_direct_read_ready")
    for required in (
        "ble_cache_is_fresh()",
        "read_selector_proof_belongs_to_operation(op)",
        "cached_session_matches_operation(op)",
        "op->conn_handle == cached_conn_handle",
        "op->connection_epoch == cached_connection_epoch",
        "op->read_handle == cached_read_handle",
        "op->write_handle != 0",
        "op->write_handle != op->read_handle",
        "op->write_handle == cached_write_handle",
        "cached_read_selector_proof.write_handle == cached_write_handle",
        "cached_read_selector_proof.observation_sequence != 0",
        "cached_read_selector_proof.observation_sequence == ble_state.cache_sequence",
    ):
        assert required in direct_ready_body

    next_epoch_body = function_body(ble, "next_connection_epoch")
    assert (
        "connection_epoch_counter++;\n"
        "    if (connection_epoch_counter == 0) {\n"
        "        connection_epoch_counter = 1;\n"
        "    }\n"
        "    return connection_epoch_counter;"
    ) in next_epoch_body

    gap_event_body = function_body(ble, "hub_ble_gap_event")
    successful_connect_epoch = (
        "case BLE_GAP_EVENT_CONNECT:\n"
        "        if (op != NULL && event->connect.status == 0) {\n"
        "            reset_read_selector_proof();\n"
        "            op->conn_handle = event->connect.conn_handle;\n"
        "            op->connection_epoch = next_connection_epoch();\n"
        "            op->connected = true;\n"
        "        }"
    )
    assert successful_connect_epoch in gap_event_body
    disconnect_proof_reset = (
        "case BLE_GAP_EVENT_DISCONNECT:\n"
        "        ble_state.connected = false;\n"
        "        if (cached_read_selector_proof.boilers\n"
        "            && cached_read_selector_proof.conn_handle == event->disconnect.conn.conn_handle) {\n"
        "            reset_read_selector_proof();\n"
        "        }"
    )
    assert disconnect_proof_reset in gap_event_body

    reset_cached_session_body = function_body(ble, "reset_cached_session")
    assert "cached_connection_epoch = 0;" in reset_cached_session_body
    assert "reset_read_selector_proof();" in reset_cached_session_body

    cached_session_match_body = function_body(ble, "cached_session_matches_operation")
    assert "cached_connection_epoch == 0" in cached_session_match_body
    assert "op->connection_epoch != cached_connection_epoch" in cached_session_match_body

    store_cached_session_body = function_body(ble, "store_cached_session")
    assert "op->connection_epoch == 0" in store_cached_session_body
    assert "cached_connection_epoch = op->connection_epoch;" in store_cached_session_body

    restore_cached_session_body = function_body(ble, "restore_cached_session")
    restored_epoch = "op->connection_epoch = cached_connection_epoch;"
    cached_owner_check = "cached_session_matches_operation(op)"
    assert restored_epoch in restore_cached_session_body
    assert cached_owner_check in restore_cached_session_body
    assert restore_cached_session_body.index(restored_epoch) < restore_cached_session_body.index(
        cached_owner_check
    )

    finish_connection_body = function_body(ble, "finish_active_connection")
    owner_epoch_reset = (
        "if (cached_read_selector_proof.boilers\n"
        "            && cached_read_selector_proof.conn_handle == op->conn_handle\n"
        "            && cached_read_selector_proof.connection_epoch == op->connection_epoch) {\n"
        "            reset_read_selector_proof();\n"
        "        }"
    )
    assert owner_epoch_reset in finish_connection_body
    assert finish_connection_body.index("reset_read_selector_proof();") < finish_connection_body.index(
        "ble_gap_terminate(op->conn_handle"
    )

    selector_write_begin = function_body(ble, "begin_read_selector_write")
    assert (
        "if (op != NULL && handle == op->read_handle) {\n"
        "        reset_read_selector_proof();\n"
        "    }"
    ) in selector_write_begin
    for writer_name in ("write_gatt", "write_gatt_with_timeout"):
        writer_body = function_body(ble, writer_name)
        assert "begin_read_selector_write(op, handle);" in writer_body
        assert "ble_gattc_write_flat(" in writer_body
        assert writer_body.index("begin_read_selector_write(op, handle);") < writer_body.index(
            "ble_gattc_write_flat("
        )

    realtime_cache_update = function_body(ble, "update_realtime_state_json")
    payload_validation = "if (!realtime_boilers_payload_is_valid(boilers_raw))"
    assert payload_validation in realtime_cache_update
    assert "return ESP_ERR_INVALID_RESPONSE;" in realtime_cache_update
    assert "record_cache_observation(cache_origin);" in realtime_cache_update
    for forbidden in (
        "cached_read_selector_proof",
        "prepare_boilers_selector_proof_candidate",
        "commit_boilers_selector_proof_candidate",
        "establish_direct_first_proof",
    ):
        assert forbidden not in realtime_cache_update
    assert "reset_read_selector_proof();" in function_body(ble, "reset_cached_session")

    selector_read_body = function_body(ble, "read_realtime_state_until_deadline")
    assert "read_setting_until_deadline(" in selector_read_body
    assert '"boilers"' in selector_read_body
    selector_payload_guard = (
        "if (err == ESP_OK && !confirmation_boilers_payload_is_valid(boilers, target))"
    )
    assert (
        'update_realtime_state_json(op, boilers, "commandConfirmation")'
        in selector_read_body
    )
    assert selector_payload_guard in selector_read_body
    assert "err = ESP_ERR_INVALID_RESPONSE;" in selector_read_body
    assert selector_read_body.index(selector_payload_guard) < selector_read_body.index(
        'update_realtime_state_json(op, boilers, "commandConfirmation")'
    )
    assert "prepare_boilers_selector_proof_candidate" not in selector_read_body
    assert "read_realtime_state_direct_until_deadline(" not in selector_read_body

    direct_read_body = function_body(ble, "read_realtime_state_direct_until_deadline")
    assert "deadline_remaining_ms(deadline_us)" in direct_read_body
    assert "read_gatt_once_with_timeout(" in direct_read_body
    assert "op->read_handle" in direct_read_body
    payload_guard = "if (!confirmation_boilers_payload_is_valid(boilers, target))"
    cache_update = 'update_realtime_state_json(op, boilers, "commandConfirmation")'
    assert payload_guard in direct_read_body
    assert "return ESP_ERR_INVALID_RESPONSE;" in direct_read_body
    assert cache_update in direct_read_body
    assert direct_read_body.index(payload_guard) < direct_read_body.index(cache_update)
    normalized_direct_read_body = " ".join(direct_read_body.split())
    assert (
        'if (!confirmation_boilers_payload_is_valid(boilers, target)) { '
        'set_last_error("BLE direct confirmation returned an invalid boilers payload."); '
        'return ESP_ERR_INVALID_RESPONSE; } '
        'err = update_realtime_state_json(op, boilers, "commandConfirmation");'
    ) in normalized_direct_read_body
    assert "prepare_boilers_selector_proof_candidate" not in direct_read_body
    assert ble.count(cache_update) == 2
    for forbidden in (
        "write_gatt(",
        "write_gatt_with_timeout(",
        "read_setting(",
        "read_setting_until_deadline(",
    ):
        assert forbidden not in direct_read_body

    confirmation_body = function_body(ble, "confirm_command_target_until_deadline")
    assert "HUB_BLE_STATE_CONFIRMATION_POLL_MS" in confirmation_body
    assert (
        "const hub_ble_confirmation_target_t *target,\n"
        "    bool direct_first,"
    ) in ble
    assert "bool selector_primed = direct_first;" in confirmation_body
    assert "bool boilers_selector_evidence = direct_first;" in confirmation_body
    assert "bool selector_primed = false;" not in confirmation_body
    assert "bool reprime_used = false;" in confirmation_body
    assert "while (deadline_remaining_ms(deadline_us) > 0)" in confirmation_body
    assert "bool direct = selector_primed;" in confirmation_body
    assert "reads++;" in confirmation_body
    assert "direct_reads++;" in confirmation_body
    assert "selector_writes++;" in confirmation_body
    direct_dispatch = (
        "esp_err_t read_err = direct\n"
        "            ? read_realtime_state_direct_until_deadline(op, target, deadline_us)\n"
        "            : read_realtime_state_until_deadline(op, target, deadline_us);"
    )
    assert direct_dispatch in confirmation_body
    assert "read_realtime_state_until_deadline(op, target, deadline_us)" in confirmation_body
    assert "selector_primed = true;" in confirmation_body
    assert "boilers_selector_evidence = true;" in confirmation_body
    reprime_guard = (
        "if (direct && read_err == ESP_ERR_INVALID_RESPONSE && !reprime_used) {\n"
        "                reprime_used = true;\n"
        "                selector_primed = false;\n"
        "                boilers_selector_evidence = false;\n"
        "                continue;\n"
        "            }"
    )
    assert reprime_guard in confirmation_body
    assert (
        "selector_writes++;\n"
        "            if (reprime_used) {\n"
        "                reprimes++;\n"
        "            }"
    ) in confirmation_body
    assert confirmation_body.count("reprime_used = true;") == 1
    first_loop = confirmation_body.index("while (deadline_remaining_ms(deadline_us) > 0)")
    assert confirmation_body.index("bool selector_primed = direct_first;") < first_loop
    assert "confirmation_target_matches(target, ble_state.state_json)" in confirmation_body
    target_match_index = confirmation_body.index(
        "confirmation_target_matches(target, ble_state.state_json)"
    )
    candidate_index = confirmation_body.index(
        "prepare_boilers_selector_proof_candidate("
    )
    assert "if (boilers_selector_evidence)" in confirmation_body[
        target_match_index:candidate_index
    ]
    assert target_match_index < candidate_index < confirmation_body.index(
        "return ESP_OK;", candidate_index
    )
    assert confirmation_body.count("prepare_boilers_selector_proof_candidate(") == 1
    assert "return ESP_ERR_TIMEOUT" in confirmation_body
    assert "hub_cloud_" not in confirmation_body
    assert "write_json_command" not in confirmation_body
    deadline_read_body = function_body(ble, "read_setting_until_deadline")
    assert deadline_read_body.count("deadline_remaining_ms(deadline_us)") >= 2
    assert "write_gatt_with_timeout" in deadline_read_body
    assert "read_gatt_once_with_timeout" in deadline_read_body
    assert "strlen(command) + 1" in deadline_read_body
    deadline_write_body = function_body(ble, "write_json_command_until_deadline")
    assert deadline_write_body.count("deadline_remaining_ms(deadline_us)") >= 3
    assert "write_gatt_with_timeout" in deadline_write_body
    assert "read_gatt_once_with_timeout" in deadline_write_body
    status_parser_body = function_body(ble, "write_status_is_success")
    assert "cJSON_Parse(status)" in status_parser_body
    assert 'cJSON_GetObjectItemCaseSensitive(root, "status")' in status_parser_body
    assert "cJSON_IsString(status_item)" in status_parser_body
    assert 'strcasecmp(status_item->valuestring, "success") == 0' in status_parser_body
    assert "cJSON_Delete(root)" in status_parser_body
    assert "contains_fold(" not in status_parser_body
    legacy_write_body = function_body(ble, "write_json_command")
    for command_writer_body in (legacy_write_body, deadline_write_body):
        assert "if (!write_status_is_success(status))" in command_writer_body
        assert "contains_fold(" not in command_writer_body
        assert "consume_read_selector_proof_before_setting_write();" in command_writer_body
    assert legacy_write_body.index(
        "consume_read_selector_proof_before_setting_write();"
    ) < legacy_write_body.index("write_gatt(op, op->write_handle")
    assert deadline_write_body.index(
        "consume_read_selector_proof_before_setting_write();"
    ) < deadline_write_body.index("write_gatt_with_timeout(")
    assert ble.count("prepare_boilers_selector_proof_candidate(") == 3
    assert ble.count("commit_boilers_selector_proof_candidate(") == 3
    diagnostics_body = function_body(ble, "hub_ble_diagnostics_json")
    assert '"\\\"cacheSequence\\\":%lu,"' in diagnostics_body
    assert '"\\\"cacheOrigin\\\":%s%s%s,"' in diagnostics_body
    assert '"\\\"controlSessionReady\\\":%s,"' in diagnostics_body
    prewarm_body = function_body(ble, "prewarm_control_write_handle")
    assert "op == NULL || op->write_handle != 0" in prewarm_body
    write_prewarm = "discover_handle(op, LM_WRITE_UUID, &op->write_handle)"
    assert write_prewarm in prewarm_body
    for read_function, state_read in (
        ("run_ble_read", "read_all_state(op)"),
        ("run_ble_realtime_read", "read_realtime_state(op)"),
    ):
        read_body = function_body(ble, read_function)
        assert "prepare_authenticated_connection(op, false)" in read_body
        assert "prewarm_control_write_handle(op)" in read_body
        assert read_body.index(state_read) < read_body.index("prewarm_control_write_handle(op)")
    realtime_run_body = function_body(ble, "run_ble_realtime_read")
    worker_order = (
        realtime_run_body.index("read_realtime_state(op)"),
        realtime_run_body.index("prewarm_control_write_handle(op)"),
        realtime_run_body.index("prepare_boilers_selector_proof_candidate("),
        realtime_run_body.index("finish_active_connection(op, err == ESP_OK)"),
        realtime_run_body.index("commit_boilers_selector_proof_candidate("),
    )
    assert worker_order == tuple(sorted(worker_order))
    assert '"workerRealtime"' in realtime_run_body
    regular_setting_read = function_body(ble, "read_setting")
    assert "strlen(command) + 1" in regular_setting_read
    control_h = source.read("main/hub_control.h")
    control_json_body = function_body(control, "hub_control_result_json")
    assert "int ble_pre_write_ms;" in control_h
    assert "int ble_write_status_ms;" in control_h
    assert "int confirmation_read_count;" in control_h
    assert "int confirmation_selector_write_count;" in control_h
    assert "int confirmation_direct_read_count;" in control_h
    assert "bool confirmation_direct_first;" in control_h
    assert "int confirmation_reprime_count;" in control_h
    assert '\\"blePreWriteMs\\":%d' in control_json_body
    assert '\\"bleWriteStatusMs\\":%d' in control_json_body
    assert '\\"confirmationReadCount\\":%d' in control_json_body
    assert '\\"confirmationSelectorWriteCount\\":%d' in control_json_body
    assert '\\"confirmationDirectReadCount\\":%d' in control_json_body
    assert '\\"confirmationDirectFirst\\":%s' in control_json_body
    assert '\\"confirmationReprimeCount\\":%d' in control_json_body
    set_ble_diagnostics_body = function_body(control, "set_ble_diagnostics")
    assert (
        "result->confirmation_read_count = ble_result->confirmation_read_count"
        in set_ble_diagnostics_body
    )
    assert (
        "result->confirmation_selector_write_count = "
        "ble_result->confirmation_selector_write_count"
    ) in " ".join(set_ble_diagnostics_body.split())
    assert (
        "result->confirmation_direct_read_count = "
        "ble_result->confirmation_direct_read_count"
    ) in " ".join(set_ble_diagnostics_body.split())
    assert (
        "result->confirmation_direct_first = ble_result->confirmation_direct_first"
        in set_ble_diagnostics_body
    )
    assert (
        "result->confirmation_reprime_count = ble_result->confirmation_reprime_count"
        in set_ble_diagnostics_body
    )
    finish_body = function_body(control, "finish_ble_first_result")
    assert "requires_confirmation ||" not in finish_body
    normalized_finish_body = " ".join(finish_body.split())
    assert (
        "ble_result != NULL && ble_result->request_started "
        "? HUB_CONTROL_TRANSPORT_BLE : HUB_CONTROL_TRANSPORT_NONE"
    ) in normalized_finish_body
    assert "ble_result != NULL && ble_result->write_accepted" not in finish_body
    assert "HUB_CONTROL_TRANSPORT_BLE" in finish_body
    assert 'result->error_code' in function_body(control, "set_ble_diagnostics")
    assert "cached_snapshot_matches_selection" in cloud
    assert "snapshot.serial_number" in cloud
    assert "hub_cloud_request_refresh()" in function_body(http, "setup_machine_handler")
    submit_body = function_body(http, "submit_control_command")
    assert "HUB_API_V2_MACHINE_SERIAL_HEADER_NAME" in submit_body
    assert '"X-LamaBuddy-Machine-Serial"' in api_v2
    assert "expected_serial_number" in submit_body


def test_statistics_candidates_are_atomic(source: SourceTree) -> None:
    cloud = source.read("main/hub_cloud.c")
    cache = source.read("main/hub_statistics_cache.c")
    statistics_body = function_body(cloud, "hub_cloud_statistics_json_alloc_for_serial")

    counter_guard = "counter_err != ESP_OK || !statistics_output_has_counter(counter_output)"
    assert counter_guard in statistics_body
    assert statistics_body.index("READONLY_DIAG_COUNTER") < statistics_body.index("READONLY_DIAG_RECENT")
    assert statistics_body.index(counter_guard) < statistics_body.index("update_counter_snapshot")
    assert statistics_body.index(counter_guard) < statistics_body.index("root = cJSON_CreateObject()")
    assert "statistics_snapshot" not in statistics_body
    assert "COFFEE_AND_FLUSH_TREND" in statistics_body
    assert "READONLY_DIAG_TREND" in statistics_body
    assert 'cJSON_AddItemToObject(root, "trend", trend)' in statistics_body
    assert 'cJSON_AddNullToObject(root, "trend")' in statistics_body
    assert "days <= 30" in cache
    assert "days <= 90" not in cache


def test_degraded_ble_recovery_does_not_block_controls(source: SourceTree) -> None:
    ble = source.read("main/hub_ble.c")
    execute_body = function_body(ble, "execute_ble_write_json")
    worker_body = function_body(ble, "hub_ble_worker_task")
    command_cache_delay_body = function_body(
        ble, "worker_delay_after_recent_command_confirmation"
    )

    assert "HUB_BLE_WORKER_RETRY_DELAYS_MS[] = {60000, 120000, 300000}" in ble
    assert "ble_worker_retry_delay_ms(ble_state.consecutive_failures)" in worker_body
    degraded_guard = "ble_state.manual_intervention_recommended && !ble_cache_is_fresh()"
    assert degraded_guard in execute_body
    assert execute_body.index(degraded_guard) < execute_body.index("xSemaphoreTake(ble_op_mutex")
    assert "BLE recovery backoff is active; use Cloud fallback." in execute_body
    assert "ble_cache_identity_matches_selection()" in command_cache_delay_body
    assert 'strcmp(ble_state.status, "cached") != 0' in command_cache_delay_body
    assert (
        'strcmp(ble_state.cache_origin, "commandConfirmation") != 0'
        in command_cache_delay_body
    )
    command_cache_skip = (
        "int command_cache_delay_ms = worker_delay_after_recent_command_confirmation();\n"
        "        if (command_cache_delay_ms > 0) {\n"
        "            full_read_delay_ms = command_cache_delay_ms;\n"
        "            continue;\n"
        "        }"
    )
    assert command_cache_skip in worker_body
    assert worker_body.index("hub_command_interactive_pending()") < worker_body.index(
        "worker_delay_after_recent_command_confirmation()"
    ) < worker_body.index("validate_read_internal(&ignored, true)")


def test_large_json_responses_do_not_pin_keep_alive_sockets(source: SourceTree) -> None:
    routes = source.read("main/hub_routes.c")
    send_body = function_body(routes, "hub_routes_send_json")

    assert "HUB_ROUTES_LARGE_JSON_BYTES 1024" in routes
    assert "HUB_ROUTES_JSON_CHUNK_BYTES 1024" in routes
    assert 'httpd_resp_set_hdr(req, "Connection", "close")' in send_body
    assert "httpd_resp_send_chunk" in send_body
    assert "httpd_resp_sendstr" not in send_body


def test_daily_controls_have_system_priority(source: SourceTree) -> None:
    app = source.read("main/app_main.c")
    coordinator = source.read("main/hub_command_coordinator.c")
    coordinator_h = source.read("main/hub_command_coordinator.h")
    cloud = source.read("main/hub_cloud.c")
    ble = source.read("main/hub_ble.c")
    http = source.read("main/hub_http.c")

    assert "interactive_command_queue" in coordinator
    assert "normal_command_queue" in coordinator
    assert "command_is_interactive_priority" in coordinator
    assert "HUB_COMMAND_NORMAL_QUEUE_CAPACITY 3" in coordinator_h
    assert "HUB_COMMAND_INTERACTIVE_QUEUE_CAPACITY 4" in coordinator_h
    assert "inflight_count_for_priority_locked" in coordinator
    assert "hub_command_interactive_pending" in coordinator_h
    assert "HUB_COMMAND_QUICK_WEIGHT" in coordinator_h
    assert "HUB_COMMAND_QUICK_WEIGHT" in function_body(coordinator, "command_is_interactive_priority")
    assert "hub_command_interactive_pending()" in function_body(cloud, "cloud_readonly_io_lock")
    assert "hub_command_interactive_pending()" in function_body(cloud, "cloud_http_request")
    assert "hub_command_interactive_pending()" in function_body(cloud, "cloud_refresh_http_request")
    assert "const int max_attempts = 1" in function_body(cloud, "cloud_refresh_http_request")
    assert "CLOUD_STATISTICS_HTTP_TIMEOUT_MS = 6500" in cloud
    assert "CLOUD_STATISTICS_RETRY_BASE_DELAY_MS = 400" in cloud
    assert "CLOUD_STATISTICS_ENDPOINT_PAUSE_MS = 300" in cloud
    statistics_attempts_body = function_body(cloud, "statistics_http_max_attempts")
    assert "case READONLY_DIAG_TREND:" in statistics_attempts_body
    assert "return 3;" in statistics_attempts_body
    statistics_pause_body = function_body(cloud, "cloud_statistics_pause_between_endpoints")
    assert "hub_command_interactive_pending()" in statistics_pause_body
    assert "cloud_command_is_waiting()" in statistics_pause_body
    assert "CLOUD_STATISTICS_ENDPOINT_PAUSE_MS" in statistics_pause_body
    assert "CLOUD_COMMAND_REFRESH_WAIT_MS = 12000" in cloud
    refresh_body = function_body(cloud, "hub_cloud_refresh")
    assert refresh_body.count("hub_command_interactive_pending()") >= 3
    assert refresh_body.index("hub_command_interactive_pending()") < refresh_body.index("ensure_refresh_mutex()")
    assert "hub_command_interactive_pending()" in function_body(ble, "hub_ble_worker_task")
    execute_body = function_body(ble, "execute_ble_write_json")
    assert "HUB_BLE_COMMAND_SYNC_TIMEOUT_MS = 250" in ble
    assert "HUB_BLE_COMMAND_MUTEX_TIMEOUT_MS = 650" in ble
    assert "wait_sem(ble_sync_sem, sync_timeout_ms)" in execute_body
    assert "wait_sem(ble_sync_sem, 5000)" not in execute_body
    assert "HUB_BLE_COMMAND_MUTEX_TIMEOUT_MS" in execute_body
    assert "HUB_BLE_OPERATION_MUTEX_TIMEOUT_MS" not in execute_body

    assert "queue_ble_diagnostic_request" in http
    assert "httpd_req_async_handler_begin" in http
    assert "httpd_req_async_handler_complete" in http
    assert "discard_async_request_body" in http
    assert "xSemaphoreCreateCounting(1, 1)" in http
    diagnostic_start_body = function_body(http, "start_ble_diagnostic_worker")
    assert "xTaskCreateWithCaps" in diagnostic_start_body
    assert "MALLOC_CAP_SPIRAM" in diagnostic_start_body
    assert "hub_ble_run_worker_job" in function_body(http, "ble_diagnostic_worker_task")
    assert "item.handler(item.request)" not in function_body(http, "ble_diagnostic_worker_task")
    assert "HUB_BLE_WORKER_STACK_BYTES = 12288" in ble
    assert "uxTaskGetStackHighWaterMark" in ble
    worker_body = function_body(ble, "hub_ble_worker_task")
    assert (
        "esp_err_t err = validate_read_internal(&ignored, true);\n"
        "        record_ble_worker_stack_high_water_mark();"
    ) in worker_body
    ble_h = source.read("main/hub_ble.h")
    assert "hub_ble_run_worker_job" in ble_h
    assert "esp_err_t hub_ble_start_worker(void)" in ble_h
    assert "esp_err_t hub_ble_start_worker(void)" in ble
    assert "ble_err = hub_ble_start_worker()" in app
    assert app.index("hub_ble_init()") < app.index("hub_state_keeper_start()")
    assert "ble_diagnostic_should_yield" in ble
    diagnostic_yield_body = function_body(ble, "ble_diagnostic_should_yield")
    assert "hub_command_interactive_pending()" in diagnostic_yield_body
    assert "xTaskGetCurrentTaskHandle() != ble_worker_job_owner" in diagnostic_yield_body
    assert "ble_diagnostic_should_yield()" in function_body(ble, "scan_for_target")
    scan_body = function_body(ble, "scan_for_target")
    assert "ble_scan_teardown_pending" in scan_body
    assert "ble_scan_teardown_pending" in function_body(ble, "prepare_authenticated_connection")
    scan_diagnostic_body = function_body(ble, "hub_ble_scan_diagnostics")
    assert "ble_scan_teardown_pending" in scan_diagnostic_body
    assert "scan_err == ESP_OK || scan_err == ESP_ERR_NOT_FOUND" in scan_diagnostic_body
    assert "scan_err == ESP_ERR_NOT_FOUND ? ESP_OK : scan_err" in scan_diagnostic_body
    for diagnostic in (
        "validate_read_internal",
        "hub_ble_full_read_probe_json",
        "hub_ble_gatt_read_probe_json",
        "hub_ble_notify_probe_json",
        "hub_ble_protocol_probe_json",
        "hub_ble_scan_diagnostics",
    ):
        assert "ble_diagnostic_should_yield()" in function_body(ble, diagnostic)
    health_body = function_body(http, "health_handler")
    assert "resetReason" in health_body
    assert "bleDiagnosticStackHighWaterMark" in health_body
    assert "firmwareBuildId" in health_body
    assert "firmwareBuildIdAlgorithm" in health_body
    assert "esp_app_get_elf_sha256_str()" in health_body
    assert "hub_command_interactive_pending()" in function_body(http, "queue_ble_diagnostic_request")
    diagnostic_worker_body = function_body(http, "ble_diagnostic_worker_task")
    assert "hub_command_interactive_pending()" in diagnostic_worker_body
    assert "discard_async_request_body(item.request)" in diagnostic_worker_body
    diagnostic_queue_body = function_body(http, "queue_ble_diagnostic_request")
    assert "discard_async_request_body(request_copy)" in diagnostic_queue_body
    for handler in (
        "ble_validate_read_handler",
        "ble_full_read_handler",
        "ble_notify_probe_handler",
        "ble_protocol_probe_handler",
        "ble_gatt_read_probe_handler",
        "ble_scan_handler",
    ):
        assert "queue_ble_diagnostic_request" in function_body(http, handler)

    assert (
        'register_uri(server, "/controls/quick-weight"' in http
        or '{"/controls/quick-weight", HTTP_POST, quick_weight_handler' in http
    )
    control = source.read("main/hub_control.c")
    quick_weight_wrapper = function_body(control, "hub_control_execute_quick_weight")
    assert "hub_control_execute_quick_weight_with_deadline" in quick_weight_wrapper
    assert "NULL" in quick_weight_wrapper
    quick_weight_body = function_body(
        control,
        "hub_control_execute_quick_weight_with_deadline",
    )
    assert 'hub_cloud_set_bbw_mode("Dose1"' in quick_weight_body
    assert 'hub_cloud_set_bbw_dose("dose1", grams' in quick_weight_body


def main() -> None:
    source = SourceTree(Path(__file__).resolve().parents[1])
    test_runtime_resource_truth(source)
    test_state_reads_are_pure(source)
    test_command_history_uses_psram(source)
    test_cloud_refresh_requests_use_the_keeper(source)
    test_commands_are_accepted_without_holding_http_connections(source)
    test_commands_are_bound_to_one_machine_selection(source)
    test_statistics_candidates_are_atomic(source)
    test_degraded_ble_recovery_does_not_block_controls(source)
    test_large_json_responses_do_not_pin_keep_alive_sockets(source)
    test_daily_controls_have_system_priority(source)
    print("stability foundation source guards: PASS")


if __name__ == "__main__":
    main()
