#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test: G17 event rescan uses a sub-ms hrtimer and never starves pending event rings.

Verifies:
1. Compiled production `Shared::queue_poll()`, `Shared::cancel_rescan()`,
   `Shared::disable_rescan()`, and `HrTimerCallback::run()` from
   `drivers/gpu/drm/asahi/g17/mod.rs`:
   - `queue_poll()` arms the hrtimer for a sub-millisecond delay (`<= 100 us`),
     coalesces duplicate calls while the deadline is still in the future without
     acquiring `rescan_handle`, and re-arms once expired or cleared;
   - `HrTimerCallback::run()` clears `rescan_deadline_ns`, queues
     `queue_event_records()` when active, skips queuing when sleeping, stopped,
     or crashed, and returns `HrTimerRestart::NoRestart`;
   - `cancel_rescan()` and `disable_rescan()` drop the handle and clear the
     deadline, and `disable_rescan()` blocks any later `queue_poll()`.
2. Structural checks on `runtime/events.rs`, `mod.rs`, `sleep.rs`, and `runtime.rs`:
   - Neither the fast path nor the slow path of `runtime_event_turn()` gates
     `channel::Events::pending()` behind `!polling` / `else if`;
   - Both paths still arm `queue_poll_after(1)` when `render_control_backpressured()`
     is true so the 1 ms retry budget on `POLL_WORK` is preserved;
   - `join_event_workers()`, `stop_runtime()`, and `shutdown()` cancel or disable
     the rescan hrtimer.
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


MUTATIONS = {
    'starve-fast-pending': [(
        'events',
        '                        if polling {\n'
        '                            self.queue_poll();\n'
        '                            if firmware.render_control_backpressured() {\n'
        '                                self.queue_poll_after(1);\n'
        '                            }\n'
        '                        }\n'
        '                        if channel::Events::pending(&firmware.init, Role::Primary)?',
        '                        if polling {\n'
        '                            self.queue_poll();\n'
        '                            if firmware.render_control_backpressured() {\n'
        '                                self.queue_poll_after(1);\n'
        '                            }\n'
        '                        } else if channel::Events::pending(&firmware.init, Role::Primary)?',
    )],
    'starve-slow-pending': [(
        'events',
        '        } else if !recover\n'
        '            && (channel::Events::pending(&firmware.init, Role::Primary)?',
        '        } else if !recover\n'
        '            && !polling\n'
        '            && (channel::Events::pending(&firmware.init, Role::Primary)?',
    )],
    'jiffy-poll': [(
        'mod',
        'const RESCAN_POLL_US: i64 = 50;',
        'const RESCAN_POLL_US: i64 = 4_000;',
    )],
    'skip-backpressure-poll': [(
        'events',
        '                            if firmware.render_control_backpressured() {\n'
        '                                self.queue_poll_after(1);\n'
        '                            }',
        '',
    )],
    'skip-sleep-cancel': [(
        'sleep',
        '        self.cancel_rescan();\n',
        '',
    )],
    'skip-shutdown-disable': [(
        'mod',
        '        self.shared.disable_rescan();\n',
        '',
    )],
}


def build_harness(mutation=None):
    files = {
        'mod': (G17 / 'mod.rs').read_text(),
        'events': (G17 / 'runtime/events.rs').read_text(),
        'sleep': (G17 / 'sleep.rs').read_text(),
        'runtime': (G17 / 'runtime.rs').read_text(),
    }
    for name, old, new in MUTATIONS.get(mutation, []):
        assert files[name].count(old) == 1, (mutation, name)
        files[name] = files[name].replace(old, new, 1)

    mod_src = files['mod']
    events_src = files['events']
    sleep_src = files['sleep']
    runtime_src = files['runtime']

    queue_poll_fn = item(mod_src, 'fn queue_poll(self: &Arc<Self>)')
    cancel_fn = item(mod_src, 'fn cancel_rescan(&self)')
    disable_fn = item(mod_src, 'fn disable_rescan(&self)')
    hrtimer_cb = item(mod_src, 'impl HrTimerCallback for Shared')
    turn_fn = item(events_src, 'fn runtime_event_turn(')
    join_fn = item(sleep_src, 'fn join_event_workers(self: &Arc<Self>)')
    stop_fn = item(runtime_src, 'pub(super) fn stop_runtime(&self)')
    shutdown_fn = item(mod_src, 'pub(crate) fn shutdown(&self)')

    fast_pending_unblocked = (
        '} else if channel::Events::pending(&firmware.init, Role::Primary)?' not in turn_fn
        and 'if channel::Events::pending(&firmware.init, Role::Primary)?' in turn_fn
    )
    slow_pending_unblocked = (
        '&& !polling\n            && (channel::Events::pending(&firmware.init, Role::Primary)?'
        not in turn_fn
        and '} else if !recover\n            && (channel::Events::pending(&firmware.init, Role::Primary)?'
        in turn_fn
    )
    backpressure_preserved = (
        turn_fn.count('if firmware.render_control_backpressured() {\n') == 2
        and turn_fn.count('self.queue_poll_after(1);') == 2
    )
    lifecycle_ok = (
        'self.cancel_rescan();' in join_fn
        and 'self.disable_rescan();' in stop_fn
        and 'self.shared.disable_rescan();' in shutdown_fn
    )

    host_queue_poll = queue_poll_fn.replace('self.rescan_handle.lock()', 'self.lock_handle()')
    host_cancel = cancel_fn.replace('self.rescan_handle.lock()', 'self.lock_handle()')

    return f'''
#![allow(dead_code, unused_variables, unused_imports)]
use std::sync::{{Arc, Mutex, MutexGuard, atomic::{{AtomicBool, AtomicU64, AtomicUsize, Ordering}}}};

static NOW_NS: AtomicU64 = AtomicU64::new(1_000_000);
fn now_ns() -> u64 {{ NOW_NS.load(Ordering::Acquire) }}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Delta(i64);
impl Delta {{
    fn from_micros(us: i64) -> Self {{ Self(us) }}
}}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HrTimerRestart {{ NoRestart, Restart }}

struct ArcBorrow<'a, T>(&'a Arc<T>);
impl<'a, T> From<ArcBorrow<'a, T>> for Arc<T> {{
    fn from(b: ArcBorrow<'a, T>) -> Self {{ Arc::clone(b.0) }}
}}
struct HrTimerCallbackContext<'a, T>(std::marker::PhantomData<&'a T>);
trait HrTimerCallback {{
    type Pointer<'a>;
    fn run(this: ArcBorrow<'_, Self>, ctx: HrTimerCallbackContext<'_, Self>) -> HrTimerRestart
    where Self: Sized;
}}

struct ArcHrTimerHandle<T> {{
    shared: Arc<T>,
    delay_us: i64,
    cancelled: Arc<AtomicUsize>,
}}
impl<T> Drop for ArcHrTimerHandle<T> {{
    fn drop(&mut self) {{
        self.cancelled.fetch_add(1, Ordering::AcqRel);
    }}
}}

struct Shared {{
    submission_enabled: bool,
    sleeping_flag: AtomicBool,
    rescan_stopped: AtomicBool,
    crashed: AtomicBool,
    rescan_deadline_ns: AtomicU64,
    rescan_handle: Mutex<Option<ArcHrTimerHandle<Shared>>>,
    lock_trips: AtomicUsize,
    starts: AtomicUsize,
    last_delay_us: AtomicU64,
    cancelled: Arc<AtomicUsize>,
    event_records: AtomicUsize,
}}

impl Shared {{
    fn new() -> Arc<Self> {{
        Arc::new(Self {{
            submission_enabled: true,
            sleeping_flag: AtomicBool::new(false),
            rescan_stopped: AtomicBool::new(false),
            crashed: AtomicBool::new(false),
            rescan_deadline_ns: AtomicU64::new(0),
            rescan_handle: Mutex::new(None),
            lock_trips: AtomicUsize::new(0),
            starts: AtomicUsize::new(0),
            last_delay_us: AtomicU64::new(0),
            cancelled: Arc::new(AtomicUsize::new(0)),
            event_records: AtomicUsize::new(0),
        }})
    }}
    fn sleeping(&self) -> bool {{ self.sleeping_flag.load(Ordering::Acquire) }}
    fn queue_event_records(self: &Arc<Self>) {{
        self.event_records.fetch_add(1, Ordering::AcqRel);
    }}
    fn lock_handle(&self) -> MutexGuard<'_, Option<ArcHrTimerHandle<Shared>>> {{
        self.lock_trips.fetch_add(1, Ordering::AcqRel);
        self.rescan_handle.lock().unwrap()
    }}
    fn start(self: Arc<Self>, delta: Delta) -> ArcHrTimerHandle<Self> {{
        self.starts.fetch_add(1, Ordering::AcqRel);
        self.last_delay_us.store(delta.0 as u64, Ordering::Release);
        let cancelled = Arc::clone(&self.cancelled);
        ArcHrTimerHandle {{ shared: self, delay_us: delta.0, cancelled }}
    }}

    {host_queue_poll}

    {host_cancel}

    {disable_fn}
}}

{hrtimer_cb}

fn main() {{
    assert!({str(fast_pending_unblocked).lower()}, "fast path starved pending events behind polling");
    assert!({str(slow_pending_unblocked).lower()}, "slow path starved pending events behind polling");
    assert!({str(backpressure_preserved).lower()}, "render_control_backpressured must still arm 1ms POLL_WORK");
    assert!({str(lifecycle_ok).lower()}, "sleep/stop/shutdown must cancel or disable rescan hrtimer");

    let s = Shared::new();
    s.queue_poll();
    assert_eq!(s.starts.load(Ordering::Acquire), 1);
    let delay = s.last_delay_us.load(Ordering::Acquire);
    assert!(delay >= 10 && delay <= 100, "rescan hrtimer delay must be sub-ms (10..=100 us), got {{delay}}");
    assert_eq!(s.lock_trips.load(Ordering::Acquire), 1);

    // Coalescing while armed must skip rescan_handle lock completely.
    NOW_NS.store(1_000_000 + delay * 500, Ordering::Release);
    s.queue_poll();
    assert_eq!(s.starts.load(Ordering::Acquire), 1);
    assert_eq!(s.lock_trips.load(Ordering::Acquire), 1);

    // Timer expiry clears deadline and queues event records without restarting.
    let restart = Shared::run(ArcBorrow(&s), HrTimerCallbackContext(std::marker::PhantomData));
    assert_eq!(restart, HrTimerRestart::NoRestart);
    assert_eq!(s.rescan_deadline_ns.load(Ordering::Acquire), 0);
    assert_eq!(s.event_records.load(Ordering::Acquire), 1);

    // Re-arm after expiry succeeds and cancels old completed handle before starting.
    s.queue_poll();
    assert_eq!(s.starts.load(Ordering::Acquire), 2);
    assert_eq!(s.cancelled.load(Ordering::Acquire), 1);

    // Sleep cancels handle and prevents callback or queue_poll from arming.
    s.sleeping_flag.store(true, Ordering::Release);
    s.cancel_rescan();
    assert_eq!(s.cancelled.load(Ordering::Acquire), 2);
    assert_eq!(s.rescan_deadline_ns.load(Ordering::Acquire), 0);
    s.queue_poll();
    assert_eq!(s.starts.load(Ordering::Acquire), 2);
    let _ = Shared::run(ArcBorrow(&s), HrTimerCallbackContext(std::marker::PhantomData));
    assert_eq!(s.event_records.load(Ordering::Acquire), 1);

    // Wake allows re-arming again.
    s.sleeping_flag.store(false, Ordering::Release);
    s.queue_poll();
    assert_eq!(s.starts.load(Ordering::Acquire), 3);

    // Shutdown disables permanently and breaks the Arc self-reference.
    s.disable_rescan();
    assert_eq!(s.cancelled.load(Ordering::Acquire), 3);
    assert_eq!(Arc::strong_count(&s), 1);
    s.queue_poll();
    assert_eq!(s.starts.load(Ordering::Acquire), 3);
    let _ = Shared::run(ArcBorrow(&s), HrTimerCallbackContext(std::marker::PhantomData));
    assert_eq!(s.event_records.load(Ordering::Acquire), 1);
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()
    src = build_harness(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-rescan-poll-') as tmp:
        rs = Path(tmp) / 'test.rs'
        exe = Path(tmp) / 'test'
        rs.write_text(src)
        subprocess.run(['rustc', '-O', str(rs), '-o', str(exe)], check=True)
        res = subprocess.run([str(exe)], capture_output=True, text=True)
        if res.returncode != 0:
            sys.stderr.write(res.stdout + res.stderr)
            sys.exit(res.returncode)


if __name__ == '__main__':
    main()
