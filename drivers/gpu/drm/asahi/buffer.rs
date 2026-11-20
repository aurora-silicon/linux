// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Tiled Vertex Buffer management
//!
//! This module manages the Tiled Vertex Buffer, also known as the Parameter Buffer (in imgtec
//! parlance) or the tiler heap (on other architectures). This buffer holds transformed primitive
//! data between the vertex/tiling stage and the fragment stage.
//!
//! On AGX, the buffer is a heap of 128K blocks split into 32K pages (which must be aligned to a
//! multiple of 32K in VA space). The buffer can be shared between multiple render jobs, and each
//! will allocate pages from it during vertex processing and return them during fragment processing.
//!
//! If the buffer runs out of free pages, the vertex pass stops and a partial fragment pass occurs,
//! spilling the intermediate render target state to RAM (a partial render). This is all managed
//! transparently by the firmware. Since partial renders are less efficient, the kernel must grow
//! the heap in response to feedback from the firmware to avoid partial renders in the future.
//! Currently, we only ever grow the heap, and never shrink it.
//!
//! AGX also supports memoryless render targets, which can be used for intermediate results within
//! a render pass. To support partial renders, it seems the GPU/firmware has the ability to borrow
//! pages from the TVB buffer as a temporary render target buffer. Since this happens during a
//! partial render itself, if the buffer runs out of space, it requires synchronous growth in
//! response to a firmware interrupt. This is not currently supported, but may be in the future,
//! though it is unclear whether it is worth the effort.
//!
//! This module is also in charge of managing the temporary objects associated with a single render
//! pass, which includes the top-level tile array, the tail pointer cache, preemption buffers, and
//! other miscellaneous structures collectively managed as a "scene".
//!
//! To avoid runaway memory usage, there is a maximum size for buffers (at that point it's unlikely
//! that partial renders will incur much overhead over the buffer data access itself). This is
//! different depending on whether memoryless render targets are in use, and is currently hardcoded.
//! to the most common value used by macOS.

use crate::debug::*;
use crate::fw::buffer;
use crate::fw::types::*;
use crate::util::*;
use crate::{
    alloc,
    fw,
    gpu,
    hw,
    mmu,
    slotalloc, //
};
use core::sync::atomic::Ordering;
use kernel::new_mutex;
use kernel::prelude::*;
use kernel::sync::{
    Arc,
    Mutex, //
};
use kernel::{
    c_str,
    static_lock_class, //
};

const DEBUG_CLASS: DebugFlags = DebugFlags::Buffer;

/// There are 127 GPU/firmware-side buffer manager slots (yes, 127, not 128).
const NUM_BUFFERS: u32 = 127;

/// Page size bits for buffer pages (32K). VAs must be aligned to this size.
pub(crate) const PAGE_SHIFT: usize = 15;
/// Page size for buffer pages.
pub(crate) const PAGE_SIZE: usize = 1 << PAGE_SHIFT;
/// Number of pages in a buffer block, which should be contiguous in VA space.
pub(crate) const PAGES_PER_BLOCK: usize = 4;
/// Size of a buffer block.
pub(crate) const BLOCK_SIZE: usize = PAGE_SIZE * PAGES_PER_BLOCK;

/// Metadata about the tiling configuration for a scene. This is computed in the `render` module.
/// based on dimensions, tile size, and other info.
pub(crate) struct TileInfo {
    /// Tile count in the X dimension. Tiles are always 32x32.
    pub(crate) tiles_x: u32,
    /// Tile count in the Y dimension. Tiles are always 32x32.
    pub(crate) tiles_y: u32,
    /// Total tile count.
    pub(crate) tiles: u32,
    /// Micro-tile width (16 or 32).
    pub(crate) utile_width: u32,
    /// Micro-tile height (16 or 32).
    pub(crate) utile_height: u32,
    // Macro-tiles in the X dimension. Always 4.
    //pub(crate) mtiles_x: u32,
    // Macro-tiles in the Y dimension. Always 4.
    //pub(crate) mtiles_y: u32,
    /// Tiles per macro-tile in the X dimension.
    pub(crate) tiles_per_mtile_x: u32,
    /// Tiles per macro-tile in the Y dimension.
    pub(crate) tiles_per_mtile_y: u32,
    // Total tiles per macro-tile.
    //pub(crate) tiles_per_mtile: u32,
    /// Micro-tiles per macro-tile in the X dimension.
    pub(crate) utiles_per_mtile_x: u32,
    /// Micro-tiles per macro-tile in the Y dimension.
    pub(crate) utiles_per_mtile_y: u32,
    // Total micro-tiles per macro-tile.
    //pub(crate) utiles_per_mtile: u32,
    /// Size of the top-level tilemap, in bytes (for all layers, one cluster).
    pub(crate) tilemap_size: usize,
    /// Size of the Tail Pointer Cache, in bytes (for all layers * clusters).
    pub(crate) tpc_size: usize,
    /// Number of blocks in the clustering meta buffer (for clustering) per layer.
    pub(crate) meta1_layer_stride: u32,
    /// Number of blocks in the clustering meta buffer (for clustering).
    pub(crate) meta1_blocks: u32,
    /// Layering metadata size.
    pub(crate) layermeta_size: usize,
    /// Minimum number of TVB blocks for this render.
    pub(crate) min_tvb_blocks: usize,
    /// Tiling parameter structure passed to firmware.
    pub(crate) params: fw::vertex::raw::TilingParameters,
}

/// A single scene, representing a render pass and its required buffers.
#[versions(AGX_ROOT)]
#[derive(Debug)]
pub(crate) struct Scene {
    object: GpuObject<buffer::Scene::ver>,
    slot: u32,
    rebind: bool,
    preempt2_off: usize,
    preempt3_off: usize,
    // Note: these are dead code only on some version variants.
    // It's easier to do this than to propagate the version conditionals everywhere.
    #[allow(dead_code)]
    meta1_off: usize,
    #[allow(dead_code)]
    meta2_off: usize,
    #[allow(dead_code)]
    meta3_off: usize,
    #[allow(dead_code)]
    meta4_off: usize,
}

#[versions(AGX_ROOT)]
impl Scene::ver {
    /// Returns true if the buffer was bound to a fresh manager slot, and therefore needs an init
    /// command before a render.
    pub(crate) fn rebind(&self) -> bool {
        self.rebind
    }

    /// Returns the buffer manager slot this scene's buffer was bound to.
    pub(crate) fn slot(&self) -> u32 {
        self.slot
    }

    /// Returns the GPU pointer to the [`buffer::Scene::ver`].
    pub(crate) fn gpu_pointer(&self) -> GpuPointer<'_, buffer::Scene::ver> {
        self.object.gpu_pointer()
    }

    /// Returns the GPU weak pointer to the [`buffer::Scene::ver`].
    pub(crate) fn weak_pointer(&self) -> GpuWeakPointer<buffer::Scene::ver> {
        self.object.weak_pointer()
    }

    /// Returns the GPU weak pointer to the kernel-side temp buffer.
    /// (purpose unknown...)
    pub(crate) fn kernel_buffer_pointer(&self) -> GpuWeakPointer<[u8]> {
        self.object.buffer.inner.lock().kernel_buffer.weak_pointer()
    }

    /// Returns the GPU pointer to the `buffer::Info::ver` object associated with this Scene.
    pub(crate) fn buffer_pointer(&self) -> GpuPointer<'_, buffer::Info::ver> {
        // SAFETY: We can't return the strong pointer directly since its lifetime crosses a lock,
        // but we know its lifetime will be valid as long as &self since we hold a reference to the
        // buffer, so just construct the strong pointer with the right lifetime here.
        unsafe { self.weak_buffer_pointer().upgrade() }
    }

    /// Returns the GPU weak pointer to the `buffer::Info::ver` object associated with this Scene.
    pub(crate) fn weak_buffer_pointer(&self) -> GpuWeakPointer<buffer::Info::ver> {
        self.object.buffer.inner.lock().info.weak_pointer()
    }

    /// Returns the GPU pointer to the TVB heap metadata buffer.
    pub(crate) fn tvb_heapmeta_pointer(&self) -> GpuPointer<'_, &'_ [u8]> {
        self.object.tvb_heapmeta.gpu_pointer()
    }

    /// Returns the GPU pointer to the layer metadata buffer.
    pub(crate) fn tvb_layermeta_pointer(&self) -> GpuPointer<'_, &'_ [u8]> {
        self.object.tvb_heapmeta.gpu_offset_pointer(0x200)
    }

    /// Returns the GPU pointer to the top-level TVB tilemap buffer.
    pub(crate) fn tvb_tilemap_pointer(&self) -> GpuPointer<'_, &'_ [u8]> {
        self.object.tvb_tilemap.gpu_pointer()
    }

    /// Returns the GPU pointer to the Tail Pointer Cache buffer.
    pub(crate) fn tpc_pointer(&self) -> GpuPointer<'_, &'_ [u8]> {
        self.object.tpc.gpu_pointer()
    }

    /// Returns the GPU pointer to the first preemption scratch buffer.
    pub(crate) fn preempt_buf_1_pointer(&self) -> GpuPointer<'_, &'_ [u8]> {
        self.object.preempt_buf.gpu_pointer()
    }

    /// Returns the GPU pointer to the second preemption scratch buffer.
    pub(crate) fn preempt_buf_2_pointer(&self) -> GpuPointer<'_, &'_ [u8]> {
        self.object
            .preempt_buf
            .gpu_offset_pointer(self.preempt2_off)
    }

    /// Returns the GPU pointer to the third preemption scratch buffer.
    pub(crate) fn preempt_buf_3_pointer(&self) -> GpuPointer<'_, &'_ [u8]> {
        self.object
            .preempt_buf
            .gpu_offset_pointer(self.preempt3_off)
    }

    /// Returns the GPU pointer to the per-cluster tilemap buffer, if clustering is enabled.
    #[allow(dead_code)]
    pub(crate) fn cluster_tilemaps_pointer(&self) -> Option<GpuPointer<'_, &'_ [u8]>> {
        self.object
            .clustering
            .as_ref()
            .map(|c| c.tilemaps.gpu_pointer())
    }

    /// Returns the GPU pointer to the clustering layer metadata buffer, if clustering is enabled.
    #[allow(dead_code)]
    pub(crate) fn tvb_cluster_layermeta_pointer(&self) -> Option<GpuPointer<'_, &'_ [u8]>> {
        self.object
            .clustering
            .as_ref()
            .map(|c| c.meta.gpu_pointer())
    }

    /// Returns the GPU pointer to the clustering metadata 1 buffer, if clustering is enabled.
    #[allow(dead_code)]
    pub(crate) fn meta_1_pointer(&self) -> Option<GpuPointer<'_, &'_ [u8]>> {
        self.object
            .clustering
            .as_ref()
            .map(|c| c.meta.gpu_offset_pointer(self.meta1_off))
    }

    /// Returns the GPU pointer to the clustering metadata 2 buffer, if clustering is enabled.
    #[allow(dead_code)]
    pub(crate) fn meta_2_pointer(&self) -> Option<GpuPointer<'_, &'_ [u8]>> {
        self.object
            .clustering
            .as_ref()
            .map(|c| c.meta.gpu_offset_pointer(self.meta2_off))
    }

    /// Returns the GPU pointer to the clustering metadata 3 buffer, if clustering is enabled.
    #[allow(dead_code)]
    pub(crate) fn meta_3_pointer(&self) -> Option<GpuPointer<'_, &'_ [u8]>> {
        self.object
            .clustering
            .as_ref()
            .map(|c| c.meta.gpu_offset_pointer(self.meta3_off))
    }

    /// Returns the GPU pointer to the clustering metadata 4 buffer, if clustering is enabled.
    #[allow(dead_code)]
    pub(crate) fn meta_4_pointer(&self) -> Option<GpuPointer<'_, &'_ [u8]>> {
        self.object
            .clustering
            .as_ref()
            .map(|c| c.meta.gpu_offset_pointer(self.meta4_off))
    }
}

#[versions(AGX_ROOT)]
impl Drop for Scene::ver {
    fn drop(&mut self) {
        let mut inner = self.object.buffer.inner.lock();
        assert_ne!(inner.active_scenes, 0);
        inner.active_scenes -= 1;

        // 27.2: a buffer keeps its slot for its lifetime. The hardware's descriptor for a slot
        // is only rebuilt cleanly for a buffer it has not served before.
        #[ver(V >= V27_2)]
        let release = false;
        #[ver(V < V27_2)]
        let release = true;
        if release && inner.active_scenes == 0 {
            mod_pr_debug!(
                "Buffer: no scenes left, dropping slot {}",
                inner.active_slot.take().unwrap().slot()
            );
            inner.active_slot = None;
        }
    }
}

/// Inner data for a single TVB buffer object.
#[versions(AGX_ROOT)]
struct BufferInner {
    info: GpuObject<buffer::Info::ver>,
    ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
    ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
    blocks: KVec<GpuOnlyArray<u8>>,
    max_blocks: usize,
    max_blocks_nomemless: usize,
    mgr: BufferManager::ver,
    active_scenes: usize,
    active_slot: Option<slotalloc::Guard<BufferSlotInner::ver>>,
    last_token: Option<slotalloc::SlotToken>,
    tpc: Option<Arc<GpuArray<u8>>>,
    kernel_buffer: GpuArray<u8>,
    stats: GpuObject<buffer::Stats>,
    cfg: &'static hw::HwConfig,
    preempt1_size: usize,
    preempt2_size: usize,
    preempt3_size: usize,
    num_clusters: usize,
}

/// 27.2: CPU address of the hardware PB descriptor table (16 bytes per buffer slot), published
/// by the backend that owns it.
static PB_DESCRIPTORS: core::sync::atomic::AtomicPtr<u8> =
    core::sync::atomic::AtomicPtr::new(core::ptr::null_mut());

/// Publishes the PB descriptor table's CPU address (0x4000 bytes).
pub(crate) fn set_pb_descriptors(table: *mut u8) {
    PB_DESCRIPTORS.store(table, Ordering::Release);
}

/// Diagnostic: logs the non-zero PB descriptor table entries (27.2).
pub(crate) fn dump_pb_descriptors(dev: &kernel::device::Device) {
    let table = PB_DESCRIPTORS.load(Ordering::Acquire);
    if table.is_null() {
        return;
    }
    for slot in 0..64usize {
        // SAFETY: the table is a live 0x4000-byte kernel mapping.
        let d: [u32; 4] = unsafe {
            let p = table.add(slot * 16).cast::<u32>();
            crate::pgtable::clean_invalidate_poc(p.cast::<u8>(), 16);
            crate::pgtable::poc_barrier();
            core::array::from_fn(|i| p.add(i).read_volatile())
        };
        if d.iter().any(|w| *w != 0) {
            dev_info!(dev, "PB descriptor[{}] = {:08x?}\n", slot, d);
        }
    }
}

/// 27.2: the page lists of all tile buffers, one region per buffer slot, mapped at the same
/// address in every client address space that renders (and once for the firmware). The
/// hardware keeps tiling with the last committed descriptor until the next setup command
/// commits another, and touches that list in whatever address space it switches to: with
/// every list mapped everywhere, a stale one is always readable.
#[pin_data]
pub(crate) struct PageListArena {
    kmap: mmu::KernelMapping,
    stride: usize,
    slots: usize,
    client_va: core::sync::atomic::AtomicU64,
    /// The arena object and its client mappings (address space id, uses, mapping).
    #[pin]
    maps: Mutex<(crate::gem::ObjectRef, KVec<(u64, u32, mmu::KernelMapping)>)>,
}

// SAFETY: the object and mappings are only used through the mutex or immutable addresses.
unsafe impl Send for PageListArena {}
// SAFETY: as above.
unsafe impl Sync for PageListArena {}

static ARENA: core::sync::atomic::AtomicPtr<PageListArena> =
    core::sync::atomic::AtomicPtr::new(core::ptr::null_mut());

fn arena() -> Option<&'static PageListArena> {
    let p = ARENA.load(Ordering::Acquire);
    // SAFETY: published once from a leaked allocation that lives until module unload.
    if p.is_null() { None } else { Some(unsafe { &*p }) }
}

/// Bytes of one slot's page list.
fn page_list_stride() -> usize {
    let max_blocks = 862_322_688 / BLOCK_SIZE;
    (max_blocks * PAGES_PER_BLOCK * 4 + mmu::UAT_PGMSK) & !mmu::UAT_PGMSK
}

/// Creates the page list arena for `slots` buffer slots, mapped for the firmware in `kvm`.
pub(crate) fn init_page_list_arena(dev: &crate::driver::AsahiDrmDevice, kvm: &mmu::Vm, kprot: mmu::Prot,
    slots: usize) -> Result {
    if !ARENA.load(Ordering::Acquire).is_null() {
        return Ok(());
    }
    let stride = page_list_stride();
    let mut obj = crate::gem::new_kernel_object(dev, stride * slots)?;
    {
        let vmap = obj.vmap()?;
        use kernel::io::Io;
        for off in (0..stride * slots).step_by(4) { vmap.try_write32(0, off)?; }
    }
    let kmap = obj.map_into_range(kvm, PAGE_LIST_KERNEL_RANGE, mmu::UAT_PGSZ as u64, kprot, true)?;
    let arena = KBox::pin_init(try_pin_init!(PageListArena {
        kmap,
        stride,
        slots,
        client_va: core::sync::atomic::AtomicU64::new(0),
        maps <- kernel::new_mutex!((obj, KVec::new())),
    }), GFP_KERNEL)?;
    // SAFETY: the arena is never moved out of its allocation, which is leaked.
    let raw = KBox::into_raw(unsafe { core::pin::Pin::into_inner_unchecked(arena) });
    ARENA.store(raw, Ordering::Release);
    Ok(())
}

/// Maps the arena into a client address space (counted per address space).
pub(crate) fn arena_acquire(vm: &mmu::Vm, range: core::ops::Range<u64>, prot: mmu::Prot) -> Result {
    let Some(a) = arena() else { return Err(ENODEV) };
    let size = (a.stride * a.slots) as u64;
    let mut guard = a.maps.lock();
    let (obj, maps) = &mut *guard;
    if let Some(e) = maps.iter_mut().find(|e| e.0 == vm.id()) {
        e.1 += 1;
        return Ok(());
    }
    let want = (range.end.saturating_sub(0x100_0000 + size)) & !(mmu::UAT_PGMSK as u64);
    let va = match a.client_va.compare_exchange(0, want, Ordering::AcqRel, Ordering::Acquire) {
        Ok(_) => want,
        Err(v) => v,
    };
    if va < range.start || va + size > range.end || va + size > 1 << 40 {
        return Err(EINVAL);
    }
    let map = obj.map_at(vm, va, prot, true)?;
    maps.push((vm.id(), 1, map), GFP_KERNEL)?;
    Ok(())
}

/// Drops one use of the arena's mapping in `vm`.
pub(crate) fn arena_release(vm: &mmu::Vm) {
    let Some(a) = arena() else { return };
    let mut guard = a.maps.lock();
    let maps = &mut guard.1;
    if let Some(i) = maps.iter().position(|e| e.0 == vm.id()) {
        maps[i].1 -= 1;
        if maps[i].1 == 0 {
            let _ = maps.remove(i);
        }
    }
}

/// The firmware and client addresses of `slot`'s page list.
fn arena_addrs(slot: u32) -> Option<(u64, u64)> {
    let a = arena()?;
    if slot as usize >= a.slots { return None; }
    let va = a.client_va.load(Ordering::Acquire);
    if va == 0 { return None; }
    let off = (a.stride * slot as usize) as u64;
    Some((a.kmap.iova() + off, va + off))
}

/// Kernel addresses for 27.2 page lists (above the timestamp range, below MMIO).
const PAGE_LIST_KERNEL_RANGE: core::ops::Range<u64> = 0xffffffae20000000..0xffffffae80000000;

/// Locked and reference counted TVB buffer.
#[versions(AGX_ROOT)]
pub(crate) struct Buffer {
    inner: Arc<Mutex<BufferInner::ver>>,
}

#[versions(AGX_ROOT)]
impl Buffer::ver {
    /// Create a new Buffer for a given VM, given the per-VM allocators.
    pub(crate) fn new<'drm>(
        gpu: &'drm dyn gpu::GpuManager,
        alloc: &mut gpu::KernelAllocators,
        ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        mgr: &BufferManager::ver,
    ) -> Result<Buffer::ver> {
        // These are the typical max numbers on macOS.
        // 8GB machines have this halved.
        let max_size: usize = 862_322_688; // bytes
        let max_size_nomemless = max_size / 3;

        let max_blocks = max_size / BLOCK_SIZE;
        let max_blocks_nomemless = max_size_nomemless / BLOCK_SIZE;
        let max_pages = max_blocks * PAGES_PER_BLOCK;
        let max_pages_nomemless = max_blocks_nomemless * PAGES_PER_BLOCK;

        let num_clusters = gpu.get_dyncfg().id.num_clusters as usize;
        let num_clusters_adj = if num_clusters > 1 {
            num_clusters + 1
        } else {
            1
        };

        let preempt1_size = num_clusters_adj * gpu.get_cfg().preempt1_size;
        let preempt2_size = num_clusters_adj * gpu.get_cfg().preempt2_size;
        let preempt3_size = num_clusters_adj * gpu.get_cfg().preempt3_size;

        #[ver(V >= V27_2)]
        let (page_list, block_list) = (
            alloc.shared.array_empty_tagged(1, b"PLST")?,
            alloc.shared.array_empty_tagged(max_blocks * 2, b"BLST")?,
        );
        #[ver(V < V27_2)]
        let (page_list, block_list) = (
            ualloc_priv.lock().array_empty_tagged(max_pages, b"PLST")?,
            ualloc_priv.lock().array_empty_tagged(max_blocks * 2, b"BLST")?,
        );
        // 27.2: the page list pointers are set when the buffer gets its slot.
        let page_list_va = (0u64, 0u64);
        let _ = page_list_va;
        let shared = &mut alloc.shared;
        let info = alloc.private.new_init(
            {
                try_init!(buffer::Info::ver {
                    block_ctl: shared.new_default::<buffer::BlockControl>()?,
                    counter: shared.new_default::<buffer::Counter>()?,
                    page_list: page_list,
                    block_list: block_list,
                })
            },
            |inner, _p| {
                try_init!(buffer::raw::Info::ver {
                    gpu_counter: 0x0,
                    unk_4: 0,
                    last_id: 0x0,
                    cur_id: -1,
                    unk_10: 0x0,
                    gpu_counter2: 0x0,
                    #[ver(V < V27_2)]
                    unk_18: 0x0,
                    #[ver(V < V13_0B4 || G >= G14X)]
                    unk_1c: 0x0,
                    #[ver(V >= V27_2)]
                    page_list_fw: U64(page_list_va.0),
                    #[ver(V >= V27_2)]
                    page_list_gpu: U64(page_list_va.1),
                    #[ver(V < V27_2)]
                    page_list: inner.page_list.gpu_pointer(),
                    page_list_size: (4 * max_pages).try_into()?,
                    page_count: AtomicU32::new(0),
                    max_blocks: max_blocks.try_into()?,
                    block_count: AtomicU32::new(0),
                    unk_38: 0x0,
                    block_list: inner.block_list.gpu_pointer(),
                    block_ctl: inner.block_ctl.gpu_pointer(),
                    #[ver(V < V27_2)]
                    last_page: AtomicU32::new(0),
                    #[ver(V >= V27_2)]
                    last_page: AtomicU32::new(u32::MAX),
                    #[ver(V < V27_2)]
                    gpu_page_ptr1: 0x0,
                    #[ver(V < V27_2)]
                    gpu_page_ptr2: 0x0,
                    #[ver(V < V27_2)]
                    unk_58: 0x0,
                    block_size: BLOCK_SIZE as u32,
                    #[ver(V >= V27_2)]
                    unk_54: 0,
                    #[ver(V >= V27_2)]
                    unk_58_27: 0,
                    #[ver(V < V27_2)]
                    unk_60: U64(0x0),
                    counter: inner.counter.gpu_pointer(),
                    #[ver(V < V27_2)]
                    unk_70: 0x0,
                    #[ver(V < V27_2)]
                    unk_74: 0x0,
                    #[ver(V < V27_2)]
                    unk_78: 0x0,
                    #[ver(V < V27_2)]
                    unk_7c: 0x0,
                    #[ver(V < V27_2)]
                    unk_80: 0x1,
                    #[ver(V >= V27_2)]
                    memless: 0,
                    #[ver(V >= V27_2)]
                    unk_68: Default::default(),
                    max_pages: max_pages.try_into()?,
                    max_pages_nomemless: max_pages_nomemless.try_into()?,
                    #[ver(V < V27_2)]
                    unk_8c: 0x0,
                    #[ver(V < V27_2)]
                    unk_90: Default::default(),
                    #[ver(V >= V27_2)]
                    discard_page: U64(0),
                    #[ver(V >= V27_2)]
                    unk_84: 0,
                    #[ver(V >= V27_2)]
                    unk_88: Default::default(),
                })
            },
        )?;

        // Technically similar to Scene below, let's play it safe.
        let kernel_buffer = alloc.shared.array_empty_tagged(0x40, b"KBUF")?;
        let stats = alloc
            .shared
            .new_object(Default::default(), |_inner| buffer::raw::Stats {
                reset: AtomicU32::from(1),
                ..Default::default()
            })?;

        Ok(Buffer::ver {
            inner: Arc::pin_init(
                new_mutex!(BufferInner::ver {
                    info,
                    ualloc,
                    ualloc_priv,
                    blocks: KVec::new(),
                    max_blocks,
                    max_blocks_nomemless,
                    mgr: mgr.clone(),
                    active_scenes: 0,
                    active_slot: None,
                    last_token: None,
                    tpc: None,
                    kernel_buffer,
                    stats,
                    cfg: gpu.get_cfg(),
                    preempt1_size,
                    preempt2_size,
                    preempt3_size,
                    num_clusters,
                }),
                GFP_KERNEL,
            )?,
        })
    }

    /// 27.2: before a tiler pass, point the slot's descriptor entry at this buffer (its page
    /// list base and page count, from Info), keeping the hardware's ring positions; the host
    /// side of a buffer becoming active, as the hardware does not do it itself.
    pub(crate) fn load_pb_desc(&self, slot: u32) {
        #[ver(V >= V27_2)]
        {
            let table = PB_DESCRIPTORS.load(Ordering::Acquire);
            if table.is_null() || slot as usize >= 0x4000 / 16 {
                return;
            }
            let inner = self.inner.lock();
            let Some((_, list)) = arena_addrs(slot) else { return };
            let page_count = inner.info.with(|raw, _inner| raw.page_count.load(Ordering::Acquire));
            // SAFETY: the table is a live 0x4000-byte kernel mapping; nothing uses this slot while
            // the pass is being built (the tiler queue is serialized).
            unsafe {
                let p = table.add(slot as usize * 16).cast::<u32>();
                crate::pgtable::clean_invalidate_poc(p.cast::<u8>(), 16);
                crate::pgtable::poc_barrier();
                let d0 = p.read_volatile();
                let d4 = p.add(1).read_volatile();
                let d0 = (((list >> 4) as u32) & !7) | (d0 & 7);
                let d4 = ((((list >> 36) & 0xf) as u32) << 28) | (d4 & 0x0fc0_0000) | (page_count & 0x3f_ffff);
                p.write_volatile(d0);
                p.add(1).write_volatile(d4);
                crate::pgtable::clean_poc(p.cast::<u8>(), 16);
            }
            crate::pgtable::poc_barrier();
        }
        let _ = slot;
    }

    /// 27.2: make the next setup command rebuild this buffer's page list and commit its
    /// descriptor (the hardware runs a tiler pass with the last committed descriptor).
    pub(crate) fn recommit_pb(&self) {
        #[ver(V >= V27_2)]
        {
            let inner = self.inner.lock();
            if inner.active_scenes > 1 {
                return;
            }
            inner.info.with(|raw, _inner| {
                raw.page_count.store(0, Ordering::Relaxed);
                raw.block_count.store(0, Ordering::Relaxed);
                raw.last_page.store(u32::MAX, Ordering::Relaxed);
            });
        }
    }

    /// Diagnostic: (Counter.count, Info gpu_counter, Info+0x10, Info+8 slot).
    pub(crate) fn counters(&self) -> (u32, u32, u32, i32) {
        let inner = self.inner.lock();
        let count = inner.info.counter.with(|raw, _inner| raw.count.load(Ordering::Acquire));
        inner.info.with(|raw, _inner| (count, raw.gpu_counter, raw.unk_10, raw.last_id))
    }

    /// Diagnostic: the first 0xc0 bytes of the firmware-visible Info, as words.
    pub(crate) fn info_words(&self) -> [u32; 0x30] {
        let inner = self.inner.lock();
        inner.info.with(|raw, _inner| {
            let p = core::ptr::from_ref(raw).cast::<u32>();
            let n = (core::mem::size_of_val(raw) / 4).min(0x30);
            // SAFETY: `raw` is a live plain-data firmware struct of at least `n` words.
            core::array::from_fn(|i| if i < n { unsafe { p.add(i).read_volatile() } } else { 0 })
        })
    }

    /// Returns the total block count allocated to this Buffer.
    pub(crate) fn block_count(&self) -> u32 {
        self.inner.lock().blocks.len() as u32
    }

    /// Automatically grow the Buffer based on feedback from the statistics.
    pub(crate) fn auto_grow(&self) -> Result<bool> {
        let inner = self.inner.lock();

        let used_pages = inner.stats.with(|raw, _inner| {
            let used = raw.max_pages.load(Ordering::Relaxed);
            raw.reset.store(1, Ordering::Release);
            used as usize
        });

        let need_blocks = (used_pages * 2)
            .div_ceil(PAGES_PER_BLOCK)
            .min(inner.max_blocks_nomemless);
        let want_blocks = (used_pages * 3)
            .div_ceil(PAGES_PER_BLOCK)
            .min(inner.max_blocks_nomemless);

        let cur_count = inner.blocks.len();

        if need_blocks <= cur_count {
            Ok(false)
        } else {
            // Grow to 3x requested size (same logic as macOS)
            core::mem::drop(inner);
            self.ensure_blocks(want_blocks)?;
            Ok(true)
        }
    }

    /// Synchronously grow the Buffer.
    pub(crate) fn sync_grow(&self) {
        let inner = self.inner.lock();

        let cur_count = inner.blocks.len();
        core::mem::drop(inner);
        if self.ensure_blocks(cur_count + 10).is_err() {
            pr_err!("BufferManager: Failed to grow buffer synchronously\n");
        }
    }

    /// Ensure that the buffer has at least a certain minimum size in blocks.
    pub(crate) fn ensure_blocks(&self, min_blocks: usize) -> Result<bool> {
        let mut inner = self.inner.lock();

        let cur_count = inner.blocks.len();
        if cur_count >= min_blocks {
            return Ok(false);
        }
        if min_blocks > inner.max_blocks {
            return Err(ENOMEM);
        }

        let add_blocks = min_blocks - cur_count;
        let new_count = min_blocks;

        let mut new_blocks: KVec<GpuOnlyArray<u8>> = KVec::new();

        // Allocate the new blocks first, so if it fails they will be dropped
        let mut ualloc = inner.ualloc.lock();
        for _i in 0..add_blocks {
            new_blocks.push(ualloc.array_gpuonly(BLOCK_SIZE)?, GFP_KERNEL)?;
        }
        core::mem::drop(ualloc);

        // Then actually commit them
        inner.blocks.reserve(add_blocks, GFP_KERNEL)?;

        for (i, block) in new_blocks.into_iter().enumerate() {
            let page_num = (block.gpu_va().get() >> PAGE_SHIFT) as u32;

            inner
                .blocks
                .push(block, GFP_KERNEL)
                .expect("push() failed after reserve()");
            inner.info.block_list[2 * (cur_count + i)] = page_num;
            // 27.2: the firmware fills the page list from the block list.
            #[ver(V < V27_2)]
            for j in 0..PAGES_PER_BLOCK {
                inner.info.page_list[(cur_count + i) * PAGES_PER_BLOCK + j] = page_num + j as u32;
            }
        }

        inner.info.block_ctl.with(|raw, _inner| {
            raw.total.store(new_count as u32, Ordering::SeqCst);
            raw.wptr.store(new_count as u32, Ordering::SeqCst);
        });

        /* Only do this update if the buffer manager is idle (which means we own it) */
        // 27.2: the firmware advances the page count itself when the setup command sees new
        // blocks in the block control.
        #[ver(V >= V27_2)]
        let host_counts = false;
        #[ver(V < V27_2)]
        let host_counts = true;
        if host_counts && inner.active_scenes == 0 {
            let page_count = (new_count * PAGES_PER_BLOCK) as u32;
            inner.info.with(|raw, _inner| {
                raw.page_count.store(page_count, Ordering::Relaxed);
                raw.block_count.store(new_count as u32, Ordering::Relaxed);
                raw.last_page.store(page_count - 1, Ordering::Relaxed);
            });
        }

        Ok(true)
    }

    /// Create a new [`Scene::ver`] (render pass) using this buffer.
    pub(crate) fn new_scene(
        &self,
        alloc: &mut gpu::KernelAllocators,
        tile_info: &TileInfo,
    ) -> Result<Scene::ver> {
        let mut inner = self.inner.lock();

        let tilemap_size = tile_info.tilemap_size;
        let tpc_size = tile_info.tpc_size;

        // TODO: what is this exactly?
        mod_pr_debug!("Buffer: Allocating TVB buffers\n");

        // This seems to be a list, with 4x2 bytes of headers and 8 bytes per entry.
        // On single-cluster devices, the used length always seems to be 1.
        // On M1 Ultra, it can grow and usually doesn't exceed 64 entries.
        // macOS allocates a whole 64K * 0x80 for this, so let's go with
        // that to be safe...
        let user_buffer = inner.ualloc.lock().array_empty_tagged(
            if inner.num_clusters > 1 {
                0x10080
            } else {
                0x80
            },
            b"UBUF",
        )?;

        let tvb_heapmeta = inner
            .ualloc
            .lock()
            .array_empty_tagged(0x200 + tile_info.layermeta_size, b"HMTA")?;
        let tvb_tilemap = inner
            .ualloc
            .lock()
            .array_empty_tagged(tilemap_size, b"TMAP")?;

        mod_pr_debug!("Buffer: Allocating misc buffers\n");
        let preempt_buf = inner.ualloc.lock().array_empty_tagged(
            inner.preempt1_size + inner.preempt2_size + inner.preempt3_size,
            b"PRMT",
        )?;

        let tpc = match inner.tpc.as_ref() {
            Some(buf) if buf.len() >= tpc_size => buf.clone(),
            _ => {
                // MacOS allocates this as shared GPU+FW, but
                // priv seems to work and might be faster?
                // Needs to be FW-writable anyway, so ualloc
                // won't work.
                let buf = Arc::new(
                    inner.ualloc_priv.lock().array_empty_tagged(
                        (tpc_size + mmu::UAT_PGMSK) & !mmu::UAT_PGMSK,
                        b"TPC ",
                    )?,
                    GFP_KERNEL,
                )?;
                inner.tpc = Some(buf.clone());
                buf
            }
        };

        let mut clmeta_size = 0;
        let mut meta1_size = 0;
        let mut meta2_size = 0;
        let mut meta3_size = 0;

        let clustering = if inner.num_clusters > 1 {
            let cfg = inner.cfg.clustering.as_ref().unwrap();

            clmeta_size = tile_info.layermeta_size * cfg.max_splits;
            // Maybe: (4x4 macro tiles + 1 global page)*n, 32bit each (17*4*n)
            // Unused on t602x?
            meta1_size = align(tile_info.meta1_blocks as usize * cfg.meta1_blocksize, 0x80);
            meta2_size = align(cfg.meta2_size, 0x80);
            meta3_size = align(cfg.meta3_size, 0x80);
            let meta4_size = cfg.meta4_size;

            let meta_size = clmeta_size + meta1_size + meta2_size + meta3_size + meta4_size;

            mod_pr_debug!("Buffer: Allocating clustering buffers\n");
            let tilemaps = inner
                .ualloc
                .lock()
                .array_empty_tagged(cfg.max_splits * tilemap_size, b"CTMP")?;
            let meta = inner.ualloc.lock().array_empty_tagged(meta_size, b"CMTA")?;
            Some(buffer::ClusterBuffers { tilemaps, meta })
        } else {
            None
        };

        // Could be made strong, but we wind up with a deadlock if we try to grab the
        // pointer through the inner.buffer path inside the closure.
        let stats_pointer = inner.stats.weak_pointer();

        let _gpu = &mut alloc.gpu;

        // macOS allocates this as private. However, the firmware does not
        // DC CIVAC this before reading it (like it does most other things),
        // which causes odd cache incoherency bugs when combined with
        // speculation on the firmware side (maybe). This doesn't happen
        // on macOS because these structs are a circular pool that is mapped
        // already initialized. Just mark this shared for now.
        let scene = alloc.shared.new_init(
            try_init!(buffer::Scene::ver {
                user_buffer: user_buffer,
                buffer: self.clone(),
                tvb_heapmeta: tvb_heapmeta,
                tvb_tilemap: tvb_tilemap,
                tpc: tpc,
                clustering: clustering,
                preempt_buf: preempt_buf,
                #[ver(G >= G14X)]
                control_word: _gpu.array_empty_tagged(1, b"CWRD")?,
            }),
            |inner, _p| {
                try_init!(buffer::raw::Scene::ver {
                    #[ver(G >= G14X)]
                    control_word: inner.control_word.gpu_pointer(),
                    #[ver(G >= G14X)]
                    control_word2: inner.control_word.gpu_pointer(),
                    pass_page_count: AtomicU32::new(0),
                    unk_4: 0,
                    unk_8: U64(0),
                    unk_10: U64(0),
                    user_buffer: inner.user_buffer.gpu_pointer(),
                    #[ver(V < V27_2)]
                    unk_20: 0,
                    #[ver(V >= V13_3 && V < V27_2)]
                    unk_28: U64(0),
                    #[ver(V >= V27_2)]
                    unk_20_27: U64(0),
                    #[ver(V >= V27_2)]
                    skip: 0,
                    #[ver(V >= V27_2)]
                    fw_flag: 0,
                    stats: stats_pointer,
                    total_page_count: AtomicU32::new(0),
                    #[ver(V >= V27_2)]
                    tvb_overflow: 0,
                    #[ver(V >= V27_2)]
                    unk_40: U64(0),
                    #[ver(G < G14X && V < V27_2)]
                    unk_30: U64(0),
                    #[ver(G < G14X && V < V27_2)]
                    unk_38: U64(0),
                })
            },
        )?;

        let mut rebind = false;

        if inner.active_slot.is_none() {
            assert_eq!(inner.active_scenes, 0);

            // 27.2 buffers hold their slot for their lifetime, so the manager must not hold
            // them (it would keep them alive); 27.2 sends no grow requests that need it.
            #[ver(V >= V27_2)]
            let owner: Option<Buffer::ver> = None;
            #[ver(V < V27_2)]
            let owner = Some(self.clone());
            let fill = |inner: &mut BufferManagerInner::ver, mgr: &mut slotalloc::Guard<BufferSlotInner::ver>| {
                inner.owners[mgr.slot() as usize] = owner;
                Ok(())
            };
            // 27.2 slots are held for a buffer's lifetime: never wait for one (the caller may
            // hold locks a slot's release needs).
            #[ver(V >= V27_2)]
            let slot = inner.mgr.0.try_get_inner(inner.last_token, fill)?;
            #[ver(V < V27_2)]
            let slot = inner.mgr.0.get_inner(inner.last_token, fill)?;
            rebind = slot.changed();

            mod_pr_debug!("Buffer: assigning slot {} (rebind={})", slot.slot(), rebind);

            inner.last_token = Some(slot.token());
            #[ver(V >= V27_2)]
            {
                let id = slot.slot() as i32;
                let (fw_list, gpu_list) = arena_addrs(slot.slot()).ok_or(ENOSPC)?;
                inner.info.with_mut(|raw, _inner| {
                    raw.last_id = id;
                    raw.page_list_fw = U64(fw_list);
                    raw.page_list_gpu = U64(gpu_list);
                });
                // A slot the hardware last used for another buffer: start this buffer's
                // descriptor over, so the setup command rebuilds it from the block list
                // instead of finding nothing new and keeping the other buffer's.
                // The buffer is idle here, so put back everything the firmware and hardware
                // advanced during its earlier use, as a new buffer starts.
                if rebind {
                    inner.info.with_mut(|raw, _inner| {
                        raw.gpu_counter = 0;
                        raw.unk_4 = 0;
                        raw.cur_id = -1;
                        raw.unk_10 = 0;
                        raw.gpu_counter2 = 0;
                        raw.unk_38 = 0;
                        raw.page_count.store(0, Ordering::Relaxed);
                        raw.block_count.store(0, Ordering::Relaxed);
                        raw.last_page.store(u32::MAX, Ordering::Relaxed);
                    });
                    inner.info.block_ctl.with(|raw, _inner| raw.unk.store(0, Ordering::Relaxed));
                    inner.info.counter.with(|raw, _inner| raw.count.store(0, Ordering::Relaxed));
                    // The slot's hardware descriptor still names the previous owner's page list
                    // and ring positions: point it at this buffer's empty list, so the setup
                    // command appends from the start.
                    let table = PB_DESCRIPTORS.load(Ordering::Acquire);
                    let slot_no = id as usize;
                    if !table.is_null() && slot_no < 0x4000 / 16 {
                        let list = gpu_list;
                        let d = [(((list >> 4) as u32) & !7) | 1, (((list >> 36) & 0xf) as u32) << 28, 0, 0];
                        // SAFETY: the table is a live 0x4000-byte kernel mapping; this slot is
                        // assigned to this buffer, which is idle.
                        unsafe {
                            let p = table.add(slot_no * 16).cast::<u32>();
                            for (i, w) in d.iter().enumerate() { p.add(i).write_volatile(*w); }
                            crate::pgtable::clean_poc(p.cast::<u8>(), 16);
                        }
                        crate::pgtable::poc_barrier();
                    }
                }
            }
            inner.active_slot = Some(slot);
        }

        inner.active_scenes += 1;

        Ok(Scene::ver {
            object: scene,
            slot: inner.active_slot.as_ref().unwrap().slot(),
            rebind,
            preempt2_off: inner.preempt1_size,
            preempt3_off: inner.preempt1_size + inner.preempt2_size,
            meta1_off: clmeta_size,
            meta2_off: clmeta_size + meta1_size,
            meta3_off: clmeta_size + meta1_size + meta2_size,
            meta4_off: clmeta_size + meta1_size + meta2_size + meta3_size,
        })
    }

    /// Increment the buffer manager usage count. Should we done once we know the Scene is ready
    /// to be committed and used in commands submitted to the GPU.
    pub(crate) fn increment(&self) {
        let inner = self.inner.lock();
        inner.info.counter.with(|raw, _inner| {
            // We could use fetch_add, but the non-LSE atomic
            // sequence Rust produces confuses the hypervisor.
            // We have inner locked anyway, so this is not racy.
            let v = raw.count.load(Ordering::Relaxed);
            raw.count.store(v + 1, Ordering::Relaxed);
        });
    }

    pub(crate) fn any_ref(&self) -> Arc<dyn core::any::Any + Send + Sync> {
        self.inner.clone()
    }
}

#[versions(AGX_ROOT)]
impl Clone for Buffer::ver {
    fn clone(&self) -> Self {
        Buffer::ver {
            inner: self.inner.clone(),
        }
    }
}

#[versions(AGX_ROOT)]
struct BufferSlotInner();

#[versions(AGX_ROOT)]
impl slotalloc::SlotItem for BufferSlotInner::ver {
    type Data = BufferManagerInner::ver;

    fn release(&mut self, data: &mut Self::Data, slot: u32) {
        mod_pr_debug!("BufferManager: Released slot {}\n", slot);
        data.owners[slot as usize] = None;
    }
}

/// Inner data for the buffer manager, to be protected by the SlotAllocator lock.
#[versions(AGX_ROOT)]
pub(crate) struct BufferManagerInner {
    owners: KVec<Option<Buffer::ver>>,
}

/// The GPU-global buffer manager, used to allocate and release buffer slots from the pool.
#[versions(AGX_ROOT)]
pub(crate) struct BufferManager(slotalloc::SlotAllocator<BufferSlotInner::ver>);

#[versions(AGX_ROOT)]
impl BufferManager::ver {
    pub(crate) fn new() -> Result<BufferManager::ver> {
        Self::with_slots(NUM_BUFFERS)
    }

    /// A manager handing out only the first `count` buffer slots.
    pub(crate) fn with_slots(count: u32) -> Result<BufferManager::ver> {
        let count = count.clamp(1, NUM_BUFFERS);
        let mut owners = KVec::new();
        for _i in 0..(count as usize) {
            owners.push(None, GFP_KERNEL)?;
        }
        Ok(BufferManager::ver(slotalloc::SlotAllocator::new(
            count,
            BufferManagerInner::ver { owners },
            |_inner, _slot| Some(BufferSlotInner::ver()),
            c_str!("BufferManager::SlotAllocator"),
            static_lock_class!(),
            static_lock_class!(),
        )?))
    }

    /// Signals a Buffer to synchronously grow.
    pub(crate) fn grow(&self, slot: u32) {
        match self
            .0
            .with_inner(|inner| inner.owners[slot as usize].as_ref().cloned())
        {
            Some(owner) => {
                pr_err!(
                    "BufferManager: Unexpected grow request for slot {}. This might deadlock. Please report this bug.\n",
                    slot
                );
                owner.sync_grow();
            }
            None => {
                pr_err!(
                    "BufferManager: Received grow request for empty slot {}\n",
                    slot
                );
            }
        }
    }
}

#[versions(AGX_ROOT)]
impl Clone for BufferManager::ver {
    fn clone(&self) -> Self {
        BufferManager::ver(self.0.clone())
    }
}
