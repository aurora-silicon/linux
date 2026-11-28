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
use super::object::{Class, Factory, FwObject, Objects, Request};
use super::resources::{PbRecordLease, RenderUmaLease};
use super::slots::Placement;
use super::Firmware;
use crate::driver::AsahiDrmDevice;
use core::{
    fmt,
    sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
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
    /// Device-lifetime firmware lease retaining ASC-cached `descriptor` (`FwCoherent`) and
    /// uncached `submitted` (`Fw`) across closed-VM reclaim until a later render overwrites
    /// this hardware UMA slot in firmware RAM.
    uma_lease: Arc<RenderUmaLease>,
    published: AtomicBool,
    retired_submitted: AtomicU64,
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
    /// Retirements that replaced an open VM's owner epoch still kept as that VM's last epoch.
    pub(crate) kept: u64,
}

pub(crate) struct PoolSet {
    units: KVec<Allocator>,
    heap: Arc<FwObject>,
    /// Blocks of the shared heap (the PB managers' lists cover the whole 256 MiB window).
    heap_blocks: u32,
    /// Freelist entries of every allocator's UMA pool.
    seeds: u32,
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
        placement: super::object::Placement::Anywhere,
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
    /// Render allocator at per-VM VA index `k` and matching device-wide hardware slot `k`.
    pub(crate) fn new(
        o: &mut Objects,
        fw_factory: &Factory,
        k: u64,
        heap: &Arc<FwObject>,
        placement: Placement,
    ) -> Result<Self> {
        Self::new_at(o, fw_factory, k, k as u32, heap, placement)
    }
    /// Render allocator mapped at per-VM local VA index `local_idx` (`0..MAX_VM_PB_SLOTS`)
    /// and bound to device-wide hardware slot `hw_slot` (`parameter_id`, UMA slot
    /// `RENDER_UMA_SLOT + hw_slot`).
    pub(crate) fn new_at(
        o: &mut Objects,
        fw_factory: &Factory,
        local_idx: u64,
        hw_slot: u32,
        heap: &Arc<FwObject>,
        placement: Placement,
    ) -> Result<Self> {
        placement.check()?;
        if local_idx >= sizes::MAX_VM_PB_SLOTS as u64 || (hw_slot as usize) >= sizes::PB_SLOTS {
            return Err(EINVAL);
        }
        let (pool_va, state_va, freelist_va) = match placement {
            Placement::Base => (
                client::render_pool(local_idx),
                client::render_state(local_idx),
                client::render_state(local_idx),
            ),
            Placement::Scs { .. } => {
                if local_idx >= sizes::BASE_PB_SLOTS as u64 {
                    return Err(EINVAL);
                }
                let pool = client::scs_render_pool(local_idx);
                (
                    pool,
                    client::scs_render_records(local_idx),
                    pool + placement.pool_bytes() as u64,
                )
            }
        };
        let nc = |va| Some((va, pte::LEAF_NC));
        // Native UMA software metadata is ASC-owned and never needs a client GPU alias.
        // Keep descriptor (FwCoherent) + submitted (Fw) in a device-lifetime RenderUmaLease so
        // firmware caches the descriptor like macOS 26B123 (`0x443`) while keeping the global
        // UMA table mapped across closed-VM reclaim without pinning the VM's ASID or pools.
        let (uma_lease, used) = fw_factory.render_uma_pool().take(fw_factory)?;
        let descriptor = uma_lease.descriptor.clone();
        let submitted = uma_lease.submitted.clone();
        if used {
            submitted.fill(0, 0, submitted.size)?;
            descriptor.fill(0, 0, uma_manager::SIZE)?;
        }
        let auxiliary = alloc(
            o,
            0x4000,
            Class::Shared,
            nc(state_va + client::RENDER_DESCRIPTOR_OFF),
            false,
        )?;
        let pool = alloc(o, placement.pool_bytes(), Class::Shared, nc(pool_va), false)?;
        let state = alloc(
            o,
            placement.freelist_bytes(),
            Class::Shared,
            nc(freelist_va),
            true,
        )?;
        let list = alloc(
            o,
            0x4000,
            Class::Shared,
            nc(state_va + client::RENDER_LIST_OFF),
            true,
        )?;
        let heap = heap.clone();
        super::slots::init_uma_pool(&state, &list, gva(&pool)?, placement.seeds())?;
        descriptor.write(
            0,
            &buffer::uma_manager_image(
                descriptor.va,
                engine::RENDER_UMA_SLOT + hw_slot,
                gva(&state)?,
                gva(&list)?,
                submitted.va,
                placement.seeds(),
                super::rt::freelist_records(placement.seeds()),
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
            uma_lease,
            published: AtomicBool::new(false),
            retired_submitted: AtomicU64::new(0),
            id: hw_slot,
        })
    }
    /// Device-lifetime lease retaining this allocator's UMA descriptor and submitted counter.
    pub(crate) fn uma_lease(&self) -> Arc<RenderUmaLease> {
        self.uma_lease.clone()
    }
    /// Device-wide hardware PB/UMA slot (`parameter_id`) owned by this allocator unit.
    pub(crate) fn id(&self) -> u32 {
        self.id
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
    /// counter advances cumulatively.
    pub(crate) fn prepare_in_flight(&self, _draw: u64, pipelined: bool) -> Result<AllocatorRefs> {
        let published = self.published.load(Ordering::Acquire);
        if !pipelined && published && !self.retired()? {
            return Err(EBUSY);
        }
        // Descriptor pointer/counter and slot must identify this retained unit before first
        // publication; once firmware owns the ASC-cached descriptor, avoid snooping it on every
        // render.
        if !published
            && (read64(&self.descriptor, 0)? != self.descriptor.va
                || self.descriptor.r32(8)? != engine::RENDER_UMA_SLOT + self.id
                || read64(&self.descriptor, 0x4c)? != self.submitted.va)
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
        self.uma_lease.publish();
        self.submitted.w64(0, submitted)?;
        self.published.store(true, Ordering::Release);
        Ok(())
    }
    pub(crate) fn retired(&self) -> Result<bool> {
        let submitted = read64(&self.submitted, 0)?;
        Ok(submitted != 0 && submitted == self.retired_submitted.load(Ordering::Acquire))
    }
    /// Whether the uncached submission counter accounts for `renders` additional renders
    /// reaching `target` beyond previously retired flights, and matches exact host conservation
    /// when `idle` drains render flights. The descriptor itself is ASC-cached (`FwCoherent`) so
    /// `COMPLETED` is not read by the host: firmware advances `COMPLETED` strictly before
    /// storing the flight's uncached `FS_STAMP`.
    pub(crate) fn flight_retired(&self, target: u64, renders: u32, idle: bool) -> Result<bool> {
        if target == 0 || renders == 0 {
            return Ok(false);
        }
        let delta = u64::from(renders).checked_mul(2).ok_or(EOVERFLOW)?;
        let needed = self
            .retired_submitted
            .load(Ordering::Acquire)
            .checked_add(delta)
            .ok_or(EOVERFLOW)?;
        let submitted = read64(&self.submitted, 0)?;
        Ok(target == needed && submitted >= target && (!idle || submitted == target))
    }
    /// Records a normal full retirement of `renders` render passes reaching `target`.
    pub(crate) fn note_retired(&self, target: u64, renders: u32) -> Result {
        if target == 0 || renders == 0 {
            return Err(EINVAL);
        }
        let delta = u64::from(renders).checked_mul(2).ok_or(EOVERFLOW)?;
        let expected = self
            .retired_submitted
            .load(Ordering::Acquire)
            .checked_add(delta)
            .ok_or(EOVERFLOW)?;
        if target != expected {
            return Err(EINVAL);
        }
        self.retired_submitted.store(target, Ordering::Release);
        self.uma_lease.retire();
        Ok(())
    }
    pub(crate) fn heap_gva(&self) -> Result<u64> {
        gva(&self.heap)
    }
}
/// Maps one contiguous PB heap extension chunk (`start_block..start_block + blocks`) inside the
/// VM's 256 MiB [`client::PB_HEAP_SIZE`] window so demand-grown heaps extend seamlessly after the
/// base [`PoolSet`] allocation without unmapping any in-flight base range.
#[allow(dead_code)]
pub(crate) fn alloc_pb_heap_chunk(
    o: &mut Objects,
    start_block: u32,
    blocks: u32,
) -> Result<Arc<FwObject>> {
    if blocks == 0
        || start_block
            .checked_add(blocks)
            .is_none_or(|end| end > buffer::MAX_BLOCKS)
    {
        return Err(EINVAL);
    }
    let gva = client::pb_heap(0) + u64::from(start_block) * buffer::BLOCK_BYTES;
    alloc(
        o,
        blocks as usize * buffer::BLOCK_BYTES as usize,
        Class::Shared,
        Some((gva, pte::LEAF_NC)),
        false,
    )
}

impl PoolSet {
    pub(crate) fn new(o: &mut Objects, fw_factory: &Factory) -> Result<Self> {
        Self::new_with_slots(o, fw_factory, [0, 1])
    }
    /// Base render allocator units mapped at per-VM local VA indices `0..BASE_PB_SLOTS` and
    /// bound to device-wide physical PB/UMA slots `slots`.
    pub(crate) fn new_with_slots(
        o: &mut Objects,
        fw_factory: &Factory,
        slots: [usize; sizes::BASE_PB_SLOTS],
    ) -> Result<Self> {
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
        for (local, &hw_slot) in slots.iter().enumerate() {
            units.push(
                Allocator::new_at(
                    o,
                    fw_factory,
                    local as u64,
                    hw_slot as u32,
                    &heap,
                    Placement::Base,
                )?,
                GFP_KERNEL,
            )?;
        }
        Ok(Self {
            units,
            heap,
            heap_blocks,
            seeds: Placement::Base.seeds(),
        })
    }
    /// The SCS-capable allocators of this set's VM (ray-query scratch): one per base PB slot,
    /// sharing this set's PB heap and hardware slot IDs, each with a pool of `seeds` entries
    /// ([`super::rt::pool_seeds`]). PB epoch reuse keeps base and SCS bindings strictly disjoint.
    pub(crate) fn scs(&self, o: &mut Objects, fw_factory: &Factory, seeds: u32) -> Result<Self> {
        let heap = self.heap.clone();
        let placement = Placement::Scs { seeds };
        let mut units = KVec::with_capacity(sizes::BASE_PB_SLOTS, GFP_KERNEL)?;
        for (local, unit) in self.units.iter().enumerate() {
            units.push(
                Allocator::new_at(o, fw_factory, local as u64, unit.id(), &heap, placement)?,
                GFP_KERNEL,
            )?;
        }
        Ok(Self {
            units,
            heap,
            heap_blocks: self.heap_blocks,
            seeds,
        })
    }
    /// Freelist entries of the set's UMA pools.
    pub(crate) fn seeds(&self) -> u32 {
        self.seeds
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
    pub(crate) fn flight_retired(
        &self,
        k: usize,
        target: u64,
        renders: u32,
        idle: bool,
    ) -> Result<bool> {
        self.units
            .get(k)
            .ok_or(EINVAL)?
            .flight_retired(target, renders, idle)
    }
    #[allow(dead_code)]
    pub(crate) fn note_retired(&self, k: usize, target: u64, renders: u32) -> Result {
        self.units
            .get(k)
            .ok_or(EINVAL)?
            .note_retired(target, renders)
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
    /// The epoch whose manager `pb_mgr_table[k]` names, kept until a newer epoch retires on
    /// slot `k`, also after its VM closed: it maps the manager's page and block lists and keeps
    /// the VM's TTBAT slot bound, the context through which those lists are written.
    owners: [Option<Arc<PbEpoch>>; sizes::PB_SLOTS],
    /// Device-lifetime firmware leases keeping each `parameter_id`'s PB manager, misc, InitBM,
    /// block-list, and uncached control/counter records mapped for firmware's global
    /// `pb_mgr_table[parameter_id]` across closed-VM reclaim until a newer `InitBM` retires on
    /// that exact slot.
    retained_pb: [Option<Arc<PbRecordLease>>; sizes::PB_SLOTS],
    #[allow(dead_code)]
    owner_vms: [Option<Arc<super::vm::VmState>>; sizes::PB_SLOTS],
    #[allow(dead_code)]
    owner_arenas: [Option<super::resources::PbOwnerArena>; sizes::PB_SLOTS],
    /// Device-lifetime firmware leases keeping each render UMA slot's `descriptor` + `submitted`
    /// records mapped for firmware's global `uma_mgr_table[RENDER_UMA_SLOT + k]` across
    /// closed-VM reclaim until a newer render retires on that exact slot.
    retained_uma: [Option<Arc<RenderUmaLease>>; sizes::PB_SLOTS],
    pending_vms: [Option<Arc<super::vm::VmState>>; sizes::PB_SLOTS],
    pending_arenas: [Option<super::resources::PbOwnerArena>; sizes::PB_SLOTS],
    pending_uma: [Option<Arc<RenderUmaLease>>; sizes::PB_SLOTS],
    /// Per allocator slot, a binding count, bumped by each fresh epoch (its InitBM rebinds the
    /// slot) and by a firmware recovery or sleep restart. It identifies a binding without
    /// holding the epoch.
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
    /// Per TTBAT slot, the epoch of the last render its VM retired, kept until a newer render
    /// of the same VM retires. The parameter manager writes back to the page list of the
    /// manager a context used last, through that context, when the context's next InitBM
    /// runs, also after another VM's epoch replaced `owners[k]` on the old slot. An entry
    /// holds its VM and so the TTBAT slot that indexes it.
    vm_last: [Option<Arc<PbEpoch>>; VM_LAST_ENTRIES],
    /// A closed VM's [`Self::vm_last`] entry was recorded since the last reclaim pass.
    closed_vm_last: bool,
}
/// Entries of [`Pools::vm_last`]: one per TTBAT user slot (ASIDs `1..=MAX_VMS`).
const VM_LAST_ENTRIES: usize = sizes::MAX_VMS as usize + 1;
impl Pools {
    #[allow(dead_code)]
    pub(crate) fn new(dev: &AsahiDrmDevice) -> Self {
        Self {
            dev: dev.as_ref().as_ref().into(),
            stats: PbStats::default(),
            owners: [const { None }; sizes::PB_SLOTS],
            retained_pb: [const { None }; sizes::PB_SLOTS],
            owner_vms: [const { None }; sizes::PB_SLOTS],
            owner_arenas: [const { None }; sizes::PB_SLOTS],
            retained_uma: [const { None }; sizes::PB_SLOTS],
            pending_vms: [const { None }; sizes::PB_SLOTS],
            pending_arenas: [const { None }; sizes::PB_SLOTS],
            pending_uma: [const { None }; sizes::PB_SLOTS],
            binds: [0; sizes::PB_SLOTS],
            keep: [false; sizes::PB_SLOTS],
            active: None,
            last_clean_retired: None,
            inflight: [0; sizes::PB_SLOTS],
            quarantined: [false; sizes::PB_SLOTS],
            quarantined_epochs: [const { None }; sizes::PB_SLOTS],
            vm_last: [const { None }; VM_LAST_ENTRIES],
            closed_vm_last: false,
        }
    }
    /// Allocates [`Pools`] directly on the heap so probe/backend bring-up does not stage the
    /// per-slot lease arrays on the kernel stack.
    #[allow(dead_code)]
    #[inline(never)]
    pub(crate) fn new_boxed(dev: &AsahiDrmDevice) -> Result<KBox<Self>> {
        Ok(KBox::init(
            init!(Self {
                dev: dev.as_ref().as_ref().into(),
                stats: PbStats::default(),
                owners: [const { None }; sizes::PB_SLOTS],
                retained_pb: [const { None }; sizes::PB_SLOTS],
                owner_vms: [const { None }; sizes::PB_SLOTS],
                owner_arenas: [const { None }; sizes::PB_SLOTS],
                retained_uma: [const { None }; sizes::PB_SLOTS],
                pending_vms: [const { None }; sizes::PB_SLOTS],
                pending_arenas: [const { None }; sizes::PB_SLOTS],
                pending_uma: [const { None }; sizes::PB_SLOTS],
                binds: [0; sizes::PB_SLOTS],
                keep: [false; sizes::PB_SLOTS],
                active: None,
                last_clean_retired: None,
                inflight: [0; sizes::PB_SLOTS],
                quarantined: [false; sizes::PB_SLOTS],
                quarantined_epochs: [const { None }; sizes::PB_SLOTS],
                vm_last: [const { None }; VM_LAST_ENTRIES],
                closed_vm_last: false,
            }),
            GFP_KERNEL,
        )?)
    }
    /// Bitmask of permanently quarantined device-wide PB/UMA allocator slots (`0..PB_SLOTS`).
    pub(crate) fn quarantined_mask(&self) -> u32 {
        let mut mask = 0u32;
        for k in 0..sizes::PB_SLOTS {
            if self.quarantined[k] {
                mask |= 1u32 << k;
            }
        }
        mask
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
    /// Whether at least one displaced physical PB allocator slot exists in `0..limit`.
    fn has_displaced_slot_within(&self, limit: usize) -> bool {
        let n = limit.min(sizes::PB_SLOTS);
        (0..n).any(|k| self.slot_displaced(k))
    }
    /// Whether at least one displaced physical PB allocator slot exists in `slot_mask`.
    fn has_displaced_slot_in_mask(&self, slot_mask: u32) -> bool {
        (0..sizes::PB_SLOTS).any(|k| (slot_mask & (1u32 << k)) != 0 && self.slot_displaced(k))
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
    /// Like [`Self::reusable`], but also requires the retired epoch's SCS mode to match
    /// `want_scs` so a base render and an SCS render never share an active UMA descriptor slot.
    pub(crate) fn reusable_mode(
        &self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        want_scs: bool,
    ) -> Option<Arc<PbEpoch>> {
        self.reusable(k, vm)
            .filter(|owner| owner.scs() == want_scs)
    }
    /// Like [`Self::reusable_mode`], but forces a fresh `InitBM` epoch when the VM's PB heap grew
    /// past the retired epoch's `heap_blocks` and a displaced slot in `slot_mask` is available
    /// to rotate onto safely.
    pub(crate) fn reusable_for_mask_mode(
        &self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        slot_mask: u32,
        min_blocks: u32,
        want_scs: bool,
    ) -> Option<Arc<PbEpoch>> {
        if k >= sizes::PB_SLOTS || (slot_mask & (1u32 << k)) == 0 {
            return None;
        }
        let owner = self.reusable_mode(k, vm, want_scs)?;
        if owner.heap_blocks() >= min_blocks || !self.has_displaced_slot_in_mask(slot_mask) {
            Some(owner)
        } else {
            None
        }
    }
    /// Like [`Self::reusable_mode`], but forces a fresh `InitBM` epoch when the VM's PB heap grew
    /// past the retired epoch's `heap_blocks` and a displaced slot within `0..limit` is available
    /// to rotate onto safely.
    #[allow(dead_code)]
    pub(crate) fn reusable_for_within_mode(
        &self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        limit: usize,
        min_blocks: u32,
        want_scs: bool,
    ) -> Option<Arc<PbEpoch>> {
        let owner = self.reusable_mode(k, vm, want_scs)?;
        if owner.heap_blocks() >= min_blocks || !self.has_displaced_slot_within(limit) {
            Some(owner)
        } else {
            None
        }
    }
    /// Like [`Self::reusable`], but forces a fresh `InitBM` epoch when the VM's PB heap grew
    /// past the retired epoch's `heap_blocks` and a displaced slot within `0..limit` is available
    /// to rotate onto safely.
    #[allow(dead_code)]
    pub(crate) fn reusable_for_within(
        &self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        limit: usize,
        min_blocks: u32,
    ) -> Option<Arc<PbEpoch>> {
        self.reusable_for_within_mode(k, vm, limit, min_blocks, false)
    }
    /// Selects a physical PB/UMA allocator slot in `slot_mask` for `vm` and `want_scs`,
    /// preferring `home` when claiming an unused warm slot (`0..WARM_PB_SLOTS`) so up to
    /// `WARM_PB_SLOTS` concurrent VMs bind disjoint warm slots regardless of arrival order.
    pub(crate) fn pick_slot_in_mask_blocks_mode(
        &self,
        vm: &Arc<super::vm::VmState>,
        behind: bool,
        slot_mask: u32,
        home: usize,
        min_blocks: u32,
        want_scs: bool,
    ) -> Option<usize> {
        let in_mask = |k: usize| k < sizes::PB_SLOTS && (slot_mask & (1u32 << k)) != 0;
        if let Some(a) = self.active {
            if in_mask(a)
                && self
                    .reusable_for_mask_mode(a, vm, slot_mask, min_blocks, want_scs)
                    .is_some()
            {
                return Some(a);
            }
        }
        if let Some(k) = (0..sizes::PB_SLOTS).find(|&k| {
            in_mask(k)
                && self
                    .reusable_for_mask_mode(k, vm, slot_mask, min_blocks, want_scs)
                    .is_some()
        }) {
            return Some(k);
        }
        if behind {
            return None;
        }
        // Cold (drained): claim the VM's unused home slot first, then any unused warm slot,
        // then a displaced warm slot (preferring home), and finally recovery spares.
        if home < sizes::WARM_PB_SLOTS
            && in_mask(home)
            && self.slot_displaced(home)
            && self.owners[home].is_none()
        {
            return Some(home);
        }
        if let Some(k) = (0..sizes::WARM_PB_SLOTS)
            .find(|&k| in_mask(k) && self.slot_displaced(k) && self.owners[k].is_none())
        {
            return Some(k);
        }
        if home < sizes::WARM_PB_SLOTS && in_mask(home) && self.slot_displaced(home) {
            return Some(home);
        }
        if let Some(k) =
            (0..sizes::WARM_PB_SLOTS).find(|&k| in_mask(k) && self.slot_displaced(k))
        {
            return Some(k);
        }
        (sizes::WARM_PB_SLOTS..sizes::PB_SLOTS)
            .find(|&k| in_mask(k) && self.slot_displaced(k) && self.owners[k].is_some())
            .or_else(|| {
                (sizes::WARM_PB_SLOTS..sizes::PB_SLOTS)
                    .find(|&k| in_mask(k) && self.slot_displaced(k))
            })
    }
    /// Whether `vm` has a warm reusable PB slot in `slot_mask` matching `want_scs` and
    /// covering `min_blocks` so it can pipeline without InitBM.
    pub(crate) fn can_pipeline_in_mask_blocks_mode(
        &self,
        vm: &Arc<super::vm::VmState>,
        slot_mask: u32,
        home: usize,
        min_blocks: u32,
        want_scs: bool,
    ) -> bool {
        self.pick_slot_in_mask_blocks_mode(vm, true, slot_mask, home, min_blocks, want_scs)
            .is_some()
    }
    /// Whether no allocator slot in `slot_mask` can accept a render for `vm` and `want_scs`
    /// once drained.
    pub(crate) fn exhausted_for_mask_mode(
        &self,
        vm: &Arc<super::vm::VmState>,
        slot_mask: u32,
        home: usize,
        want_scs: bool,
    ) -> bool {
        self.inflight.iter().all(|&c| c == 0)
            && self
                .pick_slot_in_mask_blocks_mode(vm, false, slot_mask, home, 0, want_scs)
                .is_none()
    }
    /// Selects a physical PB/UMA allocator slot within `0..limit` for `vm` and `want_scs`,
    /// requiring any reused epoch to match `want_scs` and cover at least `min_blocks` when a
    /// displaced slot is available for rotation.
    ///
    /// When `behind` is true (pipelining behind active flights), only a healthy warm reusable
    /// slot already bound to `vm` with matching SCS mode (preferring `self.active`, then any
    /// other slot in `0..limit`) is eligible; cold VMs, base<->SCS transitions, or VMs whose PB
    /// heap expanded return `None` so the engine drains once before rotating onto a fresh
    /// `InitBM` epoch.
    /// When `behind` is false (engine drained), a qualifying warm reusable slot for `vm` is
    /// preferred; otherwise an already-used displaced slot is chosen first (preserving the 2-slot
    /// working set) or the lowest fresh slot is claimed.
    #[allow(dead_code)]
    pub(crate) fn pick_slot_within_blocks_mode(
        &self,
        vm: &Arc<super::vm::VmState>,
        behind: bool,
        limit: usize,
        min_blocks: u32,
        want_scs: bool,
    ) -> Option<usize> {
        let n = limit.min(sizes::PB_SLOTS);
        if let Some(a) = self.active {
            if a < n
                && self
                    .reusable_for_within_mode(a, vm, n, min_blocks, want_scs)
                    .is_some()
            {
                return Some(a);
            }
        }
        if let Some(k) = (0..n).find(|&k| {
            self.reusable_for_within_mode(k, vm, n, min_blocks, want_scs)
                .is_some()
        }) {
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
    /// Selects a physical PB/UMA allocator slot within `0..limit` for `vm`, requiring any reused
    /// epoch to cover at least `min_blocks` when a displaced slot is available for rotation.
    #[allow(dead_code)]
    pub(crate) fn pick_slot_within_blocks(
        &self,
        vm: &Arc<super::vm::VmState>,
        behind: bool,
        limit: usize,
        min_blocks: u32,
    ) -> Option<usize> {
        self.pick_slot_within_blocks_mode(vm, behind, limit, min_blocks, false)
    }
    /// Selects a physical PB/UMA allocator slot within `0..limit` for `vm`.
    #[allow(dead_code)]
    pub(crate) fn pick_slot_within(
        &self,
        vm: &Arc<super::vm::VmState>,
        behind: bool,
        limit: usize,
    ) -> Option<usize> {
        self.pick_slot_within_blocks(vm, behind, limit, 0)
    }
    /// Selects a physical PB/UMA allocator slot across all `0..PB_SLOTS` for `vm`.
    #[allow(dead_code)]
    pub(crate) fn pick_slot(&self, vm: &Arc<super::vm::VmState>, behind: bool) -> Option<usize> {
        self.pick_slot_within(vm, behind, sizes::PB_SLOTS)
    }
    /// Whether `vm` has a warm reusable PB slot within `0..limit` matching `want_scs` and
    /// covering `min_blocks` so it can pipeline without InitBM.
    #[allow(dead_code)]
    pub(crate) fn can_pipeline_within_blocks_mode(
        &self,
        vm: &Arc<super::vm::VmState>,
        limit: usize,
        min_blocks: u32,
        want_scs: bool,
    ) -> bool {
        self.pick_slot_within_blocks_mode(vm, true, limit, min_blocks, want_scs)
            .is_some()
    }
    /// Whether `vm` has a warm reusable PB slot within `0..limit` covering `min_blocks` and can
    /// pipeline without InitBM.
    #[allow(dead_code)]
    pub(crate) fn can_pipeline_within_blocks(
        &self,
        vm: &Arc<super::vm::VmState>,
        limit: usize,
        min_blocks: u32,
    ) -> bool {
        self.can_pipeline_within_blocks_mode(vm, limit, min_blocks, false)
    }
    /// Whether `vm` has a warm reusable PB slot within `0..limit` and can pipeline without InitBM.
    #[allow(dead_code)]
    pub(crate) fn can_pipeline_within(&self, vm: &Arc<super::vm::VmState>, limit: usize) -> bool {
        self.can_pipeline_within_blocks(vm, limit, 0)
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
    /// Whether no allocator slot within `0..limit` can accept a render for `vm` and `want_scs`
    /// once drained.
    #[allow(dead_code)]
    pub(crate) fn exhausted_for_within_mode(
        &self,
        vm: &Arc<super::vm::VmState>,
        limit: usize,
        want_scs: bool,
    ) -> bool {
        self.inflight.iter().all(|&c| c == 0)
            && self
                .pick_slot_within_blocks_mode(vm, false, limit, 0, want_scs)
                .is_none()
    }
    /// Whether no allocator slot within `0..limit` can accept a render for `vm` once drained.
    ///
    /// When a VM prepared fewer PB slots than needed after a render hang (or when only one
    /// surviving slot remains and belongs to another VM), that job must cancel immediately
    /// instead of spinning on `EAGAIN` until the administrative publication deadline fails the GPU.
    #[allow(dead_code)]
    pub(crate) fn exhausted_for_within(&self, vm: &Arc<super::vm::VmState>, limit: usize) -> bool {
        self.exhausted_for_within_mode(vm, limit, false)
    }
    /// Whether no allocator slot can accept a render for `vm` once the engine is drained.
    #[allow(dead_code)]
    pub(crate) fn exhausted_for(&self, vm: &Arc<super::vm::VmState>) -> bool {
        self.exhausted_for_within(vm, sizes::PB_SLOTS)
    }
    /// Retains the candidate VM, owning render/recovery arena, and firmware UMA descriptor lease
    /// before publication visibility.
    #[allow(dead_code)]
    pub(crate) fn hold_alloc(
        &mut self,
        k: usize,
        vm: &Arc<super::vm::VmState>,
        arena: Option<super::resources::PbOwnerArena>,
        uma_lease: Option<Arc<RenderUmaLease>>,
    ) -> Result {
        if k >= sizes::PB_SLOTS || self.quarantined[k] {
            return Err(EINVAL);
        }
        self.pending_vms[k] = Some(vm.clone());
        self.pending_arenas[k] = arena;
        self.pending_uma[k] = uma_lease;
        Ok(())
    }
    /// Cancels unpublished candidate retention for allocator slot `k`.
    #[allow(dead_code)]
    pub(crate) fn cancel_alloc(&mut self, k: usize) {
        if k < sizes::PB_SLOTS {
            self.pending_vms[k] = None;
            self.pending_arenas[k] = None;
            self.pending_uma[k] = None;
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
        if self.owners[k].as_ref().is_some_and(|old| {
            !Arc::ptr_eq(&old._vm, &epoch._vm) && !old._vm.vm.is_closed() && self.is_vm_last(old)
        }) {
            self.stats.kept += 1;
        }
        if completed {
            self.note_vm_last(epoch);
        }
        self.owners[k] = Some(epoch.clone());
        self.retained_pb[k] = Some(epoch.records_lease());
        if let Some(vm) = self.pending_vms[k].take() {
            self.owner_vms[k] = Some(vm);
        }
        if let Some(arena) = self.pending_arenas[k].take() {
            self.owner_arenas[k] = Some(arena);
        }
        if let Some(uma) = self.pending_uma[k].take() {
            self.retained_uma[k] = Some(uma);
        }
        self.last_clean_retired = Some(k);
        self.retire_epoch(k, binding, completed);
        Ok(())
    }
    /// Whether `epoch` is the last retired epoch of its VM ([`Self::vm_last`]).
    fn is_vm_last(&self, epoch: &Arc<PbEpoch>) -> bool {
        self.vm_last
            .get(epoch._vm.slot as usize)
            .and_then(Option::as_ref)
            .is_some_and(|last| Arc::ptr_eq(last, epoch))
    }
    /// A render of `epoch` retired normally: its VM's context has run that render's InitBM (or
    /// reused the epoch), so the VM's earlier epoch is no longer written and may go. A closed
    /// VM keeps an entry only until [`Self::reclaim_closed`] finds nothing of it pending.
    fn note_vm_last(&mut self, epoch: &Arc<PbEpoch>) {
        let vm = &epoch._vm;
        let closed = vm.vm.is_closed();
        let Some(entry) = self.vm_last.get_mut(vm.slot as usize) else {
            return;
        };
        if entry.as_ref().is_some_and(|last| Arc::ptr_eq(last, epoch)) {
            return;
        }
        *entry = Some(epoch.clone());
        self.closed_vm_last |= closed;
    }
    /// Whether a closed VM's entry was recorded since the last call: [`Self::reclaim_closed`]
    /// must run again to release it.
    pub(crate) fn take_closed_vm_last(&mut self) -> bool {
        core::mem::take(&mut self.closed_vm_last)
    }
    /// Quarantines allocator slot `k` permanently after a passed abort, retaining all leases
    /// including any fresh epoch published on the aborted flight alongside `owners[k]`.
    #[allow(dead_code)]
    pub(crate) fn quarantine_flight(&mut self, k: usize, pb: Option<Arc<PbEpoch>>) {
        if k < sizes::PB_SLOTS {
            self.inflight[k] = self.inflight[k].saturating_sub(1);
            self.quarantined[k] = true;
            self.keep[k] = false;
            if let Some(uma) = &self.pending_uma[k] {
                uma.quarantine();
            }
            if let Some(uma) = &self.retained_uma[k] {
                uma.quarantine();
            }
            if let Some(records) = &self.retained_pb[k] {
                records.quarantine();
            }
            if let Some(owner) = &self.owners[k] {
                owner.quarantine();
            }
            if let Some(epoch) = pb {
                epoch.quarantine();
                if self.quarantined_epochs[k].is_none() {
                    self.quarantined_epochs[k] = Some(epoch);
                } else {
                    // Already retaining an aborted epoch on this slot; never unmap an exposed
                    // epoch or allocate under the device lock.
                    core::mem::forget(epoch);
                }
            }
        } else if let Some(epoch) = pb {
            epoch.quarantine();
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
    /// epoch was bound and no recovery or sleep restart happened since, the next render of the
    /// same VM may reuse
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
    /// Releases the displaced render arenas and PB heaps of closed VMs while preserving active,
    /// in-flight, undisplaced, and quarantined slots. Returns `true` if any non-quarantined
    /// slot still holds such a closed-VM lease awaiting retirement or displacement.
    ///
    /// The owner epoch itself stays until a newer epoch retires on its slot
    /// ([`Self::retire_flight`]): `pb_mgr_table[k]` keeps naming its manager, and the parameter
    /// manager can still write that manager's page list through the context of the epoch's VM.
    /// Also releases a closed VM's [`Self::vm_last`] entry once nothing of that VM is pending.
    pub(crate) fn reclaim_closed(&mut self) -> bool {
        let mut remaining = false;
        for k in 0..sizes::PB_SLOTS {
            if self.quarantined[k] {
                continue;
            }
            let pending = self.pending_vms[k].is_some()
                || self.pending_arenas[k].is_some()
                || self.pending_uma[k].is_some();
            // Only the owner epoch is left; the next epoch to retire on the slot releases it.
            if !pending && self.owner_vms[k].is_none() && self.owner_arenas[k].is_none() {
                continue;
            }
            let closed = self.owner_vms[k]
                .as_ref()
                .is_some_and(|vm| vm.vm.is_closed())
                || self.owners[k]
                    .as_ref()
                    .is_some_and(|epoch| epoch._vm.vm.is_closed())
                || self.pending_vms[k]
                    .as_ref()
                    .is_some_and(|vm| vm.vm.is_closed());
            if !closed {
                continue;
            }
            if self.slot_displaced(k) && !pending {
                self.owner_vms[k] = None;
                self.owner_arenas[k] = None;
                self.keep[k] = false;
                self.binds[k] = self.binds[k].wrapping_add(1);
            } else {
                remaining = true;
            }
        }
        // A closed VM accepts no new render. Once none it accepted waits for publication or
        // retirement, its context runs no further InitBM and its last epoch may go. A render
        // left on a quarantined slot keeps its VM there anyway.
        for i in 0..VM_LAST_ENTRIES {
            let Some(vm) = self.vm_last[i].as_ref().map(|last| &last._vm) else {
                continue;
            };
            if !vm.vm.is_closed() {
                continue;
            }
            let pending = (0..sizes::PB_SLOTS).any(|k| {
                !self.quarantined[k]
                    && self.pending_vms[k].as_ref().is_some_and(|p| Arc::ptr_eq(p, vm))
            });
            if pending || vm.renders_unpublished() {
                remaining = true;
            } else {
                self.vm_last[i] = None;
            }
        }
        remaining
    }
    #[allow(dead_code)]
    pub(crate) fn retire_binding(&mut self, k: usize, epoch: &Arc<PbEpoch>) -> Result {
        *self.owners.get_mut(k).ok_or(EINVAL)? = Some(epoch.clone());
        *self.retained_pb.get_mut(k).ok_or(EINVAL)? = Some(epoch.records_lease());
        Ok(())
    }
    pub(crate) fn write_stats(&self, f: &mut impl fmt::Write) -> fmt::Result {
        writeln!(
            f,
            "pb: M4 private allocators blocks {} requests {} bad {} reused {} fresh {} kept {}",
            buffer::MAX_BLOCKS,
            self.stats.requests,
            self.stats.bad,
            self.stats.reused,
            self.stats.fresh,
            self.stats.kept
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
    pages: Arc<FwObject>,
    pages_gpu: u64,
    blocks: Arc<FwObject>,
    ring: Arc<FwObject>,
    counter: Arc<FwObject>,
    pub(crate) misc: Arc<FwObject>,
    pub(crate) initbm: Arc<FwObject>,
    assigned: AtomicU32,
    heap_blocks: AtomicU32,
    prepared_heap_gva: AtomicU64,
    prepared_layout_id_a: AtomicU32,
    /// High-water block count previously written into the zero-allocated `pages`/`blocks` lists,
    /// so fresh `InitBM` seeding writes only the active prefix (plus any shrink tail).
    seeded_blocks: AtomicU32,
    scs: AtomicBool,
    exposed: AtomicBool,
    // Unmap the private `pages_gpu` client alias BEFORE releasing `records_lease` (so no VM
    // retains a client mapping when the record set returns to `PbRecordPool`) and BEFORE
    // releasing the VM's aliases and ASID.
    _objects: Objects,
    /// Device-lifetime firmware lease keeping `cached_records` (`managers[0..PB_SLOTS]`,
    /// `initbm`), `blocks` (`FwCoherent`), `pages` (`Shared`), and `nc_records` (`ring`,
    /// `counter`, `misc`, `Fw`) mapped across closed-VM reclaim until a newer `InitBM`
    /// overwrites `pb_mgr_table[k]`.
    records_lease: Arc<PbRecordLease>,
    _vm: Arc<super::vm::VmState>,
    // Return admission credit only after deferred unmap and binding release.
    _permit: super::budget::Permit,
}
impl PbEpoch {
    #[allow(dead_code)]
    pub(crate) fn new(
        factory: &super::object::Factory,
        vm: &Arc<super::vm::VmState>,
        permit: super::budget::Permit,
    ) -> Result<Arc<Self>> {
        Self::new_for(factory, vm, permit, None)
    }

    /// Allocates or recycles a PB epoch compatible with `target_heap_blocks` (`None` accepts
    /// only unbound record sets).
    pub(crate) fn new_for(
        factory: &super::object::Factory,
        vm: &Arc<super::vm::VmState>,
        permit: super::budget::Permit,
        target_heap_blocks: Option<u32>,
    ) -> Result<Arc<Self>> {
        let (records_lease, used) = factory
            .pb_record_pool()
            .take_for(factory, target_heap_blocks)?;
        if used {
            records_lease.pages.fill(0, 0, records_lease.pages.size)?;
            records_lease
                .nc_records
                .fill(0, 0, records_lease.nc_records.size)?;
            records_lease.initbm.fill(0, 0, records_lease.initbm.size)?;
            // Once a persistent record set has been exposed to firmware, its ASC-cached
            // `pb_mgr` slot images and read-only `blocks` table stay untouched by the host;
            // only never-exposed entries are cleared on reuse.
            if !records_lease.is_bound() {
                records_lease
                    .cached_records
                    .fill(0, 0, records_lease.cached_records.size)?;
                records_lease.blocks.fill(0, 0, records_lease.blocks.size)?;
            }
        }
        let mut o = factory.objects(&vm.vm);
        o.map_client_at(&records_lease.pages, records_lease.pages_gpu)?;
        let mut inner = PbEpochInner {
            pages: records_lease.pages.clone(),
            pages_gpu: records_lease.pages_gpu,
            blocks: records_lease.blocks.clone(),
            ring: records_lease.ring.clone(),
            counter: records_lease.counter.clone(),
            misc: records_lease.misc.clone(),
            initbm: records_lease.initbm.clone(),
            assigned: AtomicU32::new(u32::MAX),
            heap_blocks: AtomicU32::new(0),
            prepared_heap_gva: AtomicU64::new(0),
            prepared_layout_id_a: AtomicU32::new(0),
            seeded_blocks: AtomicU32::new(0),
            scs: AtomicBool::new(false),
            exposed: AtomicBool::new(false),
            _objects: o,
            records_lease,
            _vm: vm.clone(),
            _permit: permit,
        };
        inner._objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(inner)?),
            GFP_KERNEL,
        )?)
    }

    /// Device-lifetime lease retaining this epoch's PB manager, block-list, and control records.
    pub(crate) fn records_lease(&self) -> Arc<PbRecordLease> {
        self.records_lease.clone()
    }

    /// Permanent heap-block binding of this epoch's underlying record set (`0` if unbound).
    pub(crate) fn bound_blocks(&self) -> u32 {
        self.records_lease.bound_blocks()
    }

    /// Firmware VA of the assigned slot's ASC-cached `pb_mgr` record.
    pub(crate) fn manager_va(&self) -> Result<u64> {
        let k = self.assigned.load(Ordering::Acquire) as usize;
        self.records_lease.manager_va(k)
    }

    /// Records a published render flight using this PB epoch's firmware records.
    ///
    /// Leaves the uncached PB `counter` at its initial `1` rather than advancing it per flight:
    /// on 26B123, matching `pb_mgr->completed_passes == *counter_ptr` triggers hardware PB slot
    /// unload (`[SGX+0x1c860]`) on every flight, forcing a costly reload on the next render.
    /// Because exposed per-slot `pb_mgr` records in `cached_records` are never overwritten by the
    /// host after first exposure, epoch recycling never depends on firmware write-back.
    #[allow(dead_code)]
    pub(crate) fn publish_flight(&self) {
        self.records_lease.publish();
    }

    /// Records a normal full retirement of a render flight using this PB epoch's firmware records.
    #[allow(dead_code)]
    pub(crate) fn retire_flight(&self) {
        self.records_lease.retire();
    }

    /// Permanently prevents this PB epoch's firmware records from recycling.
    pub(crate) fn quarantine(&self) {
        self.records_lease.quarantine();
    }

    /// Seed only unpublished storage. A retry may choose another physical slot; after
    /// exposure neither the manager nor any of its firmware-mutated lists is reset.
    #[allow(dead_code)]
    pub(crate) fn prepare(&self, k: usize, heap: u64, heap_blocks: u32) -> Result {
        self.prepare_with_scs(k, heap, heap_blocks, false)
    }

    /// Like [`Self::prepare`], recording whether this epoch is bound to the VM's SCS-capable
    /// render allocators (`scs == true`) or its base allocators (`scs == false`).
    #[allow(dead_code)]
    pub(crate) fn prepare_with_scs(
        &self,
        k: usize,
        heap: u64,
        heap_blocks: u32,
        scs: bool,
    ) -> Result {
        self.prepare_with_scs_for(&buffer::pb_manager::V25G83, k, heap, heap_blocks, scs)
    }

    /// Like [`Self::prepare_with_scs`], encoding the PB manager with `layout` for the active
    /// firmware ABI.
    pub(crate) fn prepare_with_scs_for(
        &self,
        layout: &buffer::pb_manager::Layout,
        k: usize,
        heap: u64,
        heap_blocks: u32,
        scs: bool,
    ) -> Result {
        if k >= sizes::PB_SLOTS
            || self.exposed.load(Ordering::Acquire)
            || heap_blocks == 0
            || heap_blocks > buffer::MAX_BLOCKS
            || !self
                .records_lease
                .matches_binding(layout.id_a as u32, heap, heap_blocks)
        {
            return Err(EINVAL);
        }
        // Only the backed part of the heap is named; the rest of the zero-allocated lists stays
        // zero. On an unpublished retry with fewer blocks, clear the previously seeded tail.
        let prev_blocks = self.seeded_blocks.load(Ordering::Acquire);
        let write_blocks = heap_blocks.max(prev_blocks);
        self.seeded_blocks.store(write_blocks, Ordering::Release);
        let pages = heap_blocks * buffer::PAGES_PER_BLOCK;
        let write_pages = write_blocks * buffer::PAGES_PER_BLOCK;
        write_records(&self.pages, write_pages as usize, |i| {
            let i = i as u32;
            (if i < pages {
                buffer::page_record(heap, i)
            } else {
                0
            })
            .to_le_bytes()
        })?;
        // Firmware never mutates the block list; once this persistent record set has been
        // exposed for `(heap, heap_blocks)`, its `blocks` list already holds the exact table.
        if !self.records_lease.is_bound() {
            write_records(&self.blocks, write_blocks as usize, |i| {
                let b = i as u32;
                u64::from(if b < heap_blocks {
                    buffer::block_record(heap, b)
                } else {
                    0
                })
                .to_le_bytes()
            })?;
        }
        let posted = super::params::pb_posted_blocks(heap_blocks);
        self.ring.w32(0, posted)?;
        self.ring.w32(4, posted)?;
        self.counter.w64(0, 1)?;
        // Once slot `k`'s ASC-cached `pb_mgr` has been exposed to firmware, the host never
        // writes to it again: every static field (`parameter_id`, `pages_fw`, `pages_gpu`,
        // `blocks_fw`, `ring_fw`, `counter_fw`, `heap_blocks`) is invariant for this set and
        // slot, and firmware's `InitBM` resets the live cursors on the ASC-cached line itself.
        if !self.records_lease.slot_is_exposed(k) {
            let off = super::resources::PbRecordSet::manager_offset(k)?;
            self.records_lease.cached_records.write(
                off,
                &buffer::pb_manager_image_for(
                    layout,
                    k as u32,
                    self.pages.va,
                    self.pages_gpu,
                    self.blocks.va,
                    self.ring.va,
                    self.counter.va,
                    heap_blocks,
                ),
            )?;
        }
        self.seeded_blocks.store(heap_blocks, Ordering::Release);
        self.assigned.store(k as u32, Ordering::Release);
        self.heap_blocks.store(heap_blocks, Ordering::Release);
        self.prepared_heap_gva.store(heap, Ordering::Release);
        self.prepared_layout_id_a
            .store(layout.id_a as u32, Ordering::Release);
        self.scs.store(scs, Ordering::Release);
        Ok(())
    }
    pub(crate) fn heap_blocks(&self) -> u32 {
        self.heap_blocks.load(Ordering::Acquire)
    }
    pub(crate) fn scs(&self) -> bool {
        self.scs.load(Ordering::Acquire)
    }
    pub(crate) fn check_assignment(&self, k: usize) -> Result {
        if self.assigned.load(Ordering::Acquire) != k as u32 {
            return Err(EINVAL);
        }
        Ok(())
    }
    pub(crate) fn expose(&self) {
        let k = self.assigned.load(Ordering::Acquire) as usize;
        let blocks = self.heap_blocks.load(Ordering::Acquire);
        let heap = self.prepared_heap_gva.load(Ordering::Acquire);
        let layout_id_a = self.prepared_layout_id_a.load(Ordering::Acquire);
        self.records_lease
            .mark_slot_exposed(k, layout_id_a, heap, blocks);
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
