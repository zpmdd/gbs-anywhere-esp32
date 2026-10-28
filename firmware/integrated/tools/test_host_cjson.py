#!/usr/bin/env python3
"""Check IDF cJSON preference, managed fallback and missing-source failures."""
import os
from pathlib import Path
import tempfile
from unittest.mock import patch

import host_cjson


def main():
    with tempfile.TemporaryDirectory(prefix='hub-cjson-path-') as temporary:
        root = Path(temporary)
        idf = root / 'idf with spaces'
        legacy = idf / 'components/json/cJSON'
        managed = root / 'managed_components/espressif__cjson/cJSON'
        legacy.mkdir(parents=True)
        managed.mkdir(parents=True)
        with patch.object(host_cjson, 'ROOT', root), patch.dict(os.environ, {'IDF_PATH': str(idf)}):
            for directory in (legacy, managed):
                for name in ('cJSON.c', 'cJSON.h'):
                    (directory / name).touch()
            assert host_cjson.cjson_source_dir() == legacy
            (legacy / 'cJSON.c').unlink()
            assert host_cjson.cjson_source_dir() == managed
            (managed / 'cJSON.h').unlink()
            try:
                host_cjson.cjson_source_dir()
            except FileNotFoundError as error:
                assert 'idf.py' in str(error)
            else:
                raise AssertionError('incomplete cJSON sources accepted')
    print('Host cJSON PASS: IDF 5 preference, IDF 6 managed fallback, spaces and incomplete sources')


if __name__ == '__main__':
    main()
