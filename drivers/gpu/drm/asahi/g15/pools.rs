// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Render UMA/PB allocators, their spare set and parameter-buffer growth.
//!
//! Record layouts: [`super::fw::buffer`]; the ledger and request checks: [`growth`]
//! (host-tested).
//!
//! - One 512 MiB PB heap at [`super::hw::client::PB_HEAP`] (4096 blocks of 128 KiB), backed on
//!   demand: the initial blocks when the GPU starts, then chunks of at most
//!   [`growth::CHUNK_BLOCKS`] (4 MiB) as growth needs them ([`Firmware::back_pb`]), never freed
//!   or shrunk. The backing ([`PbChunk`]) is mapped only into user VMs, at the fixed client GVA,
//!   each VM's before its prefixes are published ([`Firmware::map_pb`]); nothing names its pages
//!   through the host window or context 0 (the records hold client GVAs only). Initial
//!   [`super::hw::tuning::PB_BLOCKS`] (1024), at most [`super::hw::tuning::PB_MAX_BLOCKS`]
//!   (4095) posted, so the producer never returns to 0; the firmware is given
//!   [`Pools::soft_max`] (default 3072) as its growth maximum. Truthful K = 4096 in every
//!   profile (PB reconstruction, also in recovery, synchronizes 8 * K bytes of the block list:
//!   K = 0x26cd overran the 0x8000-byte list and crashed the second recovery).
//! - Growth: the event drain queues type-7 requests; [`Firmware::answer_growth`] answers each
//!   one in the poll pass: type 7, PB slot 0, the context of a live published render prefix,
//!   counter >= next expected, epoch == current halt count; write records `(G - 0x1000000000)
//!   >> 15` at producer % K, barrier, block_ctl+4, barrier, then DEVCTL 7 with +4 = 1 echoing
//!   slot, context, counter, position and epoch; +10 blocks per request. Above the firmware's
//!   growth maximum the requests are answered as before, for the synchronous growth of partial
//!   renders (Mesa's partial background/EOT programs). At the ledger's maximum the reply is +4
//!   = 0, a refusal, twice per initialization; later requests are left unanswered. A refusal
//!   gives no pages and can leave the TA stalled; the poll pass returns, and the host deadline
//!   and its recovery path apply, without a guarantee that they end the stall. Invalid requests
//!   are logged and left unanswered, so the host deadline applies. Recovery drops pending
//!   requests (epoch gate).
//! - Managers ASC-cached with an uncached spare set allocated with the graph; the first
//!   recovered render abort swaps to the spare and never rewrites the cached set (the ASC may
//!   still hold dirty lines of it); later aborts rewrite the spare in place. block_ctl stays
//!   uncached when growth is on (a live host producer).
//!
//! The engine uses: [`Firmware::init_render_pools`] (render graph init: `false`; the priming
//! prefix after an abort: `true` when [`super::recovery::Recovery::reinit`] is set), the VAs
//! for its encoders ([`Pools::allocator_refs`]), [`Firmware::publish_pb`] after the render
//! threshold, and [`Pools::write_stats`] for the debugfs `pb:` line.

mod growth;

use core::fmt;
use core::sync::atomic::Ordering;
use kernel::{
    device,
    prelude::*,
    sync::{aref::ARef, Arc},
};

use super::fw::buffer::{self, block_ctl, counter, misc, pb_manager, uma_manager, uma_pools};
use super::fw::channels::DevCtl;
use super::fw::initdata::control;
use super::hw::{client, pte, sizes, tuning};
use super::object::{self, Class, FwObject, HeldPages, Objects, Placement, Request};
use super::vm::VmState;
use super::Firmware;
use crate::driver::AsahiDrmDevice as AsahiDevice;
use crate::{gem, mmu};

pub(crate) use growth::Ledger;

static_assert!(buffer::MAX_BLOCKS == sizes::PB_BLOCKS);
static_assert!(buffer::MAX_PAGES == sizes::PB_PAGES);
static_assert!(buffer::BLOCK_LIST_RECORDS == sizes::PB_LIST_RECORDS);
static_assert!(buffer::GROW_STEP == sizes::PB_GROW_STEP);
static_assert!(buffer::MAX_BLOCKS as u64 * buffer::BLOCK_BYTES == client::PB_HEAP_SIZE);
static_assert!(buffer::PAGE_LIST_SIZE as u64 == client::PB_PAGE_LIST_SIZE);
static_assert!(uma_manager::SEEDS == sizes::FREELIST_SEED);
static_assert!(uma_pools::POOL_SIZE == sizes::POOL_SIZE);
// The ledger keeps the driver's PB sizes: neither is capped by [`growth::MAX_POSTED`].
static_assert!(tuning::PB_MAX_BLOCKS <= growth::MAX_POSTED);
static_assert!(buffer::MIN_BLOCKS <= tuning::PB_BLOCKS);
static_assert!(tuning::PB_BLOCKS <= tuning::PB_MAX_BLOCKS);
// The PB alias is GPU-only (not cached and firmware-visible): dropping one is a plain unmap,
// with no firmware cache flush (`KernelMapping::drop`).
static_assert!(!mmu::Prot::from_pte(pte::LEAF_CACHED).is_cached_noncoherent());

/// Physical backing of PB blocks `[first, first + blocks)`: a zeroed GEM kernel object, pinned
/// and DMA-mapped ([`object::gpu_backing`]), aliased only into user VMs at the blocks' fixed
/// client GVA ([`Firmware::map_pb`]). Retained until reboot, like every firmware object; each
/// alias also holds the object.
struct PbChunk {
    first: u32,
    blocks: u32,
    _pages: HeldPages,
    backing: gem::ObjectRef,
}

impl PbChunk {
    fn new(dev: &AsahiDevice, first: u32, blocks: u32) -> Result<Self> {
        if blocks == 0 || first.checked_add(blocks).ok_or(EINVAL)? > buffer::MAX_BLOCKS {
            return Err(EINVAL);
        }
        let (backing, pages) =
            object::gpu_backing(dev, blocks as usize * buffer::BLOCK_BYTES as usize)?;
        Ok(Self {
            first,
            blocks,
            _pages: pages,
            backing,
        })
    }

    /// The block after the chunk's last.
    fn end(&self) -> u32 {
        self.first + self.blocks
    }

    /// Maps the whole chunk, exactly its object's size, at its fixed client GVA in `vm`.
    fn map(&self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        vm.map_at(
            client::PB_HEAP + self.first as u64 * buffer::BLOCK_BYTES,
            self.blocks as usize * buffer::BLOCK_BYTES as usize,
            &self.backing.gem,
            mmu::Prot::from_pte(pte::LEAF_CACHED),
            false,
        )
    }
}

/// The allocator state a rebuild after an abort moves to the spare.
struct AllocatorSet {
    /// TA (0) and FS (1) UMA managers and their counters.
    uma_manager: [Arc<FwObject>; 2],
    uma_counter: [Arc<FwObject>; 2],
    block_ctl: Arc<FwObject>,
    pb_counter: Arc<FwObject>,
    misc: Arc<FwObject>,
    scene_list: Arc<FwObject>,
    pb_manager: Arc<FwObject>,
}

/// One render engine's UMA pools: eight GPU-only 4 MiB pools, the freelist seeded with them and
/// the pool table.
struct UmaEngine {
    pools: KVec<Arc<FwObject>>,
    freelist: Arc<FwObject>,
    pool_table: Arc<FwObject>,
}

/// Firmware VAs of the render allocators in use (after a rebuild possibly the spare set), as
/// the TA/FS images, the scene and InitBM records name them.
#[derive(Copy, Clone, Default)]
pub(crate) struct AllocatorRefs {
    /// UMA managers of TA (0) and FS (1).
    pub(crate) uma_mgr: [u64; 2],
    /// PB manager (InitBM +0x14, TA/FS work and sequences), scene list (scene +0x0/+0x8 at
    /// `+ 4 * (draw % 48)`, TA work) and misc record (scene +0x40).
    pub(crate) pb_mgr: u64,
    pub(crate) scene_list: u64,
    pub(crate) mgr_misc: u64,
    /// Initial PB blocks of the current initialization (InitBM +0x10, TA register #33).
    pub(crate) init_blocks: u32,
}

/// PB growth statistics.
#[derive(Default)]
pub(crate) struct PbStats {
    pub(crate) requests: u64,
    pub(crate) grown: u64,
    pub(crate) refused: u64,
    pub(crate) bad: u64,
}

/// Render allocator and PB state: the allocators, the PB ledger and the spare set.
pub(crate) struct Pools {
    /// The GPU device, for logging (also used by recovery).
    pub(crate) dev: ARef<device::Device>,
    /// The DRM device, for the PB backing objects.
    drm: ARef<AsahiDevice>,
    uma: [UmaEngine; 2],
    /// The allocator set in use.
    live: AllocatorSet,
    /// The never-exposed uncached set the first rebuild moves to; afterwards the replaced
    /// cached set, retained (only with `manager_cached`).
    spare: Option<AllocatorSet>,
    /// The spare set has not been used yet.
    spare_ready: bool,
    pb_pages: Arc<FwObject>,
    pb_blocks: Arc<FwObject>,
    /// The PB backing, contiguous from block 0, in block order: the initial blocks, then the
    /// growth chunks. Its capacity, reserved up front, is the most the ledger can need
    /// ([`growth::max_chunks`]).
    chunks: KVec<PbChunk>,
    /// The PB growth ledger since the last (re)initialization.
    pub(crate) ledger: Ledger,
    /// C1 names the current initial pages.
    c1_written: bool,
    /// The unanswered-at-the-maximum warning was given since the last initialization.
    exhausted_logged: bool,
    pub(crate) stats: PbStats,
}

/// The ASC-cached twin of an allocator manager's class.
const fn manager_class(class: Class) -> Class {
    if !tuning::MANAGER_CACHED {
        return class;
    }
    match class {
        Class::Shared => Class::SharedCached,
        _ => Class::FwCached,
    }
}

/// Allocates a zeroed firmware object anywhere in the firmware window: `client` names the
/// fixed client page and leaf of its alias in every VM, `cpu` is false for GPU-only storage.
fn alloc(
    objects: &mut Objects,
    size: u64,
    class: Class,
    client: Option<(u64, u64)>,
    offset: u64,
    cpu: bool,
) -> Result<Arc<FwObject>> {
    objects.alloc(Request {
        size: size as usize,
        class,
        placement: Placement::Anywhere,
        offset: offset as usize,
        client,
        lower: None,
        cpu,
    })
}

/// Writes `count` little-endian records of `W` bytes, record `i` at `offset + i * W`, in chunks.
fn write_records<const W: usize>(
    object: &FwObject,
    offset: usize,
    count: usize,
    record: impl Fn(usize) -> [u8; W],
) -> Result {
    const CHUNK: usize = 512;
    let mut buf = [0u8; CHUNK];
    let per_chunk = CHUNK / W;
    let mut i = 0;
    while i < count {
        let n = per_chunk.min(count - i);
        for j in 0..n {
            buf[j * W..(j + 1) * W].copy_from_slice(&record(i + j));
        }
        object.write(offset + i * W, &buf[..n * W])?;
        i += n;
    }
    Ok(())
}

/// A u64 record.
fn le64(value: u64) -> [u8; 8] {
    value.to_le_bytes()
}

impl UmaEngine {
    /// Allocates engine `e`'s UMA pools, freelist and pool table, then its manager and counter,
    /// in this order.
    fn alloc(objects: &mut Objects, e: u64) -> Result<(Self, Arc<FwObject>, Arc<FwObject>)> {
        let mut pools = KVec::with_capacity(uma_pools::POOLS, GFP_KERNEL)?;
        for k in 0..uma_pools::POOLS as u64 {
            pools.push(
                alloc(
                    objects,
                    uma_pools::POOL_SIZE,
                    Class::Shared,
                    Some((client::uma(e, k), pte::LEAF_NC)),
                    0,
                    false,
                )?,
                GFP_KERNEL,
            )?;
        }
        let freelist = alloc(
            objects,
            uma_pools::FREELIST_SIZE as u64,
            Class::Shared,
            Some((client::uma_freelist(e), pte::LEAF_NC)),
            0,
            true,
        )?;
        let pool_table = alloc(
            objects,
            uma_pools::POOL_TABLE_SIZE as u64,
            Class::Shared,
            Some((client::uma_table_page(e), pte::LEAF_NC)),
            uma_pools::POOL_TABLE_OFFSET as u64,
            true,
        )?;
        let manager = alloc(
            objects,
            uma_manager::SIZE as u64,
            manager_class(Class::Shared),
            None,
            0,
            true,
        )?;
        let counter = alloc(objects, counter::SIZE as u64, Class::Shared, None, 0, true)?;
        Ok((
            Self {
                pools,
                freelist,
                pool_table,
            },
            manager,
            counter,
        ))
    }
}

/// The client GVA of an object with a client alias.
fn gva(object: &FwObject) -> Result<u64> {
    object.gva.ok_or(EINVAL)
}

impl Pools {
    /// Allocates the render allocator objects: per engine the UMA pools, freelist, pool table,
    /// manager and counter; block control, PB counter, misc, scene list and PB manager; the
    /// uncached spare set; the PB page list, block list and heap. Contents are written by
    /// [`Firmware::init_render_pools`].
    pub(crate) fn new(dev: &AsahiDevice, objects: &mut Objects) -> Result<Self> {
        let (uma0, manager0, counter0) = UmaEngine::alloc(objects, 0)?;
        let (uma1, manager1, counter1) = UmaEngine::alloc(objects, 1)?;
        let small = |objects: &mut Objects, size: usize, class| {
            alloc(objects, size as u64, class, None, 0, true)
        };
        // A live producer with growth: firmware index 2.
        let block_ctl_class = if tuning::PB_GROW {
            Class::Fw
        } else {
            manager_class(Class::Fw)
        };
        let block_ctl = small(objects, block_ctl::SIZE, block_ctl_class)?;
        let pb_counter = small(objects, counter::SIZE, Class::Fw)?;
        let misc = small(objects, misc::SIZE, manager_class(Class::Fw))?;
        let scene_list = small(objects, buffer::SCENE_LIST_SIZE, Class::Shared)?;
        let pb_manager = small(objects, pb_manager::SIZE, manager_class(Class::Fw))?;
        let live = AllocatorSet {
            uma_manager: [manager0, manager1],
            uma_counter: [counter0, counter1],
            block_ctl,
            pb_counter,
            misc,
            scene_list,
            pb_manager,
        };
        // The uncached spare set for the first rebuild after an abort (manager_cached).
        let spare = if tuning::MANAGER_CACHED {
            let m0 = small(objects, uma_manager::SIZE, Class::Shared)?;
            let c0 = small(objects, counter::SIZE, Class::Shared)?;
            let m1 = small(objects, uma_manager::SIZE, Class::Shared)?;
            let c1 = small(objects, counter::SIZE, Class::Shared)?;
            Some(AllocatorSet {
                uma_manager: [m0, m1],
                uma_counter: [c0, c1],
                block_ctl: small(objects, block_ctl::SIZE, Class::Fw)?,
                pb_counter: small(objects, counter::SIZE, Class::Fw)?,
                misc: small(objects, misc::SIZE, Class::Fw)?,
                scene_list: small(objects, buffer::SCENE_LIST_SIZE, Class::Shared)?,
                pb_manager: small(objects, pb_manager::SIZE, Class::Fw)?,
            })
        } else {
            None
        };
        let pb_pages = alloc(
            objects,
            buffer::PAGE_LIST_SIZE as u64,
            Class::Shared,
            Some((client::PB_PAGE_LIST, pte::LEAF_PB_LIST)),
            0,
            true,
        )?;
        let pb_blocks = alloc(
            objects,
            buffer::BLOCK_LIST_SIZE as u64,
            Class::Shared,
            Some((client::PB_BLOCK_LIST, pte::LEAF_PB_LIST)),
            0,
            true,
        )?;
        // Backing for the initial blocks; growth adds the rest.
        let ledger = Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);
        let mut chunks =
            KVec::with_capacity(growth::max_chunks(ledger.initial, ledger.max), GFP_KERNEL)?;
        chunks.push(PbChunk::new(dev, 0, ledger.initial)?, GFP_KERNEL)?;
        Ok(Self {
            dev: dev.as_ref().as_ref().into(),
            drm: dev.into(),
            uma: [uma0, uma1],
            live,
            spare_ready: spare.is_some(),
            spare,
            pb_pages,
            pb_blocks,
            chunks,
            ledger,
            c1_written: false,
            exhausted_logged: false,
            stats: PbStats::default(),
        })
    }

    /// First rebuild after an abort with cached managers: the cached set may still have dirty
    /// lines in the ASC, so it is retained untouched and the graph moves to the never-exposed
    /// uncached spare (and its own counters and scene list), which [`Self::init`] then
    /// initializes.
    fn take_spare(&mut self) {
        if let Some(spare) = self.spare.as_mut() {
            core::mem::swap(&mut self.live, spare);
        }
        self.spare_ready = false;
        dev_info!(
            self.dev,
            "render allocators moved to a fresh uncached set; the cached set is retained\n"
        );
    }

    /// Writes the initial allocator state: UMA pools, freelists and managers; the PB lists,
    /// manager and counters. The firmware owns them once published; rewritten (zeroed first)
    /// only after a recovered render abort, followed by a fresh InitBM.
    fn init(&mut self) -> Result {
        for e in 0..2 {
            let uma = &self.uma[e];
            let manager = &self.live.uma_manager[e];
            let count = &self.live.uma_counter[e];
            manager.fill(0, 0, uma_manager::SIZE)?;
            count.fill(0, 0, counter::SIZE)?;
            // Freelist: the eight 4 MiB pools in 0x1000 units.
            let per_pool = (uma_pools::POOL_SIZE / uma_pools::SEED_BYTES) as usize;
            let mut bases = [0u64; uma_pools::POOLS];
            for (k, pool) in uma.pools.iter().enumerate() {
                bases[k] = gva(pool)?;
            }
            write_records(&uma.freelist, 0, uma_pools::POOLS * per_pool, |i| {
                le64(bases[i / per_pool] + (i % per_pool) as u64 * uma_pools::SEED_BYTES)
            })?;
            for (k, base) in bases.iter().enumerate() {
                uma.pool_table
                    .w64(uma_pools::POOL_TABLE_STRIDE * k, uma_pools::row(*base))?;
            }
            manager.write(
                0,
                &buffer::uma_manager_image(
                    e as u32 + 1,
                    e as u32,
                    gva(&uma.freelist)?,
                    gva(&uma.pool_table)?,
                    count.va,
                ),
            )?;
            count.w32(0, 1)?;
        }

        // PB: 16384 pages of 0x8000 in 4096 blocks of 0x20000; with growth the first initial
        // blocks of them, at most 4095 posted, and page limits at the firmware's growth maximum.
        // In every profile the block list declares its real 4096-record capacity.
        self.ledger = Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);
        self.exhausted_logged = false;
        if tuning::PB_GROW {
            // C1 names the initial pages again.
            self.c1_written = false;
        }
        let set = &self.live;
        set.pb_manager.fill(0, 0, pb_manager::SIZE)?;
        set.block_ctl.fill(0, 0, block_ctl::SIZE)?;
        set.misc.fill(0, 0, misc::SIZE)?;
        set.pb_counter.fill(0, 0, counter::SIZE)?;
        set.scene_list.fill(0, 0, buffer::SCENE_LIST_SIZE)?;
        if tuning::PB_GROW {
            self.pb_pages.fill(0, 0, buffer::PAGE_LIST_SIZE)?;
            self.pb_blocks.fill(0, 0, buffer::BLOCK_LIST_SIZE)?;
        }
        let heap = client::PB_HEAP;
        let initial = self.ledger.initial;
        if initial > self.backed() {
            return Err(EINVAL);
        }
        write_records(
            &self.pb_pages,
            0,
            (buffer::PAGES_PER_BLOCK * initial) as usize,
            |i| buffer::page_record(heap, i as u32).to_le_bytes(),
        )?;
        // The high word of every block record is zero (zero-filled, or only ever written 0).
        write_records(&self.pb_blocks, 0, initial as usize, |i| {
            le64(buffer::block_record(heap, i as u32) as u64)
        })?;
        set.pb_manager.write(
            0,
            // The maximum the firmware sees is the asynchronous growth limit, below the mapped
            // heap: the blocks above it answer the synchronous requests a partial render makes
            // (the firmware cannot finish a scene at its maximum without them; refused, its TA
            // stalls and no kill ends it: ticket malik-na/linux-aurora#69).
            &buffer::pb_manager_image(
                initial,
                self.soft_max(),
                gva(&self.pb_pages)?,
                gva(&self.pb_blocks)?,
                set.block_ctl.va,
                set.pb_counter.va,
            ),
        )?;
        set.block_ctl.write(0, &buffer::block_ctl_image(initial))?;
        set.misc.w32(misc::ENABLE, 1)?;
        set.pb_counter.w32(0, 1)?;
        crate::mem::sync();
        Ok(())
    }

    /// The allocator addresses the render encoders name (the live set: after the first
    /// rebuild, the spare).
    pub(crate) fn allocator_refs(&self) -> AllocatorRefs {
        let set = &self.live;
        AllocatorRefs {
            uma_mgr: [set.uma_manager[0].va, set.uma_manager[1].va],
            pb_mgr: set.pb_manager.va,
            scene_list: set.scene_list.va,
            mgr_misc: set.misc.va,
            init_blocks: self.ledger.initial,
        }
    }

    /// The firmware's growth maximum in blocks (`g15_pb_max_blocks`), at least the initial size
    /// and at most the mapped heap.
    pub(crate) fn soft_max(&self) -> u32 {
        super::params::pb_max_blocks().clamp(self.ledger.initial, self.ledger.max)
    }

    /// The blocks with backing, from block 0.
    pub(crate) fn backed(&self) -> u32 {
        self.chunks.last().map_or(0, PbChunk::end)
    }

    /// The debugfs line `pb: blocks initial/posted/firmware-max/max backed blocks/chunks
    /// requests grown refused bad`.
    pub(crate) fn write_stats(&self, f: &mut impl fmt::Write) -> fmt::Result {
        writeln!(
            f,
            "pb: blocks {}/{}/{}/{} backed {}/{} requests {} grown {} refused {} bad {}",
            self.ledger.initial,
            self.ledger.posted,
            self.soft_max(),
            self.ledger.max,
            self.backed(),
            self.chunks.len(),
            self.stats.requests,
            self.stats.grown,
            self.stats.refused,
            self.stats.bad
        )
    }
}

impl Firmware {
    /// Maps the PB backing into `vm` up to its last chunk, before anything of a prefix of the
    /// VM is published (the start of `publish_prefix_into`). Render work runs in one VM at a
    /// time (`Engine::publish`), so the VM about to run is the only one through which the GPU
    /// can reach a posted block next; a VM created or idle while the backing grew catches up
    /// here, also a closed VM whose accepted jobs still run.
    ///
    /// Each chunk's alias joins the VM's driver mappings, held until the VM is torn down, and
    /// the VM's count moves past the chunk only then. When that fails, the alias just made is
    /// dropped again: nothing of the VM runs before its count covers the chunk, and the leaf is
    /// GPU-only, so the drop is a plain unmap. The chunks mapped before stay counted.
    ///
    /// The one-VM rule is checked first: a flight of another VM, or a count past the chunks,
    /// fails with EPROTO before anything is mapped.
    pub(crate) fn map_pb(&mut self, vm: &VmState) -> Result {
        let mut mapped = vm.pb_chunks.load(Ordering::Relaxed);
        if mapped > self.pools.chunks.len()
            || self
                .engine
                .flights
                .iter()
                .any(|f| !core::ptr::eq(&*f.job.vm, vm))
        {
            dev_err!(
                self.dev(),
                "PB backing: ctx {} maps {} of {} chunks with {} flights of another VM\n",
                vm.slot,
                mapped,
                self.pools.chunks.len(),
                self.engine
                    .flights
                    .iter()
                    .filter(|f| !core::ptr::eq(&*f.job.vm, vm))
                    .count()
            );
            return Err(super::boot::eproto());
        }
        while let Some(chunk) = self.pools.chunks.get(mapped) {
            let mut aliases = KVec::with_capacity(1, GFP_KERNEL)?;
            aliases.push(chunk.map(&vm.vm)?, GFP_KERNEL)?;
            vm.vm.append_driver_mappings(aliases)?;
            mapped += 1;
            vm.pb_chunks.store(mapped, Ordering::Relaxed);
        }
        Ok(())
    }

    /// The VM whose render work runs: every flight's (`Engine::publish` puts a flight behind
    /// another only for the same VM), and it must be the asking context's. EPROTO otherwise.
    fn running_vm(&self, ctx: u32) -> Result<Arc<VmState>> {
        let flights = &self.engine.flights;
        let vm = flights
            .first()
            .ok_or_else(super::boot::eproto)?
            .job
            .vm
            .clone();
        if vm.slot != ctx || flights.iter().any(|f| !Arc::ptr_eq(&f.job.vm, &vm)) {
            dev_err!(
                self.dev(),
                "PB growth: ctx {} is not the one VM in flight (ctx {}, {} flights)\n",
                ctx,
                vm.slot,
                flights.len()
            );
            return Err(super::boot::eproto());
        }
        Ok(vm)
    }

    /// Backs the PB up to block `end` in the VM that runs, the asking context `ctx`, before an
    /// answer posts those blocks (nothing new when they are backed already): chunks of at most
    /// [`growth::CHUNK_BLOCKS`] blocks are allocated ([`object::gpu_backing`]), mapped into that
    /// VM and kept, one at a time. The VM first catches up ([`Self::map_pb`], which also checks
    /// the one-VM rule), so its count equals the chunks. A chunk is kept
    /// only once its alias in the VM is held, and the VM's count moves with it; when the alias
    /// cannot be held, the alias and the chunk are dropped, neither ever reachable (the leaf is
    /// GPU-only: a plain unmap). An error leaves the chunks backed so far for the next request,
    /// and nothing is posted. The allocation sleeps (shmem pages with `__GFP_RETRY_MAYFAIL`)
    /// while the firmware waits for the answer; how long is not bounded here.
    fn back_pb(&mut self, end: u32, ctx: u32) -> Result {
        let vm = self.running_vm(ctx)?;
        self.map_pb(&vm)?;
        while self.pools.backed() < end {
            let (first, blocks) =
                growth::next_chunk(self.pools.backed(), self.pools.ledger.max).ok_or(ENOSPC)?;
            if self.pools.chunks.len() == self.pools.chunks.capacity() {
                return Err(ENOSPC);
            }
            let chunk = PbChunk::new(&self.pools.drm, first, blocks)?;
            let mut aliases = KVec::with_capacity(1, GFP_KERNEL)?;
            aliases.push(chunk.map(&vm.vm)?, GFP_KERNEL)?;
            vm.vm.append_driver_mappings(aliases)?;
            // Within the capacity checked above.
            self.pools
                .chunks
                .push_within_capacity(chunk)
                .map_err(|_| EIO)?;
            vm.pb_chunks
                .store(self.pools.chunks.len(), Ordering::Relaxed);
        }
        Ok(())
    }

    /// (Re)initializes the render UMA managers, PB lists and PB manager. `rebuild`: the priming
    /// prefix after a recovered abort: the first one moves to the uncached spare set, later
    /// ones rewrite it in place.
    pub(crate) fn init_render_pools(&mut self, rebuild: bool) -> Result {
        if rebuild && self.pools.spare_ready {
            self.pools.take_spare();
        }
        self.pools.init()
    }

    /// The PB counters of a publication with renders, after the render threshold: the PB
    /// counter names the prefix's last draw, and C1 = {0x07400000, 4 * initial blocks} is
    /// written once per (re)initialization. An error leaves the prefix unpublished.
    pub(crate) fn publish_pb(&mut self, draw: u32) -> Result {
        self.pools.live.pb_counter.w32(0, draw)?;
        if !self.pools.c1_written {
            self.write_c1(&buffer::c1_record(self.pools.ledger.initial))?;
            self.pools.c1_written = true;
        }
        Ok(())
    }

    /// Answers the firmware's PB-growth requests, outside the event drain.
    /// A request names PB slot 0, the context of a published render prefix and the next
    /// request counter; anything else is logged and left unanswered (its TA then stays stalled
    /// and the deadline applies). With growth up to ten more blocks of the mapped heap are
    /// written to unposted block-list records, then the live block_ctl+4 producer advances and
    /// DEVCTL 7 reports success; at the ledger's maximum the reply is the refusal (+4 = 0),
    /// which gives the firmware no new pages, and after two refusals requests are left
    /// unanswered. The firmware imports the blocks itself: P is never rewritten. Only the
    /// requests queued before the pass are taken. A failed reply is logged; a request that
    /// cannot be processed at all (a firmware object access failed) is logged and left
    /// unanswered like an invalid one. Never fails: the poll pass continues.
    pub(crate) fn answer_growth(&mut self) -> Result {
        // Only the requests queued before this pass: a reply's wait drains the rings and may
        // queue the next request, which the next pass answers (the device lock is released in
        // between).
        let queued = self.events.grow.len();
        for _ in 0..queued {
            let Ok(request) = self.events.grow.remove(0) else {
                break;
            };
            self.pools.stats.requests += 1;
            if let Err(e) = self.answer_request(&request) {
                self.pools.stats.bad += 1;
                dev_err!(
                    self.dev(),
                    "PB growth request {} ctx {} not answered: {}\n",
                    request[buffer::grow_request::COUNTER],
                    request[buffer::grow_request::CONTEXT],
                    e.to_errno()
                );
            }
        }
        Ok(())
    }

    /// Processes one growth request.
    fn answer_request(&mut self, request: &[u32; 14]) -> Result {
        let ctl = self.init.control.clone().ok_or(ENODEV)?;
        let epoch =
            ctl.r32(control::HALT_COUNT)? as u64 | (ctl.r32(control::HALT_COUNT + 4)? as u64) << 32;
        let ctx = request[buffer::grow_request::CONTEXT];
        let live = self
            .engine
            .flights
            .iter()
            .enumerate()
            .any(|(i, f)| self.receipts(i).renders != 0 && f.job.vm.slot == ctx);
        // At the maximum, after the refusals, the request is left unanswered: the poll pass
        // returns, and the host deadline and its recovery path apply to a stalled TA.
        if self.pools.ledger.exhausted() {
            self.pools.stats.bad += 1;
            if !self.pools.exhausted_logged {
                self.pools.exhausted_logged = true;
                dev_warn!(
                    self.dev(),
                    "PB growth request {} ctx {} left unanswered: {} refusals at the maximum ({} blocks); the host deadline applies\n",
                    request[buffer::grow_request::COUNTER],
                    ctx,
                    self.pools.ledger.refusals,
                    self.pools.ledger.max
                );
            }
            return Ok(());
        }
        // A request from before a halt has no authority over the PB now.
        if !self
            .pools
            .ledger
            .admits(request, tuning::PB_GROW, live, epoch)
        {
            self.pools.stats.bad += 1;
            dev_warn!(
                self.dev(),
                "PB growth request not answered: ctx {} slot {} counter {} (next {}) epoch {:#x} (now {:#x}) live {} pb_grow {}\n",
                ctx,
                request[buffer::grow_request::SLOT],
                request[buffer::grow_request::COUNTER],
                self.pools.ledger.next_request,
                growth::request_epoch(request),
                epoch,
                live as u32,
                tuning::PB_GROW as u32
            );
            return Ok(());
        }
        // Every block the answer posts is backed in the VM that runs before anything of the
        // answer is written; the one-VM rule is checked for every answer. If that fails, nothing
        // is posted or answered (answer_growth logs it; the deadline applies), and the backing
        // made so far stays for the next request.
        let end = self.pools.ledger.posted + self.pools.ledger.next_count();
        self.back_pb(end, ctx)?;
        let (first, n) = self.pools.ledger.accept(request);
        let heap = client::PB_HEAP;
        for b in first..first + n {
            self.pools.pb_blocks.w64(
                buffer::block_record_offset(b),
                buffer::block_record(heap, b) as u64,
            )?;
        }
        if n != 0 {
            // Records before their producer.
            crate::mem::sync();
            let producer = self.pools.ledger.post(n);
            self.pools
                .live
                .block_ctl
                .w32(block_ctl::PRODUCER, producer)?;
            // Producer before the reply.
            crate::mem::sync();
            self.pools.stats.grown += n as u64;
        } else {
            self.pools.stats.refused += 1;
        }
        let reply = DevCtl {
            words: buffer::grow_reply_words(request, n != 0),
        };
        let ret = self.devctl(&reply, true);
        let ledger = self.pools.ledger;
        let soft = self.pools.soft_max();
        // Every answer past the firmware's maximum is logged with its TA position: a scene
        // that keeps asking at one position is a runaway, one that advances is large.
        let past_soft = n != 0 && ledger.posted > soft;
        let info = self.pools.ledger.log_answer(request, n, ret.is_err()) || past_soft;
        let (counter, position) = (
            request[buffer::grow_request::COUNTER],
            request[buffer::grow_request::POSITION],
        );
        let ret = match ret {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        };
        if info {
            dev_info!(
                self.dev(),
                "PB growth request {} ctx {} position {}: +{} blocks ({} of {}), DEVCTL 7: {}\n",
                counter,
                ctx,
                position,
                n,
                ledger.posted,
                ledger.max,
                ret
            );
        } else {
            dev_dbg!(
                self.dev(),
                "PB growth request {} ctx {} position {}: +{} blocks ({} of {}), DEVCTL 7: {}\n",
                counter,
                ctx,
                position,
                n,
                ledger.posted,
                ledger.max,
                ret
            );
        }
        Ok(())
    }
}
