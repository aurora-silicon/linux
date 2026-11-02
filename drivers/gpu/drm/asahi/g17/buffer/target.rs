// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Rotating tile-map/heap metadata and the retained tail-pointer cache.
//! Larger targets replace only the objects that need more capacity. Every old
//! backing keeps the exact commands that still reference it until retirement.

use super::compact_alias;
use crate::g17::{
    fw::render::{RegisterPrograms, RenderPass},
    object::{CpuMap, KernelObject},
};
use crate::{
    driver::AsahiDrmDevice,
    hw::t8140::{queues, resources as cfg},
    mmu,
};
use kernel::prelude::*;

const DEPTH: usize = queues::RENDER_DEPTH as usize;

fn aligned(value: usize, alignment: usize) -> Result<usize> {
    value
        .checked_add(alignment - 1)
        .map(|v| v & !(alignment - 1))
        .ok_or(EOVERFLOW)
}

/// The 25G83 tiler parameter block: the TPC and seven sub-buffers the tiling registers name, at
/// the default parameter-buffer sizes. Sizes are
/// [TPC, tile map × 64, 0x4000 for layered targets, 0, 0xc00, 0xa00, 0x600, 0x80].
const GTP_ALIGNMENTS: [usize; 8] = [0x40, 0x40, 0x100, 1, 0x20, 0x20, 0x80, 0x40];
const GTP_FIXED_SIZES: [usize; 4] = [0xc00, 0xa00, 0x600, 0x80];

/// Per-command geometry, independent of the capacity already allocated.
#[derive(Copy, Clone, Debug)]
pub(crate) struct Layout {
    pub(crate) layer_offset: usize,
    pub(crate) heap_offset: usize,
    pub(crate) used: usize,
    pub(crate) tpc_size: usize,
    pub(crate) tvb_blocks: usize,
    /// Offsets of the tiler parameter sub-buffers in the TPC object (25G83), `None` for an
    /// empty one; the TPC is entry 0.
    gtp: [Option<usize>; 8],
    /// Bytes of the TPC object the command uses.
    tpc_end: usize,
    pub(crate) tilemap_size: usize,
    /// M5: the target slices and cluster controls, and the client's compact window, in which the
    /// target's objects lie ([`Self::with_m5_slices`]).
    m5: Option<M5Target>,
}

#[derive(Copy, Clone, Debug)]
struct M5Target {
    slices: crate::g17::fw::m5::target::TargetSlices,
    control: [u64; 3],
    /// Start and end of the compact window.
    compact: [u64; 2],
}

impl Layout {
    pub(crate) fn new(
        pass: &RenderPass,
        clusters: u32,
        programs: RegisterPrograms,
    ) -> Result<Self> {
        let width = pass.width as usize;
        let height = pass.height as usize;
        let layers = pass.layers as usize;
        let (uw, uh) = (pass.utile_width as usize, pass.utile_height as usize);
        if width == 0
            || height == 0
            || layers == 0
            || clusters == 0
            || uw == 0
            || uh == 0
            || 32 % uw != 0
            || 32 % uh != 0
        {
            return Err(EINVAL);
        }
        let utiles = (32 / uw) * (32 / uh);
        let tiles_x = width.div_ceil(32);
        let tiles_y = height.div_ceil(32);
        let macro_x = aligned(tiles_x.div_ceil(4), 4)?;
        let macro_y = aligned(tiles_y.div_ceil(4), 4)?;
        let macro_tiles = macro_x.checked_mul(macro_y).ok_or(EOVERFLOW)?;
        let tilemap_size = aligned(
            5usize
                .checked_mul(macro_tiles)
                .and_then(|v| v.checked_mul(utiles))
                .ok_or(EOVERFLOW)?,
            4,
        )?
        .checked_mul(16)
        .and_then(|v| v.checked_mul(layers))
        .ok_or(EOVERFLOW)?;
        let tpc_size = 8usize
            .checked_mul(utiles)
            .and_then(|v| v.checked_mul(macro_tiles))
            .map(|v| v / 4 * 4)
            .and_then(|v| v.checked_mul(16))
            .and_then(|v| v.checked_mul(layers))
            .and_then(|v| v.checked_mul(clusters as usize))
            .ok_or(EOVERFLOW)?;
        let layer_offset = aligned(tilemap_size, 0x1000)?;
        let heap_offset = layer_offset
            .checked_add(if layers > 1 { 0x100 } else { 0 })
            .ok_or(EOVERFLOW)?;
        let used = heap_offset.checked_add(0x200).ok_or(EOVERFLOW)?;
        let tiles = tiles_x.checked_mul(tiles_y).ok_or(EOVERFLOW)?;
        let mut tvb_blocks = aligned(tiles.div_ceil(128), 8)?.max(8);
        if clusters > 1 {
            tvb_blocks = tvb_blocks.max(
                7usize
                    .checked_add(2usize.checked_mul(layers).ok_or(EOVERFLOW)?)
                    .ok_or(EOVERFLOW)?,
            );
        }
        tvb_blocks = tvb_blocks.max(cfg::TVB_INITIAL_PAGES.len());
        let mut gtp = [None; 8];
        let mut tpc_end = tpc_size;
        if programs == RegisterPrograms::V25G83 {
            let sizes = [
                tpc_size,
                tilemap_size.checked_mul(64).ok_or(EOVERFLOW)?,
                if layers > 1 { 0x4000 } else { 0 },
                0,
                GTP_FIXED_SIZES[0],
                GTP_FIXED_SIZES[1],
                GTP_FIXED_SIZES[2],
                GTP_FIXED_SIZES[3],
            ];
            let mut position = 0;
            for ((offset, size), alignment) in gtp.iter_mut().zip(sizes).zip(GTP_ALIGNMENTS) {
                position = aligned(position, alignment)?;
                if size != 0 {
                    *offset = Some(position);
                }
                position = position.checked_add(size).ok_or(EOVERFLOW)?;
            }
            tpc_end = position;
        }
        Ok(Self {
            layer_offset,
            heap_offset,
            used,
            tpc_size,
            tvb_blocks,
            gtp,
            tpc_end,
            tilemap_size,
            m5: None,
        })
    }
    /// The M5 layout of this geometry: `groups` active tiling groups whose auxiliary tables have
    /// `segments` segments, the topology's minimum parameter buffer of `minimum_pages` (the last
    /// cluster-control word; S passes its auxiliary segment count, 64, there), a parameter
    /// inventory of `parameter_bytes`, in the client's compact window `compact` (S
    /// `with_m5_slices`).
    pub(crate) fn with_m5_slices(mut self, pass: &RenderPass, groups: u32, segments: u32,
        minimum_pages: u32, parameter_bytes: usize, compact: core::ops::Range<u64>) -> Result<Self> {
        use crate::g17::fw::m5::target;
        let slices = target::TargetSlices::new(
            self.tilemap_size, self.tpc_size, pass.layers, groups, segments, parameter_bytes)
            .ok_or(EINVAL)?;
        let control = target::cluster_control(groups, u64::from(minimum_pages)).ok_or(EINVAL)?;
        self.m5 = Some(M5Target { slices, control, compact: [compact.start, compact.end] });
        Ok(self)
    }
    pub(crate) fn is_m5(&self) -> bool {
        self.m5.is_some()
    }
    pub(crate) fn tpc_capacity(&self) -> Result<usize> {
        if let Some(m5) = &self.m5 {
            return aligned(m5.slices.bytes, mmu::UAT_PGSZ);
        }
        // TPC_MAX bounds the tail-pointer cache itself. On 25G83 this object
        // also contains GTP sub-buffers, notably 64 times the tile map. Applying
        // the cache limit to their combined extent rejects valid large targets.
        // Keep the original cache and tile-record limits; they also bound the
        // derived auxiliary extent before any backing allocation or replacement.
        if aligned(self.tpc_size, mmu::UAT_PGSZ)?.max(cfg::TPC_MIN) > cfg::TPC_MAX
            || self.record_stride()? > cfg::TARGET_RECORD_MAX
        {
            return Err(ERANGE);
        }
        Ok(aligned(self.tpc_end, mmu::UAT_PGSZ)?.max(cfg::TPC_MIN))
    }
    pub(crate) fn record_stride(&self) -> Result<usize> {
        let bytes = aligned(self.used, mmu::UAT_PGSZ)?;
        Ok(if self.m5.is_some() { bytes } else { bytes.max(cfg::TARGET_RECORD_MIN) })
    }
    /// Where the target's objects are mapped: M5's compact window, else the A18 Pro's ranges.
    fn compact(&self) -> Option<core::ops::Range<u64>> {
        self.m5.as_ref().map(|m5| m5.compact[0]..m5.compact[1])
    }
}

struct Object {
    address: u64,
    _mapping: mmu::KernelMapping,
    backing: KernelObject,
}
impl Object {
    fn new(dev: &AsahiDrmDevice, vm: &mmu::Vm, size: usize,
        compact: Option<core::ops::Range<u64>>) -> Result<Self> {
        let backing = KernelObject::backing(dev, size, CpuMap::WriteCombined)?;
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let mapping = match compact {
            Some(range) => backing.map_range(vm, 0..size, range, cfg::TVB_PAGE, prot, mmu::UAT_PGSZ)?,
            None => compact_alias(&backing, vm, 0..size, cfg::TVB_PAGE, prot, mmu::UAT_PGSZ)?,
        };
        Ok(Self {
            address: mapping.iova(),
            _mapping: mapping,
            backing,
        })
    }
}
struct Retired {
    _object: Object,
    owners: [Option<u64>; DEPTH],
}

#[derive(Copy, Clone, Debug)]
pub(crate) struct Addresses {
    pub(crate) tilemap: u64,
    pub(crate) layer: u64,
    pub(crate) heap: u64,
}

pub(crate) struct Target {
    records: Object,
    tpc: Object,
    stride: usize,
    slots: [Option<u64>; DEPTH],
    retired: KVec<Retired>,
}
impl Target {
    /// Keep this allocation frame separate from the rest of the render graph.
    #[inline(never)]
    pub(crate) fn new(dev: &AsahiDrmDevice, vm: &mmu::Vm, layout: &Layout) -> Result<KBox<Self>> {
        let tpc = layout.tpc_capacity()?;
        let stride = layout.record_stride()?;
        if layout.m5.is_none() && stride > cfg::TARGET_RECORD_MAX {
            return Err(ERANGE);
        }
        let records = Object::new(dev, vm, stride.checked_mul(DEPTH).ok_or(EOVERFLOW)?, layout.compact())?;
        let tpc = Object::new(dev, vm, tpc, layout.compact())?;
        KBox::new(
            Self {
                records,
                tpc,
                stride,
                slots: [None; DEPTH],
                retired: KVec::new(),
            },
            GFP_KERNEL,
        )
        .map_err(Into::into)
    }
    pub(crate) fn tpc_va(&self) -> u64 {
        self.tpc.address
    }

    /// The M5 tiling words naming `layout`'s target slices in the TPC object, as compact
    /// offsets, and its cluster controls (S `m5_extra`).
    pub(crate) fn m5_extra(&self, layout: &Layout)
        -> Result<crate::g17::fw::m5::render_registers::TaExtra> {
        let m5 = layout.m5.as_ref().ok_or(EINVAL)?;
        if m5.slices.bytes > self.tpc.backing.size() { return Err(ERANGE); }
        let base = self.tpc.address.checked_sub(m5.compact[0]).ok_or(EINVAL)?;
        let mut pointers = [0; 8];
        for index in 0..8 {
            if m5.slices.sizes[index] != 0 {
                pointers[index] = base.checked_add(m5.slices.offsets[index] as u64).ok_or(EOVERFLOW)?;
            }
        }
        let control = m5.control;
        Ok(crate::g17::fw::m5::render_registers::TaExtra {
            d11c8: pointers[1], d11d0: pointers[2], d11e8: pointers[4], d11f0: pointers[5],
            d11f8: pointers[6], d1208: pointers[7], d1210: control[0], d1218: control[1],
            d1220: control[2], d1590: 0, d1591: 0,
        })
    }

    /// GPU addresses of the tiler parameter sub-buffers of `layout` (25G83), zero for an empty
    /// or absent one. The TPC object holds them after the TPC.
    pub(crate) fn gtp_va(&self, layout: &Layout) -> Result<[u64; 8]> {
        if layout.tpc_end > self.tpc.backing.size() {
            return Err(ERANGE);
        }
        let mut addresses = [0; 8];
        for (address, offset) in addresses.iter_mut().zip(layout.gtp) {
            if let Some(offset) = offset {
                *address = self.tpc.address.checked_add(offset as u64).ok_or(EOVERFLOW)?;
            }
        }
        Ok(addresses)
    }

    /// Only the span consumed by this render is cleared. A small render does
    /// not clear a previous large target's complete retained record capacity.
    pub(crate) fn prepare(&mut self, ordinal: u64, layout: &Layout) -> Result<Addresses> {
        let slot = ordinal as usize % DEPTH;
        if self.slots[slot].is_some_and(|owner| owner != ordinal) {
            return Err(EBUSY);
        }
        if layout.used > self.stride {
            return Err(ERANGE);
        }
        let offset = slot * self.stride;
        let span = aligned(layout.used, mmu::UAT_PGSZ)?.min(self.stride);
        self.records
            .backing
            .bytes_mut()
            .get_mut(offset..offset + span)
            .ok_or(ERANGE)?
            .fill(0);
        self.slots[slot] = Some(ordinal);
        let tilemap = self
            .records
            .address
            .checked_add(offset as u64)
            .ok_or(EOVERFLOW)?;
        Ok(Addresses {
            tilemap,
            layer: tilemap
                .checked_add(layout.layer_offset as u64)
                .ok_or(EOVERFLOW)?,
            heap: tilemap
                .checked_add(layout.heap_offset as u64)
                .ok_or(EOVERFLOW)?,
        })
    }

    /// Growth never shrinks a live allocation. A successfully replaced TPC
    /// stays enlarged even if allocating the following record object fails.
    pub(crate) fn ensure_capacity(
        &mut self,
        dev: &AsahiDrmDevice,
        vm: &mmu::Vm,
        layout: &Layout,
        owners: &[Option<u64>; DEPTH],
    ) -> Result<bool> {
        let tpc = layout.tpc_capacity()?;
        let stride = layout.record_stride()?;
        let mut grew = false;
        if tpc > self.tpc.backing.size() {
            Self::replace(&mut self.retired, &mut self.tpc, dev, vm, tpc, owners, layout.compact())?;
            grew = true;
        }
        if stride > self.stride {
            if layout.m5.is_none() && stride > cfg::TARGET_RECORD_MAX {
                return Err(ERANGE);
            }
            Self::replace(
                &mut self.retired,
                &mut self.records,
                dev,
                vm,
                stride * DEPTH,
                owners,
                layout.compact(),
            )?;
            self.stride = stride;
            self.slots = [None; DEPTH];
            grew = true;
        }
        Ok(grew)
    }
    fn replace(
        retired: &mut KVec<Retired>,
        old: &mut Object,
        dev: &AsahiDrmDevice,
        vm: &mmu::Vm,
        size: usize,
        owners: &[Option<u64>; DEPTH],
        compact: Option<core::ops::Range<u64>>,
    ) -> Result {
        let retain = owners.iter().any(Option::is_some);
        if retain {
            retired.reserve(1, GFP_KERNEL)?;
        }
        let new = Object::new(dev, vm, size, compact)?;
        let previous = core::mem::replace(old, new);
        if retain {
            super::append_reserved(
                retired,
                Retired {
                    _object: previous,
                    owners: *owners,
                },
            );
        }
        Ok(())
    }
    /// The passes whose records are live, by slot: an owner of replaced storage.
    pub(crate) fn owners(&self) -> [Option<u64>; DEPTH] {
        self.slots
    }
    pub(crate) fn retire(&mut self, ordinal: u64) -> Result {
        let slot = &mut self.slots[ordinal as usize % DEPTH];
        if *slot == Some(ordinal) {
            *slot = None;
        }
        let mut index = 0;
        while index < self.retired.len() {
            for owner in &mut self.retired[index].owners {
                if *owner == Some(ordinal) {
                    *owner = None;
                }
            }
            if self.retired[index].owners.iter().all(Option::is_none) {
                drop(self.retired.remove(index)?);
            } else {
                index += 1;
            }
        }
        Ok(())
    }
    /// Cancels only an unpublished record; older referenced backing remains
    /// owned until normal paired retirement or the firmware-stop boundary.
    pub(crate) fn cancel(&mut self, ordinal: u64) {
        let slot = &mut self.slots[ordinal as usize % DEPTH];
        if *slot == Some(ordinal) {
            *slot = None;
        }
    }
}


#[kernel::macros::kunit_tests(asahi_g17_target_capacity)]
mod tests {
    use super::*;

    fn pass(size: u32, utile: u8) -> RenderPass {
        // SAFETY: RenderPass and its nested surfaces/programs contain only
        // integers and bools, for which the all-zero representation is valid.
        let mut pass: RenderPass = unsafe { core::mem::zeroed() };
        pass.width = size;
        pass.height = size;
        pass.layers = 1;
        pass.utile_width = utile;
        pass.utile_height = utile;
        pass
    }

    #[test]
    fn t6040_auxiliary_tiler_storage_is_not_tail_pointer_cache() {
        // CTS point_size_10000 renders to 10240 squared. Its GTP sub-buffer
        // is required by the existing 25G83 program, not a larger cache.
        for utile in [16, 32] {
            let layout = Layout::new(&pass(10240, utile), 2, RegisterPrograms::V25G83).unwrap();
            assert!(layout.tpc_size < cfg::TPC_MAX);
            assert!(layout.record_stride().unwrap() < cfg::TARGET_RECORD_MAX);
            assert!(layout.tpc_end > cfg::TPC_MAX);
            assert!(layout.tpc_capacity().unwrap() >= layout.tpc_end);
            assert_eq!(layout.gtp[0], Some(0));
            assert!(layout.gtp[1].unwrap() >= layout.tpc_size);
        }
    }

    #[test]
    fn core_and_tile_record_limits_still_reject_oversized_targets() {
        let mut layout = Layout::new(&pass(10240, 32), 2, RegisterPrograms::V25G83).unwrap();
        layout.tpc_size = cfg::TPC_MAX + 1;
        assert_eq!(layout.tpc_capacity(), Err(ERANGE));
        let mut layered = pass(16384, 16);
        layered.layers = 2;
        let layout = Layout::new(&layered, 2, RegisterPrograms::V25G83).unwrap();
        assert!(layout.record_stride().unwrap() > cfg::TARGET_RECORD_MAX);
        assert_eq!(layout.tpc_capacity(), Err(ERANGE));
    }

    #[test]
    fn neo_keeps_the_same_cache_extent_and_limit() {
        let mut layout = Layout::new(&pass(10240, 32), 1, RegisterPrograms::G17P).unwrap();
        assert!(layout.gtp.iter().all(Option::is_none));
        assert_eq!(layout.tpc_capacity().unwrap(),
            aligned(layout.tpc_size, mmu::UAT_PGSZ).unwrap().max(cfg::TPC_MIN));
        layout.tpc_size = cfg::TPC_MAX + 1;
        layout.tpc_end = layout.tpc_size;
        assert_eq!(layout.tpc_capacity(), Err(ERANGE));
    }
}
