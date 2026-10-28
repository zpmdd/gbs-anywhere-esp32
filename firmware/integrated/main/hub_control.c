#include "hub_control.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "hub_ble.h"
#include "hub_cloud.h"
#include "hub_command_coordinator.h"
#include "hub_state.h"

static int now_ms(void)
{
    return (int)(esp_timer_get_time() / 1000);
}

static int ascii_tolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        const char *hay = cursor;
        const char *term = needle;
        while (*hay != '\0' && *term != '\0' && ascii_tolower((unsigned char)*hay) == ascii_tolower((unsigned char)*term)) {
            ++hay;
            ++term;
        }
        if (*term == '\0') {
            return true;
        }
    }
    return false;
}

static bool contains_sensitive_word(const char *text)
{
    static const char *const sensitive_words[] = {
        "ble" "Auth" "Token",
        "installation" "Key",
        "refresh" "Token",
        "hub" "Token",
        "pass" "word",
    };
    for (size_t i = 0; i < sizeof(sensitive_words) / sizeof(sensitive_words[0]); ++i) {
        if (contains_case_insensitive(text, sensitive_words[i])) {
            return true;
        }
    }
    return false;
}

static void json_safe_copy(char *buffer, size_t buffer_len, const char *value, bool redact_sensitive)
{
    const char *source = value == NULL ? "" : value;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (redact_sensitive && contains_sensitive_word(source)) {
        source = "[redacted]";
    }

    size_t written = 0;
    for (const unsigned char *cursor = (const unsigned char *)source; *cursor != '\0'; ++cursor) {
        char escaped[7] = {0};
        const char *chunk = escaped;
        size_t chunk_len = 0;

        switch (*cursor) {
        case '"':
            chunk = "\\\"";
            chunk_len = 2;
            break;
        case '\\':
            chunk = "\\\\";
            chunk_len = 2;
            break;
        case '\b':
            chunk = "\\b";
            chunk_len = 2;
            break;
        case '\f':
            chunk = "\\f";
            chunk_len = 2;
            break;
        case '\n':
            chunk = "\\n";
            chunk_len = 2;
            break;
        case '\r':
            chunk = "\\r";
            chunk_len = 2;
            break;
        case '\t':
            chunk = "\\t";
            chunk_len = 2;
            break;
        default:
            if (iscntrl(*cursor)) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                chunk_len = 6;
            } else {
                escaped[0] = (char)*cursor;
                chunk_len = 1;
            }
            break;
        }

        if (written + chunk_len >= buffer_len) {
            break;
        }
        memcpy(buffer + written, chunk, chunk_len);
        written += chunk_len;
    }
    buffer[written] = '\0';
}

const char *hub_control_transport_string(hub_control_transport_t transport)
{
    switch (transport) {
    case HUB_CONTROL_TRANSPORT_BLE:
        return "ble";
    case HUB_CONTROL_TRANSPORT_CLOUD:
        return "cloud";
    case HUB_CONTROL_TRANSPORT_LOCAL:
        return "local";
    case HUB_CONTROL_TRANSPORT_NONE:
    default:
        return "none";
    }
}

const char *hub_control_risk_string(hub_control_risk_t risk)
{
    switch (risk) {
    case HUB_CONTROL_RISK_LOW:
        return "low";
    case HUB_CONTROL_RISK_MEDIUM:
        return "medium";
    case HUB_CONTROL_RISK_HIGH:
        return "high";
    default:
        return "medium";
    }
}

static void set_result(
    hub_control_result_t *result,
    bool ok,
    const char *control,
    hub_control_transport_t transport,
    hub_control_transport_t preferred_transport,
    hub_control_transport_t fallback_transport,
    bool fallback_used,
    hub_control_risk_t risk,
    bool requires_confirmation,
    int started_ms,
    const char *message
)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->ok = ok;
    snprintf(result->control, sizeof(result->control), "%s", control == NULL ? "" : control);
    result->transport = transport;
    result->preferred_transport = preferred_transport;
    result->fallback_transport = fallback_transport;
    result->fallback_used = fallback_used;
    result->risk = risk;
    result->requires_confirmation = requires_confirmation;
    result->duration_ms = now_ms() - started_ms;
    snprintf(result->message, sizeof(result->message), "%s", message == NULL ? "" : message);
}

static void set_ble_diagnostics(
    hub_control_result_t *result,
    bool attempted,
    const hub_ble_command_result_t *ble_result,
    const char *error
)
{
    if (result == NULL) {
        return;
    }
    result->ble_attempted = attempted;
    result->ble_duration_ms = ble_result != NULL ? ble_result->duration_ms : 0;
    result->upstream_attempts += ble_result != NULL ? ble_result->attempts : 0;
    if (ble_result != NULL) {
        result->write_accepted = ble_result->write_accepted;
        result->state_confirmed = ble_result->state_confirmed;
        result->state_confirmation_pending = ble_result->state_confirmation_pending;
        result->late_side_effect_risk = ble_result->late_side_effect_risk;
        result->ble_pre_write_ms = ble_result->pre_write_ms;
        result->ble_write_status_ms = ble_result->write_status_ms;
        result->state_confirmation_ms = ble_result->state_confirmation_ms;
        result->confirmation_read_count = ble_result->confirmation_read_count;
        result->confirmation_selector_write_count = ble_result->confirmation_selector_write_count;
        result->confirmation_direct_read_count = ble_result->confirmation_direct_read_count;
        result->confirmation_direct_first = ble_result->confirmation_direct_first;
        result->confirmation_reprime_count = ble_result->confirmation_reprime_count;
        result->ble_observation_sequence = ble_result->observation_sequence;
        if (ble_result->error_code[0] != '\0') {
            snprintf(
                result->error_code,
                sizeof(result->error_code),
                "%s",
                ble_result->error_code
            );
        }
    }
    snprintf(
        result->ble_error,
        sizeof(result->ble_error),
        "%s",
        error != NULL ? error : ""
    );
}

static void set_cloud_diagnostics(
    hub_control_result_t *result,
    bool attempted,
    const hub_cloud_command_result_t *cloud_result
)
{
    if (result == NULL) {
        return;
    }
    result->cloud_attempted = attempted;
    result->cloud_duration_ms = cloud_result != NULL ? cloud_result->duration_ms : 0;
    result->upstream_attempts += cloud_result != NULL ? cloud_result->upstream_attempts : 0;
}

static void set_fallback_reason(hub_control_result_t *result, const char *reason)
{
    if (result == NULL) {
        return;
    }
    snprintf(
        result->fallback_reason,
        sizeof(result->fallback_reason),
        "%s",
        reason != NULL ? reason : ""
    );
}

void hub_control_result_json(const hub_control_result_t *result, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (result == NULL) {
        snprintf(buffer, buffer_len, "{\"ok\":false,\"message\":\"missing result\"}");
        return;
    }
    char control[96];
    char message[256];
    char error_code[64];
    char ble_error[192];
    char fallback_reason[192];
    char api_prefix[32] = "";
    char deadline_fields[128] = "";
    json_safe_copy(control, sizeof(control), result->control, true);
    json_safe_copy(message, sizeof(message), result->message, true);
    json_safe_copy(error_code, sizeof(error_code), result->error_code, true);
    json_safe_copy(ble_error, sizeof(ble_error), result->ble_error, true);
    json_safe_copy(fallback_reason, sizeof(fallback_reason), result->fallback_reason, true);
    if (result->api_version == HUB_API_V2_VERSION) {
        snprintf(api_prefix, sizeof(api_prefix), "\"apiVersion\":2,");
        snprintf(
            deadline_fields,
            sizeof(deadline_fields),
            "\"status\":\"%s\",\"acceptedTtlMs\":%d,\"remainingMs\":%d,",
            hub_api_v2_command_status_string(result->v2_status),
            result->accepted_ttl_ms,
            result->remaining_ms
        );
    }
    snprintf(
        buffer,
        buffer_len,
        "{%s\"ok\":%s,\"requestId\":\"%s\",\"clientRequestId\":%s,\"pending\":%s,%s"
        "\"deduplicated\":%s,\"upstreamAttempts\":%d,\"queueDelayMs\":%d,"
        "\"errorCode\":\"%s\",\"control\":\"%s\","
        "\"transport\":\"%s\",\"preferredTransport\":\"%s\","
        "\"fallbackTransport\":\"%s\",\"fallbackUsed\":%s,\"durationMs\":%d,\"risk\":\"%s\","
        "\"requiresConfirmation\":%s,\"writeAccepted\":%s,\"stateConfirmed\":%s,"
        "\"stateConfirmationPending\":%s,\"blePreWriteMs\":%d,"
        "\"bleWriteStatusMs\":%d,\"stateConfirmationMs\":%d,"
        "\"confirmationReadCount\":%d,\"confirmationSelectorWriteCount\":%d,"
        "\"confirmationDirectReadCount\":%d,\"confirmationDirectFirst\":%s,"
        "\"confirmationReprimeCount\":%d,"
        "\"lateSideEffectRisk\":%s,\"bleObservationSequence\":%lu,"
        "\"bleAttempted\":%s,\"bleDurationMs\":%d,"
        "\"bleError\":\"%s\",\"cloudAttempted\":%s,\"cloudDurationMs\":%d,"
        "\"fallbackReason\":\"%s\",\"message\":\"%s\"}",
        api_prefix,
        result->ok ? "true" : "false",
        result->request_id,
        result->client_request_id ? "true" : "false",
        result->pending ? "true" : "false",
        deadline_fields,
        result->deduplicated ? "true" : "false",
        result->upstream_attempts,
        result->queue_delay_ms,
        error_code,
        control,
        hub_control_transport_string(result->transport),
        hub_control_transport_string(result->preferred_transport),
        hub_control_transport_string(result->fallback_transport),
        result->fallback_used ? "true" : "false",
        result->duration_ms,
        hub_control_risk_string(result->risk),
        result->requires_confirmation ? "true" : "false",
        result->write_accepted ? "true" : "false",
        result->state_confirmed ? "true" : "false",
        result->state_confirmation_pending ? "true" : "false",
        result->ble_pre_write_ms,
        result->ble_write_status_ms,
        result->state_confirmation_ms,
        result->confirmation_read_count,
        result->confirmation_selector_write_count,
        result->confirmation_direct_read_count,
        result->confirmation_direct_first ? "true" : "false",
        result->confirmation_reprime_count,
        result->late_side_effect_risk ? "true" : "false",
        (unsigned long)result->ble_observation_sequence,
        result->ble_attempted ? "true" : "false",
        result->ble_duration_ms,
        ble_error,
        result->cloud_attempted ? "true" : "false",
        result->cloud_duration_ms,
        fallback_reason,
        message
    );
}

static esp_err_t execute_schedule(const hub_command_request_t *request,hub_control_result_t *result);

esp_err_t hub_control_execute_request(
    const struct hub_command_request *raw_request,
    hub_control_result_t *result
)
{
    const hub_command_request_t *request = raw_request;
    if (request == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (request->kind) {
    case HUB_COMMAND_SCHEDULE:
        return execute_schedule(request,result);
    case HUB_COMMAND_QUICK_WEIGHT:
        return hub_control_execute_quick_weight_checked_with_deadline(
            request->value.quick_weight.grams,
            request->value.quick_weight.require_no_stored_match,
            &request->deadline,
            result
        );
    case HUB_COMMAND_BBW_DOSE:
        return hub_control_execute_bbw_dose(
            request->value.bbw_dose.dose,
            request->value.bbw_dose.grams,
            result
        );
    case HUB_COMMAND_BBW_MODE:
        if (request->value.bbw_mode.has_preset_grams) {
            return hub_control_execute_bbw_mode_checked_with_deadline(
                request->value.bbw_mode.mode,
                request->value.bbw_mode.preset_grams,
                &request->deadline,
                result
            );
        }
        return hub_control_execute_bbw_mode(request->value.bbw_mode.mode, result);
    case HUB_COMMAND_PREBREW:
        return hub_control_execute_prebrew_with_deadline(
            request->value.prebrew.field,
            request->value.prebrew.value,
            &request->deadline,
            result
        );
    case HUB_COMMAND_COFFEE_TEMPERATURE:
        return hub_control_execute_coffee_temperature(request->value.temperature.temperature, result);
    case HUB_COMMAND_STEAM_LEVEL:
        return hub_control_execute_steam_level(request->value.steam_level.level, result);
    case HUB_COMMAND_STEAM_ENABLED:
        return hub_control_execute_steam_enabled(request->value.enabled.enabled, result);
    case HUB_COMMAND_STEAM_TEMPERATURE:
        return hub_control_execute_steam_temperature(request->value.temperature.temperature, result);
    case HUB_COMMAND_SMART_STANDBY:
        return hub_control_execute_smart_standby(
            request->value.smart_standby.enabled,
            request->value.smart_standby.minutes,
            request->value.smart_standby.mode,
            request->value.smart_standby.confirm,
            result
        );
    case HUB_COMMAND_POWER:
        return hub_control_execute_power(
            request->value.power.enabled,
            request->value.power.confirm,
            result
        );
    case HUB_COMMAND_BACKFLUSH:
        return hub_control_execute_backflush(request->value.confirmation.confirm, result);
    case HUB_COMMAND_MACHINE_FIRMWARE_UPDATE:
        return hub_control_execute_machine_firmware_update(
            request->value.confirmation.confirm,
            result
        );
    }
    return ESP_ERR_INVALID_ARG;
}

static bool should_try_cloud_after_ble(esp_err_t err, const hub_ble_command_result_t *ble_result)
{
    /* BLE first; Cloud fallback is safe only when no BLE write was started. */
    return err != ESP_OK
        && ble_result != NULL
        && !ble_result->ok
        && !ble_result->guard_rejected
        && !ble_result->request_started;
}

static void record_control_result(const hub_control_result_t *result)
{
    if (result == NULL) {
        return;
    }
    hub_state_record_command(
        result->control,
        hub_control_transport_string(result->transport),
        result->ok,
        result->duration_ms,
        result->message
    );
}

static esp_err_t confirmation_required_result(
    const char *control,
    hub_control_transport_t preferred,
    hub_control_result_t *result
)
{
    int started = now_ms();
    set_result(
        result,
        false,
        control,
        HUB_CONTROL_TRANSPORT_NONE,
        preferred,
        HUB_CONTROL_TRANSPORT_NONE,
        false,
        HUB_CONTROL_RISK_HIGH,
        true,
        started,
        "This high-risk control requires confirm=true."
    );
    set_ble_diagnostics(result, false, NULL, "");
    set_cloud_diagnostics(result, false, NULL);
    record_control_result(result);
    return ESP_ERR_INVALID_ARG;
}


static bool cloud_command_outcome_is_unknown(
    esp_err_t error,
    const hub_cloud_command_result_t *result
)
{
    if (result == NULL) {
        return false;
    }
    // Match phone Cloud: only a completed authorization rejection proves no apply.
    const bool rejected = error == ESP_ERR_INVALID_RESPONSE
        && (result->http_status == 401 || result->http_status == 403);
    return result->request_started && !result->ok && !rejected;
}

static esp_err_t finish_ble_first_result(
    const char *control,
    esp_err_t ble_err,
    const hub_ble_command_result_t *ble_result,
    esp_err_t cloud_err,
    const hub_cloud_command_result_t *cloud_result,
    bool cloud_attempted,
    hub_control_risk_t risk,
    bool requires_confirmation,
    int started,
    hub_control_result_t *result
)
{
    if (ble_err == ESP_OK && ble_result != NULL && ble_result->ok) {
        set_result(
            result,
            true,
            control,
            HUB_CONTROL_TRANSPORT_BLE,
            HUB_CONTROL_TRANSPORT_BLE,
            HUB_CONTROL_TRANSPORT_CLOUD,
            false,
            risk,
            requires_confirmation,
            started,
            ble_result->message[0] != '\0' ? ble_result->message : "BLE command completed."
        );
        set_ble_diagnostics(result, true, ble_result, "");
        set_cloud_diagnostics(result, false, NULL);
        record_control_result(result);
        return ESP_OK;
    }

    const char *ble_error = ble_result != NULL && ble_result->message[0] != '\0'
        ? ble_result->message
        : esp_err_to_name(ble_err);

    if (cloud_attempted && cloud_err == ESP_OK && cloud_result != NULL && cloud_result->ok) {
        char message[192] = {0};
        snprintf(
            message,
            sizeof(message),
            "BLE failed: %.64s Cloud fallback succeeded: %.64s",
            ble_error,
            cloud_result->message[0] != '\0' ? cloud_result->message : "Cloud command completed."
        );
        set_result(
            result,
            true,
            control,
            HUB_CONTROL_TRANSPORT_CLOUD,
            HUB_CONTROL_TRANSPORT_BLE,
            HUB_CONTROL_TRANSPORT_CLOUD,
            true,
            risk,
            requires_confirmation,
            started,
            message
        );
        set_ble_diagnostics(result, true, ble_result, ble_error);
        set_cloud_diagnostics(result, true, cloud_result);
        set_fallback_reason(result, ble_error);
        record_control_result(result);
        return ESP_OK;
    }

    char message[192] = {0};
    if (cloud_attempted) {
        snprintf(
            message,
            sizeof(message),
            "BLE failed: %.64s Cloud failed: %.64s",
            ble_error,
            cloud_result != NULL && cloud_result->message[0] != '\0' ? cloud_result->message : esp_err_to_name(cloud_err)
        );
    } else {
        snprintf(
            message,
            sizeof(message),
            "BLE failed: %s",
            ble_error
        );
    }
    set_result(
        result,
        false,
        control,
        ble_result != NULL && ble_result->request_started
            ? HUB_CONTROL_TRANSPORT_BLE
            : HUB_CONTROL_TRANSPORT_NONE,
        HUB_CONTROL_TRANSPORT_BLE,
        HUB_CONTROL_TRANSPORT_CLOUD,
        cloud_attempted,
        risk,
        requires_confirmation,
        started,
        message
    );
    set_ble_diagnostics(result, true, ble_result, ble_error);
    set_cloud_diagnostics(result, cloud_attempted, cloud_result);
    set_fallback_reason(result, cloud_attempted ? ble_error : "");
    if (cloud_attempted && cloud_command_outcome_is_unknown(cloud_err, cloud_result)) {
        result->transport = HUB_CONTROL_TRANSPORT_CLOUD;
        result->state_confirmation_pending = true;
        result->late_side_effect_risk = true;
        result->v2_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
        snprintf(result->error_code, sizeof(result->error_code), "unknownOutcome");
    }
    record_control_result(result);
    if (ble_err == ESP_ERR_INVALID_ARG || cloud_err == ESP_ERR_INVALID_ARG) {
        return ESP_ERR_INVALID_ARG;
    }
    return cloud_attempted ? cloud_err : ble_err;
}

static esp_err_t finish_cloud_primary_result(
    const char *control,
    esp_err_t cloud_err,
    const hub_cloud_command_result_t *cloud_result,
    hub_control_risk_t risk,
    bool requires_confirmation,
    int started,
    hub_control_result_t *result
)
{
    const bool ok = cloud_err == ESP_OK && cloud_result != NULL && cloud_result->ok;
    const char *message = cloud_result != NULL && cloud_result->message[0] != '\0'
        ? cloud_result->message
        : (ok ? "Cloud command completed." : esp_err_to_name(cloud_err));

    set_result(
        result,
        ok,
        control,
        ok ? HUB_CONTROL_TRANSPORT_CLOUD : HUB_CONTROL_TRANSPORT_NONE,
        HUB_CONTROL_TRANSPORT_CLOUD,
        HUB_CONTROL_TRANSPORT_NONE,
        false,
        risk,
        requires_confirmation,
        started,
        message
    );
    set_ble_diagnostics(result, false, NULL, "");
    set_cloud_diagnostics(result, true, cloud_result);
    if (cloud_command_outcome_is_unknown(cloud_err, cloud_result)) {
        result->transport = HUB_CONTROL_TRANSPORT_CLOUD;
        result->state_confirmation_pending = true;
        result->late_side_effect_risk = true;
        result->v2_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
        snprintf(result->error_code, sizeof(result->error_code), "unknownOutcome");
    } else if (cloud_result != NULL && cloud_result->preset_changed) {
        result->response_http_status = 200;
        result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
        snprintf(result->error_code, sizeof(result->error_code), "presetChanged");
    }
    record_control_result(result);

    if (ok) {
        return ESP_OK;
    }
    if (cloud_err == ESP_ERR_INVALID_ARG) {
        return ESP_ERR_INVALID_ARG;
    }
    return cloud_err != ESP_OK ? cloud_err : ESP_FAIL;
}

esp_err_t hub_control_execute_bbw_dose(const char *dose, int grams, hub_control_result_t *result)
{
    int started = now_ms();
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = hub_cloud_set_bbw_dose(dose, grams, &cloud_result);
    return finish_cloud_primary_result("brewByWeightDose", cloud_err, &cloud_result, HUB_CONTROL_RISK_MEDIUM, false, started, result);
}

esp_err_t hub_control_execute_quick_weight(int grams, hub_control_result_t *result)
{
    return hub_control_execute_quick_weight_with_deadline(grams, NULL, result);
}

static esp_err_t deadline_expired_before_side_effect(
    const char *control,
    int started,
    hub_control_result_t *result
)
{
    set_result(
        result,
        false,
        control,
        HUB_CONTROL_TRANSPORT_NONE,
        HUB_CONTROL_TRANSPORT_CLOUD,
        HUB_CONTROL_TRANSPORT_NONE,
        false,
        HUB_CONTROL_RISK_MEDIUM,
        false,
        started,
        "Command deadline has insufficient time for the next upstream step."
    );
    result->response_http_status = 200;
    result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
    snprintf(result->error_code, sizeof(result->error_code), "commandExpired");
    set_ble_diagnostics(result, false, NULL, "");
    set_cloud_diagnostics(result, false, NULL);
    record_control_result(result);
    return ESP_ERR_TIMEOUT;
}

esp_err_t hub_control_execute_quick_weight_with_deadline(
    int grams,
    const hub_command_deadline_t *deadline,
    hub_control_result_t *result
)
{
    return hub_control_execute_quick_weight_checked_with_deadline(grams, false, deadline, result);
}


esp_err_t hub_control_execute_quick_weight_checked_with_deadline(
    int grams,
    bool require_no_stored_match,
    const hub_command_deadline_t *deadline,
    hub_control_result_t *result
)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int started = now_ms();
    if (grams < 0 || grams > 200) {
        set_result(
            result,
            false,
            "quickWeight",
            HUB_CONTROL_TRANSPORT_NONE,
            HUB_CONTROL_TRANSPORT_CLOUD,
            HUB_CONTROL_TRANSPORT_NONE,
            false,
            HUB_CONTROL_RISK_MEDIUM,
            false,
            started,
            "Quick weight must be between 0 and 200 grams."
        );
        set_ble_diagnostics(result, false, NULL, "");
        set_cloud_diagnostics(result, false, NULL);
        record_control_result(result);
        return ESP_ERR_INVALID_ARG;
    }
    if (!hub_command_deadline_can_start_step(
            deadline,
            now_ms(),
            HUB_COMMAND_DEADLINE_QUICK_WEIGHT_WORK_RESERVE_MS
        )) {
        return deadline_expired_before_side_effect("quickWeight", started, result);
    }

    // The Cloud helper keeps both writes under one lock and reuses its fresh snapshot and HTTP session.
    hub_cloud_command_result_t mode_result = {0}, dose_result = {0};
    esp_err_t cloud_err = hub_cloud_set_quick_weight_with_deadline(
        grams, require_no_stored_match, deadline, &mode_result, &dose_result
    );
    esp_err_t mode_err = mode_result.ok ? ESP_OK : cloud_err;
    if (mode_err != ESP_OK || !mode_result.ok) {
        set_result(
            result,
            false,
            "quickWeight",
            HUB_CONTROL_TRANSPORT_NONE,
            HUB_CONTROL_TRANSPORT_CLOUD,
            HUB_CONTROL_TRANSPORT_NONE,
            false,
            HUB_CONTROL_RISK_MEDIUM,
            false,
            started,
            mode_result.message[0] != '\0'
                ? mode_result.message
                : "Quick-weight mode restore failed."
        );
        set_ble_diagnostics(result, false, NULL, "");
        set_cloud_diagnostics(result, true, &mode_result);
        const bool unknown_outcome = cloud_command_outcome_is_unknown(mode_err, &mode_result);
        result->state_confirmation_pending = unknown_outcome;
        result->late_side_effect_risk = unknown_outcome;
        if (mode_result.preset_changed) {
            result->response_http_status = 200;
            result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
            snprintf(result->error_code, sizeof(result->error_code), "presetChanged");
        } else if (unknown_outcome) {
            result->transport = HUB_CONTROL_TRANSPORT_CLOUD;
            result->v2_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
            snprintf(result->error_code, sizeof(result->error_code), "unknownOutcome");
        } else if (deadline != NULL && deadline->enforced) {
            result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
            snprintf(
                result->error_code,
                sizeof(result->error_code),
                "%s",
                mode_err == ESP_ERR_TIMEOUT
                    ? "commandExpired"
                    : "cloudCommandUnavailable"
            );
        }
        if (deadline != NULL && deadline->enforced) {
            result->response_http_status = 200;
        }
        record_control_result(result);
        return mode_err != ESP_OK ? mode_err : ESP_FAIL;
    }

    esp_err_t dose_err = cloud_err;
    const bool ok = dose_err == ESP_OK && dose_result.ok;
    const bool partial_apply = !ok && mode_result.request_started;
    const bool unknown_outcome = cloud_command_outcome_is_unknown(dose_err, &dose_result);
    char message[192] = {0};
    if (ok) {
        snprintf(
            message,
            sizeof(message),
            "Quick weight applied as one serialized command. Mode: %.64s Dose1: %.64s",
            mode_result.message,
            dose_result.message
        );
    } else {
        snprintf(
            message,
            sizeof(message),
            "Quick-weight mode was restored, but Dose1 failed: %.96s",
            dose_result.message[0] != '\0' ? dose_result.message : esp_err_to_name(dose_err)
        );
    }
    set_result(
        result,
        ok,
        "quickWeight",
        ok || partial_apply ? HUB_CONTROL_TRANSPORT_CLOUD : HUB_CONTROL_TRANSPORT_NONE,
        HUB_CONTROL_TRANSPORT_CLOUD,
        HUB_CONTROL_TRANSPORT_NONE,
        false,
        HUB_CONTROL_RISK_MEDIUM,
        false,
        started,
        message
    );
    set_ble_diagnostics(result, false, NULL, "");
    result->cloud_attempted = true;
    result->cloud_duration_ms = mode_result.duration_ms + dose_result.duration_ms;
    result->upstream_attempts = mode_result.upstream_attempts + dose_result.upstream_attempts;
    result->state_confirmation_pending = unknown_outcome;
    result->late_side_effect_risk = unknown_outcome;
    if (partial_apply) {
        snprintf(result->error_code, sizeof(result->error_code), "partialApply");
        result->v2_status = HUB_API_V2_COMMAND_STATUS_PARTIAL_APPLY;
    } else if (unknown_outcome) {
        result->transport = HUB_CONTROL_TRANSPORT_CLOUD;
        snprintf(result->error_code, sizeof(result->error_code), "unknownOutcome");
        result->v2_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
    } else if (deadline != NULL && deadline->enforced) {
        result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
        if (!ok) {
            snprintf(
                result->error_code,
                sizeof(result->error_code),
                "%s",
                dose_err == ESP_ERR_TIMEOUT
                    ? "commandExpired"
                    : "cloudCommandUnavailable"
            );
        }
    }
    if (deadline != NULL && deadline->enforced) {
        result->response_http_status = 200;
    }
    record_control_result(result);

    if (ok) {
        return ESP_OK;
    }
    return dose_err != ESP_OK ? dose_err : ESP_FAIL;
}


esp_err_t hub_control_execute_bbw_mode_checked_with_deadline(
    const char *mode,
    int preset_grams,
    const hub_command_deadline_t *deadline,
    hub_control_result_t *result
)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int started = now_ms();
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = hub_cloud_set_bbw_mode_checked_with_deadline(
        mode, preset_grams, false, deadline, 0, &cloud_result
    );
    esp_err_t result_err = finish_cloud_primary_result(
        "brewByWeightMode", cloud_err, &cloud_result, HUB_CONTROL_RISK_MEDIUM, false, started, result
    );
    if (deadline != NULL && deadline->enforced) {
        result->response_http_status = 200;
        if (!cloud_command_outcome_is_unknown(cloud_err, &cloud_result)) {
            result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
            if (!result->ok && result->error_code[0] == '\0') {
                snprintf(result->error_code, sizeof(result->error_code), "%s",
                         cloud_err == ESP_ERR_TIMEOUT ? "commandExpired" : "cloudCommandUnavailable");
            }
        }
    }
    return result_err;
}

esp_err_t hub_control_execute_bbw_mode(const char *mode, hub_control_result_t *result)
{
    int started = now_ms();
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = hub_cloud_set_bbw_mode(mode, &cloud_result);
    return finish_cloud_primary_result("brewByWeightMode", cloud_err, &cloud_result, HUB_CONTROL_RISK_MEDIUM, false, started, result);
}

esp_err_t hub_control_execute_prebrew(const char *field, const char *value, hub_control_result_t *result)
{
    return hub_control_execute_prebrew_with_deadline(field, value, NULL, result);
}

esp_err_t hub_control_execute_prebrew_with_deadline(
    const char *field,
    const char *value,
    const hub_command_deadline_t *deadline,
    hub_control_result_t *result
)
{
    int started = now_ms();
    if (!hub_command_deadline_can_start_step(
            deadline,
            now_ms(),
            HUB_COMMAND_DEADLINE_PREBREW_WORK_RESERVE_MS
        )) {
        return deadline_expired_before_side_effect("prebrew", started, result);
    }
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = deadline != NULL && deadline->enforced
        ? hub_cloud_set_prebrew_with_deadline(
            field,
            value,
            deadline,
            0,
            &cloud_result
        )
        : hub_cloud_set_prebrew(field, value, &cloud_result);
    esp_err_t result_err = finish_cloud_primary_result(
        "prebrew",
        cloud_err,
        &cloud_result,
        HUB_CONTROL_RISK_MEDIUM,
        false,
        started,
        result
    );
    if (deadline != NULL && deadline->enforced) {
        result->response_http_status = 200;
        if (result_err != ESP_OK && cloud_result.request_started) {
            result->transport = HUB_CONTROL_TRANSPORT_CLOUD;
            result->v2_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
            snprintf(result->error_code, sizeof(result->error_code), "unknownOutcome");
        } else {
            result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
            if (result_err != ESP_OK) {
                snprintf(
                    result->error_code,
                    sizeof(result->error_code),
                    "%s",
                    result_err == ESP_ERR_TIMEOUT
                        ? "commandExpired"
                        : (result_err == ESP_ERR_INVALID_ARG
                            ? "invalidArgument"
                            : "cloudCommandUnavailable")
                );
            }
        }
    }
    return result_err;
}

esp_err_t hub_control_execute_coffee_temperature(double temperature, hub_control_result_t *result)
{
    int started = now_ms();
    hub_ble_command_result_t ble_result = {0};
    esp_err_t ble_err = hub_ble_set_coffee_temperature(temperature, &ble_result);
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = ESP_FAIL;
    bool cloud_attempted = should_try_cloud_after_ble(ble_err, &ble_result);
    if (cloud_attempted) {
        cloud_err = hub_cloud_set_coffee_temperature(temperature, &cloud_result);
    }
    return finish_ble_first_result("coffeeTemperature", ble_err, &ble_result, cloud_err, &cloud_result, cloud_attempted, HUB_CONTROL_RISK_MEDIUM, false, started, result);
}

esp_err_t hub_control_execute_steam_level(int level, hub_control_result_t *result)
{
    int started = now_ms();
    if (level < 1 || level > 3) {
        set_result(result, false, "steamLevel", HUB_CONTROL_TRANSPORT_NONE, HUB_CONTROL_TRANSPORT_BLE, HUB_CONTROL_TRANSPORT_CLOUD, false, HUB_CONTROL_RISK_MEDIUM, false, started, "Steam level must be 1, 2, or 3.");
        set_ble_diagnostics(result, false, NULL, "");
        set_cloud_diagnostics(result, false, NULL);
        record_control_result(result);
        return ESP_ERR_INVALID_ARG;
    }
    const double steam_temperature = level == 1 ? 126.0 : (level == 2 ? 128.0 : 131.0);
    hub_ble_command_result_t ble_result = {0};
    esp_err_t ble_err = hub_ble_set_steam_temperature(steam_temperature, &ble_result);
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = ESP_FAIL;
    bool cloud_attempted = should_try_cloud_after_ble(ble_err, &ble_result);
    if (cloud_attempted) {
        cloud_err = hub_cloud_set_steam_level(level, &cloud_result);
    }
    return finish_ble_first_result("steamLevel", ble_err, &ble_result, cloud_err, &cloud_result, cloud_attempted, HUB_CONTROL_RISK_MEDIUM, false, started, result);
}

esp_err_t hub_control_execute_steam_enabled(bool enabled, hub_control_result_t *result)
{
    int started = now_ms();
    hub_ble_command_result_t ble_result = {0};
    esp_err_t ble_err = hub_ble_set_steam_enabled(enabled, &ble_result);
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = ESP_FAIL;
    bool cloud_attempted = should_try_cloud_after_ble(ble_err, &ble_result);
    if (cloud_attempted) {
        cloud_err = hub_cloud_set_steam_enabled(enabled, &cloud_result);
    }
    return finish_ble_first_result("steamEnabled", ble_err, &ble_result, cloud_err, &cloud_result, cloud_attempted, HUB_CONTROL_RISK_MEDIUM, false, started, result);
}

esp_err_t hub_control_execute_steam_temperature(double temperature, hub_control_result_t *result)
{
    int started = now_ms();
    hub_ble_command_result_t ble_result = {0};
    esp_err_t ble_err = hub_ble_set_steam_temperature(temperature, &ble_result);
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = ESP_FAIL;
    bool cloud_attempted = should_try_cloud_after_ble(ble_err, &ble_result);
    if (cloud_attempted) {
        cloud_err = hub_cloud_set_steam_temperature(temperature, &cloud_result);
    }
    return finish_ble_first_result("steamTemperature", ble_err, &ble_result, cloud_err, &cloud_result, cloud_attempted, HUB_CONTROL_RISK_MEDIUM, false, started, result);
}

esp_err_t hub_control_execute_smart_standby(bool enabled, int minutes, const char *mode, bool confirm, hub_control_result_t *result)
{
    if (!confirm) {
        return confirmation_required_result("smartStandby", HUB_CONTROL_TRANSPORT_BLE, result);
    }
    int started = now_ms();
    hub_ble_command_result_t ble_result = {0};
    esp_err_t ble_err = hub_ble_set_smart_standby(enabled, minutes, mode, &ble_result);
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = ESP_FAIL;
    bool cloud_attempted = should_try_cloud_after_ble(ble_err, &ble_result);
    if (cloud_attempted) {
        cloud_err = hub_cloud_set_smart_standby(enabled, minutes, mode, &cloud_result);
    }
    return finish_ble_first_result("smartStandby", ble_err, &ble_result, cloud_err, &cloud_result, cloud_attempted, HUB_CONTROL_RISK_HIGH, true, started, result);
}

esp_err_t hub_control_execute_power(bool enabled, bool confirm, hub_control_result_t *result)
{
    if (!confirm) {
        return confirmation_required_result("power", HUB_CONTROL_TRANSPORT_BLE, result);
    }
    int started = now_ms();
    hub_ble_command_result_t ble_result = {0};
    esp_err_t ble_err = hub_ble_set_power(enabled, &ble_result);
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = ESP_FAIL;
    bool cloud_attempted = should_try_cloud_after_ble(ble_err, &ble_result);
    if (cloud_attempted) {
        cloud_err = hub_cloud_set_power(enabled, &cloud_result);
    }
    return finish_ble_first_result("power", ble_err, &ble_result, cloud_err, &cloud_result, cloud_attempted, HUB_CONTROL_RISK_HIGH, true, started, result);
}

esp_err_t hub_control_execute_backflush(bool confirm, hub_control_result_t *result)
{
    if (!confirm) {
        return confirmation_required_result("backflush", HUB_CONTROL_TRANSPORT_CLOUD, result);
    }
    int started = now_ms();
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = hub_cloud_start_backflush(&cloud_result);
    return finish_cloud_primary_result("backflush", cloud_err, &cloud_result, HUB_CONTROL_RISK_HIGH, true, started, result);
}

esp_err_t hub_control_execute_machine_firmware_update(bool confirm, hub_control_result_t *result)
{
    if (!confirm) {
        return confirmation_required_result("machineFirmwareUpdate", HUB_CONTROL_TRANSPORT_BLE, result);
    }
    int started = now_ms();
    hub_ble_command_result_t ble_result = {0};
    esp_err_t ble_err = hub_ble_update_machine_firmware(&ble_result);
    hub_cloud_command_result_t cloud_result = {0};
    esp_err_t cloud_err = ESP_FAIL;
    bool cloud_attempted = should_try_cloud_after_ble(ble_err, &ble_result);
    if (cloud_attempted) {
        cloud_err = hub_cloud_update_machine_firmware(&cloud_result);
    }
    return finish_ble_first_result("machineFirmwareUpdate", ble_err, &ble_result, cloud_err, &cloud_result, cloud_attempted, HUB_CONTROL_RISK_HIGH, true, started, result);
}

static esp_err_t execute_schedule(const hub_command_request_t *request, hub_control_result_t *result)
{
    if (!request->value.schedule.confirm) {
        return confirmation_required_result("schedule", HUB_CONTROL_TRANSPORT_CLOUD, result);
    }
    int started = now_ms();
    hub_cloud_command_result_t cloud = {0};
    esp_err_t err = hub_cloud_set_schedule(
        &request->value.schedule.expected, &request->value.schedule.requested, &cloud);
    if (cloud.request_started) hub_state_invalidate_schedule();
    bool ok = err == ESP_OK && cloud.ok;
    set_result(result, ok, "schedule",
        ok || cloud.request_started ? HUB_CONTROL_TRANSPORT_CLOUD : HUB_CONTROL_TRANSPORT_NONE,
        HUB_CONTROL_TRANSPORT_CLOUD, HUB_CONTROL_TRANSPORT_NONE, false,
        HUB_CONTROL_RISK_HIGH, true, started,
        cloud.message[0] ? cloud.message : esp_err_to_name(err));
    set_cloud_diagnostics(result, true, &cloud);
    result->write_accepted = ok;
    result->state_confirmed = ok;
    if (!ok && cloud.request_started) {
        result->state_confirmation_pending = true;
        result->late_side_effect_risk = true;
        result->v2_status = HUB_API_V2_COMMAND_STATUS_UNKNOWN_OUTCOME;
        snprintf(result->error_code, sizeof(result->error_code), "unknownOutcome");
    } else if (cloud.schedule_changed) {
        result->response_http_status = 200;
        result->v2_status = HUB_API_V2_COMMAND_STATUS_COMPLETED;
        snprintf(result->error_code, sizeof(result->error_code), "scheduleChanged");
    }
    record_control_result(result);
    return ok ? ESP_OK : (err != ESP_OK ? err : ESP_FAIL);
}
