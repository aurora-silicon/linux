#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production compute close/retirement/recycle methods with fake MMIO.

No firmware behavior is inferred from these controls. The actual cursor/stamp
predicate is compiled too; allocation and asynchronous notification are stubs.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def fixture():
    source = (ROOT / 'queue/compute.rs').read_text()
    retirement = (ROOT / 'queue/compute/retirement.rs').read_text()
    previous = item(source, 'struct Previous<') + '\n' + item(source, 'impl<B, C> Previous<B, C>')
    methods = '\n'.join(item(source, sig) for sig in [
        'fn witness_stamp(', 'pub(crate) fn release_owner(', 'pub(crate) fn reusable(',
        'pub(crate) fn recycle_quarantined_idle(', 'pub(crate) fn recycle_killed(',
        'fn recycle_settled(', 'fn release_witnessed_inner(', 'pub(crate) fn share_idle(', 'pub(crate) fn shareable(', 'pub(crate) fn can_rearm(', 'pub(crate) fn recycled_pending('])
    exit_source = (ROOT / 'queue/compute/exit.rs').read_text()
    methods += '\n' + item(exit_source, 'pub(crate) fn settle_exit(').replace('super::Previous', 'Previous').replace('crate::g17::qos::Owner', 'qos::Owner')
    drain = item((ROOT / 'mod.rs').read_text(), 'fn drain_reclaims(')
    proof = item(retirement, 'pub(crate) const fn stamp_newer(') + '\n' + item(retirement, 'pub(crate) const fn stamp_covers(') + '\n' + item(retirement, 'pub(crate) fn quarantined_retired(')
    return r'''
#![allow(dead_code)]
use std::sync::{Arc, atomic::{AtomicUsize, AtomicU32, Ordering}};
type Error = i32;
type Result<T = ()> = std::result::Result<T, Error>;
const EIO: Error = -5;
const EBUSY: Error = -16;
const EINVAL: Error = -22;
const ENODATA: Error = -61;
const ECANCELED: Error = -125;
fn now_ns() -> u64 { 123 }
struct Context { id: u64, live: Arc<AtomicUsize>, killed: AtomicU32 }
impl Context {
    fn note_compute_binding(&self, _: bool) {}
    fn new(id: u64, live: &Arc<AtomicUsize>) -> Arc<Self> {
        live.fetch_add(1, Ordering::SeqCst);
        Arc::new(Self { id, live: live.clone(), killed: AtomicU32::new(1) })
    }
}
impl Drop for Context { fn drop(&mut self) { self.live.fetch_sub(1, Ordering::SeqCst); } }
struct Binding { context: Arc<Context> }
''' + previous + '\n' + item((ROOT / 'sync_input.rs').read_text(), 'pub(super) struct ReaderHint').replace('pub(super) ', '') + '\nimpl ReaderHint {\n' + item((ROOT / 'sync_input.rs').read_text(), 'pub(super) fn has_readers(').replace('pub(super) ', '') + '\n}\n' + r'''
#[derive(Clone, Copy, PartialEq)] enum PoolPhase { Populated, ReleaseSent(u32), Vacant }
#[derive(Clone, Copy)] struct Ticket {
    owner: u64, item_count: u32, descriptor: u64, item_producer: u32, kick: u64,
}
#[derive(Clone, Copy)] struct Cursors { count: u32, producer: u32, consumer: u32 }
''' + proof + r'''
mod retirement { pub(crate) use super::{quarantined_retired, stamp_newer}; }
struct Graph { cursors: Cursors, stamp: u32, fail: bool, resets: usize, owner: u64 }
impl Graph {
    fn set_owner(&mut self, context: &Context) -> Result { if self.fail { Err(EIO) } else { self.owner = context.id; Ok(()) } }
    fn reset_items(&mut self) -> Result { if self.fail { Err(EIO) } else { self.resets += 1; Ok(()) } }
    fn cursors(&self) -> Result<Cursors> { Ok(self.cursors) }
    fn stamp(&self) -> Result<u32> { Ok(self.stamp) }
}
struct Kick;
impl Kick { fn clear_parent_after_recovery(&mut self) {} }
#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase { Published, Acknowledged, Unproven, Released }
struct Exit { phase: Phase }
impl Exit { fn released(&self) -> bool { self.phase == Phase::Released } }
struct State { killed: u32 }
impl State { fn read(context: &Context) -> Result<Self> { Ok(Self { killed: context.killed.load(Ordering::SeqCst) }) } }
struct Completion;
impl Completion {
    fn has_firmware_consumer(&self) -> bool { false }
    fn compute_words(&self) -> Result<[u32; 2]> { Ok([0, 0]) }
    fn replay_witness_words(&self) -> Option<[u32; 2]> { Some([0, 0]) }
}
struct Packet { completion: Arc<Completion> }
struct Active { packet: Arc<Packet>, ticket: Option<Ticket>, qos_pending: bool }
struct Fifo<T> { entries: Vec<T>, len: usize }
impl<T> Fifo<T> {
    fn new() -> Self { Self { entries: vec![], len: 0 } }
    fn iter(&self) -> impl DoubleEndedIterator<Item=&T> { self.entries.iter() }
    fn iter_mut(&mut self) -> impl Iterator<Item=&mut T> { self.entries.iter_mut() }
    fn front(&self) -> Option<&T> { self.entries.first() }
    fn back(&self) -> Option<&T> { self.entries.last() }
    fn push(&mut self, value:T) -> Result { self.entries.push(value); self.len += 1; Ok(()) }
    fn pop(&mut self) -> Option<T> { if self.len == 0 { None } else { self.len -= 1; Some(self.entries.remove(0)) } }
}
mod qos { pub type Owner = u64; }
enum Deferred { Retired(Arc<Completion>, Result<[u64; 4]>), Release(Arc<Completion>) }
struct Queue {
    owner: Option<u64>, installed: bool, active: Fifo<Active>, quarantined: bool,
    previous: Option<Previous>, unpublished_binding: bool, released: bool, graph: Graph, binding: Option<Binding>,
    retirement_ready: bool, released_at: u64, growing: bool, pool_phase: PoolPhase,
    needs_fresh_backing: bool, exit: Option<Exit>, replays: Fifo<Arc<Packet>>,
    awaiting_witness: bool, quarantine_error: Option<Error>, failure_status_pending: bool,
    retired_by_teardown: bool, spared_quarantine: bool, spared_deferred: bool,
    retire_pending: bool, retirement_proved: bool, replay_retirement: bool,
    replay_error: Option<Error>, replay_prepared: bool, submitted: u32, kick: Kick,
    config_cache: Option<()>, source_hint: ReaderHint, source_readers: AtomicU32, claims: u32, fault_stamp: Option<u32>,
}
impl Queue {
    fn valid_ticket(&self, _: &Active, _: &Ticket) -> Result { Ok(()) }
    fn qos_owner(&self) -> u64 { 1 }
    fn context(&self) -> Option<&Arc<Context>> { self.binding.as_ref().map(|binding| &binding.context) }
''' + methods + r'''
}
fn idle(live: &Arc<AtomicUsize>) -> Queue {
    Queue {
        owner: Some(2), installed: true, active: Fifo::new(), quarantined: false,
        previous: None, unpublished_binding: false, released: false,
        graph: Graph { cursors: Cursors { count: 1280, producer: 4, consumer: 4 }, stamp: 7 << 8, fail: false, resets: 0, owner: 2 },
        binding: Some(Binding { context: Context::new(2, live) }), retirement_ready: false,
        released_at: 0, growing: false, pool_phase: PoolPhase::Populated,
        needs_fresh_backing: false, exit: None, replays: Fifo::new(),
        awaiting_witness: false, quarantine_error: None, failure_status_pending: false,
        retired_by_teardown: false, spared_quarantine: false, spared_deferred: false,
        retire_pending: false, retirement_proved: false, replay_retirement: false,
        replay_error: None, replay_prepared: false, submitted: 0, kick: Kick,
        fault_stamp: None, config_cache: None, source_hint: ReaderHint { possible: false }, source_readers: AtomicU32::new(0), claims: 0,
    }
}
fn fault(live: &Arc<AtomicUsize>) -> Queue {
    let mut queue = idle(live);
    let old = Context::new(1, live);
    queue.previous = Some(Previous { binding: Some(Binding { context: old.clone() }), context: old, publication: Some((11, 3)) });
    queue.active.push(Active { packet: Arc::new(Packet { completion: Arc::new(Completion) }),
        ticket: Some(Ticket { owner: 2, item_count: 1280, descriptor: 0x1000, item_producer: 4, kick: 7 }), qos_pending: true }).unwrap();
    queue.quarantined = true;
    queue.awaiting_witness = true;
    queue.submitted = 1;
    queue.quarantine_error = Some(EIO);
    queue
}
struct Reclaim(Arc<AtomicUsize>);
impl Reclaim { fn finish(self) { self.0.fetch_add(1, Ordering::SeqCst); } }
struct Reclaims(Vec<Reclaim>);
impl Reclaims { fn take_one(&mut self) -> Option<Reclaim> { self.0.pop() } }
struct Mutex<T>(std::sync::Mutex<T>);
impl<T> Mutex<T> { fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() } }
struct Available(AtomicUsize); impl Available {fn notify(&self){self.0.fetch_add(1, Ordering::SeqCst);}}
struct Shared { reclaim: Mutex<Reclaims>, wakes: AtomicUsize, queues: Available }
impl Shared {
    fn queue_events(&self) { self.wakes.fetch_add(1, Ordering::SeqCst); }
''' + drain + r'''
}
fn main() {
    let finished = Arc::new(AtomicUsize::new(0));
    let shared = Arc::new(Shared { reclaim: Mutex(std::sync::Mutex::new(Reclaims(vec![Reclaim(finished.clone())]))), wakes: AtomicUsize::new(0), queues: Available(AtomicUsize::new(0)) });
    shared.drain_reclaims();
    assert_eq!(finished.load(Ordering::SeqCst), 1);
    assert_eq!(shared.wakes.load(Ordering::SeqCst), 1);
    shared.drain_reclaims();
    assert_eq!(shared.wakes.load(Ordering::SeqCst), 1);
    assert_eq!(shared.queues.0.load(Ordering::SeqCst), 1);
    let live = Arc::new(AtomicUsize::new(0));
    let mut borrowed = idle(&live);
    borrowed.retirement_ready = true;
    borrowed.source_hint.possible = true;
    borrowed.source_readers.store(1, Ordering::Release);
    assert!(!borrowed.shareable());
    borrowed.release_owner().unwrap();
    assert!(!borrowed.reusable());
    borrowed.pool_phase = PoolPhase::Vacant;
    assert!(!borrowed.can_rearm());
    borrowed.source_readers.store(0, Ordering::Release);
    assert!(borrowed.reusable() && borrowed.can_rearm());
    drop(borrowed);
    for _ in 0..512 {
        // Rebound detached queue; lost VM / killed publisher / allocation error
        // before the first new command. No future completion exists to free it.
        let mut queue = idle(&live);
        assert!(!queue.release_owner().unwrap());
        assert!(queue.owner.is_none());
        assert!(queue.reusable());
        drop(queue);
        assert_eq!(live.load(Ordering::SeqCst), 0);

        let mut queue = fault(&live);
        queue.release_owner().unwrap();
        assert!(!queue.recycle_quarantined_idle().unwrap());
        for control in 0..3 {
            queue.graph.stamp = if control == 0 { 6 << 8 } else { 7 << 8 };
            queue.graph.cursors.consumer = if control == 1 { 3 } else { 4 };
            queue.release_witnessed_inner(if control == 2 { 1 } else { 0 }, false, |_| Ok(()), &mut |_| Ok(())).unwrap();
            assert_eq!(queue.active.len, 1);
            assert!(queue.previous.is_some());
            assert!(!queue.recycle_quarantined_idle().unwrap());
        }
        queue.graph.stamp = 7 << 8;
        queue.graph.cursors.consumer = 4;
        // Collector pressure must retain exact ownership for the next pass.
        assert_eq!(queue.release_witnessed_inner(0, false, |_| Ok(()), &mut |_| Err(EBUSY)), Err(EBUSY));
        assert!(queue.previous.is_some());
        queue.release_witnessed_inner(0, false, |_| Ok(()), &mut |_| Ok(())).unwrap();
        assert_eq!(queue.active.len, 0);
        assert!(queue.previous.is_none());
        assert_eq!(live.load(Ordering::SeqCst), 1);
        assert!(queue.recycle_quarantined_idle().unwrap());
        assert_eq!(queue.graph.resets, 1);
        assert!(!queue.reusable()); // old killed/faulted backing must be released
        queue.pool_phase = PoolPhase::Vacant; // fake control-consumption boundary
        assert!(queue.reusable());
        assert!(queue.can_rearm());
        drop(queue);
        assert_eq!(live.load(Ordering::SeqCst), 0);
    }
    for _ in 0..512 {
        let mut queue = fault(&live);
        queue.release_owner().unwrap();
        queue.exit = Some(Exit { phase: Phase::Acknowledged });
        queue.context().unwrap().killed.store(0, Ordering::SeqCst);
        queue.settle_exit(true, &mut |_| Ok(()), &mut |_| Ok(())).unwrap();
        assert!(queue.previous.is_some());
        assert!(queue.exit.as_ref().unwrap().phase == Phase::Unproven);
        assert_eq!(queue.active.len, 1);
        queue.context().unwrap().killed.store(1, Ordering::SeqCst);
        queue.settle_exit(false, &mut |_| Ok(()), &mut |_| Ok(())).unwrap();
        assert_eq!(queue.active.len, 1);
        queue.settle_exit(true, &mut |_| Ok(()), &mut |_| Ok(())).unwrap();
        assert!(queue.previous.is_none());
        assert!(queue.recycle_killed().unwrap());
        assert_eq!(queue.graph.resets, 1);
        assert!(!queue.reusable());
        queue.pool_phase = PoolPhase::Vacant;
        assert!(queue.reusable());
        drop(queue);
        assert_eq!(live.load(Ordering::SeqCst), 0);
    }
    for _ in 0..512 {
        let mut queue = idle(&live);
        assert!(!queue.share_idle().unwrap(), "unretired binding cannot be shared");
        assert_eq!(queue.owner, Some(2));
        queue.retirement_ready = true;
        queue.exit = Some(Exit { phase: Phase::Acknowledged });
        assert!(!queue.share_idle().unwrap(), "issued kill cannot yield a channel");
        assert_eq!(queue.owner, Some(2));
        queue.exit = None;
        assert!(queue.share_idle().unwrap());
        assert!(queue.reusable());
        assert_eq!(queue.graph.resets, 0, "healthy sharing keeps the installed ring");
        assert!(!queue.needs_fresh_backing);
        assert_eq!(live.load(Ordering::SeqCst), 1, "predecessor remains retained");
        assert!(!queue.share_idle().unwrap());
        drop(queue);
        assert_eq!(live.load(Ordering::SeqCst), 0);
    }
    let mut queue = idle(&live);
    let old = Context::new(1, &live);
    queue.previous = Some(Previous { binding: Some(Binding { context: old.clone() }), context: old, publication: None });
    queue.release_owner().unwrap();
    assert_eq!(queue.graph.owner, 1);
    assert!(queue.previous.is_none());
    assert!(queue.reusable());
    drop(queue);
    assert_eq!(live.load(Ordering::SeqCst), 0);
    println!("PASS 512 fault/close and 512 acknowledged-kill cycles: baseline resources restored; stale stamp, cursor, recovery and collector controls preserve ownership; unpublished rollback preserved");
}
'''


def main():
    source = fixture()
    idle_fix = '''if self.previous.is_none() {
                self.retirement_ready = true;
            }'''
    fault_fix = 'Previous::retire_witnessed(&mut self.previous);'
    assert source.count(idle_fix) == 1
    assert source.count(fault_fix) == 2
    def mutate(pattern, replacement):
        changed, count = re.subn(pattern, replacement, source)
        assert count == 1, (pattern, count)
        return changed

    controls = [('fixed', source, True),
                ('missing-idle-close', source.replace(idle_fix, ''), False),
                ('missing-fault-release', source.replace(fault_fix, '', 1), False),
                ('missing-kill-release', source.replace('Previous::retire_witnessed(&mut self.previous);\n        self.exit', 'self.exit'), False),
                ('missing-reclaim-wake', source.replace('self.queue_events();', ''), False),
                ('share-before-retirement', mutate(r'(&&\s*self\.owner\.is_some\(\))\s*&&\s*self\.retirement_ready', r'\1'), False),
                ('share-during-kill', mutate(r'&&\s*self\.exit\.is_none\(\)\s*(?=&&\s*self\.active\.len)', ''), False)]
    with tempfile.TemporaryDirectory(prefix='g17-capacity-') as tmp:
        for name, rust, passes in controls:
            path = Path(tmp) / (name + '.rs')
            binary = Path(tmp) / name
            path.write_text(rust)
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            if passes:
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0, name
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
