// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The objects a render pair maps in its client's M5 compact window.
//!
//! The firmware addresses the window with 32-bit offsets, so it is 4 GiB, and every render pair
//! of the client maps its objects there. [`PairObject`] names each such object and fixes how it
//! is placed: every mapping of one goes through [`PairObject::map`] (or
//! [`PairObject::placement`]), and every room check before allocating one through
//! [`PairObject::check_room`]. SUBMIT's bound ([`PairSizes::window_bytes`]) counts the same
//! objects with the same placement, so an object cannot be mapped there without a size in the
//! bound, and the bound cannot disagree with a mapping on its alignment or guard.

use super::BlockGeometry;
use crate::g17::object::{KernelObject, Placement};
use crate::{
    hw::t8140::resources as cfg,
    mmu,
};
use core::ops::Range;
use kernel::prelude::*;

/// The client's M5 compact window.
pub(crate) const WINDOW: Range<u64> = BlockGeometry::M5_COMPACT;
/// Its size: 4 GiB.
pub(crate) const WINDOW_BYTES: u64 = WINDOW.end - WINDOW.start;

/// The task-stage mesh storage chunk: chunk bases are 64 KiB aligned, and the storage register
/// counts 64 KiB chunks.
pub(crate) const MESH_STORAGE_CHUNK: usize = 0x10000;

/// The task-stage mesh storage a render pair maps on its first task-stage kick and keeps, as
/// (chunks, bytes); `None` when the module parameter disables the binding.
pub(crate) fn mesh_storage() -> Result<Option<(u32, usize)>> {
    let chunks = crate::module_parameters::m5_mesh_storage_chunks.value();
    if chunks == 0 {
        return Ok(None);
    }
    if chunks > 0xffff {
        return Err(EINVAL);
    }
    let bytes = (chunks as usize).checked_mul(MESH_STORAGE_CHUNK).ok_or(EOVERFLOW)?;
    Ok(Some((chunks, bytes)))
}

/// Bytes of a pair's parameter-buffer scene scratch for `geometry`.
pub(crate) fn scratch_bytes(geometry: BlockGeometry) -> Result<usize> {
    use crate::g17::fw::m5::pool;
    pool::parameter_scratch_stride(geometry.capacity, 0)
        .map_err(|_| EINVAL)?
        .checked_mul(pool::SCENE_TABLE_ENTRIES)
        .ok_or(EOVERFLOW)
}

/// Bytes of the page list of a pair's block manager for `geometry`.
pub(crate) fn page_list_bytes(geometry: BlockGeometry) -> Result<usize> {
    Ok(super::layout::Layout::m5(geometry)?.page_bytes)
}

/// Bytes of a pair's metrics entry for `geometry`.
fn metrics_bytes(geometry: BlockGeometry) -> Result<usize> {
    super::MetricsIds::entry_pitch(super::layout::Layout::m5(geometry)?.metric_stride)
}

/// An object a render pair maps in its client's M5 compact window.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PairObject {
    /// The target records: one record stride for each command slot.
    Records = 0,
    /// The TPC object.
    Tpc,
    /// One parameter block, initial or grown, mapped at the M5 block stride.
    ParameterBlock,
    /// The context state of one command slot.
    SlotState,
    /// The parameter buffer's scene scratch.
    Scratch,
    /// The parameter buffer's discard.
    Discard,
    /// The block manager's page list.
    PageList,
    /// The pair's metrics entry.
    Metrics,
    /// The task-stage mesh storage, mapped on the pair's first task-stage kick and kept for the
    /// pair's lifetime.
    MeshStorage,
}

impl PairObject {
    /// Every object, in declaration order.
    pub(crate) const ALL: [Self; 9] = [
        Self::Records,
        Self::Tpc,
        Self::ParameterBlock,
        Self::SlotState,
        Self::Scratch,
        Self::Discard,
        Self::PageList,
        Self::Metrics,
        Self::MeshStorage,
    ];

    /// The alignment it is placed at.
    pub(crate) const fn align(self) -> u64 {
        match self {
            Self::MeshStorage => MESH_STORAGE_CHUNK as u64,
            Self::Metrics => mmu::UAT_PGSZ as u64,
            Self::ParameterBlock => BlockGeometry::PAGE as u64,
            Self::Records
            | Self::Tpc
            | Self::SlotState
            | Self::Scratch
            | Self::Discard
            | Self::PageList => cfg::TVB_PAGE,
        }
    }

    /// The unmapped guard that follows it.
    pub(crate) const fn guard(self) -> usize {
        match self {
            Self::Metrics => 0,
            Self::ParameterBlock => BlockGeometry::M5_BLOCK_STRIDE - BlockGeometry::M5_BLOCK_BYTES,
            Self::Records
            | Self::Tpc
            | Self::SlotState
            | Self::Scratch
            | Self::Discard
            | Self::PageList
            | Self::MeshStorage => mmu::UAT_PGSZ,
        }
    }

    /// The bytes one mapping of an object of `bytes` takes in one hole: its pages and its guard.
    fn mapped_bytes(self, bytes: usize) -> Result<usize> {
        bytes
            .checked_next_multiple_of(mmu::UAT_PGSZ)
            .and_then(|n| n.checked_add(self.guard()))
            .ok_or(EOVERFLOW)
    }

    /// The window bytes an object of `bytes` can take: its pages and its guard, rounded up to
    /// its alignment and at least to the 32 KiB most of these objects are placed at, plus, for
    /// an object aligned beyond 32 KiB, the hole that alignment can leave before it.
    pub(crate) fn window_bytes(self, bytes: usize) -> Result<usize> {
        let base = cfg::TVB_PAGE as usize;
        let align = (self.align() as usize).max(base);
        self.mapped_bytes(bytes)?
            .checked_next_multiple_of(align)
            .and_then(|n| n.checked_add(align - base))
            .ok_or(EOVERFLOW)
    }

    /// Maps `source` of `object` into `vm`'s window as this object.
    pub(crate) fn map(
        self,
        object: &KernelObject,
        vm: &mmu::Vm,
        source: Range<usize>,
        prot: mmu::Prot,
    ) -> Result<mmu::KernelMapping> {
        let mapping = object.map_range(vm, source, WINDOW, self.align(), prot, self.guard())?;
        if mapping.iova() % self.align() != 0 {
            return Err(EINVAL);
        }
        Ok(mapping)
    }

    /// [`Self::map`] at `at`, where the firmware expects this object.
    pub(crate) fn map_at(
        self,
        object: &KernelObject,
        vm: &mmu::Vm,
        source: Range<usize>,
        at: u64,
        prot: mmu::Prot,
    ) -> Result<mmu::KernelMapping> {
        let end = at.checked_add(self.mapped_bytes(source.len())? as u64).ok_or(EOVERFLOW)?;
        if at < WINDOW.start || end > WINDOW.end {
            return Err(EINVAL);
        }
        object.map_range(vm, source, at..end, self.align(), prot, self.guard())
    }

    /// The [`Placement`] of a [`KernelObject::new`] of this object in the window, whose guard is
    /// one page.
    pub(crate) fn placement(self) -> Result<Placement> {
        if self.guard() != mmu::UAT_PGSZ {
            return Err(EINVAL);
        }
        Ok(Placement::In(WINDOW, self.align()))
    }

    /// Refuses with ENOSPC, as the mapping would and before any backing is allocated, `count`
    /// of this object of `bytes` each that `vm`'s window has no room for: a hole for one, and
    /// room for all of them. The window is shared, so room seen here is no placement promise:
    /// the mapping can still fail, but only after allocating backing that could fit.
    pub(crate) fn check_room(self, vm: &mmu::Vm, bytes: usize, count: usize) -> Result {
        let (used, largest) = vm.capacity(WINDOW)?;
        let one = self.mapped_bytes(bytes)?;
        let all = one.checked_mul(count).ok_or(EOVERFLOW)?;
        if one as u64 > largest || all as u64 > WINDOW_BYTES.saturating_sub(used) {
            return Err(ENOSPC);
        }
        Ok(())
    }
}

// `PairObject::ALL` holds every object once, in order.
const _: () = {
    let mut index = 0;
    while index < PairObject::ALL.len() {
        assert!(PairObject::ALL[index] as usize == index);
        index += 1;
    }
    assert!(PairObject::ALL.len() == PairObject::MeshStorage as usize + 1);
};

/// The sizes of the objects a render pair built for one layout maps in the window.
pub(crate) struct PairSizes {
    /// The target records object: one record stride for each command slot.
    pub(crate) records: usize,
    /// The TPC object.
    pub(crate) tpc: usize,
    /// The initial parameter blocks, of `geometry`'s block size.
    pub(crate) blocks: usize,
    /// The parameter-buffer geometry, which sizes the blocks, scratch, page list and metrics.
    pub(crate) geometry: BlockGeometry,
    /// Command slots this render pair holds in flight.
    pub(crate) depth: usize,
    /// One command slot's context state.
    pub(crate) state: usize,
    /// The task-stage mesh storage the pair may map ([`mesh_storage`]).
    pub(crate) mesh_storage: Option<usize>,
}

impl PairSizes {
    /// How many of `object` the pair maps, and the bytes of each.
    fn of(&self, object: PairObject) -> Result<(usize, usize)> {
        Ok(match object {
            PairObject::Records => (1, self.records),
            PairObject::Tpc => (1, self.tpc),
            PairObject::ParameterBlock => (self.blocks, self.geometry.block_bytes),
            PairObject::SlotState => (self.depth, self.state),
            PairObject::Scratch => (1, scratch_bytes(self.geometry)?),
            PairObject::Discard => (1, crate::g17::fw::m5::pool::DISCARD_BYTES),
            PairObject::PageList => (1, page_list_bytes(self.geometry)?),
            PairObject::Metrics => (1, metrics_bytes(self.geometry)?),
            PairObject::MeshStorage => match self.mesh_storage {
                Some(bytes) => (1, bytes),
                None => (0, 0),
            },
        })
    }

    /// The window bytes of `object` in the pair.
    pub(crate) fn object_bytes(&self, object: PairObject) -> Result<usize> {
        let (count, bytes) = self.of(object)?;
        object.window_bytes(bytes)?.checked_mul(count).ok_or(EOVERFLOW)
    }

    /// The window bytes of every object of the pair.
    pub(crate) fn window_bytes(&self) -> Result<usize> {
        PairObject::ALL.into_iter().try_fold(0usize, |sum, object| {
            sum.checked_add(self.object_bytes(object)?).ok_or(EOVERFLOW)
        })
    }
}
