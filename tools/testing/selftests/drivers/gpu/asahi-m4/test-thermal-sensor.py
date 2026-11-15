#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Production PMP C decoder and hotspot aggregation regressions."""
import hashlib
import json
import random
import subprocess
from collections import Counter
from common import HERE, FIXTURES, kernel_root, output_dir, verify_fixtures, sha
from thermal_host import build
ROOT=kernel_root();OUT=output_dir();verify_fixtures()
binary=build(ROOT,OUT,HERE/'check-thermal-sensor.c')

def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True, **kwargs)

unit=run([str(binary),'--selftest']).stdout

cases = []
for raw in range(-2560, 9601):
    for name in range(6):
        cases.append(f'{raw} 1 {name} {name}\n')
for raw in [-2147483648, -2561, -2560, -33, -32, -31, -4, -1, 0, 1, 4, 31, 32, 33, 9600, 9601, 2147483647]:
    for valid in [0, 1, 2, 0xffffffff]:
        for actual in range(6):
            for expected in range(6):
                cases.append(f'{raw} {valid} {actual} {expected}\n')


input_text=''.join(cases)
oracle=json.loads((FIXTURES/'thermal-sensor-oracle.json').read_text())
assert len(cases)==oracle['rows']
assert hashlib.sha256(input_text.encode()).hexdigest()==oracle['input_sha256'], 'Reference input generator changed'
actual=run([str(binary)],input=input_text).stdout
lines=actual.splitlines(keepends=True)
assert len(lines)==len(cases)
for block,expected in enumerate(oracle['blocks']):
 start=block*oracle['block_rows'];end=start+oracle['block_rows']
 observed=hashlib.sha256(''.join(lines[start:end]).encode()).hexdigest()
 assert observed==expected, f'Independent C oracle mismatch in rows {start}..{min(end,len(cases))}; inputs {cases[start:start+3]}'
assert hashlib.sha256(actual.encode()).hexdigest()==oracle['output_sha256']

actual=actual.splitlines()
# Check the expected ties-away-from-zero rule independently for every valid raw.
for index, raw in enumerate(range(-2560, 9601)):
    magnitude = (abs(raw) * 1000 + 32) // 64
    expected = magnitude if raw >= 0 else -magnitude
    assert actual[index * 6] == f'0 {expected}', (raw, actual[index * 6], expected)


report={'passed':True,'scope':'Production PMP C decoder against frozen regression traces and hotspot aggregation checks; no hardware qualification.', 'rows':len(cases), 'production_sha256':sha(ROOT/'include/linux/soc/apple/pmp-temps.h'), 'oracle_sha256':sha(FIXTURES/'thermal-sensor-oracle.json')}
(OUT/'thermal-sensor-host.json').write_text(json.dumps(report,indent=2)+'\n')
print(unit)
print(json.dumps(report,indent=2))
