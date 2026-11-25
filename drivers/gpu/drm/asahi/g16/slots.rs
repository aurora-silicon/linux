// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-VM M4 command storage and device-wide displacement lifetime tracking.
//!
//! Each compute unit owns its own hardware UMA slot (`COMPUTE_UMA_SLOT + index`) and may be in
//! flight together, in publication order; both render units alternate slot 33, one at a time. Firmware retirement and a later
//! distinct retired owner are both required before reuse.
//! Private backing and old VM leases survive until a distinct replacement fully retires.
//! Aborted storage remains mapped and quarantined. Pool cursors are never scrubbed.

use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use kernel::{prelude::*, sync::Arc};

use super::encode::{Geometry, PassRefs};
use super::fw::buffer;
use super::fw::work::{barrier, compute, fs, ta};
use super::graph::{alloc, Client};
use super::hw::{client, engine, sizes};
use super::object::{Class, Factory, FwLease, FwObject, Objects};
use super::pools::lifetime::Lifetime;

/// The compute command records of one hardware compute unit index (work image, microsequence, and
/// barrier records), shared by every VM's [`ComputeSlot`] bound to that hardware unit: the unit
/// index is device-global and claimed by one flight at a time. The host writes them before
/// publication and they take the firmware-cached [`Class::FwCoherent`] view macOS 26B123 gives its
/// per-queue command objects; the work image keeps its uncached kernel GPU task lower alias and
/// cleans host stores to PoC for the GPU, matching [`PassCmds`].
#[derive(Clone)]
pub(crate) struct UnitCmds {
    pub(crate) work: Arc<FwObject>,
    pub(crate) seq: Arc<FwObject>,
    pub(crate) barrier: Arc<FwObject>,
}

impl UnitCmds {
    /// Allocates hardware compute unit `k`'s command records from a device-lifetime allocator
    /// ([`Factory::persistent_objects`]), which the caller never drops.
    #[allow(dead_code)]
    pub(crate) fn new(o: &mut Objects, k: u64) -> Result<Self> {
        let (mut work, mut seq, mut barrier) = (None, None, None);
        Self::init_into(o, k, &mut work, &mut seq, &mut barrier)
    }

    /// Populates any missing persistent command records for hardware compute unit `k` into the
    /// caller-owned slots so partial failure retains already-mapped `FwCoherent` records instead
    /// of leaking or re-allocating them on retry.
    pub(crate) fn init_into(
        o: &mut Objects,
        k: u64,
        work: &mut Option<Arc<FwObject>>,
        seq: &mut Option<Arc<FwObject>>,
        barrier: &mut Option<Arc<FwObject>>,
    ) -> Result<Self> {
        let coherent = Class::FwCoherent;
        if work.is_none() {
            *work = Some(o.alloc(super::object::Request {
                size: compute::WORK_SIZE,
                class: coherent,
                placement: super::object::Placement::Anywhere,
                offset: 0,
                client: None,
                lower: Some(client::WORK_PAGE + k * client::CWORK_STRIDE),
                cpu: true,
            })?);
        }
        if seq.is_none() {
            *seq = Some(alloc(o, compute::SEQ_SIZE, coherent, None, 0, false)?);
        }
        if barrier.is_none() {
            // Holds fs2c (+0x00) and per-pipe c2c (+(1 + pipe) * barrier::SIZE) barriers in 1 page.
            *barrier = Some(alloc(
                o,
                barrier::SIZE * (1 + sizes::COMPUTE_PIPES),
                coherent,
                None,
                0,
                false,
            )?);
        }
        Ok(Self {
            work: work.as_ref().ok_or(EINVAL)?.clone(),
            seq: seq.as_ref().ok_or(EINVAL)?.clone(),
            barrier: barrier.as_ref().ok_or(EINVAL)?.clone(),
        })
    }
}

const _: () = assert!(barrier::SIZE * (1 + sizes::COMPUTE_PIPES) <= super::hw::PAGE as usize);

/// One immutable allocation set. Mutable firmware pool cursors survive successful reuse.
pub(crate) struct ComputeSlot {
    pub(crate) its: Arc<FwObject>,
    pub(crate) preempt: Arc<FwObject>,
    pub(crate) work: Arc<FwObject>,
    pub(crate) seq: Arc<FwObject>,
    #[allow(dead_code)]
    pub(crate) barrier: Arc<FwObject>,
    pub(crate) descriptor: Arc<FwObject>,
    pub(crate) pool: Arc<FwObject>,
    pub(crate) pool_state: Arc<FwObject>,
    pub(crate) page_list: Arc<FwObject>,
    pub(crate) counter: Arc<FwObject>,
    pub(crate) context_store: Arc<FwObject>,
    pub(crate) aux: Arc<FwObject>,
    /// Freelist entries of the unit's UMA pool, seeded once ([`init_uma_pool`]).
    pub(crate) seeds: u32,
    /// VM-free firmware lease keeping this unit's `descriptor` and `counter` mapped in context-0
    /// UAT until a later compute retirement overwrites firmware `uma_mgr_table[1 + k]`.
    uma_lease: Arc<FwLease>,
}

/// Where a compute unit lives and how large its UMA pool is.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Placement {
    /// The VM's base units: a 32 MiB pool at the unit's original client addresses.
    Base,
    /// SCS-capable units (ray-query scratch): a pool of `seeds` 4-KiB entries
    /// ([`super::rt::required_seeds`]) and its freelist in the unit's SCS pool window, the
    /// other records at their own client addresses, sharing ledger index and UMA slot `1 + k`
    /// with base unit k.
    Scs { seeds: u32 },
}

impl Placement {
    fn pool(self, k: u64) -> u64 {
        match self {
            Placement::Base => client::compute_pool(k),
            Placement::Scs { .. } => client::scs_compute_pool(k),
        }
    }
    /// The base of the unit's list, preempt, auxiliary and context records.
    fn state(self, k: u64) -> u64 {
        match self {
            Placement::Base => client::compute_state(k),
            Placement::Scs { .. } => client::scs_compute_records(k),
        }
    }
    /// The unit's freelist: a base unit's at its state, an SCS unit's right after its pool.
    fn freelist(self, k: u64) -> u64 {
        match self {
            Placement::Base => client::compute_state(k),
            Placement::Scs { .. } => self.pool(k) + self.pool_bytes() as u64,
        }
    }
    /// Freelist entries of the unit's pool.
    pub(crate) fn seeds(self) -> u32 {
        match self {
            Placement::Base => super::rt::BASE_SEEDS,
            Placement::Scs { seeds } => seeds,
        }
    }
    /// Bytes of the unit's pool: its seeds of 4 KiB.
    pub(crate) fn pool_bytes(self) -> usize {
        (u64::from(self.seeds()) * super::rt::SEED_BYTES) as usize
    }
    /// Bytes of the unit's freelist ([`super::rt::freelist_bytes`]).
    pub(crate) fn freelist_bytes(self) -> usize {
        super::rt::freelist_bytes(self.seeds()) as usize
    }
    /// An SCS unit's pool fits [`super::rt::MAX_SCS_SEEDS`] in whole rows, so it and its
    /// freelist fit its window.
    pub(crate) fn check(self) -> Result {
        match self {
            Placement::Base => Ok(()),
            Placement::Scs { seeds } => {
                if seeds == 0
                    || seeds % super::rt::SEEDS_PER_ROW != 0
                    || seeds > super::rt::MAX_SCS_SEEDS
                {
                    Err(E2BIG)
                } else {
                    Ok(())
                }
            }
        }
    }
}
const _: () = assert!(
    sizes::POOL_BYTES == super::rt::BASE_SEEDS as u64 * super::rt::SEED_BYTES
        && super::rt::window_bytes(super::rt::MAX_SCS_SEEDS) <= client::SCS_POOL_WINDOW
);

/// Initializes the private pool exactly once, before it is exposed. Never called on reuse.
/// `seeds` 4-KiB freelist entries and their 2-MiB pool-table rows; 8192 is the 32 MiB pool
/// every unit had before SCS. The freelist `state` holds every seed, the pool table `list`
/// every row.
pub(crate) fn init_uma_pool(state: &FwObject, list: &FwObject, pool: u64, seeds: u32) -> Result {
    let seeds = seeds as usize;
    let rows = seeds / super::rt::SEEDS_PER_ROW as usize;
    if pool & 0x3fff != 0
        || seeds == 0
        || seeds % super::rt::SEEDS_PER_ROW as usize != 0
        || seeds * super::rt::RECORD_BYTES as usize > state.size
        || rows * super::fw::buffer::uma_pools::POOL_TABLE_STRIDE > list.size
    {
        return Err(EINVAL);
    }
    let mut chunk = [0u8; 512];
    for first in (0..seeds).step_by(64) {
        for j in 0..64 {
            chunk[j * 8..j * 8 + 8]
                .copy_from_slice(&(pool + ((first + j) as u64) * 0x1000).to_le_bytes());
        }
        state.write(first * 8, &chunk)?;
    }
    for i in 0..rows {
        list.w64(i * 64, (1u64 << 61) | (pool + i as u64 * 0x200000))?;
    }
    Ok(())
}

impl ComputeSlot {
    #[allow(dead_code)]
    pub(crate) fn new(
        o: &mut Objects,
        fw_factory: &Factory,
        k: u64,
        cmds: &UnitCmds,
    ) -> Result<Self> {
        Self::new_in(o, fw_factory, k, k as u32, Placement::Base, cmds)
    }

    /// Unit `k` at `placement` bound to hardware ledger slot `hw_slot`, around the device-lifetime
    /// command records `cmds` of `hw_slot`.
    pub(crate) fn new_in(
        o: &mut Objects,
        fw_factory: &Factory,
        k: u64,
        hw_slot: u32,
        placement: Placement,
        cmds: &UnitCmds,
    ) -> Result<Self> {
        placement.check()?;
        let sh = Class::Shared;
        let state = placement.state(k);
        // Keep the per-VM UMA manager descriptor and submission counter in one VM-free Class::Fw
        // page inside `uma_lease` (matching `pools::Allocator`) while `work`, `seq`, and `barrier`
        // live in device-lifetime `Class::FwCoherent` `UnitCmds`.
        let mut fw = fw_factory.fw_objects();
        let uma_page = alloc(&mut fw, 0x200, Class::Fw, None, 0, false)?;
        let descriptor = FwObject::view(&uma_page, 0, compute::DESCRIPTOR_SIZE)?;
        let counter = FwObject::view(&uma_page, 0x100, 0x80)?;
        let uma_lease = FwLease::new(fw)?;
        let unit = Self {
            its: alloc(o, 16, sh, None, 0, false)?,
            preempt: alloc(
                o,
                0x4000,
                sh,
                Client::nc(state + client::COMPUTE_PREEMPT_OFF),
                0,
                false,
            )?,
            work: cmds.work.clone(),
            seq: cmds.seq.clone(),
            barrier: cmds.barrier.clone(),
            descriptor,
            pool: o.alloc(super::object::Request {
                size: placement.pool_bytes(),
                class: sh,
                placement: super::object::Placement::Anywhere,
                offset: 0,
                client: Some((placement.pool(k), super::hw::pte::LEAF_NC)),
                lower: None,
                cpu: false,
            })?,
            pool_state: alloc(
                o,
                placement.freelist_bytes(),
                sh,
                Client::nc(placement.freelist(k)),
                0,
                false,
            )?,
            page_list: alloc(
                o,
                0x4000,
                sh,
                Client::nc(state + client::COMPUTE_LIST_OFF),
                0,
                false,
            )?,
            counter,
            context_store: alloc(
                o,
                0x40000,
                sh,
                Client::nc(state + client::COMPUTE_CONTEXT_OFF),
                0,
                false,
            )?,
            aux: alloc(
                o,
                0x4000,
                sh,
                Client::nc(state + client::COMPUTE_AUX_OFF),
                0,
                false,
            )?,
            seeds: placement.seeds(),
            uma_lease,
        };
        init_uma_pool(
            &unit.pool_state,
            &unit.page_list,
            unit.pool.gva.ok_or(EINVAL)?,
            unit.seeds,
        )?;
        unit.descriptor.write(
            0,
            &buffer::compute_uma_manager_image(
                unit.work.va,
                engine::COMPUTE_UMA_SLOT + hw_slot,
                unit.pool_state.gva.ok_or(EINVAL)?,
                unit.page_list.gva.ok_or(EINVAL)?,
                unit.counter.va,
                unit.seeds,
                super::rt::freelist_records(unit.seeds),
            ),
        )?;
        Ok(unit)
    }

    /// VM-free firmware lease keeping this unit's `descriptor` and `counter` mapped in context-0
    /// UAT.
    pub(crate) fn uma_lease(&self) -> Arc<FwLease> {
        self.uma_lease.clone()
    }
    /// Resets command-local state before reuse. Pool/list/counter/descriptor state is preserved.
    ///
    /// Clean same-VM reuse only zeroes the 16-byte internal timestamp pair so retirement
    /// checks cannot observe stale ticks; `work` and `seq` are overwritten by `prefix_compute`.
    /// First use, cross-VM retarget, cancelled preparation, or post-recovery/error reuse runs
    /// the full scrub of all scratch areas.
    fn scrub(&self, full: bool) -> Result {
        self.its.fill(0, 0, 16)?;
        if !full {
            return Ok(());
        }
        self.seq.fill(0, 0, 0x300)?;
        self.aux.fill(0, 0, 0x4000)?;
        self.context_store.fill(0, 0, 0x40000)?;
        self.preempt.fill(0, 0, 0x4000)
    }
}

/// The render command records of one pass slot index (TA and FS work images, microsequences and
/// barrier records), shared by every VM's [`PassSlot`] of that index: the index is device-global
/// and claimed by one flight at a time. The host writes them before publication and never reads
/// back what the firmware writes, so they take the firmware-cached [`Class::FwCoherent`] view
/// macOS 26B123 gives its per-queue command objects. The work images keep their uncached kernel
/// GPU task lower alias, as macOS maps its work items for the GPU. Each work image and
/// microsequence keeps its own page; the barrier records share one page, as the compute unit's
/// barriers do.
#[derive(Clone)]
pub(crate) struct PassCmds {
    pub(crate) ta_work: Arc<FwObject>,
    pub(crate) fs_work: Arc<FwObject>,
    pub(crate) ta_seq: Arc<FwObject>,
    pub(crate) fs_seq: Arc<FwObject>,
    pub(crate) own_barrier: Arc<FwObject>,
    pub(crate) prev_barrier: Arc<FwObject>,
    pub(crate) ta_fs_barrier: Arc<FwObject>,
    pub(crate) fs_dep: Arc<FwObject>,
    pub(crate) compute_barrier: Arc<FwObject>,
    pub(crate) fs_compute_barrier: Arc<FwObject>,
}

impl PassCmds {
    /// Barrier records in the shared page: own, prev, TA->FS, FS dependency (one record each),
    /// then the per-pipe compute and FS-compute barriers.
    const OWN: usize = 0;
    const PREV: usize = barrier::SIZE;
    const TA_FS: usize = 2 * barrier::SIZE;
    const FS_DEP: usize = 3 * barrier::SIZE;
    const COMPUTE: usize = 4 * barrier::SIZE;
    const FS_COMPUTE: usize = Self::COMPUTE + barrier::SIZE * sizes::COMPUTE_PIPES;
    const BARRIERS: usize = Self::FS_COMPUTE + barrier::SIZE * sizes::COMPUTE_PIPES;

    /// Allocates pass slot index `i`'s set from a device-lifetime allocator
    /// ([`Factory::persistent_objects`]), which the caller never drops.
    pub(crate) fn new(o: &mut Objects, i: u64) -> Result<Self> {
        let coherent = Class::FwCoherent;
        let off = client::pass_off(i);
        let mut work = |size, page, offset| {
            o.alloc(super::object::Request {
                size,
                class: coherent,
                placement: super::object::Placement::Anywhere,
                offset,
                client: None,
                lower: Some(page),
                cpu: true,
            })
        };
        let fs_work = work(
            fs::WORK_SIZE,
            client::FS_WORK_PAGE + off,
            client::FS_WORK_OFF as usize,
        )?;
        let ta_work = work(
            ta::WORK_SIZE,
            client::TA_WORK_PAGE + off,
            client::TA_WORK_OFF as usize,
        )?;
        let barriers = alloc(o, Self::BARRIERS, coherent, None, 0, false)?;
        let record = |offset, size| FwObject::view(&barriers, offset, size);
        Ok(Self {
            ta_work,
            fs_work,
            ta_seq: alloc(o, ta::SEQ_OBJECT, coherent, None, 0, false)?,
            fs_seq: alloc(o, fs::SEQ_OBJECT, coherent, None, 0, false)?,
            own_barrier: record(Self::OWN, barrier::SIZE)?,
            prev_barrier: record(Self::PREV, barrier::SIZE)?,
            ta_fs_barrier: record(Self::TA_FS, barrier::SIZE)?,
            fs_dep: record(Self::FS_DEP, barrier::SIZE)?,
            compute_barrier: record(Self::COMPUTE, barrier::SIZE * sizes::COMPUTE_PIPES)?,
            fs_compute_barrier: record(Self::FS_COMPUTE, barrier::SIZE * sizes::COMPUTE_PIPES)?,
        })
    }
}

const _: () = assert!(PassCmds::BARRIERS <= super::hw::PAGE as usize);

/// One render pass slot.
pub(crate) struct PassSlot {
    pub(crate) aux: Arc<FwObject>,
    pub(crate) pre: KVec<Arc<FwObject>>,
    pub(crate) tpc: Arc<FwObject>,
    pub(crate) tilemap: Arc<FwObject>,
    pub(crate) heap: Arc<FwObject>,
    pub(crate) scene_user: Arc<FwObject>,
    pub(crate) scene_list: Arc<FwObject>,
    pub(crate) fs_scratch: Arc<FwObject>,
    pub(crate) fs_work: Arc<FwObject>,
    pub(crate) ta_work: Arc<FwObject>,
    pub(crate) ta_seq: Arc<FwObject>,
    pub(crate) fs_seq: Arc<FwObject>,
    pub(crate) ta_scratch: Arc<FwObject>,
    pub(crate) scene: Arc<FwObject>,
    pub(crate) its: Arc<FwObject>,
    /// Part of the slot's firmware layout; the host does not use it.
    _uts: Arc<FwObject>,
    pub(crate) own_barrier: Arc<FwObject>,
    pub(crate) prev_barrier: Arc<FwObject>,
    pub(crate) ta_fs_barrier: Arc<FwObject>,
    pub(crate) fs_dep: Arc<FwObject>,
    pub(crate) compute_barrier: Arc<FwObject>,
    pub(crate) fs_compute_barrier: Arc<FwObject>,
}

impl PassSlot {
    /// Allocates pass slot `i`'s objects, with their client leaf classes, around the
    /// device-lifetime command records `cmds` of index `i`.
    pub(crate) fn new(o: &mut Objects, i: u64, pre_records: u32, cmds: &PassCmds) -> Result<Self> {
        let sh = Class::Shared;
        let off = client::pass_off(i);
        let tpc = if i >= sizes::BASE_RENDER_SLOTS as u64 {
            client::TPC_SLOT_SIZE
        } else {
            client::TPC_SIZE
        };
        let tilemap = if i >= sizes::BASE_RENDER_SLOTS as u64 {
            client::TILEMAP_SLOT_SIZE
        } else {
            client::TILEMAP_SIZE
        };
        let _ = pre_records;

        let aux = alloc(o, 0x20000, sh, Client::nc(client::AUX + off), 0, false)?;
        let mut pre = KVec::with_capacity(3, GFP_KERNEL)?;
        pre.push(
            alloc(o, 0x4000, sh, Client::nc(client::PRE0_PAGE + off), 0, false)?,
            GFP_KERNEL,
        )?;
        pre.push(
            alloc(o, 0x4000, sh, Client::nc(client::PRE1_PAGE + off), 0, false)?,
            GFP_KERNEL,
        )?;
        pre.push(
            alloc(o, 0x4000, sh, Client::nc(client::PRE2_PAGE + off), 0, false)?,
            GFP_KERNEL,
        )?;
        let tpc = alloc(
            o,
            tpc as usize,
            sh,
            Client::nc(client::TPC + off),
            0,
            false,
        )?;
        let tilemap = alloc(
            o,
            tilemap as usize,
            sh,
            Client::nc(client::pass_tilemap(i)),
            0,
            false,
        )?;
        let heap = alloc(
            o,
            0x4000,
            sh,
            Client::nc(client::HEAP_META_PAGE + off),
            0,
            false,
        )?;
        let scene_user = alloc(
            o,
            0x10000,
            sh,
            Client::nc(client::SCENE_USER + off),
            0,
            false,
        )?;
        let scene_list = alloc(
            o,
            super::fw::buffer::SCENE_LIST_SIZE,
            sh,
            Client::nc(client::SCENE_LIST + off),
            0,
            false,
        )?;
        let fs_scratch = alloc(
            o,
            0x10000,
            sh,
            Client::nc(client::FS_SCRATCH + off),
            0,
            false,
        )?;
        Ok(Self {
            aux,
            pre,
            tpc,
            tilemap,
            heap,
            scene_user,
            scene_list,
            fs_scratch,
            fs_work: cmds.fs_work.clone(),
            ta_work: cmds.ta_work.clone(),
            ta_seq: cmds.ta_seq.clone(),
            fs_seq: cmds.fs_seq.clone(),
            ta_scratch: alloc(o, 0x10000, sh, None, 0, false)?,
            scene: alloc(o, super::fw::work::scene::SIZE, sh, None, 0, false)?,
            // Internal TA start/end, FS start/end; legacy user cells likewise.
            its: alloc(o, 32, sh, None, 0, false)?,
            _uts: alloc(o, 32, Class::Fw, None, 0, false)?,
            own_barrier: cmds.own_barrier.clone(),
            prev_barrier: cmds.prev_barrier.clone(),
            ta_fs_barrier: cmds.ta_fs_barrier.clone(),
            fs_dep: cmds.fs_dep.clone(),
            compute_barrier: cmds.compute_barrier.clone(),
            fs_compute_barrier: cmds.fs_compute_barrier.clone(),
        })
    }

    /// Per-use reset of a pass slot's storage. The slot is neither published nor claimed; the
    /// dirty spans are passed in.
    ///
    /// Clean same-VM reuse skips the work/sequence images (immediately overwritten by
    /// `prefix_render`), unused scratch objects (`ta_scratch`, `fs_scratch`, `its`), bulk
    /// scratch (`aux`, `scene_user`), preemption save areas (`pre[0..2]`), and `tpc`
    /// (overwritten by TA hardware), zeroing only the active `tilemap` delta span and the
    /// active `heap` metadata header (`0x200` single-layer, `0x1000` multi-layer). First use,
    /// cross-VM switch, cancelled preparation, or post-recovery/error reuse runs the full
    /// scrub including `tpc_dirty`, `its`, `scene`, `pre[0..2]`, and full `heap`.
    /// Clean same-VM reset of the active per-pass WC spans (used both synchronously on fallback
    /// and off-lock after displacement).
    pub(crate) fn scrub_clean(
        &self,
        tilemap_start: u64,
        tilemap_len: u64,
        heap_dirty: u64,
    ) -> Result {
        if tilemap_len != 0 {
            self.tilemap
                .fill(tilemap_start as usize, 0, tilemap_len as usize)?;
        }
        if heap_dirty != 0 {
            self.heap.fill(0, 0, heap_dirty as usize)?;
        }
        Ok(())
    }

    fn scrub(
        &self,
        tpc_dirty: u64,
        tilemap_start: u64,
        tilemap_len: u64,
        heap_dirty: u64,
        full: bool,
        stats: &mut SlotStats,
    ) -> Result {
        if !full {
            self.scrub_clean(tilemap_start, tilemap_len, heap_dirty)?;
            stats.tilemap_clear_bytes = stats.tilemap_clear_bytes.saturating_add(tilemap_len);
            stats.pass_scrubs = stats.pass_scrubs.saturating_add(1);
            return Ok(());
        }

        self.ta_work.fill(0, 0, ta::WORK_SIZE)?;
        self.fs_work.fill(0, 0, fs::WORK_SIZE)?;
        self.ta_seq.fill(0, 0, ta::SEQ_OBJECT)?;
        self.fs_seq.fill(0, 0, fs::SEQ_OBJECT)?;
        self.ta_scratch.fill(0, 0, 0x10000)?;
        self.fs_scratch.fill(0, 0, 0x10000)?;
        self.its.fill(0, 0, 32)?;
        self.tpc.fill(0, 0, tpc_dirty as usize)?;
        stats.tpc_clear_bytes = stats.tpc_clear_bytes.saturating_add(tpc_dirty);
        self.tilemap.fill(0, 0, tilemap_len as usize)?;
        stats.tilemap_clear_bytes = stats.tilemap_clear_bytes.saturating_add(tilemap_len);
        self.aux.fill(0, 0, 0x20000)?;
        self.heap.fill(0, 0, 0x4000)?;
        self.scene.fill(0, 0, super::fw::work::scene::SIZE)?;
        self.scene_user.fill(0, 0, 0x10000)?;
        self.scene_list
            .fill(0, 0, super::fw::buffer::SCENE_LIST_SIZE)?;
        // Preemption save areas start zeroed on every full reset.
        self.pre[0].fill(0, 0, 0x4000)?;
        self.pre[1].fill(0, 0, 0x4000)?;
        self.pre[2].fill(0, 0, 0x4000)?;
        stats.pass_scrubs = stats.pass_scrubs.saturating_add(1);
        Ok(())
    }

    /// The addresses the render encoders name.
    pub(crate) fn refs(&self) -> PassRefs {
        PassRefs {
            ta_work: self.ta_work.va,
            ta_work_gva: self.ta_work.lower_gva.unwrap_or(0),
            fs_work: self.fs_work.va,
            fs_work_gva: self.fs_work.lower_gva.unwrap_or(0),
            ta_seq: self.ta_seq.va,
            fs_seq: self.fs_seq.va,
            scene: self.scene.va,
            tpc_gva: self.tpc.gva.unwrap_or(0),
            tilemap_gva: self.tilemap.gva.unwrap_or(0),
            heap_gva: self.heap.gva.unwrap_or(0),
            scene_user_gva: self.scene_user.gva.unwrap_or(0),
            scene_list: self.scene_list.gva.unwrap_or(0),
            pre_gva: [
                self.pre[0].gva.unwrap_or(0),
                self.pre[1].gva.unwrap_or(0),
                self.pre[2].gva.unwrap_or(0),
            ],
            fs_scratch_gva: self.fs_scratch.gva.unwrap_or(0),
            aux_gva: self.aux.gva.unwrap_or(0),
            its: self.its.va,
            ta_scratch: self.ta_scratch.va,
            tpc_fw: self.tpc.va,
            scratch_fw: self.scene_user.va,
        }
    }
}

#[derive(Copy, Clone)]
pub(crate) struct Claims {
    pub(crate) slot: [u32; sizes::COMPUTE_SLOTS],
    pub(crate) slot_count: u32,
    pub(crate) compute_pipe: u8,
    pub(crate) pass: [u32; sizes::RENDER_SLOTS],
    pub(crate) pass_count: u32,
    pub(crate) pass_mask: u64,
    #[allow(dead_code)]
    pub(crate) ct: [u32; sizes::RENDER_SLOTS],
    pub(crate) ct_count: u32,
    #[allow(dead_code)]
    pub(crate) rotating: bool,
    /// `Slots::scrub_gen` observed when these claims were published; a recovery invalidation
    /// advances `scrub_gen` so in-flight slots still force a full scrub after retiring.
    pub(crate) scrub_gen: u64,
}
impl Claims {
    pub(crate) const fn new() -> Self {
        Self {
            slot: [0; sizes::COMPUTE_SLOTS],
            slot_count: 0,
            compute_pipe: 0,
            pass: [0; sizes::RENDER_SLOTS],
            pass_count: 0,
            pass_mask: 0,
            ct: [0; sizes::RENDER_SLOTS],
            ct_count: 0,
            rotating: true,
            scrub_gen: 0,
        }
    }
}
/// Most recent host ownership transfer of one retained unit between client VMs.
#[derive(Copy, Clone, Default)]
pub(crate) struct VmRetarget {
    pub(crate) unit: u32,
    pub(crate) old_vm: u64,
    pub(crate) new_vm: u64,
    pub(crate) old_asid: u32,
    pub(crate) new_asid: u32,
    pub(crate) old_generation: u8,
    pub(crate) new_generation: u8,
}

#[derive(Copy, Clone, Default)]
pub(crate) struct SlotStats {
    /// Host ownership transfers, compute then render. Full wire retirement must still be
    /// observed separately to qualify a successful cross-VM hardware execution.
    pub(crate) cross_vm_retargets: [u64; 2],
    pub(crate) last_retarget: [VmRetarget; 2],
    pub(crate) ct_sync: u64,
    pub(crate) pass_sync: u64,
    pub(crate) slot_sync: u64,
    pub(crate) flushes: u64,
    pub(crate) flush_ns: u64,
    /// Bytes actually cleared, including successful writes preceding a later scrub error.
    pub(crate) tpc_clear_bytes: u64,
    pub(crate) tilemap_clear_bytes: u64,
    pub(crate) pass_scrubs: u64,
}

/// Displacement ledger and VM leases of the compute units (see [`Slots`]).
pub(crate) struct ComputeOwners {
    lifetime: Lifetime<{ sizes::COMPUTE_UNITS }>,
    owner: [Option<Arc<super::vm::VmState>>; sizes::COMPUTE_UNITS],
    pending_retarget: [Option<VmRetarget>; sizes::COMPUTE_UNITS],
    pending_owner: [Option<Arc<super::vm::VmState>>; sizes::COMPUTE_UNITS],
    /// The arena holding each owner's unit: the firmware may still write the unit's
    /// descriptor when a later owner displaces it, after its VM's client closed.
    arena: [Option<Arc<super::resources::ComputeArena>>; sizes::COMPUTE_UNITS],
    pending_arena: [Option<Arc<super::resources::ComputeArena>>; sizes::COMPUTE_UNITS],
    /// VM-free firmware leases keeping each hardware slot's `work` + `counter` mapped in
    /// context-0 UAT across closed-VM reclaim until a later compute retirement overwrites
    /// firmware `uma_mgr_table[1 + k]`.
    retained_uma: [Option<Arc<FwLease>>; sizes::COMPUTE_UNITS],
    pending_uma: [Option<Arc<FwLease>>; sizes::COMPUTE_UNITS],
    /// Force a full scrub on next claim (first use, cancel, error, or firmware recovery).
    scrub_full: [bool; sizes::COMPUTE_UNITS],
    /// Whether the most recent claim of each compute unit required a full scrub/work write.
    last_full: [bool; sizes::COMPUTE_UNITS],
}

/// Bitmask of valid render pass slot indices (`0..sizes::RENDER_SLOTS`).
const PASS_MASK: u64 = u64::MAX >> (64 - sizes::RENDER_SLOTS);

/// Shared atomic coordination between `Shared::state` slot selection/claims and off-lock
/// pre-scrubbing of displaced render pass slots.
struct ScrubState {
    /// Pass slots currently being scrubbed outside `Shared::state`; never picked or claimed.
    scrubbing: AtomicU64,
    /// Displaced pass slots whose clean same-VM spans were zeroed off-lock.
    clean_mask: AtomicU64,
    /// Incremented by [`Self::clear_all_clean`] on recovery so an in-flight async scrub that
    /// started before recovery can never publish a stale `clean_mask` bit afterwards.
    generation: AtomicU64,
    /// Set if slot selection found eligible slots blocked only by an in-flight off-lock scrub.
    waiter: AtomicBool,
    /// Set when an off-lock scrub finished while `waiter` was pending so the next `poll_step()`
    /// retries `publish_checked()` even if no flight retired.
    woke: AtomicBool,
    /// Off-lock scrub counters folded into [`SlotStats`] on subsequent locked access.
    pass_scrubs: AtomicU64,
    tilemap_clear_bytes: AtomicU64,
}

impl ScrubState {
    const fn new() -> Self {
        Self {
            scrubbing: AtomicU64::new(0),
            clean_mask: AtomicU64::new(0),
            generation: AtomicU64::new(0),
            waiter: AtomicBool::new(false),
            woke: AtomicBool::new(false),
            pass_scrubs: AtomicU64::new(0),
            tilemap_clear_bytes: AtomicU64::new(0),
        }
    }

    fn is_scrubbing(&self, k: usize) -> bool {
        self.scrubbing.load(Ordering::Acquire) & (1u64 << k) != 0
    }

    fn is_clean(&self, k: usize) -> bool {
        self.clean_mask.load(Ordering::Acquire) & (1u64 << k) != 0
    }

    fn take_clean(&self, k: usize) -> bool {
        let bit = 1u64 << k;
        self.clean_mask.fetch_and(!bit, Ordering::AcqRel) & bit != 0
    }

    fn clear_clean(&self, k: usize) {
        self.clean_mask.fetch_and(!(1u64 << k), Ordering::Release);
    }

    fn clear_all_clean(&self) {
        self.generation.fetch_add(1, Ordering::SeqCst);
        self.clean_mask.store(0, Ordering::SeqCst);
    }
}

/// One displaced render pass slot staged for off-lock WC scrubbing.
pub(crate) struct PendingScrub {
    slot: usize,
    arena: super::resources::PassOwnerArena,
    state: Arc<ScrubState>,
    generation: u64,
    done: bool,
}

impl PendingScrub {
    pub(crate) fn run_one(&mut self) -> bool {
        let bit = 1u64 << self.slot;
        let ok = if self.state.generation.load(Ordering::Acquire) != self.generation {
            false
        } else if let Some((pass, d)) = self.arena.pass_and_dirty(self.slot) {
            let dirty = d.tilemap.load(Ordering::Acquire);
            let last = d.tilemap_last.load(Ordering::Acquire);
            let already = d.tilemap_clean.load(Ordering::Acquire);
            let heap = d.heap.load(Ordering::Acquire);
            let target = if last != 0 { dirty.min(last) } else { dirty };
            let len = target.saturating_sub(already);
            match pass.scrub_clean(already, len, heap) {
                Ok(()) => {
                    // Order scrubbed WC bytes before clearing dirty / publishing clean_mask.
                    super::engine::wmb();
                    d.heap.store(0, Ordering::Release);
                    if target >= dirty {
                        d.tilemap.store(0, Ordering::Release);
                        d.tilemap_clean.store(0, Ordering::Release);
                    } else {
                        d.tilemap_clean.store(target, Ordering::Release);
                    }
                    self.state.pass_scrubs.fetch_add(1, Ordering::Relaxed);
                    self.state
                        .tilemap_clear_bytes
                        .fetch_add(len, Ordering::Relaxed);
                    true
                }
                Err(_) => false,
            }
        } else {
            false
        };
        if ok && self.state.generation.load(Ordering::SeqCst) == self.generation {
            self.state.clean_mask.fetch_or(bit, Ordering::SeqCst);
            if self.state.generation.load(Ordering::SeqCst) != self.generation {
                self.state.clean_mask.fetch_and(!bit, Ordering::SeqCst);
            }
        }
        self.state.scrubbing.fetch_and(!bit, Ordering::SeqCst);
        self.done = true;
        let woke = self.state.waiter.swap(false, Ordering::SeqCst);
        if woke {
            self.state.woke.store(true, Ordering::Release);
        }
        woke
    }
}

impl Drop for PendingScrub {
    fn drop(&mut self) {
        if !self.done {
            let bit = 1u64 << self.slot;
            self.state.scrubbing.fetch_and(!bit, Ordering::SeqCst);
        }
    }
}

/// Maximum displaced render pass slots staged in one stack-resident [`ScrubBatch`].
/// Keeping the batch bounded avoids multi-kilobyte stack frames in poll/submit paths while
/// any additional displaced slots remain flagged for the next pass.
const MAX_SCRUB_BATCH: usize = 8;

/// Zero-allocation batch of displaced render pass slots to scrub after dropping `Shared::state`.
pub(crate) struct ScrubBatch {
    items: [Option<PendingScrub>; MAX_SCRUB_BATCH],
    len: usize,
}

impl ScrubBatch {
    pub(crate) const fn new() -> Self {
        Self {
            items: [const { None }; MAX_SCRUB_BATCH],
            len: 0,
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.len == 0
    }

    fn is_full(&self) -> bool {
        self.len >= MAX_SCRUB_BATCH
    }

    fn push(&mut self, item: PendingScrub) -> bool {
        if self.len >= MAX_SCRUB_BATCH {
            return false;
        }
        self.items[self.len] = Some(item);
        self.len += 1;
        true
    }
}

/// Zero-allocation slot-indexed queue handed to the async pass-scrub workqueue.
///
/// Each pass slot `0..sizes::RENDER_SLOTS` keeps its `scrubbing` bit set while a
/// [`PendingScrub`] is queued or running, so at most one entry per slot exists at once.
pub(crate) struct ScrubQueue {
    slots: [Option<PendingScrub>; sizes::RENDER_SLOTS],
}

impl ScrubQueue {
    pub(crate) const fn new() -> Self {
        Self {
            slots: [const { None }; sizes::RENDER_SLOTS],
        }
    }

    /// Allocates an empty [`ScrubQueue`] directly on the heap to keep `Shared` setup off the
    /// probe stack.
    #[allow(dead_code)]
    #[inline(never)]
    pub(crate) fn new_boxed() -> Result<KBox<Self>> {
        Ok(KBox::init(
            init!(Self {
                slots: [const { None }; sizes::RENDER_SLOTS],
            }),
            GFP_KERNEL,
        )?)
    }

    /// Moves every staged scrub from `batch` into the queue. Returns `true` if any was queued.
    pub(crate) fn push_batch(&mut self, mut batch: ScrubBatch) -> bool {
        let mut pushed = false;
        for slot in &mut batch.items[..batch.len] {
            if let Some(item) = slot.take() {
                let idx = item.slot;
                self.slots[idx] = Some(item);
                pushed = true;
            }
        }
        batch.len = 0;
        pushed
    }

    /// Pops the next queued pass scrub, if any.
    pub(crate) fn pop(&mut self) -> Option<PendingScrub> {
        for slot in &mut self.slots {
            if slot.is_some() {
                return slot.take();
            }
        }
        None
    }
}

/// Displacement ledger and VM leases of the render pass slots (see [`Slots`]).
pub(crate) struct RenderOwners {
    lifetime: Lifetime<{ sizes::RENDER_SLOTS }>,
    // Last-owner leases remain bound through retirement and displacement. Replaced only
    // when the displaced unit is safely rebound to a new publication.
    owner: [Option<Arc<super::vm::VmState>>; sizes::RENDER_SLOTS],
    pending_retarget: [Option<VmRetarget>; sizes::RENDER_SLOTS],
    pending_owner: [Option<Arc<super::vm::VmState>>; sizes::RENDER_SLOTS],
    /// The render arena (base or extension) holding each pass slot's owner.
    arena: [Option<super::resources::PassOwnerArena>; sizes::RENDER_SLOTS],
    pending_arena: [Option<super::resources::PassOwnerArena>; sizes::RENDER_SLOTS],
    /// Force a full scrub on next pass claim (first use, cancel, error, or firmware recovery).
    scrub_full: [bool; sizes::RENDER_SLOTS],
    /// Whether the most recent claim of each render pass slot required a full scrub/work write.
    last_full: [bool; sizes::RENDER_SLOTS],
    /// Displaced pass slots staged after publication to run once `Shared::state` is released.
    staged: ScrubBatch,
    /// A clean render retired since the last unlock; stage any remaining displaced pass slots
    /// only after publication finishes so a freshly displaced slot never blocks immediate reuse.
    scrub_pending: bool,
}

pub(crate) struct Slots {
    pub(crate) cursor: u32,
    pub(crate) pass_busy: u64,
    pub(crate) pass_cursor: u32,
    scrub_gen: u64,
    pub(crate) stats: SlotStats,
    /// Shared atomic state for off-lock pre-scrubbing of displaced render pass slots.
    scrub: Arc<ScrubState>,
    /// The compute and render owners, on the heap: `Firmware` (and so `Slots`) is built on the
    /// probe's stack, which has no room for them.
    compute: KBox<ComputeOwners>,
    render: KBox<RenderOwners>,
}
impl Slots {
    #[inline(never)]
    pub(crate) fn new() -> Result<Self> {
        Ok(Self {
            cursor: 0,
            pass_busy: 0,
            pass_cursor: 0,
            scrub_gen: 0,
            stats: SlotStats::default(),
            scrub: Arc::new(ScrubState::new(), GFP_KERNEL)?,
            compute: KBox::init(
                init!(ComputeOwners {
                    lifetime: Lifetime::new(true),
                    owner: [const { None }; sizes::COMPUTE_UNITS],
                    pending_retarget: [None; sizes::COMPUTE_UNITS],
                    pending_owner: [const { None }; sizes::COMPUTE_UNITS],
                    arena: [const { None }; sizes::COMPUTE_UNITS],
                    pending_arena: [const { None }; sizes::COMPUTE_UNITS],
                    retained_uma: [const { None }; sizes::COMPUTE_UNITS],
                    pending_uma: [const { None }; sizes::COMPUTE_UNITS],
                    scrub_full: [true; sizes::COMPUTE_UNITS],
                    last_full: [true; sizes::COMPUTE_UNITS],
                }),
                GFP_KERNEL,
            )?,
            render: KBox::init(
                init!(RenderOwners {
                    lifetime: Lifetime::new(true),
                    owner: [const { None }; sizes::RENDER_SLOTS],
                    pending_retarget: [None; sizes::RENDER_SLOTS],
                    pending_owner: [const { None }; sizes::RENDER_SLOTS],
                    arena: [const { None }; sizes::RENDER_SLOTS],
                    pending_arena: [const { None }; sizes::RENDER_SLOTS],
                    scrub_full: [true; sizes::RENDER_SLOTS],
                    last_full: [true; sizes::RENDER_SLOTS],
                    staged: ScrubBatch::new(),
                    scrub_pending: false,
                }),
                GFP_KERNEL,
            )?,
        })
    }
    pub(crate) fn init(&self) -> Result {
        Ok(())
    }
    #[allow(dead_code)]
    pub(crate) fn compute_generation(&self, k: usize) -> Result<u8> {
        if k >= sizes::COMPUTE_UNITS {
            return Err(EINVAL);
        }
        Ok(self.compute.lifetime.generation(k))
    }
    #[allow(dead_code)]
    pub(crate) fn pass_generation(&self, k: usize) -> Result<u8> {
        if k >= sizes::RENDER_SLOTS {
            return Err(EINVAL);
        }
        Ok(self.render.lifetime.generation(k))
    }
    pub(crate) fn compute_exhausted(&self) -> bool {
        // Ledger indices above the most units a VM gets never have an owner.
        self.compute
            .lifetime
            .exhausted_within(super::params::compute_units())
    }
    /// Whether none of the first `n` compute units (a VM's own) can ever be claimed again.
    #[allow(dead_code)]
    pub(crate) fn compute_exhausted_within(&self, n: usize) -> bool {
        self.compute.lifetime.exhausted_within(n)
    }
    /// Whether none of the compute units in `mask` (bit `k` -> unit `k`) can ever be claimed.
    pub(crate) fn compute_exhausted_in_mask(&self, mask: u32) -> bool {
        self.compute.lifetime.exhausted_in_mask(mask)
    }
    /// Bitmask of permanently quarantined compute units (`0..COMPUTE_UNITS`).
    pub(crate) fn quarantined_compute_mask(&self) -> u32 {
        self.compute.lifetime.quarantined_mask()
    }
    pub(crate) fn render_exhausted(&self) -> bool {
        self.render.lifetime.exhausted()
    }
    /// Whether none of the first `n` render pass slots (a VM's own) can ever be claimed again.
    pub(crate) fn render_exhausted_within(&self, n: usize) -> bool {
        self.render.lifetime.exhausted_within(n)
    }
    /// Whether any base compute unit (`0..BASE_COMPUTE_UNITS`) is permanently quarantined.
    pub(crate) fn base_compute_quarantined(&self) -> bool {
        self.compute
            .lifetime
            .any_quarantined_within(super::resources::BASE_COMPUTE_UNITS)
    }
    /// Whether any base render pass slot (`0..BASE_RENDER_SLOTS`) is permanently quarantined.
    pub(crate) fn base_render_quarantined(&self) -> bool {
        self.render
            .lifetime
            .any_quarantined_within(super::resources::BASE_RENDER_SLOTS)
    }
    pub(crate) fn lifetime_counts(&self) -> [super::pools::lifetime::Counts; 2] {
        [self.compute.lifetime.counts(), self.render.lifetime.counts()]
    }
    /// Whether one of the first `n` compute units can be claimed now.
    #[allow(dead_code)]
    pub(crate) fn compute_available(&self, n: usize) -> bool {
        (0..n.min(sizes::COMPUTE_UNITS)).any(|k| self.compute.lifetime.available(k))
    }
    /// Whether any compute unit in `mask` can be claimed now.
    pub(crate) fn compute_available_in_mask(&self, mask: u32) -> bool {
        let mut rem = mask;
        while rem != 0 {
            let k = rem.trailing_zeros() as usize;
            rem &= rem - 1;
            if k < sizes::COMPUTE_UNITS && self.compute.lifetime.available(k) {
                return true;
            }
        }
        false
    }
    /// Whether any compute unit in `mask` eligible for `pipe` can be claimed now.
    /// Pipe 0 may claim any bit in `mask`; secondary pipes stay in
    /// `PIPE1_COMPUTE_START..COMPUTE_UNITS` so `0..PIPE1_COMPUTE_START` only ever hold pipe-0
    /// leases.
    pub(crate) fn compute_available_in_mask_for_pipe(&self, mask: u32, pipe: usize) -> bool {
        let allowed = if pipe == 0 {
            mask
        } else {
            mask & !((1u32 << sizes::PIPE1_COMPUTE_START) - 1)
        };
        self.compute_available_in_mask(allowed)
    }
    /// Whether one of the first `n` compute units eligible for `pipe` can be claimed now.
    #[allow(dead_code)]
    pub(crate) fn compute_available_for_pipe(&self, n: usize, pipe: usize) -> bool {
        let limit = n.min(sizes::COMPUTE_UNITS);
        let mask = if limit >= 32 {
            u32::MAX
        } else {
            (1u32 << limit) - 1
        };
        self.compute_available_in_mask_for_pipe(mask, pipe)
    }
    /// Whether one of the first `n` render pass slots can be claimed now.
    #[allow(dead_code)]
    pub(crate) fn pass_available(&self, n: usize) -> bool {
        let mut saw_scrubbing = false;
        let limit = n.min(sizes::RENDER_SLOTS);
        for k in 0..limit {
            if self.pass_busy & (1 << k) == 0 && self.render.lifetime.available(k) {
                if !self.scrub.is_scrubbing(k) {
                    return true;
                }
                saw_scrubbing = true;
            }
        }
        if saw_scrubbing {
            self.scrub.waiter.store(true, Ordering::SeqCst);
            for k in 0..limit {
                if self.pass_busy & (1 << k) == 0
                    && self.render.lifetime.available(k)
                    && !self.scrub.is_scrubbing(k)
                {
                    return true;
                }
            }
        }
        false
    }
    /// Whether any render pass slot is currently being scrubbed off-lock.
    pub(crate) fn scrubbing_active(&self) -> bool {
        self.scrub.scrubbing.load(Ordering::Acquire) != 0
    }
    /// Consumes the wakeup flag set when an off-lock scrub unblocks a waiting render submitter.
    pub(crate) fn take_scrub_woke(&self) -> bool {
        self.scrub.woke.swap(false, Ordering::AcqRel)
    }
    /// Total pass scrubs including completed off-lock pre-scrubs.
    pub(crate) fn pass_scrubs_total(&self) -> u64 {
        self.stats
            .pass_scrubs
            .saturating_add(self.scrub.pass_scrubs.load(Ordering::Relaxed))
    }
    /// Total tilemap bytes cleared including completed off-lock pre-scrubs.
    pub(crate) fn tilemap_clear_bytes_total(&self) -> u64 {
        self.stats
            .tilemap_clear_bytes
            .saturating_add(self.scrub.tilemap_clear_bytes.load(Ordering::Relaxed))
    }
    fn fold_scrub_stats(&mut self) {
        let scrubs = self.scrub.pass_scrubs.swap(0, Ordering::Relaxed);
        let bytes = self.scrub.tilemap_clear_bytes.swap(0, Ordering::Relaxed);
        self.stats.pass_scrubs = self.stats.pass_scrubs.saturating_add(scrubs);
        self.stats.tilemap_clear_bytes = self.stats.tilemap_clear_bytes.saturating_add(bytes);
    }
    /// Records that a clean render retired so displaced pass slots left idle after
    /// publication are staged for off-lock pre-scrubbing right before dropping `Shared::state`.
    pub(crate) fn note_render_retired(&mut self) {
        self.render.scrub_pending = true;
    }
    /// Stages any newly displaced, cleanly retired render pass slots for off-lock pre-scrubbing,
    /// scanning cursor-ordered from `pass_cursor` so the slots `pick_pass_in_for` will reach next
    /// are scrubbed first. Never touches `Published` or undisplaced `Retired` slots.
    pub(crate) fn stage_displaced_pass_scrubs(&mut self) {
        self.render.scrub_pending = false;
        self.fold_scrub_stats();
        let start = self.pass_cursor as usize % sizes::RENDER_SLOTS;
        for i in 0..sizes::RENDER_SLOTS {
            let k = (start + i) % sizes::RENDER_SLOTS;
            let bit = 1u64 << k;
            if self.pass_busy & bit != 0
                || !self.render.lifetime.available(k)
                || self.render.scrub_full[k]
                || self.scrub.is_scrubbing(k)
                || self.scrub.is_clean(k)
                || self.render.owner[k]
                    .as_ref()
                    .is_none_or(|vm| vm.vm.is_closed())
            {
                continue;
            }
            let Some(arena) = self.render.arena[k].clone() else {
                continue;
            };
            if self.render.staged.is_full() {
                self.render.scrub_pending = true;
                break;
            }
            self.scrub.scrubbing.fetch_or(bit, Ordering::SeqCst);
            let generation = self.scrub.generation.load(Ordering::Acquire);
            let item = PendingScrub {
                slot: k,
                arena,
                state: self.scrub.clone(),
                generation,
                done: false,
            };
            if !self.render.staged.push(item) {
                self.render.scrub_pending = true;
                break;
            }
        }
    }
    /// Removes all staged off-lock pass scrubs so the caller can run them outside `Shared::state`.
    pub(crate) fn take_staged_scrubs(&mut self) -> ScrubBatch {
        core::mem::replace(&mut self.render.staged, ScrubBatch::new())
    }
    /// Stages idle displaced render pass slots after publication (when `allow_stage` and a
    /// clean render retired since the last unlock) and hands them off for off-lock execution.
    pub(crate) fn stage_and_take_scrubs(&mut self, allow_stage: bool) -> ScrubBatch {
        if allow_stage && core::mem::take(&mut self.render.scrub_pending) {
            self.stage_displaced_pass_scrubs();
        } else if !allow_stage {
            self.render.scrub_pending = false;
        }
        self.take_staged_scrubs()
    }
    /// Releases displaced compute-unit and render-pass leases owned by closed VMs while
    /// preserving active, in-flight, undisplaced, scrubbing, and quarantined slots.
    ///
    /// Returns `true` if any non-quarantined slot still holds a closed-VM lease awaiting
    /// retirement, displacement, or an in-progress off-lock pass scrub.
    pub(crate) fn reclaim_closed(&mut self) -> bool {
        let mut remaining = false;
        for k in 0..sizes::COMPUTE_UNITS {
            if self.compute.lifetime.is_quarantined(k) {
                continue;
            }
            let owner_closed = self.compute.owner[k]
                .as_ref()
                .is_some_and(|vm| vm.vm.is_closed());
            let pending_closed = self.compute.pending_owner[k]
                .as_ref()
                .is_some_and(|vm| vm.vm.is_closed());
            if !owner_closed && !pending_closed {
                continue;
            }
            if owner_closed
                && self.compute.lifetime.available(k)
                && self.compute.pending_owner[k].is_none()
                && self.compute.pending_arena[k].is_none()
                && self.compute.pending_uma[k].is_none()
            {
                self.compute.owner[k] = None;
                self.compute.arena[k] = None;
                self.compute.scrub_full[k] = true;
                self.compute.last_full[k] = true;
            } else {
                remaining = true;
            }
        }
        for k in 0..sizes::RENDER_SLOTS {
            if self.render.lifetime.is_quarantined(k) {
                continue;
            }
            let owner_closed = self.render.owner[k]
                .as_ref()
                .is_some_and(|vm| vm.vm.is_closed());
            let pending_closed = self.render.pending_owner[k]
                .as_ref()
                .is_some_and(|vm| vm.vm.is_closed());
            if !owner_closed && !pending_closed {
                continue;
            }
            let bit = 1u64 << k;
            if owner_closed
                && self.pass_busy & bit == 0
                && self.render.lifetime.available(k)
                && self.render.pending_owner[k].is_none()
                && self.render.pending_arena[k].is_none()
            {
                if self.scrub.is_scrubbing(k) {
                    self.scrub.waiter.store(true, Ordering::SeqCst);
                }
                if !self.scrub.is_scrubbing(k) {
                    self.render.owner[k] = None;
                    self.render.arena[k] = None;
                    self.render.scrub_full[k] = true;
                    self.render.last_full[k] = true;
                    self.scrub.clear_clean(k);
                } else {
                    remaining = true;
                }
            } else {
                remaining = true;
            }
        }
        remaining
    }
    /// Compute units published and not yet retired.
    pub(crate) fn compute_published(&self) -> u32 {
        self.compute.lifetime.published()
    }
    /// Render pass slots published and not yet retired.
    #[allow(dead_code)]
    pub(crate) fn render_published(&self) -> u32 {
        self.render.lifetime.published()
    }
    #[allow(dead_code)]
    pub(crate) fn free_compute(&self) -> u32 {
        self.free_compute_in(super::params::compute_units())
    }
    /// Available compute units within the first `n` units (a VM's prepared count).
    #[allow(dead_code)]
    pub(crate) fn free_compute_in(&self, n: usize) -> u32 {
        self.free_compute_in_for_pipe(n, 0)
    }
    /// Available compute units in `mask` eligible for `pipe`.
    pub(crate) fn free_compute_in_mask_for_pipe(&self, mask: u32, pipe: usize) -> u32 {
        let mut rem = if pipe == 0 {
            mask
        } else {
            mask & !((1u32 << sizes::PIPE1_COMPUTE_START) - 1)
        };
        let mut count = 0u32;
        while rem != 0 {
            let k = rem.trailing_zeros() as usize;
            rem &= rem - 1;
            if k < sizes::COMPUTE_UNITS && self.compute.lifetime.available(k) {
                count += 1;
            }
        }
        count
    }
    /// Available compute units within the first `n` units eligible for `pipe`.
    #[allow(dead_code)]
    pub(crate) fn free_compute_in_for_pipe(&self, n: usize, pipe: usize) -> u32 {
        let limit = n.min(sizes::COMPUTE_UNITS);
        let mask = if limit >= 32 {
            u32::MAX
        } else {
            (1u32 << limit) - 1
        };
        self.free_compute_in_mask_for_pipe(mask, pipe)
    }
    /// Forces a full scrub on the next claim of every slot (after firmware recovery).
    #[allow(dead_code)]
    pub(crate) fn invalidate_scrub(&mut self) {
        self.scrub_gen = self.scrub_gen.wrapping_add(1);
        self.compute.scrub_full = [true; sizes::COMPUTE_UNITS];
        self.render.scrub_full = [true; sizes::RENDER_SLOTS];
        self.render.scrub_pending = false;
        self.scrub.clear_all_clean();
    }

    /// Forces a full scrub on the next claim of the units in `f` (after an affected error).
    #[allow(dead_code)]
    pub(crate) fn mark_dirty(&mut self, f: &Claims) {
        for &k in &f.slot[..(f.slot_count as usize).min(f.slot.len())] {
            if let Some(dirty) = self.compute.scrub_full.get_mut(k as usize) {
                *dirty = true;
            }
        }
        let mut mask = f.pass_mask & PASS_MASK;
        while mask != 0 {
            let k = mask.trailing_zeros() as usize;
            mask &= mask - 1;
            self.render.scrub_full[k] = true;
            self.scrub.clear_clean(k);
        }
    }

    #[allow(dead_code)]
    pub(crate) fn claim_compute(
        &mut self,
        units: &super::resources::Prepared,
        f: &mut Claims,
        _j: u32,
    ) -> Result<Option<usize>> {
        self.claim_compute_for(units, f, None)
    }

    /// Whether the most recent claim of compute unit `k` requires writing the entire work
    /// object (first use, cross-VM switch, cancelled preparation, error, or firmware recovery)
    /// rather than only the active non-zero spans.
    pub(crate) fn compute_needs_full_work(&self, k: usize) -> bool {
        self.compute.last_full.get(k).copied().unwrap_or(true)
    }

    #[allow(dead_code)]
    pub(crate) fn claim_compute_for(
        &mut self,
        units: &super::resources::Prepared,
        f: &mut Claims,
        vm_id: Option<u64>,
    ) -> Result<Option<usize>> {
        self.claim_compute_for_pipe(units, f, vm_id, 0)
    }

    pub(crate) fn claim_compute_for_pipe(
        &mut self,
        units: &super::resources::Prepared,
        f: &mut Claims,
        vm_id: Option<u64>,
        pipe: usize,
    ) -> Result<Option<usize>> {
        if f.slot_count as usize >= f.slot.len() {
            return Ok(None);
        }
        // Prefer a warm unit already owned by `vm_id` (`!scrub_full`), then an unowned unit,
        // before falling back to round-robin over `allowed`: pipe 0 spans `mask`, while
        // secondary pipes stay in `PIPE1_COMPUTE_START..COMPUTE_UNITS` so `0..PIPE1_COMPUTE_START`
        // only ever hold pipe-0 leases and 2-unit base/SCS VMs never split across two pipes.
        let mask = units.compute_slot_mask()?;
        let allowed = if pipe == 0 {
            mask
        } else {
            mask & !((1u32 << sizes::PIPE1_COMPUTE_START) - 1)
        };
        if allowed == 0 {
            return Ok(None);
        }
        let n = sizes::COMPUTE_UNITS;
        let base_cursor = (self.cursor as usize) % n;
        let p0_mask = (1u32 << sizes::PIPE1_COMPUTE_START) - 1;
        // On pipe 0, prefer pipe-0-exclusive units (`0..PIPE1_COMPUTE_START`) before spilling
        // into secondary-pipe units (`PIPE1_COMPUTE_START..COMPUTE_UNITS`) so concurrent
        // secondary pipes keep maximum unit headroom.
        let tiers: [u32; 2] = if pipe == 0 {
            [allowed & p0_mask, allowed & !p0_mask]
        } else {
            [allowed, 0]
        };
        let mut chosen = None;
        if let Some(id) = vm_id {
            // 1. Reuse a warm unit already owned by `vm_id` (preferring pipe-0-exclusive units).
            'warm: for tier in tiers {
                if tier == 0 {
                    continue;
                }
                for i in 0..n {
                    let k = (base_cursor + i) % n;
                    if (tier & (1u32 << k)) != 0
                        && !self.compute.scrub_full[k]
                        && self.compute.owner[k]
                            .as_ref()
                            .is_some_and(|prev| prev.id == id)
                        && self.compute.lifetime.claim(k)
                    {
                        chosen = Some(k);
                        break 'warm;
                    }
                }
            }
            // 2. Claim an unowned unit before stealing another VM's warm unit.
            if chosen.is_none() {
                'unowned: for tier in tiers {
                    if tier == 0 {
                        continue;
                    }
                    for i in 0..n {
                        let k = (base_cursor + i) % n;
                        if (tier & (1u32 << k)) != 0
                            && self.compute.owner[k].is_none()
                            && self.compute.lifetime.claim(k)
                        {
                            chosen = Some(k);
                            break 'unowned;
                        }
                    }
                }
            }
        }
        // 3. Fall back to round-robin over any available unit (preferring pipe-0-exclusive).
        if chosen.is_none() {
            'any: for tier in tiers {
                if tier == 0 {
                    continue;
                }
                for i in 0..n {
                    let k = (base_cursor + i) % n;
                    if (tier & (1u32 << k)) != 0 && self.compute.lifetime.claim(k) {
                        chosen = Some(k);
                        break 'any;
                    }
                }
            }
        }
        let Some(k) = chosen else {
            return Ok(None);
        };
        let arena_changed = self.compute.arena[k]
            .as_ref()
            .zip(units.compute_arena(k).as_ref())
            .is_some_and(|(a, b)| !Arc::ptr_eq(a, b));
        let full = self.compute.scrub_full[k]
            || arena_changed
            || vm_id.is_none_or(|id| {
                self.compute.owner[k]
                    .as_ref()
                    .is_none_or(|prev| prev.id != id)
            });
        if let Err(e) = units.compute_unit(k).and_then(|unit| unit.scrub(full)) {
            self.compute.lifetime.cancel(k);
            return Err(e);
        }
        self.compute.last_full[k] = full;
        f.slot[f.slot_count as usize] = k as u32;
        f.slot_count += 1;
        f.compute_pipe = pipe as u8;
        self.cursor = ((k + 1) % n) as u32;
        Ok(Some(k))
    }
    #[allow(dead_code)]
    pub(crate) fn pick_pass(
        &self,
        arena: &super::resources::RenderArena,
        geo: &Geometry,
        picked: u64,
    ) -> Option<usize> {
        let n = arena.passes.len().min(sizes::RENDER_SLOTS);
        let mut fallback = None;
        let mut saw_scrubbing = false;
        for i in 0..n {
            let k = (self.pass_cursor as usize + i) % n;
            let p = &arena.passes[k];
            if (self.pass_busy | picked) & (1u64 << k) == 0
                && self.render.lifetime.available(k)
                && geo.tpc_bytes <= p.tpc.size as u64
                && geo.tilemap_bytes <= p.tilemap.size as u64
            {
                if self.scrub.is_scrubbing(k) {
                    saw_scrubbing = true;
                    continue;
                }
                if self.scrub.is_clean(k) {
                    return Some(k);
                }
                fallback.get_or_insert(k);
            }
        }
        if fallback.is_some() {
            return fallback;
        }
        if saw_scrubbing {
            self.scrub.waiter.store(true, Ordering::SeqCst);
            for i in 0..n {
                let k = (self.pass_cursor as usize + i) % n;
                let p = &arena.passes[k];
                if (self.pass_busy | picked) & (1u64 << k) == 0
                    && self.render.lifetime.available(k)
                    && !self.scrub.is_scrubbing(k)
                    && geo.tpc_bytes <= p.tpc.size as u64
                    && geo.tilemap_bytes <= p.tilemap.size as u64
                {
                    return Some(k);
                }
            }
        }
        None
    }
    #[allow(dead_code)]
    pub(crate) fn pick_pass_in(
        &self,
        resources: &super::resources::Prepared,
        geo: &Geometry,
        picked: u64,
    ) -> Option<usize> {
        self.pick_pass_in_for(resources, geo, picked, None)
    }
    /// Selects a render pass slot in `resources` with capacity for `geo`, preferring slots
    /// already owned by `vm_id` (clean before dirty) and unowned slots before retargeting
    /// another VM's warm pass slot.
    pub(crate) fn pick_pass_in_for(
        &self,
        resources: &super::resources::Prepared,
        geo: &Geometry,
        picked: u64,
        vm_id: Option<u64>,
    ) -> Option<usize> {
        let n = resources.render_passes().ok()?.min(sizes::RENDER_SLOTS);
        let mut same_clean_partial = None;
        let mut same_dirty = None;
        let mut unowned_clean = None;
        let mut unowned_dirty = None;
        let mut other_full_clean = None;
        let mut other_clean_partial = None;
        let mut other_dirty = None;
        let mut saw_scrubbing = false;
        for i in 0..n {
            let k = (self.pass_cursor as usize + i) % n;
            let Ok(p) = resources.render_pass(k) else {
                continue;
            };
            if (self.pass_busy | picked) & (1u64 << k) == 0
                && self.render.lifetime.available(k)
                && geo.tpc_bytes <= p.tpc.size as u64
                && geo.tilemap_bytes <= p.tilemap.size as u64
            {
                if self.scrub.is_scrubbing(k) {
                    saw_scrubbing = true;
                    continue;
                }
                let clean = self.scrub.is_clean(k);
                let full_clean = clean
                    && resources.render_pass_dirty(k).is_ok_and(|d| {
                        let need = d.tilemap.load(Ordering::Acquire).min(geo.tilemap_bytes);
                        need <= d.tilemap_clean.load(Ordering::Acquire)
                            && d.heap.load(Ordering::Acquire) == 0
                    });
                if let Some(id) = vm_id {
                    let same_vm = !self.render.scrub_full[k]
                        && self.render.owner[k].as_ref().is_some_and(|o| o.id == id);
                    if same_vm {
                        if full_clean {
                            return Some(k);
                        }
                        if clean {
                            same_clean_partial.get_or_insert(k);
                        } else {
                            same_dirty.get_or_insert(k);
                        }
                    } else if self.render.owner[k].is_none() {
                        if full_clean {
                            unowned_clean.get_or_insert(k);
                        } else {
                            unowned_dirty.get_or_insert(k);
                        }
                    } else if full_clean {
                        other_full_clean.get_or_insert(k);
                    } else if clean {
                        other_clean_partial.get_or_insert(k);
                    } else {
                        other_dirty.get_or_insert(k);
                    }
                } else if full_clean {
                    return Some(k);
                } else if clean {
                    other_clean_partial.get_or_insert(k);
                } else {
                    other_dirty.get_or_insert(k);
                }
            }
        }
        if let Some(k) = same_clean_partial
            .or(same_dirty)
            .or(unowned_clean)
            .or(unowned_dirty)
            .or(other_full_clean)
            .or(other_clean_partial)
            .or(other_dirty)
        {
            return Some(k);
        }
        if saw_scrubbing {
            self.scrub.waiter.store(true, Ordering::SeqCst);
            for i in 0..n {
                let k = (self.pass_cursor as usize + i) % n;
                let Ok(p) = resources.render_pass(k) else {
                    continue;
                };
                if (self.pass_busy | picked) & (1u64 << k) == 0
                    && self.render.lifetime.available(k)
                    && !self.scrub.is_scrubbing(k)
                    && geo.tpc_bytes <= p.tpc.size as u64
                    && geo.tilemap_bytes <= p.tilemap.size as u64
                {
                    return Some(k);
                }
            }
        }
        None
    }
    /// Whether the most recent claim of render pass slot `k` requires writing the entire
    /// TA/FS work images (first use, cross-VM switch, cancelled preparation, error, or
    /// firmware recovery) rather than only the active non-zero spans.
    pub(crate) fn render_needs_full_work(&self, k: usize) -> bool {
        self.render.last_full.get(k).copied().unwrap_or(true)
    }

    #[allow(dead_code)]
    pub(crate) fn claim_pass(&mut self, arena: &super::resources::RenderArena, k: usize) -> Result {
        self.claim_pass_for(arena, k, None)
    }

    #[allow(dead_code)]
    pub(crate) fn claim_pass_for(
        &mut self,
        arena: &super::resources::RenderArena,
        k: usize,
        vm_id: Option<u64>,
    ) -> Result {
        let n = arena.passes.len().min(sizes::RENDER_SLOTS);
        if k >= n || self.scrub.is_scrubbing(k) || !self.render.lifetime.claim(k) {
            return Err(EBUSY);
        }
        self.fold_scrub_stats();
        // Only a retired displaced owner may reach here; no in-flight storage is reset.
        let full = self.render.scrub_full[k]
            || vm_id.is_none_or(|id| {
                self.render.owner[k]
                    .as_ref()
                    .is_none_or(|prev| prev.id != id)
            });
        let pre_cleaned = self.scrub.take_clean(k);
        let p = &arena.passes[k];
        if full {
            let prev_tpc = arena.tpc_dirty[k].load(Ordering::Acquire);
            let prev_tilemap = arena.tilemap_dirty[k].load(Ordering::Acquire);
            if let Err(e) = p.scrub(prev_tpc, 0, prev_tilemap, 0x4000, true, &mut self.stats) {
                self.render.lifetime.cancel(k);
                return Err(e);
            }
            arena.tpc_dirty[k].store(0, Ordering::Relaxed);
            arena.tilemap_dirty[k].store(0, Ordering::Relaxed);
            arena.tilemap_last[k].store(0, Ordering::Relaxed);
            arena.tilemap_clean[k].store(0, Ordering::Relaxed);
            arena.heap_dirty[k].store(0, Ordering::Relaxed);
        } else {
            let prev_dirty = arena.tilemap_dirty[k].load(Ordering::Acquire);
            let already = arena.tilemap_clean[k].load(Ordering::Acquire);
            let heap = arena.heap_dirty[k].load(Ordering::Acquire);
            let delta = prev_dirty.saturating_sub(already);
            if !(pre_cleaned && delta == 0 && heap == 0) {
                if let Err(e) = p.scrub(0, already, delta, heap, false, &mut self.stats) {
                    self.render.lifetime.cancel(k);
                    return Err(e);
                }
                arena.heap_dirty[k].store(0, Ordering::Relaxed);
                arena.tilemap_dirty[k].store(0, Ordering::Relaxed);
                arena.tilemap_clean[k].store(0, Ordering::Relaxed);
            }
        }
        self.render.last_full[k] = full;
        self.pass_cursor = ((k + 1) % n) as u32;
        Ok(())
    }

    #[allow(dead_code)]
    pub(crate) fn claim_pass_in(
        &mut self,
        resources: &super::resources::Prepared,
        k: usize,
        vm_id: Option<u64>,
        tilemap_bytes: u64,
    ) -> Result {
        let n = resources.render_passes()?.min(sizes::RENDER_SLOTS);
        if k >= n || self.scrub.is_scrubbing(k) || !self.render.lifetime.claim(k) {
            return Err(EBUSY);
        }
        self.fold_scrub_stats();
        let full = self.render.scrub_full[k]
            || vm_id.is_none_or(|id| {
                self.render.owner[k]
                    .as_ref()
                    .is_none_or(|prev| prev.id != id)
            });
        let pre_cleaned = self.scrub.take_clean(k);
        let p = resources.render_pass(k)?;
        let d = resources.render_pass_dirty(k)?;
        if full {
            let prev_tpc = d.tpc.load(Ordering::Acquire);
            let prev_tilemap = d.tilemap.load(Ordering::Acquire);
            if let Err(e) = p.scrub(prev_tpc, 0, prev_tilemap, 0x4000, true, &mut self.stats) {
                self.render.lifetime.cancel(k);
                return Err(e);
            }
            d.tpc.store(0, Ordering::Relaxed);
            d.tilemap.store(0, Ordering::Relaxed);
            d.tilemap_last.store(0, Ordering::Relaxed);
            d.tilemap_clean.store(0, Ordering::Relaxed);
            d.heap.store(0, Ordering::Relaxed);
        } else {
            // On clean same-VM reuse, zero only the uncleaned overlap with the current pass's
            // tilemap (`already..need`) and the active heap span; preserve the high-water mark
            // in `d.tilemap` for cross-VM or recovery full scrubs.
            let prev_dirty = d.tilemap.load(Ordering::Acquire);
            let already = d.tilemap_clean.load(Ordering::Acquire);
            let heap = d.heap.load(Ordering::Acquire);
            let need = prev_dirty.min(tilemap_bytes);
            let delta = need.saturating_sub(already);
            if !(pre_cleaned && delta == 0 && heap == 0) {
                if let Err(e) = p.scrub(0, already, delta, heap, false, &mut self.stats) {
                    self.render.lifetime.cancel(k);
                    return Err(e);
                }
                d.heap.store(0, Ordering::Relaxed);
                if prev_dirty != 0 && need >= prev_dirty {
                    d.tilemap.store(0, Ordering::Relaxed);
                    d.tilemap_clean.store(0, Ordering::Relaxed);
                } else if delta != 0 {
                    d.tilemap_clean.store(need, Ordering::Relaxed);
                }
            }
        }
        self.render.last_full[k] = full;
        self.pass_cursor = ((k + 1) % n) as u32;
        Ok(())
    }
    #[allow(dead_code)]
    pub(crate) fn claim_ct(&mut self, _f: &mut Claims, _bytes: u64) -> Result<Option<usize>> {
        Ok(None)
    }
    fn vm_retarget(
        unit: usize,
        old: &super::vm::VmState,
        new: &super::vm::VmState,
    ) -> Option<VmRetarget> {
        (old.id != new.id).then_some(VmRetarget {
            unit: unit as u32,
            old_vm: old.id,
            new_vm: new.id,
            old_asid: old.slot,
            new_asid: new.slot,
            old_generation: old.generation,
            new_generation: new.generation,
        })
    }

    /// Records a host transfer only after every queue producer and doorbell succeeded.
    /// Full wire retirement is a separate condition for hardware qualification.
    pub(crate) fn note_published(&mut self, f: &Claims) {
        for &k in &f.slot[..f.slot_count as usize] {
            if let Some(proof) = self.compute.pending_retarget[k as usize].take() {
                self.stats.cross_vm_retargets[0] =
                    self.stats.cross_vm_retargets[0].saturating_add(1);
                self.stats.last_retarget[0] = proof;
            }
        }
        let mut mask = f.pass_mask & PASS_MASK;
        while mask != 0 {
            let k = mask.trailing_zeros() as usize;
            mask &= mask - 1;
            if let Some(proof) = self.render.pending_retarget[k].take() {
                self.stats.cross_vm_retargets[1] =
                    self.stats.cross_vm_retargets[1].saturating_add(1);
                self.stats.last_retarget[1] = proof;
            }
        }
    }

    /// Retains one newly claimed compute unit's VM/ASID lease and owner arena in O(1).
    pub(crate) fn hold_compute_slot(
        &mut self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        resources: &super::resources::Prepared,
    ) -> Result {
        if k >= sizes::COMPUTE_UNITS {
            return Err(EINVAL);
        }
        self.compute.pending_retarget[k] = self.compute.owner[k]
            .as_ref()
            .and_then(|old| Self::vm_retarget(k, old, vm));
        self.compute.pending_owner[k] = Some(vm.clone());
        self.compute.pending_arena[k] = resources.compute_arena(k);
        self.compute.pending_uma[k] = resources.compute_unit(k).ok().map(ComputeSlot::uma_lease);
        Ok(())
    }

    /// Retains one newly claimed render pass slot's VM/ASID lease and owner arena in O(1).
    pub(crate) fn hold_render_pass(
        &mut self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        resources: &super::resources::Prepared,
    ) -> Result {
        if k >= sizes::RENDER_SLOTS {
            return Err(EINVAL);
        }
        self.render.pending_retarget[k] = self.render.owner[k]
            .as_ref()
            .and_then(|old| Self::vm_retarget(k, old, vm));
        self.render.pending_owner[k] = Some(vm.clone());
        self.render.pending_arena[k] = resources.pass_owner_arena(k);
        Ok(())
    }

    /// Validates claim bounds/masks and confirms every claimed slot already holds a pending
    /// VM lease without re-cloning `Arc` leases at publication time.
    pub(crate) fn verify_held(&self, f: &Claims) -> Result {
        if f.slot_count as usize > f.slot.len()
            || f.pass_count as usize > f.pass.len()
            || f.pass_mask & !PASS_MASK != 0
        {
            return Err(EINVAL);
        }
        for &k in &f.slot[..f.slot_count as usize] {
            let k = k as usize;
            if k >= sizes::COMPUTE_UNITS || self.compute.pending_owner[k].is_none() {
                return Err(EINVAL);
            }
        }
        if f.pass_count != 0 {
            let mut mask = 0u64;
            for &k in &f.pass[..f.pass_count as usize] {
                let k = k as usize;
                if k >= sizes::RENDER_SLOTS || self.render.pending_owner[k].is_none() {
                    return Err(EINVAL);
                }
                let bit = 1u64 << k;
                if mask & bit != 0 {
                    return Err(EINVAL);
                }
                mask |= bit;
            }
            if mask != f.pass_mask {
                return Err(EINVAL);
            }
        } else {
            let mut mask = f.pass_mask;
            while mask != 0 {
                let k = mask.trailing_zeros() as usize;
                mask &= mask - 1;
                if self.render.pending_owner[k].is_none() {
                    return Err(EINVAL);
                }
            }
        }
        Ok(())
    }

    /// Retains each actual owner's VM/ASID binding through the firmware's last-owner era.
    /// A previous lease is replaced only after the new publication fully retires.
    #[allow(dead_code)]
    pub(crate) fn hold_vm(&mut self, f: &Claims, vm: &Arc<super::vm::VmState>) -> Result {
        if f.slot_count as usize > f.slot.len()
            || f.slot[..f.slot_count as usize]
                .iter()
                .any(|&k| k as usize >= sizes::COMPUTE_UNITS)
            || f.pass_count as usize > f.pass.len()
            || f.pass_mask & !PASS_MASK != 0
        {
            return Err(EINVAL);
        }
        if f.pass_count != 0 {
            let mut mask = 0u64;
            for &k in &f.pass[..f.pass_count as usize] {
                if k as usize >= sizes::RENDER_SLOTS {
                    return Err(EINVAL);
                }
                let bit = 1u64 << k;
                if mask & bit != 0 {
                    return Err(EINVAL);
                }
                mask |= bit;
            }
            if mask != f.pass_mask {
                return Err(EINVAL);
            }
        }
        for &k in &f.slot[..f.slot_count as usize] {
            let k = k as usize;
            self.compute.pending_retarget[k] = self.compute.owner[k]
                .as_ref()
                .and_then(|old| Self::vm_retarget(k, old, vm));
            self.compute.pending_owner[k] = Some(vm.clone());
        }
        let mut mask = f.pass_mask;
        while mask != 0 {
            let k = mask.trailing_zeros() as usize;
            mask &= mask - 1;
            self.render.pending_retarget[k] = self.render.owner[k]
                .as_ref()
                .and_then(|old| Self::vm_retarget(k, old, vm));
            self.render.pending_owner[k] = Some(vm.clone());
        }
        Ok(())
    }

    /// With [`Self::hold_vm`]: the claimed units' arenas, held with their VM leases. The VM's
    /// own cache lets go of its arenas when its client closes.
    #[allow(dead_code)]
    pub(crate) fn hold_arenas(&mut self, f: &Claims, resources: &super::resources::Prepared) {
        for &k in &f.slot[..(f.slot_count as usize).min(f.slot.len())] {
            if let Some(cell) = self.compute.pending_arena.get_mut(k as usize) {
                *cell = resources.compute_arena(k as usize);
            }
            if let Some(cell) = self.compute.pending_uma.get_mut(k as usize) {
                *cell = resources
                    .compute_unit(k as usize)
                    .ok()
                    .map(ComputeSlot::uma_lease);
            }
        }
        let mut mask = f.pass_mask & PASS_MASK;
        while mask != 0 {
            let k = mask.trailing_zeros() as usize;
            mask &= mask - 1;
            self.render.pending_arena[k] = resources.pass_owner_arena(k);
        }
    }

    /// Reserve the actual same-slot publication. Call before producers become visible.
    pub(crate) fn publish(&mut self, f: &mut Claims) -> Result {
        // Validate on a private copy so a failure changes no ownership.
        let mut c = self.compute.lifetime.clone();
        let mut p = self.render.lifetime.clone();
        for &k in &f.slot[..f.slot_count as usize] {
            if !c.publish_on(k as usize, f.compute_pipe) {
                return Err(EBUSY);
            }
        }
        if f.pass_count != 0 {
            if f.pass_count as usize > f.pass.len() {
                return Err(EINVAL);
            }
            for &k in &f.pass[..f.pass_count as usize] {
                if k as usize >= sizes::RENDER_SLOTS || !p.publish(k as usize) {
                    return Err(EBUSY);
                }
            }
        } else {
            let mut mask = f.pass_mask & PASS_MASK;
            while mask != 0 {
                let k = mask.trailing_zeros() as usize;
                mask &= mask - 1;
                if !p.publish(k) {
                    return Err(EBUSY);
                }
            }
        }
        self.compute.lifetime = c;
        self.render.lifetime = p;
        f.scrub_gen = self.scrub_gen;
        Ok(())
    }
    /// Full wire retirement is a precondition; completion fences alone are insufficient.
    pub(crate) fn retire(&mut self, f: &mut Claims) -> Result {
        let mut c = self.compute.lifetime.clone();
        let mut p = self.render.lifetime.clone();
        for &k in &f.slot[..f.slot_count as usize] {
            if !c.retire(k as usize) {
                return Err(EINVAL);
            }
        }
        if f.pass_count != 0 {
            if f.pass_count as usize > f.pass.len() {
                return Err(EINVAL);
            }
            // Retire in submission order so displacement retains the newest hardware pass slot.
            for &k in &f.pass[..f.pass_count as usize] {
                if k as usize >= sizes::RENDER_SLOTS || !p.retire(k as usize) {
                    return Err(EINVAL);
                }
            }
        } else {
            let mut mask = f.pass_mask & PASS_MASK;
            while mask != 0 {
                let k = mask.trailing_zeros() as usize;
                mask &= mask - 1;
                if !p.retire(k) {
                    return Err(EINVAL);
                }
            }
        }
        self.compute.lifetime = c;
        self.render.lifetime = p;
        let clean_scrub = f.scrub_gen == self.scrub_gen;
        for &k in &f.slot[..f.slot_count as usize] {
            self.compute.owner[k as usize] = self.compute.pending_owner[k as usize].take();
            self.compute.arena[k as usize] = self.compute.pending_arena[k as usize].take();
            if let Some(lease) = self.compute.pending_uma[k as usize].take() {
                self.compute.retained_uma[k as usize] = Some(lease);
            }
            if clean_scrub {
                self.compute.scrub_full[k as usize] = false;
            }
        }
        let mut mask = f.pass_mask & PASS_MASK;
        while mask != 0 {
            let k = mask.trailing_zeros() as usize;
            mask &= mask - 1;
            self.render.owner[k] = self.render.pending_owner[k].take();
            self.render.arena[k] = self.render.pending_arena[k].take();
            if clean_scrub {
                self.render.scrub_full[k] = false;
            }
        }
        self.pass_busy &= !f.pass_mask;
        f.slot_count = 0;
        f.compute_pipe = 0;
        f.pass_count = 0;
        f.pass_mask = 0;
        f.ct_count = 0;
        Ok(())
    }
    pub(crate) fn quarantine(&mut self, f: &mut Claims) {
        for &k in &f.slot[..f.slot_count as usize] {
            self.compute.lifetime.quarantine(k as usize);
            self.compute.pending_retarget[k as usize] = None;
            self.compute.scrub_full[k as usize] = true;
        }
        let mut mask = f.pass_mask & PASS_MASK;
        while mask != 0 {
            let k = mask.trailing_zeros() as usize;
            mask &= mask - 1;
            self.render.lifetime.quarantine(k);
            self.render.pending_retarget[k] = None;
            self.render.scrub_full[k] = true;
            self.scrub.clear_clean(k);
        }
        self.pass_busy &= !f.pass_mask;
        f.slot_count = 0;
        f.compute_pipe = 0;
        f.pass_count = 0;
        f.pass_mask = 0;
        f.ct_count = 0;
    }
    /// Cancels preparation only. Published units require retire() or quarantine().
    pub(crate) fn release(&mut self, f: &mut Claims) {
        for &k in &f.slot[..f.slot_count as usize] {
            self.compute.lifetime.cancel(k as usize);
            self.compute.pending_retarget[k as usize] = None;
            self.compute.pending_owner[k as usize] = None;
            self.compute.pending_arena[k as usize] = None;
            self.compute.pending_uma[k as usize] = None;
            self.compute.scrub_full[k as usize] = true;
        }
        let mut mask = f.pass_mask & PASS_MASK;
        while mask != 0 {
            let k = mask.trailing_zeros() as usize;
            mask &= mask - 1;
            self.render.lifetime.cancel(k);
            self.render.pending_retarget[k] = None;
            self.render.pending_owner[k] = None;
            self.render.pending_arena[k] = None;
            self.render.scrub_full[k] = true;
            self.scrub.clear_clean(k);
        }
        self.pass_busy &= !f.pass_mask;
        f.slot_count = 0;
        f.compute_pipe = 0;
        f.pass_count = 0;
        f.pass_mask = 0;
        f.ct_count = 0;
    }
}
