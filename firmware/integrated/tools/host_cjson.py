#!/usr/bin/env python3
"""Locate the existing cJSON sources for IDF 5 and IDF 6 host checks."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def cjson_source_dir():
    idf = Path(os.environ.get('IDF_PATH', Path.home() / 'esp/esp-idf'))
    for directory in (idf / 'components/json/cJSON',
                      ROOT / 'managed_components/espressif__cjson/cJSON'):
        if all((directory / name).is_file() for name in ('cJSON.c', 'cJSON.h')):
            return directory
    raise FileNotFoundError('cJSON sources unavailable; configure the integrated firmware with idf.py first')


if __name__ == '__main__':
    print(cjson_source_dir())
