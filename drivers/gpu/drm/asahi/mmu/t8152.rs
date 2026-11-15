// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T8152 (G18G; Mac mini M6, J873) UAT support on the Direct backend.
//!
//! Module 1134 ran T8152 on its own MMU (`Uat::new_t8152`, handoff mode
//! `NoPeerT8152`, `m5_context.rs`). This file carries those pieces onto this
//! driver's UAT with [`UatFirmware::NoPeerRoots`]:
//! - the constructor and its firmware-reservation checks;
//! - the preparation, confirmation and mirroring of the firmware-published
//!   high roots, with the shared-L2 topology proof, the DMB barrier and the
//!   log lines of module 1134;
//! - the M5 execution-context leases (contexts 3..63, private empty upper
//!   root, explicit publication), used instead of [`ExecutionContext`];
//! - guard-page mappings and guard-tail aliases;
//! - the module-1134 names G18 calls (`retain_t8140_job`, `id`, `kernel_range`).
//!
//! Bodies are module 1134's (mmu.rs, m5_context.rs) unless a comment says otherwise.

use super::*;
use kernel::drm::gem::BaseObject;
use kernel::page::Page;

/// J873 (T8152) boot-local firmware reservations from the ADT /arm-io/sgx
/// node, as a loader copies them into /reserved-memory.
const T8152_DRAM_BASE: u64 = 0x100_0000_0000;
/// J873 DRAM end: /chosen dram-base 0x100_0000_0000 + dram-size 24 GiB.
const T8152_DRAM_END: u64 = 0x106_0000_0000;
/// (memory-region name, exact size): 26A428 GFX/GFX1 __DATA, then the
/// __DATA_SHARED_RO segments (firmware VA +0x10c000 / +0x108000).
const T8152_FIRMWARE_DATA: [(&CStr, u64); 2] = [
    (c_str!("gfx-data"), 0xbc000),
    (c_str!("gfx1-data"), 0xb4000),
];
const T8152_FIRMWARE_SHARED_RO: [(&CStr, u64); 2] = [
    (c_str!("gfx-data-shared-ro"), 0x8000),
    (c_str!("gfx1-data-shared-ro"), 0x8000),
];

/// The GFX1 root follows the GFX root in the `pagetables` region (module 1134
/// `t8140_secondary_ttb1 = pagetables + 0x40000`).
const T8152_SECONDARY_ROOT_OFFSET: u64 = 0x40000;
/// 42-bit input and output address space (module 1134 `UatConfig::T8152`).
const T8152_GEOMETRY: UatGeometry = UatGeometry { ias: 42, oas: 42 };

/// Entries 0 and 1 of each firmware root are private to that instance.
const T8140_FIRMWARE_PRIVATE_ROOT_ENTRIES: usize = 2;
/// Entries 0..2, including the shared-L2 kernel window, are published by firmware.
const T8140_FIRMWARE_SHARED_ROOT_ENTRIES: usize = 3;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140PrimaryRootSnapshot {
    entries: [u64; T8140_FIRMWARE_SHARED_ROOT_ENTRIES],
}

fn clear_stale_host_root_entries(root: &mut [u64]) {
    if root.len() > T8140_FIRMWARE_SHARED_ROOT_ENTRIES {
        root[T8140_FIRMWARE_SHARED_ROOT_ENTRIES..].fill(0);
    }
}

fn mirror_host_root_entries(primary: &[u64], secondary: &mut [u64]) -> usize {
    let mut mirrored = 0;
    for (source, destination) in primary
        .iter()
        .zip(secondary.iter_mut())
        .skip(T8140_FIRMWARE_PRIVATE_ROOT_ENTRIES)
    {
        if *source != 0 && *destination != *source {
            *destination = *source;
            mirrored += 1;
        }
    }
    mirrored
}

impl Uat {
    /// Create the T8152 UAT. The firmware never writes its half of the
    /// handoff page, so the page is run in [`UatFirmware::NoPeerRoots`] and a
    /// live peer is refused.
    ///
    /// `crash_views` gives the DATA-relative offsets of the GFX and GFX1
    /// RTKit crash views. The G18G offset is not yet known; G18 passes `None`
    /// and that is the only supported value here.
    pub(crate) fn new_t8152(
        dev: &driver::AsahiDevice,
        crash_views: Option<[u64; 2]>,
    ) -> Result<Self> {
        let node = dev.as_ref().as_ref().of_node().ok_or(EINVAL)?;
        let mut regions = [(0u64, 0u64); 4];
        for (index, (name, size)) in Iterator::chain(
            T8152_FIRMWARE_DATA.into_iter(),
            T8152_FIRMWARE_SHARED_RO,
        )
        .enumerate()
        {
            let region = node.reserved_mem_region_to_resource_byname(name)?;
            let start = region.start();
            let end = start.checked_add(region.size()).ok_or(EOVERFLOW)?;
            if start < T8152_DRAM_BASE
                || region.size() != size
                || start & UAT_PGMSK as u64 != 0
                || end > T8152_DRAM_END
            {
                dev_err!(
                    dev.as_ref(),
                    "MMU: T8152 {} reservation {:#x}+{:#x} does not match the 26A428 layout ({:#x} bytes in J873 DRAM)\n",
                    name,
                    start,
                    region.size(),
                    size
                );
                return Err(EINVAL);
            }
            if regions[..index].iter().any(|&(s, e)| start < e && s < end) {
                dev_err!(dev.as_ref(), "MMU: T8152 {} overlaps another firmware reservation\n", name);
                return Err(EINVAL);
            }
            regions[index] = (start, end);
        }
        // Module 1134 maps optional crash views here; G18 never requests them.
        if crash_views.is_some() {
            return Err(ENOTSUPP);
        }
        // The A18 Pro policy is the one module 1134 ran this UAT with: no
        // table cleaning, range TLBIs, the handoff lock and slot 0.
        Self::new(
            dev.clone(),
            T8152_GEOMETRY,
            false,
            UatFirmware::NoPeerRoots {
                secondary_root_offset: T8152_SECONDARY_ROOT_OFFSET,
            },
            UatQuirks::DEFAULT,
        )
    }

    /// Whether this is the T8152 UAT; identity only.
    pub(crate) fn is_t8152_geometry(&self) -> bool {
        matches!(self.inner.firmware, UatFirmware::NoPeerRoots { .. })
            && self.geometry.ias == T8152_GEOMETRY.ias
            && self.geometry.oas == T8152_GEOMETRY.oas
    }

    /// Prepare the firmware-published upper roots for host mappings.
    ///
    /// Each firmware instance publishes three entries during RTKit boot: two
    /// private code/data entries and the shared L2 entry used by the kernel VA
    /// window. Host mappings must be added beneath that live entry, not beneath
    /// a root image constructed before the processor starts.
    pub(crate) fn prepare_t8140_firmware_high_roots(
        &self,
    ) -> Result<T8140PrimaryRootSnapshot> {
        if !self.is_t8152_geometry() {
            return Err(EINVAL);
        }
        let primary_phys = self.kernel_vm.ttb();
        let secondary_phys = self.inner.lock().secondary_ttb1.ok_or(EIO)?;
        // SAFETY: both roots are pages of the bootloader-reserved `pagetables`
        // region, which outlives this Uat.
        let primary = unsafe { Page::borrow_phys(&primary_phys) }.ok_or(EIO)?;
        // SAFETY: as above.
        let secondary = unsafe { Page::borrow_phys(&secondary_phys) }.ok_or(EIO)?;
        let mut primary_entries = [0u64; T8140_FIRMWARE_SHARED_ROOT_ENTRIES];
        let mut secondary_entries = [0u64; T8140_FIRMWARE_SHARED_ROOT_ENTRIES];

        primary.with_pointer_into_page(0, UAT_PGSZ, |pointer| {
            // SAFETY: the pointer covers the whole root page.
            let entries = unsafe {
                core::slice::from_raw_parts_mut(
                    pointer as *mut u64,
                    UAT_PGSZ / core::mem::size_of::<u64>(),
                )
            };
            primary_entries.copy_from_slice(&entries[..T8140_FIRMWARE_SHARED_ROOT_ENTRIES]);
            Ok(())
        })?;
        secondary.with_pointer_into_page(0, UAT_PGSZ, |pointer| {
            // SAFETY: the pointer covers the whole root page.
            let entries = unsafe {
                core::slice::from_raw_parts(
                    pointer as *const u64,
                    UAT_PGSZ / core::mem::size_of::<u64>(),
                )
            };
            secondary_entries.copy_from_slice(&entries[..T8140_FIRMWARE_SHARED_ROOT_ENTRIES]);
            Ok(())
        })?;
        {
            let node = self.dev.as_ref().as_ref().of_node().ok_or(EINVAL)?;
            let shared_l2 = node.reserved_mem_region_to_resource_byname(c_str!("shared-l2"))?;
            let mask = ((1u64 << self.geometry.oas) - 1) & !(UAT_PGMSK as u64);
            if shared_l2.size() != UAT_PGSZ as u64
                || Iterator::chain(primary_entries.iter(), secondary_entries.iter()).any(|pte| pte & 3 != 3)
                || primary_entries[2] & mask != shared_l2.start()
                || secondary_entries[2] & mask != shared_l2.start()
                || primary_entries[0] & mask == secondary_entries[0] & mask
            {
                dev_err!(
                    self.dev.as_ref(),
                    "{} firmware-private roots/shared L2 do not match the native topology\n",
                    "T8152"
                );
                return Err(EIO);
            }
        }
        primary.with_pointer_into_page(0, UAT_PGSZ, |pointer| {
            // SAFETY: the pointer covers the whole root page.
            let entries = unsafe { core::slice::from_raw_parts_mut(
                pointer as *mut u64, UAT_PGSZ / core::mem::size_of::<u64>()) };
            clear_stale_host_root_entries(entries);
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        mem::tlbi_all();
        mem::sync();
        dev_info!(
            self.dev.as_ref(),
            "MMU: GFX high root {:#x} entries [{:#x}, {:#x}, {:#x}]; GFX1 root {:#x} entries [{:#x}, {:#x}, {:#x}]\n",
            primary_phys,
            primary_entries[0],
            primary_entries[1],
            primary_entries[2],
            secondary_phys,
            secondary_entries[0],
            secondary_entries[1],
            secondary_entries[2]
        );
        Ok(T8140PrimaryRootSnapshot {
            entries: primary_entries,
        })
    }

    /// Confirm construction retained the three firmware-published entries.
    pub(crate) fn confirm_t8140_primary_high_root(
        &self,
        snapshot: T8140PrimaryRootSnapshot,
    ) -> Result {
        if !self.is_t8152_geometry() {
            return Err(EINVAL);
        }
        let primary_phys = self.kernel_vm.ttb();
        // SAFETY: the root is a page of the bootloader-reserved `pagetables` region.
        let primary = unsafe { Page::borrow_phys(&primary_phys) }.ok_or(EIO)?;
        let mut current = [0u64; T8140_FIRMWARE_SHARED_ROOT_ENTRIES];
        primary.with_pointer_into_page(0, UAT_PGSZ, |pointer| {
            // SAFETY: the pointer covers the whole root page.
            let entries = unsafe {
                core::slice::from_raw_parts(
                    pointer as *const u64,
                    UAT_PGSZ / core::mem::size_of::<u64>(),
                )
            };
            current.copy_from_slice(&entries[..T8140_FIRMWARE_SHARED_ROOT_ENTRIES]);
            Ok(())
        })?;
        if current != snapshot.entries {
            dev_err!(
                self.dev.as_ref(),
                "MMU: GFX high-root firmware entries changed during graph construction: before {:?}, after {:?}\n",
                snapshot.entries,
                current
            );
            return Err(EIO);
        }
        dev_info!(
            self.dev.as_ref(),
            "MMU: GFX high-root firmware entries preserved across graph construction\n"
        );
        Ok(())
    }

    /// Mirror host-visible upper-root entries into GFX1's private top table.
    /// Entries 0 and 1 remain owned by that firmware instance.
    pub(crate) fn mirror_t8140_secondary_high_root(&self) -> Result<usize> {
        if !self.is_t8152_geometry() {
            return Err(EINVAL);
        }
        let primary_phys = self.kernel_vm.ttb();
        let secondary_phys = self.inner.lock().secondary_ttb1.ok_or(EIO)?;
        // SAFETY: both roots are pages of the bootloader-reserved `pagetables` region.
        let primary = unsafe { Page::borrow_phys(&primary_phys) }.ok_or(EIO)?;
        // SAFETY: as above.
        let secondary = unsafe { Page::borrow_phys(&secondary_phys) }.ok_or(EIO)?;
        let mirrored = primary.with_pointer_into_page(0, UAT_PGSZ, |primary_pointer| {
            secondary.with_pointer_into_page(0, UAT_PGSZ, |secondary_pointer| {
                // SAFETY: each pointer covers its whole root page; the pages differ.
                let primary_entries = unsafe {
                    core::slice::from_raw_parts(
                        primary_pointer as *const u64,
                        UAT_PGSZ / core::mem::size_of::<u64>(),
                    )
                };
                // SAFETY: as above.
                let secondary_entries = unsafe {
                    core::slice::from_raw_parts_mut(
                        secondary_pointer as *mut u64,
                        UAT_PGSZ / core::mem::size_of::<u64>(),
                    )
                };
                Ok(mirror_host_root_entries(primary_entries, secondary_entries))
            })
        })?;
        fence(Ordering::SeqCst);
        mem::tlbi_all();
        mem::sync();
        dev_info!(
            self.dev.as_ref(),
            "MMU: mirrored {} host high-root entries into GFX1 at {:#x}\n",
            mirrored,
            secondary_phys
        );
        Ok(mirrored)
    }

    /// Unpublished, driver-private VM for the qualified G18 compute fixture.
    /// Its VA-zero allocation is required by the saved native user payload.
    /// This allocates page tables only; no context slot or TTB root is written.
    pub(crate) fn new_t8152_compute_fixture_vm(&self) -> Result<Vm> {
        if !self.is_t8152_geometry() { return Err(ENODEV); }
        // The user-VM allocator of a non-Handoff UAT starts at VA 0, as the
        // module-1134 internal fixture VM's did.
        self.new_vm(0x8152_0001, IOVA_USER_BASE..T8152_GEOMETRY.user_range().end)?
            .with_t8152_lifetime()
    }

    /// G18's 26A428 HAL400 exposes 64 context IDs (8c44c50..8c44c58).
    /// The T8152 UAT already reserves the native firmware roots in 0..2;
    /// use a separate, retained lower/empty-upper pair. This claims a slot
    /// only. Root publication remains explicit and bounded by the owning G18
    /// firmware session's lifetime.
    pub(crate) fn prepare_t8152_execution_context(
        &self, vm: &Vm,
    ) -> Result<G18ExecutionContext> {
        if !self.is_t8152_geometry() {
            return Err(ENODEV);
        }
        {
            let owner = vm.gpuvm.lock_inner();
            if owner.is_kernel || !core::ptr::eq(
                Arc::as_ptr(&owner.uat_inner), Arc::as_ptr(&self.inner),
            ) {
                return Err(EINVAL);
            }
        }
        // Module 1134 claims the lowest free context in 3..63 with a CAS on
        // its own bitmap; here the same bitmap is the shared execution-context
        // set, seeded with 0..2 for NoPeerRoots and taken under the UAT lock.
        let context = {
            let mut inner = self.inner.lock();
            let free = !inner.execution_contexts & !0b111u64;
            if free == 0 {
                return Err(ENOSPC);
            }
            let index = free.trailing_zeros() as usize;
            inner.execution_contexts |= 1u64 << index;
            index
        };
        // Construct the slot guard first so a failed private-root allocation
        // returns its claim. This high VM is never exposed for mappings.
        let slot = M5ContextSlot { inner: self.inner.clone(), context };
        let range = IOVA_USER_BASE..T8152_GEOMETRY.user_range().end;
        let high = self.new_vm((0x8152u64 << 16) + 0x1000 + context as u64, range)?
            .with_t8152_lifetime()?;
        let low = vm.clone();
        let roots = context_roots(context, &low, &high)?;
        Ok(G18ExecutionContext { slot, low, high, roots, published: false })
    }
}

struct M5ContextSlot {
    inner: Arc<UatInner>,
    context: usize,
}

impl Drop for M5ContextSlot {
    fn drop(&mut self) {
        // An unexpected published root is not ours to retract or recycle.
        // Keep its bit quarantined rather than hand it to another client.
        let mut inner = self.inner.lock();
        let slot = &inner.ttbs()[self.context];
        let empty = slot.ttb0.load(Ordering::Acquire) == 0
            && slot.ttb1.load(Ordering::Acquire) == 0;
        if empty {
            inner.execution_contexts &= !(1u64 << self.context);
        }
    }
}

/// Retained M5 application root lease, shared by render and compute queues.
///
/// Native M5/G18 compute has a private lower root and a distinct empty upper
/// root. Its firmware-global root is not copied into the application slot.
/// Firmware owns contexts 0..2. Application contexts are claimed before any
/// root becomes visible, and the lease retains both VM roots. The submission
/// owner must retain the lease until completion or confirmed processor stop.
pub(crate) struct G18ExecutionContext {
    slot: M5ContextSlot,
    low: Vm,
    high: Vm,
    /// The tagged roots of `low` and `high`, computed when `low` was set
    /// (`Vm::ttb` is fallible on this driver; module 1134 recomputed them).
    roots: [u64; 2],
    published: bool,
}

fn context_roots(context: usize, low: &Vm, high: &Vm) -> Result<[u64; 2]> {
    let tag = (context as u64) << TTBR_ASID_SHIFT | TTBR_VALID;
    Ok([low.ttb() | tag, high.ttb() | tag])
}

impl G18ExecutionContext {
    pub(crate) fn vm(&self) -> &Vm { &self.low }

    pub(crate) fn context_id(&self) -> u8 { self.slot.context as u8 }

    pub(crate) fn matches(&self, vm: &Vm) -> bool {
        core::ptr::eq(&*self.low.gpuvm, &*vm.gpuvm)
    }

    pub(crate) fn roots(&self) -> [u64; 2] {
        self.roots
    }

    /// Publish only after all command-owned mappings have been prepared.
    /// An occupied slot is never replaced, even by a second M5 probe owner.
    pub(crate) fn publish(&mut self) -> Result {
        if self.published { return Err(EBUSY); }
        let roots = self.roots();
        {
            let inner = self.slot.inner.lock();
            let slot = &inner.ttbs()[self.slot.context];
            if slot.ttb0.load(Ordering::Acquire) != 0
                || slot.ttb1.load(Ordering::Acquire) != 0 {
                return Err(EBUSY);
            }
            slot.ttb0.store(roots[0], Ordering::Release);
            slot.ttb1.store(roots[1], Ordering::Release);
            self.published = true;
        }
        fence(Ordering::SeqCst);
        mem::tlbi_asid(self.context_id());
        mem::sync();
        Ok(())
    }

    pub(crate) fn is_current(&self) -> bool {
        if !self.published { return false; }
        let roots = self.roots();
        let inner = self.slot.inner.lock();
        let slot = &inner.ttbs()[self.slot.context];
        slot.ttb0.load(Ordering::Acquire) == roots[0]
            && slot.ttb1.load(Ordering::Acquire) == roots[1]
    }

    /// Retarget this lease at another application VM without giving up the
    /// hardware context slot. Any published roots are retracted and the ASID
    /// invalidated first, exactly as on drop; the caller has already verified
    /// that no work of the previous VM is outstanding. Publication of the new
    /// lower root is explicit again, once the new owner's graph is prepared.
    pub(crate) fn rebind(&mut self, vm: &Vm) -> Result {
        {
            let owner = vm.gpuvm.lock_inner();
            if owner.is_kernel || !core::ptr::eq(
                Arc::as_ptr(&owner.uat_inner), Arc::as_ptr(&self.slot.inner),
            ) {
                return Err(EINVAL);
            }
        }
        let roots = context_roots(self.slot.context, vm, &self.high)?;
        self.retract();
        self.low = vm.clone();
        self.roots = roots;
        Ok(())
    }

    fn retract(&mut self) {
        if !self.published { return; }
        let roots = self.roots();
        {
            let inner = self.slot.inner.lock();
            let slot = &inner.ttbs()[self.slot.context];
            // Only our exact pair may be retracted. VM owners remain alive
            // through root invalidation and the final ASID synchronization.
            if slot.ttb0.load(Ordering::Acquire) == roots[0]
                && slot.ttb1.load(Ordering::Acquire) == roots[1] {
                slot.ttb0.store(0, Ordering::Release);
                slot.ttb1.store(0, Ordering::Release);
            }
        }
        fence(Ordering::SeqCst);
        mem::tlbi_asid(self.context_id());
        mem::sync();
        self.published = false;
    }
}

impl Drop for G18ExecutionContext {
    fn drop(&mut self) {
        self.retract();
    }
}

impl Vm {
    /// Attach the sticky status and job lifetime that every module-1134 VM
    /// had, and nothing else: no residency gate (no idle reclaim or shrinker),
    /// no T8140 context-view bindings and no shared-footprint tracking. Use
    /// this instead of [`Vm::with_status`] for every VM of the T8152 UAT.
    pub(crate) fn with_t8152_lifetime(mut self) -> Result<Self> {
        if self.status.is_none() {
            self.status = Some(Arc::pin_init(crate::g17::status::VmStatus::new(), GFP_KERNEL)?);
        }
        self.lifetime = Some(lifetime::VmLifetime::new()?);
        Ok(self)
    }

    /// The module-1134 name of [`Vm::retain_job`].
    pub(crate) fn retain_t8140_job(&self) -> Result<VmJobGuard> {
        self.retain_job()
    }

    /// Reserve a checked number of trailing unmapped pages for M5 resources.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn map_in_range_with_guards(
        &self,
        gem: &gem::Object,
        object_range: Range<usize>,
        alignment: u64,
        range: Range<u64>,
        prot: Prot,
        guard_pages: usize,
    ) -> Result<KernelMapping> {
        let guard_size = guard_pages.checked_mul(UAT_PGSZ).ok_or(EOVERFLOW)?;
        self.map_in_range_with_guard_size(
            gem, object_range, alignment, range, prot, guard_size,
        )
    }

    /// Map a GEM object at a specific address and reserve `guard_pages`
    /// trailing unmapped pages in the same allocator node, so an alias of a
    /// guarded allocation keeps its hardware guard gap in this Vm too.
    pub(crate) fn map_at_with_guards(
        &self,
        addr: u64,
        size: usize,
        gem: &gem::Object,
        prot: Prot,
        guard_pages: usize,
    ) -> Result<KernelMapping> {
        if size != gem.size() {
            return Err(EINVAL);
        }
        let guard = guard_pages.checked_mul(UAT_PGSZ).ok_or(EOVERFLOW)?;
        let reserved = size.checked_add(guard).ok_or(EOVERFLOW)?;
        let end = addr.checked_add(reserved as u64).ok_or(EOVERFLOW)?;
        // The range holds exactly one placement, so this is a fixed-address
        // mapping of the whole object followed by its guard reservation.
        self.map_in_range_lowest(gem, 0..size, UAT_PGSZ as u64, addr..end, prot, guard)
    }
}

impl KernelMapping {
    /// Map this shared GEM subrange into the end of another mapping's guards.
    /// The caller must keep GPU work quiescent until the complete resource
    /// update is published. Existing mappings are never removed or relocated.
    pub(crate) fn map_alias_into_guard(
        &self, anchor: &mut KernelMapping, prot: Prot,
    ) -> Result<KernelMapping> {
        let size = self.size();
        let gem: &gem::Object = self.0._gem.as_deref().ok_or(EINVAL)?;
        if size == 0 || size & UAT_PGMSK != 0 || prot.is_cached_noncoherent()
            || !gem.is_kernel()
            || self.0.offset.checked_add(size).ok_or(EOVERFLOW)? > gem.size()
        {
            return Err(EINVAL);
        }
        let available = anchor.0.size().checked_sub(anchor.size() as u64).ok_or(EINVAL)?;
        if size as u64 > available { return Err(ENOSPC); }
        let owner_ref = anchor.0.owner.clone();
        // As in Vm::map_at_reserving: hold the backing and prepare the VM BO
        // outside the VM reservation, and end the bound-device lease first.
        let (_bind_lease, vm_bo) = {
            let gate = crate::binding::lookup(&owner_ref.lock_inner().dev)?;
            let binding = gate.enter()?;
            let dev = binding.device();
            let lease = crate::gem::held_sg_table(gem, dev)?.into_lease();
            let bo = owner_ref.obtain(
                gem,
                pin_init!(VmBoData {
                    newest_bind: AtomicU64::new(0),
                    segments <- new_mutex!(None),
                }),
            )?;
            prepare_sgt_segments(dev, gem, &bo, &lease)?;
            (lease, bo)
        };
        // SAFETY: no other UniqRefGpuVm exists, as in Vm::map_at_reserving.
        let mut vm_exec = unsafe { owner_ref.exec_lock_vm(None, false)? };
        let inner = vm_exec.unique_gpuvm.data();
        let mut vm_bo = Some(MappingBo::new(vm_bo, owner_ref.clone()));
        // As in map_at_with_guards, the fallible allocator operation must not
        // own DMA-reservation references whose Drop would reacquire this lock.
        let node = anchor.0.as_mut().split_tail(size as u64, KernelMappingInner {
            owner: owner_ref.clone(), uat_inner: inner.uat_inner.clone(), prot,
            bo: None, _gem: None, offset: self.0.offset, mapped_size: size,
        });
        let mut node = match node {
            Ok(node) => node,
            Err(error) => { drop(vm_exec); return Err(error); }
        };
        {
            let payload = node.as_mut().inner_mut();
            payload.bo = vm_bo.take();
            payload._gem = Some(gem::KernelMappingPin::new(gem));
        }
        let mapped = inner.map_node(&node, prot);
        if let Err(error) = mapped {
            // Restore reservation coverage before the temporary mapping is
            // dropped. Its normal Drop unmaps partial PTEs and invalidates
            // every live context; the detached node cannot free the guard.
            let rollback = anchor.0.as_mut().rejoin_tail(node.as_mut());
            drop(vm_exec);
            drop(KernelMapping(ManuallyDrop::new(node)));
            rollback?;
            return Err(error);
        }
        drop(vm_exec);
        Ok(KernelMapping(ManuallyDrop::new(node)))
    }

    /// Undo a staged guard-tail alias before publication. Consume tails in
    /// reverse growth order so each remains adjacent to its original anchor.
    /// The caller owns the tail and must ensure no GPU work can reference it.
    pub(crate) fn rollback_guard_tail(&mut self, mut tail: KernelMapping) -> Result {
        if tail.0.prot.is_cached_noncoherent() { return Err(EINVAL); }
        self.0.as_mut().rejoin_tail(tail.0.as_mut())?;
        // KernelMapping::drop removes the PTEs and flushes live contexts. Its
        // now-invalid MM node leaves the restored guard reservation intact.
        drop(tail);
        Ok(())
    }
}
