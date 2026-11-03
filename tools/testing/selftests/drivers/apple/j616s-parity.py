#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Collect J616s policy snapshots around bounded CPU and memcpy workloads."""
import argparse
import json
import pathlib
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--memcpy', required=True, help='Path to public memcpy-bw binary')
parser.add_argument('--output', required=True, help='New private evidence directory')
args = parser.parse_args()
try:
    compatible = pathlib.Path('/proc/device-tree/compatible').read_bytes()
except OSError as error:
    raise SystemExit(f'Cannot read device identity: {error.strerror}')
if b'apple,j616s' not in compatible.split(b'\0'):
    raise SystemExit('Device identity is not apple,j616s')
output = pathlib.Path(args.output)
output.mkdir(parents=True, exist_ok=False)

# Read only documented policy, cached IIO readings, and requested clock attributes.
paths = sorted(set(
    list(pathlib.Path('/sys/kernel/debug/apple-pmc').glob('*/status')) +
    list(pathlib.Path('/sys/kernel/debug').glob('*performance-controller*/status')) +
    list(pathlib.Path('/sys/devices/system/cpu/cpufreq').glob('policy*/scaling_cur_freq'))
))
als = []
for device in pathlib.Path('/sys/bus/iio/devices').glob('iio:device*'):
    try:
        if (device / 'name').read_text().strip() == 'aop-sensors-als':
            als += [device / 'in_illuminance_input', device / 'in_colortemp_input']
    except OSError:
        pass
paths += als

def read(path):
    try:
        return path.read_text().strip()
    except OSError as error:
        return {'error': error.strerror, 'errno': error.errno}

identity = {'kernel': subprocess.check_output(['uname', '-r'], text=True).strip(),
            'compatible': compatible.decode(errors='replace').split('\0'),
            'selected_paths': [str(path) for path in paths],
            'dram': {}}
for group in pathlib.Path('/sys/devices/platform').rglob('dram'):
    for name in ['type', 'state_count', 'data_bus_width', 'max_data_rate', 'peak_bandwidth', 'bandwidth_class']:
        identity['dram'][str(group / name)] = read(group / name)
(output / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
children = []
with (output / 'snapshots.jsonl').open('x') as log:
    def sample(phase):
        record = {'monotonic_ns': time.monotonic_ns(), 'phase': phase,
                  'values': {str(path): read(path) for path in paths}}
        log.write(json.dumps(record) + '\n')
        log.flush()

    def observe(phase, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            sample(phase)
            time.sleep(0.05)

    try:
        observe('idle', 2)
        with open('/dev/null', 'wb') as null:
            for _ in range(14):
                children.append(subprocess.Popen(['yes'], stdout=null))
            observe('all-core', 5)
        for child in children:
            child.terminate()
        for child in children:
            child.wait()
        children = []
        observe('cpu-recovery', 2)
        with (output / 'memcpy.txt').open('x') as metrics:
            child = subprocess.Popen([args.memcpy, '8', '256', '10'], stdout=metrics, stderr=subprocess.STDOUT)
            children = [child]
            deadline = time.monotonic() + 60
            while child.poll() is None and time.monotonic() < deadline:
                sample('memcpy-including-initialization')
                time.sleep(0.05)
            if child.poll() is None:
                raise RuntimeError('memcpy did not finish within 60 seconds')
            (output / 'memcpy-exit.txt').write_text(str(child.returncode) + '\n')
            children = []
        observe('memory-recovery', 2)
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
