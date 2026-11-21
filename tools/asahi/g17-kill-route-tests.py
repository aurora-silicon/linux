#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""G17 kill route of a closed context: executed render pairs do not hold it back.

A closed context's render pair whose commands all ran but were never stamped
keeps its pool until a kill of the context is acknowledged. Its compute exit
waited for that pool, and the render-only kill for the compute queue that
still held commands retired without their stamps (or timed out), so neither
kill was ever published. The real route choice and its inputs
(runtime/compute_exit.rs), the compute queue's exit predicates
(queue/compute/exit.rs, queue/compute.rs) and the KUnit cases of the route
are compiled against mock firmware state. The rows are the contexts of the
11-15 debugfs dumps: context 11 (compute retired without its stamp, an
executed pair holding its pool) and context 27 (timed-out compute, an executed
pair) take the compute exit and wait only for other contexts' compute; a
closed context whose compute still runs, a context already killed and M5 do
not; the old routes are unchanged. --mutation removes one part of that and
must fail.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi'
G17 = ROOT / 'g17'


def item(source, signature, attributes=False):
    """The item that starts at `signature`, through its closing brace."""
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    if attributes:
        while True:
            line = source.rfind('\n', 0, start - 1) + 1
            if not source[line:start].strip().startswith('#['):
                break
            start = line
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def line(source, prefix):
    found = [text for text in source.splitlines() if text.strip().startswith(prefix)]
    assert len(found) == 1, prefix
    return found[0]


def fixture():
    control = (G17 / 'runtime/compute_exit.rs').read_text()
    exit = (G17 / 'queue/compute/exit.rs').read_text()
    compute = (G17 / 'queue/compute.rs').read_text()
    free = '\n'.join([
        line(control, 'const RENDER_KILL_DELAY_NS:'),
        item(control, 'fn render_kill_target('),
        item(control, 'fn render_kill_eligible('),
        item(control, 'enum KillRoute {', True),
        item(control, 'fn kill_route('),
        item(control, 'fn hold_owner('),
        item(control, 'fn waits_as_failed('),
        '#[cfg(test)]\n' + item(control, 'mod render_kill_target_tests {'),
    ])
    firmware = '\n'.join(item(control, signature) for signature in [
        'fn kill_route_for(', 'fn kill_releases_render(', 'fn kill_waits_as_failed(',
        'fn render_kill_candidate(', 'fn foreign_compute_running(', 'fn compute_exit_pending(',
        'fn compute_failed('])
    queue = '\n'.join([item(exit, signature) for signature in [
        'fn active_traversal(', 'pub(crate) fn exit_candidate(', 'fn holds_only_owed_stamp(',
        'pub(crate) fn running_commands(', 'pub(crate) fn timed_out(', 'pub(crate) fn exit_published(']]
        + [item(compute, signature) for signature in [
        'pub(crate) fn fault_pending(', 'pub(crate) fn context(', 'pub(crate) fn switching_from(',
        'pub(crate) fn idle_for_context(', 'pub(crate) fn in_flight(']])
    delays = '\n'.join([line(exit, 'const QUARANTINED_DELAY_NS:'), line(exit, 'const CANCELLED_DELAY_NS:')])
    return r'''
#![allow(warnings)]
use std::cell::Cell;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(i32);
pub const ETIMEDOUT: Error = Error(-110);
pub const ECANCELED: Error = Error(-125);

const LIVENESS_NS: u64 = 5_000_000_000;
const MS: u64 = 1_000_000;

mod g17 {
    pub(crate) mod recovery {
        use std::cell::Cell;
        thread_local! { pub(crate) static SINCE_RETIRED: Cell<Option<u64>> = Cell::new(Some(1_000_000)); }
        pub(crate) fn since_retired(_now: u64) -> Option<u64> { SINCE_RETIRED.with(|since| since.get()) }
    }
    pub(crate) mod command {
        pub(crate) struct Scratch(pub(crate) bool);
        impl Scratch { pub(crate) fn enabled(&self) -> bool { self.0 } }
        pub(crate) enum Validated { Compute { scratch: Scratch }, Render }
    }
}

pub struct Context { killed: Cell<bool> }
impl Context { fn kill_published(&self) -> bool { self.killed.get() } }

pub struct Packet { command: g17::command::Validated, context: Arc<Context> }
pub struct Active { packet: Arc<Packet>, stamps_owed: bool }
pub struct Ring<T> { len: usize, items: Vec<T> }
impl<T> Ring<T> {
    fn new() -> Self { Self { len: 0, items: Vec::new() } }
    fn push(&mut self, item: T) { self.items.push(item); self.len += 1; }
    fn iter(&self) -> std::slice::Iter<'_, T> { self.items.iter() }
}
pub struct Graph { m5: bool }
pub struct Binding { context: Arc<Context> }
pub struct Previous { publication: Option<(u64, u64)>, context: Arc<Context> }
pub struct Transaction;

pub struct Queue {
    binding: Option<Binding>,
    graph: Graph,
    released: bool,
    quarantined: bool,
    quarantine_error: Option<Error>,
    spared_quarantine: bool,
    retired_by_teardown: bool,
    fault_stamp: Option<u32>,
    awaiting_witness: bool,
    exit: Option<Transaction>,
    active: Ring<Active>,
    replays: Ring<()>,
    configuration: Option<()>,
    previous: Option<Previous>,
}
''' + delays + r'''
impl Queue {
''' + queue + r'''
}

pub struct ComputeSlot(Option<Box<Queue>>);
impl ComputeSlot { fn as_deref(&self) -> Option<&Queue> { self.0.as_deref() } }
pub struct ComputeEntry { queue: ComputeSlot }
pub struct Render { executed: Vec<(Arc<Context>, u64)>, guilty: Vec<(Arc<Context>, u64)>, pools_released: Vec<Arc<Context>> }
fn slots_of(pairs: &[(Arc<Context>, u64)], context: &Arc<Context>) -> u64 {
    pairs.iter().filter(|(owner, _)| Arc::ptr_eq(owner, context)).fold(0, |slots, (_, slot)| slots | 1u64 << slot)
}
impl Render {
    fn executed_slots(&self, context: &Arc<Context>) -> u64 { slots_of(&self.executed, context) }
    fn guilty_slots(&self, context: &Arc<Context>, _generation: u64) -> u64 { slots_of(&self.guilty, context) }
    fn kill_slots(&self, context: &Arc<Context>, generation: u64) -> u64 {
        self.executed_slots(context) | self.guilty_slots(context, generation)
    }
    fn teardown_pools_released(&self, context: &Arc<Context>) -> bool {
        self.pools_released.iter().any(|owner| Arc::ptr_eq(owner, context))
    }
}
pub struct Teardown { render_kills: Vec<bool> }
impl Teardown { fn render_kill_published(&self, index: usize) -> bool { self.render_kills[index] } }
pub struct Init { m5: bool }
impl Init { fn is_m5(&self) -> bool { self.m5 } }
pub struct Queues { compute: Vec<Option<ComputeEntry>>, render: Render, teardown: Teardown }
pub struct Recovery;
impl Recovery { fn generation(&self) -> u64 { 1 } }
pub struct Firmware { queues: Queues, init: Init, recovery: Recovery }
impl Firmware {
''' + firmware + r'''
}
''' + free + r'''

#[cfg(test)]
mod tests {
    use super::*;

    fn context() -> Arc<Context> { Arc::new(Context { killed: Cell::new(false) }) }

    /// A queue bound to `context`, its owner open, holding no command.
    fn queue(context: &Arc<Context>) -> Queue {
        Queue {
            binding: Some(Binding { context: context.clone() }),
            graph: Graph { m5: false },
            released: false,
            quarantined: false,
            quarantine_error: None,
            spared_quarantine: false,
            retired_by_teardown: false,
            fault_stamp: None,
            awaiting_witness: false,
            exit: None,
            active: Ring::new(),
            replays: Ring::new(),
            configuration: None,
            previous: None,
        }
    }

    fn command(queue: &mut Queue, owed: bool) {
        let context = queue.binding.as_ref().unwrap().context.clone();
        let command = g17::command::Validated::Compute { scratch: g17::command::Scratch(false) };
        queue.active.push(Active { packet: Arc::new(Packet { command, context }), stamps_owed: owed });
    }

    /// The owner closed and its only command retired on its record, its stamp owed (qid 59).
    fn owed(context: &Arc<Context>) -> Queue {
        let mut q = queue(context);
        q.released = true;
        command(&mut q, true);
        q
    }

    /// The owner closed after the watchdog failed its command: 12 held (qid 47).
    fn timed_out(context: &Arc<Context>) -> Queue {
        let mut q = queue(context);
        q.released = true;
        q.quarantined = true;
        q.quarantine_error = Some(ETIMEDOUT);
        for _ in 0..12 {
            command(&mut q, false);
        }
        q
    }

    /// The owner closed while its command still runs.
    fn running(context: &Arc<Context>) -> Queue {
        let mut q = queue(context);
        q.released = true;
        command(&mut q, false);
        q
    }

    fn firmware(queues: Vec<Queue>) -> Firmware {
        Firmware {
            queues: Queues {
                compute: queues.into_iter().map(|q| Some(ComputeEntry { queue: ComputeSlot(Some(Box::new(q))) })).collect(),
                render: Render { executed: Vec::new(), guilty: Vec::new(), pools_released: Vec::new() },
                teardown: Teardown { render_kills: vec![false; 4] },
            },
            init: Init { m5: false },
            recovery: Recovery,
        }
    }

    const CLOSED: u64 = 3_000 * MS;

    #[test]
    fn the_route_rows() {
        // Context 11: owed-only compute (exit pending), not failed, an executed pair, pools held.
        assert_eq!(kill_route(false, false, true, true, false), KillRoute::Compute);
        // Context 27: timeout-quarantined compute (exit pending, not failed on Neo), executed pair.
        assert_eq!(kill_route(false, false, true, true, false), KillRoute::Compute);
        // A closed context whose compute still runs: no exit pending, not render-eligible.
        assert_eq!(kill_route(false, false, true, false, false), KillRoute::None);
        assert_eq!(kill_route(false, false, false, false, false), KillRoute::None);
    }

    #[test]
    fn context_11_takes_its_compute_exit_and_waits_as_a_failed_context() {
        let c11 = context();
        let mut fw = firmware(vec![owed(&c11)]);
        fw.queues.render.executed.push((c11.clone(), 32));
        assert!(fw.compute_exit_pending(&c11));
        assert!(!fw.compute_failed(&c11), "an owed stamp is no failure");
        assert!(!fw.queues.render.teardown_pools_released(&c11), "slot 32 holds its pool");
        assert!(!fw.render_kill_candidate(0, &c11, CLOSED), "qid 59 still holds its command");
        assert_eq!(fw.kill_route_for(0, &c11, CLOSED), KillRoute::Compute);
        assert!(fw.kill_waits_as_failed(&c11), "not the silent kill_unsafe wait");
        // Without the executed pair, as before: the compute exit waits for the pools.
        fw.queues.render.executed.clear();
        assert_eq!(fw.kill_route_for(0, &c11, CLOSED), KillRoute::None);
        assert!(!fw.kill_waits_as_failed(&c11));
        fw.queues.render.pools_released.push(c11.clone());
        assert_eq!(fw.kill_route_for(0, &c11, CLOSED), KillRoute::Compute);
        assert!(!fw.kill_waits_as_failed(&c11), "kill_unsafe as before");
    }

    #[test]
    fn context_27_takes_its_compute_exit() {
        let c27 = context();
        let mut fw = firmware(vec![timed_out(&c27)]);
        fw.queues.render.executed.push((c27.clone(), 40));
        assert!(fw.compute_exit_pending(&c27));
        assert!(!fw.compute_failed(&c27), "a timeout is not fault_pending on Neo");
        assert_eq!(fw.kill_route_for(0, &c27, CLOSED), KillRoute::Compute);
        assert!(fw.kill_waits_as_failed(&c27));
    }

    #[test]
    fn a_closed_context_whose_compute_still_runs_is_not_killed() {
        let c = context();
        let mut fw = firmware(vec![running(&c)]);
        fw.queues.render.executed.push((c.clone(), 8));
        assert!(!fw.compute_exit_pending(&c));
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
        fw.queues.render.executed.clear();
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
    }

    #[test]
    fn a_killed_context_is_not_killed_again_through_another_compute_queue() {
        let c = context();
        let mut published = owed(&c);
        published.exit = Some(Transaction);
        let mut fw = firmware(vec![published, owed(&c)]);
        fw.queues.render.executed.push((c.clone(), 32));
        c.killed.set(true);
        assert!(fw.compute_exit_pending(&c));
        assert!(!fw.kill_releases_render(&c));
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
        assert!(!fw.kill_waits_as_failed(&c));
    }

    #[test]
    fn m5_routes_are_unchanged() {
        let c = context();
        let mut q = owed(&c);
        q.graph.m5 = true;
        let mut fw = firmware(vec![q]);
        fw.init.m5 = true;
        fw.queues.render.executed.push((c.clone(), 32));
        assert!(fw.compute_exit_pending(&c));
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
        assert!(!fw.kill_waits_as_failed(&c));
        // An M5 timeout is failed work there.
        let c = context();
        let mut q = timed_out(&c);
        q.graph.m5 = true;
        let mut fw = firmware(vec![q]);
        fw.init.m5 = true;
        fw.queues.render.executed.push((c.clone(), 40));
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::Compute);
    }

    #[test]
    fn the_old_routes_are_unchanged() {
        // A fault the firmware reported, with commands held behind it: failed.
        let c = context();
        let mut q = timed_out(&c);
        q.quarantine_error = Some(ECANCELED);
        q.fault_stamp = Some(7);
        q.awaiting_witness = true;
        let fw = firmware(vec![q]);
        assert!(fw.compute_failed(&c));
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::Compute);
        assert!(fw.kill_waits_as_failed(&c));
        // Render-only: no compute queue holds the context, an executed pair.
        let c = context();
        let other = context();
        let mut fw = firmware(vec![running(&other)]);
        fw.queues.render.executed.push((c.clone(), 12));
        assert_eq!(fw.kill_route_for(0, &c, RENDER_KILL_DELAY_NS), KillRoute::Render);
        assert_eq!(fw.kill_route_for(0, &c, RENDER_KILL_DELAY_NS - 1), KillRoute::None);
        fw.queues.teardown.render_kills[0] = true;
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
        fw.queues.teardown.render_kills[0] = false;
        c.killed.set(true);
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
        // A queue with a traversal still running is never an exit candidate.
        let c = context();
        let mut q = timed_out(&c);
        q.active.items[0].packet = Arc::new(Packet {
            command: g17::command::Validated::Compute { scratch: g17::command::Scratch(true) },
            context: c.clone(),
        });
        let mut fw = firmware(vec![q]);
        fw.queues.render.executed.push((c.clone(), 40));
        assert!(!fw.compute_exit_pending(&c));
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
    }

    #[test]
    fn a_closed_guilty_context_is_killed_at_once_without_waiting_for_foreign_render() {
        // Render-only: a pair with a spinning group and never-started groups behind it, its VM
        // guilty (not executed). Another client's compute runs.
        let c = context();
        let other = context();
        let mut fw = firmware(vec![running(&other)]);
        fw.queues.render.guilty.push((c.clone(), 13));
        assert_eq!(fw.kill_route_for(0, &c, 0), KillRoute::Render);
        assert!(fw.kill_releases_render(&c));
        assert!(fw.kill_waits_as_failed(&c));
        // Once only, and never on M5.
        c.killed.set(true);
        assert_eq!(fw.kill_route_for(0, &c, CLOSED), KillRoute::None);
        assert!(!fw.kill_releases_render(&c));
        let c = context();
        let mut fw = firmware(vec![running(&other)]);
        fw.init.m5 = true;
        fw.queues.render.guilty.push((c.clone(), 13));
        assert!(!fw.kill_releases_render(&c));
    }

    #[test]
    fn a_kill_does_not_wait_for_another_closed_contexts_owed_compute() {
        let (c5, c11, live) = (context(), context(), context());
        let now = 10_000 * MS;
        // Contexts 5 and 11, each with a closed queue holding only owed commands.
        let fw = firmware(vec![owed(&c5), owed(&c11)]);
        assert!(!fw.foreign_compute_running(&c5, now));
        assert!(!fw.foreign_compute_running(&c11, now));
        // Another client's running command is waited for, as before.
        let mut open = queue(&live);
        command(&mut open, false);
        let fw = firmware(vec![owed(&c5), owed(&c11), open]);
        assert!(fw.foreign_compute_running(&c5, now));
        // So is an open owner's queue whose commands only wait for their stamps.
        let mut open = queue(&live);
        command(&mut open, true);
        let fw = firmware(vec![owed(&c5), open]);
        assert!(fw.foreign_compute_running(&c5, now));
        // A quarantined queue is not in flight; a stalled GPU and M5 wait for nothing.
        let mut fw = firmware(vec![owed(&c5), timed_out(&live)]);
        assert!(!fw.foreign_compute_running(&c5, now));
        let mut open = queue(&live);
        command(&mut open, false);
        let mut fw = firmware(vec![open]);
        assert!(fw.foreign_compute_running(&c5, now));
        g17::recovery::SINCE_RETIRED.with(|since| since.set(Some(LIVENESS_NS)));
        assert!(!fw.foreign_compute_running(&c5, now));
        g17::recovery::SINCE_RETIRED.with(|since| since.set(Some(MS)));
        fw.init.m5 = true;
        assert!(!fw.foreign_compute_running(&c5, now));
    }
}
'''


# Each restores old behaviour or drops one part of the rule.
MUTATIONS = {
    # The route before this change: a compute exit only for failed compute or released pools.
    'no-executed-route': [('    if exit_pending && (failed || pools_released || executed) {',
                           '    if exit_pending && (failed || pools_released) {')],
    'no-executed-input': [('            self.kill_releases_render(context),\n', '            false,\n')],
    'kill-twice': [('!self.init.is_m5() && !context.kill_published() && self.queues.render.kill_slots(context, self.recovery.generation()) != 0',
                    '!self.init.is_m5() && self.queues.render.kill_slots(context, self.recovery.generation()) != 0')],
    'executed-on-m5': [('!self.init.is_m5() && !context.kill_published() && self.queues.render.kill_slots(context, self.recovery.generation()) != 0',
                        '!context.kill_published() && self.queues.render.kill_slots(context, self.recovery.generation()) != 0')],
    'unsafe-wait': [('waits_as_failed(self.compute_failed(context), self.kill_releases_render(context))',
                     'self.compute_failed(context)')],
    # A guilty pair waits for the retirement delay like an executed one, or is not a candidate.
    'guilty-waits-delay': [('(guilty || (executed && age >= RENDER_KILL_DELAY_NS))',
                            '((guilty || executed) && age >= RENDER_KILL_DELAY_NS)')],
    'no-guilty-input': [('            self.queues.render.guilty_slots(context, self.recovery.generation()) != 0,\n',
                         '            false,\n')],
    'owed-blocks-kill': [('queue.running_commands() && !queue.context()', 'queue.in_flight() && !queue.context()')],
    'open-owed-skipped': [('self.in_flight() && !self.holds_only_owed_stamp()',
                           'self.in_flight() && !self.active.iter().all(|active| active.stamps_owed)')],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()
    code = fixture()
    for old, new in MUTATIONS.get(args.mutation, []):
        assert code.count(old) == 1, (args.mutation, old)
        code = code.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='g17-kill-route-') as tmp:
        path = Path(tmp) / 'route.rs'
        binary = path.with_suffix('')
        path.write_text(code)
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '--test', str(path),
                        '-o', str(binary)], check=True)
        # A killed mutant exits with the failed test's panic in its output.
        result = subprocess.run([str(binary)])
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
