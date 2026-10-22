#!/usr/bin/env python3
"""Compile and run host tests for the Cloud HTTP response accumulator."""

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
#define ESP_ERR_INVALID_SIZE 0x104
"""


HEAP_CAPS_STUB = r"""
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT (1U << 2)
#define MALLOC_CAP_SPIRAM (1U << 10)
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
"""


HARNESS = r"""
#include "hub_http_response.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail_allocations;

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    if (fail_allocations) {
        return NULL;
    }
    return realloc(ptr, size);
}

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void test_accumulator_keeps_first_append_error(void)
{
    hub_http_response_t response = {0};
    fail_allocations = true;
    require_true(
        hub_http_response_append(&response, "abc", 3) == ESP_ERR_NO_MEM,
        "allocation failure was not returned"
    );
    fail_allocations = false;
    require_true(
        hub_http_response_append(&response, "d", 1) == ESP_ERR_NO_MEM,
        "first append error was not preserved"
    );
    require_true(response.len == 0, "failed append changed response length");
    hub_http_response_reset(&response);
}

static void test_accumulator_rejects_body_larger_than_limit(void)
{
    hub_http_response_t response = {0};
    const char byte = 'x';
    require_true(
        hub_http_response_append(&response, &byte, 65536) == ESP_ERR_INVALID_SIZE,
        "oversized response was accepted"
    );
    hub_http_response_reset(&response);
}

static void test_finish_rejects_short_content_length(void)
{
    hub_http_response_t response = {0};
    char body[1889];
    memset(body, 'x', sizeof(body));
    require_true(
        hub_http_response_append(&response, body, sizeof(body)) == ESP_OK,
        "short response append failed"
    );
    require_true(
        hub_http_response_finish(&response, 2913, false) == ESP_ERR_INVALID_SIZE,
        "truncated content-length response was accepted"
    );
    hub_http_response_reset(&response);
}

static void test_finish_accepts_complete_content_length(void)
{
    hub_http_response_t response = {0};
    require_true(hub_http_response_append(&response, "abc", 3) == ESP_OK, "append failed");
    require_true(
        hub_http_response_finish(&response, 3, false) == ESP_OK,
        "complete content-length response was rejected"
    );
    hub_http_response_reset(&response);
}

static void test_finish_accepts_chunked_body_without_length(void)
{
    hub_http_response_t response = {0};
    require_true(hub_http_response_append(&response, "abc", 3) == ESP_OK, "append failed");
    require_true(
        hub_http_response_finish(&response, -1, true) == ESP_OK,
        "chunked response without content length was rejected"
    );
    hub_http_response_reset(&response);
}

int main(void)
{
    test_accumulator_keeps_first_append_error();
    test_accumulator_rejects_body_larger_than_limit();
    test_finish_rejects_short_content_length();
    test_finish_accepts_complete_content_length();
    test_finish_accepts_chunked_body_without_length();
    return 0;
}
"""


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="http-response-host-") as temporary:
        temporary_path = pathlib.Path(temporary)
        include_path = temporary_path / "include"
        include_path.mkdir()
        (include_path / "esp_err.h").write_text(textwrap.dedent(ESP_ERR_STUB), encoding="utf-8")
        (include_path / "esp_heap_caps.h").write_text(textwrap.dedent(HEAP_CAPS_STUB), encoding="utf-8")
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
            str(include_path),
            "-I",
            str(MAIN_DIR),
            str(harness_path),
            str(MAIN_DIR / "hub_http_response.c"),
            "-o",
            str(binary_path),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary_path)], cwd=ROOT, check=True)

    print("HTTP response accumulator host tests passed (5 cases)")


if __name__ == "__main__":
    main()
