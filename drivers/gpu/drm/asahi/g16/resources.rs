// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright (C) 2026 Eryk Wieliczko

//! Lazy per-VM command backing. Allocation runs only in SUBMIT, before accepting a fence.
//! The completion path reads prepared references and never takes the construction mutex.
//! Normal-NC maps are reclaimed only after all physical-slot leases have been displaced.

use super::{
    command::Command,
    object::{Factory, Objects},
    pools::{PbEpoch, PoolSet},
    slots::{ComputeSlot, PassSlot},
    vm::VmState,
};
use crate::mmu;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use kernel::{
    new_condvar, new_mutex, new_spinlock_irq,
    prelude::*,
    sync::{Arc, CondVar, CondVarTimeoutResult, Mutex, SpinLockIrq},
};

/// Never-exposed PB epochs an open VM keeps on standby after bursts drain.
///
/// With PB epoch reuse most renders never expose a fresh epoch; keeping a
/// shared per-VM standby pool instead of attaching one epoch per queued job
/// avoids allocating and mapping fresh epochs whenever submission depth rises.
const SPARE_EPOCHS: usize = 32;

pub(crate) struct ComputeArena(crate::cleanup::Deferred<ComputeInner>);
impl core::ops::Deref for ComputeArena {
    type Target = ComputeInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct ComputeInner {
    pub(crate) compute: KVec<ComputeSlot>,
    _objects: Objects,
    // Mappings must disappear while their root still owns its ASID.
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
/// Compute units every VM gets with its first compute command; more are added
/// ([`Cache::prepare`]) once it submits compute back to back.
pub(crate) const BASE_COMPUTE_UNITS: usize = 2;

/// Back-to-back compute submissions (each within [`BURST_GAP_NS`] of the previous) after
/// which a VM gets its extension units; a single submission with [`BURST_COMMANDS`] compute
/// commands qualifies at once.
const BURST_SUBMITS: u32 = 3;
const BURST_GAP_NS: u64 = 2_000_000;
const BURST_COMMANDS: usize = 3;

impl ComputeArena {
    /// Units `units` (their index fixes their client addresses and hardware UMA slot).
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        units: core::ops::Range<usize>,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let mut compute = KVec::with_capacity(units.len(), GFP_KERNEL)?;
        for k in units {
            compute.push(ComputeSlot::new(&mut objects, k as u64)?, GFP_KERNEL)?;
        }
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(ComputeInner {
                compute,
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
}

/// Render pass slots every VM gets with its first render command; extension slots
/// (`BASE_RENDER_SLOTS..hw::sizes::RENDER_SLOTS`) are added lazily once the VM actually
/// pipelines renders or submits a multi-pass/burst frame.
pub(crate) const BASE_RENDER_SLOTS: usize = super::hw::sizes::BASE_RENDER_SLOTS;
const EXT_RENDER_SLOTS: usize = super::hw::sizes::RENDER_SLOTS - BASE_RENDER_SLOTS;
const BASE_PB_SLOTS: usize = super::hw::sizes::BASE_PB_SLOTS;
const EXT_PB_SLOTS: usize = super::hw::sizes::PB_SLOTS - BASE_PB_SLOTS;
/// Single-submission render command count that expands extension slots immediately:
/// `BASE_RENDER_SLOTS` holds only one active render once displacement occupies a slot.
const RENDER_BURST_COMMANDS: usize = BASE_RENDER_SLOTS;

/// Device-wide hints read locklessly in `SUBMIT` to size per-VM extension backing.
#[derive(Copy, Clone, Default)]
pub(crate) struct PrepareHints {
    /// A base compute unit (`0..BASE_COMPUTE_UNITS`) is quarantined: allocate extension units.
    pub(crate) expand_compute_ext: bool,
    /// A base render pass slot (`0..BASE_RENDER_SLOTS`) is quarantined: allocate extension slots.
    pub(crate) expand_render_ext: bool,
    /// Contiguous PB/UMA allocator slots (`BASE_PB_SLOTS..=PB_SLOTS`) needed across quarantines.
    pub(crate) pb_slots: usize,
}

/// Updates the consecutive-compute burst counter and reports whether this submission
/// qualifies to allocate extension compute units (`compute_ext`).
///
/// If a base compute unit (`0..BASE_COMPUTE_UNITS`) has been permanently quarantined, fresh
/// single-command VMs must expand immediately so they can claim a healthy extension unit and
/// displace the remaining retired base unit rather than hitting `compute_exhausted_within(2)`.
fn should_expand_compute_ext(
    last_compute_ns: &mut u64,
    burst: &mut u32,
    now: u64,
    computes: usize,
    base_quarantined: bool,
) -> bool {
    *burst = if *last_compute_ns != 0 && now.wrapping_sub(*last_compute_ns) < BURST_GAP_NS {
        burst.saturating_add(1)
    } else {
        0
    };
    *last_compute_ns = now;
    base_quarantined || *burst >= BURST_SUBMITS || computes >= BURST_COMMANDS
}

/// Updates the consecutive-render burst counter and reports whether this submission
/// qualifies to allocate extension pass slots (`RenderExtArena`).
///
/// Steady 60/120 Hz single-pass depth-1 clients finish each frame before the next arrives
/// (`active_renders == 0`, `render_count == 1`, inter-frame gap >= `BURST_GAP_NS`) and stay
/// on the two base pass slots. Extension slots allocate when a base pass slot is quarantined,
/// a VM actually overlaps an in-flight render (`active_renders != 0`, including multi-ms
/// 1080p/4K pipelined frames), submits a multi-pass command stream
/// (`render_count >= BASE_RENDER_SLOTS`), or sustains a sub-2 ms back-to-back burst.
fn should_expand_render_ext(
    last_render_ns: &mut u64,
    render_burst: &mut u32,
    now: u64,
    render_count: usize,
    active_renders: u32,
    base_quarantined: bool,
) -> bool {
    *render_burst = if *last_render_ns != 0 && now.wrapping_sub(*last_render_ns) < BURST_GAP_NS {
        render_burst.saturating_add(1)
    } else {
        0
    };
    *last_render_ns = now;
    base_quarantined
        || active_renders != 0
        || *render_burst >= BURST_SUBMITS
        || render_count >= RENDER_BURST_COMMANDS
}

pub(crate) struct RenderArena(crate::cleanup::Deferred<RenderInner>);
impl core::ops::Deref for RenderArena {
    type Target = RenderInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct RenderInner {
    pub(crate) passes: KVec<PassSlot>,
    pub(crate) pools: PoolSet,
    pub(crate) tpc_dirty: [AtomicU64; BASE_RENDER_SLOTS],
    pub(crate) tilemap_dirty: [AtomicU64; BASE_RENDER_SLOTS],
    _objects: Objects,
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
impl RenderArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let mut passes = KVec::with_capacity(BASE_RENDER_SLOTS, GFP_KERNEL)?;
        for k in 0..BASE_RENDER_SLOTS {
            passes.push(PassSlot::new(&mut objects, k as u64, 1)?, GFP_KERNEL)?;
        }
        let pools = PoolSet::new(&mut objects)?;
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(RenderInner {
                passes,
                pools,
                tpc_dirty: [const { AtomicU64::new(0) }; BASE_RENDER_SLOTS],
                tilemap_dirty: [const { AtomicU64::new(0) }; BASE_RENDER_SLOTS],
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
    pub(crate) fn set_dirty(&self, k: usize, tpc: u64, tilemap: u64) -> Result {
        let pass = self.passes.get(k).ok_or(EINVAL)?;
        if tpc > pass.tpc.size as u64 || tilemap > pass.tilemap.size as u64 {
            return Err(E2BIG);
        }
        self.tpc_dirty[k].fetch_max(tpc, Ordering::Relaxed);
        self.tilemap_dirty[k].fetch_max(tilemap, Ordering::Relaxed);
        Ok(())
    }
}

/// Extension render pass slots (`BASE_RENDER_SLOTS..hw::sizes::RENDER_SLOTS`), allocated lazily
/// for VMs that submit renders back to back so idle or single-pass VMs keep only the two base
/// pass slots.
pub(crate) struct RenderExtArena(crate::cleanup::Deferred<RenderExtInner>);
impl core::ops::Deref for RenderExtArena {
    type Target = RenderExtInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct RenderExtInner {
    pub(crate) passes: KVec<PassSlot>,
    pub(crate) tpc_dirty: [AtomicU64; EXT_RENDER_SLOTS],
    pub(crate) tilemap_dirty: [AtomicU64; EXT_RENDER_SLOTS],
    _objects: Objects,
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
impl RenderExtArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let mut passes = KVec::with_capacity(EXT_RENDER_SLOTS, GFP_KERNEL)?;
        for k in BASE_RENDER_SLOTS..super::hw::sizes::RENDER_SLOTS {
            passes.push(PassSlot::new(&mut objects, k as u64, 1)?, GFP_KERNEL)?;
        }
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(RenderExtInner {
                passes,
                tpc_dirty: [const { AtomicU64::new(0) }; EXT_RENDER_SLOTS],
                tilemap_dirty: [const { AtomicU64::new(0) }; EXT_RENDER_SLOTS],
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
}

/// One lazily allocated recovery PB/UMA allocator slot (`BASE_PB_SLOTS..PB_SLOTS`),
/// sharing the VM's base PB heap so non-fault VMs pay zero extra allocator memory.
pub(crate) struct PoolExtArena(crate::cleanup::Deferred<PoolExtInner>);
impl core::ops::Deref for PoolExtArena {
    type Target = PoolExtInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct PoolExtInner {
    pub(crate) allocator: super::pools::Allocator,
    _objects: Objects,
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
impl PoolExtArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        slot: usize,
        heap: &Arc<super::object::FwObject>,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let allocator = super::pools::Allocator::new(&mut objects, slot as u64, heap)?;
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(PoolExtInner {
                allocator,
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
}

/// Keeps the arena owning a retained PB/UMA allocator slot alive through displacement.
/// The wrapped `Arc`s are held solely for their `Drop` lifetime and never read back.
#[derive(Clone)]
#[allow(dead_code)]
pub(crate) enum PbOwnerArena {
    Base(Arc<RenderArena>),
    Ext(Arc<RenderArena>, Arc<PoolExtArena>),
}

/// Keeps the arena owning a retained pass slot alive through displacement.
#[derive(Clone)]
pub(crate) enum PassOwnerArena {
    Base(Arc<RenderArena>),
    Ext(Arc<RenderExtArena>),
}

impl PassOwnerArena {
    /// Returns the pass slot and its tilemap dirty high-water mark for global slot `k`.
    pub(crate) fn pass_and_tilemap_dirty(&self, k: usize) -> Option<(&PassSlot, &AtomicU64)> {
        match self {
            Self::Base(arena) => Some((arena.passes.get(k)?, arena.tilemap_dirty.get(k)?)),
            Self::Ext(arena) => {
                let idx = k.checked_sub(BASE_RENDER_SLOTS)?;
                Some((arena.passes.get(idx)?, arena.tilemap_dirty.get(idx)?))
            }
        }
    }
}

#[derive(Default)]
struct Ready {
    compute: Option<Arc<ComputeArena>>,
    /// Units from [`BASE_COMPUTE_UNITS`] on, for a VM that keeps compute queued.
    compute_ext: Option<Arc<ComputeArena>>,
    render: Option<Arc<RenderArena>>,
    /// Pass slots from [`BASE_RENDER_SLOTS`] on, for a VM that keeps renders queued.
    render_ext: Option<Arc<RenderExtArena>>,
    /// Recovery PB/UMA allocator slots (`BASE_PB_SLOTS..PB_SLOTS`), allocated lazily after hangs.
    pb_ext: [Option<Arc<PoolExtArena>>; EXT_PB_SLOTS],
    /// Last compute submission and the back-to-back run it continues.
    last_compute_ns: u64,
    burst: u32,
    /// Last render submission and the back-to-back run it continues.
    last_render_ns: u64,
    render_burst: u32,
}
struct Standby {
    /// Never-exposed epochs kept on standby for this VM's queued renders.
    /// Invariant: while open or while `reserved > 0`, `epochs.len() >= reserved as usize`.
    epochs: KVec<Arc<PbEpoch>>,
    /// Accepted, not-yet-published render commands of this VM.
    reserved: u32,
    /// `SUBMIT` callers currently sleeping on `standby_available`.
    waiters: u32,
}

#[pin_data]
pub(crate) struct Cache {
    #[pin]
    ready: Mutex<Ready>,
    /// Shared per-VM standby pool of never-exposed PB epochs. Each holds the VM, which
    /// holds this cache: [`Self::close`] drains surplus epochs immediately and the remaining
    /// reserved epochs as accepted renders finish.
    #[pin]
    standby: SpinLockIrq<Standby>,
    /// Wakes `SUBMIT` callers waiting for standby reservation headroom.
    #[pin]
    standby_available: CondVar,
    /// The VM's client closed: no arena or surplus spare is kept here any more.
    closed: AtomicBool,
    /// Prepared render jobs on this VM that have not yet retired and dropped.
    active_renders: AtomicU32,
    pub(crate) factory: Factory,
    bind: mmu::VmBind,
    live: Arc<super::vm::LiveToken>,
    budget: Arc<super::budget::Budget>,
}
impl Cache {
    pub(crate) fn new(
        factory: Factory,
        bind: mmu::VmBind,
        live: Arc<super::vm::LiveToken>,
    ) -> Result<Arc<Self>> {
        let budget = super::budget::Budget::new()?;
        let epochs = KVec::with_capacity(super::budget::LIMIT as usize, GFP_KERNEL)?;
        Arc::pin_init(
            pin_init!(Self { ready <- new_mutex!(Ready::default(), "g16::resources"),
            standby <- new_spinlock_irq!(
                Standby {
                    epochs,
                    reserved: 0,
                    waiters: 0,
                },
                "g16::spare_epochs"
            ),
            standby_available <- new_condvar!(),
            closed: AtomicBool::new(false),
            active_renders: AtomicU32::new(0),
            factory, bind, live, budget }),
            GFP_KERNEL,
        )
    }

    /// Pops one never-exposed standby epoch for a fresh InitBM publication.
    fn take_standby(&self) -> Result<Arc<PbEpoch>> {
        let mut standby = self.standby.lock();
        while let Some(epoch) = standby.epochs.pop() {
            if epoch.unexposed() {
                return Ok(epoch);
            }
        }
        Err(EINVAL)
    }

    /// Returns a never-exposed epoch whose publication was deferred or failed before visibility.
    fn return_standby(&self, epoch: Arc<PbEpoch>) {
        if !epoch.unexposed() {
            return;
        }
        let overflow = {
            let mut standby = self.standby.lock();
            let keep = if self.closed.load(Ordering::Acquire) {
                standby.reserved as usize
            } else {
                super::budget::LIMIT as usize
            };
            if standby.epochs.len() < keep {
                standby.epochs.push_within_capacity(epoch).err().map(|e| e.0)
            } else {
                Some(epoch)
            }
        };
        drop(overflow);
    }

    /// Releases `count` reserved render credits and trims any surplus standby epochs outside
    /// the spinlock (each `PbEpoch` defers its own unmap, so dropping here never sleeps).
    fn release_reserved(&self, count: u32) {
        if count == 0 {
            return;
        }
        let (mut wake, excess) = {
            let mut standby = self.standby.lock();
            standby.reserved = standby.reserved.saturating_sub(count);
            let wake = standby.waiters != 0;
            let keep = if self.closed.load(Ordering::Acquire) {
                standby.reserved as usize
            } else {
                (standby.reserved as usize).max(SPARE_EPOCHS)
            };
            let excess = if standby.epochs.len() > keep {
                standby.epochs.pop()
            } else {
                None
            };
            (wake, excess)
        };
        if let Some(excess) = excess {
            drop(excess);
            // Subsequent iterations only trim remaining surplus; count was already subtracted.
            loop {
                let more = {
                    let mut standby = self.standby.lock();
                    wake |= standby.waiters != 0;
                    let keep = if self.closed.load(Ordering::Acquire) {
                        standby.reserved as usize
                    } else {
                        (standby.reserved as usize).max(SPARE_EPOCHS)
                    };
                    if standby.epochs.len() > keep {
                        standby.epochs.pop()
                    } else {
                        None
                    }
                };
                if more.is_none() {
                    break;
                }
                drop(more);
            }
        }
        if wake {
            self.standby_available.notify_all();
        }
    }

    /// The VM's client closed. Lets go of the arenas and surplus standby epochs: accepted
    /// renders keep only the exact standby count they reserved until they finish, and hardware
    /// slots hold their own last-owner leases.
    pub(crate) fn close(&self) {
        self.closed.store(true, Ordering::Release);
        loop {
            let excess = {
                let mut standby = self.standby.lock();
                let keep = standby.reserved as usize;
                if standby.epochs.len() > keep {
                    standby.epochs.pop()
                } else {
                    None
                }
            };
            if excess.is_none() {
                break;
            }
            drop(excess);
        }
        self.standby_available.notify_all();
        let ready = {
            let mut ready = self.ready.lock();
            (
                ready.compute.take(),
                ready.compute_ext.take(),
                ready.render.take(),
                ready.render_ext.take(),
                core::mem::take(&mut ready.pb_ext),
            )
        };
        drop(ready);
    }

    pub(crate) fn prepare(
        self: &Arc<Self>,
        vm: &Arc<VmState>,
        commands: &[Command],
        hints: PrepareHints,
    ) -> Result<Prepared> {
        let computes = commands
            .iter()
            .filter(|c| matches!(c, Command::Compute(_)))
            .count();
        let compute = computes != 0;
        let render_count: u32 = commands
            .iter()
            .filter(|c| matches!(c, Command::Render(_)))
            .count()
            .try_into()?;
        let render = render_count != 0;
        let (compute, compute_ext, render_arena, render_ext, pb_ext) = {
            // Never held by publication, completion, recovery, fence signalling or
            // standby waits, so Cache::close() and same-VM compute SUBMITs never block.
            let mut ready = self.ready.lock();
            if vm.status.get() != 0 || vm.vm.is_closed() || self.closed.load(Ordering::Acquire) {
                return Err(EIO);
            }
            if compute && ready.compute.is_none() {
                ready.compute = Some(ComputeArena::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                    0..BASE_COMPUTE_UNITS,
                )?);
            }
            if compute && ready.compute_ext.is_none() {
                let units = super::params::compute_units();
                if units > BASE_COMPUTE_UNITS {
                    // SAFETY: ktime_get has no preconditions.
                    let now = unsafe { kernel::bindings::ktime_get() } as u64;
                    let state = &mut *ready;
                    if should_expand_compute_ext(
                        &mut state.last_compute_ns,
                        &mut state.burst,
                        now,
                        computes,
                        hints.expand_compute_ext,
                    ) {
                        // Best effort: without them the VM keeps the base compute units.
                        state.compute_ext = ComputeArena::new(
                            &self.factory,
                            &self.bind,
                            &vm.vm,
                            &self.live,
                            BASE_COMPUTE_UNITS..units,
                        )
                        .ok();
                    }
                }
            }
            if render && ready.render.is_none() {
                ready.render = Some(RenderArena::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                )?);
            }
            if render {
                let target_pb = hints.pb_slots.clamp(BASE_PB_SLOTS, super::hw::sizes::PB_SLOTS);
                if target_pb > BASE_PB_SLOTS {
                    if let Some(base) = ready.render.as_ref() {
                        let heap = base.pools.heap().clone();
                        for k in BASE_PB_SLOTS..target_pb {
                            let idx = k - BASE_PB_SLOTS;
                            if ready.pb_ext[idx].is_none() {
                                // Best effort: without recovery slots, keep prior PB slots.
                                ready.pb_ext[idx] = PoolExtArena::new(
                                    &self.factory,
                                    &self.bind,
                                    &vm.vm,
                                    &self.live,
                                    k,
                                    &heap,
                                )
                                .ok();
                                if ready.pb_ext[idx].is_none() {
                                    break;
                                }
                            }
                        }
                    }
                }
                if ready.render_ext.is_none() && EXT_RENDER_SLOTS != 0 {
                    // SAFETY: ktime_get has no preconditions.
                    let now = unsafe { kernel::bindings::ktime_get() } as u64;
                    let state = &mut *ready;
                    let active = self.active_renders.load(Ordering::Acquire);
                    if should_expand_render_ext(
                        &mut state.last_render_ns,
                        &mut state.render_burst,
                        now,
                        render_count as usize,
                        active,
                        hints.expand_render_ext,
                    ) {
                        // Best effort: without extension slots the VM keeps two base pass slots.
                        state.render_ext =
                            RenderExtArena::new(&self.factory, &self.bind, &vm.vm, &self.live).ok();
                    }
                }
            }
            let pb_ext = if render {
                core::array::from_fn(|idx| ready.pb_ext[idx].clone())
            } else {
                [const { None }; EXT_PB_SLOTS]
            };
            (
                if compute { ready.compute.clone() } else { None },
                if compute { ready.compute_ext.clone() } else { None },
                if render { ready.render.clone() } else { None },
                if render { ready.render_ext.clone() } else { None },
                pb_ext,
            )
        };
        if render {
            if render_count > super::budget::LIMIT {
                return Err(E2BIG);
            }
            let mut remaining = kernel::time::msecs_to_jiffies(10_000);
            loop {
                let deficit = {
                    let mut standby = self.standby.lock();
                    if vm.status.get() != 0
                        || vm.vm.is_closed()
                        || self.closed.load(Ordering::Acquire)
                    {
                        return Err(EIO);
                    }
                    if let Some(needed) = standby
                        .reserved
                        .checked_add(render_count)
                        .filter(|&n| n <= super::budget::LIMIT)
                    {
                        if standby.epochs.len() >= needed as usize {
                            standby.reserved = needed;
                            break;
                        }
                        needed - standby.epochs.len() as u32
                    } else {
                        standby.waiters += 1;
                        let waited = self
                            .standby_available
                            .wait_interruptible_timeout(&mut standby, remaining);
                        standby.waiters -= 1;
                        match waited {
                            CondVarTimeoutResult::Woken { jiffies } => {
                                remaining = jiffies;
                                continue;
                            }
                            CondVarTimeoutResult::Signal { .. } => return Err(ERESTARTSYS),
                            CondVarTimeoutResult::Timeout => {
                                if vm.status.get() != 0
                                    || vm.vm.is_closed()
                                    || self.closed.load(Ordering::Acquire)
                                {
                                    return Err(EIO);
                                }
                                let needed = standby
                                    .reserved
                                    .checked_add(render_count)
                                    .filter(|&n| n <= super::budget::LIMIT)
                                    .ok_or(EBUSY)?;
                                if standby.epochs.len() >= needed as usize {
                                    standby.reserved = needed;
                                    break;
                                }
                                needed - standby.epochs.len() as u32
                            }
                        }
                    }
                };
                let mut epochs = super::budget::Budget::reserve(&self.budget, deficit)?;
                let mut fresh = KVec::with_capacity(deficit as usize, GFP_KERNEL)?;
                for _ in 0..deficit {
                    fresh.push(PbEpoch::new(&self.factory, vm, epochs.take()?)?, GFP_KERNEL)?;
                }
                let mut overflow = KVec::with_capacity(fresh.len(), GFP_KERNEL)?;
                {
                    let mut standby = self.standby.lock();
                    let closed = self.closed.load(Ordering::Acquire);
                    for epoch in fresh {
                        if closed {
                            let _ = overflow.push_within_capacity(epoch);
                        } else if let Err(err) = standby.epochs.push_within_capacity(epoch) {
                            let _ = overflow.push_within_capacity(err.0);
                        }
                    }
                }
                drop(overflow);
            }
        }
        // A concurrent fault or close retains even newly constructed backing in this VM cache.
        if vm.status.get() != 0 || vm.vm.is_closed() || self.closed.load(Ordering::Acquire) {
            if render {
                self.release_reserved(render_count);
            }
            return Err(EIO);
        }
        let cache = if render_count != 0 {
            self.active_renders.fetch_add(1, Ordering::AcqRel);
            Some(self.clone())
        } else {
            None
        };
        Ok(Prepared {
            compute,
            compute_ext,
            render: render_arena,
            render_ext,
            pb_ext,
            unpublished_renders: AtomicU32::new(render_count),
            cache,
        })
    }
}

/// Immutable references carried by an accepted job. No fallible lazy work in the engine.
pub(crate) struct Prepared {
    pub(crate) compute: Option<Arc<ComputeArena>>,
    /// The VM's extension compute units when the job was prepared.
    pub(crate) compute_ext: Option<Arc<ComputeArena>>,
    pub(crate) render: Option<Arc<RenderArena>>,
    /// The VM's extension render pass slots when the job was prepared.
    pub(crate) render_ext: Option<Arc<RenderExtArena>>,
    /// The VM's lazily allocated recovery PB/UMA allocator slots when the job was prepared.
    pub(crate) pb_ext: [Option<Arc<PoolExtArena>>; EXT_PB_SLOTS],
    /// Render commands of this job whose standby reservation has not yet been committed.
    unpublished_renders: AtomicU32,
    /// The VM cache holding the standby PB epoch pool.
    cache: Option<Arc<Cache>>,
}

impl Drop for Prepared {
    fn drop(&mut self) {
        let Some(cache) = self.cache.take() else {
            return;
        };
        let previous = cache.active_renders.fetch_sub(1, Ordering::AcqRel);
        debug_assert!(previous != 0);
        let remaining = self.unpublished_renders.swap(0, Ordering::AcqRel);
        cache.release_reserved(remaining);
    }
}
impl Prepared {
    pub(crate) fn compute(&self) -> Result<&ComputeArena> {
        self.compute.as_deref().ok_or(EINVAL)
    }
    /// Compute units this job can use.
    pub(crate) fn compute_units(&self) -> Result<usize> {
        Ok(self.compute()?.compute.len()
            + self.compute_ext.as_deref().map_or(0, |e| e.compute.len()))
    }
    /// The arena holding compute unit `k`.
    pub(crate) fn compute_arena(&self, k: usize) -> Option<Arc<ComputeArena>> {
        let base = self.compute.as_ref()?;
        if k < base.compute.len() {
            Some(base.clone())
        } else {
            self.compute_ext.clone()
        }
    }
    /// Contiguous PB/UMA allocator slots (`0..n`) this job can use.
    pub(crate) fn pb_slots(&self) -> Result<usize> {
        let _ = self.render()?;
        let ext = self.pb_ext.iter().take_while(|slot| slot.is_some()).count();
        Ok(BASE_PB_SLOTS + ext)
    }
    fn pb_allocator(&self, k: usize) -> Result<&super::pools::Allocator> {
        let base = self.render()?;
        if k < BASE_PB_SLOTS {
            base.pools.unit(k).ok_or(EINVAL)
        } else {
            self.pb_ext
                .get(k - BASE_PB_SLOTS)
                .and_then(|slot| slot.as_deref())
                .map(|arena| &arena.allocator)
                .ok_or(EINVAL)
        }
    }
    pub(crate) fn prepare_pb_in_flight(
        &self,
        k: usize,
        draw: u64,
        pipelined: bool,
    ) -> Result<super::pools::AllocatorRefs> {
        self.pb_allocator(k)?.prepare_in_flight(draw, pipelined)
    }
    pub(crate) fn pb_allocator_refs(&self, k: usize) -> Result<super::pools::AllocatorRefs> {
        self.pb_allocator(k)?.refs()
    }
    pub(crate) fn publish_pb_batch(&self, k: usize, submitted: u64, renders: u32) -> Result {
        self.pb_allocator(k)?.publish_batch(submitted, renders)
    }
    pub(crate) fn pb_retired(&self, k: usize) -> Result<bool> {
        self.pb_allocator(k)?.retired()
    }
    pub(crate) fn pb_retired_at(&self, k: usize, target: u64) -> Result<bool> {
        self.pb_allocator(k)?.retired_at(target)
    }
    pub(crate) fn pb_heap_gva(&self, k: usize) -> Result<u64> {
        self.pb_allocator(k)?.heap_gva()
    }
    pub(crate) fn pb_heap_blocks(&self) -> Result<u32> {
        Ok(self.render()?.pools.heap_blocks())
    }
    /// The arena owning PB/UMA allocator slot `k`.
    pub(crate) fn pb_owner_arena(&self, k: usize) -> Option<PbOwnerArena> {
        let base = self.render.as_ref()?;
        if k < BASE_PB_SLOTS {
            Some(PbOwnerArena::Base(base.clone()))
        } else {
            self.pb_ext
                .get(k - BASE_PB_SLOTS)
                .and_then(|slot| slot.as_ref())
                .map(|ext| PbOwnerArena::Ext(base.clone(), ext.clone()))
        }
    }
    /// Render pass slots this job can use.
    pub(crate) fn render_passes(&self) -> Result<usize> {
        Ok(self.render()?.passes.len()
            + self.render_ext.as_deref().map_or(0, |e| e.passes.len()))
    }
    /// Render pass slot `k`: the VM's base slots, then its extension slots.
    pub(crate) fn render_pass(&self, k: usize) -> Result<&PassSlot> {
        let base = &self.render()?.passes;
        match base.get(k) {
            Some(pass) => Ok(pass),
            None => self
                .render_ext
                .as_deref()
                .and_then(|e| e.passes.get(k - base.len()))
                .ok_or(EINVAL),
        }
    }
    /// The dirty high-water marks (`tpc_dirty`, `tilemap_dirty`) for pass slot `k`.
    pub(crate) fn render_pass_dirty(&self, k: usize) -> Result<(&AtomicU64, &AtomicU64)> {
        let base = self.render()?;
        if k < base.passes.len() {
            Ok((&base.tpc_dirty[k], &base.tilemap_dirty[k]))
        } else {
            let ext = self.render_ext.as_deref().ok_or(EINVAL)?;
            let idx = k - base.passes.len();
            if idx >= ext.passes.len() {
                return Err(EINVAL);
            }
            Ok((&ext.tpc_dirty[idx], &ext.tilemap_dirty[idx]))
        }
    }
    /// Updates dirty high-water marks for pass slot `k` (base or extension).
    pub(crate) fn set_pass_dirty(&self, k: usize, tpc: u64, tilemap: u64) -> Result {
        let pass = self.render_pass(k)?;
        if tpc > pass.tpc.size as u64 || tilemap > pass.tilemap.size as u64 {
            return Err(E2BIG);
        }
        let (tpc_dirty, tilemap_dirty) = self.render_pass_dirty(k)?;
        tpc_dirty.fetch_max(tpc, Ordering::Relaxed);
        tilemap_dirty.fetch_max(tilemap, Ordering::Relaxed);
        Ok(())
    }
    /// The arena owning render pass slot `k`.
    pub(crate) fn pass_owner_arena(&self, k: usize) -> Option<PassOwnerArena> {
        let base = self.render.as_ref()?;
        if k < base.passes.len() {
            Some(PassOwnerArena::Base(base.clone()))
        } else {
            self.render_ext.as_ref().map(|e| PassOwnerArena::Ext(e.clone()))
        }
    }
    /// Compute unit `k`: the VM's base units, then its extension units.
    pub(crate) fn compute_unit(&self, k: usize) -> Result<&ComputeSlot> {
        let base = &self.compute()?.compute;
        match base.get(k) {
            Some(unit) => Ok(unit),
            None => self
                .compute_ext
                .as_deref()
                .and_then(|e| e.compute.get(k - base.len()))
                .ok_or(EINVAL),
        }
    }
    pub(crate) fn render(&self) -> Result<&RenderArena> {
        self.render.as_deref().ok_or(EINVAL)
    }
    pub(crate) fn take_pb(&self) -> Result<Arc<PbEpoch>> {
        self.cache.as_ref().ok_or(EINVAL)?.take_standby()
    }
    pub(crate) fn return_pb(&self, epoch: Arc<PbEpoch>) {
        if let Some(cache) = &self.cache {
            cache.return_standby(epoch);
        }
    }
    /// Commits one render command's standby reservation once it reaches firmware visibility.
    pub(crate) fn commit_render(&self) {
        if let Some(cache) = &self.cache {
            if self
                .unpublished_renders
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_sub(1))
                .is_ok()
            {
                cache.release_reserved(1);
            }
        }
    }
}
