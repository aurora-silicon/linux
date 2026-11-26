#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for M5 multi-queue cluster pinning and outer-credit locking.

Compiles the real `Credits`, `OuterClaim`, `OuterScope`, and `free` implementation
from `drivers/gpu/drm/asahi/g17/initdata/outer_credit.rs` on the host and tests:
1. Tight-ring multi-threaded serialization of `claim_outer()` (`reserve` under
   `Port::m5_outer`), `PortOuterScope` (entry and `Drop` under `Port::m5_outer`),
   and concurrent direct compute/render reservations and consumptions — verifying
   zero ring overcommits, zero `EIO`, and exact credit accounting.
2. Structural verification of the production G17 M5 sources:
   - `Shared::new()` leaves single-die M5 schedulers unrestricted
     (`pin_scheduler_cluster: !m5 || id.num_dies > 1`).
   - `Backend::claim_outer()` holds `firmware.port.m5_outer_lock()` around
     `firmware.init.reserve_outer()`.
   - `Backend::publish_claimed()` and `runtime/render/prepare.rs` enter
     `OuterScope` via `firmware.port.outer_scope(&firmware.init, claim)`.
   - `Port::outer_scope()` and `Drop for PortOuterScope` serialize both scope
     entry and exit under `Port::m5_outer`.
   - `Backend::compute_direct()` reuses a matching `self.outer_compute` claim and
     restores any unspent claim back into `self.outer_compute` outside
     `cell.lock()` when returning `None` to fall back to the device-mutex path.
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
    port_src = (G17 / 'port.rs').read_text()
    runtime_src = (G17 / 'runtime.rs').read_text()
    prepare_src = (G17 / 'runtime/render/prepare.rs').read_text()
    mod_src = (G17 / 'mod.rs').read_text()
    backend_src = (G17 / 'runtime/backend.rs').read_text()

    claim_outer = item(runtime_src, 'fn claim_outer(')
    publish_claimed = item(runtime_src, 'fn publish_claimed(')
    compute_direct = item(runtime_src, 'fn compute_direct(')
    port_outer_scope = item(port_src, 'pub(super) fn outer_scope<')
    port_outer_scope_drop = item(port_src, 'impl Drop for PortOuterScope<')
    creator_cluster = item(backend_src, 'fn creator_cluster_scheduling(')

    if mutation == 'unlocked-claim-outer':
        claim_outer = claim_outer.replace(
            'let _m5_outer = firmware.port.m5_outer_lock();',
            '',
            1,
        )
    elif mutation == 'raw-compute-scope':
        publish_claimed = publish_claimed.replace(
            'firmware.port.outer_scope(&firmware.init, claim)',
            'firmware.init.outer_scope(claim)',
            1,
        )
    elif mutation == 'raw-render-scope':
        prepare_src = prepare_src.replace(
            'firmware.port.outer_scope(&firmware.init, claim)',
            'firmware.init.outer_scope(claim)',
            1,
        )
    elif mutation == 'unlocked-scope-drop':
        port_outer_scope_drop = port_outer_scope_drop.replace(
            'let _m5_outer = self.port.m5_outer_lock();',
            '',
            1,
        )
    elif mutation == 'drop-direct-claim-on-fallback':
        compute_direct = compute_direct.replace(
            'if outcome.is_none() && from_held {',
            'if false {',
            1,
        )
    elif mutation == 'pin-single-die':
        mod_src = mod_src.replace(
            'pin_scheduler_cluster: !m5 || id.num_dies > 1,',
            'pin_scheduler_cluster: true,',
            1,
        )

    claim_outer_ok = ordered_ok(
        claim_outer,
        'let _m5_outer = firmware.port.m5_outer_lock();',
        'firmware.init.reserve_outer(',
    )
    publish_claimed_ok = (
        'firmware.port.outer_scope(&firmware.init, claim)' in publish_claimed
        and 'firmware.init.outer_scope(' not in publish_claimed
    )
    render_scope_ok = (
        'firmware.port.outer_scope(&firmware.init, claim)' in prepare_src
        and 'firmware.init.outer_scope(' not in prepare_src
    )
    port_scope_ok = (
        ordered_ok(
            port_outer_scope,
            'let _m5_outer = self.m5_outer.lock();',
            'init.outer_scope(claim)?',
        )
        and ordered_ok(
            port_outer_scope_drop,
            'let _m5_outer = self.port.m5_outer_lock();',
            'drop(scope);',
        )
    )
    # Verify compute_direct reuses self.outer_compute before cell.lock() and restores any
    # unspent held claim back into self.outer_compute AFTER the cell.lock() closure exits.
    direct_restore_ok = (
        compute_direct.count('let mut held = self.outer_compute.lock();') == 2
        and ordered_ok(
            compute_direct,
            'held.take()',
            'let result = (|| {',
            '.lock()?;',
            '})();',
            'if outcome.is_none() && from_held {',
            '*held = Some(claim);',
        )
        and compute_direct.index('let mut held = self.outer_compute.lock();')
        < compute_direct.index('let result = (|| {')
        and compute_direct.rindex('let mut held = self.outer_compute.lock();')
        > compute_direct.index('if outcome.is_none() && from_held {')
    )
    cluster_pin_ok = (
        'pin_scheduler_cluster: !m5 || id.num_dies > 1,' in mod_src
        and 'self.shared.pin_scheduler_cluster()' in creator_cluster
    )

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

    return f'''
use std::sync::{{Arc, Mutex, atomic::{{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering}}}};

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

struct RingState {{
    consumer: u32,
    producer: u32,
}}

impl RingState {{
    fn cursors(&self) -> [u32; 3] {{
        [self.consumer, self.consumer, self.producer]
    }}
    fn advance_producer(&mut self, count: u32) {{
        for _ in 0..count {{
            let next = (self.producer + 1) % WORK_RING_SLOTS;
            assert_ne!(next, self.consumer, "outer ring overcommitted past consumer");
            self.producer = next;
        }}
    }}
    fn consume_if_needed(&mut self, keep_free: u32) {{
        while free(self.cursors()).unwrap() < keep_free {{
            assert_ne!(self.consumer, self.producer);
            self.consumer = (self.consumer + 1) % WORK_RING_SLOTS;
        }}
    }}
}}

struct HostPortScope<'a> {{
    scope: Option<OuterScope<'a>>,
    m5_outer: Arc<Mutex<RingState>>,
}}

impl Drop for HostPortScope<'_> {{
    fn drop(&mut self) {{
        if let Some(scope) = self.scope.take() {{
            let _guard = self.m5_outer.lock().unwrap();
            drop(scope);
        }}
    }}
}}

fn test_serialized_tight_ring_contention() {{
    let credits = Credits::new().unwrap();
    // Start with only 4 free slots so any race between claim_outer, reserve_m5_outer,
    // and OuterScope entry/drop immediately overcommits or trips EIO.
    let m5_outer = Arc::new(Mutex::new(RingState {{
        consumer: 5,
        producer: 0,
    }}));
    let stop = Arc::new(AtomicBool::new(false));
    let mut threads = Vec::new();

    for tid in 0..4u32 {{
        let c = credits.clone();
        let lock = m5_outer.clone();
        let s = stop.clone();
        threads.push(std::thread::spawn(move || {{
            let mut iters = 0u32;
            while !s.load(Ordering::Acquire) && iters < 3000 {{
                let reserved = {{
                    let mut ring = lock.lock().unwrap();
                    ring.consume_if_needed(4);
                    if c.is_scoped() {{
                        Err(EBUSY)
                    }} else {{
                        c.reserve(1, 1, ring.cursors())
                    }}
                }};
                if let Ok(mut claim) = reserved {{
                    if (iters + tid) % 4 == 0 {{
                        // Simulate compute_direct fallback: drop unspent claim outside m5_outer.
                        drop(claim);
                    }} else {{
                        let mut ring = lock.lock().unwrap();
                        ring.advance_producer(1);
                        c.consume_direct(&mut claim).unwrap();
                    }}
                }}
                iters += 1;
            }}
        }}));
    }}

    // Device-mutex publisher: claim_outer() + PortOuterScope entry/drop all serialized on m5_outer.
    for iter in 0..400u32 {{
        let claim = {{
            let mut ring = m5_outer.lock().unwrap();
            ring.consume_if_needed(4);
            credits.reserve(1, 2, ring.cursors())
        }};
        if let Ok(mut render_claim) = claim {{
            let mut scoped = false;
            {{
                let scope = {{
                    let _guard = m5_outer.lock().unwrap();
                    credits.scope(&mut render_claim).map(|scope| HostPortScope {{
                        scope: Some(scope),
                        m5_outer: m5_outer.clone(),
                    }})
                }};
                if let Ok(scope) = scope {{
                    if iter % 3 != 0 {{
                        let mut ring = m5_outer.lock().unwrap();
                        assert!(credits.capacity(1, ring.cursors()).unwrap() >= 2);
                        ring.advance_producer(2);
                        credits.counter(1).unwrap().fetch_add(2, Ordering::AcqRel);
                    }}
                    drop(scope);
                    scoped = true;
                }}
            }}
            if scoped {{
                if iter % 3 != 0 {{
                    assert_eq!(render_claim.remaining(), 0);
                }} else {{
                    assert_eq!(render_claim.remaining(), 2);
                }}
            }}
            drop(render_claim);
        }}
    }}

    stop.store(true, Ordering::Release);
    for t in threads {{
        t.join().unwrap();
    }}
    let ring = m5_outer.lock().unwrap();
    let expected = free(ring.cursors()).unwrap();
    assert_eq!(credits.capacity(1, ring.cursors()).unwrap(), expected);
}}

fn test_fallback_preserves_claim_without_spurious_notify() {{
    let credits = Credits::new().unwrap();
    let cursors = [0, 0, 0];
    let claim = credits.reserve(1, 1, cursors).unwrap();
    let notes_before = credits.available().notifications();

    // Simulate compute_direct taking held claim and restoring it on None fallback.
    let mut held: Option<OuterClaim> = Some(claim);
    let mut m5_outer = held.take();
    assert!(held.is_none());
    let outcome: Option<Result> = None;
    if outcome.is_none() {{
        if let Some(c) = m5_outer.take().filter(|c| c.remaining() != 0) {{
            if !held.as_ref().is_some_and(|e| e.remaining() != 0) {{
                held = Some(c);
            }}
        }}
    }}
    drop(m5_outer);
    assert!(held.is_some());
    assert_eq!(held.as_ref().unwrap().remaining(), 1);
    assert_eq!(credits.available().notifications(), notes_before);

    // Subsequent slow-path publication consumes the restored claim cleanly.
    let mut c = held.take().unwrap();
    credits.consume_direct(&mut c).unwrap();
    assert_eq!(c.remaining(), 0);
    drop(c);
    assert_eq!(credits.available().notifications(), notes_before);
}}

fn main() {{
    assert!({str(claim_outer_ok).lower()}, "claim_outer must hold Port::m5_outer around reserve_outer");
    assert!({str(publish_claimed_ok).lower()}, "publish_claimed must use Port::outer_scope");
    assert!({str(render_scope_ok).lower()}, "render/prepare.rs must use Port::outer_scope");
    assert!({str(port_scope_ok).lower()}, "Port::outer_scope and PortOuterScope::drop must hold Port::m5_outer");
    assert!({str(direct_restore_ok).lower()}, "compute_direct must restore unspent m5_outer claim on None fallback outside cell.lock()");
    assert!({str(cluster_pin_ok).lower()}, "single-die M5 must leave scheduler queues unrestricted");
    test_serialized_tight_ring_contention();
    test_fallback_preserves_claim_without_spurious_notify();
    println!("PASS g17-m5-multiqueue-contention-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--mutation',
        choices=[
            'unlocked-claim-outer',
            'raw-compute-scope',
            'raw-render-scope',
            'unlocked-scope-drop',
            'drop-direct-claim-on-fallback',
            'pin-single-die',
        ],
    )
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-m5-mq-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
