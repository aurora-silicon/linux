// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright (C) 2026 Eryk Wieliczko

//! Lazy per-VM command backing. Allocation runs only in SUBMIT, before accepting a fence.
//! The completion path reads prepared references and never takes the construction mutex.
//! Normal-NC maps are reclaimed only after all physical-slot leases have been displaced.
//!
//! Shader-context scratch (ray queries) takes its pages from the UMA pool of the unit that
//! runs it, on top of the pool's base pages ([`super::rt::required_seeds`]). A job with
//! scratch therefore runs on SCS-capable storage, created with its first such job: two
//! compute units ([`Placement::Scs`]) for its compute commands, and a second set of render
//! allocators ([`PoolSet::scs`]) for its renders. Jobs without scratch keep the base
//! storage, bytes and addresses unchanged.

use super::{
    command::Command,
    object::{Factory, Objects},
    pools::{PbEpoch, PoolSet},
    slots::{ComputeSlot, PassSlot, Placement},
    vm::VmState,
};
use crate::mmu;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use kernel::{
    new_mutex, new_spinlock,
    prelude::*,
    sync::{Arc, Mutex, SetOnce, SpinLock},
};

/// Never-exposed PB epochs a VM keeps for its next renders. With PB epoch reuse most
/// renders never expose the epoch prepared for them; allocating and mapping a fresh one per
/// render submit costs far more than the render itself.
const SPARE_EPOCHS: usize = 4;

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
    /// Units `units` (their index and `placement` fix their client addresses; the index fixes
    /// their hardware UMA slot).
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        units: core::ops::Range<usize>,
        placement: Placement,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let mut compute = KVec::with_capacity(units.len(), GFP_KERNEL)?;
        for k in units {
            compute.push(
                ComputeSlot::new_in(&mut objects, k as u64, placement)?,
                GFP_KERNEL,
            )?;
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
/// The SCS-capable render allocators of a render arena and the mappings they own.
pub(crate) struct ScsPools {
    pub(crate) pools: PoolSet,
    _objects: Objects,
}

pub(crate) struct RenderInner {
    pub(crate) passes: KVec<PassSlot>,
    pub(crate) pools: PoolSet,
    /// [`PoolSet::scs`], with the VM's first render that has scratch. Held with the arena, so
    /// a pass slot's owner lease keeps it too. Dropped before the arena's own mappings.
    pub(crate) scs: SetOnce<ScsPools>,
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
                scs: SetOnce::new(),
                tpc_dirty: [const { AtomicU64::new(0) }; 2],
                tilemap_dirty: [const { AtomicU64::new(0) }; 2],
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
    /// Creates the SCS-capable render allocators unless they exist. Called under the VM
    /// cache's construction mutex, before any job uses them.
    fn ensure_scs(&self, factory: &Factory, vm: &mmu::Vm) -> Result {
        if self.scs.as_ref().is_some() {
            return Ok(());
        }
        let mut objects = factory.objects(vm);
        let pools = self.pools.scs(&mut objects)?;
        objects.finish_private()?;
        // The construction mutex excludes a second populate.
        if !self.scs.populate(ScsPools {
            pools,
            _objects: objects,
        }) {
            return Err(EBUSY);
        }
        Ok(())
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
    /// SCS-capable units, for jobs with compute scratch.
    compute_scs: Option<Arc<ComputeArena>>,
    render: Option<Arc<RenderArena>>,
    /// Last compute submission and the back-to-back run it continues.
    last_compute_ns: u64,
    burst: u32,
}
#[pin_data]
pub(crate) struct Cache {
    #[pin]
    ready: Mutex<Ready>,
    /// Never-exposed epochs returned by retired jobs (at most [`SPARE_EPOCHS`]). Each holds
    /// the VM, which holds this cache: [`Self::close`] breaks that cycle.
    #[pin]
    spares: SpinLock<KVec<Arc<PbEpoch>>>,
    /// The VM's client closed: no arena or spare is kept here any more.
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
        let spares = KVec::with_capacity(SPARE_EPOCHS, GFP_KERNEL)?;
        Arc::pin_init(
            pin_init!(Self { ready <- new_mutex!(Ready::default(), "g16::resources"),
            spares <- new_spinlock!(spares, "g16::spare_epochs"),
            closed: AtomicBool::new(false),
            factory, bind, live, budget }),
            GFP_KERNEL,
        )
    }
    /// Returns a retired job's never-exposed epoch for the next renders. Any context.
    fn give_back(&self, epoch: Arc<PbEpoch>) {
        let mut spares = self.spares.lock();
        // Without room, or once the client closed, the epoch drops: its deferred cleanup
        // unmaps it as before.
        if !self.closed.load(Ordering::Acquire) {
            let _ = spares.push_within_capacity(epoch);
        }
    }

    /// The VM's client closed. Lets go of the arenas and spare epochs: what jobs and the
    /// hardware slots' last-owner leases still need, they hold themselves.
    pub(crate) fn close(&self) {
        let spares = {
            let mut spares = self.spares.lock();
            self.closed.store(true, Ordering::Release);
            core::mem::take(&mut *spares)
        };
        drop(spares);
        let ready = {
            let mut ready = self.ready.lock();
            (
                ready.compute.take(),
                ready.compute_ext.take(),
                ready.compute_scs.take(),
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
        let render_count = commands
            .iter()
            .filter(|c| matches!(c, Command::Render(_)))
            .count();
        let render = render_count != 0;
        // Scratch is sized on top of a pool's base pages: such a job runs on SCS-capable
        // storage, for all its commands of that engine.
        let compute_scs = commands
            .iter()
            .any(|c| matches!(c, Command::Compute(_)) && c.scs());
        let render_scs = commands
            .iter()
            .any(|c| matches!(c, Command::Render(_)) && c.scs());
        let mut epochs = super::budget::Budget::reserve(&self.budget, render_count.try_into()?)?;
        let (compute, compute_ext, render) = {
            // Never held by publication, completion, recovery or fence signalling.
            let mut ready = self.ready.lock();
            if vm.status.get() != 0 || vm.vm.is_closed() || self.closed.load(Ordering::Acquire) {
                return Err(EIO);
            }
            if compute && !compute_scs && ready.compute.is_none() {
                ready.compute = Some(ComputeArena::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                    0..BASE_COMPUTE_UNITS,
                    Placement::Base,
                )?);
            }
            if compute_scs && ready.compute_scs.is_none() {
                ready.compute_scs = Some(ComputeArena::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                    0..super::hw::sizes::SCS_COMPUTE_UNITS,
                    Placement::Scs,
                )?);
            }
            if compute && !compute_scs {
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
                        Placement::Base,
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
            if render_scs {
                ready
                    .render
                    .as_ref()
                    .ok_or(EINVAL)?
                    .ensure_scs(&self.factory, &vm.vm)?;
            }
            // A concurrent fault retains even newly constructed backing in this VM cache.
            if vm.status.get() != 0 || vm.vm.is_closed() {
                return Err(EIO);
            }
            (
                match (compute, compute_scs) {
                    (true, true) => ready.compute_scs.clone(),
                    (true, false) => ready.compute.clone(),
                    (false, _) => None,
                },
                if compute && !compute_scs {
                    ready.compute_ext.clone()
                } else {
                    None
                },
                if render { ready.render.clone() } else { None },
            )
        };
        let mut pb = KVec::with_capacity(commands.len(), GFP_KERNEL)?;
        for command in commands {
            pb.push(
                if matches!(command, Command::Render(_)) {
                    // A spare keeps the budget permit it was created with; the reservation
                    // returns the unused credit when it drops.
                    let spare = self.spares.lock().pop();
                    Some(match spare {
                        Some(epoch) if epoch.unexposed() => epoch,
                        _ => PbEpoch::new(&self.factory, vm, epochs.take()?)?,
                    })
                } else {
                    None
                },
                GFP_KERNEL,
            )?;
        }
        if vm.status.get() != 0 || vm.vm.is_closed() {
            return Err(EIO);
        }
        Ok(Prepared {
            compute,
            compute_ext,
            render,
            render_scs,
            pb,
            cache: Some(self.clone()),
        })
    }
}

/// Immutable references carried by an accepted job. No fallible lazy work in the engine.
pub(crate) struct Prepared {
    pub(crate) compute: Option<Arc<ComputeArena>>,
    /// The VM's extension compute units when the job was prepared.
    pub(crate) compute_ext: Option<Arc<ComputeArena>>,
    pub(crate) render: Option<Arc<RenderArena>>,
    /// The job's renders use the arena's SCS-capable allocators ([`Self::render_pools`]).
    render_scs: bool,
    pb: KVec<Option<Arc<PbEpoch>>>,
    /// The VM cache that unused epochs return to.
    cache: Option<Arc<Cache>>,
}

impl Drop for Prepared {
    fn drop(&mut self) {
        let Some(cache) = self.cache.take() else {
            return;
        };
        for epoch in self.pb.iter_mut().filter_map(Option::take) {
            if epoch.unexposed() {
                cache.give_back(epoch);
            }
        }
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
    /// The render allocators of the job: the arena's SCS-capable set for a job with render
    /// scratch, otherwise its base set.
    pub(crate) fn render_pools(&self) -> Result<&PoolSet> {
        let render = self.render()?;
        if self.render_scs {
            render.scs.as_ref().map(|scs| &scs.pools).ok_or(EINVAL)
        } else {
            Ok(&render.pools)
        }
    }
    pub(crate) fn pb(&self, index: usize) -> Result<&Arc<PbEpoch>> {
        self.pb.get(index).and_then(Option::as_ref).ok_or(EINVAL)
    }
}
