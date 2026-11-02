// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Parameter-buffer IDs, scene ownership and sparse tiler heap growth.
//!
//! The device mutex serializes the shared ID allocator. A render graph owns
//! its buffer ID independently of the submission references using that graph;
//! a published command keeps its reference until firmware retirement is witnessed.

use super::fw::buffer as fw;
use crate::hw::t8140::resources as cfg;
mod geometry;
mod layout;
pub(crate) mod target;
pub(crate) use geometry::BlockGeometry;

use kernel::prelude::*;

const BUFFER_IDS: usize = 127;

#[derive(Copy, Clone)]
struct BufferId {
    references: u8,
    owner: bool,
    token: u64,
}
impl BufferId {
    fn free(self) -> bool {
        self.references == 0 && !self.owner
    }
}

/// Device-wide identities for retained render parameter buffers.
pub(crate) struct BufferIds {
    entries: [BufferId; BUFFER_IDS],
    returned: [u8; BUFFER_IDS],
    returned_count: usize,
    next_token: u64,
}
impl BufferIds {
    pub(crate) fn new() -> impl Init<Self, Error> {
        kernel::try_init!(Self {
            entries <- pin_init::init_array_from_fn(|_| BufferId {
                references: 0,
                owner: false,
                token: 0,
            }),
            returned <- pin_init::init_array_from_fn(|index| (BUFFER_IDS - index - 1) as u8),
            returned_count: BUFFER_IDS,
            next_token: 0,
        })
    }
    fn remove_returned(&mut self, id: u8) {
        if let Some(at) = self.returned[..self.returned_count]
            .iter()
            .position(|entry| *entry == id)
        {
            self.returned.copy_within(at + 1..self.returned_count, at);
            self.returned_count -= 1;
        }
    }
    fn return_id(&mut self, id: u8) {
        if !self.returned[..self.returned_count].contains(&id) && self.returned_count < BUFFER_IDS {
            self.returned[self.returned_count] = id;
            self.returned_count += 1;
        }
    }
    fn fresh_token(&mut self) -> u64 {
        let token = if self.next_token == u64::MAX {
            0
        } else {
            self.next_token
        };
        self.next_token = token + 1;
        token
    }
    pub(crate) fn allocate_render(&mut self) -> Result<(u8, u64)> {
        let id = if self.returned_count != 0 {
            self.returned_count -= 1;
            self.returned[self.returned_count]
        } else {
            self.entries
                .iter()
                .position(|entry| entry.free())
                .ok_or_else(|| super::capacity::refused(super::capacity::Site::ParameterBuffers))? as u8
        };
        if !self.entries[usize::from(id)].free() {
            return Err(EIO);
        }
        let token = self.fresh_token();
        self.entries[usize::from(id)] = BufferId {
            references: 1,
            owner: false,
            token,
        };
        Ok((id, token))
    }

    /// Retains the existing identity for a graph independently of its jobs.
    pub(crate) fn reserve_owner(&mut self, id: u8) -> Result {
        let entry = self.entries.get(usize::from(id)).ok_or(EINVAL)?;
        if entry.owner {
            return Ok(());
        }
        if entry.free() {
            let token = self.fresh_token();
            self.entries[usize::from(id)].token = token;
        }
        self.entries[usize::from(id)].owner = true;
        self.remove_returned(id);
        Ok(())
    }
    pub(crate) fn acquire_owner(&mut self, id: u8) -> Result<(u8, u64)> {
        let entry = self.entries.get_mut(usize::from(id)).ok_or(EINVAL)?;
        if !entry.owner {
            return Err(EINVAL);
        }
        entry.references = entry.references.checked_add(1).ok_or(ENOSPC)?;
        Ok((id, entry.token))
    }
    /// Preflight shared completion bookkeeping before any of its counters change.
    pub(crate) fn can_release(&self, id: u8) -> Result {
        let entry = self.entries.get(usize::from(id)).ok_or(EINVAL)?;
        if entry.references == 0 {
            return Err(EINVAL);
        }
        Ok(())
    }
    pub(crate) fn release(&mut self, id: u8) -> Result {
        let entry = self.entries.get_mut(usize::from(id)).ok_or(EINVAL)?;
        entry.references = entry.references.checked_sub(1).ok_or(EINVAL)?;
        if entry.free() {
            self.return_id(id);
        }
        Ok(())
    }
    pub(crate) fn unreserve_owner(&mut self, id: u8) -> Result {
        let entry = self.entries.get_mut(usize::from(id)).ok_or(EINVAL)?;
        if !entry.owner {
            return Ok(());
        }
        entry.owner = false;
        if entry.free() {
            self.return_id(id);
        }
        Ok(())
    }
}

/// Each scene and its shorter scratch slot are one lifetime. A completed
/// paired render releases both; merely publishing a later scene does not.
pub(crate) struct Scenes {
    owners: [Option<(u64, u8)>; fw::SCRATCH_SLOTS],
    next: usize,
}
impl Scenes {
    pub(crate) const fn new() -> Self {
        Self {
            owners: [None; fw::SCRATCH_SLOTS],
            next: fw::FIRST_SCENE,
        }
    }
    pub(crate) fn select(&self, ordinal: u64) -> Result<usize> {
        if let Some((_, scene)) = self
            .owners
            .iter()
            .flatten()
            .find(|(owner, _)| *owner == ordinal)
        {
            return Ok(usize::from(*scene));
        }
        (0..fw::SCENES)
            .map(|offset| (self.next + offset) % fw::SCENES)
            .find(|scene| self.owners[scene % fw::SCRATCH_SLOTS].is_none())
            .ok_or(ENOSPC)
    }
    pub(crate) fn commit(&mut self, ordinal: u64, scene: usize) -> Result {
        if self.select(ordinal)? != scene {
            return Err(EINVAL);
        }
        let slot = &mut self.owners[scene % fw::SCRATCH_SLOTS];
        if slot.is_none() {
            *slot = Some((ordinal, scene as u8));
            self.next = (scene + 1) % fw::SCENES;
        }
        Ok(())
    }
    pub(crate) fn release(&mut self, ordinal: u64) -> Result {
        let slot = self
            .owners
            .iter_mut()
            .find(|entry| entry.is_some_and(|(owner, _)| owner == ordinal))
            .ok_or(EINVAL)?;
        *slot = None;
        Ok(())
    }
}

/// A snapshot of the block ring; all values are modulo the geometry's block capacity
/// ([`BlockGeometry::capacity`]: Neo's `TVB_BLOCK_SLOTS`, M5's inventory). The capacity
/// arguments are S's (linux-aurora t6050-gpu, "integrate M5 Pro firmware").
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct Cursors {
    pub(crate) total: u32,
    pub(crate) committed: u32,
    pub(crate) read: u32,
}
impl Cursors {
    fn valid(self, capacity: usize) -> bool {
        [self.total, self.committed, self.read]
            .into_iter()
            .all(|v| (v as usize) < capacity)
    }
    /// Firmware-paused growth permits consumption of the already committed
    /// prefix. Retired-host growth requires the complete snapshot unchanged.
    pub(crate) fn permits_capacity(self, after: Self, firmware_request: bool, capacity: usize) -> bool {
        if !self.valid(capacity)
            || !after.valid(capacity)
            || self.total != after.total
            || self.committed != after.committed
        {
            return false;
        }
        if !firmware_request {
            return self == after;
        }
        let capacity = capacity as u32;
        (after.read + capacity - self.read) % capacity
            <= (self.committed + capacity - self.read) % capacity
    }
}

/// Checked block-table indices for one bulk allocation. One ring slot stays
/// empty even when all physical backing has been retained by the owner.
pub(crate) struct Growth {
    capacity: usize,
    before: Cursors,
    added: usize,
    retained: usize,
    target: usize,
}
impl Growth {
    pub(crate) fn with_capacity(before: Cursors, added: usize, capacity: usize) -> Result<Self> {
        if capacity < 2 || capacity > u32::MAX as usize
            || !before.valid(capacity) || before.total != before.committed || added == 0 {
            return Err(EINVAL);
        }
        let retained = (before.total as usize + capacity - before.read as usize) % capacity;
        if retained.checked_add(added).ok_or(EOVERFLOW)? >= capacity {
            return Err(ENOSPC);
        }
        let target = (before.total as usize + added) % capacity;
        Ok(Self {
            capacity,
            before,
            added,
            retained,
            target,
        })
    }
    pub(crate) fn target(&self) -> u32 {
        self.target as u32
    }
    pub(crate) fn retained(&self) -> usize {
        self.retained
    }
    pub(crate) fn available(&self) -> usize {
        self.retained + self.added
    }
    pub(crate) fn added_slot(&self, index: usize) -> Result<usize> {
        if index >= self.added {
            return Err(EINVAL);
        }
        Ok((self.before.total as usize + index) % self.capacity)
    }
    pub(crate) fn available_slot(&self, index: usize) -> Result<usize> {
        if index >= self.available() {
            return Err(EINVAL);
        }
        Ok((self.before.read as usize + index) % self.capacity)
    }
}

/// The high-water sample estimates capacity, not current occupancy. A growth
/// reserves three times the demand only once twice that demand no longer fits.
/// `pages` is the geometry's pages per block and `maximum` the layout's normal
/// maximum in blocks (Neo: `TVB_PAGES_PER_BLOCK`, `NORMAL_MAX_BLOCKS`).
fn automatic_target_for(used_pages: usize, current: usize, pages: usize, maximum: usize) -> Option<usize> {
    if current >= maximum {
        return None;
    }
    let need = used_pages
        .saturating_mul(2)
        .div_ceil(pages)
        .min(maximum);
    if need <= current {
        return None;
    }
    Some(
        used_pages
            .saturating_mul(3)
            .div_ceil(pages)
            .min(maximum),
    )
}

use super::object::{CpuMap, KernelObject};
use crate::{driver::AsahiDrmDevice, mmu};
use core::ops::Range;

pub(crate) fn compact_alias(
    object: &KernelObject,
    vm: &mmu::Vm,
    source: Range<usize>,
    align: u64,
    prot: mmu::Prot,
    guard: usize,
) -> Result<mmu::KernelMapping> {
    for range in cfg::compact_ranges() {
        match object.map_range(vm, source.clone(), range, align, prot, guard) {
            Ok(mapping) => return Ok(mapping),
            Err(error) if error == ENOSPC => {}
            Err(error) => return Err(error),
        }
    }
    Err(ENOSPC)
}

/// Allocated aliases remain private until the owner installs them in its VM
/// cache. On rollback mappings are destroyed before backing memory.
pub(crate) struct TvbGrowth {
    mappings: KVec<mmu::KernelMapping>,
    addresses: KVec<u64>,
    extension: KernelObject,
}

/// Aliases and backing of growth extensions a shrink removed from an idle
/// heap. Field order unmaps the aliases before freeing their backing.
pub(crate) struct TvbRelease {
    _mappings: KVec<mmu::KernelMapping>,
    _extensions: KVec<KernelObject>,
}

/// Data blocks of the profile's geometry ([`BlockGeometry`]): the A18 Pro's packed 128-KiB
/// blocks at guarded 160-KiB virtual strides, M5's 512-KiB ones at 544 KiB.
/// Firmware owns their contents; CPU zeroing occurs only on fresh backing.
pub(crate) struct Tvb {
    geometry: BlockGeometry,
    addresses: KVec<u64>,
    // Retain sparse-block backing until the pool and its aliases retire.
    _primary: KernelObject,
    extensions: KVec<KernelObject>,
}
impl Tvb {
    fn map_block(
        object: &KernelObject,
        vm: &mmu::Vm,
        relative: usize,
        geometry: BlockGeometry,
    ) -> Result<mmu::KernelMapping> {
        let start = relative.checked_mul(geometry.backing_stride).ok_or(EOVERFLOW)?;
        let source = start..start.checked_add(geometry.block_bytes).ok_or(EOVERFLOW)?;
        let guard = geometry.stride - geometry.block_bytes;
        if geometry == BlockGeometry::NEO {
            let mapping =
                compact_alias(object, vm, source, cfg::TVB_PAGE, mmu::PROT_GPU_SHARED_RW, guard)?;
            Self::first_page(geometry, mapping.iova())?;
            return Ok(mapping);
        }
        let mapping = object.map_range(vm, source, geometry.base..geometry.end,
            BlockGeometry::PAGE as u64, mmu::PROT_GPU_SHARED_RW, guard)?;
        Self::first_page(geometry, mapping.iova())?;
        Ok(mapping)
    }
    /// The block's first page in the firmware's page numbering: Neo's compact
    /// pages from `USER_BASE`, M5's from its compact window.
    fn first_page(geometry: BlockGeometry, address: u64) -> Result<u32> {
        if geometry == BlockGeometry::NEO {
            fw::compact_page(address)
        } else {
            geometry.compact_page(address)
        }
    }
    pub(crate) fn new(
        dev: &AsahiDrmDevice,
        vm: &mmu::Vm,
        blocks: usize,
    ) -> Result<(Self, KVec<mmu::KernelMapping>)> {
        Self::with_geometry(dev, vm, blocks, BlockGeometry::NEO)
    }
    pub(crate) fn with_geometry(
        dev: &AsahiDrmDevice,
        vm: &mmu::Vm,
        blocks: usize,
        geometry: BlockGeometry,
    ) -> Result<(Self, KVec<mmu::KernelMapping>)> {
        if blocks == 0 || blocks > geometry.max_blocks() {
            return Err(ERANGE);
        }
        let backing = KernelObject::backing(dev, geometry.backing_bytes(blocks)?, CpuMap::WriteCombined)?;
        let mut mappings = KVec::with_capacity(blocks, GFP_KERNEL)?;
        let mut addresses = KVec::with_capacity(blocks, GFP_KERNEL)?;
        for index in 0..blocks {
            let mapping = Self::map_block(&backing, vm, index, geometry)?;
            addresses.push(mapping.iova(), GFP_KERNEL)?;
            mappings.push(mapping, GFP_KERNEL)?;
        }
        Ok((
            Self {
                geometry,
                addresses,
                _primary: backing,
                extensions: KVec::new(),
            },
            mappings,
        ))
    }
    pub(crate) fn blocks(&self) -> usize {
        self.addresses.len()
    }
    pub(crate) fn addresses(&self) -> &[u64] {
        &self.addresses
    }
    pub(crate) fn geometry(&self) -> BlockGeometry {
        self.geometry
    }
    /// Blocks of each growth extension, laid out like the primary at the
    /// geometry's backing stride ([`BlockGeometry::backing_bytes`] inverted).
    fn extension_blocks(&self, extension: &KernelObject) -> usize {
        let geometry = self.geometry;
        extension
            .size()
            .checked_sub(geometry.block_bytes)
            .map_or(0, |rest| rest / geometry.backing_stride + 1)
    }
    /// Blocks of the primary backing, which a heap never gives back.
    fn primary_blocks(&self) -> usize {
        self.extensions
            .iter()
            .fold(self.blocks(), |blocks, extension| {
                blocks.saturating_sub(self.extension_blocks(extension))
            })
    }

    pub(crate) fn prepare_growth(
        &mut self,
        dev: &AsahiDrmDevice,
        vm: &mmu::Vm,
        target: usize,
    ) -> Result<Option<TvbGrowth>> {
        if target <= self.blocks() {
            return Ok(None);
        }
        // Neo: `TVB_MAX_BLOCKS`; M5: its inventory less the empty ring slot.
        if target > self.geometry.max_blocks() {
            return Err(ERANGE);
        }
        self.reserve_growth(target - self.blocks())?;
        Self::allocate_growth(dev, vm, self.geometry, self.blocks(), target)
    }
    /// Room for one extension of `added` blocks in the heap's own lists.
    fn reserve_growth(&mut self, added: usize) -> Result {
        self.extensions.reserve(1, GFP_KERNEL)?;
        self.addresses.reserve(added, GFP_KERNEL)?;
        Ok(())
    }
    /// Backs and maps the blocks that take a heap of `geometry` from `blocks` to `target`,
    /// without the heap: an M5 pair stays in place while this runs off the device mutex
    /// ([`Manager::accept_growth`] adds it to the heap under the mutex).
    pub(crate) fn allocate_growth(
        dev: &AsahiDrmDevice,
        vm: &mmu::Vm,
        geometry: BlockGeometry,
        blocks: usize,
        target: usize,
    ) -> Result<Option<TvbGrowth>> {
        if target <= blocks {
            return Ok(None);
        }
        if target > geometry.max_blocks() {
            return Err(ERANGE);
        }
        let added = target - blocks;
        let backing = KernelObject::backing(dev, geometry.backing_bytes(added)?, CpuMap::WriteCombined)?;
        let mut mappings = KVec::with_capacity(added, GFP_KERNEL)?;
        let mut addresses = KVec::with_capacity(added, GFP_KERNEL)?;
        for relative in 0..added {
            let mapping = Self::map_block(&backing, vm, relative, geometry)?;
            addresses.push(mapping.iova(), GFP_KERNEL)?;
            mappings.push(mapping, GFP_KERNEL)?;
        }
        Ok(Some(TvbGrowth {
            mappings,
            addresses,
            extension: backing,
        }))
    }
}

use super::object::Allocator;
use core::sync::atomic::{fence, AtomicU32, AtomicU64, Ordering};
use kernel::sync::Arc;

/// First-free metrics entries are shared by all parameter-buffer graphs.
/// An entry returns only when the graph and its firmware references retire.
pub(crate) struct MetricsIds {
    used: [AtomicU64; 2],
    scene_stride: usize,
}
impl MetricsIds {
    pub(crate) fn new() -> Self {
        Self {
            used: [AtomicU64::new(0), AtomicU64::new(0)],
            scene_stride: size_of::<u32>(),
        }
    }
    pub(crate) fn with_stride(scene_stride: usize) -> Result<Self> {
        if scene_stride < 4 || scene_stride % 4 != 0 { return Err(EINVAL); }
        scene_stride.checked_mul(fw::SCENES).ok_or(EOVERFLOW)?;
        Ok(Self { scene_stride, ..Self::new() })
    }
    /// Backing required for every entry owned by this allocator.
    pub(crate) fn storage_bytes(&self) -> Result<usize> {
        self.scene_stride.checked_mul(fw::SCENES)
            .and_then(|bytes| bytes.checked_mul(self.used.len() * u64::BITS as usize))
            .ok_or(EOVERFLOW)
    }
}

pub(crate) struct MetricsLease {
    _backing: Arc<KernelObject>,
    allocator: Arc<MetricsIds>,
    slot: usize,
    addresses: fw::MetricsAddresses,
    scene_stride: usize,
}
impl MetricsLease {
    pub(crate) fn addresses(&self) -> fw::MetricsAddresses { self.addresses }
    pub(crate) fn set_client_base(&mut self, base: u64) -> Result {
        let offset = self.slot.checked_mul(self.scene_stride)
            .and_then(|bytes| bytes.checked_mul(fw::SCENES)).ok_or(EOVERFLOW)?;
        self.addresses.client = base.checked_add(offset as u64).ok_or(EOVERFLOW)?;
        Ok(())
    }
    /// Clears only the selected retired metrics entry before reusing it.
    pub(crate) fn new(
        ids: &Arc<MetricsIds>,
        object: &Arc<KernelObject>,
        bases: fw::MetricsAddresses,
    ) -> Result<Self> {
        let scene_stride = ids.scene_stride;
        let bytes = scene_stride.checked_mul(fw::SCENES).ok_or(EOVERFLOW)?;
        let slots = (object.size() / bytes).min(ids.used.len() * 64);
        let mut selected = None;
        for (bank, used) in ids.used.iter().enumerate() {
            let mut value = used.load(Ordering::Acquire);
            loop {
                let bit = value.trailing_ones();
                if bit == 64 || bank * 64 + bit as usize >= slots {
                    break;
                }
                match used.compare_exchange_weak(
                    value,
                    value | (1u64 << bit),
                    Ordering::AcqRel,
                    Ordering::Acquire,
                ) {
                    Ok(_) => {
                        selected = Some(bank * 64 + bit as usize);
                        break;
                    }
                    Err(current) => value = current,
                }
            }
            if selected.is_some() {
                break;
            }
        }
        let slot = selected.ok_or_else(|| super::capacity::refused(super::capacity::Site::MetricsIds))?;
        // Construct the lease before any fallible address/view validation so
        // every constructor failure returns the selected bit.
        let mut lease = Self {
            _backing: object.clone(),
            allocator: ids.clone(),
            slot,
            addresses: fw::MetricsAddresses::default(),
            scene_stride,
        };
        let offset = slot * bytes;
        lease.addresses = fw::MetricsAddresses {
            firmware: bases.firmware.checked_add(offset as u64).ok_or(EOVERFLOW)?,
            client: bases.client.checked_add(offset as u64).ok_or(EOVERFLOW)?,
        };
        let words = object.pointer(offset, bytes)?.cast::<u32>();
        object.word(offset)?;
        for index in 0..bytes / size_of::<u32>() {
            // SAFETY: The complete naturally aligned metrics entry is inside
            // the retained object; the exclusive lease excludes other owners.
            unsafe { words.add(index).write_volatile(0) };
        }
        Ok(lease)
    }
}
impl Drop for MetricsLease {
    fn drop(&mut self) {
        self.allocator.used[self.slot / 64]
            .fetch_and(!(1u64 << (self.slot % 64)), Ordering::Release);
    }
}

pub(crate) struct ManagerArgs {
    pub(crate) scratch: u64,
    pub(crate) discard: u64,
    pub(crate) buffer_id: u8,
    pub(crate) generation: u64,
    pub(crate) first_ordinal: u64,
}

/// A retained parameter-buffer graph and all of its sparse backing. Client
/// mappings live in the VM's owner cache; graph replacement never returns a
/// published backing early merely because an error fence was signalled.
pub(crate) struct Manager {
    _page_list: mmu::KernelMapping,
    graph: KernelObject,
    layout: layout::Layout,
    tvb: Tvb,
    metrics: MetricsLease,
    scenes: Scenes,
    page_list_client: u64,
    scratch: u64,
    buffer_id: u8,
    generation: u64,
    shrink: Shrink,
    /// A tiling pass initialized this buffer manager (25G83 InitBM published).
    initialized: bool,
}

/// Native tiler-heap shrink policy, matching the heap behaviour seen
/// under macOS. Once per
/// period the page high-water folds into an average,
/// `max((sample + 4 * average) / 5, sample)`. Each period whose average
/// stays below a quarter of the capacity earns one 512 KiB block of shrink,
/// and a heap idle for ten seconds returns to its minimum. Only an idle heap
/// is rebuilt here, by whole growth extensions.
///
/// Unlike macOS, the minimum keeps what the recent work needs: never below
/// the size at which the automatic growth check would grow the heap again
/// for the largest high-water sample of the last ten active periods. A
/// shrink then never makes the next pass at the recent load grow back.
const SHRINK_PERIOD_NS: u64 = 1_000_000_000;
const SHRINK_IDLE_NS: u64 = 10_000_000_000;
/// Blocks in one native 512 KiB parameter-buffer block.
const SHRINK_STEP_BLOCKS: usize = 0x8_0000 / cfg::TVB_BLOCK_SIZE;
/// Active periods in the recent-load window.
const SHRINK_WINDOW: usize = (SHRINK_IDLE_NS / SHRINK_PERIOD_NS) as usize;

#[derive(Default)]
struct Shrink {
    last_busy: u64,
    period_start: u64,
    /// High-water samples since the last period and the last growth check.
    period_max: u32,
    growth_max: u32,
    average: u32,
    credit: usize,
    /// Blocks the most recent pass layout required.
    required: usize,
    /// High-water samples of the last active periods (with a sample or a
    /// pass) and their largest value. Idle periods leave both unchanged.
    window: [u32; SHRINK_WINDOW],
    window_next: usize,
    busy_peak: u32,
}

pub(crate) struct Submitted<'a> {
    word: &'a AtomicU32,
    next: u32,
}
impl Submitted<'_> {
    pub(crate) fn commit(self) {
        self.word.store(self.next, Ordering::Relaxed);
        fence(Ordering::SeqCst);
    }
}

impl Manager {
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        vm: &mmu::Vm,
        tvb: Tvb,
        metrics: MetricsLease,
        args: ManagerArgs,
    ) -> Result<Self> {
        let geometry = tvb.geometry;
        let neo = geometry == BlockGeometry::NEO;
        let layout = if neo { layout::Layout::NEO } else { layout::Layout::m5(geometry)? };
        if metrics.scene_stride != layout.metric_stride {
            return Err(EINVAL);
        }
        let mut graph = alloc.kernel(
            layout.bytes,
            cfg::TVB_PAGE,
            mmu::PROT_GPU_FW_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        let source = layout.pages..layout.pages + layout.page_bytes;
        let page_list = if neo {
            compact_alias(&graph, vm, source, cfg::TVB_PAGE, mmu::PROT_GPU_FW_SHARED_RW, mmu::UAT_PGSZ)?
        } else {
            graph.map_range(vm, source, geometry.base..geometry.end, cfg::TVB_PAGE,
                mmu::PROT_GPU_FW_SHARED_RW, mmu::UAT_PGSZ)?
        };
        let address = graph.gpu_va();
        let graph_args = fw::GraphArgs {
            fw_va: address,
            page_list_client: page_list.iova(),
            metrics: metrics.addresses,
            blocks: tvb.addresses(),
            scratch: args.scratch,
            discard: args.discard,
            buffer_id: args.buffer_id.into(),
            generation: args.generation,
        };
        if neo {
            graph.initialize::<fw::Graph>(0, |view| view.init(&graph_args))?;
        } else {
            layout.initialize(geometry, &graph_args, graph.bytes_mut())?;
        }
        fence(Ordering::SeqCst);
        let mut scenes = Scenes::new();
        scenes.commit(args.first_ordinal, fw::FIRST_SCENE)?;
        let page_list_client = page_list.iova();
        Ok(Self {
            _page_list: page_list,
            graph,
            layout,
            tvb,
            metrics,
            scenes,
            page_list_client,
            scratch: args.scratch,
            buffer_id: args.buffer_id,
            generation: args.generation,
            shrink: Shrink::default(),
            initialized: false,
        })
    }

    /// Whether a published tiling pass initialized this manager (25G83 InitBM).
    pub(crate) fn initialized(&self) -> bool {
        self.initialized
    }

    pub(crate) fn mark_initialized(&mut self) {
        self.initialized = true;
    }
    pub(crate) fn buffer_id(&self) -> u8 {
        self.buffer_id
    }
    pub(crate) fn blocks(&self) -> usize {
        self.tvb.blocks()
    }
    /// Read-only diagnostic while firmware is halted. In particular this
    /// does not reset the high-water counter as automatic_target() does.
    pub(crate) fn report_recovery(&self) {
        let read = |offset| -> Result<u32> {
            Ok(self.graph.word(offset)?.load(Ordering::Relaxed))
        };
        pr_warn!("G17: recovery tvb id={} blocks={} cursors={:?} pages={:?} capacity={:?} producer={:?} consumer={:?} max_pages={:?} reset={:?} initialized={}\n",
            self.buffer_id, self.blocks(), self.cursors(),
            read(core::mem::offset_of!(fw::State, pages)),
            read(core::mem::offset_of!(fw::State, block_capacity)),
            read(core::mem::offset_of!(fw::State, producer)),
            read(core::mem::offset_of!(fw::State, consumer)),
            read(self.layout.control + core::mem::offset_of!(fw::Control, max_pages)),
            read(self.layout.control + core::mem::offset_of!(fw::Control, reset)),
            self.initialized);
    }
    pub(crate) fn state_va(&self) -> u64 {
        self.graph.gpu_va()
    }
    pub(crate) fn geometry(&self) -> BlockGeometry {
        self.tvb.geometry()
    }
    pub(crate) fn page_list_va(&self) -> u64 {
        self.page_list_client
    }
    pub(crate) fn trailer_va(&self) -> u64 {
        self.graph.gpu_va() + self.layout.scenes as u64 + (fw::SCENES * size_of::<fw::Scene>()) as u64
    }
    pub(crate) fn scene_va(&self, index: usize) -> Result<u64> {
        if index >= fw::SCENES {
            return Err(EINVAL);
        }
        Ok(self.graph.gpu_va() + self.layout.scenes as u64 + (index * size_of::<fw::Scene>()) as u64)
    }
    pub(crate) fn graph(&self) -> &KernelObject {
        &self.graph
    }
    /// The scene's scratch offset and metric pointer register values. M5's are offsets from the
    /// compact window's base, the metric pointer tagged in bit 0.
    pub(crate) fn scene_registers(&self, index: usize) -> Result<(u64, u64)> {
        let geometry = self.tvb.geometry;
        if geometry == BlockGeometry::NEO {
            return fw::scene_registers(index, self.scratch, self.metrics.addresses.client);
        }
        if index >= fw::SCENES { return Err(EINVAL); }
        let scratch = self.scratch.checked_sub(geometry.base).ok_or(EINVAL)?
            .checked_add((index % self.layout.scratch_slots * self.layout.scratch_stride) as u64)
            .ok_or(EOVERFLOW)?;
        let metric = self.metrics.addresses.client.checked_sub(geometry.base).ok_or(EINVAL)?
            .checked_add((index * self.layout.metric_stride) as u64).ok_or(EOVERFLOW)?;
        if scratch > u64::from(u32::MAX) || metric > u64::from(u32::MAX) || metric & 3 != 0 {
            return Err(EINVAL);
        }
        Ok((scratch, metric | 1))
    }
    pub(crate) fn prepare_scene(&mut self, ordinal: u64) -> Result<usize> {
        let scene = self.scenes.select(ordinal)?;
        let offset = self.layout.scenes + scene * size_of::<fw::Scene>();
        let used = self
            .graph
            .word(offset + core::mem::offset_of!(fw::Scene, used))?;
        let generation = self
            .graph
            .pointer(offset + core::mem::offset_of!(fw::Scene, generation), 8)?;
        let id = self
            .graph
            .word(core::mem::offset_of!(fw::State, buffer_id))?;
        used.store(0, Ordering::Relaxed);
        // SAFETY: The complete packed field was checked before any write.
        // This scene is exclusively retained or still unclaimed.
        unsafe { generation.cast::<u64>().write_unaligned(self.generation) };
        id.store(u32::from(self.buffer_id), Ordering::Relaxed);
        fence(Ordering::SeqCst);
        Ok(scene)
    }
    pub(crate) fn claim_scene(&mut self, ordinal: u64, scene: usize) -> Result {
        self.scenes.commit(ordinal, scene)
    }
    pub(crate) fn retire_scene(&mut self, ordinal: u64) -> Result {
        self.scenes.release(ordinal)
    }
    pub(crate) fn prepare_submission(&mut self) -> Result<Submitted<'_>> {
        let word = self.graph.word(self.layout.counter)?;
        Ok(Submitted {
            word,
            next: word.load(Ordering::Relaxed).wrapping_add(1),
        })
    }
    /// Reads the firmware page high-water and requests its reset. Both the
    /// growth check and the shrink period see every sample.
    fn sample_max_pages(&mut self) -> Result<u32> {
        let max_pages = self
            .graph
            .word(self.layout.control + core::mem::offset_of!(fw::Control, max_pages))?;
        let reset = self
            .graph
            .word(self.layout.control + core::mem::offset_of!(fw::Control, reset))?;
        fence(Ordering::Acquire);
        let pages = max_pages.load(Ordering::Relaxed);
        fence(Ordering::Release);
        reset.store(1, Ordering::Relaxed);
        self.shrink.period_max = self.shrink.period_max.max(pages);
        self.shrink.growth_max = self.shrink.growth_max.max(pages);
        Ok(pages)
    }
    pub(crate) fn automatic_target(&mut self) -> Result<Option<usize>> {
        self.sample_max_pages()?;
        let pages = core::mem::take(&mut self.shrink.growth_max);
        Ok(automatic_target_for(pages as usize, self.blocks(), self.tvb.geometry.pages(),
            self.layout.normal_blocks))
    }
    /// The size a firmware growth request grows to: `GROW_BLOCKS` more, within
    /// the geometry's inventory (S `Manager::firmware_target`).
    pub(crate) fn firmware_target(&self) -> usize {
        self.blocks().saturating_add(fw::GROW_BLOCKS).min(self.tvb.geometry.max_blocks())
    }
    /// Records the blocks a pass layout requires, the floor of any shrink.
    /// A pass is being prepared at `now`; the heap counts as busy.
    pub(crate) fn note_required(&mut self, blocks: usize, now: u64) {
        self.shrink.required = blocks;
        self.shrink.last_busy = now;
    }
    /// The smallest heap a shrink may leave: the primary backing, the blocks
    /// of the last pass layout, and the size below which the automatic growth
    /// check would grow again for the recent peak.
    fn shrink_floor(&self) -> usize {
        let recent = (self.shrink.busy_peak as usize)
            .saturating_mul(2)
            .div_ceil(cfg::TVB_PAGES_PER_BLOCK);
        self.tvb.primary_blocks().max(self.shrink.required).max(recent)
    }
    /// Whether the heap holds growth beyond its primary backing.
    pub(crate) fn grown(&self) -> bool {
        !self.tvb.extensions.is_empty()
    }
    /// Whether the heap holds growth that a shrink could give back. The shrink
    /// policy is Neo's (`TVB_PAGES_PER_BLOCK`, the Neo layout); an M5 heap keeps
    /// its growth until its pair is rebuilt.
    pub(crate) fn shrinkable(&self) -> bool {
        self.tvb.geometry == BlockGeometry::NEO
            && self.tvb.extensions.last().is_some_and(|extension| {
                self.blocks() - self.tvb.extension_blocks(extension) >= self.shrink_floor()
            })
    }
    /// Advances the shrink policy at `now` and returns the block count an idle
    /// heap should shrink to. `busy` is true while the pair has work in flight
    /// or is borrowed; a busy heap only updates its average and recent peak.
    /// Call it for every grown heap, so the recent peak can also fall.
    pub(crate) fn shrink_target(&mut self, now: u64, busy: bool) -> Result<Option<usize>> {
        if busy || self.shrink.last_busy == 0 {
            self.shrink.last_busy = now;
        }
        if self.shrink.period_start == 0 {
            self.shrink.period_start = now;
        }
        if now.saturating_sub(self.shrink.period_start) >= SHRINK_PERIOD_NS {
            self.sample_max_pages()?;
            let sample = core::mem::take(&mut self.shrink.period_max);
            let average = (u64::from(sample) + 4 * u64::from(self.shrink.average)) / 5;
            self.shrink.average = (average as u32).max(sample);
            if sample != 0 || self.shrink.last_busy >= self.shrink.period_start {
                let next = self.shrink.window_next;
                self.shrink.window[next] = sample;
                self.shrink.window_next = (next + 1) % SHRINK_WINDOW;
                self.shrink.busy_peak = self.shrink.window.iter().copied().max().unwrap_or(0);
            }
            self.shrink.period_start = now;
            let capacity = self.blocks() * cfg::TVB_PAGES_PER_BLOCK;
            if (self.shrink.average as usize).saturating_mul(4) < capacity {
                self.shrink.credit = self.shrink.credit.saturating_add(SHRINK_STEP_BLOCKS);
            } else {
                self.shrink.credit = 0;
            }
        }
        if !self.shrinkable() {
            self.shrink.credit = 0;
            return Ok(None);
        }
        if busy {
            return Ok(None);
        }
        if now.saturating_sub(self.shrink.last_busy) >= SHRINK_IDLE_NS {
            return Ok(Some(self.shrink_floor()));
        }
        let last = self.tvb.extensions.last().map_or(0, |extension| self.tvb.extension_blocks(extension));
        Ok((last != 0 && self.shrink.credit >= last).then(|| self.blocks() - last))
    }
    fn cursors(&self) -> Result<Cursors> {
        Ok(Cursors {
            total: self.graph.word(self.layout.control)?.load(Ordering::Relaxed),
            committed: self.graph.word(self.layout.control + 4)?.load(Ordering::Relaxed),
            read: self.graph.word(self.layout.control + 8)?.load(Ordering::Relaxed),
        })
    }
}

/// All allocations and inventory words prepared without publishing shared state.
/// The pair remains exclusively borrowed until commit or discard.
pub(crate) struct PreparedGrowth {
    owner: u64,
    previous: usize,
    target: usize,
    before: Cursors,
    plan: Growth,
    growth: TvbGrowth,
    added: KVec<(usize, u32)>,
    rebuilt: KVec<u32>,
    pages: u32,
    descriptor: Option<[u32; 4]>,
}
impl Manager {
    /// Validate every retained sparse block and its reserved trailing guard in
    /// one VM snapshot before constructing more aliases.
    pub(crate) fn validate_vm(&self, vm: &mmu::Vm) -> Result {
        let geometry = self.tvb.geometry;
        let minimum = if geometry == BlockGeometry::NEO { cfg::TVB_INITIAL_PAGES.len() } else { 1 };
        if self.blocks() < minimum
            || self.blocks() > geometry.max_blocks()
            || !vm.covers_sparse_blocks_with_guards(
                self.tvb.addresses(),
                geometry.block_bytes as u64,
                geometry.stride as u64,
            )
        {
            return Err(EFAULT);
        }
        Ok(())
    }

    /// Allocate growth and precompute its words while the pair is exclusively
    /// borrowed. A descriptor snapshot requests idle-host growth; None leaves
    /// the firmware-owned page list and PB state untouched.
    pub(crate) fn prepare_growth(
        &mut self,
        dev: &AsahiDrmDevice,
        vm: &mmu::Vm,
        target: usize,
        descriptor: Option<[u32; 4]>,
    ) -> Result<Option<PreparedGrowth>> {
        let previous = self.blocks();
        if target <= previous {
            return Ok(None);
        }
        self.validate_vm(vm)?;
        let Some(growth) = self.tvb.prepare_growth(dev, vm, target)? else {
            return Ok(None);
        };
        self.plan_growth(previous, target, &mut Some(growth), descriptor).map(Some)
    }

    /// M5: adds `growth`, allocated off the device mutex for a heap of `from` blocks
    /// ([`Tvb::allocate_growth`]), to `target`: a firmware-request growth without `descriptor`,
    /// an idle-host growth with the buffer's descriptor row (as [`Self::prepare_growth`]). The
    /// heap must still hold `from` blocks. `growth` is taken only once the plan is complete, so
    /// a refused one stays with the caller, to be dropped off the mutex.
    pub(crate) fn accept_growth(
        &mut self,
        vm: &mmu::Vm,
        growth: &mut Option<TvbGrowth>,
        from: usize,
        target: usize,
        descriptor: Option<[u32; 4]>,
    ) -> Result<PreparedGrowth> {
        let added = growth.as_ref().ok_or(EINVAL)?.addresses.len();
        if self.blocks() != from || target.checked_sub(from) != Some(added) {
            return Err(EBUSY);
        }
        self.validate_vm(vm)?;
        self.tvb.reserve_growth(added)?;
        self.plan_growth(from, target, growth, descriptor)
    }

    /// The words a growth of `previous` blocks to `target` writes, with `growth`'s blocks. It
    /// takes `growth` last, when nothing more can fail.
    fn plan_growth(
        &self,
        previous: usize,
        target: usize,
        growth: &mut Option<TvbGrowth>,
        descriptor: Option<[u32; 4]>,
    ) -> Result<PreparedGrowth> {
        let geometry = self.tvb.geometry;
        let block_pages = geometry.pages();
        let before = self.cursors()?;
        let plan = Growth::with_capacity(before, target - previous, geometry.capacity)?;
        let firmware_request = descriptor.is_none();
        let pages = u32::try_from(target.checked_mul(block_pages).ok_or(EOVERFLOW)?)?;
        let addresses = &growth.as_ref().ok_or(EINVAL)?.addresses;
        let mut added = KVec::with_capacity(addresses.len(), GFP_KERNEL)?;
        for (index, &address) in addresses.iter().enumerate() {
            let first = Tvb::first_page(geometry, address)?;
            first
                .checked_add(block_pages as u32 - 1)
                .ok_or(EOVERFLOW)?;
            added.push((plan.added_slot(index)?, first), GFP_KERNEL)?;
        }
        let rebuilt_count = if firmware_request {
            0
        } else {
            plan.available() * block_pages
        };
        let mut rebuilt = KVec::with_capacity(rebuilt_count, GFP_KERNEL)?;
        if !firmware_request {
            if self.cursors()? != before {
                return Err(EAGAIN);
            }
            for ordinal in 0..plan.available() {
                let first = if ordinal < plan.retained() {
                    let slot = plan.available_slot(ordinal)?;
                    self.graph
                        .word(self.layout.blocks + slot * size_of::<fw::Block>())?
                        .load(Ordering::Relaxed)
                } else {
                    added[ordinal - plan.retained()].1
                };
                first
                    .checked_add(block_pages as u32 - 1)
                    .ok_or(EOVERFLOW)?;
                for page in 0..block_pages {
                    rebuilt.push(first + page as u32, GFP_KERNEL)?;
                }
            }
        }
        let descriptor =
            descriptor.map(|old| fw::reload_descriptor(old, self.page_list_client, pages));
        Ok(PreparedGrowth {
            owner: self.graph.gpu_va(),
            previous,
            target,
            before,
            plan,
            growth: growth.take().ok_or(EINVAL)?,
            added,
            rebuilt,
            pages,
            descriptor,
        })
    }

    /// The caller holds the device mutex after revalidating its preparation
    /// epoch. The final alias installation and all firmware writes occur under
    /// that same exclusion. The returned PB row is committed there as well.
    pub(crate) fn commit_growth(
        &mut self,
        prepared: &mut Option<PreparedGrowth>,
        install: impl FnOnce(usize, usize, &mut KVec<mmu::KernelMapping>) -> Result,
    ) -> Result<Option<[u32; 4]>> {
        let candidate = prepared.as_mut().ok_or(EINVAL)?;
        if self.graph.gpu_va() != candidate.owner || self.blocks() != candidate.previous {
            return Err(EBUSY);
        }
        let firmware_request = candidate.descriptor.is_none();
        let layout = self.layout;
        let base = self.graph.pointer(0, layout.bytes)?.cast::<u32>();
        // Bounds, actual alignment and every reserved host entry are checked
        // before transferring aliases or consuming the caller-owned plan.
        self.graph.word(0)?;
        if self.tvb.extensions.len() == self.tvb.extensions.capacity()
            || candidate.growth.addresses.len()
                > self.tvb.addresses.capacity() - self.tvb.addresses.len()
        {
            return Err(EIO);
        }
        if !candidate.before.permits_capacity(self.cursors()?, firmware_request,
            self.tvb.geometry.capacity)
        {
            return Err(EAGAIN);
        }
        // The callback retains its input on error. The caller can therefore
        // drop every rejected mapping/backing after releasing the device lock.
        install(
            candidate.previous,
            candidate.target,
            &mut candidate.growth.mappings,
        )?;
        if !candidate.growth.mappings.is_empty() {
            return Err(EIO);
        }
        let PreparedGrowth {
            before,
            plan,
            growth,
            added,
            rebuilt,
            pages,
            descriptor,
            ..
        } = prepared.take().ok_or(EIO)?;
        let TvbGrowth {
            addresses,
            extension,
            ..
        } = growth;

        // From here all vector capacity, indices, addresses and CPU views have
        // been checked. Retain backing before publishing its block entries.
        for address in addresses.into_iter() {
            append_reserved(&mut self.tvb.addresses, address);
        }
        let write = |offset: usize, value: u32| {
            // SAFETY: The full graph and base alignment were checked above.
            // Every offset is an aligned field or a checked array index of
            // Graph. No mutable reference aliases firmware-owned fields.
            unsafe { AtomicU32::from_ptr(base.add(offset / 4)).store(value, Ordering::Relaxed) };
        };
        for (slot, first) in added.into_iter() {
            let at = layout.blocks + slot * size_of::<fw::Block>();
            write(at, first);
            write(at + 4, 0);
        }
        if descriptor.is_some() {
            for (entry, value) in rebuilt.into_iter().enumerate() {
                write(layout.pages + entry * 4, value);
            }
            write(core::mem::offset_of!(fw::State, pages), pages);
            write(core::mem::offset_of!(fw::State, producer), plan.target());
            write(core::mem::offset_of!(fw::State, consumer), before.read);
            write(core::mem::offset_of!(fw::State, last_page), pages - 1);
        }
        append_reserved(&mut self.tvb.extensions, extension);
        write(layout.control, plan.target());
        fence(Ordering::SeqCst);
        write(layout.control + 4, plan.target());
        fence(Ordering::SeqCst);
        Ok(descriptor)
    }
}

impl Manager {
    /// Rebuilds an idle heap with whole trailing growth extensions removed,
    /// keeping at least `target` blocks. The kept blocks are laid out as
    /// `Graph::init` lays out a fresh heap, starting at the firmware read
    /// cursor. This runs only while every block is back in the inventory, as
    /// an idle-host growth requires the complete snapshot unchanged.
    ///
    /// `truncate(blocks, kept, out)` moves the removed blocks' client aliases
    /// out of the VM cache. Returns the reloaded PB row and the released
    /// aliases and backing, which the caller frees after the device mutex.
    /// None leaves the heap, the firmware records and the VM unchanged.
    pub(crate) fn shrink(
        &mut self,
        target: usize,
        descriptor: [u32; 4],
        truncate: impl FnOnce(usize, usize, &mut KVec<mmu::KernelMapping>) -> Result,
    ) -> Result<Option<([u32; 4], TvbRelease)>> {
        // The rebuild below writes Neo's layout (see `shrinkable`).
        if self.tvb.geometry != BlockGeometry::NEO {
            return Ok(None);
        }
        let blocks = self.blocks();
        let mut kept = blocks;
        let mut dropped = 0;
        for extension in self.tvb.extensions.iter().rev() {
            let size = self.tvb.extension_blocks(extension);
            if kept - size < target.max(self.tvb.primary_blocks()) {
                break;
            }
            kept -= size;
            dropped += 1;
        }
        if dropped == 0 {
            return Ok(None);
        }
        let before = self.cursors()?;
        let retained = (before.total as usize + cfg::TVB_BLOCK_SLOTS - before.read as usize)
            % cfg::TVB_BLOCK_SLOTS;
        if !before.valid(cfg::TVB_BLOCK_SLOTS) || before.total != before.committed || retained != blocks {
            return Ok(None);
        }
        // Allocation failure only postpones the shrink to a later pass.
        let (Ok(mappings), Ok(extensions)) = (
            KVec::with_capacity(blocks - kept, GFP_NOWAIT),
            KVec::with_capacity(dropped, GFP_NOWAIT),
        ) else {
            return Ok(None);
        };
        let mut release = TvbRelease {
            _mappings: mappings,
            _extensions: extensions,
        };
        for &address in &self.tvb.addresses[..kept] {
            fw::compact_page(address)?
                .checked_add(cfg::TVB_PAGES_PER_BLOCK as u32 - 1)
                .ok_or(EOVERFLOW)?;
        }
        let base = self.graph.pointer(0, fw::SIZE)?.cast::<u32>();
        self.graph.word(0)?;
        // The callback moves aliases only after checking the cached owner,
        // its block count and the spare capacity.
        truncate(blocks, kept, &mut release._mappings)?;

        // From here every index, address and CPU view has been checked.
        let write = |offset: usize, value: u32| {
            // SAFETY: The full graph and base alignment were checked above.
            // Every offset is an aligned field or a checked array index of
            // Graph. No mutable reference aliases firmware-owned fields.
            unsafe { AtomicU32::from_ptr(base.add(offset / 4)).store(value, Ordering::Relaxed) };
        };
        for (index, &address) in self.tvb.addresses[..kept].iter().enumerate() {
            // The same pure conversion succeeded for every kept block above.
            let Ok(first) = fw::compact_page(address) else {
                continue;
            };
            let slot = (before.read as usize + index) % cfg::TVB_BLOCK_SLOTS;
            let at = fw::BLOCK_TABLE + slot * size_of::<fw::Block>();
            write(at, first);
            write(at + 4, 0);
            for page in 0..cfg::TVB_PAGES_PER_BLOCK {
                let entry = index * cfg::TVB_PAGES_PER_BLOCK + page;
                write(fw::PAGE_LIST + entry * 4, first + page as u32);
            }
        }
        let total = ((before.read as usize + kept) % cfg::TVB_BLOCK_SLOTS) as u32;
        let pages = (kept * cfg::TVB_PAGES_PER_BLOCK) as u32;
        write(core::mem::offset_of!(fw::State, pages), pages);
        write(core::mem::offset_of!(fw::State, producer), total);
        write(core::mem::offset_of!(fw::State, consumer), before.read);
        write(core::mem::offset_of!(fw::State, last_page), pages - 1);
        write(fw::CONTROL, total);
        fence(Ordering::SeqCst);
        write(fw::CONTROL + 4, total);
        fence(Ordering::SeqCst);

        self.tvb.addresses.truncate(kept);
        for _ in 0..dropped {
            if let Some(extension) = self.tvb.extensions.pop() {
                append_reserved(&mut release._extensions, extension);
            }
        }
        self.shrink.credit = self.shrink.credit.saturating_sub(blocks - kept);
        Ok(Some((
            fw::reload_descriptor(descriptor, self.page_list_client, pages),
            release,
        )))
    }
}

/// Caller reserves all needed entries before any firmware-visible mutation.
fn append_reserved<T>(values: &mut KVec<T>, value: T) {
    let length = values.len();
    values.spare_capacity_mut()[0].write(value);
    // SAFETY: All callers preflighted this spare entry and initialized it once.
    unsafe { values.set_len(length + 1) };
}

impl BufferIds {
    pub(crate) fn write_capacity(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(f, "parameter_buffers used={} total={} owned={} referenced={}",
                 self.entries.iter().filter(|entry| !entry.free()).count(), BUFFER_IDS,
                 self.entries.iter().filter(|entry| entry.owner).count(),
                 self.entries.iter().filter(|entry| entry.references != 0).count())
    }
}
impl MetricsIds {
    /// Entries leased; the backing object bounds how many exist ([`MetricsLease::new`]).
    pub(crate) fn used(&self) -> u32 {
        self.used.iter().map(|bits| bits.load(Ordering::Acquire).count_ones()).sum::<u32>()
    }
}

#[kunit_tests(asahi_g17_buffer_capacity)]
mod capacity_tests {
    use super::*;

    #[test]
    fn failed_build_and_fault_retirement_return_parameter_buffers() {
        let mut ids = KBox::init(BufferIds::new(), GFP_KERNEL).unwrap();
        for _ in 0..8 {
            let mut clients = KVec::with_capacity(BUFFER_IDS, GFP_KERNEL).unwrap();
            for _ in 0..BUFFER_IDS {
                clients.push(ids.allocate_render().unwrap().0, GFP_KERNEL).unwrap();
            }
            assert_eq!(ids.allocate_render(), Err(ENOSPC));
            for (index, id) in clients.into_iter().enumerate() {
                if index % 2 == 0 {
                    // Failed constructor never installed the graph owner.
                    ids.release(id).unwrap();
                } else {
                    ids.reserve_owner(id).unwrap();
                    ids.release(id).unwrap();
                    ids.acquire_owner(id).unwrap();
                    // Close cannot return a still-owned failed command.
                    ids.unreserve_owner(id).unwrap();
                    assert!(!ids.entries[id as usize].free());
                    ids.release(id).unwrap();
                }
            }
            assert!(ids.entries.iter().all(|entry| entry.free()));
            assert_eq!(ids.returned_count, BUFFER_IDS);
        }
    }
}

/// The growth arithmetic at Neo's sizes (where it must equal the fixed-slot
/// arithmetic it replaced) and at an M5 inventory (a measured 32-block pair).
#[kunit_tests(asahi_g17_buffer_growth)]
mod growth_tests {
    use super::*;

    const M5_CAPACITY: usize = 33 * 294;
    const M5_PAGES: usize = 0x80000 / BlockGeometry::PAGE;

    #[test]
    fn neo_geometry_is_the_fixed_constants() {
        let neo = BlockGeometry::NEO;
        assert_eq!(neo.capacity, cfg::TVB_BLOCK_SLOTS);
        assert_eq!(neo.max_blocks(), cfg::TVB_MAX_BLOCKS);
        assert_eq!(neo.pages(), cfg::TVB_PAGES_PER_BLOCK);
        assert_eq!(neo.block_bytes, cfg::TVB_BLOCK_SIZE);
        assert_eq!(neo.stride as u64, cfg::TVB_BLOCK_STRIDE);
        let layout = layout::Layout::NEO;
        assert_eq!(layout.blocks, fw::BLOCK_TABLE);
        assert_eq!(layout.pages, fw::PAGE_LIST);
        assert_eq!(layout.control, fw::CONTROL);
        assert_eq!(layout.bytes, fw::SIZE);
        assert_eq!(layout.normal_blocks, fw::NORMAL_MAX_BLOCKS);
    }

    #[test]
    fn neo_growth_wraps_at_the_block_slots() {
        let slots = cfg::TVB_BLOCK_SLOTS;
        let at = |total: usize, read: usize| Cursors {
            total: total as u32,
            committed: total as u32,
            read: read as u32,
        };
        let plan = Growth::with_capacity(at(slots - 2, 5), 3, slots).unwrap();
        assert_eq!(plan.retained(), slots - 7);
        assert_eq!(plan.target(), 1);
        assert_eq!(plan.added_slot(0).unwrap(), slots - 2);
        assert_eq!(plan.added_slot(2).unwrap(), 0);
        assert_eq!(plan.available_slot(plan.available() - 1).unwrap(), 0);
        // One ring slot stays empty.
        assert_eq!(Growth::with_capacity(at(4, 5), 1, slots).err(), Some(ENOSPC));
        assert_eq!(Growth::with_capacity(at(slots, 0), 1, slots).err(), Some(EINVAL));
        // The automatic target of the fixed-constant function.
        assert_eq!(automatic_target_for(100, 32, cfg::TVB_PAGES_PER_BLOCK, fw::NORMAL_MAX_BLOCKS),
            Some(75));
        assert_eq!(automatic_target_for(60, 32, cfg::TVB_PAGES_PER_BLOCK, fw::NORMAL_MAX_BLOCKS),
            None);
        assert_eq!(automatic_target_for(usize::MAX / 4, 0, cfg::TVB_PAGES_PER_BLOCK,
            fw::NORMAL_MAX_BLOCKS), Some(fw::NORMAL_MAX_BLOCKS));
    }

    #[test]
    fn m5_growth_uses_the_inventory() {
        let at = |total: usize, read: usize| Cursors {
            total: total as u32,
            committed: total as u32,
            read: read as u32,
        };
        // A measured pair: 32 blocks, read 0. Past Neo's slot count, which the old
        // arithmetic would have refused as invalid cursors.
        let plan = Growth::with_capacity(at(0x20, 0), 103, M5_CAPACITY).unwrap();
        assert_eq!(plan.target(), 135);
        assert_eq!(plan.available(), 135);
        let far = cfg::TVB_BLOCK_SLOTS + 10;
        let plan = Growth::with_capacity(at(far, far - 32), 8, M5_CAPACITY).unwrap();
        assert_eq!(plan.added_slot(7).unwrap(), far + 7);
        assert_eq!(Growth::with_capacity(at(M5_CAPACITY - 1, 0), 1, M5_CAPACITY).err(),
            Some(ENOSPC));
        // Measured high-water samples: 467 pages (exact) and 717 (restart), on 32
        // blocks of 16 pages, with the layout's normal maximum (capacity / 3).
        let normal = M5_CAPACITY / 3;
        assert_eq!(automatic_target_for(467, 32, M5_PAGES, normal), Some(88));
        assert_eq!(automatic_target_for(717, 32, M5_PAGES, normal), Some(135));
        assert_eq!(automatic_target_for(200, 32, M5_PAGES, normal), None);
    }

    #[test]
    fn firmware_paused_growth_checks_the_capacity() {
        let before = Cursors { total: 40, committed: 40, read: 30 };
        let consumed = Cursors { read: 38, ..before };
        let past = Cursors { read: 41, ..before };
        assert!(before.permits_capacity(consumed, true, M5_CAPACITY));
        assert!(!before.permits_capacity(past, true, M5_CAPACITY));
        assert!(!before.permits_capacity(consumed, false, M5_CAPACITY));
        assert!(before.permits_capacity(before, false, M5_CAPACITY));
        let beyond = Cursors { total: 40, committed: 40, read: M5_CAPACITY as u32 };
        assert!(!before.permits_capacity(beyond, true, M5_CAPACITY));
        // At Neo's slot count, M5-sized cursors are invalid, as before.
        let big = Cursors { total: 5000, committed: 5000, read: 4990 };
        assert!(!big.permits_capacity(big, false, cfg::TVB_BLOCK_SLOTS));
        assert!(big.permits_capacity(big, false, M5_CAPACITY));
    }
}
