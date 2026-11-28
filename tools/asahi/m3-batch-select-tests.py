#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production M3 batch selection (`next_batch`) on queues of host packets.

The extracted Runtime method and the production batch policy choose the commands of one firmware
batch. The packets, parameters and measured costs are host models: this covers which commands
share a batch, not their encoding or retirement.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
M3 = ROOT / 'drivers/gpu/drm/asahi/m3'


def method(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREFIX = r'''
#![allow(dead_code)]
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
type Result<T = ()> = std::result::Result<T, i32>;
#[path = "POLICY"]
mod policy;
struct Flags;
const GFP_KERNEL: Flags = Flags;
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn new() -> Self { Self(Vec::new()) }
    fn push(&mut self, value: T, _flags: Flags) -> Result { self.0.push(value); Ok(()) }
}
impl<T> std::ops::Deref for KVec<T> {
    type Target = [T];
    fn deref(&self) -> &[T] { &self.0 }
}
static COMPUTE_BATCH: AtomicUsize = AtomicUsize::new(16);
static RENDER_BATCH: AtomicUsize = AtomicUsize::new(1);
static OVERRIDDEN: AtomicBool = AtomicBool::new(false);
mod m3 {
    pub mod m3_compute_storage { pub const SLOTS: usize = 16; }
    pub mod m3_submit {
        #[derive(Clone, Copy)]
        pub enum Command { Render { command: u32, usc: u32 }, Compute(u32) }
        pub struct Vm(pub u64);
        impl Vm {
            pub fn id(&self) -> u64 { self.0 }
            pub fn same(&self, other: &Vm) -> bool { self.0 == other.0 }
        }
        pub struct Packet { pub vm: Vm, pub urgency: u8, pub commands: Vec<Command> }
    }
    pub mod m3_params {
        use std::sync::atomic::Ordering;
        pub fn mixed_batch() -> bool { false }
        pub fn render_batch_size() -> usize { crate::RENDER_BATCH.load(Ordering::Relaxed) }
        pub fn compute_batch_size() -> usize { crate::COMPUTE_BATCH.load(Ordering::Relaxed) }
        pub fn compute_batch_overridden() -> bool { crate::OVERRIDDEN.load(Ordering::Relaxed) }
        pub fn render_batch_budget_ns(soc: &crate::Soc) -> Option<u64> { soc.budget }
    }
}
use m3::m3_submit::{Command, Packet, Vm};
struct Soc { chip_id: u32, budget: Option<u64> }
struct Device(Soc);
impl Device { fn soc(&self) -> &Soc { &self.0 } }
struct Job(bool);
impl Job { fn started(&self) -> bool { self.0 } }
enum NativeJob { Render(Job), Compute(Job) }
struct Batch { entries: Vec<(Arc<Packet>, usize)> }
struct Inner {
    pending: Vec<(Arc<Packet>, usize)>, active: Vec<Batch>, jobs: Vec<NativeJob>, device: Device,
    render_initialized: bool, solo_next: bool,
    pass_cost: policy::PassCosts, compute_cost: policy::PassCosts,
}
impl Inner {
'''
TESTS = r'''
}
const BUDGET: Option<u64> = Some(policy::default_budget_us(true) * 1000);
const SLOTS: usize = m3::m3_compute_storage::SLOTS;
fn compute(vm: u64, urgency: u8, commands: usize) -> (Arc<Packet>, usize) {
    (Arc::new(Packet { vm: Vm(vm), urgency, commands: vec![Command::Compute(0); commands] }), 0)
}
fn runtime(pending: Vec<(Arc<Packet>, usize)>, budget: Option<u64>) -> Inner {
    Inner { pending, active: Vec::new(), jobs: vec![NativeJob::Render(Job(true))],
        device: Device(Soc { chip_id: 0x8122, budget }), render_initialized: true, solo_next: false,
        pass_cost: policy::PassCosts::default(), compute_cost: policy::PassCosts::default() }
}
/// The batch as (pending position, command index) pairs.
fn batch(inner: &Inner) -> Vec<(usize, usize)> {
    inner.next_batch(SLOTS).unwrap().iter().map(|(packet, index)| {
        (inner.pending.iter().position(|(p, _)| Arc::ptr_eq(p, packet)).unwrap(), *index)
    }).collect()
}
fn count(inner: &Inner) -> usize { batch(inner).len() }
fn queued(packets: usize, commands: usize) -> Vec<(Arc<Packet>, usize)> {
    (0..packets).map(|_| compute(1, 0, commands)).collect()
}
fn main() {
    // No measured compute cost for the VM: one command per batch, whether the commands come from
    // queued packets or from one packet.
    assert_eq!(count(&runtime(queued(8, 1), BUDGET)), 1);
    assert_eq!(count(&runtime(queued(1, SLOTS), BUDGET)), 1);
    // A measurement of another VM, or of the VM's render passes, doesn't count.
    let mut inner = runtime(queued(8, 1), BUDGET);
    inner.compute_cost.record(2, 182);
    inner.pass_cost.record(1, 182);
    assert_eq!(count(&inner), 1);
    // A light dispatch (182 ns): every queued packet joins, up to the storage's slots.
    inner.compute_cost.record(1, 182);
    assert_eq!(batch(&inner), (0..8).map(|p| (p, 0)).collect::<Vec<_>>());
    let mut inner = runtime(queued(3, SLOTS), BUDGET);
    inner.compute_cost.record(1, 182);
    assert_eq!(batch(&inner), (0..SLOTS).map(|i| (0, i)).collect::<Vec<_>>());
    // The measured cost caps the count: cost times commands stays within the 4 ms budget.
    for (ns, commands) in [(250_000, 16), (500_000, 8), (1_000_000, 4), (1_500_000, 2),
        (2_000_000, 2), (2_000_001, 1), (4_000_000, 1), (200_000_000, 1)] {
        for pending in [queued(SLOTS, 1), queued(1, SLOTS), queued(6, 3)] {
            let mut inner = runtime(pending, BUDGET);
            inner.compute_cost.record(1, ns);
            assert_eq!(count(&inner), commands, "{} ns", ns);
        }
    }
    // A batch cut inside a joined packet leaves that packet's remaining commands for later.
    let mut inner = runtime(queued(6, 3), BUDGET);
    inner.compute_cost.record(1, 1_000_000);
    assert_eq!(batch(&inner), [(0, 0), (0, 1), (0, 2), (1, 0)]);
    // Without a budget every compute batch is one command, measured or not.
    let mut inner = runtime(queued(8, 1), None);
    inner.compute_cost.record(1, 182);
    assert_eq!(count(&inner), 1);
    // The batch size parameter still bounds a budgeted batch.
    COMPUTE_BATCH.store(3, Ordering::Relaxed);
    let mut inner = runtime(queued(8, 1), BUDGET);
    inner.compute_cost.record(1, 182);
    assert_eq!(count(&inner), 3);
    COMPUTE_BATCH.store(SLOTS, Ordering::Relaxed);
    // Only packets of the batch's VM and urgency join, in queue order, from their first command.
    for (other, joins) in [(compute(1, 0, 1), true), (compute(2, 0, 1), false),
        (compute(1, 1, 1), false), ((compute(1, 0, 2).0, 1), false)] {
        let mut inner = runtime(vec![compute(1, 0, 1), other, compute(1, 0, 1)], BUDGET);
        inner.compute_cost.record(1, 182);
        assert_eq!(count(&inner), if joins { 3 } else { 1 });
    }
    // A packet that goes on with a render pass ends the batch after its compute commands.
    let mixed = Arc::new(Packet { vm: Vm(1), urgency: 0,
        commands: vec![Command::Compute(0), Command::Render { command: 0, usc: 0 }] });
    let mut inner = runtime(vec![compute(1, 0, 1), (mixed, 0), compute(1, 0, 1)], BUDGET);
    inner.compute_cost.record(1, 182);
    assert_eq!(batch(&inner), [(0, 0), (1, 0)]);
    // After a batch with joined packets failed before publication the front packet goes alone.
    let mut inner = runtime(queued(4, 2), BUDGET);
    inner.compute_cost.record(1, 182);
    assert_eq!(count(&inner), 8);
    inner.solo_next = true;
    assert_eq!(batch(&inner), [(0, 0), (0, 1)]);
    // An explicit override bounds the front packet's own commands without the budget; joined
    // packets still need it.
    OVERRIDDEN.store(true, Ordering::Relaxed);
    let mut inner = runtime(queued(2, 4), BUDGET);
    assert_eq!(count(&inner), 4);
    inner.compute_cost.record(1, 1_000_000);
    assert_eq!(count(&inner), 4);
    let mut inner = runtime(queued(2, 4), BUDGET);
    inner.compute_cost.record(1, 182);
    assert_eq!(count(&inner), 8);
    assert_eq!(count(&runtime(queued(2, 4), None)), 4);
    OVERRIDDEN.store(false, Ordering::Relaxed);
    // A stale cost: a VM measured at 100 us per command queues 200 ms dispatches. The estimate
    // still admits a full batch. Its deadline is the sum of the deadlines the commands had
    // alone, so the batch (3.2 s) is in time; its retirement then corrects the estimate.
    let mut inner = runtime(queued(1, SLOTS), BUDGET);
    inner.compute_cost.record(1, 100_000);
    let commands = count(&inner);
    assert_eq!(commands, SLOTS);
    let alone = policy::batch_deadline_secs(1);
    assert_eq!(policy::batch_deadline_secs(commands), alone * commands as i64);
    assert!(commands as i64 * 200 < policy::batch_deadline_secs(commands) * 1000);
    inner.compute_cost.record(1, 200_000_000);
    assert_eq!(count(&inner), 1);
    println!("PASS: production compute batch selection under the GPU-time budget");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=['unbudgeted'])
    args = parser.parse_args()
    runtime = (M3 / 'm3_runtime.rs').read_text()
    body = method(runtime, 'fn next_batch(')
    if args.mutation:
        gate = body[body.index('if !policy::admits('):]
        gate = gate[:gate.index('}') + 1]
        assert 'return Ok(entries);' in gate
        body = body.replace(gate, '')
    out = args.out or Path(tempfile.mkdtemp(prefix='m3-batch-select-'))
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'select.rs'
    source.write_text(PREFIX.replace('POLICY', str(M3 / 'm3_batch_policy.rs')) + body + TESTS)
    binary = out / 'select'
    subprocess.run(['rustc', '--edition=2021', str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)])
    if result.returncode:
        # Preserve the Rust assertion's exit and diagnostic. A Python traceback
        # means a broken harness to run-host-tests, not a killed mutation.
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
