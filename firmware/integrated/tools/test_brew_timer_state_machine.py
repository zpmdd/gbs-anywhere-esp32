#!/usr/bin/env python3

import os
import pathlib
import subprocess
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"


HARNESS = r"""
#include "hub_brew_timer.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void render(char *json, size_t json_len, int64_t now_us) {
    memset(json, 0, json_len);
    require_true(hub_brew_timer_live_state_json(json, json_len, now_us), "render failed");
}

static void require_contains(const char *json, const char *needle) {
    if (strstr(json, needle) == NULL) {
        fprintf(stderr, "missing %s in %s\n", needle, json);
        exit(1);
    }
}

static void require_not_contains(const char *json, const char *needle) {
    if (strstr(json, needle) != NULL) {
        fprintf(stderr, "unexpected %s in %s\n", needle, json);
        exit(1);
    }
}

int main(void) {
    char json[512];
    char tiny[8];

    require_true(!hub_brew_timer_available(), "zero-initialized timer must not be available");

    hub_brew_timer_reset();
    render(json, sizeof(json), 1000000);
    require_contains(json, "\"brewTimerState\":\"idle\"");
    require_contains(json, "\"extractionSeconds\":null");
    require_contains(json, "\"source\":\"unconfigured\"");
    require_contains(json, "\"confidence\":\"unconfigured\"");
    require_not_contains(json, "\"source\":null");
    require_true(!hub_brew_timer_live_state_json(tiny, sizeof(tiny), 1000000),
                 "small buffer render should fail");

    hub_brew_timer_record_cloud_seconds(-1.0, 1500000);
    hub_brew_timer_record_cloud_seconds(INFINITY, 1600000);
    hub_brew_timer_record_cloud_seconds(1e100, 1650000);
    hub_brew_timer_record_cloud_seconds(NAN, 1700000);
    render(json, sizeof(json), 1700000);
    require_contains(json, "\"brewTimerState\":\"idle\"");
    require_contains(json, "\"extractionSeconds\":null");
    require_contains(json, "\"source\":\"unconfigured\"");
    require_true(!hub_brew_timer_available(), "invalid Cloud samples must not enable availability");

    hub_brew_timer_reset();
    hub_brew_timer_update_mode("BrewingMode", 2000000);
    render(json, sizeof(json), 4500000);
    require_contains(json, "\"brewTimerState\":\"idle\"");
    require_contains(json, "\"source\":\"unconfigured\"");
    require_contains(json, "\"extractionSeconds\":null");
    require_true(!hub_brew_timer_available(), "machineMode freshness must not enable brew timer availability");

    hub_brew_timer_reset();
    hub_brew_timer_update_mode("bReWiNgMoDe", 2000000);
    render(json, sizeof(json), 4500000);
    require_contains(json, "\"brewTimerState\":\"idle\"");
    require_contains(json, "\"source\":\"unconfigured\"");
    require_contains(json, "\"extractionSeconds\":null");

    hub_brew_timer_record_cloud_seconds(3.25, 5250000);
    render(json, sizeof(json), 5250000);
    require_contains(json, "\"brewTimerState\":\"brewing\"");
    require_contains(json, "\"source\":\"cloud_websocket\"");
    require_contains(json, "\"extractionSeconds\":3.25");

    hub_brew_timer_update_mode("StandBy", 29800000);
    render(json, sizeof(json), 29800000);
    require_contains(json, "\"brewTimerState\":\"brewing\"");
    require_contains(json, "\"source\":\"cloud_websocket\"");
    require_contains(json, "\"extractionSeconds\":3.25");

    hub_brew_timer_update_mode("StandBy", 35000000);
    render(json, sizeof(json), 35000000);
    require_contains(json, "\"brewTimerState\":\"brewing\"");
    require_contains(json, "\"source\":\"cloud_websocket\"");
    require_contains(json, "\"extractionSeconds\":3.25");

    hub_brew_timer_record_cloud_seconds(1.25, 48250000);
    render(json, sizeof(json), 48250000);
    require_contains(json, "\"brewTimerState\":\"brewing\"");
    require_contains(json, "\"source\":\"cloud_websocket\"");
    require_contains(json, "\"extractionSeconds\":1.25");

    hub_brew_timer_update_source_stale(48000000);
    render(json, sizeof(json), 48000000);
    require_contains(json, "\"brewTimerState\":\"stale\"");
    require_contains(json, "\"source\":\"unconfigured\"");
    require_true(!hub_brew_timer_available(), "stale timer must not be available");

    hub_brew_timer_reset();
    hub_brew_timer_record_cloud_seconds(20.0, 20000000);
    render(json, sizeof(json), 20000000);
    require_contains(json, "\"brewTimerState\":\"brewing\"");
    require_contains(json, "\"source\":\"cloud_websocket\"");
    require_contains(json, "\"extractionSeconds\":20");
    hub_brew_timer_update_mode("BrewingMode", 30000000);
    hub_brew_timer_update_mode("StandBy", 40000000);
    render(json, sizeof(json), 40000000);
    require_contains(json, "\"brewTimerState\":\"brewing\"");
    require_contains(json, "\"source\":\"cloud_websocket\"");
    require_contains(json, "\"extractionSeconds\":20");

    hub_brew_timer_reset();
    hub_brew_timer_update_mode("BrewingMode", 1000000);
    hub_brew_timer_update_mode("StandBy", 11000000);
    hub_brew_timer_update_source_stale(12000000);
    render(json, sizeof(json), 12000000);
    require_contains(json, "\"brewTimerState\":\"stale\"");
    require_contains(json, "\"source\":\"unconfigured\"");
    require_contains(json, "\"extractionSeconds\":null");
    render(json, sizeof(json), 27000000);
    require_not_contains(json, "\"brewTimerState\":\"finished\"");
    require_contains(json, "\"source\":\"unconfigured\"");
    require_contains(json, "\"extractionSeconds\":null");

    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="brew-timer-host-") as tmp:
        tmp_path = pathlib.Path(tmp)
        harness = tmp_path / "harness.c"
        binary = tmp_path / "harness"
        harness.write_text(textwrap.dedent(HARNESS), encoding="utf-8")

        cc = os.environ.get("CC", "cc")
        command = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(MAIN_DIR),
            str(harness),
            str(MAIN_DIR / "hub_brew_timer.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)

    print("brew timer state machine host test passed")


if __name__ == "__main__":
    main()
