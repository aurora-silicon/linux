#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for M5 submission_effort() honoring BusyRuns::bound alongside waited.

Extracts production `submission_effort()` from `drivers/gpu/drm/asahi/g17/initdata.rs`
and `Effort` / `BusyRuns` from `drivers/gpu/drm/asahi/g17/power.rs`, verifying:
1. M5 publishes FULL_EFFORT when a client is continuously GPU-bound (`bound == true`)
   even when `waited == false` (pipelined submission loops).
2. M5 still publishes FULL_EFFORT when `waited == true` (submit-and-wait loops).
3. M5 publishes FULL_EFFORT when `compute_waited == true` (scoped pure-compute syncobj waits).
4. M5 publishes 0 when none of `waited`, `bound`, or `compute_waited` hold (paced work with
   idle gaps >= 2 ms) and decays cleanly back to 0 after IDLE_HOLD_NS of idle.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def build_harness(mutation=None):
    init_src = (G17 / 'initdata.rs').read_text()
    power_src = (G17 / 'power.rs').read_text()

    sub_fn = item(init_src, 'pub(super) fn submission_effort(').replace('pub(super) ', '')
    if mutation == 'm5-ignores-bound':
        sub_fn = sub_fn.replace('if waited || bound', 'if waited', 1)

    effort_struct = item(power_src, 'pub(super) struct Effort')
    effort_impl = item(power_src, 'impl Effort')
    busy_struct = item(power_src, 'pub(super) struct BusyRuns')
    busy_run_struct = item(power_src, 'struct BusyRun ')
    busy_run_impl = item(power_src, 'impl BusyRun ')
    busy_impl = item(power_src, 'impl BusyRuns')

    parts = '\n'.join([
        effort_struct, effort_impl, busy_run_struct, busy_run_impl, busy_struct, busy_impl,
    ]).replace('pub(super) ', '')

    return f'''
const FULL_EFFORT: u32 = 0x10000;
const IDLE_HOLD_NS: u64 = 16_000_000;
const CLIENT_RUNS: usize = 16;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum IdleAction {{ None, Clear, Recheck(u64) }}

{parts}

struct MockInitData {{
    m5: bool,
    active_effort: u32,
}}

impl MockInitData {{
    {sub_fn}
}}

fn main() {{
    let m5 = MockInitData {{ m5: true, active_effort: 0 }};
    let g17p = MockInitData {{ m5: false, active_effort: 0x5555 }};

    // Direct policy matrix on M5 vs non-M5.
    assert_eq!(m5.submission_effort(FULL_EFFORT, false, false, false), 0);
    assert_eq!(m5.submission_effort(FULL_EFFORT, false, false, true), FULL_EFFORT);
    assert_eq!(m5.submission_effort(FULL_EFFORT, true, false, false), FULL_EFFORT);
    assert_eq!(m5.submission_effort(FULL_EFFORT, false, true, false), FULL_EFFORT);
    assert_eq!(m5.submission_effort(FULL_EFFORT, true, true, false), FULL_EFFORT);
    assert_eq!(m5.submission_effort(0, true, true, true), 0);

    assert_eq!(g17p.submission_effort(FULL_EFFORT, false, false, false), 0x5555);
    assert_eq!(g17p.submission_effort(FULL_EFFORT, false, true, false), FULL_EFFORT);
    assert_eq!(g17p.submission_effort(FULL_EFFORT, false, false, true), FULL_EFFORT);

    // End-to-end pipelined compute loop on M5 without host waiters:
    // 24 MHz clock -> idle_ticks = 2 ms = 48_000 ticks, hold_ticks = 16 ms = 384_000 ticks.
    let idle_ticks = 48_000u64;
    let hold_ticks = 384_000u64;
    let mut runs = BusyRuns::new();
    let mut effort = Effort::new();

    effort.submit();
    let mut tick = 100_000u64;
    for _ in 0..500 {{
        // 40 us execution + 10 us gap (< 2 ms idle_ticks) -> sustained busy run (> 20 ms total).
        runs.note(7, tick, tick + 960, idle_ticks);
        tick += 1_200;
    }}
    assert!(runs.bound(tick, hold_ticks, idle_ticks));
    let changed = effort.note_bound(true);
    assert!(changed);
    assert_eq!(
        m5.submission_effort(
            FULL_EFFORT,
            effort.waited(),
            effort.bound(),
            effort.compute_waited(),
        ),
        FULL_EFFORT,
        "M5 must publish FULL_EFFORT when BusyRuns::bound is true even without a waiter",
    );

    // Idle decay clears streaming, bound, and effort cleanly back to 0.
    assert_eq!(effort.idle(1_000_000, false), IdleAction::Recheck(IDLE_HOLD_NS));
    assert_eq!(effort.idle(1_000_000 + IDLE_HOLD_NS, false), IdleAction::Clear);
    runs.end();
    assert!(!effort.bound());
    assert!(!runs.bound(tick, hold_ticks, idle_ticks));
    assert_eq!(
        m5.submission_effort(0, effort.waited(), effort.bound(), effort.compute_waited()),
        0,
    );

    // Paced work with 5 ms idle gaps stays unbound and publishes 0 on M5.
    let mut paced = 1_000_000u64;
    for _ in 0..20 {{
        runs.note(9, paced, paced + 24_000, idle_ticks);
        paced += 120_000; // 5 ms > 2 ms idle_ticks
    }}
    assert!(!runs.bound(paced, hold_ticks, idle_ticks));
    assert_eq!(m5.submission_effort(FULL_EFFORT, false, false, false), 0);

    println!("PASS g17-m5-effort-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['m5-ignores-bound'])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-m5-eff-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
