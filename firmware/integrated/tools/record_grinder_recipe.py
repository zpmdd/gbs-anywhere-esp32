#!/usr/bin/env python3
"""Capture cached recipe targets, scale observations and cup results without controls."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import time
import urllib.request

from run_ota_update import OPENER, read_token_physical_line, validate_base_url


FIELDS = {
    '/health': ('deviceModel', 'firmwareBuildId', 'uptimeSeconds'),
    '/diagnostics/grinder': (
        'mode', 'phase', 'have_job', 'pending', 'canceling', 'baseline_pending',
        'target_grams', 'target_seconds', 'successes', 'last_outcome',
        'latest_coffee_ms', 'latest_seconds', 'latest_grams',
        'last_success_seconds', 'last_success_grams',
        'wifi', 'grinder', 'cloud', 'storage', 'uptime_s',
    ),
    '/machine/state': (
        'connectionState', 'brewByWeightMode', 'brewByWeightDose1',
        'brewByWeightDose2', 'scaleConnected', 'lastUpdated',
        'thingScaleConnected', 'scaleBatteryLevel', 'scaleObservedMs',
        'scaleCalibrationRequired',
    ),
}


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def project(path, payload):
    result = {key: payload[key] for key in FIELDS[path] if key in payload
              and isinstance(payload[key], (str, bool, int, float, type(None)))}
    for key, value in result.items():
        if isinstance(value, float) and not math.isfinite(value):
            result[key] = None
    # The cached target is cleared when a job ends; idle zero is not a recipe.
    if path == '/diagnostics/grinder':
        active = payload.get('have_job') is True
        grams = payload.get('target_grams')
        valid = type(grams) in (int, float) and math.isfinite(grams) and 0 < grams <= 200
        result['targetStatus'] = 'idle' if not active else 'available' if valid else 'unavailable'
        job = payload.get('grind_uuid')
        if active and isinstance(job, str) and job:
            result['jobRef'] = hashlib.sha256(job.encode()).hexdigest()[:12]
        follow = payload.get('recipe_follow')
        if isinstance(follow, dict):
            result['recipeFollow'] = {key: follow[key] for key in (
                'enabled', 'phase', 'target_grams', 'recipe_index', 'download_bytes', 'elapsed_ms', 'csv_rows',
            ) if key in follow and type(follow[key]) in (str, bool, int)}
            identity = follow.get('grind_uuid')
            if isinstance(identity, str) and identity:
                result['recipeFollow']['jobRef'] = hashlib.sha256(identity.encode()).hexdigest()[:12]
    return result


def read(base_url, token, path):
    if path not in FIELDS:
        raise ValueError('endpoint_not_allowed')
    request = urllib.request.Request(base_url + path, method='GET', headers={
        'Authorization': 'Bearer ' + token, 'Accept': 'application/json',
    })
    started = time.monotonic()
    try:
        with OPENER.open(request, timeout=3) as response:
            raw = response.read(65537)
            if len(raw) > 65536:
                raise ValueError('response_too_large')
            payload = json.loads(raw)
            if not isinstance(payload, dict):
                raise ValueError('invalid_payload')
            return {'status': response.status, 'elapsedMs': round((time.monotonic() - started) * 1000),
                    'data': project(path, payload)}
    except Exception as error:
        return {'status': getattr(error, 'code', 0), 'errorType': type(error).__name__}


def record(base_url, token, output, duration):
    if not 1 <= duration <= 3600:
        raise ValueError('duration_must_be_1_to_3600_seconds')
    health = read(base_url, token, '/health')
    if health.get('data', {}).get('deviceModel') not in (
            'lamabuddy-esp32s3-touch', 'lamabuddy-esp32s3-amoled216', 'lamabuddy-esp32s31-mosaico'):
        raise RuntimeError('touchscreen_device_not_verified')
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    summary = {'startedAt': utc_now(), 'readOnly': True, 'health': health,
               'samples': 0, 'failedReads': 0, 'jobs': {}}
    deadline = time.monotonic() + duration
    print('Read-only capture ready; only cached Hub GET requests will be sent.', flush=True)
    try:
        with (output / 'samples.jsonl').open('x', encoding='utf-8') as stream:
            while time.monotonic() < deadline:
                sample = {'sampledAt': utc_now()}
                for path in ('/diagnostics/grinder', '/machine/state'):
                    sample[path] = read(base_url, token, path)
                stream.write(json.dumps(sample, ensure_ascii=False) + '\n')
                stream.flush()
                summary['samples'] += 1
                for path in ('/diagnostics/grinder', '/machine/state'):
                    if sample[path]['status'] != 200:
                        summary['failedReads'] += 1
                grinder = sample['/diagnostics/grinder'].get('data', {})
                job = grinder.get('jobRef')
                if job:
                    observed = summary['jobs'].setdefault(job, {'firstSeenAt': sample['sampledAt'], 'targetsGrams': []})
                    grams = grinder.get('target_grams')
                    if grinder.get('targetStatus') == 'available' and grams not in observed['targetsGrams']:
                        observed['targetsGrams'].append(grams)
                    follow = grinder.get('recipeFollow', {})
                    if follow.get('jobRef') == job:
                        for key in ('phase', 'target_grams', 'elapsed_ms'):
                            value = follow.get(key)
                            values = observed.setdefault('recipe_' + key, [])
                            if value is not None and value not in values:
                                values.append(value)
                if any(sample[path]['status'] in (401, 403) for path in FIELDS if path in sample):
                    summary['stoppedReason'] = 'authorization_rejected'
                    break
                time.sleep(max(0, min(2, deadline - time.monotonic())))
    except KeyboardInterrupt:
        summary['stoppedReason'] = 'operator_stopped'
    finally:
        summary['finishedAt'] = utc_now()
        (output / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'samples': summary['samples'], 'failedReads': summary['failedReads'],
                      'jobsObserved': len(summary['jobs'])}), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', required=True)
    parser.add_argument('--token-file', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--duration-seconds', type=int, default=900)
    args = parser.parse_args()
    os.umask(0o077)
    try:
        record(validate_base_url(args.base_url), read_token_physical_line(args.token_file, 1),
               args.output, args.duration_seconds)
    except Exception as error:
        parser.exit(1, 'Capture stopped: ' + type(error).__name__ + '\n')


if __name__ == '__main__':
    main()
