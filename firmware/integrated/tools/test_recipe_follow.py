#!/usr/bin/env python3
"""Run the automatic recipe transaction against in-memory services."""
from pathlib import Path
import subprocess
import tempfile
import test_command_coordinator as coordinator
from host_cjson import cjson_source_dir

ROOT = Path(__file__).resolve().parents[1]
NVS = '''#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);
'''


def main():
    json_dir = cjson_source_dir()
    with tempfile.TemporaryDirectory(prefix='gbs-follow-test-') as directory:
        root = Path(directory); (root / 'freertos').mkdir()
        for name, data in {'esp_err.h': coordinator.ESP_ERR_STUB + '\n#define ESP_ERR_NOT_FINISHED 0x109\nconst char *esp_err_to_name(esp_err_t);\n',
                           'esp_timer.h': coordinator.TIMER_STUB,
                           'freertos/FreeRTOS.h': coordinator.FREERTOS_STUB,
                           'freertos/semphr.h': coordinator.SEMPHR_STUB,
                           'nvs.h': NVS}.items():
            (root / name).write_text(data)
        exe = root / 'follow'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                        '-fsanitize=address,undefined', '-g', '-I', str(root), '-I', str(ROOT / 'main'),
                        '-I', str(ROOT / 'integrated/main'), '-I', str(json_dir),
                        str(ROOT / 'integrated/tests/test_recipe_follow.c'), str(ROOT / 'integrated/main/logic.c'),
                        str(json_dir / 'cJSON.c'), '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
