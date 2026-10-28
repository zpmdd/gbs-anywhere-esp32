#!/usr/bin/env python3
"""Run the real BLE worker loop with a deterministic queue and clock on the host."""

from pathlib import Path
import re
import tempfile

from test_integrated_support import ROOT, compile_run, function_body


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
typedef struct { int unused; } hub_ble_read_result_t;
typedef struct { esp_err_t (*function)(void *); void *context; } hub_ble_worker_job_t;
static struct {
    uint32_t cache_sequence;
    int consecutive_failures;
    int64_t cached_at_ms;
    char status[16], cache_origin[32];
} ble_state;
static bool ble_worker_job_yielded, ble_worker_job_active;
static void *ble_worker_job_owner;
static int ble_worker_job_result, ble_worker_job_queue, ble_worker_job_complete;
static int64_t clock_ms;
static bool identity_matches;
static jmp_buf stop_worker;
static int waits[8], wait_count, stop_at, step, background_reads, completed_jobs;
static int initial_failures;
enum { RECOVER, NO_UPDATE, FAIL_AFTER_UPDATE, EXPIRED, WRONG_SELECTION, YIELDED };
static int job_kind;
static bool interactive_once;
static int64_t now_ms(void) { return clock_ms; }
static bool ble_cache_identity_matches_selection(void) { return identity_matches; }
static bool hub_ble_configured(void) { return true; }
static bool hub_command_interactive_pending(void) { return interactive_once && step == 2; }
static int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
static void hub_brew_timer_update_source_stale(int64_t t) { (void)t; }
static void record_ble_worker_stack_high_water_mark(void) {}
static void *xTaskGetCurrentTaskHandle(void) { return (void *)&clock_ms; }
/* SOURCE CONSTANTS */
/* SOURCE POLICY */
static void cache_observation(void)
{
    if (++ble_state.cache_sequence == 0) ble_state.cache_sequence = 1;
    ble_state.cached_at_ms = clock_ms;
    ble_state.consecutive_failures = 0;
    strcpy(ble_state.status, "cached");
    strcpy(ble_state.cache_origin, "fullRead");
}
static esp_err_t diagnostic(void *context)
{
    (void)context;
    assert(ble_worker_job_active && ble_worker_job_owner == xTaskGetCurrentTaskHandle());
    if (job_kind != NO_UPDATE) cache_observation();
    else ble_state.consecutive_failures = 0; // Successful scan/probe, no new state.
    if (job_kind == FAIL_AFTER_UPDATE) ble_state.consecutive_failures = 1;
    if (job_kind == EXPIRED) clock_ms += HUB_BLE_STATE_CACHE_TTL_MS + 1;
    if (job_kind == WRONG_SELECTION) identity_matches = false;
    if (job_kind == YIELDED) ble_worker_job_yielded = true;
    // HTTP handlers can successfully send an error response, so this is not BLE proof.
    return ESP_OK;
}
static int ulTaskNotifyTake(int clear, int timeout)
{
    assert(clear == pdTRUE && wait_count < 8);
    step = wait_count;
    waits[wait_count++] = timeout;
    if (step == stop_at) longjmp(stop_worker, 1);
    if (step != 1) clock_ms += timeout; // The queued diagnostic wakes backoff immediately.
    return step == 1;
}
static int xQueueReceive(int queue, hub_ble_worker_job_t *job, int timeout)
{
    assert(queue == ble_worker_job_queue && timeout == 0);
    if (step != 1) return 0;
    *job = (hub_ble_worker_job_t){diagnostic, NULL};
    return pdTRUE;
}
static void xSemaphoreGive(int semaphore)
{
    assert(semaphore == ble_worker_job_complete);
    assert(!ble_worker_job_active && ble_worker_job_owner == NULL);
    completed_jobs++;
}
static esp_err_t validate_read_internal(hub_ble_read_result_t *result, bool realtime)
{
    (void)result;
    assert(realtime && !hub_command_interactive_pending());
    if (++background_reads == 1) {
        ble_state.consecutive_failures = initial_failures;
        return ESP_FAIL;
    }
    cache_observation();
    strcpy(ble_state.cache_origin, "workerRealtime");
    return ESP_OK;
}
/* SOURCE WORKER */
static void run(int failures, int kind, bool interactive, bool wrap, int last_step)
{
    memset(&ble_state, 0, sizeof(ble_state));
    ble_state.cache_sequence = wrap ? UINT32_MAX : 7;
    clock_ms = 1000;
    ble_state.cached_at_ms = clock_ms;
    identity_matches = true;
    ble_worker_job_yielded = ble_worker_job_active = false;
    ble_worker_job_owner = NULL;
    wait_count = background_reads = completed_jobs = 0;
    initial_failures = failures;
    job_kind = kind;
    interactive_once = interactive;
    stop_at = last_step;
    if (setjmp(stop_worker) == 0) hub_ble_worker_task(NULL);
    assert(waits[0] == HUB_BLE_WORKER_BOOT_DELAY_MS);
    assert(waits[1] == ble_worker_retry_delay_ms(failures));
    assert(completed_jobs == 1);
}
int main(void)
{
    // Recovery must refresh again before the existing 45-second TTL expires.
    for (int failures = 1; failures <= 3; failures++) {
        run(failures, RECOVER, false, false, 3);
        assert(waits[2] == 30000 && waits[3] == 30000);
        assert(background_reads == 2 && ble_state.cache_sequence == 9);
        assert(ble_cache_is_fresh());
        assert(!strcmp(ble_state.cache_origin, "workerRealtime"));
    }
    // A successful HTTP response, unchanged cache, failed read, expired observation,
    // changed selection or yielded diagnostic cannot silently end radio backoff.
    for (int kind = NO_UPDATE; kind <= YIELDED; kind++) {
        run(3, kind, false, false, 2);
        assert(waits[2] == 300000 && background_reads == 1);
    }
    run(3, RECOVER, false, true, 2);
    assert(waits[2] == 30000 && ble_state.cache_sequence == 1);

    // Daily interactive control still wins over a scheduled refresh after recovery.
    run(3, RECOVER, true, false, 4);
    assert(waits[2] == 30000 && waits[3] == 1000 && waits[4] == 30000);
    assert(background_reads == 2 && ble_cache_is_fresh());

    // Test the actual TTL boundary and command-confirmation deferral as well.
    clock_ms = ble_state.cached_at_ms + HUB_BLE_STATE_CACHE_TTL_MS;
    assert(ble_cache_is_fresh());
    clock_ms++;
    assert(!ble_cache_is_fresh());
    cache_observation();
    strcpy(ble_state.cache_origin, "commandConfirmation");
    clock_ms += 500;
    assert(worker_delay_after_recent_command_confirmation() == 29500);
    clock_ms += 29500;
    assert(worker_delay_after_recent_command_confirmation() == 0);
    return 0;
}
'''


def extract_function(source: str, name: str) -> str:
    signature = re.search(rf"^[\w\s\*]+{name}\s*\([^)]*\)\s*\{{", source, re.M)
    assert signature is not None, name
    return signature.group() + function_body(source, name) + "}"


def main() -> None:
    source = (ROOT / "main/hub_ble.c").read_text()
    constants = "\n".join(re.findall(
        r"^static const int HUB_BLE_(?:WORKER_[A-Z_]+|STATE_CACHE_TTL_MS)(?:\[\])? = [^;]+;",
        source, re.M,
    ))
    policy = "\n".join(extract_function(source, name) for name in (
        "ble_worker_retry_delay_ms", "ble_cache_is_fresh",
        "worker_delay_after_recent_command_confirmation",
    ))
    harness = HARNESS.replace("/* SOURCE CONSTANTS */", constants).replace(
        "/* SOURCE POLICY */", policy,
    ).replace("/* SOURCE WORKER */", extract_function(source, "hub_ble_worker_task"))
    with tempfile.TemporaryDirectory(prefix="ble-worker-recovery-") as temporary:
        compile_run(Path(temporary), "worker", harness, ["-Wno-unused-parameter"])
    print("BLE worker recovery, failure backoff, TTL and interactive priority: PASS")


if __name__ == "__main__":
    main()
