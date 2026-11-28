#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production freelist Lease and rollback transitions under direct sibling loans.

Compiles the production Owner, LeasePhase, Lease, control_consumed,
Pools::rollback, and Firmware::rollback_render_pool extracted from
drivers/gpu/drm/asahi/g17/ against host stubs.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def check(cond, line, msg):
    if not cond:
        print(f'FAIL line {line}: {msg}', file=sys.stderr)
        sys.exit(1)


def extract_code(mutation):
    freelist = (ROOT / 'freelist.rs').read_text()
    pools = (ROOT / 'runtime/render/pools.rs').read_text()
    render = (ROOT / 'runtime/render.rs').read_text()
    direct = (ROOT / 'runtime/render/direct.rs').read_text()
    submit = (ROOT / 'runtime/render/submit.rs').read_text()

    check(direct.count('self.rollback_render_pool(preparation)') == 2, 43,
          'direct.rs must use rollback_render_pool at both rollback sites')
    check(submit.count('self.rollback_render_pool(preparation)') == 1, 45,
          'submit.rs must use rollback_render_pool')

    owner = item(freelist, 'pub(crate) struct Owner')
    phase = item(freelist, 'enum LeasePhase')
    lease = item(freelist, 'pub(crate) struct Lease')
    impl_lease = item(freelist, 'impl Lease {')
    consumed = item(freelist, 'pub(crate) fn control_consumed(')
    pools_rollback = item(pools, 'pub(super) fn rollback(').replace('pub(super) ', 'pub ')
    fw_rollback = item(render, 'fn rollback_render_pool(').replace('pools::Preparation', 'Preparation')

    lease_code = '\n\n'.join([
        '#[derive(Copy, Clone, Debug, PartialEq, Eq)]\n' + owner,
        '#[derive(Copy, Clone, Debug, PartialEq, Eq)]\n' + phase,
        '#[derive(Copy, Clone, Debug)]\n' + lease,
        impl_lease,
        consumed,
    ])
    lease_code = lease_code.replace('pub(crate) ', 'pub ')
    lease_code = lease_code.replace('super::fw::channels::FreeListCompletion', 'FreeListCompletion')
    lease_code = lease_code.replace('super::fw::channels::FreeListRelease', 'FreeListRelease')
    lease_code = lease_code.replace('super::fw::channels::DEVICE_CONTROL_SLOTS', 'DEVICE_CONTROL_SLOTS')
    lease_code = lease_code.replace('super::now_ns()', 'now_ns()')

    if mutation == 'reject-preparing-complete':
        target = 'if self.phase != LeasePhase::Ready\n            || !self.published'
        replacement = 'if self.phase != LeasePhase::Ready\n            || self.preparing != 0\n            || !self.published'
        check(target in lease_code, 71, 'vacuous mutation: reject-preparing-complete')
        lease_code = lease_code.replace(target, replacement, 1)
    elif mutation == 'snapshot-rollback':
        target = 'Ok(lease.cancel_prepare()?.is_some())'
        replacement = 'self.leases[preparation.slot] = preparation.snapshot; Ok(self.leases[preparation.slot].and_then(|l| l.pending_release()).is_some())'
        check(target in pools_rollback, 76, 'vacuous mutation: snapshot-rollback')
        pools_rollback = pools_rollback.replace(target, replacement, 1)
    elif mutation == 'drop-pending-flush':
        target = 'match self.release_render_lists()'
        replacement = 'match Ok(())'
        check(target in fw_rollback, 81, 'vacuous mutation: drop-pending-flush')
        fw_rollback = fw_rollback.replace(target, replacement, 1)
    elif mutation == 'drop-doorbell-loss':
        target = 'self.primary.state.shared.lose_device_quietly();\n                    return Err(error);'
        replacement = 'return Err(error);'
        check(target in fw_rollback, 86, 'vacuous mutation: drop-doorbell-loss')
        fw_rollback = fw_rollback.replace(target, replacement, 1)

    return lease_code, pools_rollback, fw_rollback


def build_harness(mutation):
    lease_code, pools_rollback, fw_rollback = extract_code(mutation)
    return f'''
#![allow(dead_code)]
type Error = i32;
type Result<T = ()> = std::result::Result<T, Error>;
const EINVAL: Error = -22;
const EIO: Error = -5;
const EAGAIN: Error = -11;
const EBUSY: Error = -16;
const ETIMEDOUT: Error = -110;
const EOVERFLOW: Error = -75;
const DEVICE_CONTROL_SLOTS: u32 = 256;

#[derive(Copy, Clone, Debug)]
pub struct FreeListCompletion;

// Match production FreeListRelease: intentionally does not derive PartialEq.
#[derive(Copy, Clone, Debug)]
pub struct FreeListRelease {{
    pub generation: u64,
    pub buffer_slot: u32,
}}

impl FreeListRelease {{
    pub fn new(generation: u64, buffer_slot: u32) -> Self {{
        Self {{ generation, buffer_slot }}
    }}
}}

fn now_ns() -> u64 {{ 1_000_000 }}

{lease_code}

struct Preparation {{
    slot: usize,
    generation: u64,
    snapshot: Option<Lease>,
}}

struct Pools {{
    leases: Vec<Option<Lease>>,
}}

impl Pools {{
    fn new() -> Self {{
        Self {{ leases: vec![None; 256] }}
    }}
    fn prepare(&mut self, slot: usize, owner: Owner) -> Result<Preparation> {{
        let snapshot = self.leases[slot];
        let next = Lease::acquire(snapshot.as_ref(), owner)?;
        let generation = next.generation();
        self.leases[slot] = Some(next);
        Ok(Preparation {{ slot, generation, snapshot }})
    }}
    {pools_rollback}
}}

struct Shared {{
    events_queued: usize,
    lost: bool,
}}
impl Shared {{
    fn queue_events(&mut self) {{ self.events_queued += 1; }}
    fn lose_device_quietly(&mut self) {{ self.lost = true; }}
}}
struct PrimaryState {{ shared: Shared }}
struct Primary {{ state: PrimaryState }}
struct Render {{ pools: Pools }}
struct Queues {{ render: Render }}
struct Firmware {{
    queues: Queues,
    primary: Primary,
    release_calls: usize,
    release_result: Result,
}}
impl Firmware {{
    fn new() -> Self {{
        Self {{
            queues: Queues {{ render: Render {{ pools: Pools::new() }} }},
            primary: Primary {{ state: PrimaryState {{ shared: Shared {{ events_queued: 0, lost: false }} }} }},
            release_calls: 0,
            release_result: Ok(()),
        }}
    }}
    fn release_render_lists(&mut self) -> Result {{
        self.release_calls += 1;
        self.release_result
    }}
    {fw_rollback}
}}

fn owner(slot: u32, serial: u64) -> Owner {{
    Owner {{
        serial,
        epoch: 1,
        buffer_slot: slot,
        control_va: 0x1000,
        ring_va: 0x2000,
        cookie: 0x3000,
    }}
}}

fn main() {{
    let o = owner(129, 1);

    // 1. Sibling completion while direct preparation is lent off-lock.
    let mut lease = Lease::acquire(None, o).unwrap();
    assert!(matches!(lease.complete_render(false), Err(EINVAL)), "unactivated prepare cannot complete");
    lease.retire_prepare().unwrap();

    lease = Lease::acquire(Some(&lease), o).unwrap();
    assert_eq!(lease.generation(), 0);
    assert!(lease.complete_render(false).expect("sibling must complete while direct prepare is lent").is_none());
    assert!(matches!(lease.complete_render(false), Err(EINVAL)), "no second published render yet");
    lease.retire_prepare().unwrap();
    let rel = lease.complete_render(false).unwrap().expect("final release");
    assert_eq!((rel.generation, rel.buffer_slot), (0, 129));
    lease.mark_release_sent(3).unwrap();
    lease.observe_releases(3, 3).unwrap();
    assert!(lease.released());

    // 2. Production Pools::rollback + Firmware::rollback_render_pool after sibling completion.
    let mut fw = Firmware::new();
    let _p0 = fw.queues.render.pools.prepare(129, o).unwrap();
    fw.queues.render.pools.leases[129].as_mut().unwrap().retire_prepare().unwrap();
    let p1 = fw.queues.render.pools.prepare(129, o).unwrap();
    assert!(fw.queues.render.pools.leases[129].as_mut().unwrap().complete_render(false).unwrap().is_none());
    fw.rollback_render_pool(p1).unwrap();
    let rel = fw.queues.render.pools.leases[129].unwrap().pending_release().expect("rollback after sibling completion must expose final release");
    assert_eq!((rel.generation, rel.buffer_slot), (0, 129));
    assert_eq!(fw.release_calls, 1, "rollback exposing final release must flush release_render_lists");
    assert!(!fw.primary.state.shared.lost);

    // 3. EAGAIN on release_render_lists queues events without losing device.
    let mut fw = Firmware::new();
    fw.release_result = Err(EAGAIN);
    let _p0 = fw.queues.render.pools.prepare(129, o).unwrap();
    fw.queues.render.pools.leases[129].as_mut().unwrap().retire_prepare().unwrap();
    let p1 = fw.queues.render.pools.prepare(129, o).unwrap();
    assert!(fw.queues.render.pools.leases[129].as_mut().unwrap().complete_render(false).unwrap().is_none());
    fw.rollback_render_pool(p1).unwrap();
    assert_eq!(fw.release_calls, 1);
    assert_eq!(fw.primary.state.shared.events_queued, 1);
    assert!(!fw.primary.state.shared.lost);

    // 4. Mailbox/transport error on release_render_lists loses the device so committed releases cannot strand.
    let mut fw = Firmware::new();
    fw.release_result = Err(ETIMEDOUT);
    let _p0 = fw.queues.render.pools.prepare(129, o).unwrap();
    fw.queues.render.pools.leases[129].as_mut().unwrap().retire_prepare().unwrap();
    let p1 = fw.queues.render.pools.prepare(129, o).unwrap();
    assert!(fw.queues.render.pools.leases[129].as_mut().unwrap().complete_render(false).unwrap().is_none());
    assert_eq!(fw.rollback_render_pool(p1), Err(ETIMEDOUT));
    assert!(fw.primary.state.shared.lost, "transport failure during rollback release must lose device");

    // 5. Concurrent dual preparations with mixed activate/rollback and completions.
    let mut lease = Lease::acquire(None, o).unwrap();
    lease.retire_prepare().unwrap();
    lease = Lease::acquire(Some(&lease), o).unwrap();
    lease = Lease::acquire(Some(&lease), o).unwrap();
    assert!(lease.complete_render(false).unwrap().is_none());
    assert!(matches!(lease.complete_render(false), Err(EINVAL)));
    lease.retire_prepare().unwrap();
    assert!(lease.cancel_prepare().unwrap().is_none());
    let rel = lease.complete_render(false).unwrap().expect("final release");
    assert_eq!((rel.generation, rel.buffer_slot), (0, 129));

    // 6. Rollback of unpublished fresh generation and reacquire over ReleaseSent.
    let mut fresh = Lease::acquire(None, o).unwrap();
    assert!(fresh.cancel_prepare().unwrap().is_none());
    assert!(fresh.released());
    assert!(fresh.pending_release().is_none());

    let mut sent = Lease::acquire(Some(&fresh), o).unwrap();
    sent.retire_prepare().unwrap();
    assert!(sent.complete_render(false).unwrap().is_some());
    sent.mark_release_sent(7).unwrap();

    let mut unconsumed = Lease::acquire(Some(&sent), o).unwrap();
    assert!(unconsumed.cancel_prepare().unwrap().is_none());
    assert!(unconsumed.reacquirable());
    assert!(!unconsumed.released());
    assert_eq!(unconsumed.release_witness(), Some(7));

    let mut consumed = Lease::acquire(Some(&sent), o).unwrap();
    consumed.observe_releases(8, 8).unwrap();
    assert!(consumed.cancel_prepare().unwrap().is_none());
    assert!(consumed.released());
    assert_eq!(consumed.release_witness(), None);

    // 7. Rollback after acquiring a lingering registration exposes its pending release.
    let mut lingering = Lease::acquire(None, o).unwrap();
    lingering.retire_prepare().unwrap();
    assert!(lingering.complete_render(true).unwrap().is_none());
    assert!(lingering.lingering().is_some());
    let mut from_linger = Lease::acquire(Some(&lingering), o).unwrap();
    assert_eq!(from_linger.generation(), 0);
    let rel = from_linger.cancel_prepare().unwrap().expect("linger rollback must release");
    assert_eq!((rel.generation, rel.buffer_slot), (0, 129));

    println!("PASS freelist lease direct sibling completion, rollback release, doorbell loss, and witness preservation");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['reject-preparing-complete', 'snapshot-rollback',
                                               'drop-pending-flush', 'drop-doorbell-loss'])
    args = parser.parse_args()

    code = build_harness(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-freelist-lease-') as tmp:
        src = Path(tmp) / 'test.rs'
        binary = Path(tmp) / 'test'
        src.write_text(code)
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(src), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if result.stdout:
            print(result.stdout.strip())
        if result.stderr:
            print(result.stderr.strip(), file=sys.stderr)
        sys.exit(result.returncode)


if __name__ == '__main__':
    main()
