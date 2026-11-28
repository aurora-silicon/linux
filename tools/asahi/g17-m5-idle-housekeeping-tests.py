#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for deferred idle housekeeping cadence in `runtime_event_turn()`.

Compiles the real `HOUSEKEEPING_NS`, `housekeeping_due()`, and
`deferred_housekeeping_delay()` helpers from
`drivers/gpu/drm/asahi/g17/runtime/events.rs` on the host and tests:
1. Sparse completions where `in_flight` transiently drops to `false` within
   `< HOUSEKEEPING_NS` take `fast_done`, do NOT run housekeeping/power MMIO,
   pass actual `in_flight` to `idle_check(now, in_flight)`, and arm the
   deferred housekeeping timer within `<= HOUSEKEEPING_NS`.
2. Once `HOUSEKEEPING_NS` elapses (or `requested`, `recover`,
   `recovery_pending`, `teardown_pending`, or any capacity waiter is active),
   housekeeping runs and `deferred_housekeeping_delay()` returns `None`.
3. While `in_flight` stays `true`, `fast_done` skips housekeeping without
   arming the deferred housekeeping timer.
4. Structural verification of `runtime/events.rs`, `mod.rs`, and `sleep.rs`:
   `HOUSEKEEPING_WORK` is cancelled on sleep and disabled on shutdown, and
   `runtime_event_turn()` passes actual `in_flight` to `idle_check()` and
   arms `queue_housekeeping()` whenever `!in_flight` defers housekeeping.
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
    events_src = (G17 / 'runtime/events.rs').read_text()
    mod_src = (G17 / 'mod.rs').read_text()
    sleep_src = (G17 / 'sleep.rs').read_text()

    hk_const = 'const HOUSEKEEPING_NS: u64 = 1_000_000;'
    assert hk_const in events_src
    hk_due = item(events_src, 'fn housekeeping_due(')
    hk_delay = item(events_src, 'fn deferred_housekeeping_delay(')
    turn = item(events_src, 'fn runtime_event_turn(')

    if mutation == 'idle-forces-housekeeping':
        hk_due = hk_due.replace(
            'now.saturating_sub(last_housekeeping_ns) >= HOUSEKEEPING_NS',
            'true',
            1,
        )
    elif mutation == 'no-deferred-timer':
        hk_delay = hk_delay.replace(
            'if in_flight || housekeeping_ran {\n        return None;\n    }',
            'return None;',
            1,
        )
    elif mutation == 'hardcode-in-flight-true':
        turn = turn.replace(
            'firmware.idle_check(now, in_flight)?',
            'firmware.idle_check(now, true)?',
            1,
        )
    elif mutation == 'skip-cadence-expiry':
        hk_due = hk_due.replace(
            '|| now.saturating_sub(last_housekeeping_ns) >= HOUSEKEEPING_NS',
            '',
            1,
        )

    fast_idle_ok = (
        'firmware.idle_check(now, in_flight)?' in turn
        and 'firmware.idle_check(now, true)?' not in turn
    )
    fast_timer_ok = turn.count('self.queue_housekeeping(delay);') == 2
    no_idle_force_ok = (
        '|| !in_flight' not in turn
        and 'let no_housekeeping = in_flight' not in turn
    )
    lifecycle_ok = (
        'self.shared.disable_timer(&self.shared.housekeeping_timer);' in mod_src
        and 'self.cancel_timer(&self.housekeeping_timer);' in sleep_src
        and 'impl WorkItem<HOUSEKEEPING_WORK> for Shared' in mod_src
    )

    return f'''
{hk_const}

{hk_due}

{hk_delay}

struct TurnCounters {{
    fast_done: usize,
    housekeeping_runs: usize,
    power_mmio_reads: usize,
    idle_check_in_flight_false: usize,
    deferred_timer_arms: usize,
    last_deferred_delay_ns: Option<u64>,
}}

struct MockEventLoop {{
    last_hk: u64,
    counters: TurnCounters,
}}

impl MockEventLoop {{
    fn new(last_hk: u64) -> Self {{
        Self {{
            last_hk,
            counters: TurnCounters {{
                fast_done: 0,
                housekeeping_runs: 0,
                power_mmio_reads: 0,
                idle_check_in_flight_false: 0,
                deferred_timer_arms: 0,
                last_deferred_delay_ns: None,
            }},
        }}
    }}

    fn run_compute_completion(
        &mut self,
        now: u64,
        in_flight: bool,
        requested: bool,
        recover: bool,
        recovery_pending: bool,
        teardown_pending: bool,
        has_waiters: bool,
    ) {{
        let no_hk = !housekeeping_due(
            requested,
            recover,
            recovery_pending,
            teardown_pending,
            has_waiters,
            now,
            self.last_hk,
        );
        if !recover && !requested && no_hk {{
            if !in_flight {{
                self.counters.idle_check_in_flight_false += 1;
            }}
            if let Some(delay) = deferred_housekeeping_delay(in_flight, false, now, self.last_hk) {{
                self.counters.deferred_timer_arms += 1;
                self.counters.last_deferred_delay_ns = Some(delay);
            }}
            self.counters.fast_done += 1;
            return;
        }}
        let hk = housekeeping_due(
            requested,
            recover,
            recovery_pending,
            teardown_pending,
            has_waiters,
            now,
            self.last_hk,
        );
        if hk {{
            self.last_hk = now;
            self.counters.housekeeping_runs += 1;
            self.counters.power_mmio_reads += 3;
        }}
        if let Some(delay) = deferred_housekeeping_delay(in_flight, hk, now, self.last_hk) {{
            self.counters.deferred_timer_arms += 1;
            self.counters.last_deferred_delay_ns = Some(delay);
        }}
    }}
}}

fn main() {{
    assert!({str(fast_idle_ok).lower()}, "fast_done must pass actual in_flight to idle_check()");
    assert!({str(fast_timer_ok).lower()}, "both fast and slow paths must arm queue_housekeeping when !in_flight defers");
    assert!({str(no_idle_force_ok).lower()}, "transient !in_flight must not force immediate housekeeping");
    assert!({str(lifecycle_ok).lower()}, "housekeeping_timer must be wired to sleep cancel and shutdown disable");

    // 1. Sparse !in_flight completions within < HOUSEKEEPING_NS hit fast_done with zero power MMIO
    //    and arm the deferred housekeeping timer within <= HOUSEKEEPING_NS.
    let mut lp = MockEventLoop::new(10_000_000);
    for step in 1..=10u64 {{
        lp.run_compute_completion(10_000_000 + step * 80_000, false, false, false, false, false, false);
    }}
    assert_eq!(lp.counters.fast_done, 10, "sparse !in_flight completions within 1 ms must hit fast_done");
    assert_eq!(lp.counters.housekeeping_runs, 0, "sparse !in_flight completions must not run housekeeping");
    assert_eq!(lp.counters.power_mmio_reads, 0, "sparse !in_flight completions must not read power MMIO");
    assert_eq!(lp.counters.idle_check_in_flight_false, 10);
    assert_eq!(lp.counters.deferred_timer_arms, 10, "deferred !in_flight must arm housekeeping timer");
    assert_eq!(lp.counters.last_deferred_delay_ns, Some(200_000));

    // 2. Once HOUSEKEEPING_NS elapses, housekeeping runs and clears the deferred timer requirement.
    lp.run_compute_completion(11_000_000, false, false, false, false, false, false);
    assert_eq!(lp.counters.housekeeping_runs, 1);
    assert_eq!(lp.counters.power_mmio_reads, 3);
    assert_eq!(lp.counters.deferred_timer_arms, 10, "running housekeeping must not re-arm deferred timer");

    // 3. Explicit triggers run housekeeping immediately even within < HOUSEKEEPING_NS.
    for (req, rec, rec_pend, tear, wait) in [
        (true, false, false, false, false),
        (false, true, false, false, false),
        (false, false, true, false, false),
        (false, false, false, true, false),
        (false, false, false, false, true),
    ] {{
        assert!(housekeeping_due(req, rec, rec_pend, tear, wait, 11_050_000, 11_000_000));
    }}

    // 4. Busy in_flight completions within < HOUSEKEEPING_NS hit fast_done without arming deferred timer.
    let mut busy = MockEventLoop::new(20_000_000);
    busy.run_compute_completion(20_100_000, true, false, false, false, false, false);
    assert_eq!(busy.counters.fast_done, 1);
    assert_eq!(busy.counters.housekeeping_runs, 0);
    assert_eq!(busy.counters.deferred_timer_arms, 0);

    // 5. Boundary: remaining delay is clamped to >= 1 ns even right at the threshold.
    assert_eq!(deferred_housekeeping_delay(false, false, 21_000_000, 20_000_000), Some(1));
    assert_eq!(deferred_housekeeping_delay(false, true, 20_100_000, 20_000_000), None);
    assert_eq!(deferred_housekeeping_delay(true, false, 20_100_000, 20_000_000), None);

    println!("PASS g17-m5-idle-housekeeping-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['idle-forces-housekeeping',
                                               'no-deferred-timer',
                                               'hardcode-in-flight-true',
                                               'skip-cadence-expiry'])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-hk-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
