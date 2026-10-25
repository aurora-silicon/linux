#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Shared production C policy and Rust scheduling/selector regressions."""
import hashlib
import json
import random
import subprocess
from collections import Counter
from common import HERE, FIXTURES, kernel_root, output_dir, rustc, verify_fixtures, sha
from thermal_host import build
ROOT=kernel_root();OUT=output_dir();verify_fixtures()
source=(HERE/'check-thermal-policy.rs').read_text().replace('@KERNEL@',str(ROOT))
check=OUT/'check.rs';check.write_text(source)
binary=build(ROOT,OUT,HERE/'check-thermal-policy.c');unit_binary=OUT/'thermal-policy-unit'

def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True, **kwargs)

run(rustc()+['--edition=2021','--test','-Dwarnings',str(check),'-o',str(unit_binary)])
unit=run([str(unit_binary)]).stdout

cases = []
labels = Counter()
MASK = (1 << 64) - 1

def add(label, op, now=0, mc=90000, error=0, gov=0, power=0, configured=9, cap=9, user=9, query=None):
    now &= MASK
    if query is None:
        query = now
    assert -40000 <= mc <= 150000
    cases.append(f'{op} {now} {mc} {error} {gov} {power} {configured} {cap} {user} {query & MASK}\n')
    labels[label] += 1

# Independent boundary sweeps cover every fixed-point sensor temperature.
for raw in range(-2560, 9601):
    scaled = raw * 1000
    mc = (scaled + 32) // 64 if scaled >= 0 else -((-scaled + 32) // 64)
    add('all-valid-sensor-temperatures', 'H', mc=mc)
# Complete selector boundary cross-product includes zero and u32::MAX.
for configured in [0, 1, 2, 8, 9, 10, 255, 0xffffffff]:
    for cap in [0, 1, 2, 8, 9, 10, 255, 0xffffffff]:
        for user in [0, 1, 2, 8, 9, 10, 255, 0xffffffff]:
            add('selector-cross-product', 'L', configured=configured, cap=cap, user=user)

# Make time boundaries independently visible, including repeated sensor reads,
# 5ms polling, 20ms confirmation, 50ms prediction, 200ms release, 250ms stale.
for offset in [0, 1, 4_999_999, 5_000_000, 5_000_001, 19_999_999, 20_000_000,
               20_000_001, 49_999_999, 50_000_000, 50_000_001, 199_999_999,
               200_000_000, 200_000_001, 249_999_999, 250_000_000, 250_000_001]:
    add('time-boundary', 'R')
    add('time-boundary', 'S', now=1_000_000_000, error=-5)
    add('time-boundary', 'D', now=1_000_000_000 + offset)
    for n in range(1, 11):
        add('time-boundary', 'S', now=1_000_000_000 + n * 20_000_000)
    add('time-boundary', 'S', now=1_200_000_000 + offset, mc=105000)
    add('time-boundary', 'S', now=1_450_000_000 + offset, mc=107000)
    add('time-boundary', 'C', now=1_450_000_000 + offset)

# 2048 deterministic sequences: all threshold neighborhoods and all optional
# provider requests, each with startup, persistent/noisy heat, errors and release.
thresholds = [-40000, 0, 104999, 105000, 105999, 106000, 106001, 107999,
              108000, 108001, 108999, 109000, 110000, 112000, 114999, 115000,
              115001, 150000]
for seq in range(2048):
    add('deterministic-sequence-reset', 'R')
    now = 1_000_000_000
    for n in range(10):
        now += 20_000_000
        add('deterministic-startup', 'S', now=now, mc=90000)
    for step in range(40):
        now += [0, 1, 4_999_999, 5_000_000, 20_000_000, 49_999_999, 50_000_000,
                199_999_999, 200_000_000, 249_999_999, 250_000_000, 2_000_000_000][(seq + step) % 12]
        mc = thresholds[(seq * 11 + step) % len(thresholds)]
        error = [-5, -19, 1][step % 3] if (seq + step) % 23 == 0 else 0
        add('deterministic-sample', 'S', now=now, mc=mc, error=error,
            query=now + [0, 4_999_999, 5_000_000][step % 3])
        add('deterministic-ceiling', 'C', now=now, mc=mc, gov=(seq + step) % 10,
            power=(seq // 10 + step) % 10)
    for step in range(20):
        now += 200_000_000
        add('deterministic-cool-recovery', 'S', now=now, mc=90000)
        add('deterministic-cool-recovery', 'C', now=now)

# 2048 randomized sequences, seeded and reproducible. Both always-sample and
# due-only wrappers are exercised so missing intervals and observer calls differ.
rng = random.Random(0x4D34544845524D41)
for seq in range(2048):
    add('random-sequence-reset', 'R')
    # Occasional u64 wrap is supplemental: all wrapping deltas still monotonic.
    now = (MASK - 2_000_000_000) if seq % 64 == 0 else rng.randrange(1, 100_000_000_000)
    sample_ns = 0
    last_mc = 90000
    for step in range(64):
        now = (now + rng.choice([0, 1, 1_000, 1_000_000, 4_999_999, 5_000_000,
                                19_999_999, 20_000_000, 50_000_000, 200_000_000,
                                250_000_000, rng.randrange(500_000_000, 10_000_000_000)])) & MASK
        if step % 8 == 0:
            last_mc = rng.randrange(-40000, 150001)
        else:
            last_mc = max(-40000, min(150000, last_mc + rng.randrange(-3000, 3001)))
        error = rng.choice([-110, -5, -19, 1]) if rng.randrange(11) == 0 else 0
        should_sample = seq % 2 == 0 or sample_ns == 0 or ((now - sample_ns) & MASK) >= 5_000_000
        if should_sample:
            add('random-sample', 'S', now=now, mc=last_mc, error=error,
                query=now + rng.choice([0, 4_999_999, 5_000_000, 5_000_001]))
            sample_ns = now
        else:
            add('random-read-throttle', 'D', now=now, mc=last_mc)
        if rng.randrange(5) == 0:
            add('random-governor-observe', 'O', now=now, gov=rng.choice(list(range(9)) + [9, 99, 0xffffffff]))
        gov = rng.choice(list(range(9)) + [9, 99, 0xffffffff])
        power = rng.choice(list(range(9)) + [9, 99, 0xffffffff])
        add('random-ceiling', 'C', now=now, mc=last_mc, gov=gov, power=power,
            configured=rng.randrange(0, 16), cap=rng.randrange(0, 16), user=rng.randrange(0, 16),
            query=now + rng.choice([0, 4_999_999, 5_000_000, 5_000_001]))



input_text=''.join(cases)
oracle=json.loads((FIXTURES/'thermal-policy-oracle.json').read_text())
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

field_names = ['last_ns', 'good_ns', 'anchor_ns', 'release_ns', 'dt_ns', 'anchor_mc',
               'previous_mc', 'projected_mc', 'good_reads', 'prediction', 'level', 'hard',
               'anchor_valid', 'reason', 'due_before', 'due_after', 'ceiling', 'ceiling_reason',
               'final_limit', 'heat_level']
# Independent expected-value assertions, rather than only C-vs-Rust agreement.
def trace(ops):
    output = run([str(binary)], input=''.join(ops)).stdout.splitlines()
    return [dict(zip(field_names, row.split())) for row in output]

def operation(op, now=0, mc=90000, error=0, gov=0, power=0, configured=9, cap=9, user=9, query=None):
    if query is None: query = now
    return f'{op} {now} {mc} {error} {gov} {power} {configured} {cap} {user} {query}\n'

# Sentinel last_ns=0 always reads; errors store their attempted read time;
# read_due does not itself mutate state or advance a sample.
poll = trace([operation('R'), operation('D', now=0), operation('S', now=100_000_000, error=-5),
              operation('D', now=104_999_999), operation('D', now=105_000_000)])
assert poll[0]['due_after'] == poll[1]['due_after'] == '1'
assert poll[2]['last_ns'] == poll[3]['last_ns'] == poll[4]['last_ns'] == '100000000'
assert poll[2]['due_after'] == poll[3]['due_after'] == '0'
assert poll[4]['due_after'] == '1'
assert poll[2]['reason'] == 'sensor' and poll[2]['hard'] == '1'

# More reads at the same time cannot accelerate confirmation or release.
ops = [operation('R')] + [operation('S', now=20_000_000)] * 10000
repeat = trace(ops)
assert repeat[-1]['good_reads'] == '1' and repeat[-1]['hard'] == '1'
ops = [operation('R')] + [operation('S', now=n * 20_000_000) for n in range(1, 11)]
ops += [operation('S', now=399_999_999), operation('S', now=400_000_000),
        operation('S', now=600_000_000), operation('S', now=601_000_000, error=-5)]
confirm = trace(ops)
assert confirm[9]['good_reads'] == '9' and confirm[9]['hard'] == '1'
assert confirm[10]['good_reads'] == '10' and confirm[10]['hard'] == '0'
assert [confirm[n]['level'] for n in (10, 11, 12, 13, 14)] == ['8', '8', '7', '6', '8']
assert confirm[14]['good_reads'] == '0' and confirm[14]['anchor_valid'] == '0'
assert confirm[14]['projected_mc'] == '0' and confirm[14]['reason'] == 'sensor'

# Reach fully released level, then prove prediction is one step while measured
# heat is immediate; idle gaps invalidate a slope; governor restriction remains.
ops = [operation('R')] + [operation('S', now=n * 20_000_000) for n in range(1, 11)]
ops += [operation('S', now=200_000_000 + n * 200_000_000) for n in range(1, 9)]
ops += [operation('S', now=2_200_000_000, mc=106000), operation('S', now=2_250_000_000, mc=107000),
        operation('S', now=2_300_000_000, mc=112000), operation('S', now=2_700_000_000, mc=110000),
        operation('C', now=2_700_000_000, gov=8), operation('C', now=2_700_000_000),
        operation('S', now=2_701_000_000, mc=115000), operation('C', now=2_701_000_000)]
projection = trace(ops)
assert projection[18]['level'] == '0'
assert projection[20]['projected_mc'] == '109000' and projection[20]['prediction'] == '2'
assert projection[20]['level'] == '1' and projection[20]['reason'] == 'prediction'
assert projection[21]['level'] == '5' and projection[21]['reason'] == 'measured-heat'
assert projection[22]['dt_ns'] == '0' and projection[22]['prediction'] == '0'
assert projection[23]['ceiling'] == projection[24]['ceiling'] == '1'
assert projection[23]['reason'] == projection[24]['reason'] == 'governor'
assert projection[-1]['ceiling'] == '1' and projection[-1]['ceiling_reason'] == 'measured-heat'



report={'passed':True,'scope':'Shared production C cooling policy against frozen regression traces, plus production Rust scheduling/selector units; no hardware qualification.', 'rows':len(cases), 'production_sha256':sha(ROOT/'include/linux/soc/apple/pmp-cooling.h'), 'rust_composition_sha256':sha(ROOT/'drivers/gpu/drm/asahi/g16/power/policy.rs'), 'oracle_sha256':sha(FIXTURES/'thermal-policy-oracle.json')}
(OUT/'thermal-policy-host.json').write_text(json.dumps(report,indent=2)+'\n')
print(unit)

# The governor's accounting is policy::Governor, run by the units above. These
# are the calls that connect it: publication, drain, the power service with
# whether flights are outstanding, and the idle poll's re-arm.
G16 = ROOT/'drivers/gpu/drm/asahi/g16'
for path, call in [
        ('engine.rs', 'self.power.utilization.busy(now);'),
        ('poll.rs', 'self.power.utilization.idle(self.engine.stats.idle_from_ns);'),
        ('power.rs', '.govern(now, limit, !self.engine.flights.is_empty());'),
        ('poll.rs', 'Ok(false) if firmware.power.utilization.decaying() => {\n                        self.mod_poll('),
]:
    assert (G16/path).read_text().count(call) == 1, (path, call)
print('Governor connected at publication, drain, power service and idle re-arm')
print(json.dumps(report,indent=2))

# Read/reattach interleavings exercise the provider's actual epoch state helpers.
lease_source = r'''
#include <assert.h>
#include <stdbool.h>
#include "@HEADER@"
int main(void) {
    struct apple_pmp_gpu_lease s = {};
    for (unsigned int i=0;i<1000;i++) {
        unsigned long long old=apple_pmp_gpu_lease_attach(&s);
        assert(old && !apple_pmp_gpu_lease_attach(&s));
        assert(apple_pmp_gpu_lease_level(&s,old,1000000000ULL)==8);
        apple_pmp_gpu_lease_publish(&s,old,0,1000000000ULL);
        assert(apple_pmp_gpu_lease_level(&s,old,1000000000ULL)==0);
        apple_pmp_gpu_lease_detach(&s,old);
        unsigned long long next=apple_pmp_gpu_lease_attach(&s);
        assert(next>old);
        apple_pmp_gpu_lease_publish(&s,old,0,1000000000ULL);
        apple_pmp_gpu_lease_detach(&s,old);
        assert(s.attached && apple_pmp_gpu_lease_level(&s,next,1000000000ULL)==8);
        apple_pmp_gpu_lease_publish(&s,next,1,1000000000ULL);
        assert(apple_pmp_gpu_lease_level(&s,next,1000000000ULL)==1);
        assert(apple_pmp_gpu_lease_level(&s,old,1000000000ULL)==8);
        assert(apple_pmp_gpu_lease_level(&s,next,4001000000ULL)==8);
        apple_pmp_gpu_lease_publish(&s,0,8,0);
        assert(apple_pmp_gpu_lease_level(&s,next,1000000000ULL)==8);
        apple_pmp_gpu_lease_detach(&s,next);
    }
    s.epoch=~0ULL;
    assert(!apple_pmp_gpu_lease_attach(&s));
    assert(!s.attached);
    return 0;
}
'''.replace('@HEADER@',str(ROOT/'include/linux/soc/apple/pmp-power-policy.h'))
lease_unit=OUT/'gpu-lease.c';lease_unit.write_text(lease_source)
run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',str(lease_unit),'-o',str(OUT/'gpu-lease')])
run([str(OUT/'gpu-lease')])
print('GPU attach epochs reject stale worker publications and stale detach, begin restricted, expire budgets and never wrap')
