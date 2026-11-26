#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for M5 direct compute outer-ring credits and post-signal report ordering.

Compiles the real `Credits`, `OuterClaim`, `OuterScope`, and `free` implementation
from `drivers/gpu/drm/asahi/g17/initdata/outer_credit.rs` on the host and tests:
1. Mixed render (2-slot) and compute (1-slot) reservations/consumptions across
   `WORK_RING_SLOTS` ring wrap and exact capacity boundaries.
2. Queued claims held across a generation change and `OuterScope` (locked render /
   recovery), verifying waiters wake on drop and direct consumption never debits
   `OuterScope`'s `published[]` counter.
3. Multi-threaded contention between concurrent direct compute reservations/consumptions
   and periodic locked `OuterScope` transitions.
4. Structural verification of `runtime_event_turn()` in `runtime/events.rs`:
   completions signal (`batch.finish()`) before report drains, recovery drains
   reports before `service_runtime_recovery()` regardless of `HOUSEKEEPING_NS`,
   and steady-state passes throttle report drains without double-draining on recovery.
"""
import argparse
import os
import re
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


def ordered_ok(source, *parts):
    at = -1
    for part in parts:
        if source.count(part) != 1:
            return False
        idx = source.index(part)
        if idx <= at:
            return False
        at = idx
    return True


def build_harness(mutation=None):
    outer_src = (G17 / 'initdata/outer_credit.rs').read_text()
    events_src = (G17 / 'runtime/events.rs').read_text()

    turn = item(events_src, 'fn runtime_event_turn(')
    if mutation == 'no-pre-recovery-drain':
        turn = turn.replace(
            'firmware.drain_reports();\n            reports_drained = true;\n'
            '            firmware.service_runtime_recovery(deferred)?;',
            'firmware.service_runtime_recovery(deferred)?;',
            1,
        )

    # Verify report-drain ordering contract in production runtime_event_turn().
    ordering_ok = ordered_ok(
        turn,
        'deferred.finish();',
        'let mut reports_drained = false;',
        'firmware.drain_reports();\n            reports_drained = true;',
        'firmware.service_runtime_recovery(deferred)?;',
        'if housekeeping {',
        'if !reports_drained {',
    )

    # Strip kernel imports, inner doc comments, and kunit module from outer_credit.rs for host compilation.
    kunit_idx = outer_src.index('#[kernel::macros::kunit_tests(')
    prod = outer_src[:kunit_idx]
    prod = re.sub(r'^//!.*$', '', prod, flags=re.M)
    prod = re.sub(r'^use crate::g17::.*$', '', prod, flags=re.M)
    prod = re.sub(r'^use core::sync::atomic::.*$', '', prod, flags=re.M)
    prod = re.sub(r'^use kernel::.*$', '', prod, flags=re.M)
    prod = prod.replace('Arc::new(', 'HostArc::new(')
    prod = prod.replace(', GFP_KERNEL)?', ')?')
    prod = prod.replace('pub(crate)', 'pub')
    prod = prod.replace('pub(super)', 'pub')

    if mutation == 'direct-debits-published':
        prod = prod.replace(
            'self.reserved[claim.priority].fetch_sub(count, Ordering::AcqRel);',
            'self.reserved[claim.priority].fetch_sub(count, Ordering::AcqRel);\n'
            '        self.published[claim.priority].fetch_add(u64::from(count), Ordering::AcqRel);',
            1,
        )
    elif mutation == 'scope-allows-reserve':
        prod = prod.replace(
            'if self.scoped.load(Ordering::Acquire) { return Err(EBUSY); }',
            '',
            1,
        )

    return f'''
use std::sync::{{Arc, atomic::{{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering}}}};

const EINVAL: i32 = -22;
const EBUSY: i32 = -16;
const EIO: i32 = -5;
const WORK_RING_SLOTS: u32 = 128;
type Result<T = ()> = std::result::Result<T, i32>;

struct HostArc;
impl HostArc {{
    fn new<T>(v: T) -> Result<Arc<T>> {{ Ok(Arc::new(v)) }}
}}

pub struct Availability {{
    waiters: AtomicUsize,
    notifications: AtomicUsize,
}}
impl Availability {{
    pub fn new() -> Result<Arc<Self>> {{
        Ok(Arc::new(Self {{
            waiters: AtomicUsize::new(0),
            notifications: AtomicUsize::new(0),
        }}))
    }}
    pub fn has_waiters(&self) -> bool {{ self.waiters.load(Ordering::Acquire) != 0 }}
    pub fn notify(&self) {{ self.notifications.fetch_add(1, Ordering::AcqRel); }}
    pub fn notifications(&self) -> usize {{ self.notifications.load(Ordering::Acquire) }}
}}

{prod}

fn test_mixed_wrap_and_generation_change() {{
    let credits = Credits::new().unwrap();
    let n = WORK_RING_SLOTS;
    let mut ca = n - 6;
    let mut cb = n - 6;
    let mut prod_idx = n - 2;

    let mut render_claim = credits.reserve(2, 2, [ca, cb, prod_idx]).unwrap();
    let mut comp_a = credits.reserve(2, 1, [ca, cb, prod_idx]).unwrap();
    credits.consume_direct(&mut comp_a).unwrap();
    prod_idx = (prod_idx + 1) % n;

    let mut comp_b = credits.reserve(2, 1, [ca, cb, prod_idx]).unwrap();
    credits.consume_direct(&mut comp_b).unwrap();
    prod_idx = (prod_idx + 1) % n;
    assert_eq!(prod_idx, 0);

    let rem = credits.capacity(2, [ca, cb, prod_idx]).unwrap();
    let filler = credits.reserve(2, rem - 1, [ca, cb, prod_idx]).unwrap();
    assert_eq!(credits.capacity(2, [ca, cb, prod_idx]).unwrap(), 1);
    assert_eq!(credits.reserve(2, 2, [ca, cb, prod_idx]).err(), Some(EBUSY));
    let mut edge = credits.reserve(2, 1, [ca, cb, prod_idx]).unwrap();
    credits.consume_direct(&mut edge).unwrap();
    prod_idx = (prod_idx + 1) % n;
    assert_eq!(credits.capacity(2, [ca, cb, prod_idx]).unwrap(), 0);
    drop(filler);

    ca = 1;
    cb = 1;
    let wrapped = [ca, cb, prod_idx];
    assert!(credits.progressed(2, wrapped).unwrap());

    // Hold a queued compute claim across OuterScope + generation change.
    let queued_comp = credits.reserve(2, 1, wrapped).unwrap();
    let mut pre_direct = credits.reserve(2, 1, wrapped).unwrap();
    let notes_before = credits.available().notifications();
    {{
        let _scope = credits.scope(&mut render_claim).unwrap();
        assert!(credits.is_scoped());
        assert_eq!(credits.reserve(2, 1, wrapped).err(), Some(EBUSY));
        // A direct compute claim already reserved before OuterScope must not debit OuterScope.
        credits.consume_direct(&mut pre_direct).unwrap();
    }}
    assert!(!credits.is_scoped());
    assert_eq!(render_claim.remaining(), 2, "direct compute must not debit OuterScope");
    assert_eq!(credits.counter(2).unwrap().load(Ordering::Acquire), 0);

    // Generation changed: dropping the queued compute claim restores its credit and wakes waiters.
    drop(queued_comp);
    assert!(credits.available().notifications() > notes_before);
    drop(render_claim);
    assert_eq!(credits.capacity(2, wrapped).unwrap(), n - 1);
}}

fn test_concurrent_direct_and_scoped() {{
    let credits = Credits::new().unwrap();
    let stop = Arc::new(AtomicBool::new(false));
    let mut threads = Vec::new();
    for _ in 0..4 {{
        let c = credits.clone();
        let s = stop.clone();
        threads.push(std::thread::spawn(move || {{
            let mut iters = 0u32;
            while !s.load(Ordering::Acquire) && iters < 2000 {{
                if let Ok(mut claim) = c.reserve(1, 1, [0, 0, 0]) {{
                    if iters % 5 == 0 {{
                        drop(claim);
                    }} else {{
                        c.consume_direct(&mut claim).unwrap();
                    }}
                }}
                iters += 1;
            }}
        }}));
    }}
    for _ in 0..200 {{
        if let Ok(mut render_claim) = credits.reserve(1, 2, [0, 0, 0]) {{
            let mut scoped = false;
            if let Ok(scope) = credits.scope(&mut render_claim) {{
                assert_eq!(credits.reserve(1, 1, [0, 0, 0]).err(), Some(EBUSY));
                credits.counter(1).unwrap().fetch_add(2, Ordering::AcqRel);
                drop(scope);
                scoped = true;
            }}
            if scoped {{
                assert_eq!(render_claim.remaining(), 0);
            }}
        }}
    }}
    stop.store(true, Ordering::Release);
    for t in threads {{
        t.join().unwrap();
    }}
    assert_eq!(credits.capacity(1, [0, 0, 0]).unwrap(), WORK_RING_SLOTS - 1);
}}

fn main() {{
    assert!({str(ordering_ok).lower()}, "runtime_event_turn must drain reports post-signal and pre-recovery");
    test_mixed_wrap_and_generation_change();
    test_concurrent_direct_and_scoped();
    println!("PASS g17-m5-direct-compute-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['no-pre-recovery-drain',
                                               'direct-debits-published',
                                               'scope-allows-reserve'])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-m5-dc-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
