#!/usr/bin/env python3
"""Verify that a plausible RTC date after wake still requires a new SNTP sync."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

from validate_stability_foundation import function_body

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM };
#define pdMS_TO_TICKS(ms) (ms)
typedef struct { bool wait_for_sync; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) ((esp_sntp_config_t){0})
static bool sntp_initialized, valid_date;
static atomic_bool sntp_synchronized, sntp_failed;
static unsigned init_calls, wait_calls, sync_at;
static esp_err_t init_error;
static time_t fake_time(time_t *out)
{
    time_t now = valid_date ? 1793500000 : 0;
    if (out) *out = now;
    return now;
}
#define time fake_time
static esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(config->wait_for_sync); ++init_calls; return init_error;
}
static esp_err_t esp_netif_sntp_sync_wait(int ticks)
{
    assert(ticks == 2000 && sntp_initialized); ++wait_calls;
    return sync_at && wait_calls >= sync_at ? ESP_OK : ESP_ERR_TIMEOUT;
}
/* PRODUCTION */
static void reset(void)
{
    sntp_initialized = sntp_synchronized = false;
    sntp_failed = false;
    valid_date = true; init_calls = wait_calls = sync_at = 0; init_error = ESP_OK;
}
int main(void)
{
    reset(); sync_at = 1;
    assert(!strcmp(hub_cloud_time_status(), "syncing"));
    assert(ensure_time_synced() == ESP_OK);
    assert(init_calls == 1 && wait_calls == 1 && sntp_synchronized);
    assert(!strcmp(hub_cloud_time_status(), "synchronized"));
    assert(ensure_time_synced() == ESP_OK && init_calls == 1 && wait_calls == 1);
    reset(); // Deep sleep resets RAM but retains a plausible, potentially wrong RTC date.
    assert(ensure_time_synced() == ESP_ERR_TIMEOUT);
    assert(init_calls == 1 && wait_calls == 15 && !sntp_synchronized);
    assert(!strcmp(hub_cloud_time_status(), "failed"));
    sync_at = 16;
    assert(ensure_time_synced() == ESP_OK && init_calls == 1 && wait_calls == 16);
    assert(!strcmp(hub_cloud_time_status(), "synchronized"));
    reset(); init_error = ESP_ERR_NO_MEM;
    assert(ensure_time_synced() == ESP_ERR_NO_MEM && !sntp_initialized && !wait_calls);
    assert(!strcmp(hub_cloud_time_status(), "failed"));
    init_error = ESP_OK; sync_at = 2;
    assert(ensure_time_synced() == ESP_OK && init_calls == 2 && wait_calls == 2);
    reset(); valid_date = false; sync_at = 1;
    assert(ensure_time_synced() == ESP_ERR_TIMEOUT && !sntp_synchronized);
    valid_date = true;
    assert(ensure_time_synced() == ESP_OK && sntp_synchronized);
    valid_date = false;
    assert(strcmp(hub_cloud_time_status(), "synchronized"));
    puts("Cloud time PASS: every boot requires SNTP evidence, bounded failures and same-boot reuse");
}
'''


def main():
    source = (ROOT / "main/hub_cloud.c").read_text()
    functions = []
    for name in ("time_is_reasonable", "hub_cloud_time_status", "ensure_time_synced"):
        signature = re.search(r"(?m)^[\w* ]+\b" + name + r"\([^;{]*\)\s*\{", source)
        assert signature, name
        functions.append(signature.group(0) + function_body(source, name) + "}\n")
    with tempfile.TemporaryDirectory(prefix="cloud-time-") as temp:
        folder = Path(temp)
        (folder / "test.c").write_text(HARNESS.replace("/* PRODUCTION */", "\n".join(functions)))
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
            str(folder / "test.c"), "-o", str(folder / "test")], check=True)
        subprocess.run([str(folder / "test")], check=True)


if __name__ == "__main__":
    main()
