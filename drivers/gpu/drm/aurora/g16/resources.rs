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
use core::sync::atomic::{AtomicU64, Ordering};
use kernel::{
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
};

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
impl ComputeArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let mut compute = KVec::with_capacity(2, GFP_KERNEL)?;
        for k in 0..2 {
            compute.push(ComputeSlot::new(&mut objects, k)?, GFP_KERNEL)?;
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
    render: Option<Arc<RenderArena>>,
}
#[pin_data]
pub(crate) struct Cache {
    #[pin]
    ready: Mutex<Ready>,
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
        Arc::pin_init(
            pin_init!(Self { ready <- new_mutex!(Ready::default(), "g16::resources"),
            factory, bind, live, budget }),
            GFP_KERNEL,
        )
    }
    pub(crate) fn prepare(&self, vm: &Arc<VmState>, commands: &[Command]) -> Result<Prepared> {
        let compute = commands.iter().any(|c| matches!(c, Command::Compute(_)));
        let render_count = commands
            .iter()
            .filter(|c| matches!(c, Command::Render(_)))
            .count();
        let render = render_count != 0;
        let mut epochs = super::budget::Budget::reserve(&self.budget, render_count.try_into()?)?;
        let (compute, render) = {
            // Never held by publication, completion, recovery or fence signalling.
            let mut ready = self.ready.lock();
            if vm.status.get() != 0 || vm.vm.is_closed() {
                return Err(EIO);
            }
            if compute && ready.compute.is_none() {
                ready.compute = Some(ComputeArena::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                )?);
            }
            if render && ready.render.is_none() {
                ready.render = Some(RenderArena::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                )?);
            }
            // A concurrent fault retains even newly constructed backing in this VM cache.
            if vm.status.get() != 0 || vm.vm.is_closed() {
                return Err(EIO);
            }
            (
                if compute { ready.compute.clone() } else { None },
                if render { ready.render.clone() } else { None },
            )
        };
        let mut pb = KVec::with_capacity(commands.len(), GFP_KERNEL)?;
        for command in commands {
            pb.push(
                if matches!(command, Command::Render(_)) {
                    Some(PbEpoch::new(&self.factory, vm, epochs.take()?)?)
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
            render,
            pb,
        })
    }
}

/// Immutable references carried by an accepted job. No fallible lazy work in the engine.
pub(crate) struct Prepared {
    pub(crate) compute: Option<Arc<ComputeArena>>,
    pub(crate) render: Option<Arc<RenderArena>>,
    pb: KVec<Option<Arc<PbEpoch>>>,
}
impl Prepared {
    pub(crate) fn compute(&self) -> Result<&ComputeArena> {
        self.compute.as_deref().ok_or(EINVAL)
    }
    pub(crate) fn render(&self) -> Result<&RenderArena> {
        self.render.as_deref().ok_or(EINVAL)
    }
    pub(crate) fn pb(&self, index: usize) -> Result<&Arc<PbEpoch>> {
        self.pb.get(index).and_then(Option::as_ref).ok_or(EINVAL)
    }
}
