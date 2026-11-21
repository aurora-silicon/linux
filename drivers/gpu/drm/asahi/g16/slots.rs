// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-VM M4 command storage and device-wide displacement lifetime tracking.
//!
//! Each compute unit owns its own hardware UMA slot (`COMPUTE_UMA_SLOT + index`) and may be in
//! flight together, in publication order; both render units alternate slot 33, one at a time. Firmware retirement and a later
//! distinct retired owner are both required before reuse.
//! Private backing and old VM leases survive until a distinct replacement fully retires.
//! Aborted storage remains mapped and quarantined. Pool cursors are never scrubbed.

use core::sync::atomic::Ordering;
use kernel::{prelude::*, sync::Arc};

use super::encode::{Geometry, PassRefs};
use super::fw::work::{barrier, fs, ta};
use super::graph::{alloc, alloc_work, cmd_fw, Client};
use super::hw::{client, sizes};
use super::object::{Class, FwObject, Objects};
use super::pools::lifetime::Lifetime;

/// One immutable allocation set. Mutable firmware pool cursors survive successful reuse.
pub(crate) struct ComputeSlot {
    pub(crate) its: Arc<FwObject>,
    pub(crate) preempt: Arc<FwObject>,
    pub(crate) work: Arc<FwObject>,
    pub(crate) seq: Arc<FwObject>,
    #[allow(dead_code)]
    pub(crate) barrier: Arc<FwObject>,
    pub(crate) pool: Arc<FwObject>,
    pub(crate) pool_state: Arc<FwObject>,
    pub(crate) page_list: Arc<FwObject>,
    pub(crate) counter: Arc<FwObject>,
    pub(crate) context_store: Arc<FwObject>,
    pub(crate) aux: Arc<FwObject>,
}

/// Initializes the private pool exactly once, before it is exposed. Never called on reuse.
pub(crate) fn init_uma_pool(state: &FwObject, list: &FwObject, pool: u64) -> Result {
    if pool & 0x3fff != 0 {
        return Err(EINVAL);
    }
    let mut chunk = [0u8; 512];
    for first in (0..8192).step_by(64) {
        for j in 0..64 {
            chunk[j * 8..j * 8 + 8]
                .copy_from_slice(&(pool + ((first + j) as u64) * 0x1000).to_le_bytes());
        }
        state.write(first * 8, &chunk)?;
    }
    for i in 0..16 {
        list.w64(i * 64, (1u64 << 61) | (pool + i as u64 * 0x200000))?;
    }
    Ok(())
}

impl ComputeSlot {
    pub(crate) fn new(o: &mut Objects, k: u64) -> Result<Self> {
        let sh = Class::Shared;
        let unit = Self {
            its: alloc(o, 16, sh, None, 0, false)?,
            preempt: alloc(
                o,
                0x4000,
                sh,
                Client::nc(client::compute_preempt(k)),
                0,
                false,
            )?,
            work: alloc_work(o, 0x2000, client::WORK_PAGE + k * client::CWORK_STRIDE, 0)?,
            seq: alloc(o, 0x300, cmd_fw(), None, 0, false)?,
            barrier: alloc(o, barrier::SIZE, cmd_fw(), None, 0, false)?,
            pool: o.alloc(super::object::Request {
                size: 0x2000000,
                class: sh,
                placement: super::object::Placement::Anywhere,
                offset: 0,
                client: Some((client::compute_pool(k), super::hw::pte::LEAF_NC)),
                lower: None,
                cpu: false,
            })?,
            pool_state: alloc(
                o,
                0x20000,
                sh,
                Client::nc(client::compute_state(k)),
                0,
                false,
            )?,
            page_list: alloc(o, 0x4000, sh, Client::nc(client::compute_list(k)), 0, false)?,
            counter: alloc(o, 0x80, sh, None, 0, false)?,
            context_store: alloc(
                o,
                0x40000,
                sh,
                Client::nc(client::compute_context(k)),
                0,
                false,
            )?,
            aux: alloc(o, 0x4000, sh, Client::nc(client::compute_aux(k)), 0, false)?,
        };
        init_uma_pool(
            &unit.pool_state,
            &unit.page_list,
            unit.pool.gva.ok_or(EINVAL)?,
        )?;
        Ok(unit)
    }
    /// Resets command-local state before reuse. Pool/list/counter/descriptor state is preserved.
    ///
    /// Clean same-VM reuse only zeroes the 16-byte internal timestamp pair so retirement
    /// checks cannot observe stale ticks; `work` (including its embedded sequence) is
    /// overwritten completely by `prefix_compute`. First use, cross-VM retarget, cancelled
    /// preparation, or post-recovery/error reuse runs the full scrub of all scratch areas.
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
    #[allow(dead_code)]
    pub(crate) prev_barrier: Arc<FwObject>,
    #[allow(dead_code)]
    pub(crate) fs_dep: Arc<FwObject>,
}

impl PassSlot {
    /// Allocates pass slot `i`'s objects, with their client leaf classes.
    pub(crate) fn new(o: &mut Objects, i: u64, pre_records: u32) -> Result<Self> {
        let sh = Class::Shared;
        let (ps, ws) = (i * client::PASS_STRIDE, i * client::WORK_STRIDE);
        let tpc = if i != 0 {
            client::TPC_SLOT_SIZE
        } else {
            client::TPC_SIZE
        };
        let tilemap = if i != 0 {
            client::TILEMAP_SLOT_SIZE
        } else {
            client::TILEMAP_SIZE
        };
        let _ = pre_records;

        let aux = alloc(
            o,
            0x20000,
            sh,
            Client::nc(client::AUX + i * client::AUX_STRIDE),
            0,
            false,
        )?;
        let mut pre = KVec::with_capacity(3, GFP_KERNEL)?;
        pre.push(
            alloc(o, 0x4000, sh, Client::nc(client::PRE0_PAGE + ps), 0, false)?,
            GFP_KERNEL,
        )?;
        pre.push(
            alloc(o, 0x4000, sh, Client::nc(client::PRE1_PAGE + ps), 0, false)?,
            GFP_KERNEL,
        )?;
        pre.push(
            alloc(o, 0x4000, sh, Client::nc(client::PRE2_PAGE + ps), 0, false)?,
            GFP_KERNEL,
        )?;
        let tpc = alloc(
            o,
            tpc as usize,
            sh,
            Client::nc(client::TPC + i * client::TPC_SLOT_STRIDE),
            0,
            false,
        )?;
        let tilemap = alloc(
            o,
            tilemap as usize,
            sh,
            Client::nc(client::TILEMAP + i * client::TPC_SLOT_STRIDE),
            0,
            false,
        )?;
        let heap = alloc(
            o,
            0x4000,
            sh,
            Client::nc(client::HEAP_META_PAGE + ps),
            0,
            false,
        )?;
        let scene_user = alloc(
            o,
            0x10000,
            sh,
            Client::nc(client::SCENE_USER + ps),
            0,
            false,
        )?;
        let scene_list = alloc(
            o,
            super::fw::buffer::SCENE_LIST_SIZE,
            sh,
            Client::nc(client::SCENE_LIST + ps),
            0,
            false,
        )?;
        let fs_scratch = alloc(
            o,
            0x10000,
            sh,
            Client::nc(client::FS_SCRATCH + ps),
            0,
            false,
        )?;
        let fs_work = alloc_work(
            o,
            fs::WORK_SIZE,
            client::FS_WORK_PAGE + ws,
            client::FS_WORK_OFF as usize,
        )?;
        let ta_work = alloc_work(
            o,
            ta::WORK_SIZE,
            client::TA_WORK_PAGE + ws,
            client::TA_WORK_OFF as usize,
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
            fs_work,
            ta_work,
            ta_seq: alloc(o, ta::SEQ_OBJECT, cmd_fw(), None, 0, false)?,
            fs_seq: alloc(o, fs::SEQ_OBJECT, cmd_fw(), None, 0, false)?,
            ta_scratch: alloc(o, 0x10000, sh, None, 0, false)?,
            scene: alloc(o, super::fw::work::scene::SIZE, sh, None, 0, false)?,
            // Internal TA start/end, FS start/end; legacy user cells likewise.
            its: alloc(o, 32, sh, None, 0, false)?,
            _uts: alloc(o, 32, Class::Fw, None, 0, false)?,
            own_barrier: alloc(o, barrier::SIZE, cmd_fw(), None, 0, false)?,
            prev_barrier: alloc(o, barrier::SIZE, cmd_fw(), None, 0, false)?,
            fs_dep: alloc(o, barrier::SIZE, cmd_fw(), None, 0, false)?,
        })
    }

    /// Per-use reset of a pass slot's storage. The slot is neither published nor claimed; the
    /// dirty spans are passed in.
    ///
    /// Clean same-VM reuse skips the work/sequence images (immediately overwritten by
    /// `prefix_render`), unused scratch objects (`ta_scratch`, `fs_scratch`), bulk scratch
    /// (`aux`, `scene_user`), and `tpc` (overwritten by TA hardware), zeroing only the stage
    /// timestamp cells, active `heap` header (`0x1000`), `tilemap_dirty`, and active
    /// preemption record headers. First use, cross-VM switch, cancelled preparation, or
    /// post-recovery/error reuse runs the full scrub including `tpc_dirty`.
    fn scrub(
        &self,
        pre_records: u32,
        tpc_dirty: u64,
        tilemap_dirty: u64,
        full: bool,
        stats: &mut SlotStats,
    ) -> Result {
        let r = pre_records as usize;

        if !full {
            self.its.fill(0, 0, 32)?;
            self.tilemap.fill(0, 0, tilemap_dirty as usize)?;
            stats.tilemap_clear_bytes = stats.tilemap_clear_bytes.saturating_add(tilemap_dirty);
            self.heap.fill(0, 0, 0x1000)?;
            self.pre[0].fill(0, 0, r * 0x540)?;
            self.pre[1].fill(0, 0, r * 0x280)?;
            self.pre[2].fill(0, 0, r * 0x40)?;
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
        self.tilemap.fill(0, 0, tilemap_dirty as usize)?;
        stats.tilemap_clear_bytes = stats.tilemap_clear_bytes.saturating_add(tilemap_dirty);
        self.aux.fill(0, 0, 0x20000)?;
        self.heap.fill(0, 0, 0x4000)?;
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
    pub(crate) pass: [u32; sizes::RENDER_SLOTS],
    pub(crate) pass_mask: u32,
    #[allow(dead_code)]
    pub(crate) ct: [u32; sizes::RENDER_SLOTS],
    pub(crate) ct_count: u32,
    #[allow(dead_code)]
    pub(crate) rotating: bool,
}
impl Claims {
    pub(crate) const fn new() -> Self {
        Self {
            slot: [0; sizes::COMPUTE_SLOTS],
            slot_count: 0,
            pass: [0; sizes::RENDER_SLOTS],
            pass_mask: 0,
            ct: [0; sizes::RENDER_SLOTS],
            ct_count: 0,
            rotating: true,
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
    /// Force a full scrub on next claim (first use, cancel, error, or firmware recovery).
    scrub_full: [bool; sizes::COMPUTE_UNITS],
}

/// Bitmask of valid render pass slot indices (`0..sizes::RENDER_SLOTS`).
const PASS_MASK: u32 = (1u32 << sizes::RENDER_SLOTS) - 1;

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
}

pub(crate) struct Slots {
    pub(crate) cursor: u32,
    pub(crate) pass_busy: u32,
    pub(crate) pass_cursor: u32,
    pub(crate) stats: SlotStats,
    /// The compute and render owners, on the heap: `Firmware` (and so `Slots`) is built on the
    /// probe's stack, which has no room for them.
    compute: KBox<ComputeOwners>,
    render: KBox<RenderOwners>,
}
impl Slots {
    pub(crate) fn new() -> Result<Self> {
        Ok(Self {
            cursor: 0,
            pass_busy: 0,
            pass_cursor: 0,
            stats: SlotStats::default(),
            compute: KBox::new(
                ComputeOwners {
                    lifetime: Lifetime::new(true),
                    owner: [const { None }; sizes::COMPUTE_UNITS],
                    pending_retarget: [None; sizes::COMPUTE_UNITS],
                    pending_owner: [const { None }; sizes::COMPUTE_UNITS],
                    arena: [const { None }; sizes::COMPUTE_UNITS],
                    pending_arena: [const { None }; sizes::COMPUTE_UNITS],
                    scrub_full: [true; sizes::COMPUTE_UNITS],
                },
                GFP_KERNEL,
            )?,
            render: KBox::new(
                RenderOwners {
                    lifetime: Lifetime::new(true),
                    owner: [const { None }; sizes::RENDER_SLOTS],
                    pending_retarget: [None; sizes::RENDER_SLOTS],
                    pending_owner: [const { None }; sizes::RENDER_SLOTS],
                    arena: [const { None }; sizes::RENDER_SLOTS],
                    pending_arena: [const { None }; sizes::RENDER_SLOTS],
                    scrub_full: [true; sizes::RENDER_SLOTS],
                },
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
    pub(crate) fn compute_exhausted_within(&self, n: usize) -> bool {
        self.compute.lifetime.exhausted_within(n)
    }
    pub(crate) fn render_exhausted(&self) -> bool {
        self.render.lifetime.exhausted()
    }
    /// Whether none of the first `n` render pass slots (a VM's own) can ever be claimed again.
    pub(crate) fn render_exhausted_within(&self, n: usize) -> bool {
        self.render.lifetime.exhausted_within(n)
    }
    pub(crate) fn lifetime_counts(&self) -> [super::pools::lifetime::Counts; 2] {
        [self.compute.lifetime.counts(), self.render.lifetime.counts()]
    }
    /// Whether one of the first `n` compute units can be claimed now.
    pub(crate) fn compute_available(&self, n: usize) -> bool {
        (0..n.min(sizes::COMPUTE_UNITS)).any(|k| self.compute.lifetime.available(k))
    }
    /// Whether one of the first `n` render pass slots can be claimed now.
    #[allow(dead_code)]
    pub(crate) fn pass_available(&self, n: usize) -> bool {
        (0..n.min(sizes::RENDER_SLOTS))
            .any(|k| self.pass_busy & (1 << k) == 0 && self.render.lifetime.available(k))
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
        (0..super::params::compute_units())
            .filter(|&k| self.compute.lifetime.available(k))
            .count() as u32
    }
    /// Forces a full scrub on the next claim of every slot (after firmware recovery).
    #[allow(dead_code)]
    pub(crate) fn invalidate_scrub(&mut self) {
        self.compute.scrub_full = [true; sizes::COMPUTE_UNITS];
        self.render.scrub_full = [true; sizes::RENDER_SLOTS];
    }

    /// Forces a full scrub on the next claim of the units in `f` (after an affected error).
    #[allow(dead_code)]
    pub(crate) fn mark_dirty(&mut self, f: &Claims) {
        for &k in &f.slot[..(f.slot_count as usize).min(f.slot.len())] {
            if let Some(dirty) = self.compute.scrub_full.get_mut(k as usize) {
                *dirty = true;
            }
        }
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 {
                self.render.scrub_full[k] = true;
            }
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

    pub(crate) fn claim_compute_for(
        &mut self,
        units: &super::resources::Prepared,
        f: &mut Claims,
        vm_id: Option<u64>,
    ) -> Result<Option<usize>> {
        if f.slot_count != 0 {
            return Ok(None);
        }
        // Round robin over this VM's units: the next one in publication order is the
        // oldest, the first to be displaced by a later retired publication.
        let n = units.compute_units()?.min(sizes::COMPUTE_UNITS);
        for i in 0..n {
            let k = (self.cursor as usize + i) % n;
            if self.compute.lifetime.claim(k) {
                let full = self.compute.scrub_full[k]
                    || vm_id.is_none_or(|id| {
                        self.compute.owner[k]
                            .as_ref()
                            .is_none_or(|prev| prev.id != id)
                    });
                if let Err(e) = units.compute_unit(k).and_then(|unit| unit.scrub(full)) {
                    self.compute.lifetime.cancel(k);
                    return Err(e);
                }
                f.slot[0] = k as u32;
                f.slot_count = 1;
                self.cursor = ((k + 1) % n) as u32;
                return Ok(Some(k));
            }
        }
        Ok(None)
    }
    #[allow(dead_code)]
    pub(crate) fn pick_pass(
        &self,
        arena: &super::resources::RenderArena,
        geo: &Geometry,
        picked: u32,
    ) -> Option<usize> {
        let n = arena.passes.len().min(sizes::RENDER_SLOTS);
        for i in 0..n {
            let k = (self.pass_cursor as usize + i) % n;
            let p = &arena.passes[k];
            if (self.pass_busy | picked) & (1 << k) == 0
                && self.render.lifetime.available(k)
                && geo.tpc_bytes <= p.tpc.size as u64
                && geo.tilemap_bytes <= p.tilemap.size as u64
            {
                return Some(k);
            }
        }
        None
    }
    #[allow(dead_code)]
    pub(crate) fn pick_pass_in(
        &self,
        resources: &super::resources::Prepared,
        geo: &Geometry,
        picked: u32,
    ) -> Option<usize> {
        let n = resources.render_passes().ok()?.min(sizes::RENDER_SLOTS);
        for i in 0..n {
            let k = (self.pass_cursor as usize + i) % n;
            let Ok(p) = resources.render_pass(k) else {
                continue;
            };
            if (self.pass_busy | picked) & (1 << k) == 0
                && self.render.lifetime.available(k)
                && geo.tpc_bytes <= p.tpc.size as u64
                && geo.tilemap_bytes <= p.tilemap.size as u64
            {
                return Some(k);
            }
        }
        None
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
        if k >= n || !self.render.lifetime.claim(k) {
            return Err(EBUSY);
        }
        // Only a retired displaced owner may reach here; no in-flight storage is reset.
        let full = self.render.scrub_full[k]
            || vm_id.is_none_or(|id| {
                self.render.owner[k]
                    .as_ref()
                    .is_none_or(|prev| prev.id != id)
            });
        let p = &arena.passes[k];
        if let Err(e) = p.scrub(
            1,
            arena.tpc_dirty[k].load(Ordering::Relaxed),
            arena.tilemap_dirty[k].load(Ordering::Relaxed),
            full,
            &mut self.stats,
        ) {
            self.render.lifetime.cancel(k);
            return Err(e);
        }
        if full {
            arena.tpc_dirty[k].store(0, Ordering::Relaxed);
        }
        arena.tilemap_dirty[k].store(0, Ordering::Relaxed);
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
        if k >= n || !self.render.lifetime.claim(k) {
            return Err(EBUSY);
        }
        let full = self.render.scrub_full[k]
            || vm_id.is_none_or(|id| {
                self.render.owner[k]
                    .as_ref()
                    .is_none_or(|prev| prev.id != id)
            });
        let p = resources.render_pass(k)?;
        let (tpc_dirty, tilemap_dirty) = resources.render_pass_dirty(k)?;
        // On clean same-VM reuse, zero only the overlap with the current pass's tilemap so a
        // small render never scrubs leftover bytes from an earlier large render; preserve the
        // high-water mark in `tilemap_dirty` for cross-VM or recovery full scrubs.
        let prev_tilemap = tilemap_dirty.load(Ordering::Relaxed);
        let clear_tilemap = if full {
            prev_tilemap
        } else {
            prev_tilemap.min(tilemap_bytes)
        };
        if let Err(e) = p.scrub(
            1,
            tpc_dirty.load(Ordering::Relaxed),
            clear_tilemap,
            full,
            &mut self.stats,
        ) {
            self.render.lifetime.cancel(k);
            return Err(e);
        }
        if full {
            tpc_dirty.store(0, Ordering::Relaxed);
            tilemap_dirty.store(0, Ordering::Relaxed);
        }
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
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 {
                if let Some(proof) = self.render.pending_retarget[k].take() {
                    self.stats.cross_vm_retargets[1] =
                        self.stats.cross_vm_retargets[1].saturating_add(1);
                    self.stats.last_retarget[1] = proof;
                }
            }
        }
    }

    /// Retains each actual owner's VM/ASID binding through the firmware's last-owner era.
    /// A previous lease is replaced only after the new publication fully retires.
    pub(crate) fn hold_vm(&mut self, f: &Claims, vm: &Arc<super::vm::VmState>) -> Result {
        if f.slot_count > 2
            || f.slot[..f.slot_count as usize]
                .iter()
                .any(|&k| k as usize >= sizes::COMPUTE_UNITS)
            || f.pass_mask & !PASS_MASK != 0
        {
            return Err(EINVAL);
        }
        for &k in &f.slot[..f.slot_count as usize] {
            let k = k as usize;
            self.compute.pending_retarget[k] = self.compute.owner[k]
                .as_ref()
                .and_then(|old| Self::vm_retarget(k, old, vm));
            self.compute.pending_owner[k] = Some(vm.clone());
        }
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 {
                self.render.pending_retarget[k] = self.render.owner[k]
                    .as_ref()
                    .and_then(|old| Self::vm_retarget(k, old, vm));
                self.render.pending_owner[k] = Some(vm.clone());
            }
        }
        Ok(())
    }

    /// With [`Self::hold_vm`]: the claimed units' arenas, held with their VM leases. The VM's
    /// own cache lets go of its arenas when its client closes.
    pub(crate) fn hold_arenas(&mut self, f: &Claims, resources: &super::resources::Prepared) {
        for &k in &f.slot[..(f.slot_count as usize).min(f.slot.len())] {
            if let Some(cell) = self.compute.pending_arena.get_mut(k as usize) {
                *cell = resources.compute_arena(k as usize);
            }
        }
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 {
                self.render.pending_arena[k] = resources.pass_owner_arena(k);
            }
        }
    }

    /// Reserve the actual same-slot publication. Call before producers become visible.
    pub(crate) fn publish(&mut self, f: &Claims) -> Result {
        // Validate on a private copy so a failure changes no ownership.
        let mut c = self.compute.lifetime.clone();
        let mut p = self.render.lifetime.clone();
        for &k in &f.slot[..f.slot_count as usize] {
            if !c.publish(k as usize) {
                return Err(EBUSY);
            }
        }
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 && !p.publish(k) {
                return Err(EBUSY);
            }
        }
        self.compute.lifetime = c;
        self.render.lifetime = p;
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
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 && !p.retire(k) {
                return Err(EINVAL);
            }
        }
        self.compute.lifetime = c;
        self.render.lifetime = p;
        for &k in &f.slot[..f.slot_count as usize] {
            self.compute.owner[k as usize] = self.compute.pending_owner[k as usize].take();
            self.compute.arena[k as usize] = self.compute.pending_arena[k as usize].take();
            self.compute.scrub_full[k as usize] = false;
        }
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 {
                self.render.owner[k] = self.render.pending_owner[k].take();
                self.render.arena[k] = self.render.pending_arena[k].take();
                self.render.scrub_full[k] = false;
            }
        }
        self.pass_busy &= !f.pass_mask;
        f.slot_count = 0;
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
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 {
                self.render.lifetime.quarantine(k);
                self.render.pending_retarget[k] = None;
                self.render.scrub_full[k] = true;
            }
        }
        self.pass_busy &= !f.pass_mask;
        f.slot_count = 0;
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
            self.compute.scrub_full[k as usize] = true;
        }
        for k in 0..sizes::RENDER_SLOTS {
            if f.pass_mask & (1 << k) != 0 {
                self.render.lifetime.cancel(k);
                self.render.pending_retarget[k] = None;
                self.render.pending_owner[k] = None;
                self.render.pending_arena[k] = None;
                self.render.scrub_full[k] = true;
            }
        }
        self.pass_busy &= !f.pass_mask;
        f.slot_count = 0;
        f.pass_mask = 0;
        f.ct_count = 0;
    }
}
