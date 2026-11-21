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
use super::hw::{client, engine, pte};
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

struct Allocator {
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
    fn new(o: &mut Objects, k: u64, heap: &Arc<FwObject>) -> Result<Self> {
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
                engine::RENDER_UMA_SLOT,
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
    fn refs(&self) -> Result<AllocatorRefs> {
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
    fn retired(&self) -> Result<bool> {
        let submitted = read64(&self.submitted, 0)?;
        let completed = read64(&self.descriptor, uma_manager::COMPLETED)?;
        Ok(submitted != 0 && submitted == completed)
    }
    fn retired_at(&self, target: u64) -> Result<bool> {
        let completed = read64(&self.descriptor, uma_manager::COMPLETED)?;
        Ok(target != 0 && completed >= target)
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
        let mut units = KVec::with_capacity(2, GFP_KERNEL)?;
        for k in 0..2 {
            units.push(Allocator::new(o, k, &heap)?, GFP_KERNEL)?;
        }
        Ok(Self { units, heap_blocks })
    }
    #[allow(dead_code)]
    pub(crate) fn allocator_refs_for(&self, k: usize) -> Result<AllocatorRefs> {
        self.units.get(k).ok_or(EINVAL)?.refs()
    }
    #[allow(dead_code)]
    pub(crate) fn prepare(&self, k: usize, draw: u64) -> Result<AllocatorRefs> {
        self.prepare_in_flight(k, draw, false)
    }
    /// Prepares UMA/PB allocator slot `k` (`0` or `1`). When `pipelined` is true, earlier
    /// same-VM renders are still in flight on the same active descriptor and PB manager, so
    /// the submitted counter advances cumulatively while descriptor identity is still verified.
    pub(crate) fn prepare_in_flight(
        &self,
        k: usize,
        _draw: u64,
        pipelined: bool,
    ) -> Result<AllocatorRefs> {
        let u = self.units.get(k).ok_or(EINVAL)?;
        if !pipelined && u.published.load(Ordering::Acquire) && !u.retired()? {
            return Err(EBUSY);
        }
        // Descriptor pointer/counter and slot must still identify this exact retained unit.
        if read64(&u.descriptor, 0)? != u.descriptor.va
            || u.descriptor.r32(8)? != engine::RENDER_UMA_SLOT
            || read64(&u.descriptor, 0x4c)? != u.submitted.va
        {
            return Err(EINVAL);
        }
        u.refs()
    }
    pub(crate) fn publish(&self, k: usize, submitted: u64) -> Result {
        let u = self.units.get(k).ok_or(EINVAL)?;
        if submitted != read64(&u.submitted, 0)?.checked_add(2).ok_or(EOVERFLOW)? {
            return Err(EINVAL);
        }
        u.submitted.w64(0, submitted)?;
        u.published.store(true, Ordering::Release);
        Ok(())
    }
    pub(crate) fn retired(&self, k: usize) -> Result<bool> {
        self.units.get(k).ok_or(EINVAL)?.retired()
    }
    #[allow(dead_code)]
    pub(crate) fn retired_at(&self, k: usize, target: u64) -> Result<bool> {
        self.units.get(k).ok_or(EINVAL)?.retired_at(target)
    }
    pub(crate) fn heap_gva(&self, k: usize) -> Result<u64> {
        gva(&self.units.get(k).ok_or(EINVAL)?.heap)
    }
    pub(crate) fn heap_blocks(&self) -> u32 {
        self.heap_blocks
    }
}

/// Device-wide physical PB-slot leases (`parameter_id` 0 and 1, hardware UMA slot 33).
///
/// Decoupled from per-pass storage (`PassSlot 0..RENDER_SLOTS`): same-VM renders stay on one
/// active `pb_slot` and reuse its exposed `PbEpoch` (including pipelined renders in flight),
/// while cross-VM switches or post-recovery/error renders drain and switch to the displaced
/// alternate `pb_slot` so the previous VM's UMA/PB storage remains mapped through displacement.
pub(crate) struct Pools {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) stats: PbStats,
    owners: [Option<Arc<PbEpoch>>; 2],
    #[allow(dead_code)]
    owner_vms: [Option<Arc<super::vm::VmState>>; 2],
    #[allow(dead_code)]
    owner_arenas: [Option<Arc<super::resources::RenderArena>>; 2],
    pending_vms: [Option<Arc<super::vm::VmState>>; 2],
    pending_arenas: [Option<Arc<super::resources::RenderArena>>; 2],
    /// Per allocator slot, a binding count, bumped by each fresh epoch (its InitBM rebinds the
    /// slot) and by a firmware recovery. It identifies a binding without holding the epoch.
    binds: [u64; 2],
    /// Per allocator slot: `owners[k]` may serve the next render of its VM without InitBM.
    keep: [bool; 2],
    /// Most recently published allocator slot (`0` or `1`).
    active: Option<usize>,
    /// Most recently cleanly retired allocator slot (proves the alternate slot is displaced).
    last_clean_retired: Option<usize>,
    /// Live in-flight render count per allocator slot.
    inflight: [u32; 2],
    /// Permanently quarantined allocator slots after an aborted receipt.
    quarantined: [bool; 2],
}
impl Pools {
    pub(crate) fn new(dev: &AsahiDrmDevice) -> Self {
        Self {
            dev: dev.as_ref().as_ref().into(),
            stats: PbStats::default(),
            owners: [None, None],
            owner_vms: [None, None],
            owner_arenas: [None, None],
            pending_vms: [None, None],
            pending_arenas: [None, None],
            binds: [0, 0],
            keep: [false, false],
            active: None,
            last_clean_retired: None,
            inflight: [0, 0],
            quarantined: [false, false],
        }
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
    /// Selects the physical PB/UMA allocator slot (`0` or `1`) for `vm`.
    ///
    /// When `behind` is true (pipelining behind in-flight same-VM renders), the active slot must
    /// be healthy and reusable for `vm`. When `behind` is false (engine drained), a same-VM
    /// render stays on its reusable active slot; otherwise it switches to the alternate slot
    /// only once that alternate is fresh or displaced by a clean retirement of the active slot.
    #[allow(dead_code)]
    pub(crate) fn pick_slot(&self, vm: &Arc<super::vm::VmState>, behind: bool) -> Option<usize> {
        if behind {
            let a = self.active?;
            return (!self.quarantined[a] && self.reusable(a, vm).is_some()).then_some(a);
        }
        match self.active {
            None => (!self.quarantined[0])
                .then_some(0)
                .or_else(|| (!self.quarantined[1]).then_some(1)),
            Some(a) => {
                if !self.quarantined[a] && self.reusable(a, vm).is_some() {
                    return Some(a);
                }
                let other = 1 - a;
                let displaced =
                    self.owners[other].is_none() || self.last_clean_retired == Some(a);
                (!self.quarantined[other] && displaced).then_some(other)
            }
        }
    }
    /// Whether `vm` can pipeline another render behind the currently active same-VM renders.
    #[allow(dead_code)]
    pub(crate) fn can_pipeline(&self, vm: &Arc<super::vm::VmState>) -> bool {
        self.pick_slot(vm, true).is_some()
    }
    /// Whether no allocator slot can ever accept another render (even after draining).
    #[allow(dead_code)]
    pub(crate) fn exhausted(&self) -> bool {
        if self.inflight != [0, 0] {
            return false;
        }
        if self.quarantined[0] && self.quarantined[1] {
            return true;
        }
        match self.active {
            None => false,
            Some(a) => {
                let other = 1 - a;
                let can_reuse_active = !self.quarantined[a]
                    && self.keep[a]
                    && self.owners[a]
                        .as_ref()
                        .is_some_and(|o| o.exposed.load(Ordering::Acquire));
                let can_switch_other = !self.quarantined[other]
                    && (self.owners[other].is_none() || self.last_clean_retired == Some(a));
                !can_reuse_active && !can_switch_other
            }
        }
    }
    /// Whether no allocator slot can accept a render for `vm` once the engine is drained.
    ///
    /// When one slot is quarantined and the surviving slot is owned by another VM, the global
    /// render class stays alive for that owner while other VMs must be cancelled immediately
    /// instead of spinning on `EAGAIN` until the administrative publication deadline fails the GPU.
    #[allow(dead_code)]
    pub(crate) fn exhausted_for(&self, vm: &Arc<super::vm::VmState>) -> bool {
        self.inflight == [0, 0] && self.pick_slot(vm, false).is_none()
    }
    /// Retains the candidate VM and render arena (holding the `PoolSet` UMA descriptors) before
    /// publication visibility.
    #[allow(dead_code)]
    pub(crate) fn hold_alloc(
        &mut self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        arena: Option<Arc<super::resources::RenderArena>>,
    ) -> Result {
        if k >= 2 || self.quarantined[k] {
            return Err(EINVAL);
        }
        self.pending_vms[k] = Some(vm.clone());
        self.pending_arenas[k] = arena;
        Ok(())
    }
    /// Cancels unpublished candidate retention for allocator slot `k`.
    #[allow(dead_code)]
    pub(crate) fn cancel_alloc(&mut self, k: usize) {
        if k < 2 {
            self.pending_vms[k] = None;
            self.pending_arenas[k] = None;
        }
    }
    /// Records a render reaching firmware visibility on allocator slot `k`.
    #[allow(dead_code)]
    pub(crate) fn publish_flight(&mut self, k: usize) {
        if k < 2 {
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
        if k >= 2 {
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
    /// Quarantines allocator slot `k` permanently after a passed abort, retaining all leases.
    #[allow(dead_code)]
    pub(crate) fn quarantine_flight(&mut self, k: usize) {
        if k < 2 {
            self.inflight[k] = self.inflight[k].saturating_sub(1);
            self.quarantined[k] = true;
            self.keep[k] = false;
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
        if k >= 2
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
