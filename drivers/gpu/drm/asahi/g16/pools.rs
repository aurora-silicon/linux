// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Private render UMA/heap backing and device-wide PB-slot leases.
//!
//! Each VM keeps two persistent UMA units and one PB heap they share: a render is published
//! only while no other command is in flight and retires with its blocks returned, so the two
//! pass slots' PB managers never hand out heap blocks at the same time. A render reuses its
//! pass slot's lease, its VM's last successfully retired epoch of the slot, while the slot is
//! still bound to it, unchanged (the host writes nothing to it, no InitBM). Otherwise it
//! installs a fresh PB manager with full InitBM and never reinitializes exposed cursors. The
//! previous PB-slot lease survives until the replacement's complete normal retirement.

#[path = "pools/lifetime.rs"]
pub(crate) mod lifetime;

use super::fw::buffer::{self, uma_manager};
use super::hw::{client, engine, pte, sizes};
use super::object::{Class, FwObject, Objects, Placement, Request};
use super::Firmware;
use crate::driver::AsahiDrmDevice;
use core::{
    fmt,
    sync::atomic::{AtomicBool, AtomicU32, Ordering},
};
use kernel::{
    device,
    prelude::*,
    sync::{aref::ARef, Arc},
};

#[derive(Copy, Clone, Default)]
pub(crate) struct AllocatorRefs {
    pub(crate) uma_mgr: [u64; 2],
    /// Firmware aliases of GPU-writable per-engine auxiliary state, separate from the manager.
    pub(crate) uma_aux: [u64; 2],
    pub(crate) uma_gpu: u64,
    pub(crate) pb_mgr: u64,
    pub(crate) mgr_misc: u64,
    pub(crate) init_blocks: u32,
    pub(crate) parameter_id: u32,
    #[allow(dead_code)]
    pub(crate) submitted_fw: u64,
    #[allow(dead_code)]
    pub(crate) counter_completed_fw: u64,
    pub(crate) uma_submitted: u64,
    pub(crate) init_buffer: bool,
}

pub(crate) struct Allocator {
    descriptor: Arc<FwObject>,
    auxiliary: Arc<FwObject>,
    #[allow(dead_code)]
    pool: Arc<FwObject>,
    #[allow(dead_code)]
    state: Arc<FwObject>,
    #[allow(dead_code)]
    list: Arc<FwObject>,
    submitted: Arc<FwObject>,
    heap: Arc<FwObject>,
    published: AtomicBool,
    id: u32,
}

#[derive(Default)]
pub(crate) struct PbStats {
    pub(crate) requests: u64,
    #[allow(dead_code)]
    pub(crate) grown: u64,
    #[allow(dead_code)]
    pub(crate) refused: u64,
    pub(crate) bad: u64,
    /// Pass slot bindings that reused a retired epoch / started a fresh one (a deferred
    /// render counts again when it is retried).
    pub(crate) reused: u64,
    pub(crate) fresh: u64,
}

pub(crate) struct PoolSet {
    units: KVec<Allocator>,
    heap: Arc<FwObject>,
    /// Blocks of the shared heap (the PB managers' lists cover the whole 256 MiB window).
    heap_blocks: u32,
}

fn alloc(
    o: &mut Objects,
    size: usize,
    class: Class,
    client: Option<(u64, u64)>,
    cpu: bool,
) -> Result<Arc<FwObject>> {
    o.alloc(Request {
        size,
        class,
        placement: Placement::Anywhere,
        offset: 0,
        client,
        lower: None,
        cpu,
    })
}
fn gva(o: &FwObject) -> Result<u64> {
    o.gva.ok_or(EINVAL)
}
fn read64(o: &FwObject, at: usize) -> Result<u64> {
    // Stable high/low/high observation of an unaligned firmware-owned u64.
    for _ in 0..4 {
        let high = o.r32(at + 4)?;
        let low = o.r32(at)?;
        if high == o.r32(at + 4)? {
            return Ok(u64::from(high) << 32 | u64::from(low));
        }
    }
    Err(EAGAIN)
}
fn write_records<const N: usize>(
    o: &FwObject,
    count: usize,
    record: impl Fn(usize) -> [u8; N],
) -> Result {
    let mut chunk = [0u8; 512];
    let mut first = 0;
    while first < count {
        let n = (count - first).min(512 / N);
        for j in 0..n {
            chunk[j * N..(j + 1) * N].copy_from_slice(&record(first + j));
        }
        o.write(first * N, &chunk[..n * N])?;
        first += n;
    }
    Ok(())
}
impl Allocator {
    pub(crate) fn new(o: &mut Objects, k: u64, heap: &Arc<FwObject>) -> Result<Self> {
        let nc = |va| Some((va, pte::LEAF_NC));
        // Native UMA software metadata is ASC-owned and never needs a client GPU alias.
        // In particular its firmware pointers, +0x54 completion and +0x5c exclusion flag
        // must not share a writable GPU page with the engine auxiliary records.
        let descriptor = alloc(o, uma_manager::SIZE, Class::Fw, None, true)?;
        let auxiliary = alloc(
            o,
            0x4000,
            Class::Shared,
            nc(client::render_descriptor(k)),
            false,
        )?;
        let pool = alloc(
            o,
            0x2000000,
            Class::Shared,
            nc(client::render_pool(k)),
            false,
        )?;
        let state = alloc(o, 0x20000, Class::Shared, nc(client::render_state(k)), true)?;
        let list = alloc(o, 0x4000, Class::Shared, nc(client::render_list(k)), true)?;
        let submitted = alloc(o, 0x40, Class::Fw, None, true)?;
        let heap = heap.clone();
        super::slots::init_uma_pool(&state, &list, gva(&pool)?)?;
        descriptor.write(
            0,
            &buffer::uma_manager_image(
                descriptor.va,
                engine::RENDER_UMA_SLOT + k as u32,
                gva(&state)?,
                gva(&list)?,
                submitted.va,
            ),
        )?;
        Ok(Self {
            descriptor,
            auxiliary,
            pool,
            state,
            list,
            submitted,
            heap,
            published: AtomicBool::new(false),
            id: k as u32,
        })
    }
    pub(crate) fn refs(&self) -> Result<AllocatorRefs> {
        let prior = read64(&self.submitted, 0)?;
        if prior & 1 != 0 {
            return Err(EINVAL);
        }
        let next = prior.checked_add(2).ok_or(EOVERFLOW)?;
        Ok(AllocatorRefs {
            uma_mgr: [self.descriptor.va; 2],
            uma_aux: [self.auxiliary.va + 0x100, self.auxiliary.va + 0x200],
            uma_gpu: gva(&self.auxiliary)?,
            pb_mgr: 0,
            mgr_misc: 0,
            init_blocks: buffer::MAX_BLOCKS,
            parameter_id: self.id,
            submitted_fw: self.submitted.va,
            counter_completed_fw: self.descriptor.va + 0x54,
            uma_submitted: next,
            init_buffer: true,
        })
    }
    /// Prepares this UMA/PB allocator slot. When `pipelined` is true, earlier same-VM renders
    /// are still in flight on the same active descriptor and PB manager, so the submitted
    /// counter advances cumulatively while descriptor identity is still verified.
    pub(crate) fn prepare_in_flight(&self, _draw: u64, pipelined: bool) -> Result<AllocatorRefs> {
        if !pipelined && self.published.load(Ordering::Acquire) && !self.retired()? {
            return Err(EBUSY);
        }
        // Descriptor pointer/counter and slot must still identify this exact retained unit.
        if read64(&self.descriptor, 0)? != self.descriptor.va
            || self.descriptor.r32(8)? != engine::RENDER_UMA_SLOT + self.id
            || read64(&self.descriptor, 0x4c)? != self.submitted.va
        {
            return Err(EINVAL);
        }
        self.refs()
    }
    pub(crate) fn publish_batch(&self, submitted: u64, renders: u32) -> Result {
        if renders == 0 {
            return Err(EINVAL);
        }
        let delta = u64::from(renders).checked_mul(2).ok_or(EOVERFLOW)?;
        if submitted
            != read64(&self.submitted, 0)?
                .checked_add(delta)
                .ok_or(EOVERFLOW)?
        {
            return Err(EINVAL);
        }
        self.submitted.w64(0, submitted)?;
        self.published.store(true, Ordering::Release);
        Ok(())
    }
    pub(crate) fn retired(&self) -> Result<bool> {
        let submitted = read64(&self.submitted, 0)?;
        let completed = read64(&self.descriptor, uma_manager::COMPLETED)?;
        Ok(submitted != 0 && submitted == completed)
    }
    pub(crate) fn retired_at(&self, target: u64) -> Result<bool> {
        let completed = read64(&self.descriptor, uma_manager::COMPLETED)?;
        Ok(target != 0 && completed >= target)
    }
    pub(crate) fn heap_gva(&self) -> Result<u64> {
        gva(&self.heap)
    }
}
impl PoolSet {
    pub(crate) fn new(o: &mut Objects) -> Result<Self> {
        // The firmware copes with a full PB without the host (its overflow counters move and
        // the scene completes), so the heap is sized well below the 256 MiB window.
        let heap_blocks = super::params::pb_heap_blocks().min(buffer::MAX_BLOCKS);
        let heap = alloc(
            o,
            heap_blocks as usize * buffer::BLOCK_BYTES as usize,
            Class::Shared,
            Some((client::pb_heap(0), pte::LEAF_NC)),
            false,
        )?;
        let mut units = KVec::with_capacity(sizes::BASE_PB_SLOTS, GFP_KERNEL)?;
        for k in 0..sizes::BASE_PB_SLOTS as u64 {
            units.push(Allocator::new(o, k, &heap)?, GFP_KERNEL)?;
        }
        Ok(Self {
            units,
            heap,
            heap_blocks,
        })
    }
    #[allow(dead_code)]
    pub(crate) fn unit(&self, k: usize) -> Option<&Allocator> {
        self.units.get(k)
    }
    #[allow(dead_code)]
    pub(crate) fn heap(&self) -> &Arc<FwObject> {
        &self.heap
    }
    #[allow(dead_code)]
    pub(crate) fn allocator_refs_for(&self, k: usize) -> Result<AllocatorRefs> {
        self.units.get(k).ok_or(EINVAL)?.refs()
    }
    #[allow(dead_code)]
    pub(crate) fn prepare(&self, k: usize, draw: u64) -> Result<AllocatorRefs> {
        self.prepare_in_flight(k, draw, false)
    }
    /// Prepares base UMA/PB allocator slot `k` (`0..BASE_PB_SLOTS`).
    #[allow(dead_code)]
    pub(crate) fn prepare_in_flight(
        &self,
        k: usize,
        draw: u64,
        pipelined: bool,
    ) -> Result<AllocatorRefs> {
        self.units
            .get(k)
            .ok_or(EINVAL)?
            .prepare_in_flight(draw, pipelined)
    }
    #[allow(dead_code)]
    pub(crate) fn publish_batch(&self, k: usize, submitted: u64, renders: u32) -> Result {
        self.units
            .get(k)
            .ok_or(EINVAL)?
            .publish_batch(submitted, renders)
    }
    #[allow(dead_code)]
    pub(crate) fn retired(&self, k: usize) -> Result<bool> {
        self.units.get(k).ok_or(EINVAL)?.retired()
    }
    #[allow(dead_code)]
    pub(crate) fn retired_at(&self, k: usize, target: u64) -> Result<bool> {
        self.units.get(k).ok_or(EINVAL)?.retired_at(target)
    }
    #[allow(dead_code)]
    pub(crate) fn heap_gva(&self, k: usize) -> Result<u64> {
        self.units.get(k).ok_or(EINVAL)?.heap_gva()
    }
    pub(crate) fn heap_blocks(&self) -> u32 {
        self.heap_blocks
    }
}

/// Device-wide physical PB-slot leases (`parameter_id` `0..PB_SLOTS`, hardware UMA slot 33).
///
/// Decoupled from per-pass storage (`PassSlot 0..RENDER_SLOTS`): same-VM renders stay on one
/// active `pb_slot` and reuse its exposed `PbEpoch` (including pipelined renders in flight),
/// while cross-VM switches or post-recovery/error renders drain and switch to a displaced or
/// fresh alternate `pb_slot` so the previous VM's UMA/PB storage remains mapped through
/// displacement. Under normal non-fault operation the working set stays on the two base slots;
/// recovery slots `BASE_PB_SLOTS..PB_SLOTS` are claimed only after a render hang permanently
/// quarantines a slot.
pub(crate) struct Pools {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) stats: PbStats,
    owners: [Option<Arc<PbEpoch>>; sizes::PB_SLOTS],
    #[allow(dead_code)]
    owner_vms: [Option<Arc<super::vm::VmState>>; sizes::PB_SLOTS],
    #[allow(dead_code)]
    owner_arenas: [Option<super::resources::PbOwnerArena>; sizes::PB_SLOTS],
    pending_vms: [Option<Arc<super::vm::VmState>>; sizes::PB_SLOTS],
    pending_arenas: [Option<super::resources::PbOwnerArena>; sizes::PB_SLOTS],
    /// Per allocator slot, a binding count, bumped by each fresh epoch (its InitBM rebinds the
    /// slot) and by a firmware recovery. It identifies a binding without holding the epoch.
    binds: [u64; sizes::PB_SLOTS],
    /// Per allocator slot: `owners[k]` may serve the next render of its VM without InitBM.
    keep: [bool; sizes::PB_SLOTS],
    /// Most recently published allocator slot (`0..PB_SLOTS`).
    active: Option<usize>,
    /// Most recently cleanly retired allocator slot (proves every other slot is displaced).
    last_clean_retired: Option<usize>,
    /// Live in-flight render count per allocator slot.
    inflight: [u32; sizes::PB_SLOTS],
    /// Permanently quarantined allocator slots after an aborted receipt.
    quarantined: [bool; sizes::PB_SLOTS],
    /// Fresh PB epoch retained when an aborted render had not yet replaced `owners[k]`.
    quarantined_epochs: [Option<Arc<PbEpoch>>; sizes::PB_SLOTS],
}
impl Pools {
    pub(crate) fn new(dev: &AsahiDrmDevice) -> Self {
        Self {
            dev: dev.as_ref().as_ref().into(),
            stats: PbStats::default(),
            owners: [const { None }; sizes::PB_SLOTS],
            owner_vms: [const { None }; sizes::PB_SLOTS],
            owner_arenas: [const { None }; sizes::PB_SLOTS],
            pending_vms: [const { None }; sizes::PB_SLOTS],
            pending_arenas: [const { None }; sizes::PB_SLOTS],
            binds: [0; sizes::PB_SLOTS],
            keep: [false; sizes::PB_SLOTS],
            active: None,
            last_clean_retired: None,
            inflight: [0; sizes::PB_SLOTS],
            quarantined: [false; sizes::PB_SLOTS],
            quarantined_epochs: [const { None }; sizes::PB_SLOTS],
        }
    }
    /// Number of contiguous PB/UMA allocator slots (`0..n`) each rendering VM should prepare
    /// so at least [`sizes::BASE_PB_SLOTS`] healthy slots remain available across quarantines.
    #[allow(dead_code)]
    pub(crate) fn needed_slots(&self) -> usize {
        let quarantined = self.quarantined.iter().filter(|&&q| q).count();
        sizes::BASE_PB_SLOTS
            .saturating_add(quarantined)
            .min(sizes::PB_SLOTS)
    }
    /// Number of in-flight render prefixes currently active on allocator slot `k`.
    #[allow(dead_code)]
    pub(crate) fn slot_inflight(&self, k: usize) -> u32 {
        self.inflight.get(k).copied().unwrap_or(0)
    }
    /// Whether slot `k` is healthy, idle, not active, and either fresh or displaced by a clean
    /// retirement of some other slot.
    fn slot_displaced(&self, k: usize) -> bool {
        !self.quarantined[k]
            && self.inflight[k] == 0
            && self.active != Some(k)
            && (self.owners[k].is_none() || self.last_clean_retired.is_some_and(|r| r != k))
    }
    /// Allocator slot `k`'s owner epoch, if its render retired normally while the slot was still
    /// bound to it, it belongs to `vm` and was exposed (its InitBM ran). InitBM binds the
    /// device-wide slot to one epoch and VM; a fresh epoch handed to the slot rebinds it, so
    /// the owner is then stale and the render starts fresh. Adds no reference beyond the owner
    /// lease itself.
    pub(crate) fn reusable(&self, k: usize, vm: &Arc<super::vm::VmState>) -> Option<Arc<PbEpoch>> {
        if *self.quarantined.get(k)? || !*self.keep.get(k)? {
            return None;
        }
        let owner = self.owners.get(k)?.as_ref()?;
        (Arc::ptr_eq(&owner._vm, vm) && owner.exposed.load(Ordering::Acquire))
            .then(|| owner.clone())
    }
    /// Selects a physical PB/UMA allocator slot within `0..limit` for `vm`.
    ///
    /// When `behind` is true (pipelining behind active flights), only a healthy warm reusable
    /// slot already bound to `vm` (preferring `self.active`, then any other slot in `0..limit`)
    /// is eligible; cold VMs requiring `InitBM` return `None` so the engine drains first.
    /// When `behind` is false (engine drained), a warm reusable slot for `vm` is preferred;
    /// otherwise an already-used displaced slot is chosen first (preserving the 2-slot working
    /// set) or the lowest fresh slot is claimed.
    pub(crate) fn pick_slot_within(
        &self,
        vm: &Arc<super::vm::VmState>,
        behind: bool,
        limit: usize,
    ) -> Option<usize> {
        let n = limit.min(sizes::PB_SLOTS);
        if let Some(a) = self.active {
            if a < n && self.reusable(a, vm).is_some() {
                return Some(a);
            }
        }
        if let Some(k) = (0..n).find(|&k| self.reusable(k, vm).is_some()) {
            return Some(k);
        }
        if behind {
            return None;
        }
        // Prefer reusing a displaced slot over claiming a fresh recovery slot.
        (0..n)
            .find(|&k| self.slot_displaced(k) && self.owners[k].is_some())
            .or_else(|| (0..n).find(|&k| self.slot_displaced(k)))
    }
    /// Selects a physical PB/UMA allocator slot across all `0..PB_SLOTS` for `vm`.
    #[allow(dead_code)]
    pub(crate) fn pick_slot(&self, vm: &Arc<super::vm::VmState>, behind: bool) -> Option<usize> {
        self.pick_slot_within(vm, behind, sizes::PB_SLOTS)
    }
    /// Whether `vm` has a warm reusable PB slot within `0..limit` and can pipeline without InitBM.
    #[allow(dead_code)]
    pub(crate) fn can_pipeline_within(&self, vm: &Arc<super::vm::VmState>, limit: usize) -> bool {
        self.pick_slot_within(vm, true, limit).is_some()
    }
    /// Whether `vm` has a warm reusable PB slot and can pipeline without InitBM.
    #[allow(dead_code)]
    pub(crate) fn can_pipeline(&self, vm: &Arc<super::vm::VmState>) -> bool {
        self.can_pipeline_within(vm, sizes::PB_SLOTS)
    }
    /// Whether no allocator slot can ever accept another render (even after draining).
    #[allow(dead_code)]
    pub(crate) fn exhausted(&self) -> bool {
        if self.inflight.iter().any(|&c| c != 0) {
            return false;
        }
        if (0..sizes::PB_SLOTS).any(|k| {
            !self.quarantined[k]
                && self.keep[k]
                && self.owners[k]
                    .as_ref()
                    .is_some_and(|o| o.exposed.load(Ordering::Acquire))
        }) {
            return false;
        }
        !(0..sizes::PB_SLOTS).any(|k| self.slot_displaced(k))
    }
    /// Whether no allocator slot within `0..limit` can accept a render for `vm` once drained.
    ///
    /// When a VM prepared fewer PB slots than needed after a render hang (or when only one
    /// surviving slot remains and belongs to another VM), that job must cancel immediately
    /// instead of spinning on `EAGAIN` until the administrative publication deadline fails the GPU.
    #[allow(dead_code)]
    pub(crate) fn exhausted_for_within(&self, vm: &Arc<super::vm::VmState>, limit: usize) -> bool {
        self.inflight.iter().all(|&c| c == 0) && self.pick_slot_within(vm, false, limit).is_none()
    }
    /// Whether no allocator slot can accept a render for `vm` once the engine is drained.
    #[allow(dead_code)]
    pub(crate) fn exhausted_for(&self, vm: &Arc<super::vm::VmState>) -> bool {
        self.exhausted_for_within(vm, sizes::PB_SLOTS)
    }
    /// Retains the candidate VM and owning render/recovery arena (holding the UMA descriptors)
    /// before publication visibility.
    #[allow(dead_code)]
    pub(crate) fn hold_alloc(
        &mut self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        arena: Option<super::resources::PbOwnerArena>,
    ) -> Result {
        if k >= sizes::PB_SLOTS || self.quarantined[k] {
            return Err(EINVAL);
        }
        self.pending_vms[k] = Some(vm.clone());
        self.pending_arenas[k] = arena;
        Ok(())
    }
    /// Cancels unpublished candidate retention for allocator slot `k`.
    #[allow(dead_code)]
    pub(crate) fn cancel_alloc(&mut self, k: usize) {
        if k < sizes::PB_SLOTS {
            self.pending_vms[k] = None;
            self.pending_arenas[k] = None;
        }
    }
    /// Records a render reaching firmware visibility on allocator slot `k`.
    #[allow(dead_code)]
    pub(crate) fn publish_flight(&mut self, k: usize) {
        if k < sizes::PB_SLOTS {
            self.active = Some(k);
            self.inflight[k] = self.inflight[k].saturating_add(1);
        }
    }
    /// Retires one render flight on allocator slot `k` after a full normal receipt.
    #[allow(dead_code)]
    pub(crate) fn retire_flight(
        &mut self,
        k: usize,
        epoch: &Arc<PbEpoch>,
        binding: u64,
        completed: bool,
    ) -> Result {
        if k >= sizes::PB_SLOTS {
            return Err(EINVAL);
        }
        self.inflight[k] = self.inflight[k].saturating_sub(1);
        if self.quarantined[k] {
            return Ok(());
        }
        self.owners[k] = Some(epoch.clone());
        if let Some(vm) = self.pending_vms[k].take() {
            self.owner_vms[k] = Some(vm);
        }
        if let Some(arena) = self.pending_arenas[k].take() {
            self.owner_arenas[k] = Some(arena);
        }
        self.last_clean_retired = Some(k);
        self.retire_epoch(k, binding, completed);
        Ok(())
    }
    /// Quarantines allocator slot `k` permanently after a passed abort, retaining all leases
    /// including any fresh epoch published on the aborted flight alongside `owners[k]`.
    #[allow(dead_code)]
    pub(crate) fn quarantine_flight(&mut self, k: usize, pb: Option<Arc<PbEpoch>>) {
        if k < sizes::PB_SLOTS {
            self.inflight[k] = self.inflight[k].saturating_sub(1);
            self.quarantined[k] = true;
            self.keep[k] = false;
            if let Some(epoch) = pb {
                if self.quarantined_epochs[k].is_none() {
                    self.quarantined_epochs[k] = Some(epoch);
                } else {
                    // Already retaining an aborted epoch on this slot; never unmap an exposed
                    // epoch or allocate under the device lock.
                    core::mem::forget(epoch);
                }
            }
        } else if let Some(epoch) = pb {
            core::mem::forget(epoch);
        }
    }
    /// Slot `k` is handed to a render's epoch, `reused` or fresh (with InitBM). Returns the
    /// binding the render retires with.
    pub(crate) fn bind(&mut self, k: usize, reused: bool) -> u64 {
        let (Some(binds), Some(keep)) = (self.binds.get_mut(k), self.keep.get_mut(k)) else {
            return 0;
        };
        if !reused {
            *binds = binds.wrapping_add(1);
            *keep = false;
        }
        *binds
    }
    /// A render on slot `k` with `binding` retired, after its `retire_binding`. If no other
    /// epoch was bound and no recovery started since, the next render of the same VM may reuse
    /// its epoch after a `completed` render; after a failed one it starts fresh.
    pub(crate) fn retire_epoch(&mut self, k: usize, binding: u64, completed: bool) {
        if self.binds.get(k) == Some(&binding) {
            if let Some(keep) = self.keep.get_mut(k) {
                *keep = completed && !self.quarantined[k];
            }
        }
    }
    /// Reuse nothing on slot `k` (its render was aborted).
    #[allow(dead_code)]
    pub(crate) fn forget(&mut self, k: usize) {
        if let Some(keep) = self.keep.get_mut(k) {
            *keep = false;
        }
    }
    /// A firmware recovery started: no epoch bound so far is reused, also not by a render that
    /// retires after it (its binding no longer matches); each slot's next render starts fresh.
    pub(crate) fn invalidate(&mut self) {
        for (binds, keep) in self.binds.iter_mut().zip(self.keep.iter_mut()) {
            *binds = binds.wrapping_add(1);
            *keep = false;
        }
    }
    #[allow(dead_code)]
    pub(crate) fn retire_binding(&mut self, k: usize, epoch: &Arc<PbEpoch>) -> Result {
        *self.owners.get_mut(k).ok_or(EINVAL)? = Some(epoch.clone());
        Ok(())
    }
    pub(crate) fn write_stats(&self, f: &mut impl fmt::Write) -> fmt::Result {
        writeln!(
            f,
            "pb: M4 private allocators blocks {} requests {} bad {} reused {} fresh {}",
            buffer::MAX_BLOCKS,
            self.stats.requests,
            self.stats.bad,
            self.stats.reused,
            self.stats.fresh
        )
    }
}

/// One never-reinitialized PB manager installation. Unless a same-VM command reuses its
/// slot's retired epoch, every command gets a fresh manager and full InitBM; no host reset of
/// exposed firmware cursors is required.
pub(crate) struct PbEpoch(crate::cleanup::Deferred<PbEpochInner>);
impl core::ops::Deref for PbEpoch {
    type Target = PbEpochInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct PbEpochInner {
    pub(crate) manager: Arc<FwObject>,
    pages: Arc<FwObject>,
    blocks: Arc<FwObject>,
    ring: Arc<FwObject>,
    counter: Arc<FwObject>,
    pub(crate) misc: Arc<FwObject>,
    pub(crate) initbm: Arc<FwObject>,
    assigned: AtomicU32,
    exposed: AtomicBool,
    // Unmap private records before releasing the VM's aliases and ASID.
    _objects: Objects,
    _vm: Arc<super::vm::VmState>,
    // Return admission credit only after deferred unmap and binding release.
    _permit: super::budget::Permit,
}
impl PbEpoch {
    pub(crate) fn new(
        factory: &super::object::Factory,
        vm: &Arc<super::vm::VmState>,
        permit: super::budget::Permit,
    ) -> Result<Arc<Self>> {
        let mut o = factory.objects(&vm.vm);
        let records = alloc(&mut o, 0x500, Class::Fw, None, true)?;
        let mut inner = PbEpochInner {
            manager: FwObject::view(&records, 0, 0xc0)?,
            pages: o.alloc_dynamic_client(buffer::PAGE_LIST_SIZE)?,
            blocks: alloc(&mut o, buffer::BLOCK_LIST_SIZE, Class::Fw, None, true)?,
            ring: FwObject::view(&records, 0x100, 8)?,
            counter: FwObject::view(&records, 0x200, 0x40)?,
            misc: FwObject::view(&records, 0x300, 0x40)?,
            initbm: FwObject::view(&records, 0x400, 0x20)?,
            assigned: AtomicU32::new(u32::MAX),
            exposed: AtomicBool::new(false),
            _objects: o,
            _vm: vm.clone(),
            _permit: permit,
        };
        inner._objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(inner)?),
            GFP_KERNEL,
        )?)
    }

    /// Seed only unpublished storage. A retry may choose another physical slot; after
    /// exposure neither the manager nor any of its firmware-mutated lists is reset.
    pub(crate) fn prepare(&self, k: usize, heap: u64, heap_blocks: u32) -> Result {
        if k >= sizes::PB_SLOTS
            || self.exposed.load(Ordering::Acquire)
            || heap_blocks == 0
            || heap_blocks > buffer::MAX_BLOCKS
        {
            return Err(EINVAL);
        }
        // Only the backed part of the heap is named; the rest of the lists stays zero.
        let pages = heap_blocks * buffer::PAGES_PER_BLOCK;
        write_records(&self.pages, buffer::MAX_PAGES as usize, |i| {
            let i = i as u32;
            (if i < pages { buffer::page_record(heap, i) } else { 0 }).to_le_bytes()
        })?;
        write_records(&self.blocks, buffer::MAX_BLOCKS as usize, |i| {
            let b = i as u32;
            u64::from(if b < heap_blocks { buffer::block_record(heap, b) } else { 0 }).to_le_bytes()
        })?;
        let posted = super::params::pb_posted_blocks(heap_blocks);
        self.ring.w32(0, posted)?;
        self.ring.w32(4, posted)?;
        self.counter.w64(0, 1)?;
        self.manager.write(
            0,
            &buffer::pb_manager_image(
                k as u32,
                self.pages.va,
                gva(&self.pages)?,
                self.blocks.va,
                self.ring.va,
                self.counter.va,
                heap_blocks,
            ),
        )?;
        self.assigned.store(k as u32, Ordering::Release);
        Ok(())
    }
    pub(crate) fn check_assignment(&self, k: usize) -> Result {
        if self.assigned.load(Ordering::Acquire) != k as u32 {
            return Err(EINVAL);
        }
        Ok(())
    }
    pub(crate) fn expose(&self) {
        self.exposed.store(true, Ordering::Release);
    }
    /// Never handed to the firmware: its storage may serve another render of its VM.
    pub(crate) fn unexposed(&self) -> bool {
        !self.exposed.load(Ordering::Acquire)
    }
}

impl Firmware {
    pub(crate) fn init_render_pools(&mut self, rebuild: bool) -> Result {
        // An abort must quarantine its allocation set. M4 has no qualified allocator reset.
        if rebuild {
            return Err(EIO);
        }
        Ok(())
    }
    pub(crate) fn answer_growth(&mut self) -> Result {
        // M4 publishes its entire bounded heap up front; G15's event-7/device-control
        // protocol is not an M4 ABI. Never synthesize an unqualified firmware reply.
        if !self.events.grow.is_empty() {
            self.pools.stats.requests += self.events.grow.len() as u64;
            self.pools.stats.bad += self.events.grow.len() as u64;
            return Err(super::boot::eproto());
        }
        Ok(())
    }
}
