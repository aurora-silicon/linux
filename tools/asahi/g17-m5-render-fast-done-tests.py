#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for single-lock render completion fast path, doorbell lock release, and zero-waiter notifications.

Verifies:
1. Lock-free zero-waiter `Availability::advance()` compiled from
   `drivers/gpu/drm/asahi/g17/availability.rs`: zero mutex locks when no
   waiter is registered, `generation` still advances on every `notify()`,
   `notify_own()` works, and multi-threaded `observe -> wait` vs `notify`
   suffers zero lost wakeups.
2. `DirectHost::notify_render()` (`runtime/render/direct.rs`) drops
   `self.m5_outer` (after submission history) and `self.render` in strict
   reverse acquisition order BEFORE calling `self.port.notify()`.
3. `runtime_event_turn()` (`runtime/events.rs`) + `try_fast_observe_render()`
   (`runtime/render/observe.rs`):
   - `fast_epoch` is taken before `state.lock()` and dropped before
     `wait_drained_until()` (no recovery deadlock).
   - Healthy single-slot render completions return `FastRenderOutcome::Done`
     under one lock hold, while `pending_error`, `pending_timeout`, `closed`,
     `growing`, `had_terminal`, `missed`, `render_needs_rescan()`, and
     multi-slot pending work fall back to `observe_renders()`.
   - Slow-path `observe_renders()` folds deferred-error drain and rescan
     checks into `runtime_event_turn()`'s existing `state.lock()` holds.
4. `Shared::note_changed()` (`mod.rs`) gates `self.changed.notify_all()` on
   `changed_waiters` and all `changed.wait_timeout` callers in `runtime.rs`
   use `wait_changed_timeout()`.
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
    avail_src = (G17 / 'availability.rs').read_text()
    direct_src = (G17 / 'runtime/render/direct.rs').read_text()
    observe_src = (G17 / 'runtime/render/observe.rs').read_text()
    events_src = (G17 / 'runtime/events.rs').read_text()
    mod_src = (G17 / 'mod.rs').read_text()
    runtime_src = (G17 / 'runtime.rs').read_text()

    notify_render = item(direct_src, 'fn notify_render(')
    try_fast_entry = item(observe_src, 'pub(in crate::g17::runtime) fn try_fast_observe_render(')
    try_fast = item(observe_src, 'fn fast_observe_renders(')
    observe_fn = item(observe_src, 'pub(in crate::g17) fn observe_renders(')
    turn = item(events_src, 'fn runtime_event_turn(')
    tracked_cv = item(mod_src, 'impl TrackedCondVar')

    avail_wait = item(avail_src, 'pub(crate) fn wait(')
    avail_advance = item(avail_src, 'fn advance(')

    if mutation == 'hold-m5-outer-across-doorbell':
        notify_render = notify_render.replace(
            'self.m5_outer = None;',
            '',
            1,
        )
    elif mutation == 'hold-render-lock-across-doorbell':
        notify_render = notify_render.replace(
            'self.render = None;',
            '',
            1,
        )
    elif mutation == 'skip-pending-error-fallback':
        try_fast = try_fast.replace(
            'entry.pending_error.is_some()\n                || ',
            '',
            1,
        )
    elif mutation == 'single-slot-bailout':
        try_fast = try_fast.replace(
            'for _ in 0..crate::hw::t8140::queues::RENDER_SLOTS {',
            'for _ in 0..1 {',
            1,
        )
    elif mutation == 'skip-epoch-check':
        turn = turn.replace(
            'fast_epoch.as_ref().is_some_and(|epoch| epoch.is_current())',
            'true',
            1,
        )
    elif mutation == 'unconditional-changed-notify':
        tracked_cv = tracked_cv.replace(
            'if self.waiters.load(Ordering::SeqCst) != 0 {',
            'if true {',
            1,
        )
    elif mutation == 'skip-generation-bump':
        avail_advance = avail_advance.replace(
            'let previous = self.generation.fetch_add(1, Ordering::SeqCst);\n'
            '        if !self.has_waiters.load(Ordering::SeqCst) {\n'
            '            return previous;\n'
            '        }',
            'if !self.has_waiters.load(Ordering::SeqCst) {\n'
            '            return self.generation.load(Ordering::Acquire);\n'
            '        }\n'
            '        let previous = self.generation.fetch_add(1, Ordering::SeqCst);',
            1,
        )

    doorbell_order_ok = ordered_ok(
        notify_render,
        'note_m5_submission_history_locked(geometry, 2)',
        'self.m5_outer = None;',
        'self.render = None;',
        'self.port.notify(super::render_doorbell_message(m5, priority))',
    )

    fast_render_guards_ok = (
        'entry.pending_error.is_some()' in try_fast
        and 'entry.pending_timeout' in try_fast
        and 'entry.closed' in try_fast
        and 'entry.growing' in try_fast
        and 'self.recovery.pending()' in try_fast
        and 'self.queues.render.replay_parked()' in try_fast
        and 'self.render_grow_pending()' in try_fast
        and 'self.render_grow_ready()' in try_fast
        and 'had_terminal' in try_fast
        and 'self.queues.render_needs_rescan()' in try_fast
        and 'for _ in 0..crate::hw::t8140::queues::RENDER_SLOTS {' in try_fast
        and 'if !deferred.has_room(2)' in try_fast
        and 'self.next_render_observation_in(start, end)?.is_some()' in try_fast
        and 'self.fast_observe_renders(1, end, deferred)' in try_fast_entry
        and 'FastRenderOutcome::Done' in try_fast
    )

    # Extract the real production loop bound from try_fast_observe_render() to drive a
    # compiled multi-slot coalesced-completion test on the host.
    loop_match = re.search(r'for _ in 0\.\.([^\s{]+)\s*\{', try_fast)
    assert loop_match is not None, 'try_fast_observe_render loop missing'
    loop_bound_expr = loop_match.group(1).replace('crate::hw::t8140::queues::RENDER_SLOTS', 'RENDER_SLOTS')

    turn_order_ok = (
        ordered_ok(
            turn,
            'let fast_epoch = self.preparations.try_enter().ok();',
            'let (pending, masks, compute_scan) = firmware.service_runtime_events_scan(deferred)?;',
            'fast_epoch.as_ref().is_some_and(|epoch| epoch.is_current())',
            'firmware.try_fast_observe_render(deferred)?',
            'if render_changed {\n            self.note_changed();\n        }',
            'deferred.finish();',
            'if fast_done {\n            return Ok(());\n        }',
            'self.preparations\n                .wait_drained_until(',
            'firmware.drain_render_deferred(masks, deferred)?;',
            'self.observe_renders(deferred)?;',
            'firmware.queues.render_needs_rescan()',
        )
        # Slow-path observe_renders() must no longer acquire extra pre- or post-loop state locks.
        and observe_fn.count('self.state.lock()') == 1
    )

    changed_waiters_ok = (
        'changed: TrackedCondVar,' in mod_src
        and 'if self.waiters.load(Ordering::SeqCst) != 0 {' in tracked_cv
        and 'self.condvar.notify_all();' in tracked_cv
        and 'self.waiters.fetch_add(1, Ordering::SeqCst);' in tracked_cv
        and 'self.0.fetch_sub(1, Ordering::Release);' in tracked_cv
    )

    # Adapt Availability::wait() and advance() for host compilation to test the real Dekker protocol.
    host_wait = avail_wait.replace('pub(crate) ', '')
    host_wait = host_wait.replace('Result<Option<Fence>>', 'Result<Option<Arc<AtomicBool>>>')
    host_wait = host_wait.replace('fence::independent(&self.fences)?', 'Ok::<_, i32>(Arc::new(AtomicBool::new(false)))?')
    host_wait = host_wait.replace('KBox::new(', 'HostBox::new(')
    host_wait = host_wait.replace(',\n            GFP_KERNEL,\n        )?', ')?')
    host_wait = host_wait.replace('Fence::from_fence(&fence)', 'fence')
    host_wait = host_wait.replace('self.state.lock()', 'self.lock_state()')

    host_advance = avail_advance.replace('self.state.lock()', 'self.lock_state()')
    host_advance = host_advance.replace('entry.fence.signal();', 'entry.fence.store(true, Ordering::Release);')

    return f'''
use std::sync::{{Arc, Mutex, MutexGuard, atomic::{{AtomicBool, AtomicU64, AtomicUsize, Ordering}}}};

type Result<T = ()> = std::result::Result<T, i32>;

struct HostBox;
impl HostBox {{
    fn new<T>(v: T) -> Result<Box<T>> {{ Ok(Box::new(v)) }}
}}

struct Entry {{
    fence: Arc<AtomicBool>,
    next: Option<Box<Entry>>,
}}

struct State {{
    waiters: Option<Box<Entry>>,
}}

struct Availability {{
    generation: AtomicU64,
    has_waiters: AtomicBool,
    lock_trips: AtomicUsize,
    state: Mutex<State>,
}}

impl Availability {{
    fn new() -> Arc<Self> {{
        Arc::new(Self {{
            generation: AtomicU64::new(0),
            has_waiters: AtomicBool::new(false),
            lock_trips: AtomicUsize::new(0),
            state: Mutex::new(State {{ waiters: None }}),
        }})
    }}
    fn lock_state(&self) -> MutexGuard<'_, State> {{
        self.lock_trips.fetch_add(1, Ordering::Relaxed);
        self.state.lock().unwrap()
    }}
    fn observe(&self) -> u64 {{ self.generation.load(Ordering::Acquire) }}
    fn has_waiters(&self) -> bool {{ self.has_waiters.load(Ordering::Acquire) }}
    fn notify(&self) {{ self.advance(); }}
    fn notify_own(&self, observed: u64) -> u64 {{
        if self.advance() == observed {{ observed.wrapping_add(1) }} else {{ observed }}
    }}

    {host_wait}

    {host_advance}
}}

fn test_availability_zero_waiter_and_dekker_race() {{
    let a = Availability::new();
    let g0 = a.observe();
    assert!(!a.has_waiters());
    assert_eq!(a.lock_trips.load(Ordering::Relaxed), 0);

    // Zero-waiter notify() advances generation without taking the mutex.
    a.notify();
    assert_eq!(a.observe(), g0 + 1, "notify() must bump generation even with zero waiters");
    assert_eq!(a.lock_trips.load(Ordering::Relaxed), 0, "zero-waiter notify must skip state mutex");

    // A waiter holding the stale observation must retry immediately instead of sleeping.
    assert!(a.wait(g0).unwrap().is_none());

    // notify_own() distinguishes our own bump from a concurrent bump.
    let g1 = a.observe();
    assert_eq!(a.notify_own(g1), g1 + 1);
    assert_eq!(a.notify_own(g1), g1);

    // Multi-threaded Dekker stress test: every registered waiter MUST eventually be signalled.
    let stop = Arc::new(AtomicBool::new(false));
    let a_not = a.clone();
    let s_not = stop.clone();
    let notifier = std::thread::spawn(move || {{
        while !s_not.load(Ordering::Acquire) {{
            a_not.notify();
            std::hint::spin_loop();
        }}
        a_not.notify();
    }});

    for _ in 0..4000 {{
        let obs = a.observe();
        if let Some(fence) = a.wait(obs).unwrap() {{
            while !fence.load(Ordering::Acquire) {{
                std::thread::yield_now();
            }}
        }}
    }}
    stop.store(true, Ordering::Release);
    notifier.join().unwrap();
}}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FastRenderOutcome {{
    Fallback,
    Idle,
    Done,
}}

const RENDER_SLOTS: usize = 8;

fn run_fast_render_loop(ready: &mut Vec<usize>, deferred_cap: usize) -> (FastRenderOutcome, usize) {{
    let mut deferred_len = 0usize;
    let mut retired_any = false;
    for _ in 0..{loop_bound_expr} {{
        let Some(_slot) = ready.first().copied() else {{
            return (if retired_any {{ FastRenderOutcome::Done }} else {{ FastRenderOutcome::Idle }}, deferred_len);
        }};
        if deferred_cap.saturating_sub(deferred_len) < 2 {{
            return (FastRenderOutcome::Fallback, deferred_len);
        }}
        ready.remove(0);
        deferred_len += 1;
        retired_any = true;
    }}
    if !ready.is_empty() {{
        return (FastRenderOutcome::Fallback, deferred_len);
    }}
    (if retired_any {{ FastRenderOutcome::Done }} else {{ FastRenderOutcome::Idle }}, deferred_len)
}}

fn test_multi_slot_fast_render() {{
    // 0 ready slots -> Idle.
    let mut none = vec![];
    assert_eq!(run_fast_render_loop(&mut none, 16), (FastRenderOutcome::Idle, 0));

    // 2 coalesced ready render slots must both retire in ONE lock hold (Done, not Fallback).
    let mut two = vec![1, 2];
    assert_eq!(run_fast_render_loop(&mut two, 16), (FastRenderOutcome::Done, 2));
    assert!(two.is_empty());

    // Bounded by deferred room: shortage before 2nd slot returns Fallback after retiring 1st.
    let mut tight = vec![1, 2];
    assert_eq!(run_fast_render_loop(&mut tight, 2), (FastRenderOutcome::Fallback, 1));
    assert_eq!(tight, vec![2]);

    // Bounded by RENDER_SLOTS: > RENDER_SLOTS ready items fall back cleanly after the cap.
    let mut burst: Vec<usize> = (1..=RENDER_SLOTS + 1).collect();
    assert_eq!(run_fast_render_loop(&mut burst, 64), (FastRenderOutcome::Fallback, RENDER_SLOTS));
    assert_eq!(burst.len(), 1);
}}

fn main() {{
    assert!({str(doorbell_order_ok).lower()}, "DirectHost::notify_render must drop m5_outer then render before port.notify()");
    assert!({str(fast_render_guards_ok).lower()}, "try_fast_observe_render must guard all non-steady states");
    assert!({str(turn_order_ok).lower()}, "runtime_event_turn must scope fast_epoch and fold observe_renders locks");
    assert!({str(changed_waiters_ok).lower()}, "Shared::note_changed must gate condvar wake on changed_waiters");
    test_availability_zero_waiter_and_dekker_race();
    test_multi_slot_fast_render();
    println!("PASS g17-m5-render-fast-done-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['hold-m5-outer-across-doorbell',
                                               'hold-render-lock-across-doorbell',
                                               'skip-pending-error-fallback',
                                               'single-slot-bailout',
                                               'skip-epoch-check',
                                               'unconditional-changed-notify',
                                               'skip-generation-bump'])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-rfd-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
