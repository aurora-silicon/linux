// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Rotating tile-map/heap metadata and the retained tail-pointer cache.
//! Larger targets replace only the objects that need more capacity. Every old
//! backing keeps the exact commands that still reference it until retirement.

use super::compact::PairObject;
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
const GTP_FIXED_SIZES: [usize; 4] = [0xc00, 0xa00, GTP6_SIZE, 0x80];
/// Room for the tiler's records across a partial render. macOS uses 0x1c00, so this is a superset,
/// and the size every partial-render test ran with.
const GTP_SLOTS: usize = 64;
const GTP6_SIZE: usize = GTP_SLOTS
    * (((cfg::TVB_MAX_BLOCKS * cfg::TVB_BLOCK_SIZE).div_ceil(0x8000) + 0x17ff) / 24 * 8 + 8);
/// Clustering metadata 2-4 (0x1c948, 0x1c888, 0x16c41/0x1ca40) on GPUs with more than two
/// clusters, at the sizes the M2 Pro/Max path gives them (`hw/t602x.rs` clustering config).
/// Metadata 1 (0x1c920) is sized per target there: [`META1_BLOCK_SIZE`] per block of tiles.
///
/// macOS 26.6.2 on the J616C spaces a clustered pass's metadata 2, 3 and 4 0x1180, 0x1400
/// and 0x1600 apart: metadata 2 needs 0x1180, more than the M2 Pro/Max 0xc00.
const GTP_MULTI_CLUSTER_SIZES: [usize; 3] = [0x1180, 0x280 * 16, 0x10 * 128];
/// Floor for four-cluster metadata 1 (`0x1c920`, `gtp[7]`).
///
/// The M2 Pro/Max per-block formula yields 0x100 (32x32 utiles) or 0x180 (16x16) on a
/// 3456x2234 Retina frame (Minecraft RD32) and 0x1100 on 10240x10240 (`point_size_10000`,
/// 16x16 utiles). Because metadata 1 is the last sub-buffer in the TPC object, neither
/// macOS's allocation size nor the hardware's written high-water mark was visible in the
/// J616C 26.6.2 capture; keep a conservative 16 KiB floor until a capture or hardware
/// high-water measurement shows the true upper bound.
const META1_MIN_SIZE: usize = 0x4000;
const META1_BLOCK_SIZE: usize = 0x44;

/// Per-command geometry, independent of the capacity already allocated.
#[derive(Copy, Clone, Debug)]
pub(crate) struct Layout {
    pub(crate) tilemap_size: usize,
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
    /// The M5 tiler slices of the TPC object and their cluster controls.
    m5: Option<(crate::g17::fw::m5::target::TargetSlices, [u64; 3])>,
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
            // The fixed sizes are the two-cluster M4 Pro's. With four clusters (M4 Max) clustered
            // tiling outgrows them: the firmware then never starts a heavy render and kills it as
            // no-progress (reason 0). Size them as the M2 Pro/Max path does.
            let [meta2, meta3, meta4, meta1] = if clusters > 2 {
                let meta1_blocks = aligned(tiles_x, 2)?
                    .checked_mul(aligned(tiles_y, 4)?)
                    .and_then(|v| v.checked_mul(utiles))
                    .ok_or(EOVERFLOW)?
                    .div_ceil(0x1980);
                [
                    GTP_MULTI_CLUSTER_SIZES[0],
                    GTP_MULTI_CLUSTER_SIZES[1],
                    // Partial renders need the larger size here too.
                    GTP_MULTI_CLUSTER_SIZES[2].max(GTP6_SIZE),
                    aligned(
                        meta1_blocks.checked_mul(META1_BLOCK_SIZE).ok_or(EOVERFLOW)?,
                        0x80,
                    )?
                    .max(META1_MIN_SIZE),
                ]
            } else {
                GTP_FIXED_SIZES
            };
            let sizes = [
                tpc_size,
                tilemap_size.checked_mul(64).ok_or(EOVERFLOW)?,
                if layers > 1 { 0x4000 } else { 0 },
                0,
                meta2,
                meta3,
                meta4,
                meta1,
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
            tilemap_size,
            layer_offset,
            heap_offset,
            used,
            tpc_size,
            tvb_blocks,
            gtp,
            tpc_end,
            m5: None,
        })
    }
    pub(crate) fn with_m5_slices(mut self, pass: &RenderPass, clusters: u32, groups: u32,
        segments: u32, parameter_bytes: usize) -> Result<Self> {
        let slices = crate::g17::fw::m5::target::TargetSlices::new(
            self.tilemap_size, self.tpc_size, pass.layers, groups, segments, parameter_bytes)
            .ok_or(EINVAL)?;
        // The last word is the part's minimum parameter-buffer size, whatever
        // the group count.
        let control = crate::g17::fw::m5::target::cluster_control(groups,
            crate::g17::fw::m5::target::min_parameter_buffer_pages(clusters))
            .ok_or(EINVAL)?;
        self.m5 = Some((slices, control));
        Ok(self)
    }
    pub(crate) fn is_m5(&self) -> bool { self.m5.is_some() }
    fn check_capacity(&self, stride: usize) -> Result {
        if self.m5.is_none() && stride > cfg::TARGET_RECORD_MAX {
            return Err(ERANGE);
        }
        stride.checked_mul(DEPTH).ok_or(EOVERFLOW)?;
        Ok(())
    }
    pub(crate) fn tpc_capacity(&self) -> Result<usize> {
        if let Some((slices, _)) = self.m5 {
            return aligned(slices.bytes, mmu::UAT_PGSZ);
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
        Ok(aligned(self.shared_end(), mmu::UAT_PGSZ)?.max(cfg::TPC_MIN))
    }
    /// End of the shared part, the TPC and GTP 1-3: GTP 4, where the metadata starts. GTP 4
    /// need not be page aligned, and the tile maps run up to it.
    fn shared_end(&self) -> usize {
        match (self.m5, self.gtp[4]) {
            (None, Some(start)) => start,
            _ => self.tpc_end,
        }
    }
    /// One render's clustering-metadata copy, page aligned; zero when there is none.
    fn meta_capacity(&self) -> Result<usize> {
        aligned(self.meta_bytes(), mmu::UAT_PGSZ)
    }
    /// Start of the per-render part: clustering metadata (GTP 4-7) begins at GTP 4, rounded
    /// down to a page so every sub-buffer keeps its alignment in a page-aligned copy.
    fn meta_base(&self) -> usize {
        match (self.m5, self.gtp[4]) {
            (None, Some(start)) => start / mmu::UAT_PGSZ * mmu::UAT_PGSZ,
            _ => self.tpc_end,
        }
    }
    /// Bytes of one render's metadata copy, zero without clustering metadata.
    fn meta_bytes(&self) -> usize {
        self.tpc_end - self.meta_base()
    }
    pub(crate) fn record_stride(&self) -> Result<usize> {
        let bytes = aligned(self.used, mmu::UAT_PGSZ)?;
        Ok(if self.m5.is_some() { bytes } else { bytes.max(cfg::TARGET_RECORD_MIN) })
    }
}

struct Object {
    address: u64,
    _mapping: mmu::KernelMapping,
    backing: KernelObject,
}
impl Object {
    /// An object of `size` bytes, on M5 mapped in the client's compact window as `m5`.
    fn new(dev: &AsahiDrmDevice, vm: &mmu::Vm, size: usize, m5: Option<PairObject>)
        -> Result<Self> {
        if let Some(object) = m5 {
            object.check_room(vm, size, 1)?;
        }
        // An M5 target is sized by the client's pass: charge it to the client.
        let backing = if m5.is_some() {
            KernelObject::client_backing(dev, size, vm.memory_client())?
        } else {
            KernelObject::backing(dev, size, CpuMap::WriteCombined)?
        };
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let mapping = match m5 {
            Some(object) => object.map(&backing, vm, 0..size, prot)?,
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
    /// The TPC and tile-map sub-buffers, shared by every render of the target.
    tpc: Object,
    tpc_stride: usize,
    /// One zeroed clustering-metadata copy (GTP 4-7) per in-flight render, like the records.
    /// Only these are copied: a copy of the whole TPC object per render (32 in flight) filled
    /// the client's target window once a game kept several large targets.
    meta: Option<Object>,
    meta_stride: usize,
    stride: usize,
    slots: [Option<u64>; DEPTH],
    retired: KVec<Retired>,
    m5: bool,
}
impl Target {
    /// Keep this allocation frame separate from the rest of the render graph.
    #[inline(never)]
    pub(crate) fn new(dev: &AsahiDrmDevice, vm: &mmu::Vm, layout: &Layout) -> Result<KBox<Self>> {
        let tpc = layout.tpc_capacity()?;
        let stride = layout.record_stride()?;
        layout.check_capacity(stride)?;
        let m5 = layout.m5.is_some();
        let records = Object::new(dev, vm, stride * DEPTH, m5.then_some(PairObject::Records))?;
        let tpc_stride = tpc;
        let tpc = Object::new(dev, vm, tpc, m5.then_some(PairObject::Tpc))?;
        let meta_stride = layout.meta_capacity()?;
        let meta = if meta_stride == 0 {
            None
        } else {
            Some(Object::new(dev, vm, meta_stride.checked_mul(DEPTH).ok_or(EOVERFLOW)?, None)?)
        };
        KBox::new(
            Self {
                records,
                tpc,
                tpc_stride,
                meta,
                meta_stride,
                stride,
                slots: [None; DEPTH],
                retired: KVec::new(),
                m5,
            },
            GFP_KERNEL,
        )
        .map_err(Into::into)
    }
    fn meta_offset(&self, ordinal: u64) -> usize {
        (ordinal as usize % DEPTH) * self.meta_stride
    }

    pub(crate) fn tpc_va(&self) -> u64 {
        self.tpc.address
    }

    /// The M5 tiling words naming the TPC object's slices of `layout`.
    pub(crate) fn m5_extra(&self, layout: &Layout)
        -> Result<crate::g17::fw::m5::render_registers::TaExtra> {
        if !self.m5 { return Err(EINVAL); }
        let (slices, control) = layout.m5.ok_or(EINVAL)?;
        if slices.bytes > self.tpc.backing.size() { return Err(ERANGE); }
        let base = self.tpc.address.checked_sub(super::BlockGeometry::M5_COMPACT.start)
            .ok_or(EINVAL)?;
        let mut pointers = [0; 8];
        for index in 0..8 {
            if slices.sizes[index] != 0 {
                pointers[index] = base.checked_add(slices.offsets[index] as u64).ok_or(EOVERFLOW)?;
            }
        }
        Ok(crate::g17::fw::m5::render_registers::TaExtra {
            gtp1: pointers[1], gtp2: pointers[2], gtp4: pointers[4], gtp5: pointers[5],
            gtp6: pointers[6], gtp7: pointers[7], segment_control: control[0], segment_index: control[1],
            min_free_tvb_pages: control[2], scs_bracket: 0, matrix_bracket: 0, j813: false,
        })
    }

    /// GPU addresses of the tiler parameter sub-buffers of `layout` (25G83), zero for an empty
    /// or absent one. The TPC object holds them after the TPC.
    pub(crate) fn gtp_va(&self, ordinal: u64, layout: &Layout) -> Result<[u64; 8]> {
        let split = layout.meta_base();
        if layout.shared_end() > self.tpc_stride || layout.meta_bytes() > self.meta_stride {
            return Err(ERANGE);
        }
        let meta = match &self.meta {
            Some(meta) => meta.address.checked_add(self.meta_offset(ordinal) as u64).ok_or(EOVERFLOW)?,
            None => 0,
        };
        let mut addresses = [0; 8];
        for (address, offset) in addresses.iter_mut().zip(layout.gtp) {
            if let Some(offset) = offset {
                *address = if offset < split {
                    self.tpc.address.checked_add(offset as u64).ok_or(EOVERFLOW)?
                } else {
                    if meta == 0 { return Err(ERANGE); }
                    meta.checked_add((offset - split) as u64).ok_or(EOVERFLOW)?
                };
            }
        }
        Ok(addresses)
    }

    /// Only the span consumed by this render is cleared. A small render does
    /// not clear a previous large target's complete retained record capacity.
    pub(crate) fn prepare(&mut self, ordinal: u64, layout: &Layout) -> Result<Addresses> {
        let slot = ordinal as usize % DEPTH;
        if self.m5 != layout.m5.is_some() { return Err(EINVAL); }
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
        // Clustered tiling expects zeroed clustering metadata (GTP 4-7), as G15 gives every
        // clustered pass a zeroed buffer. Zero this render's own copy.
        if !self.m5 {
            if let Some(start) = layout.gtp[4] {
                let split = layout.meta_base();
                if layout.meta_bytes() > self.meta_stride {
                    return Err(ERANGE);
                }
                let base = self.meta_offset(ordinal);
                self.meta
                    .as_mut()
                    .ok_or(ERANGE)?
                    .backing
                    .bytes_mut()
                    .get_mut(base + start - split..base + layout.tpc_end - split)
                    .ok_or(ERANGE)?
                    .fill(0);
            }
        }
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
        if self.m5 != layout.m5.is_some() { return Err(EINVAL); }
        layout.check_capacity(stride)?;
        let mut grew = false;
        if tpc > self.tpc_stride {
            Self::replace(
                &mut self.retired,
                &mut self.tpc,
                dev,
                vm,
                tpc,
                owners,
                self.m5.then_some(PairObject::Tpc),
            )?;
            self.tpc_stride = tpc;
            grew = true;
        }
        let meta = layout.meta_capacity()?;
        if meta > self.meta_stride {
            let size = meta.checked_mul(DEPTH).ok_or(EOVERFLOW)?;
            match self.meta.as_mut() {
                Some(old) => Self::replace(&mut self.retired, old, dev, vm, size, owners, None)?,
                None => self.meta = Some(Object::new(dev, vm, size, None)?),
            }
            self.meta_stride = meta;
            grew = true;
        }
        if stride > self.stride {
            Self::replace(
                &mut self.retired,
                &mut self.records,
                dev,
                vm,
                stride * DEPTH,
                owners,
                self.m5.then_some(PairObject::Records),
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
        m5: Option<PairObject>,
    ) -> Result {
        let retain = owners.iter().any(Option::is_some);
        if retain {
            retired.reserve(1, GFP_KERNEL)?;
        }
        let new = Object::new(dev, vm, size, m5)?;
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
            assert_eq!(layout.gtp[0], Some(0));
            assert!(layout.gtp[1].unwrap() >= layout.tpc_size);
            fits(&layout);
        }
    }

    /// Every sub-buffer of `layout` lies whole in its object with its alignment: the TPC and
    /// GTP 1-3 in the shared TPC object, GTP 4-7 in a render's metadata copy.
    fn fits(layout: &Layout) {
        let shared = layout.tpc_capacity().unwrap();
        let split = layout.meta_base();
        // GTP 0-3 sizes, as Layout::new gives them; GTP 2 exists for layered targets only.
        let sizes = [layout.tpc_size, layout.tilemap_size * 64, 0x4000, 0];
        for (index, size) in sizes.into_iter().enumerate() {
            if let Some(offset) = layout.gtp[index] {
                assert!(offset < split);
                assert!(offset + size <= shared);
            }
        }
        assert!(layout.meta_capacity().unwrap() >= layout.tpc_end - split);
        for (offset, alignment) in layout.gtp.iter().zip(GTP_ALIGNMENTS).skip(4) {
            assert!(offset.unwrap() >= split);
            assert_eq!((offset.unwrap() - split) % alignment, 0);
        }
    }

    #[test]
    fn only_clustering_metadata_is_copied_per_render() {
        // 1728x1117 on the four-cluster M4 Max: the per-render copy is the metadata alone,
        // not the TPC and the megabyte of tile maps.
        let mut p = pass(1728, 32);
        p.height = 1117;
        let layout = Layout::new(&p, 4, RegisterPrograms::V25G83).unwrap();
        let start = layout.gtp[4].unwrap();
        assert!(layout.meta_base() <= start && start - layout.meta_base() < mmu::UAT_PGSZ);
        // Metadata 4 (GTP 6) has room for 64 partial renders: 0x65e00 bytes.
        assert!(layout.meta_capacity().unwrap() <= 0x8_0000);
        assert!(layout.meta_capacity().unwrap() < layout.tilemap_size * 64);
        fits(&layout);
    }

    #[test]
    fn the_shared_object_holds_the_last_tile_map() {
        // With 32x32 utiles at 3456x2234 the TPC is 0x46000 bytes, so GTP 4 is not page
        // aligned and the tile maps end past GTP 4 rounded down to a page.
        let mut p = pass(3456, 32);
        p.height = 2234;
        let layout = Layout::new(&p, 4, RegisterPrograms::V25G83).unwrap();
        assert_ne!(layout.gtp[4].unwrap() % mmu::UAT_PGSZ, 0);
        let end = layout.gtp[1].unwrap() + layout.tilemap_size * 64;
        assert!(end > aligned(layout.meta_base(), mmu::UAT_PGSZ).unwrap());
        assert!(layout.tpc_capacity().unwrap() >= end);
        for (width, height) in [(1728, 1117), (3456, 2234), (2048, 2048), (1280, 720)] {
            for (utile_width, utile_height) in [(32, 32), (32, 16), (16, 16)] {
                for layers in [1, 2] {
                    for clusters in [2, 4] {
                        let mut p = pass(width, utile_width);
                        p.height = height;
                        p.utile_height = utile_height;
                        p.layers = layers;
                        fits(&Layout::new(&p, clusters, RegisterPrograms::V25G83).unwrap());
                    }
                }
            }
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

    #[test]
    fn four_cluster_v25g83_metadata_layout() {
        let mut retina = pass(3456, 32);
        retina.height = 2234;
        for (target, utile) in [(retina, 32), (retina, 16), (pass(10240, 16), 16)] {
            let mut p = target;
            p.utile_width = utile;
            p.utile_height = utile;
            let layout = Layout::new(&p, 4, RegisterPrograms::V25G83).unwrap();
            let gtp4 = layout.gtp[4].unwrap();
            let gtp5 = layout.gtp[5].unwrap();
            let gtp6 = layout.gtp[6].unwrap();
            let gtp7 = layout.gtp[7].unwrap();
            assert_eq!(gtp5 - gtp4, GTP_MULTI_CLUSTER_SIZES[0]);
            assert!(gtp6 - gtp5 >= GTP_MULTI_CLUSTER_SIZES[1]);
            assert!(gtp7 - gtp6 >= GTP_MULTI_CLUSTER_SIZES[2].max(GTP6_SIZE));
            assert_eq!(layout.tpc_end - gtp7, META1_MIN_SIZE);
        }
        // Two-cluster V25G83 (M4 Pro) keeps its original fixed metadata sizes.
        let two = Layout::new(&retina, 2, RegisterPrograms::V25G83).unwrap();
        assert_eq!(two.gtp[5].unwrap() - two.gtp[4].unwrap(), GTP_FIXED_SIZES[0]);
        assert_eq!(two.tpc_end - two.gtp[7].unwrap(), GTP_FIXED_SIZES[3]);
    }
}
