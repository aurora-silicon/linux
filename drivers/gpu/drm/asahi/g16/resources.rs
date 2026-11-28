// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright (C) 2026 Eryk Wieliczko

//! Lazy per-VM command backing. Allocation runs only in SUBMIT, before accepting a fence.
//! The completion path reads prepared references and never takes the construction mutex.
//! Normal-NC maps are reclaimed only after all physical-slot leases have been displaced.

use super::{
    command::Command,
    object::{Factory, FwObject, Objects},
    pools::{PbEpoch, PoolSet},
    slots::{ComputeSlot, PassCmds, PassSlot, Placement, UnitCmds},
    vm::VmState,
    Shared,
};
use crate::mmu;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use kernel::{
    new_condvar, new_mutex, new_spinlock_irq,
    prelude::*,
    sync::{Arc, CondVar, CondVarTimeoutResult, Mutex, SetOnce, SpinLockIrq},
};

/// One pass slot index's device-lifetime render command records and the allocator that maps
/// them. The allocator is never dropped: an ASC-cached M4 mapping has no qualified teardown
/// (`mmu::KernelMapping::drop` refuses it), so even a failed construction keeps its mappings.
struct PersistentPassCmds {
    objects: core::mem::ManuallyDrop<Objects>,
    cmds: Option<PassCmds>,
}

/// The firmware-cached render command records (work images, microsequences and barrier records)
/// of every pass slot index (`0..RENDER_SLOTS`), created on first use and kept until reboot. A
/// pass slot index is device-global and claimed by one flight at a time, so every VM's
/// [`PassSlot`] of an index shares its records, and closing a VM drops no firmware-cached
/// mapping.
#[pin_data]
pub(crate) struct RenderCmds {
    #[pin]
    sets: Mutex<[Option<KBox<PersistentPassCmds>>; super::hw::sizes::RENDER_SLOTS]>,
}

impl RenderCmds {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                sets <- new_mutex!(
                    [const { None }; super::hw::sizes::RENDER_SLOTS],
                    "g16::render_cmds"
                ),
            }),
            GFP_KERNEL,
        )
    }

    /// The records of pass slot index `k`, allocated from `factory` on first use.
    pub(crate) fn get(&self, factory: &Factory, k: usize) -> Result<PassCmds> {
        let mut sets = self.sets.lock();
        let entry = sets.get_mut(k).ok_or(EINVAL)?;
        if let Some(cmds) = entry.as_ref().and_then(|set| set.cmds.as_ref()) {
            return Ok(cmds.clone());
        }
        let mut set = KBox::new(
            PersistentPassCmds {
                objects: core::mem::ManuallyDrop::new(factory.persistent_objects()),
                cmds: None,
            },
            GFP_KERNEL,
        )?;
        // From here on a failure leaks the allocator with whatever it mapped.
        let cmds = PassCmds::new(&mut set.objects, k as u64)?;
        set.objects.finish_private()?;
        set.cmds = Some(cmds.clone());
        *entry = Some(set);
        Ok(cmds)
    }
}

/// One hardware compute unit's device-lifetime command records and the allocator that maps
/// them. The allocator is never dropped for the same reason as [`PersistentPassCmds`]; partial
/// allocations are retained in place so a retry finishes without leaking or duplicating pages.
struct PersistentUnitCmds {
    objects: core::mem::ManuallyDrop<Objects>,
    work: Option<Arc<FwObject>>,
    seq: Option<Arc<FwObject>>,
    barrier: Option<Arc<FwObject>>,
    finished: bool,
}

/// The firmware-cached compute command records (work image, microsequence, and barrier records)
/// of every hardware compute unit (`0..COMPUTE_UNITS`), created on first use and kept until
/// reboot. A hardware compute unit is device-global and claimed by one flight at a time, so every
/// VM's [`ComputeSlot`] bound to hardware unit `k` shares its records, and closing a VM drops no
/// firmware-cached mapping.
#[pin_data]
pub(crate) struct ComputeCmds {
    #[pin]
    sets: Mutex<[Option<KBox<PersistentUnitCmds>>; super::hw::sizes::COMPUTE_UNITS]>,
}

impl ComputeCmds {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                sets <- new_mutex!(
                    [const { None }; super::hw::sizes::COMPUTE_UNITS],
                    "g16::compute_cmds"
                ),
            }),
            GFP_KERNEL,
        )
    }

    /// The records of hardware compute unit `k`, allocated from `factory` on first use.
    pub(crate) fn get(&self, factory: &Factory, k: usize) -> Result<UnitCmds> {
        let mut sets = self.sets.lock();
        let entry = sets.get_mut(k).ok_or(EINVAL)?;
        if entry.is_none() {
            let slot = KBox::<PersistentUnitCmds>::new_uninit(GFP_KERNEL)?;
            *entry = Some(KBox::write(
                slot,
                PersistentUnitCmds {
                    objects: core::mem::ManuallyDrop::new(factory.persistent_objects()),
                    work: None,
                    seq: None,
                    barrier: None,
                    finished: false,
                },
            ));
        }
        let set: &mut PersistentUnitCmds = &mut **entry.as_mut().ok_or(EINVAL)?;
        if set.finished {
            if let (Some(work), Some(seq), Some(barrier)) = (&set.work, &set.seq, &set.barrier) {
                return Ok(UnitCmds {
                    work: work.clone(),
                    seq: seq.clone(),
                    barrier: barrier.clone(),
                });
            }
        }
        let cmds = UnitCmds::init_into(
            &mut set.objects,
            k as u64,
            &mut set.work,
            &mut set.seq,
            &mut set.barrier,
        )?;
        set.objects.finish_private()?;
        set.finished = true;
        Ok(cmds)
    }
}

/// Never-exposed PB epochs an open VM keeps on standby after bursts drain.
///
/// With PB epoch reuse most renders never expose a fresh epoch; keeping a
/// shared per-VM standby pool instead of attaching one epoch per queued job
/// avoids allocating and mapping fresh epochs whenever submission depth rises.
const SPARE_EPOCHS: usize = 32;

/// Backing for one physical compute unit (`k`) of a VM, deferred-dropped independently so an
/// undisplaced last-retired owner lease on one CDM pipe never pins the VM's other compute units.
pub(crate) struct ComputeArena(crate::cleanup::Deferred<ComputeInner>);
impl core::ops::Deref for ComputeArena {
    type Target = ComputeInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct ComputeInner {
    pub(crate) unit: ComputeSlot,
    _objects: Objects,
    // Mappings must disappear while their root still owns its ASID.
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
/// Compute units every VM gets with its first compute command; more are added
/// ([`Cache::prepare`]) once it submits compute back to back.
pub(crate) const BASE_COMPUTE_UNITS: usize = 2;

/// Back-to-back compute submissions (each within [`BURST_GAP_NS`] of the previous) after
/// which a VM gets its extension units; a single submission with [`BURST_COMMANDS`] compute
/// commands qualifies at once.
const BURST_SUBMITS: u32 = 3;
const BURST_GAP_NS: u64 = 2_000_000;
const BURST_COMMANDS: usize = 3;

impl ComputeArena {
    /// Freelist entries of the unit's UMA pool.
    pub(crate) fn seeds(&self) -> u32 {
        self.unit.seeds
    }
    /// Allocates one self-contained compute unit at local VA index `local` bound to device-wide
    /// hardware ledger slot `hw_slot` (`local` and `placement` fix its client addresses; `hw_slot`
    /// selects its shared device-lifetime command records).
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        local: usize,
        hw_slot: usize,
        placement: Placement,
    ) -> Result<Arc<Self>> {
        let cmds = factory.compute_cmds(hw_slot)?;
        let mut objects = factory.objects(vm);
        let unit = ComputeSlot::new_in(
            &mut objects,
            factory,
            local as u64,
            hw_slot as u32,
            placement,
            &cmds,
        )?;
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(ComputeInner {
                unit,
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
}

/// Contiguous range of independently reference-counted [`ComputeArena`] units owned by a VM.
///
/// Each unit `k` owns its own `Arc<ComputeArena>` so `Cache::close()` and
/// `Slots::reclaim_closed()` release all unused or displaced units immediately even while a
/// CDM pipe retains one undisplaced last-retired unit until displacement.
pub(crate) struct ComputeUnitSet {
    units: KVec<Arc<ComputeArena>>,
    /// Device-wide compute ledger indices bound to local SCS units `0..SCS_COMPUTE_UNITS`, or
    /// `None` for contiguous base/extension sets whose local index equals the ledger index.
    hw_slots: Option<[usize; super::hw::sizes::SCS_COMPUTE_UNITS]>,
}
impl ComputeUnitSet {
    /// Units `range` (each index and `placement` fix its client addresses; the index fixes its
    /// hardware UMA slot).
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        range: core::ops::Range<usize>,
        placement: Placement,
    ) -> Result<Arc<Self>> {
        let mut units = KVec::with_capacity(range.len(), GFP_KERNEL)?;
        for k in range {
            units.push(
                ComputeArena::new(factory, bind, vm, live, k, k, placement)?,
                GFP_KERNEL,
            )?;
        }
        Ok(Arc::new(
            Self {
                units,
                hw_slots: None,
            },
            GFP_KERNEL,
        )?)
    }
    /// SCS-capable compute units mapped at local VA indices `0..SCS_COMPUTE_UNITS` and bound to
    /// device-wide healthy compute ledger indices `hw_slots`.
    fn new_scs(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        hw_slots: [usize; super::hw::sizes::SCS_COMPUTE_UNITS],
        seeds: u32,
    ) -> Result<Arc<Self>> {
        let mut units = KVec::with_capacity(super::hw::sizes::SCS_COMPUTE_UNITS, GFP_KERNEL)?;
        for (local, &hw_slot) in hw_slots.iter().enumerate() {
            units.push(
                ComputeArena::new(
                    factory,
                    bind,
                    vm,
                    live,
                    local,
                    hw_slot,
                    Placement::Scs { seeds },
                )?,
                GFP_KERNEL,
            )?;
        }
        Ok(Arc::new(
            Self {
                units,
                hw_slots: Some(hw_slots),
            },
            GFP_KERNEL,
        )?)
    }
    fn len(&self) -> usize {
        self.units.len()
    }
    /// Freelist entries of the set's UMA pools (all its units share a placement).
    pub(crate) fn seeds(&self) -> u32 {
        self.units.first().map_or(0, |arena| arena.seeds())
    }
    fn unit(&self, idx: usize) -> Option<&ComputeSlot> {
        self.units.get(idx).map(|arena| &arena.unit)
    }
    fn arena(&self, idx: usize) -> Option<Arc<ComputeArena>> {
        self.units.get(idx).cloned()
    }
}

/// Render pass slots every VM gets with its first render command; extension slots
/// (`BASE_RENDER_SLOTS..hw::sizes::RENDER_SLOTS`) are grown lazily in bounded tiers once the VM
/// pipelines renders or submits multi-pass/burst frames.
pub(crate) const BASE_RENDER_SLOTS: usize = super::hw::sizes::BASE_RENDER_SLOTS;
/// Demand-grown extension render pass slot tiers (`BASE_RENDER_SLOTS..hw::sizes::RENDER_SLOTS`).
///
/// Each extension `PassSlot` backs 3.44 MiB (2 MiB TPC + 1 MiB tilemap + 448 KiB metadata and
/// scratch; its work images, microsequences and barriers are the device-lifetime [`RenderCmds`]
/// of its index). Growing in four bounded chunks keeps compositor and shallow desktop VMs on
/// Tier 0 (~20.6 MiB), keeps 20-31 pass workloads on Tier 2 without paying for 48 slots, and
/// keeps every tier's client-object batch well below `MAX_CLIENT` (512):
/// - Tier 0 (`2..8`, 6 slots, +20.63 MiB / 20.63 MiB cum): compositors, 2-6 pass frames.
/// - Tier 1 (`8..18`, 10 slots, +34.38 MiB / 55.00 MiB cum): moderate multi-pass bursts.
/// - Tier 2 (`18..32`, 14 slots, +48.13 MiB / 103.13 MiB cum): 20-31 pass pipelined frames.
/// - Tier 3 (`32..RENDER_SLOTS`, 16 slots, +55.00 MiB / 158.13 MiB cum at 48 slots): deep
///   multi-pass bursts (160 client mappings <= `MAX_CLIENT`).
pub(crate) const EXT_RENDER_TIERS: [(usize, usize); 4] = [
    (BASE_RENDER_SLOTS, 8),
    (8, 18),
    (18, 32),
    (32, super::hw::sizes::RENDER_SLOTS),
];
const _: () = assert!(EXT_RENDER_TIERS[0].0 == BASE_RENDER_SLOTS);
const _: () = assert!(EXT_RENDER_TIERS[0].1 == EXT_RENDER_TIERS[1].0);
const _: () = assert!(EXT_RENDER_TIERS[1].1 == EXT_RENDER_TIERS[2].0);
const _: () = assert!(EXT_RENDER_TIERS[2].1 == EXT_RENDER_TIERS[3].0);
const _: () = assert!(EXT_RENDER_TIERS[3].1 == super::hw::sizes::RENDER_SLOTS);
const BASE_PB_SLOTS: usize = super::hw::sizes::BASE_PB_SLOTS;
const EXT_PB_SLOTS: usize = super::hw::sizes::EXT_PB_SLOTS;
/// Demand-grown contiguous PB heap extensions within the 256 MiB
/// [`super::hw::client::PB_HEAP_SIZE`] window so steady 60/120 Hz single-pass clients stay on
/// the 64 MiB base heap while multi-pass and heavy 3D workloads expand to 128 MiB (Tier 0) and
/// 256 MiB (Tier 1) without TVB partial-render spills.
pub(crate) const PB_HEAP_EXT_TIERS: [(u32, u32); 2] =
    [(512, 1024), (1024, super::fw::buffer::MAX_BLOCKS)];
const _: () = assert!(PB_HEAP_EXT_TIERS[0].1 == PB_HEAP_EXT_TIERS[1].0);
const _: () = assert!(PB_HEAP_EXT_TIERS[1].1 == super::fw::buffer::MAX_BLOCKS);
const _: () = assert!(
    (PB_HEAP_EXT_TIERS[1].1 as u64) * super::fw::buffer::BLOCK_BYTES
        <= super::hw::client::PB_HEAP_SIZE
);
/// Single-submission render command count that expands Tier 0 extension slots immediately:
/// `BASE_RENDER_SLOTS` holds only one active render once displacement occupies a slot.
const RENDER_BURST_COMMANDS: usize = BASE_RENDER_SLOTS;

/// Device-wide hints read locklessly in `SUBMIT` to size per-VM extension backing.
#[derive(Copy, Clone)]
pub(crate) struct PrepareHints<'a> {
    /// A base compute unit (`0..BASE_COMPUTE_UNITS`) is quarantined: allocate extension units.
    pub(crate) expand_compute_ext: bool,
    /// Bitmask of permanently quarantined device-wide compute units (`0..COMPUTE_UNITS`).
    pub(crate) quarantined_compute_mask: u32,
    /// Bitmask of healthy pipe-0-exclusive compute units reserved for fresh SCS compute VMs.
    pub(crate) scs_compute_mask: u32,
    /// A base render pass slot (`0..BASE_RENDER_SLOTS`) is quarantined: allocate extension slots.
    pub(crate) expand_render_ext: bool,
    /// Bitmask of permanently quarantined device-wide PB/UMA allocator slots (`0..PB_SLOTS`).
    pub(crate) quarantined_pb_mask: u32,
    /// Round-robin cursor assigning a VM's `[home, alt]` base PB/UMA slots in `0..WARM_PB_SLOTS`.
    pub(crate) next_pb_slot: &'a AtomicU32,
    /// Live open VMs owning a `RenderArena`, used for multi-VM pass-slot headroom.
    pub(crate) active_render_vms: &'a Arc<AtomicU32>,
}

/// Selects `SCS_COMPUTE_UNITS` distinct hardware compute ledger indices for a VM's SCS compute
/// set, preferring healthy units already reserved for pipe 0 (`reserved_mask`) so fresh SCS
/// VMs never bind onto units carrying an undisplaced secondary-pipe lease, then falling back
/// to any non-quarantined units in `0..max_units`.
fn pick_scs_compute_slots(
    reserved_mask: u32,
    quarantined_mask: u32,
    max_units: usize,
) -> [usize; super::hw::sizes::SCS_COMPUTE_UNITS] {
    let limit = max_units
        .min(super::hw::sizes::COMPUTE_UNITS)
        .max(super::hw::sizes::SCS_COMPUTE_UNITS);
    let mut out = [0usize; super::hw::sizes::SCS_COMPUTE_UNITS];
    let mut found = 0usize;
    let preferred = reserved_mask & !quarantined_mask;
    for k in 0..limit {
        if (preferred & (1u32 << k)) != 0 {
            out[found] = k;
            found += 1;
            if found == out.len() {
                return out;
            }
        }
    }
    for k in 0..limit {
        if (quarantined_mask & (1u32 << k)) == 0 && !out[..found].contains(&k) {
            out[found] = k;
            found += 1;
            if found == out.len() {
                return out;
            }
        }
    }
    for k in 0..limit {
        if !out[..found].contains(&k) {
            out[found] = k;
            found += 1;
            if found == out.len() {
                return out;
            }
        }
    }
    out
}

/// Selects `[home, alt]` distinct physical PB/UMA allocator slots for a new `RenderArena`,
/// starting round-robin from `start % WARM_PB_SLOTS` and skipping permanently quarantined slots
/// in `quarantined_mask`. Scans `0..WARM_PB_SLOTS` first, then `WARM_PB_SLOTS..PB_SLOTS`, so
/// both base and `render_scs` VMs always start with two healthy PB slots after recovery.
fn pick_base_pb_slots(start: usize, quarantined_mask: u32) -> [usize; BASE_PB_SLOTS] {
    let warm = super::hw::sizes::WARM_PB_SLOTS;
    let total = super::hw::sizes::PB_SLOTS;
    let mut out = [0usize; BASE_PB_SLOTS];
    let mut found = 0usize;
    for i in 0..warm {
        let slot = (start + i) % warm;
        if (quarantined_mask & (1u32 << slot)) == 0 {
            out[found] = slot;
            found += 1;
            if found == out.len() {
                return out;
            }
        }
    }
    for slot in warm..total {
        if (quarantined_mask & (1u32 << slot)) == 0 {
            out[found] = slot;
            found += 1;
            if found == out.len() {
                return out;
            }
        }
    }
    for i in 0..total {
        let slot = (start + i) % total;
        if !out[..found].contains(&slot) {
            out[found] = slot;
            found += 1;
            if found == out.len() {
                return out;
            }
        }
    }
    out
}

/// Updates the consecutive-compute burst counter and reports whether this submission
/// qualifies to allocate extension compute units (`compute_ext`).
///
/// If a base compute unit (`0..BASE_COMPUTE_UNITS`) has been permanently quarantined, fresh
/// single-command VMs must expand immediately so they can claim a healthy extension unit and
/// displace the remaining retired base unit rather than hitting `compute_exhausted_within(2)`.
fn should_expand_compute_ext(
    last_compute_ns: &mut u64,
    burst: &mut u32,
    now: u64,
    computes: usize,
    base_quarantined: bool,
) -> bool {
    *burst = if *last_compute_ns != 0 && now.wrapping_sub(*last_compute_ns) < BURST_GAP_NS {
        burst.saturating_add(1)
    } else {
        0
    };
    *last_compute_ns = now;
    base_quarantined || *burst >= BURST_SUBMITS || computes >= BURST_COMMANDS
}

/// Updates the consecutive-render burst counter and returns the target render pass slot count
/// (`BASE_RENDER_SLOTS..=hw::sizes::RENDER_SLOTS`) this VM should have backed before admission.
///
/// Steady 60/120 Hz single-pass depth-1 clients finish each frame before the next arrives
/// (`active_passes == 0`, `render_count == 1`, inter-frame gap >= `BURST_GAP_NS`) and stay
/// on the two base pass slots (~48.9 MiB). Extension tiers grow on demand: Tier 0 (`2..8`,
/// +20.6 MiB) allocates when a base pass slot is quarantined, a VM overlaps an in-flight render,
/// submits a multi-pass command stream (`render_count >= BASE_RENDER_SLOTS`), or sustains a
/// sub-2 ms back-to-back burst; higher tiers allocate only when live passes plus this submission
/// and one displacement slot exceed the currently backed capacity.
fn target_render_slots(
    last_render_ns: &mut u64,
    render_burst: &mut u32,
    now: u64,
    render_count: usize,
    active_passes: u32,
    base_quarantined: bool,
    current_slots: usize,
) -> usize {
    *render_burst = if *last_render_ns != 0 && now.wrapping_sub(*last_render_ns) < BURST_GAP_NS {
        render_burst.saturating_add(1)
    } else {
        0
    };
    *last_render_ns = now;
    let live_need = (active_passes as usize)
        .saturating_add(render_count)
        .saturating_add(1);
    let want = if current_slots <= BASE_RENDER_SLOTS {
        if base_quarantined
            || active_passes != 0
            || *render_burst >= BURST_SUBMITS
            || render_count >= RENDER_BURST_COMMANDS
        {
            live_need.max(BASE_RENDER_SLOTS + 1)
        } else {
            BASE_RENDER_SLOTS
        }
    } else {
        live_need
    };
    let mut target = current_slots.max(BASE_RENDER_SLOTS);
    for &(start, end) in &EXT_RENDER_TIERS {
        if target >= want {
            break;
        }
        if target < end && want > start {
            target = end;
        }
    }
    target.min(super::hw::sizes::RENDER_SLOTS)
}

/// Like [`target_render_slots`], returning `(effective_target, single_vm_target)`.
///
/// When `active_render_vms` reaches Tier 0's pass-slot ceiling (`EXT_RENDER_TIERS[0].1 == 8`)
/// and this VM is already bursting (`single_vm_target > BASE_RENDER_SLOTS`), `effective_target`
/// expands up to Tier 1 (`EXT_RENDER_TIERS[1].1 == 18`) so up to 16 concurrent render VMs each
/// keep a warm pass slot without cross-VM full-scrub thrashing. Steady 60/120 Hz single-pass
/// clients stay at `BASE_RENDER_SLOTS`, and `single_vm_target` keeps `PB_HEAP_EXT_TIERS` gated
/// on actual single-VM pass demand so multi-VM light renders do not expand to 256 MiB PB heaps.
fn target_render_slots_for(
    last_render_ns: &mut u64,
    render_burst: &mut u32,
    now: u64,
    render_count: usize,
    active_passes: u32,
    base_quarantined: bool,
    current_slots: usize,
    single_vm_slots: usize,
    active_render_vms: u32,
) -> (usize, usize) {
    let single = target_render_slots(
        last_render_ns,
        render_burst,
        now,
        render_count,
        active_passes,
        base_quarantined,
        single_vm_slots.max(BASE_RENDER_SLOTS),
    );
    let mut target = current_slots.max(single);
    if single > BASE_RENDER_SLOTS && (active_render_vms as usize) >= EXT_RENDER_TIERS[0].1 {
        let multi_need = (active_render_vms as usize)
            .saturating_add(1)
            .min(EXT_RENDER_TIERS[1].1);
        for &(start, end) in &EXT_RENDER_TIERS[..2] {
            if target >= multi_need {
                break;
            }
            if target < end && multi_need > start {
                target = end;
            }
        }
    }
    (target.min(super::hw::sizes::RENDER_SLOTS), single)
}

pub(crate) struct RenderArena(crate::cleanup::Deferred<RenderInner>);
impl core::ops::Deref for RenderArena {
    type Target = RenderInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
/// The SCS-capable render allocators of a render arena and the mappings they own.
pub(crate) struct ScsPools {
    pub(crate) pools: PoolSet,
    _objects: Objects,
}

pub(crate) struct RenderInner {
    pub(crate) passes: KVec<PassSlot>,
    pub(crate) pools: PoolSet,
    /// Device-wide physical PB/UMA allocator slots `[home, alt]` bound to `pools.unit(0..2)`.
    pub(crate) base_pb_slots: [usize; BASE_PB_SLOTS],
    /// [`PoolSet::scs`], with the VM's first render that has scratch. Held with the arena, so
    /// a pass slot's owner lease keeps it too. Dropped before the arena's own mappings.
    pub(crate) scs: SetOnce<ScsPools>,
    pub(crate) tpc_dirty: [AtomicU64; BASE_RENDER_SLOTS],
    pub(crate) tilemap_dirty: [AtomicU64; BASE_RENDER_SLOTS],
    pub(crate) tilemap_last: [AtomicU64; BASE_RENDER_SLOTS],
    pub(crate) tilemap_clean: [AtomicU64; BASE_RENDER_SLOTS],
    pub(crate) heap_dirty: [AtomicU64; BASE_RENDER_SLOTS],
    _objects: Objects,
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
/// Per-pass dirty and pre-cleaned WC byte spans for one render pass slot.
#[derive(Copy, Clone)]
pub(crate) struct PassDirty<'a> {
    pub(crate) tpc: &'a AtomicU64,
    pub(crate) tilemap: &'a AtomicU64,
    pub(crate) tilemap_last: &'a AtomicU64,
    pub(crate) tilemap_clean: &'a AtomicU64,
    pub(crate) heap: &'a AtomicU64,
}
impl RenderArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        base_pb_slots: [usize; BASE_PB_SLOTS],
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let mut passes = KVec::with_capacity(BASE_RENDER_SLOTS, GFP_KERNEL)?;
        for k in 0..BASE_RENDER_SLOTS {
            let cmds = factory.pass_cmds(k)?;
            passes.push(PassSlot::new(&mut objects, k as u64, 1, &cmds)?, GFP_KERNEL)?;
        }
        let pools = PoolSet::new_with_slots(&mut objects, factory, base_pb_slots)?;
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(RenderInner {
                passes,
                pools,
                base_pb_slots,
                scs: SetOnce::new(),
                tpc_dirty: [const { AtomicU64::new(0) }; BASE_RENDER_SLOTS],
                tilemap_dirty: [const { AtomicU64::new(0) }; BASE_RENDER_SLOTS],
                tilemap_last: [const { AtomicU64::new(0) }; BASE_RENDER_SLOTS],
                tilemap_clean: [const { AtomicU64::new(0) }; BASE_RENDER_SLOTS],
                heap_dirty: [const { AtomicU64::new(0) }; BASE_RENDER_SLOTS],
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
    /// Creates the SCS-capable render allocators with pools of `seeds.pool` entries unless
    /// they exist. Existing ones must hold `seeds.needed`: once the hardware may hold them they
    /// cannot grow (E2BIG). Called under the VM cache's construction mutex, before any job uses
    /// them.
    fn ensure_scs(&self, factory: &Factory, vm: &mmu::Vm, seeds: super::rt::ScsSeeds) -> Result {
        if let Some(scs) = self.scs.as_ref() {
            return if scs.pools.seeds() >= seeds.needed {
                Ok(())
            } else {
                Err(E2BIG)
            };
        }
        let mut objects = factory.objects(vm);
        let pools = self.pools.scs(&mut objects, factory, seeds.pool)?;
        objects.finish_private()?;
        // The construction mutex excludes a second populate.
        if !self.scs.populate(ScsPools {
            pools,
            _objects: objects,
        }) {
            return Err(EBUSY);
        }
        Ok(())
    }
    pub(crate) fn set_dirty(&self, k: usize, tpc: u64, tilemap: u64) -> Result {
        let pass = self.passes.get(k).ok_or(EINVAL)?;
        if tpc > pass.tpc.size as u64 || tilemap > pass.tilemap.size as u64 {
            return Err(E2BIG);
        }
        self.tpc_dirty[k].fetch_max(tpc, Ordering::Relaxed);
        self.tilemap_dirty[k].fetch_max(tilemap, Ordering::Relaxed);
        self.tilemap_last[k].store(tilemap, Ordering::Relaxed);
        self.tilemap_clean[k].store(0, Ordering::Relaxed);
        self.heap_dirty[k].fetch_max(0x200, Ordering::Relaxed);
        Ok(())
    }
}

/// One demand-grown tier of extension render pass slots (`slots`), allocated lazily so idle,
/// single-pass, and shallow multi-pass VMs back only the tiers their concurrency requires.
pub(crate) struct RenderExtArena(crate::cleanup::Deferred<RenderExtInner>);
impl core::ops::Deref for RenderExtArena {
    type Target = RenderExtInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct RenderExtInner {
    pub(crate) start: usize,
    pub(crate) passes: KVec<PassSlot>,
    pub(crate) tpc_dirty: KVec<AtomicU64>,
    pub(crate) tilemap_dirty: KVec<AtomicU64>,
    pub(crate) tilemap_last: KVec<AtomicU64>,
    pub(crate) tilemap_clean: KVec<AtomicU64>,
    pub(crate) heap_dirty: KVec<AtomicU64>,
    _objects: Objects,
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
impl RenderExtArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        slots: core::ops::Range<usize>,
    ) -> Result<Arc<Self>> {
        let count = slots.len();
        let mut objects = factory.objects(vm);
        let mut passes = KVec::with_capacity(count, GFP_KERNEL)?;
        let mut tpc_dirty = KVec::with_capacity(count, GFP_KERNEL)?;
        let mut tilemap_dirty = KVec::with_capacity(count, GFP_KERNEL)?;
        let mut tilemap_last = KVec::with_capacity(count, GFP_KERNEL)?;
        let mut tilemap_clean = KVec::with_capacity(count, GFP_KERNEL)?;
        let mut heap_dirty = KVec::with_capacity(count, GFP_KERNEL)?;
        for k in slots.clone() {
            let cmds = factory.pass_cmds(k)?;
            passes.push(PassSlot::new(&mut objects, k as u64, 1, &cmds)?, GFP_KERNEL)?;
            tpc_dirty.push(AtomicU64::new(0), GFP_KERNEL)?;
            tilemap_dirty.push(AtomicU64::new(0), GFP_KERNEL)?;
            tilemap_last.push(AtomicU64::new(0), GFP_KERNEL)?;
            tilemap_clean.push(AtomicU64::new(0), GFP_KERNEL)?;
            heap_dirty.push(AtomicU64::new(0), GFP_KERNEL)?;
        }
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(RenderExtInner {
                start: slots.start,
                passes,
                tpc_dirty,
                tilemap_dirty,
                tilemap_last,
                tilemap_clean,
                heap_dirty,
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
}

/// One lazily allocated recovery PB/UMA allocator slot (`WARM_PB_SLOTS..PB_SLOTS`),
/// mapped at per-VM local VA index `BASE_PB_SLOTS..MAX_VM_PB_SLOTS` and sharing the VM's base PB
/// heap so non-fault VMs pay zero extra allocator memory.
pub(crate) struct PoolExtArena(crate::cleanup::Deferred<PoolExtInner>);
impl core::ops::Deref for PoolExtArena {
    type Target = PoolExtInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct PoolExtInner {
    pub(crate) allocator: super::pools::Allocator,
    _objects: Objects,
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
impl PoolExtArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        local_idx: usize,
        hw_slot: usize,
        heap: &Arc<super::object::FwObject>,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let allocator = super::pools::Allocator::new_at(
            &mut objects,
            factory,
            local_idx as u64,
            hw_slot as u32,
            heap,
            Placement::Base,
        )?;
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(PoolExtInner {
                allocator,
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
}

/// One demand-grown contiguous PB heap chunk extending the VM's base [`PoolSet`] heap within the
/// 256 MiB `PB_HEAP_SIZE` GPU VA window without unmapping any in-flight lower range.
pub(crate) struct PbHeapExtArena(crate::cleanup::Deferred<PbHeapExtInner>);
impl core::ops::Deref for PbHeapExtArena {
    type Target = PbHeapExtInner;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
pub(crate) struct PbHeapExtInner {
    pub(crate) blocks: u32,
    _heap: Arc<super::object::FwObject>,
    _objects: Objects,
    _bind: mmu::VmBind,
    _live: Arc<super::vm::LiveToken>,
}
impl PbHeapExtArena {
    fn new(
        factory: &Factory,
        bind: &mmu::VmBind,
        vm: &mmu::Vm,
        live: &Arc<super::vm::LiveToken>,
        start_block: u32,
        blocks: u32,
    ) -> Result<Arc<Self>> {
        let mut objects = factory.objects(vm);
        let heap = super::pools::alloc_pb_heap_chunk(&mut objects, start_block, blocks)?;
        objects.finish_private()?;
        Ok(Arc::new(
            Self(crate::cleanup::Deferred::new(PbHeapExtInner {
                blocks,
                _heap: heap,
                _objects: objects,
                _bind: bind.clone(),
                _live: live.clone(),
            })?),
            GFP_KERNEL,
        )?)
    }
}

/// Keeps the arenas owning a retained PB/UMA allocator slot and its backed PB heap tiers alive
/// through displacement. The wrapped `Arc`s are held solely for their `Drop` lifetime and never
/// read back.
#[derive(Clone)]
#[allow(dead_code)]
pub(crate) enum PbOwnerArena {
    Base(
        Arc<RenderArena>,
        [Option<Arc<PbHeapExtArena>>; PB_HEAP_EXT_TIERS.len()],
    ),
    Ext(
        Arc<RenderArena>,
        Arc<PoolExtArena>,
        [Option<Arc<PbHeapExtArena>>; PB_HEAP_EXT_TIERS.len()],
    ),
}

/// Keeps the arena owning a retained pass slot alive through displacement.
#[derive(Clone)]
pub(crate) enum PassOwnerArena {
    Base(Arc<RenderArena>),
    Ext(Arc<RenderExtArena>),
}

impl PassOwnerArena {
    /// Returns the pass slot and its dirty/clean WC span trackers for global slot `k`.
    pub(crate) fn pass_and_dirty(&self, k: usize) -> Option<(&PassSlot, PassDirty<'_>)> {
        match self {
            Self::Base(arena) => Some((
                arena.passes.get(k)?,
                PassDirty {
                    tpc: arena.tpc_dirty.get(k)?,
                    tilemap: arena.tilemap_dirty.get(k)?,
                    tilemap_last: arena.tilemap_last.get(k)?,
                    tilemap_clean: arena.tilemap_clean.get(k)?,
                    heap: arena.heap_dirty.get(k)?,
                },
            )),
            Self::Ext(arena) => {
                let idx = k.checked_sub(arena.start)?;
                Some((
                    arena.passes.get(idx)?,
                    PassDirty {
                        tpc: arena.tpc_dirty.get(idx)?,
                        tilemap: arena.tilemap_dirty.get(idx)?,
                        tilemap_last: arena.tilemap_last.get(idx)?,
                        tilemap_clean: arena.tilemap_clean.get(idx)?,
                        heap: arena.heap_dirty.get(idx)?,
                    },
                ))
            }
        }
    }
}

#[derive(Default)]
struct Ready {
    compute: Option<Arc<ComputeUnitSet>>,
    /// Units from [`BASE_COMPUTE_UNITS`] on, for a VM that keeps compute queued.
    compute_ext: Option<Arc<ComputeUnitSet>>,
    /// SCS-capable units, for jobs with compute scratch.
    compute_scs: Option<Arc<ComputeUnitSet>>,
    render: Option<Arc<RenderArena>>,
    /// Demand-grown extension render pass tiers (`EXT_RENDER_TIERS`).
    render_ext: [Option<Arc<RenderExtArena>>; EXT_RENDER_TIERS.len()],
    /// Demand-grown contiguous PB heap extension tiers (`PB_HEAP_EXT_TIERS`).
    pb_heap_ext: [Option<Arc<PbHeapExtArena>>; PB_HEAP_EXT_TIERS.len()],
    /// Recovery PB/UMA allocator slots (`WARM_PB_SLOTS..PB_SLOTS`), allocated lazily after hangs.
    pb_ext: [Option<Arc<PoolExtArena>>; EXT_PB_SLOTS],
    /// Single-VM render pass slot target (excluding multi-VM Tier 1 expansion), gating
    /// `PB_HEAP_EXT_TIERS` so concurrent light-render VMs do not allocate 256 MiB PB heaps.
    single_vm_render_slots: usize,
    /// Device-wide count of open rendering VMs; decremented once on `Cache::close()` or drop.
    render_vm_counter: Option<Arc<AtomicU32>>,
    /// Last compute submission and the back-to-back run it continues.
    last_compute_ns: u64,
    burst: u32,
    /// Last render submission and the back-to-back run it continues.
    last_render_ns: u64,
    render_burst: u32,
}
impl Drop for Ready {
    fn drop(&mut self) {
        if let Some(counter) = self.render_vm_counter.take() {
            counter.fetch_sub(1, Ordering::AcqRel);
        }
    }
}
struct Standby {
    /// Never-exposed epochs kept on standby for this VM's queued renders.
    /// Invariant: while open or while `reserved > 0`, `epochs.len() >= reserved as usize`.
    epochs: KVec<Arc<PbEpoch>>,
    /// Accepted, not-yet-published render commands of this VM.
    reserved: u32,
    /// `SUBMIT` callers currently sleeping on `standby_available`.
    waiters: u32,
}

#[pin_data]
pub(crate) struct Cache {
    #[pin]
    ready: Mutex<Ready>,
    /// Shared per-VM standby pool of never-exposed PB epochs. Each holds the VM, which
    /// holds this cache: [`Self::close`] drains surplus epochs immediately and the remaining
    /// reserved epochs as accepted renders finish.
    #[pin]
    standby: SpinLockIrq<Standby>,
    /// Wakes `SUBMIT` callers waiting for standby reservation headroom.
    #[pin]
    standby_available: CondVar,
    /// The VM's client closed: no arena or surplus spare is kept here any more.
    closed: AtomicBool,
    /// Render passes across prepared jobs on this VM that have not yet retired and dropped.
    active_render_passes: AtomicU32,
    pub(crate) factory: Factory,
    bind: mmu::VmBind,
    live: Arc<super::vm::LiveToken>,
    budget: Arc<super::budget::Budget>,
    shared: Arc<Shared>,
}
impl Cache {
    pub(crate) fn new(
        factory: Factory,
        bind: mmu::VmBind,
        live: Arc<super::vm::LiveToken>,
        shared: Arc<Shared>,
    ) -> Result<Arc<Self>> {
        let budget = super::budget::Budget::new()?;
        let epochs = KVec::with_capacity(super::budget::LIMIT as usize, GFP_KERNEL)?;
        Arc::pin_init(
            pin_init!(Self { ready <- new_mutex!(Ready::default(), "g16::resources"),
            standby <- new_spinlock_irq!(
                Standby {
                    epochs,
                    reserved: 0,
                    waiters: 0,
                },
                "g16::spare_epochs"
            ),
            standby_available <- new_condvar!(),
            closed: AtomicBool::new(false),
            active_render_passes: AtomicU32::new(0),
            factory, bind, live, budget, shared }),
            GFP_KERNEL,
        )
    }

    /// Pops one never-exposed standby epoch for a fresh InitBM publication.
    fn take_standby(&self) -> Result<Arc<PbEpoch>> {
        let mut standby = self.standby.lock();
        while let Some(epoch) = standby.epochs.pop() {
            if epoch.unexposed() {
                return Ok(epoch);
            }
        }
        Err(EINVAL)
    }

    /// Returns a never-exposed epoch whose publication was deferred or failed before visibility.
    fn return_standby(&self, epoch: Arc<PbEpoch>) {
        if !epoch.unexposed() {
            return;
        }
        let overflow = {
            let mut standby = self.standby.lock();
            let keep = if self.closed.load(Ordering::Acquire) {
                standby.reserved as usize
            } else {
                super::budget::LIMIT as usize
            };
            if standby.epochs.len() < keep {
                standby.epochs.push_within_capacity(epoch).err().map(|e| e.0)
            } else {
                Some(epoch)
            }
        };
        drop(overflow);
    }

    /// Releases `count` reserved render credits and trims any surplus standby epochs outside
    /// the spinlock (each `PbEpoch` defers its own unmap, so dropping here never sleeps).
    fn release_reserved(&self, count: u32) {
        if count == 0 {
            return;
        }
        let (mut wake, reclaim, excess) = {
            let mut standby = self.standby.lock();
            standby.reserved = standby.reserved.saturating_sub(count);
            let wake = standby.waiters != 0;
            let closed = self.closed.load(Ordering::Acquire);
            let reclaim = closed && standby.reserved == 0;
            let keep = if closed {
                standby.reserved as usize
            } else {
                (standby.reserved as usize).max(SPARE_EPOCHS)
            };
            let excess = if standby.epochs.len() > keep {
                standby.epochs.pop()
            } else {
                None
            };
            (wake, reclaim, excess)
        };
        if let Some(excess) = excess {
            drop(excess);
            // Subsequent iterations only trim remaining surplus; count was already subtracted.
            loop {
                let more = {
                    let mut standby = self.standby.lock();
                    wake |= standby.waiters != 0;
                    let keep = if self.closed.load(Ordering::Acquire) {
                        standby.reserved as usize
                    } else {
                        (standby.reserved as usize).max(SPARE_EPOCHS)
                    };
                    if standby.epochs.len() > keep {
                        standby.epochs.pop()
                    } else {
                        None
                    }
                };
                if more.is_none() {
                    break;
                }
                drop(more);
            }
        }
        if wake {
            self.standby_available.notify_all();
        }
        if reclaim {
            self.shared.wake_closed_vm_reclaim();
        }
    }

    fn release_render_passes(&self, count: u32) {
        let previous = self.active_render_passes.fetch_sub(count, Ordering::AcqRel);
        debug_assert!(previous >= count);
    }

    /// Whether accepted renders still hold standby reservations, that is, are not yet
    /// published ([`Prepared::commit_render`]) or dropped.
    pub(crate) fn renders_unpublished(&self) -> bool {
        self.standby.lock().reserved != 0
    }

    /// The VM's client closed. Lets go of the arenas and surplus standby epochs: accepted
    /// renders keep only the exact standby count they reserved until they finish, and hardware
    /// slots hold their own last-owner leases.
    pub(crate) fn close(&self) {
        self.closed.store(true, Ordering::Release);
        loop {
            let excess = {
                let mut standby = self.standby.lock();
                let keep = standby.reserved as usize;
                if standby.epochs.len() > keep {
                    standby.epochs.pop()
                } else {
                    None
                }
            };
            if excess.is_none() {
                break;
            }
            drop(excess);
        }
        self.standby_available.notify_all();
        let ready = {
            let mut ready = self.ready.lock();
            if let Some(counter) = ready.render_vm_counter.take() {
                counter.fetch_sub(1, Ordering::AcqRel);
            }
            (
                ready.compute.take(),
                ready.compute_ext.take(),
                ready.compute_scs.take(),
                ready.render.take(),
                core::mem::take(&mut ready.render_ext),
                core::mem::take(&mut ready.pb_heap_ext),
                core::mem::take(&mut ready.pb_ext),
            )
        };
        drop(ready);
    }

    pub(crate) fn prepare(
        self: &Arc<Self>,
        vm: &Arc<VmState>,
        commands: &[Command],
        hints: PrepareHints<'_>,
    ) -> Result<Prepared> {
        let computes = commands
            .iter()
            .filter(|c| matches!(c, Command::Compute(_)))
            .count();
        let compute = computes != 0;
        let render_count: u32 = commands
            .iter()
            .filter(|c| matches!(c, Command::Render(_)))
            .count()
            .try_into()?;
        let render = render_count != 0;
        // Scratch is sized on top of a pool's base pages: such a job runs on SCS-capable
        // storage, for all its commands of that engine, whose pools hold the largest of them.
        let scs = |engine: fn(&Command) -> bool| {
            commands
                .iter()
                .filter(|&c| engine(c))
                .map(Command::scs)
                .fold(super::rt::ScsSeeds::NONE, |a, b| super::rt::ScsSeeds {
                    needed: a.needed.max(b.needed),
                    pool: a.pool.max(b.pool).max(b.needed),
                })
        };
        let compute_seeds = scs(|c| matches!(c, Command::Compute(_)));
        let render_seeds = scs(|c| matches!(c, Command::Render(_)));
        let compute_scs = compute_seeds.needed != 0;
        let render_scs = render_seeds.needed != 0;
        let (compute, compute_ext, render_arena, render_ext, pb_heap_ext, pb_ext) = {
            // Never held by publication, completion, recovery, fence signalling or
            // standby waits, so Cache::close() and same-VM compute SUBMITs never block.
            let mut ready = self.ready.lock();
            if vm.status.get() != 0 || vm.vm.is_closed() || self.closed.load(Ordering::Acquire) {
                return Err(EIO);
            }
            if compute && !compute_scs && ready.compute.is_none() {
                ready.compute = Some(ComputeUnitSet::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                    0..BASE_COMPUTE_UNITS,
                    Placement::Base,
                )?);
            }
            if compute_scs {
                match ready.compute_scs.as_ref().map(|set| set.seeds()) {
                    None => {
                        let hw_slots = pick_scs_compute_slots(
                            hints.scs_compute_mask,
                            hints.quarantined_compute_mask,
                            super::params::compute_units(),
                        );
                        ready.compute_scs = Some(ComputeUnitSet::new_scs(
                            &self.factory,
                            &self.bind,
                            &vm.vm,
                            &self.live,
                            hw_slots,
                            compute_seeds.pool,
                        )?);
                    }
                    // The units cannot grow once the hardware may hold them.
                    Some(seeds) if seeds < compute_seeds.needed => return Err(E2BIG),
                    Some(_) => {}
                }
            }
            if compute && !compute_scs && ready.compute_ext.is_none() {
                let units = super::params::compute_units();
                if units > BASE_COMPUTE_UNITS {
                    // SAFETY: ktime_get has no preconditions.
                    let now = unsafe { kernel::bindings::ktime_get() } as u64;
                    let state = &mut *ready;
                    if should_expand_compute_ext(
                        &mut state.last_compute_ns,
                        &mut state.burst,
                        now,
                        computes,
                        hints.expand_compute_ext,
                    ) {
                        // Best effort: without them the VM keeps the base compute units.
                        state.compute_ext = ComputeUnitSet::new(
                            &self.factory,
                            &self.bind,
                            &vm.vm,
                            &self.live,
                            BASE_COMPUTE_UNITS..units,
                            Placement::Base,
                        )
                        .ok();
                    }
                }
            }
            if render && ready.render.is_none() {
                let start = hints.next_pb_slot.fetch_add(1, Ordering::Relaxed) as usize;
                let base_pb_slots = pick_base_pb_slots(start, hints.quarantined_pb_mask);
                ready.render = Some(RenderArena::new(
                    &self.factory,
                    &self.bind,
                    &vm.vm,
                    &self.live,
                    base_pb_slots,
                )?);
                if ready.render_vm_counter.is_none() {
                    hints.active_render_vms.fetch_add(1, Ordering::AcqRel);
                    ready.render_vm_counter = Some(hints.active_render_vms.clone());
                }
            }
            if render_scs {
                ready.render.as_ref().ok_or(EINVAL)?.ensure_scs(
                    &self.factory,
                    &vm.vm,
                    render_seeds,
                )?;
            }
            if render {
                if let Some(base) = ready.render.as_ref() {
                    let quarantined = hints.quarantined_pb_mask;
                    let mut healthy = base
                        .base_pb_slots
                        .iter()
                        .filter(|&&slot| (quarantined & (1u32 << slot)) == 0)
                        .count();
                    for (idx, ext) in ready.pb_ext.iter().enumerate() {
                        let hw_slot = super::hw::sizes::WARM_PB_SLOTS + idx;
                        if ext.is_some() && (quarantined & (1u32 << hw_slot)) == 0 {
                            healthy += 1;
                        }
                    }
                    if healthy < BASE_PB_SLOTS {
                        let heap = base.pools.heap().clone();
                        for idx in 0..EXT_PB_SLOTS {
                            if healthy >= BASE_PB_SLOTS {
                                break;
                            }
                            let hw_slot = super::hw::sizes::WARM_PB_SLOTS + idx;
                            if ready.pb_ext[idx].is_some()
                                || (quarantined & (1u32 << hw_slot)) != 0
                            {
                                continue;
                            }
                            let local_idx = BASE_PB_SLOTS + idx;
                            // Best effort: without recovery slots, keep prior PB slots.
                            ready.pb_ext[idx] = PoolExtArena::new(
                                &self.factory,
                                &self.bind,
                                &vm.vm,
                                &self.live,
                                local_idx,
                                hw_slot,
                                &heap,
                            )
                            .ok();
                            if ready.pb_ext[idx].is_some() {
                                healthy += 1;
                            } else {
                                break;
                            }
                        }
                    }
                }
                let current_slots = BASE_RENDER_SLOTS
                    + ready
                        .render_ext
                        .iter()
                        .take_while(|tier| tier.is_some())
                        .flatten()
                        .map(|tier| tier.passes.len())
                        .sum::<usize>();
                if current_slots < super::hw::sizes::RENDER_SLOTS {
                    // SAFETY: ktime_get has no preconditions.
                    let now = unsafe { kernel::bindings::ktime_get() } as u64;
                    let state = &mut *ready;
                    let active_passes = self.active_render_passes.load(Ordering::Acquire);
                    let active_vms = hints.active_render_vms.load(Ordering::Acquire);
                    let (target, single_target) = target_render_slots_for(
                        &mut state.last_render_ns,
                        &mut state.render_burst,
                        now,
                        render_count as usize,
                        active_passes,
                        hints.expand_render_ext,
                        current_slots,
                        state.single_vm_render_slots,
                        active_vms,
                    );
                    state.single_vm_render_slots = state.single_vm_render_slots.max(single_target);
                    for (idx, &(start, end)) in EXT_RENDER_TIERS.iter().enumerate() {
                        if start >= target {
                            break;
                        }
                        if state.render_ext[idx].is_none() {
                            // Best effort: stop on allocation failure, keeping lower tiers.
                            state.render_ext[idx] = RenderExtArena::new(
                                &self.factory,
                                &self.bind,
                                &vm.vm,
                                &self.live,
                                start..end,
                            )
                            .ok();
                            if state.render_ext[idx].is_none() {
                                break;
                            }
                        }
                    }
                }
                if let Some(base) = ready.render.as_ref() {
                    let base_blocks = base.pools.heap_blocks();
                    let single_slots = ready.single_vm_render_slots;
                    for (idx, &(start, end)) in PB_HEAP_EXT_TIERS.iter().enumerate() {
                        if single_slots < EXT_RENDER_TIERS[idx].1
                            || ready.render_ext[idx].is_none()
                        {
                            break;
                        }
                        if base_blocks >= end {
                            continue;
                        }
                        if ready.pb_heap_ext[idx].is_none() {
                            let chunk_start = start.max(base_blocks);
                            // Best effort: stop on failure so the backed PB VA stays contiguous.
                            ready.pb_heap_ext[idx] = PbHeapExtArena::new(
                                &self.factory,
                                &self.bind,
                                &vm.vm,
                                &self.live,
                                chunk_start,
                                end - chunk_start,
                            )
                            .ok();
                            if ready.pb_heap_ext[idx].is_none() {
                                break;
                            }
                        }
                    }
                }
            }
            let render_ext = if render {
                core::array::from_fn(|idx| ready.render_ext[idx].clone())
            } else {
                [const { None }; EXT_RENDER_TIERS.len()]
            };
            let pb_heap_ext = if render {
                core::array::from_fn(|idx| ready.pb_heap_ext[idx].clone())
            } else {
                [const { None }; PB_HEAP_EXT_TIERS.len()]
            };
            let pb_ext = if render {
                core::array::from_fn(|idx| ready.pb_ext[idx].clone())
            } else {
                [const { None }; EXT_PB_SLOTS]
            };
            (
                match (compute, compute_scs) {
                    (true, true) => ready.compute_scs.clone(),
                    (true, false) => ready.compute.clone(),
                    (false, _) => None,
                },
                if compute && !compute_scs {
                    ready.compute_ext.clone()
                } else {
                    None
                },
                if render { ready.render.clone() } else { None },
                render_ext,
                pb_heap_ext,
                pb_ext,
            )
        };
        if render {
            if render_count > super::budget::LIMIT {
                return Err(E2BIG);
            }
            let mut remaining = kernel::time::msecs_to_jiffies(10_000);
            loop {
                let deficit = {
                    let mut standby = self.standby.lock();
                    if vm.status.get() != 0
                        || vm.vm.is_closed()
                        || self.closed.load(Ordering::Acquire)
                    {
                        return Err(EIO);
                    }
                    if let Some(needed) = standby
                        .reserved
                        .checked_add(render_count)
                        .filter(|&n| n <= super::budget::LIMIT)
                    {
                        if standby.epochs.len() >= needed as usize {
                            standby.reserved = needed;
                            break;
                        }
                        needed - standby.epochs.len() as u32
                    } else {
                        standby.waiters += 1;
                        let waited = self
                            .standby_available
                            .wait_interruptible_timeout(&mut standby, remaining);
                        standby.waiters -= 1;
                        match waited {
                            CondVarTimeoutResult::Woken { jiffies } => {
                                remaining = jiffies;
                                continue;
                            }
                            CondVarTimeoutResult::Signal { .. } => return Err(ERESTARTSYS),
                            CondVarTimeoutResult::Timeout => {
                                if vm.status.get() != 0
                                    || vm.vm.is_closed()
                                    || self.closed.load(Ordering::Acquire)
                                {
                                    return Err(EIO);
                                }
                                let needed = standby
                                    .reserved
                                    .checked_add(render_count)
                                    .filter(|&n| n <= super::budget::LIMIT)
                                    .ok_or(EBUSY)?;
                                if standby.epochs.len() >= needed as usize {
                                    standby.reserved = needed;
                                    break;
                                }
                                needed - standby.epochs.len() as u32
                            }
                        }
                    }
                };
                let mut epochs = super::budget::Budget::reserve(&self.budget, deficit)?;
                let mut fresh = KVec::with_capacity(deficit as usize, GFP_KERNEL)?;
                for _ in 0..deficit {
                    fresh.push(PbEpoch::new(&self.factory, vm, epochs.take()?)?, GFP_KERNEL)?;
                }
                let mut overflow = KVec::with_capacity(fresh.len(), GFP_KERNEL)?;
                {
                    let mut standby = self.standby.lock();
                    let closed = self.closed.load(Ordering::Acquire);
                    for epoch in fresh {
                        if closed {
                            let _ = overflow.push_within_capacity(epoch);
                        } else if let Err(err) = standby.epochs.push_within_capacity(epoch) {
                            let _ = overflow.push_within_capacity(err.0);
                        }
                    }
                }
                drop(overflow);
            }
        }
        // A concurrent fault or close retains even newly constructed backing in this VM cache.
        if vm.status.get() != 0 || vm.vm.is_closed() || self.closed.load(Ordering::Acquire) {
            if render {
                self.release_reserved(render_count);
            }
            return Err(EIO);
        }
        let cache = if render_count != 0 {
            self.active_render_passes
                .fetch_add(render_count, Ordering::AcqRel);
            Some(self.clone())
        } else {
            None
        };
        Ok(Prepared {
            compute,
            compute_ext,
            compute_scs,
            render: render_arena,
            render_scs,
            render_ext,
            pb_heap_ext,
            pb_ext,
            active_renders: AtomicU32::new(render_count),
            unpublished_renders: AtomicU32::new(render_count),
            cache,
        })
    }
}

/// Immutable references carried by an accepted job. No fallible lazy work in the engine.
pub(crate) struct Prepared {
    pub(crate) compute: Option<Arc<ComputeUnitSet>>,
    /// The VM's extension compute units when the job was prepared.
    pub(crate) compute_ext: Option<Arc<ComputeUnitSet>>,
    /// The job's computes use the VM's SCS-capable compute arena (`0..SCS_COMPUTE_UNITS`).
    compute_scs: bool,
    pub(crate) render: Option<Arc<RenderArena>>,
    /// The job's renders use the arena's SCS-capable allocators (`render.scs`).
    render_scs: bool,
    /// The VM's demand-grown extension render pass tiers when the job was prepared.
    pub(crate) render_ext: [Option<Arc<RenderExtArena>>; EXT_RENDER_TIERS.len()],
    /// The VM's demand-grown contiguous PB heap extension tiers when the job was prepared.
    pub(crate) pb_heap_ext: [Option<Arc<PbHeapExtArena>>; PB_HEAP_EXT_TIERS.len()],
    /// The VM's lazily allocated recovery PB/UMA allocator slots when the job was prepared.
    pub(crate) pb_ext: [Option<Arc<PoolExtArena>>; EXT_PB_SLOTS],
    /// Render commands still accounted against `Cache::active_render_passes`.
    active_renders: AtomicU32,
    /// Render commands of this job whose standby reservation has not yet been committed.
    unpublished_renders: AtomicU32,
    /// The VM cache holding the standby PB epoch pool.
    cache: Option<Arc<Cache>>,
}

impl Drop for Prepared {
    fn drop(&mut self) {
        let Some(cache) = &self.cache else {
            return;
        };
        cache.release_render_passes(*self.active_renders.get_mut());
        cache.release_reserved(*self.unpublished_renders.get_mut());
    }
}
impl Prepared {
    /// Returns canceled render credits even if the job stays retained for its published work.
    /// The device lock serializes suffix discard with publication.
    pub(crate) fn discard_unpublished_renders(&self) {
        let Some(cache) = &self.cache else {
            return;
        };
        let remaining = self.unpublished_renders.swap(0, Ordering::AcqRel);
        if remaining != 0 {
            self.active_renders.fetch_sub(remaining, Ordering::AcqRel);
            cache.release_render_passes(remaining);
            cache.release_reserved(remaining);
        }
    }

    pub(crate) fn compute(&self) -> Result<&ComputeUnitSet> {
        self.compute.as_deref().ok_or(EINVAL)
    }
    /// Compute units this job can use.
    #[allow(dead_code)]
    pub(crate) fn compute_units(&self) -> Result<usize> {
        Ok(self.compute()?.len() + self.compute_ext.as_deref().map_or(0, ComputeUnitSet::len))
    }
    /// Hardware compute ledger bitmask (bit `k` -> unit `k`) this job can use.
    pub(crate) fn compute_slot_mask(&self) -> Result<u32> {
        let base = self.compute()?;
        if let Some(slots) = base.hw_slots {
            let mut mask = 0u32;
            for slot in slots {
                mask |= 1u32 << slot;
            }
            return Ok(mask);
        }
        let total = self.compute_units()?.min(super::hw::sizes::COMPUTE_UNITS);
        Ok(if total >= 32 {
            u32::MAX
        } else {
            (1u32 << total) - 1
        })
    }
    /// The per-unit arena holding compute unit `k`.
    pub(crate) fn compute_arena(&self, k: usize) -> Option<Arc<ComputeArena>> {
        let base = self.compute.as_ref()?;
        if let Some(slots) = base.hw_slots {
            let local = slots.iter().position(|&slot| slot == k)?;
            return base.arena(local);
        }
        if k < base.len() {
            base.arena(k)
        } else {
            self.compute_ext
                .as_ref()
                .and_then(|ext| ext.arena(k - base.len()))
        }
    }
    /// Whether this job's computes use SCS-capable compute units.
    pub(crate) fn compute_scs(&self) -> bool {
        self.compute_scs
    }
    /// Whether this job's renders use SCS-capable render allocators.
    pub(crate) fn render_scs(&self) -> bool {
        self.render_scs
    }
    /// Physical PB/UMA allocator slot bitmask and preferred home slot this job can use.
    pub(crate) fn pb_slot_mask(&self) -> Result<(u32, usize)> {
        let base = self.render()?;
        let mut mask = 0u32;
        for &slot in &base.base_pb_slots {
            mask |= 1u32 << slot;
        }
        if !self.render_scs {
            for (idx, ext) in self.pb_ext.iter().enumerate() {
                if ext.is_some() {
                    mask |= 1u32 << (super::hw::sizes::WARM_PB_SLOTS + idx);
                }
            }
        }
        Ok((mask, base.base_pb_slots[0]))
    }
    fn pb_allocator(&self, k: usize) -> Result<&super::pools::Allocator> {
        let base = self.render()?;
        if let Some(local) = base.base_pb_slots.iter().position(|&slot| slot == k) {
            return if self.render_scs {
                base.scs
                    .as_ref()
                    .and_then(|scs| scs.pools.unit(local))
                    .ok_or(EINVAL)
            } else {
                base.pools.unit(local).ok_or(EINVAL)
            };
        }
        if self.render_scs {
            return Err(EINVAL);
        }
        let idx = k.checked_sub(super::hw::sizes::WARM_PB_SLOTS).ok_or(EINVAL)?;
        self.pb_ext
            .get(idx)
            .and_then(|slot| slot.as_deref())
            .map(|arena| &arena.allocator)
            .ok_or(EINVAL)
    }
    pub(crate) fn prepare_pb_in_flight(
        &self,
        k: usize,
        draw: u64,
        pipelined: bool,
    ) -> Result<super::pools::AllocatorRefs> {
        self.pb_allocator(k)?.prepare_in_flight(draw, pipelined)
    }
    pub(crate) fn pb_allocator_refs(&self, k: usize) -> Result<super::pools::AllocatorRefs> {
        self.pb_allocator(k)?.refs()
    }
    pub(crate) fn publish_pb_batch(&self, k: usize, submitted: u64, renders: u32) -> Result {
        self.pb_allocator(k)?.publish_batch(submitted, renders)
    }
    pub(crate) fn pb_retired(&self, k: usize) -> Result<bool> {
        self.pb_allocator(k)?.retired()
    }
    pub(crate) fn pb_retired_at(&self, k: usize, target: u64) -> Result<bool> {
        self.pb_allocator(k)?.retired_at(target)
    }
    pub(crate) fn pb_heap_gva(&self, k: usize) -> Result<u64> {
        self.pb_allocator(k)?.heap_gva()
    }
    pub(crate) fn pb_heap_blocks(&self) -> Result<u32> {
        let base = self.render()?.pools.heap_blocks();
        let ext: u32 = self
            .pb_heap_ext
            .iter()
            .take_while(|tier| tier.is_some())
            .flatten()
            .map(|tier| tier.blocks)
            .sum();
        Ok(base.saturating_add(ext).min(super::fw::buffer::MAX_BLOCKS))
    }
    /// The arenas owning PB/UMA allocator slot `k` and its backed PB heap tiers.
    pub(crate) fn pb_owner_arena(&self, k: usize) -> Option<PbOwnerArena> {
        let base = self.render.as_ref()?;
        let heap_ext = core::array::from_fn(|idx| self.pb_heap_ext[idx].clone());
        if base.base_pb_slots.contains(&k) {
            Some(PbOwnerArena::Base(base.clone(), heap_ext))
        } else {
            let idx = k.checked_sub(super::hw::sizes::WARM_PB_SLOTS)?;
            self.pb_ext
                .get(idx)
                .and_then(|slot| slot.as_ref())
                .map(|ext| PbOwnerArena::Ext(base.clone(), ext.clone(), heap_ext))
        }
    }
    /// VM-free firmware lease keeping PB/UMA allocator slot `k`'s `descriptor` + `submitted`
    /// pages mapped in context-0 UAT across closed-VM reclaim.
    pub(crate) fn pb_uma_lease(&self, k: usize) -> Option<Arc<super::object::FwLease>> {
        self.pb_allocator(k)
            .ok()
            .map(super::pools::Allocator::uma_lease)
    }
    fn render_ext_for(&self, k: usize) -> Option<(&Arc<RenderExtArena>, usize)> {
        for tier in self.render_ext.iter().take_while(|t| t.is_some()).flatten() {
            if let Some(idx) = k.checked_sub(tier.start) {
                if idx < tier.passes.len() {
                    return Some((tier, idx));
                }
            }
        }
        None
    }
    /// Render pass slots this job can use.
    pub(crate) fn render_passes(&self) -> Result<usize> {
        let ext: usize = self
            .render_ext
            .iter()
            .take_while(|tier| tier.is_some())
            .flatten()
            .map(|tier| tier.passes.len())
            .sum();
        Ok(self.render()?.passes.len() + ext)
    }
    /// Render pass slot `k`: the VM's base slots, then its demand-grown extension tiers.
    pub(crate) fn render_pass(&self, k: usize) -> Result<&PassSlot> {
        let base = &self.render()?.passes;
        match base.get(k) {
            Some(pass) => Ok(pass),
            None => self
                .render_ext_for(k)
                .and_then(|(tier, idx)| tier.passes.get(idx))
                .ok_or(EINVAL),
        }
    }
    /// The dirty high-water marks and clean prefix trackers for pass slot `k`.
    pub(crate) fn render_pass_dirty(&self, k: usize) -> Result<PassDirty<'_>> {
        let base = self.render()?;
        if k < base.passes.len() {
            Ok(PassDirty {
                tpc: &base.tpc_dirty[k],
                tilemap: &base.tilemap_dirty[k],
                tilemap_last: &base.tilemap_last[k],
                tilemap_clean: &base.tilemap_clean[k],
                heap: &base.heap_dirty[k],
            })
        } else {
            let (tier, idx) = self.render_ext_for(k).ok_or(EINVAL)?;
            Ok(PassDirty {
                tpc: &tier.tpc_dirty[idx],
                tilemap: &tier.tilemap_dirty[idx],
                tilemap_last: &tier.tilemap_last[idx],
                tilemap_clean: &tier.tilemap_clean[idx],
                heap: &tier.heap_dirty[idx],
            })
        }
    }
    /// Updates dirty high-water marks and last-used spans for pass slot `k` (base or extension).
    pub(crate) fn set_pass_dirty(&self, k: usize, tpc: u64, tilemap: u64, heap: u64) -> Result {
        let pass = self.render_pass(k)?;
        if tpc > pass.tpc.size as u64
            || tilemap > pass.tilemap.size as u64
            || heap > pass.heap.size as u64
        {
            return Err(E2BIG);
        }
        let dirty = self.render_pass_dirty(k)?;
        dirty.tpc.fetch_max(tpc, Ordering::Relaxed);
        dirty.tilemap.fetch_max(tilemap, Ordering::Relaxed);
        dirty.tilemap_last.store(tilemap, Ordering::Relaxed);
        dirty.tilemap_clean.store(0, Ordering::Relaxed);
        dirty.heap.fetch_max(heap, Ordering::Relaxed);
        Ok(())
    }
    /// The arena owning render pass slot `k`.
    pub(crate) fn pass_owner_arena(&self, k: usize) -> Option<PassOwnerArena> {
        let base = self.render.as_ref()?;
        if k < base.passes.len() {
            Some(PassOwnerArena::Base(base.clone()))
        } else {
            self.render_ext_for(k)
                .map(|(tier, _)| PassOwnerArena::Ext(tier.clone()))
        }
    }
    /// Compute unit `k`: the VM's base/SCS units, then its extension units.
    pub(crate) fn compute_unit(&self, k: usize) -> Result<&ComputeSlot> {
        let base = self.compute()?;
        if let Some(slots) = base.hw_slots {
            let local = slots.iter().position(|&slot| slot == k).ok_or(EINVAL)?;
            return base.unit(local).ok_or(EINVAL);
        }
        match base.unit(k) {
            Some(unit) => Ok(unit),
            None => self
                .compute_ext
                .as_deref()
                .and_then(|e| e.unit(k - base.len()))
                .ok_or(EINVAL),
        }
    }
    pub(crate) fn render(&self) -> Result<&RenderArena> {
        self.render.as_deref().ok_or(EINVAL)
    }
    pub(crate) fn take_pb(&self) -> Result<Arc<PbEpoch>> {
        self.cache.as_ref().ok_or(EINVAL)?.take_standby()
    }
    pub(crate) fn return_pb(&self, epoch: Arc<PbEpoch>) {
        if let Some(cache) = &self.cache {
            cache.return_standby(epoch);
        }
    }
    /// Commits `count` render commands' standby reservations once they reach firmware visibility.
    pub(crate) fn commit_render(&self, count: u32) {
        if count == 0 {
            return;
        }
        if let Some(cache) = &self.cache {
            if let Ok(prev) = self.unpublished_renders.fetch_update(
                Ordering::AcqRel,
                Ordering::Acquire,
                |n| Some(n.saturating_sub(count)),
            ) {
                let released = prev.min(count);
                if released != 0 {
                    cache.release_reserved(released);
                }
            }
        }
    }
}
