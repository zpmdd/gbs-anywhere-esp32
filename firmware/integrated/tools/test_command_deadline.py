#!/usr/bin/env python3
"""Compile/run the deadline module and verify deterministic Hub v2 outcomes."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import textwrap

from mock_hub_v2 import MockHubV2Engine


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"

HARNESS = r"""
#include "hub_command_deadline.h"

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
    hub_command_deadline_t default_ttl = {.enforced = true};
    require_true(hub_command_deadline_accept(&default_ttl, 1000), "default ttl rejected");
    require_true(default_ttl.accepted_ttl_ms == 8000, "default ttl changed");
    require_true(default_ttl.deadline_at_ms == 9000, "absolute deadline changed");
    require_true(hub_command_deadline_remaining_ms(&default_ttl, 1250) == 7750,
                 "remaining budget changed");

    hub_command_deadline_t explicit_ttl = {
        .enforced = true,
        .client_ttl_present = true,
        .client_ttl_ms = 4000,
    };
    require_true(hub_command_deadline_accept(&explicit_ttl, 500), "explicit ttl rejected");
    require_true(explicit_ttl.accepted_ttl_ms == 4000, "explicit ttl changed");
    require_true(!hub_command_deadline_can_admit(&explicit_ttl, 500, 4000),
                 "admission ignored terminal reserve");
    require_true(hub_command_deadline_can_admit(&explicit_ttl, 500, 3750),
                 "exact admission budget rejected");

    hub_command_deadline_t invalid_zero = {
        .enforced = true,
        .client_ttl_present = true,
        .client_ttl_ms = 0,
    };
    require_true(!hub_command_deadline_accept(&invalid_zero, 0), "zero ttl accepted");
    hub_command_deadline_t invalid_large = {
        .enforced = true,
        .client_ttl_present = true,
        .client_ttl_ms = 8001,
    };
    require_true(!hub_command_deadline_accept(&invalid_large, 0), "oversized ttl accepted");

    require_true(
        hub_command_deadline_timeout_ms(&default_ttl, 3000, 12000, 1000) == 4750,
        "dynamic timeout did not reserve terminal/following work"
    );
    require_true(
        hub_command_deadline_timeout_ms(&default_ttl, 8800, 12000, 0) == 0,
        "expired transport received a timeout"
    );

    hub_command_deadline_t legacy = {0};
    require_true(hub_command_deadline_accept(&legacy, 1000), "legacy marker rejected");
    require_true(hub_command_deadline_remaining_ms(&legacy, 5000) == -1,
                 "legacy request gained a deadline");
    require_true(hub_command_deadline_timeout_ms(&legacy, 5000, 12000, 1000) == 12000,
                 "legacy timeout changed");
    return 0;
}
"""


def _run_c_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="command-deadline-host-") as temporary:
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
            str(MAIN_DIR / "hub_command_deadline.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)


def _payload(request_id: str, *, grams: int = 30, ttl_ms: int = 8000) -> dict[str, object]:
    return {
        "requestId": request_id,
        "grams": grams,
        "clientTtlMs": ttl_ms,
        "clientSurface": "watchApp",
    }


def _run_mock_contract() -> None:
    request_id = "11111111-2222-4333-8444-555555555555"
    engine = MockHubV2Engine(scenario="success")
    status, accepted = engine.submit(
        route="/controls/quick-weight",
        machine_serial="MACHINE-A",
        payload=_payload(request_id),
        now_ms=1000,
    )
    assert status == 202 and accepted["acceptedTtlMs"] == 8000
    status, duplicate = engine.submit(
        route="/controls/quick-weight",
        machine_serial="MACHINE-A",
        payload=_payload(request_id, ttl_ms=4000),
        now_ms=1100,
    )
    assert status == 202
    assert duplicate["acceptedTtlMs"] == 8000
    status, conflict = engine.submit(
        route="/controls/quick-weight",
        machine_serial="MACHINE-A",
        payload=_payload(request_id, grams=31),
        now_ms=1100,
    )
    assert status == 409 and conflict["errorCode"] == "requestIdPayloadConflict"
    status, completed = engine.lookup(request_id=request_id, now_ms=1400)
    assert status == 200 and completed["status"] == "completed"

    for scenario, expected in (
        ("partialApply", "partialApply"),
        ("unknownOutcome", "unknownOutcome"),
        ("disconnect", "unknownOutcome"),
    ):
        scenario_engine = MockHubV2Engine(scenario=scenario)
        scenario_id = str(
            {
                "partialApply": "22222222-2222-4222-8222-222222222222",
                "unknownOutcome": "33333333-3333-4333-8333-333333333333",
                "disconnect": "44444444-4444-4444-8444-444444444444",
            }[scenario]
        )
        status, _ = scenario_engine.submit(
            route="/controls/quick-weight",
            machine_serial="MACHINE-A",
            payload=_payload(scenario_id),
            now_ms=0,
        )
        assert status == 202
        status, terminal = scenario_engine.lookup(request_id=scenario_id, now_ms=500)
        assert status == 200 and terminal["status"] == expected

    capacity = MockHubV2Engine(scenario="expireBeforeStart")
    status, rejected = capacity.submit(
        route="/controls/quick-weight",
        machine_serial="MACHINE-A",
        payload=_payload(
            "55555555-5555-4555-8555-555555555555",
            ttl_ms=1000,
        ),
        now_ms=0,
    )
    assert status == 503
    assert rejected["errorCode"] == "commandDeadlineCapacity"
    assert rejected["upstreamAttempts"] == 0

    for invalid_ttl in (0, True):
        invalid_payload = _payload(
            "66666666-6666-4666-8666-666666666666",
        )
        invalid_payload["clientTtlMs"] = invalid_ttl
        try:
            MockHubV2Engine().submit(
                route="/controls/quick-weight",
                machine_serial="MACHINE-A",
                payload=invalid_payload,
                now_ms=0,
            )
        except ValueError as exc:
            assert str(exc) == "invalidTtl"
        else:
            raise AssertionError("invalid TTL was accepted")


def main() -> None:
    _run_c_contract()
    _run_mock_contract()
    print("command deadline/mock contract tests passed (13 cases)")


if __name__ == "__main__":
    main()
