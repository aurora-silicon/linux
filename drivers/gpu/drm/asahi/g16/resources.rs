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
    pub(crate) tpc_dirty: [AtomicU64; 2],
    pub(crate) tilemap_dirty: [AtomicU64; 2],
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
        let mut passes = KVec::with_capacity(2, GFP_KERNEL)?;
        for k in 0..2 {
            passes.push(PassSlot::new(&mut objects, k, 1)?, GFP_KERNEL)?;
        }
        let pools = PoolSet::new(&mut objects)?;
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(RenderInner {
                passes,
                pools,
                tpc_dirty: [const { AtomicU64::new(0) }; 2],
                tilemap_dirty: [const { AtomicU64::new(0) }; 2],
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

#[derive(Default)]
struct Ready {
    compute: Option<Arc<ComputeArena>>,
    /// Units from [`BASE_COMPUTE_UNITS`] on, for a VM that keeps compute queued.
    compute_ext: Option<Arc<ComputeArena>>,
    render: Option<Arc<RenderArena>>,
    /// Last compute submission and the back-to-back run it continues.
    last_compute_ns: u64,
    burst: u32,
}
struct Standby {
    /// Never-exposed epochs kept on standby for this VM's queued renders.
    /// Invariant: while open or while `reserved > 0`, `epochs.len() >= reserved as usize`.
    epochs: KVec<Arc<PbEpoch>>,
    /// Accepted, not-yet-published render commands of this VM.
    reserved: u32,
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
            standby <- new_spinlock_irq!(Standby { epochs, reserved: 0 }, "g16::spare_epochs"),
            standby_available <- new_condvar!(),
            closed: AtomicBool::new(false),
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
        loop {
            let excess = {
                let mut standby = self.standby.lock();
                standby.reserved = standby.reserved.saturating_sub(count);
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
            if excess.is_none() {
                break;
            }
            drop(excess);
            // Subsequent iterations only trim remaining surplus; count was already subtracted.
            loop {
                let more = {
                    let mut standby = self.standby.lock();
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
            break;
        }
        self.standby_available.notify_all();
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
            )
        };
        drop(ready);
    }

    pub(crate) fn prepare(
        self: &Arc<Self>,
        vm: &Arc<VmState>,
        commands: &[Command],
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
        let (compute, compute_ext, render) = {
            // Never held by publication, completion, recovery or fence signalling.
            // Serializing standby top-up under `ready` prevents concurrent SUBMITs on the
            // same VM from over-allocating or racing each other's deficit calculation.
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
            if compute {
                // SAFETY: ktime_get has no preconditions.
                let now = unsafe { kernel::bindings::ktime_get() } as u64;
                ready.burst = if now.wrapping_sub(ready.last_compute_ns) < BURST_GAP_NS {
                    ready.burst.saturating_add(1)
                } else {
                    0
                };
                ready.last_compute_ns = now;
                let units = super::params::compute_units();
                if ready.compute_ext.is_none()
                    && units > BASE_COMPUTE_UNITS
                    && (ready.burst >= BURST_SUBMITS || computes >= BURST_COMMANDS)
                {
                    // Best effort: without them the VM keeps one command in flight.
                    ready.compute_ext = ComputeArena::new(
                        &self.factory,
                        &self.bind,
                        &vm.vm,
                        &self.live,
                        BASE_COMPUTE_UNITS..units,
                    )
                    .ok();
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
                            match self
                                .standby_available
                                .wait_interruptible_timeout(&mut standby, remaining)
                            {
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
            // A concurrent fault retains even newly constructed backing in this VM cache.
            if vm.status.get() != 0 || vm.vm.is_closed() {
                if render {
                    self.release_reserved(render_count);
                }
                return Err(EIO);
            }
            (
                if compute { ready.compute.clone() } else { None },
                if compute { ready.compute_ext.clone() } else { None },
                if render { ready.render.clone() } else { None },
            )
        };
        Ok(Prepared {
            compute,
            compute_ext,
            render,
            unpublished_renders: AtomicU32::new(render_count),
            cache: render.is_some().then(|| self.clone()),
        })
    }
}

/// Immutable references carried by an accepted job. No fallible lazy work in the engine.
pub(crate) struct Prepared {
    pub(crate) compute: Option<Arc<ComputeArena>>,
    /// The VM's extension compute units when the job was prepared.
    pub(crate) compute_ext: Option<Arc<ComputeArena>>,
    pub(crate) render: Option<Arc<RenderArena>>,
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
    /// The render arena.
    pub(crate) fn render_arena(&self) -> Option<Arc<RenderArena>> {
        self.render.clone()
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
