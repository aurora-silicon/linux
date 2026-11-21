#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the real G16 QUEUE_LIMITS report, logical-queue credit and Queue::new on the host.

The VM lookup, scheduler, timeline and allocations are models. This covers the reported
bound, its enforcement across files and VMs, rollback and drop order; it claims no firmware
execution-context reservation and no DRM scheduling behaviour.
"""
import json
import re
import subprocess
from common import kernel_root, output_dir, rustc, sha

ROOT = kernel_root()
G16 = ROOT / 'drivers/gpu/drm/asahi/g16'


def block(text, marker):
    """`marker` up to the brace that closes the first brace after it."""
    start = text.index(marker)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def constant(text, name):
    match = re.search(r'pub\(crate\) const ' + name + r': ([^=]+) = ([^;]+);', text)
    return f'pub const {name}: {match[1]} = {match[2]};'


HARNESS = r'''
#![allow(dead_code, non_camel_case_types)]
use std::{cell::Cell, ops::Deref, sync::{Arc as StdArc, Barrier, Mutex,
    atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering}}};
#[derive(Copy, Clone, Debug, PartialEq, Eq)] struct Error(i32);
type Result<T = ()> = std::result::Result<T, Error>;
const EIO: Error = Error(-5); const EINVAL: Error = Error(-22); const ENOMEM: Error = Error(-12);
const GFP_KERNEL: () = ();
thread_local! { static FAIL: Cell<u32> = const { Cell::new(0) }; }
fn failure(n: u32) -> bool { FAIL.with(|f| f.get() == n) }
struct Arc<T>(StdArc<T>);
impl<T> Clone for Arc<T> { fn clone(&self) -> Self { Self(self.0.clone()) } }
impl<T> Deref for Arc<T> { type Target = T; fn deref(&self) -> &T { &self.0 } }
impl<T> Arc<T> {
    fn wrap(v: T) -> Self { Self(StdArc::new(v)) }
    fn new(v: T, _: ()) -> Result<Self> { if failure(4) { Err(ENOMEM) } else { Ok(Self::wrap(v)) } }
}
struct Trace { counter: StdArc<AtomicU32>, drains: Mutex<Vec<u32>> }
struct Device { trace: StdArc<Trace> }
struct Shared { logical_queues: StdArc<AtomicU32>, crashed: AtomicBool, dev: Device }
struct Status(AtomicI32);
impl Status { fn get(&self) -> i32 { self.0.load(Ordering::SeqCst) } }
struct VmState { status: Status }
mod mmu { use super::*; pub struct Vm { pub state: Arc<VmState> } }
mod vm {
    use super::*;
    pub fn lookup(_: &Gpu, vm: &mmu::Vm) -> Result<Arc<VmState>> {
        if failure(1) { Err(ENOMEM) } else { Ok(vm.state.clone()) }
    }
}
mod hw { use std::ops::Range; __USER_RANGE__ pub mod sizes { __SIZES__ } }
mod uapi {
    pub const drm_asahi_priority_DRM_ASAHI_PRIORITY_REALTIME: u32 = 3;
    #[derive(Default, Debug)]
    pub struct drm_asahi_queue_limits { pub max_queues: u32, pub max_in_flight_per_queue: u32, pub flags: u32, pub pad: u32 }
}
mod usage { pub struct Client; }
__USC_WINDOW__
struct Gpu { shared: Arc<Shared> }
impl Gpu { __LIMITS__ }
struct Scheduler { trace: StdArc<Trace> }
impl Scheduler {
    fn new(dev: &Device) -> Result<Self> {
        if failure(3) { Err(ENOMEM) } else { Ok(Self { trace: dev.trace.clone() }) }
    }
}
impl Drop for Scheduler {
    fn drop(&mut self) {
        let held = self.trace.counter.load(Ordering::SeqCst);
        assert!(held > 0, "scheduler must drain before credit release");
        self.trace.drains.lock().unwrap().push(held);
    }
}
struct Timeline;
impl Timeline { fn new() -> Result<Arc<Self>> { if failure(2) { Err(ENOMEM) } else { Ok(Arc::wrap(Self)) } } }
__STATE__
__CREDIT__
__QUEUE__
impl Queue { __NEW__ }

fn setup() -> Gpu {
    let counter = StdArc::new(AtomicU32::new(0));
    let trace = StdArc::new(Trace { counter: counter.clone(), drains: Mutex::new(Vec::new()) });
    Gpu { shared: Arc::wrap(Shared { logical_queues: counter, crashed: AtomicBool::new(false), dev: Device { trace } }) }
}
fn vm_with(status: i32) -> Arc<VmState> { Arc::wrap(VmState { status: Status(AtomicI32::new(status)) }) }
fn base() -> u64 { hw::USER_RANGE.start.next_multiple_of(USC_WINDOW) }
fn new_on(gpu: &Gpu, vm: &Arc<VmState>, priority: u32, base: u64) -> Result<Queue> {
    Queue::new(gpu, mmu::Vm { state: vm.clone() }, priority, base, Arc::wrap(usage::Client))
}
fn new(gpu: &Gpu, status: i32, priority: u32, base: u64) -> Result<Queue> { new_on(gpu, &vm_with(status), priority, base) }
fn count(gpu: &Gpu) -> u32 { gpu.shared.logical_queues.load(Ordering::SeqCst) }

fn main() {
    let gpu = setup();
    let limits = gpu.queue_limits().expect("G16 reports QUEUE_LIMITS (INDEPENDENT_QUEUES)");
    assert_eq!(limits.max_queues, hw::sizes::MAX_QUEUES, "reported bound must equal the enforced bound");
    assert_eq!(limits.max_in_flight_per_queue, hw::sizes::SCHEDULER_CREDITS, "in-flight limit is the per-queue scheduler credit");
    assert!(limits.max_queues > 1 && limits.max_in_flight_per_queue > 0);
    assert_eq!((limits.flags, limits.pad), (0, 0));
    let max = limits.max_queues;

    // Several queues on one VM, as one Vulkan device creates them.
    let shared_vm = vm_with(0);
    let same: Vec<_> = (0..8).map(|_| new_on(&gpu, &shared_vm, 2, base()).expect("queue on a shared VM")).collect();
    assert_eq!(count(&gpu), 8);
    drop(same);
    assert_eq!(count(&gpu), 0, "release returns every credit");

    // The bound is device-wide, across distinct VMs and files.
    let gpu = setup();
    let mut queues = Vec::new();
    for _ in 0..max { queues.push(new(&gpu, 0, 2, base()).expect("live logical queue below the bound")); }
    assert_eq!(count(&gpu), max, "reported capacity must be reserved");
    assert!(matches!(new(&gpu, 0, 2, base()), Err(ENOMEM)), "queue past the reported limit must refuse");
    assert_eq!(count(&gpu), max);
    drop(queues);
    assert_eq!(count(&gpu), 0, "release returns every credit");
    assert_eq!(*gpu.shared.dev.trace.drains.lock().unwrap(), (1..=max).rev().collect::<Vec<_>>(),
               "every scheduler drains before its credit returns");
    assert!(new(&gpu, 0, 2, base()).is_ok(), "capacity reusable after drain");
    assert_eq!(count(&gpu), 0);

    // An allocation failure after the VM lookup never leaks a credit.
    for point in 1..=4 {
        let gpu = setup();
        FAIL.with(|f| f.set(point));
        assert!(new(&gpu, 0, 2, base()).is_err());
        FAIL.with(|f| f.set(0));
        assert_eq!(count(&gpu), 0, "allocation failure must return the credit");
        let drains = gpu.shared.dev.trace.drains.lock().unwrap().clone();
        assert_eq!(drains, if point == 4 { vec![1] } else { vec![] }, "rollback drains before the credit returns");
        drop(new(&gpu, 0, 2, base()).expect("allocation failure leaves capacity usable"));
        assert_eq!(count(&gpu), 0);
    }

    // Admission refusals consume no capacity.
    let beyond = hw::USER_RANGE.end.next_multiple_of(USC_WINDOW);
    for (status, priority, base) in [(-5, 2, base()), (0, 4, base()), (0, 2, base() + 1), (0, 2, beyond)] {
        let gpu = setup();
        assert!(new(&gpu, status, priority, base).is_err());
        assert_eq!(count(&gpu), 0);
    }
    let gpu = setup();
    gpu.shared.crashed.store(true, Ordering::SeqCst);
    assert!(matches!(new(&gpu, 0, 2, base()), Err(EIO)));
    assert_eq!(count(&gpu), 0);

    // Contention: 16 threads, 8 acquisitions each, all held until every thread has tried.
    let gpu = setup();
    let ready = StdArc::new(Barrier::new(17));
    let release = StdArc::new(Barrier::new(17));
    let won = StdArc::new(AtomicU32::new(0));
    let threads: Vec<_> = (0..16).map(|_| {
        let (shared, ready, release, won) = (gpu.shared.clone(), ready.clone(), release.clone(), won.clone());
        std::thread::spawn(move || {
            let held: Vec<_> = (0..8).filter_map(|_| QueueCredit::acquire(&shared).ok()).collect();
            won.fetch_add(held.len() as u32, Ordering::SeqCst);
            ready.wait();
            release.wait();
            drop(held);
        })
    }).collect();
    ready.wait();
    assert_eq!(won.load(Ordering::SeqCst), max, "concurrent reservations respect the bound");
    assert_eq!(count(&gpu), max, "concurrent reservations respect the bound");
    release.wait();
    for thread in threads { thread.join().unwrap(); }
    assert_eq!(count(&gpu), 0, "release returns every credit");

    // A saturated counter refuses instead of wrapping.
    gpu.shared.logical_queues.store(u32::MAX, Ordering::SeqCst);
    assert!(matches!(QueueCredit::acquire(&gpu.shared), Err(ENOMEM)));
    assert_eq!(count(&gpu), u32::MAX);
    println!("PASS QUEUE_LIMITS {}/{}, {} queues then ENOMEM, 8 on one VM, 4 rollbacks, refusals, 16-thread contention, saturation",
             limits.max_queues, limits.max_in_flight_per_queue, max);
}
'''


def main():
    out = output_dir()
    queue = (G16 / 'queue.rs').read_text()
    gpu = (G16 / 'mod.rs').read_text()
    hw = (G16 / 'hw.rs').read_text()
    command = (G16 / 'command.rs').read_text()
    job = (G16 / 'job.rs').read_text()
    scheduler = block(job, 'pub(crate) fn new(dev: &device::Device)')
    if 'hw::sizes::SCHEDULER_CREDITS' not in scheduler:
        raise RuntimeError('Scheduler::new no longer takes SCHEDULER_CREDITS; update the reported in-flight limit')
    credit = queue[queue.index('/// One device-wide logical-queue credit'):queue.index('/// A G16 logical queue.')]
    queue_impl = queue[queue.index('impl Queue {'):]
    replacements = {
        '__USER_RANGE__': constant(hw, 'USER_RANGE'),
        '__SIZES__': constant(hw, 'MAX_QUEUES') + ' ' + constant(hw, 'SCHEDULER_CREDITS'),
        '__USC_WINDOW__': constant(command, 'USC_WINDOW'),
        '__LIMITS__': block(gpu, 'fn queue_limits(&self)'),
        '__STATE__': block(queue, 'pub(crate) struct QueueState {'),
        '__CREDIT__': credit,
        '__QUEUE__': block(queue, 'pub(crate) struct Queue {'),
        '__NEW__': block(queue_impl, 'pub(crate) fn new('),
    }
    code = HARNESS
    for old, new in replacements.items():
        code = code.replace(old, new)
    # Each mutation must be caught at run time by the assertion named.
    mutations = {
        'bound-not-enforced': ('count < hw::sizes::MAX_QUEUES', 'count < u32::MAX',
                               'queue past the reported limit must refuse'),
        'reported-bound-differs': ('max_queues: hw::sizes::MAX_QUEUES,', 'max_queues: hw::sizes::MAX_QUEUES + 1,',
                                   'reported bound must equal the enforced bound'),
        'credit-never-returned': ('self.shared.logical_queues.fetch_sub(1, Ordering::AcqRel)',
                                  'self.shared.logical_queues.load(Ordering::Acquire) + 1',
                                  'release returns every credit'),
        'credit-before-scheduler': ('pub(crate) struct Queue {\n', 'pub(crate) struct Queue {\n    _credit: QueueCredit,\n',
                                    'scheduler must drain before credit release'),
    }
    variants = {'control': (code, None)}
    for name, (old, new, message) in mutations.items():
        if code.count(old) != 1:
            raise RuntimeError(f'{name}: mutation anchor found {code.count(old)} times')
        variant = code.replace(old, new)
        if name == 'credit-before-scheduler':
            tail = '    _credit: QueueCredit,\n}'
            index = variant.rindex(tail)
            variant = variant[:index] + '}' + variant[index + len(tail):]
        variants[name] = (variant, message)
    report = {'scope': __doc__.strip(), 'sources': {name: sha(G16 / name) for name in
              ['queue.rs', 'mod.rs', 'hw.rs', 'command.rs', 'job.rs']}, 'variants': {}}
    failed = []
    for name, (source, message) in variants.items():
        path = out / f'{name}.rs'
        path.write_text(source)
        binary = out / name
        built = subprocess.run(rustc() + ['--edition=2021', '-o', str(binary), str(path)],
                               capture_output=True, text=True)
        if built.returncode:
            failed.append(f'{name}: does not build: {built.stderr}')
            continue
        ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        caught = message is not None and ran.returncode != 0 and message in ran.stderr
        report['variants'][name] = {'exit': ran.returncode, 'stdout': ran.stdout.strip(),
                                    'expected_failure': message, 'caught': caught}
        if message is None and ran.returncode:
            failed.append(f'{name}: {ran.stdout}{ran.stderr}')
        if message is not None and not caught:
            failed.append(f'{name}: mutation not caught: exit {ran.returncode} {ran.stderr}')
    report['passed'] = not failed
    report['errors'] = failed
    (out / 'queue-credit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
