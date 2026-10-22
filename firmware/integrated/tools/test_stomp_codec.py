#!/usr/bin/env python3
"""Compile and run host tests for the bounded STOMP 1.2 codec."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"


HARNESS = r"""
#include "hub_stomp.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void test_connected_header_only_crlf(void)
{
    char raw[] = "CONNECTED\r\nversion:1.2\r\nheart-beat:0,0\r\n\r\n\0";
    hub_stomp_frame_t frame = {0};
    require_true(hub_stomp_parse(raw, sizeof(raw) - 1, &frame), "CONNECTED parse failed");
    require_true(frame.type == HUB_STOMP_CONNECTED, "CONNECTED type mismatch");
    require_true(frame.body_len == 0, "CONNECTED body must be empty");
}

static void test_message_lf_and_content_length(void)
{
    char raw[] = "MESSAGE\ndestination:/ws/sn/MI017200/dashboard\ncontent-length:7\n\n{\"a\":1}\0";
    hub_stomp_frame_t frame = {0};
    require_true(hub_stomp_parse(raw, sizeof(raw) - 1, &frame), "MESSAGE parse failed");
    require_true(frame.type == HUB_STOMP_MESSAGE, "MESSAGE type mismatch");
    require_true(strcmp(frame.destination, "/ws/sn/MI017200/dashboard") == 0, "destination mismatch");
    require_true(frame.body_len == 7, "body length mismatch");
    require_true(memcmp(frame.body, "{\"a\":1}", 7) == 0, "body mismatch");
}

static void test_truncated_content_length_is_rejected(void)
{
    char raw[] = "MESSAGE\ncontent-length:8\n\n{\"a\":1}\0";
    hub_stomp_frame_t frame = {0};
    require_true(!hub_stomp_parse(raw, sizeof(raw) - 1, &frame), "truncated frame accepted");
}

static void test_heartbeat(void)
{
    char raw[] = "\n";
    hub_stomp_frame_t frame = {0};
    require_true(hub_stomp_parse(raw, sizeof(raw) - 1, &frame), "heartbeat parse failed");
    require_true(frame.type == HUB_STOMP_HEARTBEAT, "heartbeat type mismatch");
}

static void test_connect_and_subscribe_encoding(void)
{
    char frame[1536] = {0};
    size_t length = hub_stomp_write_connect(frame, sizeof(frame), "token-value");
    require_true(length > 0, "CONNECT encode failed");
    require_true(strstr(frame, "CONNECT\n") == frame, "CONNECT command missing");
    require_true(strstr(frame, "Authorization:Bearer token-value\n") != NULL, "authorization missing");
    require_true(frame[length - 1] == '\0', "CONNECT terminator missing");

    memset(frame, 0, sizeof(frame));
    length = hub_stomp_write_subscribe(
        frame,
        sizeof(frame),
        "lb-dashboard-1",
        "/ws/sn/MI017200/dashboard"
    );
    require_true(length > 0, "SUBSCRIBE encode failed");
    require_true(strstr(frame, "SUBSCRIBE\n") == frame, "SUBSCRIBE command missing");
    require_true(strstr(frame, "ack:auto\n") != NULL, "ack header missing");
    require_true(strstr(frame, "content-length:0\n") != NULL, "content-length missing");
    require_true(frame[length - 1] == '\0', "SUBSCRIBE terminator missing");
}

static void test_control_characters_are_rejected(void)
{
    char frame[256] = {0};
    require_true(
        hub_stomp_write_subscribe(frame, sizeof(frame), "bad\nid", "/safe") == 0,
        "header injection was accepted"
    );
}

int main(void)
{
    test_connected_header_only_crlf();
    test_message_lf_and_content_length();
    test_truncated_content_length_is_rejected();
    test_heartbeat();
    test_connect_and_subscribe_encoding();
    test_control_characters_are_rejected();
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="stomp-codec-host-") as temporary:
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
            str(MAIN_DIR / "hub_stomp.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)

    print("STOMP codec host tests passed (6 cases)")


if __name__ == "__main__":
    main()
