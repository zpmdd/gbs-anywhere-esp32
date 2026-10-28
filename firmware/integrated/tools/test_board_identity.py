#!/usr/bin/env python3
"""Check board identity and Mosaico precedence without the ESP toolchain."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = '#include "hub_version.h"\n#include <stdio.h>\nint main(void) { puts(LAMABUDDY_DEVICE_MODEL); puts(LAMABUDDY_CHIP_NAME); puts(LAMABUDDY_MDNS_HOSTNAME); puts(LAMABUDDY_TRANSPORT_SOURCE); }\n'
    profiles = [
        (['HUB_MOSAICO', 'HUB_AMOLED_216', 'HUB_TOUCHSCREEN', 'HUB_GRINDER'],
         ['lamabuddy-esp32s31-mosaico', 'esp32s31', 'lamabuddy-mosaico', 'esp32-s31']),
        (['HUB_AMOLED_216', 'HUB_TOUCHSCREEN', 'HUB_GRINDER'],
         ['lamabuddy-esp32s3-amoled216', 'esp32s3', 'lamabuddy-amoled216', 'esp32-s3']),
        (['HUB_TOUCHSCREEN', 'HUB_GRINDER'],
         ['lamabuddy-esp32s3-touch', 'esp32s3', 'lamabuddy-touch', 'esp32-s3']),
        (['HUB_GRINDER'], ['lamabuddy-esp32s3-integrated', 'esp32s3', 'lamabuddy-integrated', 'esp32-s3']),
        ([], ['lamabuddy-esp32s3-hub', 'esp32s3', 'lamabuddy-hub', 'esp32-s3']),
    ]
    with tempfile.TemporaryDirectory(prefix='hub-board-identity-') as temporary:
        directory = Path(temporary)
        path = directory / 'identity.c'
        path.write_text(source)
        for defines, expected in profiles:
            output = directory / 'identity'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main'), *['-D' + value for value in defines],
                            str(path), '-o', str(output)], check=True)
            result = subprocess.run([str(output)], check=True, capture_output=True, text=True)
            assert result.stdout.splitlines() == expected
    print('Board identity PASS: Mosaico precedence and all existing S3 profiles')


if __name__ == '__main__':
    main()
