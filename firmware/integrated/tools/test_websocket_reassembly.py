#!/usr/bin/env python3
"""Compile and run host tests for bounded WebSocket DATA reassembly."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"


HARNESS = r"""
#include "hub_ws_reassembly.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPACITY 32U

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static hub_ws_reassembly_result_t feed(
    hub_ws_reassembly_t *state,
    char *slots[HUB_WS_REASSEMBLY_SLOT_COUNT],
    uint8_t opcode,
    bool fin,
    size_t payload_len,
    size_t payload_offset,
    const char *data,
    size_t data_len,
    char **completed,
    size_t *completed_len
)
{
    return hub_ws_reassembly_feed(
        state, slots, CAPACITY, opcode, fin, payload_len, payload_offset,
        data, data_len, completed, completed_len
    );
}

static void test_single_callback(void)
{
    hub_ws_reassembly_t state = {0};
    char first[CAPACITY + 1] = {0};
    char second[CAPACITY + 1] = {0};
    char *slots[] = {first, second};
    char *completed = NULL;
    size_t completed_len = 0;
    require_true(
        feed(&state, slots, 0x01, true, 5, 0, "hello", 5, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_COMPLETE,
        "single callback did not complete"
    );
    require_true(completed_len == 5 && strcmp(completed, "hello") == 0, "single callback body mismatch");
}

static void test_component_buffer_chunks(void)
{
    hub_ws_reassembly_t state = {0};
    char first[CAPACITY + 1] = {0};
    char second[CAPACITY + 1] = {0};
    char *slots[] = {first, second};
    char *completed = NULL;
    size_t completed_len = 0;
    require_true(
        feed(&state, slots, 0x01, true, 11, 0, "hello ", 6, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_MORE,
        "first component chunk completed early"
    );
    require_true(
        feed(&state, slots, 0x01, true, 11, 6, "world", 5, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_COMPLETE,
        "second component chunk did not complete"
    );
    require_true(completed_len == 11 && strcmp(completed, "hello world") == 0, "chunked body mismatch");
}

static void test_websocket_continuation_frames(void)
{
    hub_ws_reassembly_t state = {0};
    char first[CAPACITY + 1] = {0};
    char second[CAPACITY + 1] = {0};
    char *slots[] = {first, second};
    char *completed = NULL;
    size_t completed_len = 0;
    require_true(
        feed(&state, slots, 0x01, false, 6, 0, "hello ", 6, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_MORE,
        "non-FIN text frame completed"
    );
    require_true(
        feed(&state, slots, 0x00, true, 5, 0, "wo", 2, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_MORE,
        "continuation chunk completed early"
    );
    require_true(
        feed(&state, slots, 0x00, true, 5, 2, "rld", 3, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_COMPLETE,
        "continuation frame did not complete"
    );
    require_true(completed_len == 11 && strcmp(completed, "hello world") == 0, "continuation body mismatch");
}

static void test_unexpected_continuation(void)
{
    hub_ws_reassembly_t state = {0};
    char first[CAPACITY + 1] = {0};
    char second[CAPACITY + 1] = {0};
    char *slots[] = {first, second};
    char *completed = NULL;
    size_t completed_len = 0;
    require_true(
        feed(&state, slots, 0x00, true, 1, 0, "x", 1, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_UNEXPECTED_CONTINUATION,
        "unexpected continuation accepted"
    );
}

static void test_new_text_during_fragment(void)
{
    hub_ws_reassembly_t state = {0};
    char first[CAPACITY + 1] = {0};
    char second[CAPACITY + 1] = {0};
    char *slots[] = {first, second};
    char *completed = NULL;
    size_t completed_len = 0;
    require_true(
        feed(&state, slots, 0x01, false, 2, 0, "ab", 2, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_MORE,
        "fragment setup failed"
    );
    require_true(
        feed(&state, slots, 0x01, true, 1, 0, "x", 1, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_UNEXPECTED_TEXT,
        "new text frame replaced a fragmented message"
    );
}

static void test_oversized_and_invalid_bounds(void)
{
    hub_ws_reassembly_t state = {0};
    char first[CAPACITY + 1] = {0};
    char second[CAPACITY + 1] = {0};
    char *slots[] = {first, second};
    char *completed = NULL;
    size_t completed_len = 0;
    require_true(
        feed(&state, slots, 0x01, true, CAPACITY + 1, 0, "x", 1, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_OVERSIZED,
        "oversized payload accepted"
    );
    hub_ws_reassembly_reset(&state);
    require_true(
        feed(&state, slots, 0x01, true, 3, 2, "xx", 2, &completed, &completed_len)
            == HUB_WS_REASSEMBLY_INVALID,
        "out-of-range component chunk accepted"
    );
}

static void test_completed_messages_rotate_slots(void)
{
    hub_ws_reassembly_t state = {0};
    char first[CAPACITY + 1] = {0};
    char second[CAPACITY + 1] = {0};
    char *slots[] = {first, second};
    char *one = NULL;
    char *two = NULL;
    size_t length = 0;
    require_true(
        feed(&state, slots, 0x01, true, 1, 0, "a", 1, &one, &length)
            == HUB_WS_REASSEMBLY_COMPLETE,
        "first rotating message failed"
    );
    require_true(
        feed(&state, slots, 0x01, true, 1, 0, "b", 1, &two, &length)
            == HUB_WS_REASSEMBLY_COMPLETE,
        "second rotating message failed"
    );
    require_true(one != two && strcmp(one, "a") == 0 && strcmp(two, "b") == 0, "slots did not rotate");
}

int main(void)
{
    test_single_callback();
    test_component_buffer_chunks();
    test_websocket_continuation_frames();
    test_unexpected_continuation();
    test_new_text_during_fragment();
    test_oversized_and_invalid_bounds();
    test_completed_messages_rotate_slots();
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="websocket-reassembly-host-") as temporary:
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
            str(MAIN_DIR / "hub_ws_reassembly.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)

    print("WebSocket reassembly host tests passed (7 cases)")


if __name__ == "__main__":
    main()
