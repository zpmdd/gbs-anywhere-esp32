#!/usr/bin/env python3
"""Compile and run host tests for WebSocket resource and reconnect policy."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"


HARNESS = r"""
#include "hub_cloud_websocket_policy.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

int main(void)
{
    hub_cloud_websocket_policy_t policy = {0};
    hub_cloud_websocket_policy_reset(&policy);

    require_true(
        hub_cloud_websocket_policy_start_eligible(&policy, true, 32768, 12288, 0),
        "exact start threshold rejected"
    );
    require_true(
        !hub_cloud_websocket_policy_start_eligible(&policy, true, 32767, 12288, 0),
        "low free heap accepted"
    );
    require_true(
        !hub_cloud_websocket_policy_start_eligible(&policy, true, 32768, 12287, 0),
        "small largest block accepted"
    );
    require_true(
        !hub_cloud_websocket_policy_start_eligible(&policy, false, 65536, 32768, 0),
        "missing PSRAM accepted"
    );

    require_true(!hub_cloud_websocket_policy_sample_low_resources(&policy, 23999, 8191, 1000), "circuit opened after one sample");
    require_true(!hub_cloud_websocket_policy_sample_low_resources(&policy, 23999, 8191, 11000), "circuit opened after two samples");
    require_true(hub_cloud_websocket_policy_sample_low_resources(&policy, 23999, 8191, 21000), "circuit did not open after three samples");
    require_true(policy.circuit_open_until_ms == 621000, "circuit duration mismatch");
    require_true(!hub_cloud_websocket_policy_start_eligible(&policy, true, 65536, 32768, 620999), "open circuit was ignored");
    require_true(hub_cloud_websocket_policy_start_eligible(&policy, true, 32768, 12288, 621000), "recovered circuit stayed closed");

    const uint32_t expected[] = {5000, 15000, 30000, 60000, 120000, 300000, 300000};
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
        require_true(
            hub_cloud_websocket_policy_backoff_ms((unsigned)index, 500) == expected[index],
            "backoff ladder mismatch"
        );
    }
    require_true(
        hub_cloud_websocket_policy_backoff_ms(4, 0) == 96000,
        "negative jitter boundary mismatch"
    );
    require_true(
        hub_cloud_websocket_policy_backoff_ms(4, 1000) == 144000,
        "positive jitter boundary mismatch"
    );
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="cloud-ws-policy-host-") as temporary:
        temporary_path = pathlib.Path(temporary)
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
            str(MAIN_DIR),
            str(harness_path),
            str(MAIN_DIR / "hub_cloud_websocket_policy.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)

    print("Cloud WebSocket policy host tests passed")


if __name__ == "__main__":
    main()
