#!/usr/bin/env python3
"""Exercise the real recorder against a local read-only synthetic Hub."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import tempfile
import threading

from record_grinder_recipe import FIELDS, project, read, record


def main():
    path = '/diagnostics/grinder'
    for grams in (0, -2147483648, None, True, float('nan'), 201):
        projected = project(path, {'have_job': True, 'target_grams': grams})
        assert projected['targetStatus'] == 'unavailable'
        json.dumps(projected, allow_nan=False)
    assert project(path, {'have_job': False, 'target_grams': 0})['targetStatus'] == 'idle'
    assert project(path, {'have_job': True, 'target_grams': 36.5})['targetStatus'] == 'available'
    follow = project(path, {'recipe_follow': {'enabled': True, 'phase': 'applied', 'target_grams': 30,
        'grind_uuid': 'private-uuid', 'ownerPassword': 'private-password', 'elapsed_ms': 1020}})['recipeFollow']
    assert follow['target_grams'] == 30 and follow['elapsed_ms'] == 1020
    assert 'private-' not in json.dumps(follow) and 'ownerPassword' not in follow
    scale = {'scaleConnected': False, 'thingScaleConnected': True,
             'scaleBatteryLevel': 1, 'scaleObservedMs': 12345,
             'scaleCalibrationRequired': False}
    assert project('/machine/state', scale) == scale
    unknown_scale = {key: None for key in scale}
    unknown_scale['scaleObservedMs'] = 12345
    assert project('/machine/state', unknown_scale) == unknown_scale

    class Hub(BaseHTTPRequestHandler):
        calls = []
        redirect = False
        model = 'lamabuddy-esp32s3-touch'

        def log_message(self, *args):
            pass

        def do_GET(self):
            self.calls.append((self.command, self.path))
            assert self.headers['Authorization'] == 'Bearer synthetic'
            if self.redirect:
                self.send_response(302)
                self.send_header('Location', '/controls/power')
                self.end_headers()
                return
            data = {
                '/health': {'deviceModel': self.model, 'firmwareBuildId': '12345678'},
                path: {'have_job': True, 'grind_uuid': 'synthetic-job-identity',
                       'target_grams': 36, 'target_seconds': 30, 'successes': 8},
                '/machine/state': {'brewByWeightMode': 'Dose1', 'brewByWeightDose1': 20,
                                   **scale, 'scaleName': 'private-scale-name'},
            }[self.path]
            data.update(token='must-not-be-saved', serialNumber='private-serial')
            self.send_response(200)
            self.end_headers()
            self.wfile.write(json.dumps(data).encode())

    server = ThreadingHTTPServer(('127.0.0.1', 0), Hub)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    old_umask = os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'capture'
            summary = record(base, 'synthetic', output, 1)
            assert summary['samples'] == 1 and summary['failedReads'] == 0
            assert list(summary['jobs'].values())[0]['targetsGrams'] == [36]
            saved = ''.join(p.read_text() for p in output.iterdir())
            for secret in ('must-not-be-saved', 'private-serial', 'synthetic-job-identity',
                           'private-scale-name'):
                assert secret not in saved
            sample = json.loads((output / 'samples.jsonl').read_text().splitlines()[0])
            captured_scale = sample['/machine/state']['data']
            assert all(captured_scale[key] == value for key, value in scale.items())
            assert all(p.stat().st_mode & 0o777 == 0o600 for p in output.iterdir())
            assert Hub.calls == [('GET', p) for p in FIELDS]
            try:
                read(base, 'synthetic', '/controls/power')
            except ValueError:
                pass
            else:
                raise AssertionError('control endpoint accepted')
            Hub.redirect = True
            assert read(base, 'synthetic', path)['status'] == 302
            assert Hub.calls[-1] == ('GET', path) and len(Hub.calls) == 4
            Hub.redirect = False
            for model in ('lamabuddy-esp32s3-amoled216', 'lamabuddy-esp32s31-mosaico'):
                Hub.model = model
                Hub.calls = []
                capture = record(base, 'synthetic', Path(temp) / model, 1)
                assert capture['health']['data']['deviceModel'] == Hub.model
                assert capture['samples'] == 1 and capture['failedReads'] == 0
                assert Hub.calls == [('GET', p) for p in FIELDS]
            for model in ('lamabuddy-esp32s3-headless', 'amoled216',
                          'lamabuddy-esp32s3-amoled216-other', 'lamabuddy-esp32s31-mosaico-other', 'unknown', None):
                Hub.model = model
                Hub.calls = []
                output = Path(temp) / 'wrong-device'
                try:
                    record(base, 'synthetic', output, 1)
                except RuntimeError:
                    pass
                else:
                    raise AssertionError('wrong device accepted')
                assert not output.exists()
                assert Hub.calls == [('GET', '/health')]
    finally:
        os.umask(old_umask)
        server.shutdown()
        server.server_close()
    print('Recipe capture PASS: target semantics, real GET-only flow, redaction, permissions, redirects and device identity')


if __name__ == '__main__':
    main()
