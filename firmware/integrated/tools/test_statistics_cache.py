#!/usr/bin/env python3
"""Compile and run transactional host tests for the statistics cache."""

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
typedef unsigned int TickType_t;
typedef int portMUX_TYPE;
#define pdTRUE 1
#define pdFALSE 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(value) ((TickType_t)(value))
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
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


HARNESS = r"""
#include "freertos/semphr.h"
#include "hub_statistics_cache.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static char *owned_body(const char *text)
{
    size_t length = strlen(text) + 1;
    char *body = malloc(length);
    require_true(body != NULL, "test allocation failed");
    memcpy(body, text, length);
    return body;
}

static hub_statistics_cache_ticket_t begin(const char *serial, int days, int64_t now_ms)
{
    hub_statistics_cache_ticket_t ticket = {0};
    require_true(
        hub_statistics_cache_begin(serial, days, now_ms, &ticket) == ESP_OK,
        "cache begin failed"
    );
    return ticket;
}

static void complete_ok(const char *serial, int days, int64_t started_ms, int64_t completed_ms, const char *body)
{
    hub_statistics_cache_ticket_t ticket = begin(serial, days, started_ms);
    require_true(
        hub_statistics_cache_complete(&ticket, ESP_OK, owned_body(body), completed_ms) == ESP_OK,
        "cache complete failed"
    );
}

static hub_statistics_read_state_t copy(
    const char *serial,
    int days,
    char *destination,
    size_t capacity,
    hub_statistics_cache_metadata_t *metadata
)
{
    return hub_statistics_cache_copy(serial, days, destination, capacity, metadata);
}

static void test_begin_29_does_not_change_last_good_30(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 30, 10, 20, "{\"window\":30}");
    hub_statistics_cache_ticket_t ticket_29 = begin("SERIAL", 29, 30);
    char body[128] = {0};
    hub_statistics_cache_metadata_t metadata = {0};

    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "30-day last-good disappeared when 29-day refresh began");
    require_true(strcmp(body, "{\"window\":30}") == 0, "30-day body changed while 29-day refresh ran");
    require_true(metadata.days == 30 && metadata.completed_ms == 20,
                 "30-day metadata changed while 29-day refresh ran");
    require_true(
        hub_statistics_cache_complete(&ticket_29, ESP_ERR_TIMEOUT, NULL, 40) == ESP_ERR_TIMEOUT,
        "failed 29-day refresh did not report its error"
    );
}

static void test_failed_29_keeps_last_good_30_body_and_metadata(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 30, 10, 20, "{\"window\":30}");
    hub_statistics_cache_ticket_t ticket_29 = begin("SERIAL", 29, 30);
    require_true(
        hub_statistics_cache_complete(&ticket_29, ESP_ERR_TIMEOUT, NULL, 40) == ESP_ERR_TIMEOUT,
        "failed 29-day refresh did not report its error"
    );
    char body[128] = {0};
    hub_statistics_cache_metadata_t metadata = {0};
    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "failed 29-day refresh removed 30-day cache");
    require_true(strcmp(body, "{\"window\":30}") == 0, "failed refresh mutated 30-day body");
    require_true(metadata.completed_ms == 20 && metadata.last_error == ESP_OK,
                 "failed refresh mutated 30-day metadata");
}

static void test_successful_29_does_not_evict_recently_used_30(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 7, 10, 11, "{\"window\":7}");
    complete_ok("SERIAL", 14, 20, 21, "{\"window\":14}");
    complete_ok("SERIAL", 30, 30, 31, "{\"window\":30}");
    char body[128] = {0};
    hub_statistics_cache_metadata_t metadata = {0};
    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "30-day cache missing before LRU test");

    complete_ok("SERIAL", 29, 40, 41, "{\"window\":29}");

    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "recently used 30-day cache was evicted");
    require_true(copy("SERIAL", 29, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "new 29-day cache was not committed");
    require_true(copy("SERIAL", 14, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "wrong LRU entry was evicted");
    require_true(copy("SERIAL", 7, body, sizeof(body), &metadata) == HUB_STATISTICS_UNAVAILABLE,
                 "least recently used 7-day entry was not evicted");
}

static void test_complete_swaps_body_only_after_validation(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 30, 10, 20, "{\"version\":1}");
    hub_statistics_cache_ticket_t ticket = begin("SERIAL", 30, 30);
    char body[128] = {0};
    hub_statistics_cache_metadata_t metadata = {0};
    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "last-good missing during refresh");
    require_true(metadata.stale, "in-flight last-good was not marked stale");
    require_true(strcmp(body, "{\"version\":1}") == 0, "body swapped before completion");

    char *invalid = owned_body("");
    require_true(
        hub_statistics_cache_complete(&ticket, ESP_OK, invalid, 40) == ESP_ERR_INVALID_RESPONSE,
        "empty candidate was committed"
    );
    free(invalid);
    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "invalid completion removed last-good");
    require_true(strcmp(body, "{\"version\":1}") == 0 && metadata.completed_ms == 20,
                 "invalid completion mutated last-good");

    ticket = begin("SERIAL", 30, 50);
    require_true(
        hub_statistics_cache_complete(&ticket, ESP_OK, owned_body("{\"version\":2}"), 60) == ESP_OK,
        "valid replacement failed"
    );
    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
                 "valid replacement missing");
    require_true(strcmp(body, "{\"version\":2}") == 0 && metadata.completed_ms == 60,
                 "valid replacement did not swap atomically");
}

static void test_copy_reports_refreshing_without_fake_empty_success(void)
{
    hub_statistics_cache_reset();
    hub_statistics_cache_ticket_t ticket = begin("SERIAL", 30, 10);
    char body[128] = "sentinel";
    hub_statistics_cache_metadata_t metadata = {0};
    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_REFRESHING,
                 "cache miss in flight did not report refreshing");
    require_true(body[0] == '\0', "refreshing cache returned a fake empty-success body");
    require_true(
        hub_statistics_cache_complete(&ticket, ESP_FAIL, NULL, 20) == ESP_FAIL,
        "failed refresh did not complete"
    );
    require_true(copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_UNAVAILABLE,
                 "failed cache miss did not report unavailable");
    require_true(metadata.failure_ms == 20 && metadata.last_error == ESP_FAIL,
                 "failed cache miss metadata was lost");
}

static void test_lru_never_frees_body_while_copying(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 7, 10, 11, "{\"window\":7,\"payload\":\"copied\"}");
    char copied[128] = {0};
    hub_statistics_cache_metadata_t metadata = {0};
    require_true(copy("SERIAL", 7, copied, sizeof(copied), &metadata) == HUB_STATISTICS_HIT,
                 "copy source missing");

    complete_ok("SERIAL", 14, 20, 21, "{\"window\":14}");
    complete_ok("SERIAL", 21, 30, 31, "{\"window\":21}");
    complete_ok("SERIAL", 30, 40, 41, "{\"window\":30}");
    complete_ok("SERIAL", 29, 50, 51, "{\"window\":29}");

    require_true(strcmp(copied, "{\"window\":7,\"payload\":\"copied\"}") == 0,
                 "eviction invalidated the caller-owned copy");
}

static void test_refresh_request_state_distinguishes_ready_inflight_needed_and_blocked(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 30, 10, 20, "{\"window\":30}");
    hub_statistics_cache_ticket_t ticket_29 = begin("SERIAL", 29, 30);

    require_true(
        hub_statistics_cache_refresh_state("SERIAL", 30, 40, 120000)
            == HUB_STATISTICS_REFRESH_READY,
        "fresh 30-day cache was not reported ready while 29-day refresh ran"
    );
    require_true(
        hub_statistics_cache_refresh_state("SERIAL", 29, 40, 120000)
            == HUB_STATISTICS_REFRESH_INFLIGHT,
        "same 29-day refresh was not reported inflight"
    );
    require_true(
        hub_statistics_cache_refresh_state("SERIAL", 14, 40, 120000)
            == HUB_STATISTICS_REFRESH_BLOCKED,
        "different cache miss was not blocked by the 29-day refresh"
    );

    require_true(
        hub_statistics_cache_complete(&ticket_29, ESP_OK, owned_body("{\"window\":29}"), 50)
            == ESP_OK,
        "29-day refresh completion failed"
    );
    require_true(
        hub_statistics_cache_refresh_state("SERIAL", 30, 60, 120000)
            == HUB_STATISTICS_REFRESH_READY,
        "30-day cache stopped being ready after 29-day completion"
    );
    require_true(
        hub_statistics_cache_refresh_state("SERIAL", 14, 60, 120000)
            == HUB_STATISTICS_REFRESH_NEEDED,
        "idle cache miss was not reported as needing a refresh"
    );
}

static void test_failed_refresh_is_not_reported_ready_from_older_last_good(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 30, 10, 20, "{\"window\":30,\"version\":1}");
    hub_statistics_cache_ticket_t ticket = begin("SERIAL", 30, 30);
    require_true(
        hub_statistics_cache_complete(&ticket, ESP_ERR_TIMEOUT, NULL, 40) == ESP_ERR_TIMEOUT,
        "same-key failed refresh did not report its error"
    );

    require_true(
        hub_statistics_cache_refresh_state("SERIAL", 30, 50, 120000)
            == HUB_STATISTICS_REFRESH_NEEDED,
        "newer same-key failure was hidden by the older fresh last-good"
    );
    require_true(
        !hub_statistics_cache_is_fresh("SERIAL", 30, 50, 120000),
        "freshness helper hid a newer same-key failure"
    );
}

static void test_failure_for_another_window_does_not_hide_cached_window_failure(void)
{
    hub_statistics_cache_reset();
    complete_ok("SERIAL", 30, 10, 20, "{\"window\":30,\"version\":1}");

    hub_statistics_cache_ticket_t ticket_30 = begin("SERIAL", 30, 30);
    require_true(
        hub_statistics_cache_complete(&ticket_30, ESP_ERR_TIMEOUT, NULL, 40) == ESP_ERR_TIMEOUT,
        "30-day refresh failure was not recorded"
    );
    hub_statistics_cache_ticket_t ticket_29 = begin("SERIAL", 29, 50);
    require_true(
        hub_statistics_cache_complete(&ticket_29, ESP_FAIL, NULL, 60) == ESP_FAIL,
        "29-day refresh failure was not recorded"
    );

    require_true(
        hub_statistics_cache_refresh_state("SERIAL", 30, 70, 120000)
            == HUB_STATISTICS_REFRESH_NEEDED,
        "29-day failure hid the newer failure on the cached 30-day window"
    );
    char body[128] = {0};
    hub_statistics_cache_metadata_t metadata = {0};
    require_true(
        copy("SERIAL", 30, body, sizeof(body), &metadata) == HUB_STATISTICS_HIT,
        "failed refresh removed the 30-day last-good"
    );
    require_true(
        metadata.stale && metadata.failure_ms == 40 && metadata.last_error == ESP_ERR_TIMEOUT,
        "30-day failure metadata was overwritten by another window"
    );
}

static void test_rejects_statistics_windows_above_30_days(void)
{
    hub_statistics_cache_reset();
    hub_statistics_cache_ticket_t ticket = {0};
    require_true(
        hub_statistics_cache_begin("SERIAL", 31, 10, &ticket) == ESP_ERR_INVALID_ARG,
        "cache accepted a statistics window above 30 days"
    );
}

int main(void)
{
    test_begin_29_does_not_change_last_good_30();
    test_failed_29_keeps_last_good_30_body_and_metadata();
    test_successful_29_does_not_evict_recently_used_30();
    test_complete_swaps_body_only_after_validation();
    test_copy_reports_refreshing_without_fake_empty_success();
    test_lru_never_frees_body_while_copying();
    test_refresh_request_state_distinguishes_ready_inflight_needed_and_blocked();
    test_failed_refresh_is_not_reported_ready_from_older_last_good();
    test_failure_for_another_window_does_not_hide_cached_window_failure();
    test_rejects_statistics_windows_above_30_days();
    hub_statistics_cache_reset();
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="statistics-cache-host-") as temporary:
        temporary_path = pathlib.Path(temporary)
        include_path = temporary_path / "include" / "freertos"
        include_path.mkdir(parents=True)
        (temporary_path / "include" / "esp_err.h").write_text(
            textwrap.dedent(ESP_ERR_STUB), encoding="utf-8"
        )
        (include_path / "FreeRTOS.h").write_text(textwrap.dedent(FREERTOS_STUB), encoding="utf-8")
        (include_path / "semphr.h").write_text(textwrap.dedent(SEMPHR_STUB), encoding="utf-8")
        harness_path = temporary_path / "harness.c"
        harness_path.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
        binary_path = temporary_path / "harness"

        command = [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(temporary_path / "include"),
            "-I",
            str(MAIN_DIR),
            str(harness_path),
            str(MAIN_DIR / "hub_statistics_cache.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)

    print("statistics cache host tests passed (10 cases)")


if __name__ == "__main__":
    main()
