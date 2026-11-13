#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the production G17 application-context choice and the queue creator's wait.

A returned context is handed to a new owner only once no queue record names it and the
firmware passed its fence; until then the creator waits, bounded, and is then refused.
The mutations restore the reuse of a waiting context and remove the bound.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/gpu/drm/asahi'
MUTATIONS = ('take-waiting', 'ignore-holds', 'wait-past-deadline')


def item(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def constant(source, name):
    return re.search(r'^(?:pub\(crate\) )?const ' + name + r': [^;]+;', source, re.M).group(0)


def swap(source, old, new):
    assert source.count(old) == 1, old
    return source.replace(old, new)


TESTS = r'''
use mmu::{ContextRoom, UatShared, EXECUTION_CONTEXT_MASK, UAT_NUM_CTX};

/// The allocator's bookkeeping around the production choice: allocation, the root's
/// release, and a hold's drop (`ExecutionRoot::release`, `ContextHold::drop`).
struct Contexts {
    shared: UatShared,
    /// Device-control records published and consumed (`channel::control_fence`).
    published: u64,
    consumed: u64,
}

impl Contexts {
    fn new() -> Self {
        Self {
            shared: UatShared {
                execution_contexts: !EXECUTION_CONTEXT_MASK,
                context_returns: [0; UAT_NUM_CTX],
                context_fences: [0; UAT_NUM_CTX],
                context_holds: [0; UAT_NUM_CTX],
                context_clock: 0,
            },
            published: 0,
            consumed: 0,
        }
    }
    fn room(&self) -> ContextRoom {
        self.shared.context_room(self.consumed)
    }
    /// A new queue: its context, holding its own identity.
    fn create(&mut self) -> Option<usize> {
        let ContextRoom::Ready(id) = self.room() else { return None };
        assert_eq!(self.shared.execution_contexts & (1u64 << id), 0, "context {id} has an owner");
        assert_eq!(self.shared.context_holds[id], 0, "context {id} is still named by a record");
        assert!(self.shared.context_fences[id] <= self.consumed, "context {id} is fenced");
        self.shared.execution_contexts |= 1u64 << id;
        self.shared.context_holds[id] += 1;
        Some(id)
    }
    /// One more record names the context (an installed compute queue's).
    fn hold(&mut self, id: usize) {
        self.shared.context_holds[id] += 1;
    }
    /// The scheduler release was consumed: the root goes back.
    fn release(&mut self, id: usize) {
        self.shared.context_clock += 1;
        self.shared.context_returns[id] = self.shared.context_clock;
        self.shared.execution_contexts &= !(1u64 << id);
    }
    /// A record that named the context is gone; the firmware had seen the identity.
    fn drop_hold(&mut self, id: usize) {
        let fence = self.published + 1;
        let inner = &mut self.shared;
        inner.context_holds[id] = inner.context_holds[id].saturating_sub(1);
        inner.context_fences[id] = inner.context_fences[id].max(fence);
        if inner.context_holds[id] == 0 && inner.execution_contexts & (1u64 << id) == 0 {
            inner.context_clock += 1;
            inner.context_returns[id] = inner.context_clock;
        }
    }
    /// The driver publishes a control record and the firmware consumes it.
    fn control_record(&mut self) {
        self.published += 1;
        self.consumed = self.published;
    }
    fn fill(&mut self) -> Vec<usize> {
        let mut ids = Vec::new();
        while let Some(id) = self.create() {
            ids.push(id);
        }
        ids
    }
}

#[test]
fn every_context_is_handed_out_once_then_none() {
    let mut contexts = Contexts::new();
    assert_eq!(contexts.room(), ContextRoom::Ready(5));
    let ids = contexts.fill();
    assert_eq!(ids.len(), EXECUTION_CONTEXT_MASK.count_ones() as usize);
    assert_eq!(ids, (5..63).collect::<Vec<_>>());
    assert_eq!(contexts.room(), ContextRoom::Full);
}

/// The failure this guards against: with every other context in use, a queue that owned a
/// compute channel exits. Its context returns, but the channel's installed record names it
/// until the channel gets its next owner. A new queue must not get that context.
#[test]
fn a_context_named_by_an_installed_record_is_not_handed_out() {
    let mut contexts = Contexts::new();
    let ids = contexts.fill();
    let exited = ids[40];
    contexts.hold(exited);
    // Scheduler release published and consumed, then the context object goes.
    contexts.control_record();
    contexts.release(exited);
    contexts.drop_hold(exited);
    // Other clients keep the firmware consuming control records.
    for _ in 0..8 {
        contexts.control_record();
    }
    assert_eq!(contexts.room(), ContextRoom::Waiting);
    assert_eq!(contexts.create(), None);
    // The channel's next owner replaces the record; the firmware has not shown progress.
    contexts.drop_hold(exited);
    assert_eq!(contexts.room(), ContextRoom::Waiting);
    assert_eq!(contexts.create(), None);
    contexts.control_record();
    assert_eq!(contexts.room(), ContextRoom::Ready(exited));
    assert_eq!(contexts.create(), Some(exited));
    assert_eq!(contexts.room(), ContextRoom::Full);
}

#[test]
fn a_fenced_context_waits_for_the_next_control_record() {
    let mut contexts = Contexts::new();
    let ids = contexts.fill();
    let closed = ids[3];
    contexts.control_record();
    contexts.release(closed);
    assert_eq!(contexts.room(), ContextRoom::Waiting, "its own record still names it");
    contexts.drop_hold(closed);
    assert_eq!(contexts.room(), ContextRoom::Waiting, "the firmware showed no progress since");
    contexts.control_record();
    assert_eq!(contexts.room(), ContextRoom::Ready(closed));
}

#[test]
fn a_ready_context_is_taken_before_a_waiting_one_oldest_first() {
    let mut contexts = Contexts::new();
    let ids = contexts.fill();
    let (held, first, second) = (ids[10], ids[20], ids[30]);
    contexts.hold(held);
    for id in [held, first, second] {
        contexts.control_record();
        contexts.release(id);
        contexts.drop_hold(id);
    }
    // `second` still waits for a record after its release; `held` for its channel.
    assert_eq!(contexts.create(), Some(first));
    assert_eq!(contexts.room(), ContextRoom::Waiting);
    contexts.control_record();
    assert_eq!(contexts.create(), Some(second));
    assert_eq!(contexts.room(), ContextRoom::Waiting);
    contexts.drop_hold(held);
    contexts.control_record();
    assert_eq!(contexts.create(), Some(held));
}

#[test]
fn a_context_the_firmware_never_saw_returns_at_once() {
    let mut contexts = Contexts::new();
    let ids = contexts.fill();
    let unpublished = ids[7];
    contexts.release(unpublished);
    // `ContextHold::drop` without `mark_seen`: no fence.
    contexts.shared.context_holds[unpublished] -= 1;
    assert_eq!(contexts.room(), ContextRoom::Ready(unpublished));
}

#[test]
fn the_creator_waits_bounded_then_is_refused() {
    use runtime::{context_wait_ms, RETAINED_CONTEXT_WAIT_NS, WAITING_CONTEXT_POLL_MS};
    let bound_ms = (RETAINED_CONTEXT_WAIT_NS / 1_000_000) as u32;
    assert!(WAITING_CONTEXT_POLL_MS > 0 && WAITING_CONTEXT_POLL_MS < bound_ms);
    // Waiting contexts become ready without a notification: poll, whatever is closing.
    for closing in [0, 3] {
        assert_eq!(context_wait_ms(ContextRoom::Waiting, closing, 0), Some(WAITING_CONTEXT_POLL_MS));
        assert_eq!(
            context_wait_ms(ContextRoom::Waiting, closing, RETAINED_CONTEXT_WAIT_NS - 2_500_000),
            Some(3)
        );
        assert_eq!(context_wait_ms(ContextRoom::Waiting, closing, RETAINED_CONTEXT_WAIT_NS), None);
        assert_eq!(context_wait_ms(ContextRoom::Waiting, closing, u64::MAX), None);
    }
    // No free context: wait for closing queues (their return notifies), else refuse at once.
    assert_eq!(context_wait_ms(ContextRoom::Full, 2, 0), Some(bound_ms));
    assert_eq!(context_wait_ms(ContextRoom::Full, 2, 1_000_000_000), Some(bound_ms - 1000));
    assert_eq!(context_wait_ms(ContextRoom::Full, 2, RETAINED_CONTEXT_WAIT_NS), None);
    assert_eq!(context_wait_ms(ContextRoom::Full, 0, 0), None);
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--mutation', choices=MUTATIONS)
    args = ap.parse_args()
    work = (args.out or Path(tempfile.mkdtemp(prefix='asahi-context-reuse-'))).resolve()
    work.mkdir(parents=True, exist_ok=True)

    mmu = (DRIVER / 'mmu.rs').read_text()
    runtime = (DRIVER / 'g17/runtime.rs').read_text()
    timeout = (DRIVER / 'g17/timeout.rs').read_text()
    job = (DRIVER / 'g17/job.rs').read_text()

    # The allocator takes only a ready context and has no other way to an identity.
    allocator = item(mmu, 'pub(crate) fn new_execution_context(')
    assert 'let ContextRoom::Ready(id) = inner.context_room(witnessed) else {' in allocator
    assert 'pick_returned' not in allocator and 'effective_fences' not in allocator
    assert allocator.count('inner.execution_contexts |= 1u64 << id;') == 1
    # The fixture's bookkeeping is the production one.
    release = item(mmu, 'impl ExecutionRoot {')
    for line in ('inner.context_clock += 1;', 'inner.context_returns[id] = inner.context_clock;',
                 'inner.execution_contexts &= !(1u64 << id);'):
        assert line in release, line
    drop = item(mmu, 'impl Drop for ContextHold {')
    for line in ('let fence = if self.seen.load(Ordering::Acquire) { (self.fence)() } else { 0 };',
                 'inner.context_holds[id] = inner.context_holds[id].saturating_sub(1);',
                 'inner.context_fences[id] = inner.context_fences[id].max(fence);',
                 'if inner.context_holds[id] == 0 && inner.execution_contexts & (1u64 << id) == 0 {'):
        assert line in drop, line
    # The creator asks before it reserves anything, waits as context_wait_ms says, and
    # refuses otherwise.
    creator = item(runtime, 'pub(super) fn new(\n        shared: &Arc<super::Shared>,\n        vm: &crate::mmu::Vm,')
    order = [creator.index(text) for text in (
        'super::channel::observe_control(consumer, producer);',
        'let room = firmware.uat.execution_context_room(witnessed);',
        'let slot = admission.reserve_queue()?;',
        'firmware.uat.new_execution_context(vm, witnessed)',
        'let Some(wait_ms) = context_wait_ms(room, closing, waited) else {',
        'return Err(super::capacity::refused(super::capacity::Site::ExecutionContexts));',
        'shared.queues.block(observed, wait_ms, true)?;',
    )]
    assert order == sorted(order), order
    assert 'pub(super) const TIMEOUT_MS: kernel::time::Msecs = super::timeout::POLL_MS;' in job
    poll_ms = re.search(r'pub\(crate\) const POLL_MS: u32 = (\d+);', timeout).group(1)

    shared = item(mmu, 'fn effective_fences(&self)') + '\n' + item(mmu, 'fn context_room(&self, witnessed: u64)')
    choice = item(mmu, 'fn context_room(free: u64')
    wait = item(runtime, 'fn context_wait_ms(')
    if args.mutation == 'take-waiting':
        choice = swap(choice, 'Some((_, false)) => ContextRoom::Waiting,', 'Some((id, false)) => ContextRoom::Ready(id),')
    if args.mutation == 'ignore-holds':
        shared = swap(shared, 'if holds != 0 {', 'if false && holds != 0 {')
    if args.mutation == 'wait-past-deadline':
        wait = swap(wait, 'if waited_ns >= RETAINED_CONTEXT_WAIT_NS {', 'if false {')
        wait = swap(wait, '(RETAINED_CONTEXT_WAIT_NS - waited_ns)', 'RETAINED_CONTEXT_WAIT_NS.saturating_sub(waited_ns)')

    fixture = '\n'.join([
        '#![allow(dead_code)]',
        'mod mmu {',
        constant(mmu, 'UAT_NUM_CTX').replace('const ', 'pub const ', 1),
        constant(mmu, 'EXECUTION_CONTEXT_MASK').replace('const ', 'pub const ', 1),
        'pub struct UatShared {',
        '    pub execution_contexts: u64,',
        '    pub context_returns: [u64; UAT_NUM_CTX],',
        '    pub context_fences: [u64; UAT_NUM_CTX],',
        '    pub context_holds: [u32; UAT_NUM_CTX],',
        '    pub context_clock: u64,',
        '}',
        'impl UatShared {',
        shared.replace('fn context_room(&self', 'pub fn context_room(&self', 1),
        '}',
        '#[derive(Clone, Copy, Debug, PartialEq, Eq)]',
        item(mmu, 'pub(crate) enum ContextRoom {'),
        choice,
        item(mmu, 'pub(crate) fn pick_returned('),
        '}',
        'mod runtime {',
        f'const WORK_RING_WAIT_NS: u64 = {poll_ms} as u64 * 1_000_000;',
        constant(runtime, 'RETAINED_CONTEXT_WAIT_NS').replace('const ', 'pub const ', 1),
        constant(runtime, 'WAITING_CONTEXT_POLL_MS').replace('const ', 'pub const ', 1),
        'pub ' + wait,
        '}',
        TESTS,
    ])
    (work / 'test.rs').write_text(fixture)
    subprocess.run(['rustc', '--edition=2021', '--test', str(work / 'test.rs'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode if error.returncode > 0 else 128 - error.returncode)
