#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the G15 PB growth answer and its on-demand backing, actual bodies, against a model.

The harness extracts from drivers/gpu/drm/asahi/g15/pools.rs Firmware::answer_growth(),
answer_request(), map_pb(), running_vm() and back_pb(), Pools::soft_max() and backed(),
PbChunk and Pools::new's PB backing statements; pb_max_blocks() from params.rs; the PB sizes,
tuning and PB leaf word from hw.rs; Prot::from_pte() and is_cached_noncoherent() from
pgtable.rs; and the parameter's default from asahi_core.rs. It includes pools/growth.rs,
fw/buffer.rs and fw/channels.rs whole. The bodies run against a model of the firmware objects,
the GEM backing allocation, the shared layer's map_at() and append_driver_mappings(), and the
DEVCTL wait, which logs every allocation, alias map, append, unmap, free, block record,
barrier, producer store, reply and log line in order. Allocation, alias map and append
failures can be injected. Source checks: publish_prefix_into() starts with the PB catch-up,
and pools.rs asserts the PB leaf GPU-only at compile time.

What it shows:
- a poll pass answers only the requests queued when it starts, in order; one the reply's wait
  queues goes to the next pass. A request that cannot be processed does not end the pass;
- from the driver's initial size to its maximum, every block record is written once at its
  offset, each producer store follows its records and a barrier, each reply follows its producer
  store and a barrier, and every producer is in 1..=4095, ending at 4095;
- at the maximum two refusals are answered; further requests get no reply, count as bad and
  warn once until the ledger is made again, as init() does;
- the soft maximum is the parameter clamped to 64..=4095 and then to the ledger's
  initial..=max; growth answers above it are logged at info level, and the one that reaches it
  exactly is not;
- invalid requests (type, slot, stale counter, epoch, no live render) get no reply, and requests
  rejected so allocate nothing;
- the backing: the initial blocks only when the GPU starts; chunks of at most 4 MiB, allocated
  and held by the running VM before any record or producer names their blocks, never past the
  maximum, at most growth::max_chunks(); a failed allocation, alias map or append posts and
  answers nothing, leaves the request counter and the VM's count alone and drops what is not
  held, and the next request is answered; a failed reply keeps everything;
- a VM's count always equals the aliases it holds, contiguous from the heap's start; a VM that
  runs after another grew the backing maps it first (at publication, and again before it grows);
  a partial catch-up counts only held aliases;
- the one-VM rule: a catch-up with a flight of another VM, or a count past the chunks, and a
  grant with flights of two VMs, are refused (EPROTO) before anything is mapped or posted;
- the ledger reset of Pools::init() re-posts from the backing without allocating;
- the PB leaf decodes as GPU-only, not cached-noncoherent: dropping an alias is a plain unmap;
- Ledger::new() is Ledger::new() as of dba2a110de00 (the carried upstream constructor), capped at
  4095, over every growth and fixed input in 0..=4200 and at the u32 extremes. The driver's
  values are unchanged.

Not shown: kernel, firmware or GPU execution, timing, or what the firmware does with a reply, a
refusal or an unanswered request. The model's VMs have no open/closed state: that a closed VM
whose jobs still run keeps its aliases comes from the VM lifetime source (Job and VmJobGuard
hold the VM), not from these tests. The ledger reset is not a recovery. The shared layer's
mapping and its rollback are modelled, not run. Running only running_vm()'s or only map_pb()'s
one-VM check on a grant is not distinguishable: each covers it.

With --mutations, each mutation below must fail its intended test with the expected message, and
the control (a driver maximum of the whole 4096-block heap) must pass, held by the ledger's cap.
A run that hangs (60 s) counts as no test failed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
G15 = "drivers/gpu/drm/asahi/g15"
POOLS = f"{G15}/pools.rs"
GROWTH = f"{G15}/pools/growth.rs"
BUFFER = f"{G15}/fw/buffer.rs"
CHANNELS = f"{G15}/fw/channels.rs"
INITDATA = f"{G15}/fw/initdata.rs"
PARAMS = f"{G15}/params.rs"
HW = f"{G15}/hw.rs"
CORE = "drivers/gpu/drm/asahi/asahi_core.rs"
ENGINE = f"{G15}/engine.rs"
PGTABLE = "drivers/gpu/drm/asahi/pgtable.rs"
OLD_REF = "dba2a110de00743eeecd41edf23e54ede4491d5e"

# Ledger::new() at OLD_REF, the tip of the four PR363 carries (upstream 027bd7623f37), verbatim:
# the constructor before the cap below K.
OLD_LEDGER_NEW = '''    pub(crate) const fn new(grow: bool, blocks: u32, max_blocks: u32) -> Self {
        let (initial, max) = if grow {
            let nb = clamp(blocks, MIN_BLOCKS, MAX_BLOCKS);
            (nb, clamp(max_blocks, nb, MAX_BLOCKS))
        } else {
            (MAX_BLOCKS, MAX_BLOCKS)
        };
        Self {
            initial,
            max,
            posted: initial,
            next_request: 0,
            refusal_logged: false,
            refusals: 0,
        }
    }'''

# Pools::new's PB backing, run verbatim by the model.
POOLS_NEW_BACKING = ('        // Backing for the initial blocks; growth adds the rest.\n', '        chunks.push(')
# The compile-time proof that a PB alias drop is a plain unmap.
PB_LEAF_ASSERT = 'static_assert!(!mmu::Prot::from_pte(pte::LEAF_CACHED).is_cached_noncoherent());'

# The two statements of Pools::init() that start a new ledger.
LEDGER_RESET = ['self.ledger = Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);',
                'self.exhausted_logged = false;']

MUTATIONS = {
    # Answer until the queue is empty, including requests queued by the replies' waits.
    'drain-until-empty': (POOLS, 'for _ in 0..queued {', 'while !self.events.grow.is_empty() {'),
    # Leave the last request queued before the pass for later.
    'skip-last-queued': (POOLS, 'for _ in 0..queued {', 'for _ in 0..queued.saturating_sub(1) {'),
    # End the pass at a request that cannot be processed.
    'return-after-request-error': (POOLS, '                self.pools.stats.bad += 1;\n                dev_err!(',
                                   '                self.pools.stats.bad += 1;\n                return Ok(());\n'
                                   '                dev_err!('),
    # Answer three refusals at the maximum.
    'three-refusals': (GROWTH, 'pub(crate) const MAX_REFUSALS: u32 = 2;', 'pub(crate) const MAX_REFUSALS: u32 = 3;'),
    # Answer refusals for ever.
    'never-exhausted': (GROWTH, 'self.refusals >= MAX_REFUSALS', 'false && self.refusals >= MAX_REFUSALS'),
    # Warn for every unanswered request.
    'warn-every-time': (POOLS, 'if !self.pools.exhausted_logged {', 'if true {'),
    # Store the producer without the barrier after the block records.
    'no-record-barrier': (POOLS, '            // Records before their producer.\n            crate::mem::sync();\n',
                          '            // Records before their producer.\n'),
    # Reply without the barrier after the producer store.
    'no-reply-barrier': (POOLS, '            // Producer before the reply.\n            crate::mem::sync();\n',
                         '            // Producer before the reply.\n'),
    # Log the answer that reaches the soft maximum exactly.
    'soft-boundary-inclusive': (POOLS, 'let past_soft = n != 0 && ledger.posted > soft;',
                                'let past_soft = n != 0 && ledger.posted >= soft;'),
    # Give the firmware the parameter without the ledger's bounds.
    'soft-max-unclamped': (POOLS, 'super::params::pb_max_blocks().clamp(self.ledger.initial, self.ledger.max)',
                           'super::params::pb_max_blocks()'),
    # Ledger::new() without the cap below K.
    'uncapped-ledger': (GROWTH, 'let nb = clamp(blocks, MIN_BLOCKS, MAX_POSTED);\n'
                                '            (nb, clamp(max_blocks, nb, MAX_POSTED))\n'
                                '        } else {\n            (MAX_POSTED, MAX_POSTED)',
                        'let nb = clamp(blocks, MIN_BLOCKS, MAX_BLOCKS);\n'
                        '            (nb, clamp(max_blocks, nb, MAX_BLOCKS))\n'
                        '        } else {\n            (MAX_BLOCKS, MAX_BLOCKS)'),
    # Post blocks without backing them first.
    'grant-without-backing': (POOLS, '        self.back_pb(end, ctx)?;\n        let (first, n) = self.pools.ledger.accept(request);',
                              '        let (first, n) = self.pools.ledger.accept(request);'),
    # Back the blocks only after the request is accepted.
    'back-after-accept': (POOLS, '        self.back_pb(end, ctx)?;\n        let (first, n) = self.pools.ledger.accept(request);',
                          '        let (first, n) = self.pools.ledger.accept(request);\n        self.back_pb(end, ctx)?;'),
    # Count a catch-up alias before the VM holds it.
    'catch-up-cursor-before-append': (POOLS, '            vm.vm.append_driver_mappings(aliases)?;\n            mapped += 1;\n'
                                             '            vm.pb_chunks.store(mapped, Ordering::Relaxed);',
                                      '            mapped += 1;\n            vm.pb_chunks.store(mapped, Ordering::Relaxed);\n'
                                      '            vm.vm.append_driver_mappings(aliases)?;'),
    # Count a growth chunk's alias before the VM holds it.
    'growth-cursor-before-append': (POOLS, '            vm.vm.append_driver_mappings(aliases)?;\n            // Within the capacity checked above.',
                                    '            vm.pb_chunks.store(self.pools.chunks.len() + 1, Ordering::Relaxed);\n'
                                    '            vm.vm.append_driver_mappings(aliases)?;\n            // Within the capacity checked above.'),
    # Keep a chunk whose alias the VM never held.
    'keep-unappended-chunk': (POOLS, '            vm.vm.append_driver_mappings(aliases)?;\n            // Within the capacity checked above.\n'
                                     '            self.pools\n                .chunks\n                .push_within_capacity(chunk)\n'
                                     '                .map_err(|_| EIO)?;',
                              '            // Within the capacity checked above.\n            self.pools\n                .chunks\n'
                              '                .push_within_capacity(chunk)\n                .map_err(|_| EIO)?;\n'
                              '            vm.vm.append_driver_mappings(aliases)?;'),
    # Grow without catching the running VM up first.
    'no-growth-catch-up': (POOLS, '        let vm = self.running_vm(ctx)?;\n        self.map_pb(&vm)?;\n',
                           '        let vm = self.running_vm(ctx)?;\n'),
    # Catch up without the one-VM check.
    'no-activation-guard': (POOLS, '.any(|f| !core::ptr::eq(&*f.job.vm, vm))', '.any(|_| false)'),
    # Neither the catch-up's nor the grant's one-VM check (each alone covers a grant).
    'no-same-vm-checks': [(POOLS, '.any(|f| !core::ptr::eq(&*f.job.vm, vm))', '.any(|_| false)'),
                          (POOLS, 'if vm.slot != ctx || flights.iter().any(|f| !Arc::ptr_eq(&f.job.vm, &vm)) {',
                           'if vm.slot != ctx {')],
    # Back the whole heap when the GPU starts.
    'initial-backs-whole-heap': (POOLS, 'chunks.push(PbChunk::new(dev, 0, ledger.initial)?, GFP_KERNEL)?;',
                                 'chunks.push(PbChunk::new(dev, 0, buffer::MAX_BLOCKS)?, GFP_KERNEL)?;'),
    # Growth chunks of 8 MiB.
    'chunks-above-4mib': (GROWTH, 'pub(crate) const CHUNK_BLOCKS: u32 = 32;', 'pub(crate) const CHUNK_BLOCKS: u32 = 64;'),
    # A firmware-visible cached PB leaf (its drop would ask the firmware to flush).
    'pb-leaf-fw-visible': (HW, 'pub(crate) const LEAF_CACHED: u64 = 0x00c0000000000c83;',
                           'pub(crate) const LEAF_CACHED: u64 = 0x00c0000000000c03;'),
}

# The test each mutation must fail, and a piece of its failure message.
INTENDED = {
    'drain-until-empty': ('queued_snapshot_keeps_order', 'unbounded answering in one poll pass'),
    'skip-last-queued': ('queued_snapshot_keeps_order', 'replies in the first pass'),
    'return-after-request-error': ('unprocessed_request_does_not_end_the_pass', 'requests taken in the pass'),
    'three-refusals': ('default_growth_to_exhaustion', 'want Some(Log("warn"'),
    'never-exhausted': ('default_growth_to_exhaustion', 'growth did not end'),
    'warn-every-time': ('default_growth_to_exhaustion', 'later request after exhaustion'),
    'no-record-barrier': ('default_growth_to_exhaustion', 'got Some(Producer(1034)), want Some(Sync)'),
    'no-reply-barrier': ('default_growth_to_exhaustion',
                         'got Some(Reply([7, 1, 0, 5, 0, 256, 3, 1, 0, 0, 0, 0, 0, 0])), want Some(Sync)'),
    'soft-boundary-inclusive': ('soft_max_boundaries', 'got Some(Log("info", "")), want Some(Log("dbg", ""))'),
    'soft-max-unclamped': ('soft_max_bounds', 'soft_max, param 0'),
    'uncapped-ledger': ('constructor_is_the_old_one_capped', 'new(true, '),
    'grant-without-backing': ('growth_backs_before_posting', 'record of block'),
    'back-after-accept': ('allocation_failure_posts_nothing', 'ledger after a backing failure'),
    'catch-up-cursor-before-append': ('partial_catch_up_counts_only_held_aliases', 'count after a failed append'),
    'growth-cursor-before-append': ('append_failure_posts_nothing', 'count differs from held aliases'),
    'keep-unappended-chunk': ('append_failure_posts_nothing', 'chunks after a backing failure'),
    'no-growth-catch-up': ('growth_catches_the_running_vm_up_first', 'running VM not caught up'),
    'no-activation-guard': ('catch_up_refuses_flights_of_another_vm', 'catch-up with a flight of another VM'),
    'no-same-vm-checks': ('grant_refuses_flights_of_two_vms', 'grant with flights of two VMs'),
    'initial-backs-whole-heap': ('startup_backs_the_initial_blocks', 'startup allocation'),
    'chunks-above-4mib': ('growth_backs_before_posting', 'a growth chunk above 4 MiB'),
    'pb-leaf-fw-visible': ('pb_leaf_is_gpu_only', 'PB leaf not GPU-only'),
}
assert set(INTENDED) == set(MUTATIONS)

# Changes that must pass: the ledger's cap holds a driver maximum of the whole heap.
CONTROLS = {
    'tuning-whole-heap': (HW, 'pub(crate) const PB_MAX_BLOCKS: u32 = super::sizes::PB_BLOCKS - 1;',
                          'pub(crate) const PB_MAX_BLOCKS: u32 = super::sizes::PB_BLOCKS;'),
}


def item(text, start):
    a = text.index(start)
    b = text.index("{", a) + 1
    depth = 1
    while depth:
        depth += (text[b] == "{") - (text[b] == "}")
        b += 1
    return text[a:b]


def consts(text, names):
    out = []
    for name in names:
        m = re.search(rf'pub\(crate\) const {name}: (\w+) = ([^;]+);', text)
        if not m:
            raise SystemExit(f'{name} not found')
        out.append(f'pub const {name}: {m.group(1)} = {m.group(2)};')
    return '\n'.join(out)


PREFIX = r'''
#![allow(dead_code, unused_imports, unused_macros, unused_variables, unreachable_code, non_upper_case_globals)]
extern crate self as kernel;
use std::cell::{Cell, RefCell};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(i32);
impl Error {
    pub fn to_errno(&self) -> i32 {
        self.0
    }
}
pub type Result<T = ()> = core::result::Result<T, Error>;
pub const EINVAL: Error = Error(-22);
pub const ENODEV: Error = Error(-19);
pub const EIO: Error = Error(-5);
pub const ENOMEM: Error = Error(-12);
pub const ENOSPC: Error = Error(-28);
pub const EBUSY: Error = Error(-16);
pub const EPROTO: Error = Error(-71);
pub mod prelude {
    pub use crate::{static_assert, Error, Result, EBUSY, EINVAL, EIO, ENODEV, ENOMEM, ENOSPC, KVec, GFP_KERNEL};
}
pub type ARef<T> = std::sync::Arc<T>;
#[derive(Copy, Clone)]
pub struct Flags;
pub const GFP_KERNEL: Flags = Flags;
/// A kernel vector: pushes past the reserved capacity fail only through push_within_capacity.
pub struct KVec<T>(pub Vec<T>);
impl<T> KVec<T> {
    pub fn with_capacity(n: usize, _: Flags) -> Result<Self> {
        Ok(KVec(Vec::with_capacity(n)))
    }
    pub fn push(&mut self, value: T, _: Flags) -> Result {
        self.0.push(value);
        Ok(())
    }
    pub fn push_within_capacity(&mut self, value: T) -> core::result::Result<(), T> {
        if self.0.len() == self.0.capacity() {
            return Err(value);
        }
        self.0.push(value);
        Ok(())
    }
    pub fn capacity(&self) -> usize {
        self.0.capacity()
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn get(&self, i: usize) -> Option<&T> {
        self.0.get(i)
    }
    pub fn last(&self) -> Option<&T> {
        self.0.last()
    }
}
#[macro_export]
macro_rules! static_assert {
    ($e:expr) => {
        const _: () = assert!($e);
    };
}

/// One firmware-visible access, barrier or log line, in program order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Ev {
    Record(usize, u64),
    Sync,
    Producer(u32),
    Reply([u32; 14]),
    Log(&'static str, String),
    /// Backing object `id` of this many bytes allocated, or freed (its last reference gone).
    Alloc(u32, usize),
    Free(u32),
    /// A PB alias of object `id` at GVA in the VM with this context, made, held by the VM
    /// (appended to its driver mappings), or unmapped.
    Map(u32, u64, usize, u32),
    Append(u32, u64),
    Unmap(u32, u64),
}
thread_local! {
    pub static LOG: RefCell<Vec<Ev>> = RefCell::new(Vec::new());
    pub static PARAM: Cell<u32> = Cell::new(PARAM_DEFAULT);
    /// Failures to inject: the next this many allocations, alias maps, appends fail.
    pub static FAIL_ALLOC: Cell<u32> = Cell::new(0);
    pub static FAIL_MAP: Cell<u32> = Cell::new(0);
    pub static MAP_OK_BEFORE_FAIL: Cell<u32> = Cell::new(0);
    pub static FAIL_APPEND: Cell<u32> = Cell::new(0);
    pub static NEXT_ID: Cell<u32> = Cell::new(1);
}
/// Takes one injected failure of `which`.
pub fn fail(which: &'static std::thread::LocalKey<Cell<u32>>) -> bool {
    which.with(|c| {
        let n = c.get();
        if n != 0 {
            c.set(n - 1);
        }
        n != 0
    })
}
pub fn ev(e: Ev) {
    LOG.with(|l| l.borrow_mut().push(e));
}
pub fn take() -> Vec<Ev> {
    LOG.with(|l| std::mem::take(&mut *l.borrow_mut()))
}
macro_rules! dev_log {
    ($lvl:expr, $dev:expr, $($arg:tt)*) => {{
        let _ = $dev;
        crate::ev(crate::Ev::Log($lvl, format!($($arg)*)));
    }};
}
macro_rules! dev_err { ($($t:tt)*) => { dev_log!("err", $($t)*) }; }
macro_rules! dev_warn { ($($t:tt)*) => { dev_log!("warn", $($t)*) }; }
macro_rules! dev_info { ($($t:tt)*) => { dev_log!("info", $($t)*) }; }
macro_rules! dev_dbg { ($($t:tt)*) => { dev_log!("dbg", $($t)*) }; }

pub mod mem {
    pub fn sync() {
        crate::ev(crate::Ev::Sync);
    }
}

/// A boot parameter: read-only after load, read by value as on Linux 7.3.
pub mod module_parameters {
    pub struct Param;
    impl Param {
        pub fn value(&self) -> u32 {
            crate::PARAM.with(|p| p.get())
        }
    }
    pub static g15_pb_max_blocks: Param = Param;
}

pub mod driver {
    pub struct AsahiDevice;
}

/// GEM kernel objects: freed when the last reference (chunk or alias) goes.
pub mod gem {
    pub struct Object {
        pub id: u32,
        pub size: usize,
    }
    impl Drop for Object {
        fn drop(&mut self) {
            crate::ev(crate::Ev::Free(self.id));
        }
    }
    pub struct ObjectRef {
        pub gem: crate::ARef<Object>,
    }
}

/// The shared layer's VM: kernel aliases at fixed addresses, held by the VM once appended.
pub mod mmu {
    use crate::prelude::*;
    pub struct Prot(pub u64);
    impl Prot {
        pub const fn from_pte(pte: u64) -> Self {
            Prot(pte)
        }
    }
    pub struct KernelMapping {
        pub slot: u32,
        pub gva: u64,
        pub size: usize,
        pub prot: u64,
        pub gem: crate::ARef<crate::gem::Object>,
    }
    impl Drop for KernelMapping {
        fn drop(&mut self) {
            crate::ev(crate::Ev::Unmap(self.slot, self.gva));
        }
    }
    pub struct Vm {
        pub slot: u32,
        pub held: std::cell::RefCell<Vec<KernelMapping>>,
    }
    impl Vm {
        pub fn new(slot: u32) -> Self {
            Vm { slot, held: Default::default() }
        }
        pub fn map_at(
            &self,
            addr: u64,
            size: usize,
            gem: &crate::ARef<crate::gem::Object>,
            prot: Prot,
            guard: bool,
        ) -> Result<KernelMapping> {
            assert!(!guard);
            // The shared layer maps whatever size it is given: the whole object, exactly.
            assert_eq!(size, gem.size, "alias size differs from its object");
            let skip = crate::MAP_OK_BEFORE_FAIL.with(|c| {
                let n = c.get();
                c.set(n.saturating_sub(1));
                n != 0
            });
            if !skip && crate::fail(&crate::FAIL_MAP) {
                return Err(ENOMEM);
            }
            // Leaves already in use: EBUSY (prepare_map).
            let end = addr + size as u64;
            if self.held.borrow().iter().any(|m| m.gva < end && addr < m.gva + m.size as u64) {
                return Err(EBUSY);
            }
            crate::ev(crate::Ev::Map(self.slot, addr, size, gem.id));
            Ok(KernelMapping { slot: self.slot, gva: addr, size, prot: prot.0, gem: gem.clone() })
        }
        pub fn append_driver_mappings(&self, mappings: KVec<KernelMapping>) -> Result {
            if crate::fail(&crate::FAIL_APPEND) {
                return Err(ENOMEM); // the mappings drop here
            }
            for m in mappings.0 {
                crate::ev(crate::Ev::Append(self.slot, m.gva));
                self.held.borrow_mut().push(m);
            }
            Ok(())
        }
    }
}
'''

MODEL = r'''
        pub struct Ctl {
            pub epoch: u64,
        }
        impl Ctl {
            pub fn r32(&self, offset: usize) -> Result<u32> {
                if offset == control::HALT_COUNT {
                    Ok(self.epoch as u32)
                } else if offset == control::HALT_COUNT + 4 {
                    Ok((self.epoch >> 32) as u32)
                } else {
                    panic!("unexpected control read {offset:#x}")
                }
            }
        }
        pub struct Init {
            pub control: Option<std::sync::Arc<Ctl>>,
        }
        pub struct VmState {
            pub slot: u32,
            pub vm: mmu::Vm,
            pub pb_chunks: AtomicUsize,
        }
        impl VmState {
            pub fn new(slot: u32) -> Arc<Self> {
                Arc::new(VmState { slot, vm: mmu::Vm::new(slot), pb_chunks: AtomicUsize::new(0) })
            }
        }
        pub struct Job {
            pub vm: Arc<VmState>,
        }
        pub struct Flight {
            pub job: Job,
        }
        pub struct Engine {
            pub flights: Vec<Flight>,
        }
        pub struct Receipts {
            pub renders: u32,
        }
        /// A firmware object: the block list, block_ctl or the heap.
        pub struct FwObject {
            pub gva: Option<u64>,
            pub size: usize,
            /// Writes that fail from now on.
            pub failures: Cell<u32>,
        }
        impl FwObject {
            pub fn w64(&self, offset: usize, value: u64) -> Result {
                assert!(offset % 8 == 0 && offset + 8 <= self.size, "record write at {offset:#x}");
                if self.failures.get() != 0 {
                    self.failures.set(self.failures.get() - 1);
                    return Err(EIO);
                }
                crate::ev(crate::Ev::Record(offset, value));
                Ok(())
            }
            pub fn w32(&self, offset: usize, value: u32) -> Result {
                assert_eq!(offset, block_ctl::PRODUCER);
                assert!(offset + 4 <= self.size);
                crate::ev(crate::Ev::Producer(value));
                Ok(())
            }
        }
        fn gva(object: &FwObject) -> Result<u64> {
            object.gva.ok_or(EINVAL)
        }
        #[derive(Default)]
        pub struct PbStats {
            pub requests: u64,
            pub grown: u64,
            pub refused: u64,
            pub bad: u64,
        }
        pub struct Live {
            pub block_ctl: FwObject,
        }
        pub struct Pools {
            pub ledger: Ledger,
            pub exhausted_logged: bool,
            pub stats: PbStats,
            pub pb_blocks: FwObject,
            pub live: Live,
            pub drm: ARef<AsahiDevice>,
            pub chunks: KVec<PbChunk>,
        }
        impl Pools {
            pub fn chunk_count(&self) -> usize {
                self.chunks.len()
            }
            pub fn chunk_capacity(&self) -> usize {
                self.chunks.capacity()
            }
        }
        pub struct Queue(pub std::collections::VecDeque<[u32; 14]>);
        impl Queue {
            pub fn len(&self) -> usize {
                self.0.len()
            }
            pub fn is_empty(&self) -> bool {
                self.0.is_empty()
            }
            pub fn remove(&mut self, i: usize) -> core::result::Result<[u32; 14], ()> {
                assert_eq!(i, 0);
                self.0.pop_front().ok_or(())
            }
        }
        pub struct Events {
            pub grow: Queue,
        }
        /// The firmware side: its requests, and whether the next one arrives during a reply.
        pub struct FwModel {
            pub ctx: u32,
            pub epoch: u64,
            pub next_counter: u32,
            pub ask_after_reply: bool,
            pub reply_fails: bool,
            pub replies_this_pass: u32,
        }
        impl FwModel {
            pub fn request(&mut self) -> [u32; 14] {
                let mut q = [0u32; 14];
                q[buffer::grow_request::TYPE] = 7;
                q[buffer::grow_request::CONTEXT] = self.ctx;
                q[buffer::grow_request::SLOT] = 0;
                q[buffer::grow_request::COUNTER] = self.next_counter;
                q[buffer::grow_request::POSITION] = 0x100 + self.next_counter;
                q[buffer::grow_request::EPOCH] = self.epoch as u32;
                q[buffer::grow_request::EPOCH + 1] = (self.epoch >> 32) as u32;
                self.next_counter += 1;
                q
            }
        }
        pub struct Firmware {
            pub events: Events,
            pub pools: Pools,
            pub init: Init,
            pub engine: Engine,
            pub renders: Vec<u32>,
            pub fw: FwModel,
        }
        pub const HEAP: u64 = super::hw::client::PB_HEAP;
        impl Firmware {
            /// The VM with context 5 runs: one flight, its PB backing mapped (map_pb, as at the
            /// start of its prefix's publication).
            pub fn new() -> Self {
                let vm = VmState::new(5);
                let mut f = Self::without_flights();
                f.engine.flights.push(Flight { job: Job { vm: vm.clone() } });
                f.map_pb(&vm).unwrap();
                crate::take();
                f
            }
            /// Pools::new's PB backing: the initial chunk, room for every chunk the ledger needs.
            pub fn without_flights() -> Self {
                let drm = ARef::new(AsahiDevice);
                let (ledger, chunks) = pools_new_backing(&drm).unwrap();
                Firmware {
                    events: Events { grow: Queue(Default::default()) },
                    pools: Pools {
                        ledger,
                        exhausted_logged: false,
                        stats: PbStats::default(),
                        drm,
                        chunks,
                        pb_blocks: FwObject {
                            gva: Some(super::hw::client::PB_BLOCK_LIST),
                            size: buffer::BLOCK_LIST_SIZE,
                            failures: Cell::new(0),
                        },
                        live: Live {
                            block_ctl: FwObject { gva: None, size: block_ctl::SIZE, failures: Cell::new(0) },
                        },
                    },
                    init: Init { control: Some(std::sync::Arc::new(Ctl { epoch: 0x1_0000_0003 })) },
                    engine: Engine { flights: Vec::new() },
                    renders: vec![1],
                    fw: FwModel {
                        ctx: 5,
                        epoch: 0x1_0000_0003,
                        next_counter: 0,
                        ask_after_reply: false,
                        reply_fails: false,
                        replies_this_pass: 0,
                    },
                }
            }
            pub fn dev(&self) -> &() {
                &()
            }
            pub fn receipts(&self, i: usize) -> Receipts {
                Receipts { renders: self.renders[i] }
            }
            pub fn devctl(&mut self, reply: &DevCtl, wait: bool) -> Result {
                assert!(wait);
                crate::ev(crate::Ev::Reply(reply.words));
                self.fw.replies_this_pass += 1;
                assert!(self.fw.replies_this_pass <= 64, "unbounded answering in one poll pass");
                // The reply's wait drains the rings: the firmware's next request may queue now.
                if self.fw.ask_after_reply {
                    let q = self.fw.request();
                    self.events.grow.0.push_back(q);
                }
                if self.fw.reply_fails {
                    Err(EIO)
                } else {
                    Ok(())
                }
            }
            pub fn queue(&mut self) {
                let q = self.fw.request();
                self.events.grow.0.push_back(q);
            }
            /// One poll pass.
            pub fn pass(&mut self) {
                self.fw.replies_this_pass = 0;
                assert_eq!(self.answer_growth(), Ok(()));
            }
            /// What Pools::init() does to the ledger.
            pub fn reset_ledger(&mut self) {
                let p = &mut self.pools;
                p.RESET
            }
        }
'''

TESTS = r'''
#[cfg(test)]
mod pb_growth_tests {
    use crate::g15::firmware::{Firmware, VmState, Flight, Job, HEAP};
    use crate::g15::hw::{client, pte};
    use crate::g15::fw::buffer::{BLOCK_BYTES, MAX_BLOCKS};
    use crate::g15::pools::growth::{max_chunks, CHUNK_BLOCKS};
    use crate::g15::pgtable::Prot;
    use crate::{EPROTO, FAIL_ALLOC, FAIL_APPEND, FAIL_MAP};
    use core::sync::atomic::Ordering;
    use std::sync::Arc;
    use crate::g15::fw::buffer::{self, grow_reply, BLOCK_LIST_RECORDS, GROW_STEP};
    use crate::g15::fw::channels::devctl;
    use crate::g15::hw::tuning;
    use crate::g15::pools::growth::Ledger;
    use crate::g15::{params, OldLedger};
    use crate::{take, Ev, ENOMEM, PARAM, PARAM_DEFAULT};

    const CAP: u32 = BLOCK_LIST_RECORDS - 1;
    /// Refusals answered at the maximum, as the commit states them.
    const REFUSALS: u32 = 2;

    fn clamp(v: u32, lo: u32, hi: u32) -> u32 {
        v.max(lo).min(hi)
    }

    fn soft(param: u32, initial: u32, max: u32) -> u32 {
        clamp(clamp(param, 64, tuning::PB_MAX_BLOCKS), initial, max)
    }

    /// Event logs compared with a short message at the first difference.
    fn same(got: &[Ev], want: &[Ev]) {
        if let Some(i) = (0..got.len().max(want.len())).find(|&i| got.get(i) != want.get(i)) {
            panic!("event {i} of {}/{}: got {:?}, want {:?}", got.len(), want.len(), got.get(i), want.get(i));
        }
    }

    fn level(e: &Ev) -> Option<&'static str> {
        match e {
            Ev::Log(l, _) => Some(l),
            _ => None,
        }
    }

    /// The accesses, replies and log levels a ledger at `posted` gives for the firmware's
    /// requests from counter 0 on, if it keeps asking after every reply: growth to the maximum,
    /// MAX_REFUSALS refusals, then one unanswered request.
    fn expected(initial: u32, max: u32, soft_max: u32) -> (Vec<Ev>, u32, u32) {
        let mut out = Vec::new();
        let (mut posted, mut refusals, mut logged_refusal) = (initial, 0, false);
        let mut counter = 0;
        loop {
            assert!(counter < 10_000, "the expected trace does not end");
            if refusals >= REFUSALS {
                out.push(Ev::Log("warn", String::new()));
                break;
            }
            let n = GROW_STEP.min(max - posted);
            for b in posted..posted + n {
                out.push(Ev::Record(8 * b as usize, buffer::block_record(HEAP, b) as u64));
            }
            if n != 0 {
                out.push(Ev::Sync);
                out.push(Ev::Producer(posted + n));
                out.push(Ev::Sync);
            }
            let mut r = [0u32; 14];
            r[grow_reply::OPCODE] = devctl::GROW_REPLY;
            r[grow_reply::GREW] = (n != 0) as u32;
            r[grow_reply::CONTEXT] = 5;
            r[grow_reply::COUNTER] = counter;
            r[grow_reply::POSITION] = 0x100 + counter;
            r[grow_reply::EPOCH] = 3;
            r[grow_reply::EPOCH + 1] = 1;
            out.push(Ev::Reply(r));
            posted += n;
            let first_refusal = n == 0 && !logged_refusal;
            if n == 0 {
                refusals += 1;
                logged_refusal = true;
            }
            let info = counter < 2 || (n != 0 && posted > soft_max) || first_refusal;
            out.push(Ev::Log(if info { "info" } else { "dbg" }, String::new()));
            counter += 1;
        }
        (out, posted, counter + 1)
    }

    fn levels_only(evs: Vec<Ev>) -> Vec<Ev> {
        evs.into_iter()
            .filter(|e| !is_backing(e))
            .map(|e| match e {
                Ev::Log(l, _) => Ev::Log(l, String::new()),
                e => e,
            })
            .collect()
    }

    /// Growth to the maximum with the firmware asking after every reply, one pass at a time.
    fn run_to_exhaustion(param: u32) {
        PARAM.with(|p| p.set(param));
        let mut f = Firmware::new();
        let (initial, max) = (f.pools.ledger.initial, f.pools.ledger.max);
        assert_eq!(f.pools.soft_max(), soft(param, initial, max));
        f.fw.ask_after_reply = true;
        f.queue();
        take();
        let mut passes = 0;
        while !f.events.grow.is_empty() {
            let queued = f.events.grow.len();
            let before = f.pools.stats.requests;
            f.pass();
            passes += 1;
            // Exactly the requests queued before the pass were taken.
            assert_eq!(f.pools.stats.requests - before, queued as u64);
            assert!(passes <= 1000, "growth did not end");
        }
        let log = take();
        let (want, posted, requests) = expected(initial, max, soft(param, initial, max));
        same(&levels_only(log.clone()), &want);
        assert_eq!(passes, requests);
        assert_eq!(f.pools.ledger.posted, posted);
        assert_eq!(posted, max);
        // Every producer store: increasing, in 1..=4095, the last one the maximum.
        let producers: Vec<u32> = log
            .iter()
            .filter_map(|e| if let Ev::Producer(p) = e { Some(*p) } else { None })
            .collect();
        assert!(producers.windows(2).all(|w| w[0] < w[1]));
        assert!(producers.iter().all(|p| (1..=CAP).contains(p)));
        assert_eq!(*producers.last().unwrap(), max);
        // Every block above the initial ones was written once, below the list's end.
        let records: Vec<usize> = log
            .iter()
            .filter_map(|e| if let Ev::Record(o, _) = e { Some(*o) } else { None })
            .collect();
        assert_eq!(records.len() as u32, max - initial);
        assert!(records.iter().all(|o| o + 8 <= buffer::BLOCK_LIST_SIZE));
        let s = &f.pools.stats;
        assert_eq!((s.grown, s.refused, s.bad), ((max - initial) as u64, REFUSALS as u64, 1));
        let warn: Vec<&String> = log
            .iter()
            .filter_map(|e| if let Ev::Log("warn", m) = e { Some(m) } else { None })
            .collect();
        assert_eq!(warn.len(), 1);
        assert!(warn[0].contains(&format!("left unanswered: {REFUSALS} refusals at the maximum ({max} blocks)")));
        // A later request: no reply, counted, no second warning.
        f.queue();
        f.pass();
        let log = take();
        assert!(log.is_empty(), "later request after exhaustion: {log:?}");
        assert_eq!(f.pools.stats.bad, 2);
        // A new ledger, as Pools::init() makes, answers again.
        f.reset_ledger();
        f.queue();
        f.pass();
        let log = take();
        assert_eq!(log.iter().filter(|e| matches!(e, Ev::Producer(_))).count(), 1);
        assert!(log.contains(&Ev::Producer(initial + GROW_STEP.min(max - initial))));
    }

    #[test]
    fn driver_values() {
        let l = Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);
        assert!(tuning::PB_GROW);
        assert_eq!((l.initial, l.max, l.posted), (1024, 4095, 1024));
        assert_eq!(PARAM_DEFAULT, 3072);
        PARAM.with(|p| p.set(PARAM_DEFAULT));
        assert_eq!(params::pb_max_blocks(), 3072);
        assert_eq!(Firmware::new().pools.soft_max(), 3072);
    }

    #[test]
    fn soft_max_bounds() {
        let f = Firmware::new();
        for param in [0, 1, 63, 64, 65, 1023, 1024, 1025, 3071, 3072, 3073, 4094, 4095, 4096, 8192, u32::MAX] {
            PARAM.with(|p| p.set(param));
            assert_eq!(params::pb_max_blocks(), clamp(param, 64, tuning::PB_MAX_BLOCKS), "pb_max_blocks, param {param}");
            // Below the initial 1024 the firmware still gets 1024.
            assert_eq!(f.pools.soft_max(), clamp(param, 1024, CAP), "soft_max, param {param}");
        }
    }

    #[test]
    fn default_growth_to_exhaustion() {
        run_to_exhaustion(PARAM_DEFAULT);
    }

    #[test]
    fn soft_max_boundaries() {
        // 3074 is reached exactly (1024 + 205 * 10): that answer is not past it.
        for param in [0, 1024, 3072, 3074, 3075, 4094, 4095, u32::MAX] {
            run_to_exhaustion(param);
        }
    }

    #[test]
    fn queued_snapshot_keeps_order() {
        let mut f = Firmware::new();
        f.fw.ask_after_reply = true;
        f.queue();
        f.queue();
        take();
        f.pass();
        let log = take();
        let replies: Vec<u32> = log
            .iter()
            .filter_map(|e| if let Ev::Reply(r) = e { Some(r[grow_reply::COUNTER]) } else { None })
            .collect();
        assert_eq!(replies, vec![0, 1], "replies in the first pass");
        // Compare both requests' complete record/barrier/producer/reply/log sequences,
        // not just their reply order. Each request finishes before the next one's records.
        let (want, _, _) = expected(f.pools.ledger.initial, f.pools.ledger.max, f.pools.soft_max());
        let end = want.iter().position(|e| matches!(e, Ev::Reply(r) if r[grow_reply::COUNTER] == 1))
            .expect("second expected reply") + 2; // Include its log too.
        same(&levels_only(log), &want[..end]);
        let queued: Vec<u32> = f.events.grow.0.iter().map(|q| q[buffer::grow_request::COUNTER]).collect();
        assert_eq!(queued, vec![2, 3]);
        assert_eq!(f.pools.stats.requests, 2);
        f.pass();
        assert_eq!(f.pools.stats.requests, 4);
        assert_eq!(f.events.grow.len(), 2);
    }

    #[test]
    fn empty_queue_does_nothing() {
        let mut f = Firmware::new();
        take();
        f.pass();
        assert!(take().is_empty());
        assert_eq!(f.pools.stats.requests, 0);
    }

    #[test]
    fn unprocessed_request_does_not_end_the_pass() {
        let mut f = Firmware::new();
        f.queue();
        f.queue();
        f.pools.pb_blocks.failures.set(1);
        take();
        f.pass();
        let log = take();
        // The second request was answered in the same pass, from the same posted count.
        assert_eq!(f.pools.stats.requests, 2, "requests taken in the pass");
        assert_eq!(f.pools.stats.bad, 1);
        assert!(log.iter().any(|e| matches!(e, Ev::Log("err", m) if m.contains("not answered: -5"))));
        assert!(log.contains(&Ev::Producer(1024 + GROW_STEP)));
        assert!(f.events.grow.is_empty());
    }

    #[test]
    fn failed_reply_is_logged_and_counted_as_grown() {
        let mut f = Firmware::new();
        f.fw.reply_fails = true;
        f.queue();
        f.queue();
        f.queue();
        take();
        f.pass();
        let log = take();
        assert_eq!(f.pools.ledger.posted, 1024 + 3 * GROW_STEP);
        assert_eq!((f.pools.stats.grown, f.pools.stats.bad), (3 * GROW_STEP as u64, 0));
        // Each failed reply is logged at info level.
        let info = log.iter().filter(|e| matches!(e, Ev::Log("info", m) if m.contains("DEVCTL 7: -5"))).count();
        assert_eq!(info, 3);
    }

    #[test]
    fn invalid_requests_get_no_reply() {
        let mut f = Firmware::new();
        let good = f.fw.request();
        let mut cases = Vec::new();
        let mut q = good;
        q[buffer::grow_request::TYPE] = 6;
        cases.push(q);
        let mut q = good;
        q[buffer::grow_request::SLOT] = 1;
        cases.push(q);
        let mut q = good;
        q[buffer::grow_request::EPOCH] = 2;
        cases.push(q);
        let mut q = good;
        q[buffer::grow_request::CONTEXT] = 6;
        cases.push(q);
        for q in cases {
            f.events.grow.0.push_back(q);
        }
        take();
        f.pass();
        let log = take();
        assert!(log.iter().all(|e| matches!(e, Ev::Log("warn", _))), "{log:?}");
        assert_eq!(log.len(), 4);
        assert_eq!(f.pools.stats.bad, 4);
        assert_eq!(f.pools.ledger, Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS));
        // No render published: not live.
        f.renders[0] = 0;
        f.events.grow.0.push_back(good);
        f.pass();
        assert!(!take().iter().any(|e| matches!(e, Ev::Reply(_))));
        // A stale counter after an answer.
        f.renders[0] = 1;
        f.events.grow.0.push_back(good);
        f.pass();
        assert!(take().iter().any(|e| matches!(e, Ev::Reply(_))));
        f.events.grow.0.push_back(good);
        f.pass();
        assert!(!take().iter().any(|e| matches!(e, Ev::Reply(_))));
        assert_eq!(f.pools.stats.bad, 6);
    }

    fn is_backing(e: &Ev) -> bool {
        matches!(e, Ev::Alloc(..) | Ev::Free(_) | Ev::Map(..) | Ev::Append(..) | Ev::Unmap(..))
    }

    /// A VM's PB aliases: its count, and the aliases it holds, from block 0 in order.
    fn assert_cursor(vm: &VmState, chunks: usize) {
        let held = vm.vm.held.borrow();
        assert_eq!(vm.pb_chunks.load(Ordering::Relaxed), held.len(), "count differs from held aliases");
        assert!(held.len() <= chunks);
        let mut gva = client::PB_HEAP;
        for m in held.iter() {
            assert_eq!(m.gva, gva, "aliases not contiguous from the heap's start");
            assert_eq!(m.prot, pte::LEAF_CACHED);
            gva += m.size as u64;
        }
    }

    /// In the order of `log`: no block record and no producer past the blocks the VM with
    /// context `slot` holds aliases for at that point.
    fn assert_posted_backed(log: &[Ev], slot: u32, start: u64) {
        let mut mapped = start;
        for e in log {
            match e {
                Ev::Append(s, gva) if *s == slot => {
                    assert_eq!(*gva, client::PB_HEAP + mapped * BLOCK_BYTES, "append out of order");
                }
                Ev::Map(s, gva, size, _) if *s == slot && *gva == client::PB_HEAP + mapped * BLOCK_BYTES => {
                    // Counted at its append.
                    let _ = size;
                }
                _ => {}
            }
            if let Ev::Append(s, gva) = e {
                if *s == slot {
                    let size = log
                        .iter()
                        .find_map(|m| match m {
                            Ev::Map(ms, mg, size, _) if ms == s && mg == gva => Some(*size as u64),
                            _ => None,
                        })
                        .unwrap();
                    mapped += size / BLOCK_BYTES;
                }
            }
            if let Ev::Record(off, _) = e {
                assert!((*off as u64 / 8) < mapped, "record of block {} with {} mapped", off / 8, mapped);
            }
            if let Ev::Producer(p) = e {
                assert!(*p as u64 <= mapped, "producer {} with {} mapped", p, mapped);
            }
        }
    }

    fn count(log: &[Ev], f: impl Fn(&Ev) -> bool) -> usize {
        log.iter().filter(|e| f(e)).count()
    }

    /// The firmware asks until the ledger reaches `target` posted blocks, one request a pass.
    fn grow_to(f: &mut Firmware, target: u32) {
        for _ in 0..1000 {
            if f.pools.ledger.posted >= target {
                return;
            }
            f.queue();
            f.pass();
        }
        panic!("growth stopped at {} blocks", f.pools.ledger.posted);
    }

    #[test]
    fn startup_backs_the_initial_blocks() {
        let mut f = Firmware::without_flights();
        let log = take();
        let (initial, max) = (f.pools.ledger.initial, f.pools.ledger.max);
        assert_eq!(log, vec![Ev::Alloc(1, initial as usize * BLOCK_BYTES as usize)], "startup allocation");
        assert_eq!((f.pools.backed(), f.pools.chunk_count()), (initial, 1));
        assert_eq!(f.pools.chunk_capacity(), max_chunks(initial, max));
        assert_eq!(max_chunks(initial, max), 97);
        // The first prefix of a VM maps the backing, nothing else.
        let vm = VmState::new(7);
        f.engine.flights.push(Flight { job: Job { vm: vm.clone() } });
        f.map_pb(&vm).unwrap();
        let size = initial as usize * BLOCK_BYTES as usize;
        assert_eq!(take(), vec![Ev::Map(7, client::PB_HEAP, size, 1), Ev::Append(7, client::PB_HEAP)]);
        assert_cursor(&vm, 1);
        // Mapping again maps nothing.
        f.map_pb(&vm).unwrap();
        assert!(take().is_empty());
    }

    #[test]
    fn pb_leaf_is_gpu_only() {
        let prot = Prot::from_pte(pte::LEAF_CACHED);
        assert!(prot.ap_is_gpu() && !prot.is_cached_noncoherent(), "PB leaf not GPU-only");
    }

    #[test]
    fn growth_backs_before_posting() {
        PARAM.with(|p| p.set(PARAM_DEFAULT));
        let mut f = Firmware::new();
        f.fw.ask_after_reply = true;
        f.queue();
        take();
        let mut passes = 0;
        while !f.events.grow.is_empty() {
            f.pass();
            passes += 1;
            assert!(passes <= 1000, "growth did not end");
        }
        let log = take();
        let (initial, max) = (f.pools.ledger.initial, f.pools.ledger.max);
        assert_posted_backed(&log, 5, initial as u64);
        // Chunks of at most 32 blocks, allocated only as needed: up to the maximum, no further.
        let allocs: Vec<usize> = log
            .iter()
            .filter_map(|e| if let Ev::Alloc(_, b) = e { Some(*b) } else { None })
            .collect();
        assert_eq!(allocs.len() + 1, max_chunks(initial, max));
        assert!(allocs.iter().all(|b| *b <= 4 << 20), "a growth chunk above 4 MiB");
        assert_eq!(CHUNK_BLOCKS as u64 * BLOCK_BYTES, 4 << 20);
        assert_eq!(f.pools.backed(), max);
        assert_eq!(count(&log, |e| matches!(e, Ev::Free(_) | Ev::Unmap(..))), 0);
        assert_cursor(&f.engine.flights[0].job.vm, f.pools.chunk_count());
    }

    /// A failure while backing posts nothing and answers nothing; the request counter and the
    /// VM's count stay; the next request is answered.
    fn failure_posts_nothing(which: &'static std::thread::LocalKey<std::cell::Cell<u32>>) -> Vec<Ev> {
        let mut f = Firmware::new();
        which.with(|c| c.set(1));
        f.queue();
        f.pass();
        let log = take();
        assert_eq!(count(&log, |e| matches!(e, Ev::Record(..) | Ev::Producer(_) | Ev::Reply(_) | Ev::Sync)), 0,
                   "posted after a backing failure: {log:?}");
        assert_eq!((f.pools.ledger.posted, f.pools.ledger.next_request), (1024, 0), "ledger after a backing failure");
        assert_eq!((f.pools.stats.bad, f.pools.chunk_count()), (1, 1), "chunks after a backing failure");
        assert_cursor(&f.engine.flights[0].job.vm, 1);
        assert!(log.iter().any(|e| matches!(e, Ev::Log("err", m) if m.contains("not answered: -12"))));
        f.queue();
        f.pass();
        let retry = take();
        assert!(retry.contains(&Ev::Producer(1034)));
        assert_eq!(f.pools.chunk_count(), 2);
        assert_cursor(&f.engine.flights[0].job.vm, 2);
        log
    }

    #[test]
    fn allocation_failure_posts_nothing() {
        let log = failure_posts_nothing(&FAIL_ALLOC);
        assert_eq!(count(&log, is_backing), 0);
    }

    #[test]
    fn map_failure_posts_nothing() {
        let log = failure_posts_nothing(&FAIL_MAP);
        let backing: Vec<&Ev> = log.iter().filter(|e| is_backing(e)).collect();
        assert!(matches!(backing[..], [Ev::Alloc(2, _), Ev::Free(2)]), "{backing:?}");
    }

    #[test]
    fn append_failure_posts_nothing() {
        let log = failure_posts_nothing(&FAIL_APPEND);
        let backing: Vec<&Ev> = log.iter().filter(|e| is_backing(e)).collect();
        assert!(
            matches!(backing[..], [Ev::Alloc(2, _), Ev::Map(5, _, _, 2), Ev::Unmap(5, _), Ev::Free(2)]),
            "{backing:?}"
        );
    }

    #[test]
    fn transport_failure_keeps_everything() {
        let mut f = Firmware::new();
        f.fw.reply_fails = true;
        f.queue();
        f.pass();
        let log = take();
        assert!(log.contains(&Ev::Producer(1034)));
        assert_eq!((f.pools.ledger.posted, f.pools.chunk_count()), (1034, 2));
        assert_cursor(&f.engine.flights[0].job.vm, 2);
        assert_eq!(count(&log, |e| matches!(e, Ev::Free(_) | Ev::Unmap(..))), 0);
    }

    #[test]
    fn other_vms_catch_up_before_they_run() {
        let mut f = Firmware::new();
        let a = f.engine.flights[0].job.vm.clone();
        grow_to(&mut f, 1100);
        take();
        assert_eq!(f.pools.chunk_count(), 4);
        // A's work retires; a new VM runs next: it maps every chunk first.
        f.engine.flights.clear();
        let b = VmState::new(6);
        f.engine.flights.push(Flight { job: Job { vm: b.clone() } });
        f.fw.ctx = 6;
        f.map_pb(&b).unwrap();
        let log = take();
        assert_eq!(count(&log, |e| matches!(e, Ev::Append(6, _))), 4);
        assert_eq!(count(&log, |e| matches!(e, Ev::Alloc(..))), 0);
        assert_cursor(&b, 4);
        // B grows the backing; A, behind, catches up when it runs again.
        grow_to(&mut f, 1200);
        let log = take();
        assert_posted_backed(&log, 6, 1120);
        assert_eq!(count(&log, |e| matches!(e, Ev::Map(5, ..))), 0);
        f.engine.flights.clear();
        f.engine.flights.push(Flight { job: Job { vm: a.clone() } });
        f.map_pb(&a).unwrap();
        assert_cursor(&a, f.pools.chunk_count());
        assert_eq!(a.pb_chunks.load(Ordering::Relaxed), f.pools.chunk_count());
    }

    #[test]
    fn growth_catches_the_running_vm_up_first() {
        let mut f = Firmware::new();
        let a = f.engine.flights[0].job.vm.clone();
        // B grows the backing while A is idle.
        f.engine.flights.clear();
        let b = VmState::new(6);
        f.engine.flights.push(Flight { job: Job { vm: b.clone() } });
        f.map_pb(&b).unwrap();
        f.fw.ctx = 6;
        grow_to(&mut f, 1100);
        // A runs again; its growth request maps what it lacks before backing more.
        f.engine.flights.clear();
        f.engine.flights.push(Flight { job: Job { vm: a.clone() } });
        f.fw.ctx = 5;
        take();
        grow_to(&mut f, 1130);
        let log = take();
        assert_eq!(a.vm.held.borrow().len(), f.pools.chunk_count(), "running VM not caught up");
        assert_cursor(&a, f.pools.chunk_count());
        assert_posted_backed(&log, 5, 1024);
    }

    #[test]
    fn partial_catch_up_counts_only_held_aliases() {
        let mut f = Firmware::new();
        grow_to(&mut f, 1100);
        f.engine.flights.clear();
        let b = VmState::new(6);
        f.engine.flights.push(Flight { job: Job { vm: b.clone() } });
        take();
        // The third alias cannot be made: two held.
        FAIL_MAP.with(|c| c.set(0));
        crate::MAP_OK_BEFORE_FAIL.with(|c| c.set(2));
        FAIL_MAP.with(|c| c.set(1));
        assert_eq!(f.map_pb(&b), Err(ENOMEM));
        assert_cursor(&b, 4);
        assert_eq!(b.pb_chunks.load(Ordering::Relaxed), 2);
        // The third alias is made but cannot be held: dropped, still two.
        FAIL_APPEND.with(|c| c.set(1));
        assert_eq!(f.map_pb(&b), Err(ENOMEM));
        assert_eq!(b.pb_chunks.load(Ordering::Relaxed), 2, "count after a failed append");
        assert!(take().iter().any(|e| matches!(e, Ev::Unmap(6, _))));
        assert_cursor(&b, 4);
        f.map_pb(&b).unwrap();
        assert_eq!(b.pb_chunks.load(Ordering::Relaxed), 4);
        assert_cursor(&b, 4);
    }

    /// The ledger reset of Pools::init(), not a full recovery.
    #[test]
    fn ledger_reset_reuses_the_backing() {
        let mut f = Firmware::new();
        grow_to(&mut f, 1300);
        let chunks = f.pools.chunk_count();
        take();
        f.reset_ledger();
        assert_eq!(f.pools.ledger.posted, 1024);
        grow_to(&mut f, 1300);
        let log = take();
        assert_eq!(count(&log, |e| matches!(e, Ev::Alloc(..) | Ev::Map(..))), 0);
        assert_eq!(f.pools.chunk_count(), chunks);
        assert_posted_backed(&log, 5, f.pools.backed() as u64);
    }

    #[test]
    fn rejected_requests_allocate_nothing() {
        let mut f = Firmware::new();
        let mut q = f.fw.request();
        q[buffer::grow_request::EPOCH] = 2; // a halt epoch that has passed
        f.events.grow.0.push_back(q);
        f.pass();
        let log = take();
        assert_eq!(count(&log, is_backing), 0);
        assert_eq!(f.pools.stats.bad, 1);
    }

    #[test]
    fn catch_up_refuses_flights_of_another_vm() {
        let mut f = Firmware::new();
        let a = f.engine.flights[0].job.vm.clone();
        let b = VmState::new(6);
        f.engine.flights.push(Flight { job: Job { vm: b.clone() } });
        assert_eq!(f.map_pb(&b), Err(EPROTO), "catch-up with a flight of another VM");
        assert_eq!(b.pb_chunks.load(Ordering::Relaxed), 0);
        assert_eq!(count(&take(), is_backing), 0);
        // A count past the chunks.
        f.engine.flights.pop();
        a.pb_chunks.store(f.pools.chunk_count() + 1, Ordering::Relaxed);
        assert_eq!(f.map_pb(&a), Err(EPROTO), "catch-up with a count past the chunks");
    }

    #[test]
    fn grant_refuses_flights_of_two_vms() {
        let mut f = Firmware::new();
        let b = VmState::new(6);
        f.engine.flights.push(Flight { job: Job { vm: b.clone() } });
        f.renders.push(1);
        f.queue();
        f.pass();
        let log = take();
        assert_eq!(
            count(&log, |e| is_backing(e) || matches!(e, Ev::Reply(_) | Ev::Producer(_) | Ev::Record(..))),
            0,
            "grant with flights of two VMs"
        );
        assert!(log.iter().any(|e| matches!(e, Ev::Log("err", m) if m.contains("not answered: -71"))));
    }

    #[test]
    fn constructor_is_the_old_one_capped() {
        let mut values: Vec<u32> = (0..=4200).collect();
        values.extend([8191, 8192, u32::MAX - 1, u32::MAX]);
        let mut changed = 0u64;
        let mut cases = 0u64;
        for grow in [true, false] {
            for &blocks in &values {
                for &max_blocks in &values {
                    let old = OldLedger::new(grow, blocks, max_blocks);
                    let new = Ledger::new(grow, blocks, max_blocks);
                    cases += 1;
                    assert_eq!(
                        (new.initial, new.max, new.posted),
                        (old.initial.min(CAP), old.max.min(CAP), old.posted.min(CAP)),
                        "new({grow}, {blocks}, {max_blocks})"
                    );
                    assert!((1..=CAP).contains(&(new.posted % BLOCK_LIST_RECORDS)));
                    if old.max <= CAP {
                        assert_eq!((new.initial, new.max), (old.initial, old.max));
                    } else {
                        changed += 1;
                    }
                }
            }
        }
        // Only inputs the old constructor took to the whole 4096-block heap changed.
        assert!(changed > 0 && changed < cases);
        // The driver's values are below the cap, so its ledger is the old one.
        let d = (tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);
        let (old, new) = (OldLedger::new(d.0, d.1, d.2), Ledger::new(d.0, d.1, d.2));
        if d.2 <= CAP {
            assert_eq!((old.initial, old.max, old.posted), (new.initial, new.max, new.posted));
        }
        println!("constructor cases {cases}, changed {changed}");
    }
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--output', type=Path, help='Work directory (default: a temporary one)')
    p.add_argument('--ref', help='Read the tested bodies at this git revision')
    p.add_argument('--mutation', choices=sorted(MUTATIONS) + sorted(CONTROLS))
    p.add_argument('--mutations', action='store_true',
                   help='Also require every mutation to fail and every control to pass')
    a = p.parse_args()
    import tempfile
    out = (a.output or Path(tempfile.mkdtemp(prefix='g15-pb-growth-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)

    def read(path, ref=None):
        ref = ref or a.ref
        return (subprocess.check_output(['git', 'show', f'{ref}:{path}'], cwd=ROOT, text=True,
                                        stderr=subprocess.DEVNULL)
                if ref else (ROOT / path).read_text())

    # The embedded reference is the commit's own text, when the commit is here.
    try:
        old_git = item(read(GROWTH.replace("drivers/gpu/drm/asahi/", "drivers/gpu/drm/asahi-m3/"), OLD_REF), '    pub(crate) const fn new(')
    except (subprocess.CalledProcessError, ValueError):
        old_git = None
    if old_git is not None and old_git != OLD_LEDGER_NEW:
        raise SystemExit('embedded Ledger::new() differs from ' + OLD_REF)

    def run(mutation):
        paths = (POOLS, GROWTH, BUFFER, CHANNELS, INITDATA, PARAMS, HW, CORE, ENGINE, PGTABLE)
        src = {path: read(path) for path in paths}
        if mutation:
            spec = {**MUTATIONS, **CONTROLS}[mutation]
            for path, old, new in (spec if isinstance(spec, list) else [spec]):
                if src[path].count(old) != 1:
                    raise SystemExit(f'mutation {mutation}: anchor not found once')
                src[path] = src[path].replace(old, new)
        pools, hw = src[POOLS], src[HW]
        bodies = [item(pools, s) for s in ('    pub(crate) fn answer_growth(&mut self) -> Result {',
                                           '    fn answer_request(&mut self, request: &[u32; 14]) -> Result {',
                                           '    pub(crate) fn map_pb(&mut self, vm: &VmState) -> Result {',
                                           '    fn running_vm(&self, ctx: u32) -> Result<Arc<VmState>> {',
                                           '    fn back_pb(&mut self, end: u32, ctx: u32) -> Result {')]
        soft_max = item(pools, '    pub(crate) fn soft_max(&self) -> u32 {')
        backed = item(pools, '    pub(crate) fn backed(&self) -> u32 {')
        chunk = item(pools, 'struct PbChunk {') + '\n' + item(pools, 'impl PbChunk {')
        start = pools.index(POOLS_NEW_BACKING[0])
        end = pools.index('\n', pools.index(POOLS_NEW_BACKING[1], start)) + 1
        pools_new_backing = pools[start:end]
        if pools.count(PB_LEAF_ASSERT) != 1:
            raise SystemExit('pools.rs does not assert the PB leaf GPU-only')
        # Every prefix maps the PB into its VM before anything else.
        prefix_into = item(src[ENGINE], '    fn publish_prefix_into(&mut self, job: &Arc<Job>, f: &mut Flight) -> Result {')
        statements = [l.strip() for l in prefix_into.splitlines()[1:] if l.strip() and not l.strip().startswith('//')]
        if not statements[0].startswith('self.map_pb(&job.vm)'):
            raise SystemExit('publish_prefix_into does not start with the PB catch-up')
        pgtable = src[PGTABLE]
        pg_consts = '\n'.join(re.search(rf'^const {n}: \w+ = [^;]+;', pgtable, re.M).group(0)
                              for n in ('UAT_AP_SHIFT', 'UAT_AP_BITS', 'UAT_HIGH_BITS_SHIFT', 'UAT_MEMATTR_SHIFT',
                                        'UAT_MEMATTR_BITS', 'MEMATTR_CACHED', 'AP_GPU'))
        prot = (item(pgtable, 'pub(crate) struct Prot {') + '\nimpl Prot {\n' +
                item(pgtable, '    pub(crate) const fn from_pte(pte: u64) -> Self {') + '\n' +
                item(pgtable, '    pub(crate) const fn is_cached_noncoherent(&self) -> bool {') +
                '\n    pub fn ap_is_gpu(&self) -> bool { self.ap == AP_GPU }\n}\n')
        init = item(pools, '    fn init(&mut self) -> Result {')
        for line in LEDGER_RESET:
            if init.count(line) != 1:
                raise SystemExit('Pools::init() does not hold: ' + line)
        reset = '\n'.join('p.' + line[len('self.'):] for line in LEDGER_RESET)
        pb_max_blocks = item(src[PARAMS], 'pub(crate) fn pb_max_blocks() -> u32 {')
        m = re.search(r'g15_pb_max_blocks: u32 \{\s*default: (\d+),', src[CORE])
        if not m:
            raise SystemExit('g15_pb_max_blocks default not found')
        param_default = int(m.group(1))
        sizes = consts(item(hw, 'pub(crate) mod sizes {'), ['PB_BLOCKS'])
        tuning = consts(item(hw, 'pub(crate) mod tuning {'), ['PB_GROW', 'PB_BLOCKS', 'PB_MAX_BLOCKS'])
        client = consts(item(hw, 'pub(crate) mod client {'), ['PB_HEAP', 'PB_BLOCK_LIST'])
        pte = consts(item(hw, 'pub(crate) mod pte {'), ['LEAF_CACHED'])
        control = consts(item(src[INITDATA], 'pub(crate) mod control {'), ['HALT_COUNT'])
        growth = src[GROWTH]
        clamp_fn = item(growth, 'const fn clamp(v: u32, lo: u32, hi: u32) -> u32 {')
        tested = (''.join(bodies) + soft_max + backed + chunk + pools_new_backing + pb_max_blocks + growth +
                  src[BUFFER] + sizes + tuning + pte + prot)
        dir = out / (mutation or 'actual')
        dir.mkdir(exist_ok=True)
        for path in (GROWTH, BUFFER, CHANNELS):
            (dir / Path(path).name).write_text(src[path])
        code = (PREFIX + f'pub const PARAM_DEFAULT: u32 = {param_default};\n' +
                'pub mod g15 {\n'
                '    pub mod fw {\n'
                f'        #[path = "{dir / "channels.rs"}"] pub mod channels;\n'
                f'        #[path = "{dir / "buffer.rs"}"] pub mod buffer;\n'
                '        pub mod initdata { pub mod control {\n' + control + '\n} }\n'
                '    }\n'
                f'    pub mod pools {{ #[path = "{dir / "growth.rs"}"] pub mod growth; }}\n'
                '    pub mod hw {\n'
                '        pub mod sizes {\n' + sizes + '\n}\n'
                '        pub mod tuning {\n' + tuning + '\n}\n'
                '        pub mod client {\n' + client + '\n}\n'
                '        pub mod pte {\n' + pte + '\n}\n'
                '    }\n'
                '    pub mod pgtable {\n' + pg_consts + '\n' + prot + '\n    }\n'
                '    pub mod boot {\n        pub fn eproto() -> crate::Error { crate::EPROTO }\n    }\n'
                '    /// The checked backing allocation, failing where a test injects it.\n'
                '    pub mod object {\n'
                '        use crate::prelude::*;\n'
                '        pub struct HeldPages;\n'
                '        pub fn gpu_backing(_: &crate::driver::AsahiDevice, bytes: usize)\n'
                '            -> Result<(crate::gem::ObjectRef, HeldPages)> {\n'
                '            if crate::fail(&crate::FAIL_ALLOC) { return Err(ENOMEM); }\n'
                '            let id = crate::NEXT_ID.with(|n| { let id = n.get(); n.set(id + 1); id });\n'
                '            crate::ev(crate::Ev::Alloc(id, bytes));\n'
                '            Ok((crate::gem::ObjectRef { gem: crate::ARef::new(crate::gem::Object { id, size: bytes }) }, HeldPages))\n'
                '        }\n'
                '    }\n'
                '    pub mod params {\n        use crate::module_parameters;\n' + pb_max_blocks + '\n    }\n'
                '    /// Ledger::new() at ' + OLD_REF[:12] + '.\n'
                '    pub struct OldLedger { pub initial: u32, pub max: u32, pub posted: u32, pub next_request: u32,\n'
                '                           pub refusal_logged: bool, pub refusals: u32 }\n'
                '    use crate::g15::fw::buffer::{MAX_BLOCKS, MIN_BLOCKS};\n'
                '    impl OldLedger {\n' + OLD_LEDGER_NEW + '\n    }\n' + clamp_fn + '\n'
                '    pub mod firmware {\n'
                '        use crate::prelude::*;\n'
                '        use std::cell::Cell;\n'
                '        use super::fw::buffer::{self, block_ctl};\n'
                '        use super::fw::channels::DevCtl;\n'
                '        use super::fw::initdata::control;\n'
                '        use super::hw::{client, pte, tuning};\n'
                '        use super::pools::growth::{self, Ledger};\n'
                '        use super::object::{self, HeldPages};\n'
                '        use crate::driver::AsahiDevice;\n'
                '        use crate::{gem, mmu, ARef};\n'
                '        use core::sync::atomic::{AtomicUsize, Ordering};\n'
                '        use std::sync::Arc;\n' +
                MODEL.replace('p.RESET', reset).replace('POOLS_NEW_BACKING', '') +
                chunk + '\n'
                '        /// Pools::new\'s PB backing, verbatim.\n'
                '        pub fn pools_new_backing(dev: &AsahiDevice) -> Result<(Ledger, KVec<PbChunk>)> {\n' +
                pools_new_backing + '        Ok((ledger, chunks))\n        }\n'
                '        impl Pools {\n' + soft_max + '\n' + backed + '\n        }\n'
                '        impl Firmware {\n' + '\n'.join(bodies) + '\n        }\n'
                '    }\n'
                '}\n' + TESTS)
        path = dir / 'growth-answer.rs'
        path.write_text(code)
        binary = dir / 'growth-answer'
        build = subprocess.run(['rustup', 'run', '1.93.1', 'rustc', '--edition=2021', '--test', '-O',
                                '-C', 'overflow-checks=on', str(path), '-o', str(binary)],
                               capture_output=True, text=True)
        if build.returncode != 0:
            raise SystemExit(f'{mutation or "actual"}: build failed\n{build.stderr[-3000:]}')

        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        try:
            r = subprocess.run([str(binary), '--show-output', 'pb_growth_tests::'], capture_output=True,
                               text=True, preexec_fn=no_core, timeout=60)
        except subprocess.TimeoutExpired:
            # A hang is not a test failure: no test is reported failed, so no mutation counts.
            return {'mutation': mutation, 'exit': 'timeout', 'summary': [], 'failed': [], 'output': {},
                    'stderr': 'timed out after 60 s'}
        summary = [line for line in r.stdout.splitlines() if line.startswith('test result:')]
        failed = sorted(line.split()[1].removeprefix('pb_growth_tests::') for line in r.stdout.splitlines()
                        if line.startswith('test ') and line.endswith('FAILED'))
        # Each test's captured output: the panic message of a failure, the counts of a success.
        output = {m.group(1).removeprefix('pb_growth_tests::'): m.group(2).strip()
                  for m in re.finditer(r'---- (\S+) stdout ----\n(.*?)(?=\n---- |\n(?:failures|successes):\n|\Z)',
                                       r.stdout, re.S)}
        source_hashes = {path: hashlib.sha256(src[path].encode()).hexdigest() for path in paths}
        source_inputs = json.dumps(source_hashes, sort_keys=True, separators=(',', ':')).encode()
        return {'mutation': mutation, 'tested_sha256': hashlib.sha256(tested.encode()).hexdigest(),
                'source_sha256': source_hashes,
                'source_inputs_sha256': hashlib.sha256(source_inputs).hexdigest(),
                'exit': r.returncode, 'summary': summary, 'failed': failed, 'output': output,
                'stderr': r.stderr[-2000:]}

    result = run(a.mutation)
    receipt = {'ref': a.ref, 'old_ref': OLD_REF, 'old_reference_checked_against_git': old_git is not None,
               **result,
               'scope': 'Actual answer_growth/answer_request/soft_max/pb_max_blocks, pools/growth.rs, '
                        'fw/buffer.rs and fw/channels.rs over a model of the firmware objects with an '
                        'ordered access log, and Ledger::new against its body at '
                        f'{OLD_REF[:12]}. Not kernel, firmware or GPU execution, timing, or recovery.'}
    if a.mutations and not a.mutation:
        receipt['mutations'] = {}
        for m in sorted(MUTATIONS):
            res = run(m)
            test, message = INTENDED[m]
            got = res['output'].get(test, '')
            receipt['mutations'][m] = {'exit': res['exit'], 'failed': res['failed'], 'intended_test': test,
                                       'intended_failure': test in res['failed'] and message in got,
                                       'message': '\n'.join(l for l in got.splitlines()
                                                            if not l.startswith('note:'))[-600:]}
        receipt['controls'] = {}
        for m in sorted(CONTROLS):
            res = run(m)
            receipt['controls'][m] = {'exit': res['exit'], 'failed': res['failed'], 'summary': res['summary']}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k not in ('output', 'stderr')}, indent=2))
    # The actual bodies and the controls must pass; a single requested mutation must fail.
    expect_fail = a.mutation in MUTATIONS
    if (receipt['exit'] != 0) != expect_fail:
        raise SystemExit(1)
    if expect_fail:
        test, message = INTENDED[a.mutation]
        if test not in receipt['failed'] or message not in receipt['output'].get(test, ''):
            raise SystemExit('the mutation timed out or failed elsewhere than intended')
    if a.mutations and any(v['exit'] == 0 or not v['intended_failure']
                           for v in receipt.get('mutations', {}).values()):
        raise SystemExit('a mutation survived or failed elsewhere than intended')
    if a.mutations and any(v['exit'] != 0 for v in receipt.get('controls', {}).values()):
        raise SystemExit('a control failed')


if __name__ == '__main__':
    main()
