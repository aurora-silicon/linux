// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! GPU UAT (MMU) management
//!
//! AGX GPUs use an MMU called the UAT, which is largely compatible with the ARM64 page table
//! format. This module manages the global MMU structures, including a shared handoff structure
//! that is used to coordinate VM management operations with the firmware, the TTBAT which points
//! to currently active GPU VM contexts, as well as the individual `Vm` operations to map and
//! unmap buffer objects into a single user or kernel address space.
//!
//! The actual page table management is in the `pt` module.
//!
//! G17 firmware does not take part in the handoff protocol. Instead, each of its two coprocessor
//! instances publishes the top-level entries it owns into its own kernel root while it boots, and
//! TTBAT contexts other than a VM's bind slot may name a VM's root. See [`UatFirmware`].

use core::fmt::Debug;
use core::mem::size_of;
use core::num::NonZeroUsize;
use core::ops::Range;
use core::sync::atomic::{
    fence,
    AtomicBool,
    AtomicU32,
    AtomicU64,
    AtomicU8,
    Ordering, //
};

use core::cmp;

use kernel::{
    addr::PhysicalAddr,
    bindings::drm_gpuvm_flags_DRM_GPUVM_IMMEDIATE_MODE,
    c_str,
    device,
    drm::{
        gem::shmem,
        gpuvm,
        mm, //
    },
    error::Result,
    io,
    new_mutex,
    prelude::*,
    static_lock_class,
    sync::{
        aref::ARef,
        lock::{
            mutex::MutexBackend,
            Guard, //
        },
        Arc,
        Mutex, //
    },
    time::{
        delay::fsleep,
        Delta,
        Instant,
        Monotonic, //
    }, //
};

use crate::debug::*;
use crate::module_parameters;
use crate::no_debug;
use crate::{
    driver,
    fw,
    gem,
    mem,
    pgtable,
    slotalloc,
    util::RangeExt, //
};

// KernelMapping protection types
pub(crate) use crate::pgtable::Prot;
pub(crate) use pgtable::prot::*;
pub(crate) use pgtable::{
    UatPageTable,
    UAT_PGBIT,
    UAT_PGMSK,
    UAT_PGSZ,
    UAT_TOP_LEVEL_SHIFT, //
};

use pin_init;

const DEBUG_CLASS: DebugFlags = DebugFlags::Mmu;

mod bind;
mod context;
mod shared;
mod lifetime;
pub(crate) use bind::PreparedUserBindBatch;
pub(crate) use lifetime::VmJobGuard;

/// PPL magic number for the handoff region
const PPL_MAGIC: u64 = 0x4b1d000000000002;

/// Number of supported context entries in the TTBAT
const UAT_NUM_CTX: usize = 64;

/// Application contexts owned by independent logical queues. Contexts 0..4
/// and 63 remain reserved for the firmware and device-global resources.
const EXECUTION_CONTEXT_MASK: u64 = ((1u64 << 63) - 1) & !((1u64 << 5) - 1);
/// Maximum simultaneously retained logical queue contexts.
pub(crate) const MAX_EXECUTION_CONTEXTS: u32 = EXECUTION_CONTEXT_MASK.count_ones();
/// First context available for users
const UAT_USER_CTX_START: usize = 1;
/// Number of available user contexts
const UAT_USER_CTX: usize = UAT_NUM_CTX - UAT_USER_CTX_START;

/// Lower/user base VA
pub(crate) const IOVA_USER_BASE: u64 = UAT_PGSZ as u64;

/// Top-level page table entry of the upper half that holds the driver-managed kernel VA range.
const IOVA_KERN_TOP_LEVEL_INDEX: u64 = 2;

/// TTBAT context that starts out as an alias of the kernel context (with its own ASID) with
/// [`UatFirmware::PublishedRoots`].
const KERNEL_ALIAS_CTX: usize = 1;

/// How the GPU firmware shares UAT management with the driver.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum UatFirmware {
    /// One coprocessor that publishes its magic number in the handoff region, arbitrates the
    /// handoff lock with the driver, and acknowledges cache flush requests (G13/G14).
    Handoff,
    /// Two coprocessor instances that never touch the handoff region (G17).
    ///
    /// Each instance has its own kernel root and writes the top-level entries below and including
    /// the driver-managed kernel window into it while it boots: the lower ones are private to the
    /// instance, and the kernel window entry points at a table that the bootloader reserved. The
    /// primary instance uses the `pagetables` root, the secondary one the root at
    /// `secondary_root_offset` in the same region. TTBAT context 1 starts as an alias of the kernel
    /// context.
    PublishedRoots {
        /// Offset of the secondary instance's kernel root in the `pagetables` region.
        secondary_root_offset: u64,
    },
}

const TTBR_VALID: u64 = 0x1; // BIT(0)
const TTBR_ASID_SHIFT: usize = 48;

/// UAT address space geometry of a given SoC.
#[derive(Debug, Copy, Clone)]
pub(crate) struct UatGeometry {
    /// Input address bits translated by each page table root.
    pub(crate) ias: u32,
    /// Output (physical) address bits.
    pub(crate) oas: u32,
}

impl UatGeometry {
    /// Lower/user top VA
    const fn user_top(&self) -> u64 {
        1 << self.ias
    }

    /// Lower/user VA range
    pub(crate) const fn user_range(&self) -> Range<u64> {
        IOVA_USER_BASE..self.user_top()
    }

    /// Address of a special dummy page?
    pub(crate) const fn unk_page(&self) -> u64 {
        self.user_top() - 2 * UAT_PGSZ as u64
    }

    /// User VA range excluding the unk page
    pub(crate) const fn user_usable_range(&self) -> Range<u64> {
        IOVA_USER_BASE..self.unk_page()
    }

    /// Upper/kernel base VA
    const fn upper_base(&self) -> u64 {
        !(self.user_top() - 1)
    }

    /// Driver-managed kernel VA range
    pub(crate) const fn kernel_range(&self) -> Range<u64> {
        let start = self.upper_base() + (IOVA_KERN_TOP_LEVEL_INDEX << UAT_TOP_LEVEL_SHIFT);
        start..(start + (1 << UAT_TOP_LEVEL_SHIFT))
    }

    /// Full kernel VA range
    #[cfg(CONFIG_DEV_COREDUMP)]
    const fn upper_range(&self) -> Range<u64> {
        self.upper_base()..(!UAT_PGMSK as u64)
    }
}

/// A pre-allocated memory region for UAT management
struct UatRegion {
    base: PhysicalAddr,
    map: io::mem::Mem,
}

/// SAFETY: It's safe to share UAT region records across threads.
unsafe impl Send for UatRegion {}
/// SAFETY: It's safe to share UAT region records across threads.
unsafe impl Sync for UatRegion {}

/// Handoff region flush info structure
#[repr(C)]
struct FlushInfo {
    state: AtomicU64,
    addr: AtomicU64,
    size: AtomicU64,
}

/// UAT Handoff region layout
#[repr(C)]
struct Handoff {
    magic_ap: AtomicU64,
    magic_fw: AtomicU64,

    lock_ap: AtomicU8,
    lock_fw: AtomicU8,
    // Implicit padding: 2 bytes
    turn: AtomicU32,
    cur_slot: AtomicU32,
    // Implicit padding: 4 bytes
    flush: [FlushInfo; UAT_NUM_CTX + 1],

    unk2: AtomicU8,
    // Implicit padding: 7 bytes
    unk3: AtomicU64,
}

const HANDOFF_SIZE: usize = size_of::<Handoff>();

/// One VM slot in the TTBAT
#[repr(C)]
struct SlotTTBS {
    ttb0: AtomicU64,
    ttb1: AtomicU64,
}

const SLOTS_SIZE: usize = UAT_NUM_CTX * size_of::<SlotTTBS>();

// We need at least page 0 (ttb0)
const PAGETABLES_SIZE: usize = UAT_PGSZ;

/// Lock-free visibility of the mapping epoch. Writers run under the GPUVM
/// exec lock; nested mutations keep cached validation unavailable until the
/// outermost mutation has finished its leaf writes and invalidations.
struct MappingEpoch {
    visible: AtomicU64,
    pending: AtomicU64,
    writers: AtomicU32,
}

impl MappingEpoch {
    fn new() -> Self {
        Self {
            visible: AtomicU64::new(0),
            pending: AtomicU64::new(0),
            writers: AtomicU32::new(0),
        }
    }
    fn snapshot(&self) -> Option<u64> {
        let value = self.visible.load(Ordering::Acquire);
        (value != u64::MAX).then_some(value)
    }
}

#[must_use = "retain until the mapping mutation and its invalidations finish"]
struct MappingMutation(Option<Arc<MappingEpoch>>);

impl Drop for MappingMutation {
    fn drop(&mut self) {
        if let Some(epoch) = self.0.as_ref() {
            if epoch.writers.fetch_sub(1, Ordering::Relaxed) == 1 {
                epoch
                    .visible
                    .store(epoch.pending.load(Ordering::Relaxed), Ordering::Release);
            }
        }
    }
}

/// Inner data for a Vm instance. This is reference-counted by the outer Vm object.
struct VmInner {
    dev: driver::AsahiDevRef,
    is_kernel: bool,
    va_range: Range<u64>,
    page_table: UatPageTable,
    mapping_epoch: Option<Arc<MappingEpoch>>,
    epoch: Option<u64>,
    mm: mm::Allocator<(), KernelMappingInner>,
    uat_inner: Arc<UatInner>,
    binding: Arc<Mutex<VmBinding>>,
    id: u64,
}

/// Slot binding-related inner data for a Vm instance.
struct VmBinding {
    active_users: usize,
    binding: Option<slotalloc::Guard<SlotInner>>,
    bind_token: Option<slotalloc::SlotToken>,
    ttb: u64,
}

struct VmBoInner {
    sgt: Option<shmem::SGTable<gem::AsahiObject>>,
    sg_vec: Option<KVVec<(usize, Range<usize>)>>,
}

/// Data associated with a VM <=> BO pairing
#[pin_data]
struct VmBo {
    #[pin]
    inner: Mutex<VmBoInner>,
}

impl gpuvm::DriverGpuVmBo for VmBo {
    fn new() -> impl PinInit<Self> {
        pin_init!(VmBo {
            inner <- new_mutex!(VmBoInner {
                sgt: None,
                sg_vec: None,
            }, "VmBinding"),
        })
    }
}

#[derive(Default)]
struct StepContext {
    new_va: Option<Pin<KBox<gpuvm::GpuVa<VmInner>>>>,
    prev_va: Option<Pin<KBox<gpuvm::GpuVa<VmInner>>>>,
    next_va: Option<Pin<KBox<gpuvm::GpuVa<VmInner>>>>,
    vm_bo: Option<ARef<gpuvm::GpuVmBo<VmInner>>>,
    prot: Prot,
}

impl gpuvm::DriverGpuVm for VmInner {
    type Driver = driver::AsahiDriver;
    type GpuVmBo = VmBo;
    type StepContext = StepContext;

    fn step_map(
        self: &mut gpuvm::UpdatingGpuVm<'_, Self>,
        op: &mut gpuvm::OpMap<Self>,
        ctx: &mut Self::StepContext,
    ) -> Result {
        let _mutation = self.mapping_mutation();
        let mut iova = op.addr();
        let mut left = op.range() as usize;
        let mut offset = op.offset() as usize;

        let bo = ctx.vm_bo.as_ref().expect("step_map with no BO");

        let one_page = op.flags().contains(gpuvm::GpuVaFlags::REPEAT);

        let mut do_map = |mut addr: usize, mut len: usize, offset: &mut usize| -> Result<bool> {
            if left == 0 {
                return Ok(false);
            }

            if *offset > 0 {
                let skip = len.min(*offset);
                addr += skip;
                len -= skip;
                *offset -= skip;
            }
            if len == 0 {
                return Ok(true);
            }
            assert!(*offset == 0);

            if one_page {
                len = left;
            } else {
                len = len.min(left);
            }

            mod_dev_dbg!(
                self.dev,
                "MMU: map: {:#x}:{:#x} -> {:#x} [OP={}]\n",
                addr,
                len,
                iova,
                one_page
            );

            if let Err(error) = self.page_table.map_pages(
                iova..(iova + len as u64),
                addr as PhysicalAddr,
                ctx.prot,
                one_page,
            ) {
                self.rollback_failed_map(op.addr(), op.range());
                return Err(error);
            }

            left -= len;
            iova += len as u64;
            Ok(true)
        };

        let guard = bo.inner().inner.lock();
        if let Some(sg_vec) = guard.sg_vec.as_ref() {
            let start_idx = sg_vec
                .binary_search_by(|range| {
                    if range.0 > offset {
                        cmp::Ordering::Greater
                    } else if (range.0 + range.1.len()) <= offset {
                        cmp::Ordering::Less
                    } else {
                        cmp::Ordering::Equal
                    }
                })
                .expect("sg_vec does not contain offset???");

            offset -= sg_vec[start_idx].0 as usize;

            for cur in start_idx..sg_vec.len() {
                let addr = sg_vec[cur].1.start as usize;
                let len: usize = sg_vec[cur].1.len() as usize;
                if do_map(addr, len, &mut offset)? == false {
                    break;
                }
            }
        } else {
            for range in guard.sgt.as_ref().expect("step_map with no SGT").iter() {
                // TODO: proper DMA address/length handling
                let addr = range.dma_address() as usize;
                let len: usize = range.dma_len() as usize;
                if do_map(addr, len, &mut offset)? == false {
                    break;
                }
            }
        }

        let gpuva = ctx.new_va.take().expect("Multiple step_map calls");

        if op
            .map_and_link_va(
                self,
                gpuva,
                ctx.vm_bo.as_ref().expect("step_map with no BO"),
            )
            .is_err()
        {
            dev_err!(
                self.dev.as_ref(),
                "map_and_link_va failed: {:#x} [{:#x}] -> {:#x}\n",
                op.offset(),
                op.range(),
                op.addr()
            );
            self.rollback_failed_map(op.addr(), op.range());
            return Err(EINVAL);
        }
        self.tlbi_contexts_naming_root(op.addr(), op.range() as usize);
        Ok(())
    }
    fn step_unmap(
        self: &mut gpuvm::UpdatingGpuVm<'_, Self>,
        op: &mut gpuvm::OpUnMap<Self>,
        _ctx: &mut Self::StepContext,
    ) -> Result {
        let va = op.va().expect("step_unmap: missing VA");

        self.unmap_gpuva(va)?;

        if op.unmap_and_unlink_va_defer().is_none() {
            dev_err!(self.dev.as_ref(), "step_unmap: could not unlink gpuva");
        }
        Ok(())
    }
    fn step_remap(
        self: &mut gpuvm::UpdatingGpuVm<'_, Self>,
        op: &mut gpuvm::OpReMap<Self>,
        vm_bo: &gpuvm::GpuVmBo<Self>,
        ctx: &mut Self::StepContext,
    ) -> Result {
        let va = op.unmap().va().expect("No previous VA");
        let orig_addr = va.addr();
        let orig_range = va.range();

        // Only unmap the hole between prev/next, if they exist
        let unmap_start = if let Some(op) = op.prev_map() {
            op.addr() + op.range()
        } else {
            orig_addr
        };

        let unmap_end = if let Some(op) = op.next_map() {
            op.addr()
        } else {
            orig_addr + orig_range
        };

        mod_dev_dbg!(
            self.dev,
            "MMU: unmap for remap: {:#x}..{:#x} (from {:#x}:{:#x})\n",
            unmap_start,
            unmap_end,
            orig_addr,
            orig_range
        );

        let unmap_range = unmap_end - unmap_start;

        let _mutation = self.mapping_mutation();
        self.page_table.unmap_pages(unmap_start..unmap_end)?;

        if let Some(asid) = self.slot() {
            fence(Ordering::SeqCst);
            self.tlbi_range(asid as u8, unmap_start, unmap_range as usize);
            mod_dev_dbg!(
                self.dev,
                "MMU: flush range: asid={:#x} start={:#x} len={:#x}\n",
                asid,
                unmap_start,
                unmap_range,
            );
            mem::sync();
        }

        self.tlbi_contexts_naming_root(unmap_start, unmap_range as usize);

        if op.unmap().unmap_and_unlink_va_defer().is_none() {
            dev_err!(self.dev.as_ref(), "step_unmap: could not unlink gpuva");
        }

        if let Some(prev_op) = op.prev_map() {
            let prev_gpuva = ctx
                .prev_va
                .take()
                .expect("Multiple step_remap calls with prev_op");
            if prev_op.map_and_link_va(self, prev_gpuva, vm_bo).is_err() {
                dev_err!(self.dev.as_ref(), "step_remap: could not relink prev gpuva");
                return Err(EINVAL);
            }
        }

        if let Some(next_op) = op.next_map() {
            let next_gpuva = ctx
                .next_va
                .take()
                .expect("Multiple step_remap calls with next_op");
            if next_op.map_and_link_va(self, next_gpuva, vm_bo).is_err() {
                dev_err!(self.dev.as_ref(), "step_remap: could not relink next gpuva");
                return Err(EINVAL);
            }
        }

        Ok(())
    }
}

impl VmInner {
    /// Remove hardware leaves for one complete user GPUVA without allocating.
    fn unmap_gpuva(&mut self, va: &gpuvm::GpuVa<Self>) -> Result {
        mod_dev_dbg!(self.dev, "MMU: unmap: {:#x}:{:#x}\n", va.addr(), va.range());

        let _mutation = self.mapping_mutation();
        self.page_table
            .unmap_pages(va.addr()..(va.addr() + va.range()))?;

        if let Some(asid) = self.slot() {
            fence(Ordering::SeqCst);
            self.tlbi_range(asid as u8, va.addr(), va.range() as usize);
            mod_dev_dbg!(
                self.dev,
                "MMU: flush range: asid={:#x} start={:#x} len={:#x}\n",
                asid,
                va.addr(),
                va.range(),
            );
            mem::sync();
        }

        self.tlbi_contexts_naming_root(va.addr(), va.range() as usize);

        Ok(())
    }

    /// A failed map has no GPUVA owner to retain its backing or remove its leaves.
    /// GPUVM has already split or removed overlaps throughout this operation's range.
    fn rollback_failed_map(&mut self, addr: u64, size: u64) {
        // Include the current segment: map_pages can fail after writing some of its leaves.
        // The checked GPUVM operation supplies an aligned range without overflow.
        let result = self.page_table.discard_partial_map(addr..addr + size);
        if self.uat_inner.firmware == UatFirmware::Handoff {
            if let Some(asid) = self.slot() {
                fence(Ordering::SeqCst);
                self.tlbi_range(asid as u8, addr, size as usize);
                mem::sync();
            }
        }
        self.tlbi_contexts_naming_root(addr, size as usize);
        if let Err(error) = result {
            dev_err!(
                self.dev.as_ref(),
                "Could not discard failed mapping: {:?}\n",
                error
            );
        }
    }

    fn mapping_mutation(&mut self) -> MappingMutation {
        let state = self.mapping_epoch.clone();
        if let Some(state) = state.as_ref() {
            state.writers.fetch_add(1, Ordering::Relaxed);
            state.visible.swap(u64::MAX, Ordering::AcqRel);
            self.epoch = self.epoch.and_then(|epoch| epoch.checked_add(1));
            state
                .pending
                .store(self.epoch.unwrap_or(u64::MAX), Ordering::Relaxed);
        }
        MappingMutation(state)
    }

    /// Returns the slot index, if this VM is bound.
    fn slot(&self) -> Option<u32> {
        if self.is_kernel {
            // The GFX ASC does not care about the ASID. Pick an arbitrary one.
            // TODO: This needs to be a persistently reserved ASID once we integrate
            // with the ARM64 kernel ASID machinery to avoid overlap.
            Some(0)
        } else {
            // We don't check whether we lost the slot, which could cause unnecessary
            // invalidations against another Vm. However, this situation should be very
            // rare (e.g. a Vm lost its slot, which means 63 other Vms bound in the
            // interim, and then it gets killed / drops its mappings without doing any
            // final rendering). Anything doing active maps/unmaps is probably also
            // rendering and therefore likely bound.
            self.binding
                .lock()
                .bind_token
                .as_ref()
                .map(|token| token.last_slot() + UAT_USER_CTX_START as u32)
        }
    }

    /// Returns the translation table base for this Vm
    fn ttb(&self) -> u64 {
        self.page_table.ttb()
    }

    /// Invalidate a VA range of this Vm's address space in the TLB of one ASID.
    fn tlbi_range(&self, asid: u8, iova: u64, size: usize) {
        match self.uat_inner.firmware {
            UatFirmware::Handoff => mem::tlbi_range(asid, iova as usize, size),
            UatFirmware::PublishedRoots { .. } => {
                mem::tlbi_range_or_asid(asid, iova as usize, size)
            }
        }
    }

    /// Invalidate a VA range in every TTBAT context that names this Vm's root as its lower half.
    ///
    /// With [`UatFirmware::PublishedRoots`], contexts other than a Vm's bind slot name its root
    /// (the kernel lower root sits in contexts 0 and 1 and has no slot at all), so a mapping
    /// change must reach all of them. Other firmware only uses the bind slot, which the callers
    /// already invalidate.
    fn tlbi_contexts_naming_root(&self, iova: u64, size: usize) {
        if self.uat_inner.firmware == UatFirmware::Handoff {
            return;
        }

        let contexts = self.uat_inner.lock().contexts_naming_root(self.ttb());
        if contexts == 0 {
            return;
        }

        fence(Ordering::SeqCst);
        for ctx in 0..UAT_NUM_CTX {
            if contexts & (1 << ctx) != 0 {
                mem::tlbi_range_or_asid(ctx as u8, iova as usize, size);
            }
        }
        mem::sync();
    }

    /// Map an `mm::Node` representing an mapping in VA space.
    fn map_node(&mut self, node: &mm::Node<(), KernelMappingInner>, prot: Prot) -> Result {
        let end = node
            .start()
            .checked_add(node.mapped_size as u64)
            .ok_or(EOVERFLOW)?;
        let _mutation = self.mapping_mutation();
        self.page_table.prepare_map(node.start()..end)?;
        let result = self.map_node_prepared(node, prot);
        if result.is_err() {
            self.rollback_failed_map(node.start(), node.mapped_size as u64);
        }
        result
    }

    /// Install only into the empty range reserved by `map_node`.
    fn map_node_prepared(&mut self, node: &mm::Node<(), KernelMappingInner>, prot: Prot) -> Result {
        let mut iova = node.start();
        let guard = node.bo.as_ref().ok_or(EINVAL)?.get()?.inner().inner.lock();
        let sgt = guard.sgt.as_ref().ok_or(EINVAL)?;
        let mut offset = node.offset;
        let mut left = node.mapped_size;

        for range in sgt.iter() {
            if left == 0 {
                break;
            }

            // TODO: proper DMA address/length handling
            let mut addr = range.dma_address() as usize;
            let mut len: usize = range.dma_len() as usize;

            if (offset | addr | len | iova as usize) & UAT_PGMSK != 0 {
                dev_err!(
                    self.dev.as_ref(),
                    "MMU: KernelMapping {:#x}:{:#x} -> {:#x} is not page-aligned\n",
                    addr,
                    len,
                    iova
                );
                return Err(EINVAL);
            }

            if offset > 0 {
                let skip = len.min(offset);
                addr += skip;
                len -= skip;
                offset -= skip;
            }

            len = len.min(left);

            if len == 0 {
                continue;
            }

            mod_dev_dbg!(
                self.dev,
                "MMU: map: {:#x}:{:#x} -> {:#x}\n",
                addr,
                len,
                iova
            );

            self.page_table.map_pages(
                iova..(iova + len as u64),
                addr as PhysicalAddr,
                prot,
                false,
            )?;

            iova += len as u64;
            left -= len;
        }

        // A context naming this root may be live and hold a negative translation for the range.
        self.tlbi_contexts_naming_root(node.start(), node.mapped_size);
        Ok(())
    }
}

/// Shared reference to a virtual memory address space ([`Vm`]).
#[derive(Clone)]
pub(crate) struct Vm {
    id: u64,
    mapping_epoch: Option<Arc<MappingEpoch>>,
    lifetime: Option<Arc<lifetime::VmLifetime>>,
    context_bindings: Option<Arc<context::ContextBindings>>,
    shared_bindings: Option<Arc<shared::SharedBindings>>,
    status: Option<Arc<crate::g17::status::VmStatus>>,
    inner: ARef<gpuvm::GpuVm<VmInner>>,
    dummy_obj: ARef<gem::Object>,
    binding: Arc<Mutex<VmBinding>>,
}
no_debug!(Vm);

/// An application TTBAT identity retained by a logical queue and its jobs.
/// The root lease is dropped before the VM, so no page table can be freed
/// while an active context still names it. Installed firmware queues retain
/// this object through replacement retirement or processor stop; a consumed
/// closed-scheduler release may return its root identity while keeping storage.
pub(crate) struct ExecutionContext {
    root: ExecutionRoot,
    vm: Vm,
}

struct ExecutionRoot {
    released: AtomicBool,
    inner: Arc<UatInner>,
    id: u8,
    generation: u8,
    low: u64,
    high: u64,
}

impl ExecutionContext {
    pub(crate) fn id(&self) -> u32 {
        self.root.id.into()
    }
    pub(crate) fn generation(&self) -> u8 {
        self.root.generation
    }
    pub(crate) fn vm(&self) -> &Vm {
        &self.vm
    }

    /// Called only after the closed scheduler's release was consumed and all
    /// its commands retired. Installed storage may outlive this root lease.
    pub(crate) fn release(&self) {
        self.root.release();
    }

    pub(crate) fn is_current(&self) -> bool {
        let inner = self.root.inner.lock();
        if self.root.released.load(Ordering::Acquire) {
            return false;
        }
        let roots = &inner.ttbs()[usize::from(self.root.id)];
        roots.ttb0.load(Ordering::Acquire) == self.root.low
            && roots.ttb1.load(Ordering::Acquire) == self.root.high
    }
}

impl ExecutionRoot {
    fn release(&self) {
        if self.released.swap(true, Ordering::AcqRel) {
            return;
        }
        let id = usize::from(self.id);
        let release = {
            let inner = self.inner.lock();
            inner.handoff().lock();
            let roots = &inner.ttbs()[id];
            let low = roots.ttb0.load(Ordering::Acquire);
            let high = roots.ttb1.load(Ordering::Acquire);
            let owned = low == self.low && high == self.high;
            if owned {
                roots.ttb0.store(0, Ordering::Release);
                roots.ttb1.store(0, Ordering::Release);
            }
            inner.handoff().unlock();
            owned || (low == 0 && high == 0)
        };
        if release {
            fence(Ordering::SeqCst);
            mem::tlbi_asid(self.id);
            mem::sync();
            self.inner.lock().execution_contexts &= !(1u64 << id);
        } else {
            pr_err!(
                "MMU: application context {} changed owners while leased\n",
                id
            );
        }
    }
}

impl Drop for ExecutionRoot {
    fn drop(&mut self) {
        self.release();
    }
}

/// Slot data for a [`Vm`] slot (nothing, we only care about the indices).
pub(crate) struct SlotInner();

impl slotalloc::SlotItem for SlotInner {
    type Data = ();
}

/// Represents a single user of a binding of a [`Vm`] to a slot.
///
/// The number of users is counted, and the slot will be freed when it drops to 0.
#[derive(Debug)]
pub(crate) struct VmBind(Vm, u32);

impl VmBind {
    /// Returns the slot that this `Vm` is bound to.
    pub(crate) fn slot(&self) -> u32 {
        self.1
    }
}

impl Drop for VmBind {
    fn drop(&mut self) {
        let mut binding = self.0.binding.lock();

        assert_ne!(binding.active_users, 0);
        binding.active_users -= 1;
        mod_pr_debug!(
            "MMU: slot {} active users {}\n",
            self.1,
            binding.active_users
        );
        if binding.active_users == 0 {
            binding.binding = None;
        }
    }
}

impl Clone for VmBind {
    fn clone(&self) -> VmBind {
        let mut binding = self.0.binding.lock();

        binding.active_users += 1;
        mod_pr_debug!(
            "MMU: slot {} active users {}\n",
            self.1,
            binding.active_users
        );
        VmBind(self.0.clone(), self.1)
    }
}

/// A mapping BO reference released under the immediate-mode GPUVA lock.
/// Retain the owner until deferred cleanup completes, including its final BO.
struct MappingBo {
    bo: Option<ARef<gpuvm::GpuVmBo<VmInner>>>,
    owner: ARef<gpuvm::GpuVm<VmInner>>,
}
impl MappingBo {
    fn new(
        bo: ARef<gpuvm::GpuVmBo<VmInner>>,
        owner: ARef<gpuvm::GpuVm<VmInner>>,
    ) -> Self {
        Self {
            bo: Some(bo),
            owner,
        }
    }
    fn get(&self) -> Result<&gpuvm::GpuVmBo<VmInner>> {
        self.bo.as_deref().ok_or(EINVAL)
    }
}
impl Drop for MappingBo {
    fn drop(&mut self) {
        let Some(bo) = self.bo.take() else {
            return;
        };
        let raw = ARef::into_raw(bo);
        // SAFETY: GpuVmBo is repr(C) with drm_gpuvm_bo first. into_raw
        // transfers one live reference. Every Vm here uses IMMEDIATE_MODE;
        // deferred put selects its GPUVA lock and retains cleanup ownership.
        unsafe {
            kernel::bindings::drm_gpuvm_bo_put_deferred(
                raw.as_ptr().cast::<kernel::bindings::drm_gpuvm_bo>(),
            )
        };
        self.owner.bo_deferred_cleanup();
    }
}

/// Inner data required for an object mapping into a [`Vm`].
pub(crate) struct KernelMappingInner {
    // Drop order matters:
    // - Deferred-put the GpuVmBo under its immediate-mode GPUVA lock
    // - Drop the GEM BO next, since BO free can take the resv lock itself
    // - Drop the owner GpuVm last, since that again can take resv locks when the refcount drops to 0
    bo: Option<MappingBo>,
    _gem: Option<ARef<gem::Object>>,
    owner: ARef<gpuvm::GpuVm<VmInner>>,
    uat_inner: Arc<UatInner>,
    prot: Prot,
    offset: usize,
    mapped_size: usize,
}

/// Prepared CPU timestamp access; destruction happens after completion signalling.
pub(crate) struct TimestampMapping {
    mapping: shmem::VMap<gem::AsahiObject, u8>,
    offset: usize,
}

impl TimestampMapping {
    pub(crate) fn write(&self, value: u64) {
        let map = self.mapping.get();
        for (index, byte) in value.to_le_bytes().into_iter().enumerate() {
            // SAFETY: timestamp_mapping checked the complete eight-byte extent.
            // The owned vmap pins its backing; byte atomics permit unaligned and
            // overlapping timestamp destinations without mutable slice aliases.
            let destination = unsafe {
                &*map.as_mut_ptr().add(self.offset + index).cast::<AtomicU8>()
            };
            destination.store(byte, Ordering::Relaxed);
        }
    }
}

/// An object mapping into a [`Vm`], which reserves the address range from use by other mappings.
pub(crate) struct KernelMapping(mm::Node<(), KernelMappingInner>);

impl KernelMapping {
    /// Returns the IOVA base of this mapping
    pub(crate) fn iova(&self) -> u64 {
        self.0.start()
    }

    /// Returns the size of this mapping in bytes
    pub(crate) fn size(&self) -> usize {
        self.0.mapped_size
    }

    /// Returns the IOVA base of this mapping
    pub(crate) fn iova_range(&self) -> Range<u64> {
        self.0.start()..(self.0.start() + self.0.mapped_size as u64)
    }

    /// Resolves a byte in this retained mapping without creating page tables.
    pub(crate) fn translate_offset(&self, offset: usize) -> Result<u64> {
        if offset >= self.size() {
            return Err(EINVAL);
        }
        let address = self.iova().checked_add(offset as u64).ok_or(EOVERFLOW)?;
        self.0.owner.exec_lock(None, false)?.page_table.translate_iova(address)
    }

    /// Writes a userspace timestamp through the retained GEM mapping. Byte
    /// stores support unaligned destinations and concurrent completion writes
    /// without creating mutable aliases. The complete timestamp is not atomic.
    /// Retain CPU access before this destination's completion fence is published.
    pub(crate) fn timestamp_mapping(&self, offset: usize) -> Result<TimestampMapping> {
        use kernel::drm::gem::BaseObject;
        let gem = self.0._gem.as_ref().ok_or(EINVAL)?;
        if offset.checked_add(size_of::<u64>()).ok_or(EOVERFLOW)? > self.size() {
            return Err(ERANGE);
        }
        let start = self.0.offset.checked_add(offset).ok_or(EOVERFLOW)?;
        let end = start.checked_add(size_of::<u64>()).ok_or(EOVERFLOW)?;
        if end > gem.size() {
            return Err(ERANGE);
        }
        Ok(TimestampMapping { mapping: gem.owned_vmap::<u8>()?, offset: start })
    }

    /// Gives a fresh kernel mapping a firmware-cached prefix, preserving its tail.
    /// The result includes whether any page-table entry may have changed; after
    /// mutation the caller must retain the mapping until processor stop even if
    /// the readback fails. The original protection is retained for teardown.
    ///
    /// # Safety
    /// The mapping must be unpublished. After mutation it must not be unmapped
    /// while a firmware instance can retain a cached translation or contents.
    pub(crate) unsafe fn set_fw_cached_prefix(&self, prefix: usize) -> (Result<[u64; 2]>, bool) {
        const MAX_PAGES: usize = 32;
        const ATTRIBUTE_SHIFT: u32 = 2;
        const ATTRIBUTE_MASK: u64 = 7;
        const UNCACHED: u64 = 2;
        const CACHE_ATTRIBUTE_CHANGE: u64 = UNCACHED << ATTRIBUTE_SHIFT;
        let mut mutated = false;
        let result = (|| {
            if self.0.uat_inner.firmware == UatFirmware::Handoff
                || prefix == 0
                || prefix > self.size()
                || (prefix | self.size()) & UAT_PGMSK != 0
                || self.size() > MAX_PAGES * UAT_PGSZ
            {
                return Err(EINVAL);
            }
            let mut owner = self.0.owner.exec_lock(None, false)?;
            if !owner.is_kernel {
                return Err(EINVAL);
            }
            let pages = self.size() / UAT_PGSZ;
            let cached_pages = prefix / UAT_PGSZ;
            let mut before = [0; MAX_PAGES];
            for (index, value) in before[..pages].iter_mut().enumerate() {
                *value = owner
                    .page_table
                    .leaf_entry(self.iova() + (index * UAT_PGSZ) as u64)?;
                if (*value >> ATTRIBUTE_SHIFT) & ATTRIBUTE_MASK != UNCACHED {
                    return Err(EINVAL);
                }
            }
            let _mutation = owner.mapping_mutation();
            mutated = true;
            owner.page_table.reprot_pages(
                self.iova()..self.iova() + prefix as u64,
                PROT_GPU_FW_PRIV_RW,
            )?;
            fence(Ordering::SeqCst);
            mem::tlbi_all();
            mem::sync();
            let mut after = [0; MAX_PAGES];
            for (index, value) in after[..pages].iter_mut().enumerate() {
                *value = owner
                    .page_table
                    .leaf_entry(self.iova() + (index * UAT_PGSZ) as u64)?;
                let change = if index < cached_pages {
                    CACHE_ATTRIBUTE_CHANGE
                } else {
                    0
                };
                if before[index] ^ *value != change {
                    return Err(EFAULT);
                }
            }
            Ok([after[0], after[cached_pages.min(pages - 1)]])
        })();
        (result, mutated)
    }

    /// Remap a cached mapping as uncached, then synchronously flush that range of VAs from the
    /// coprocessor cache. This is required to safely unmap cached/private mappings.
    fn remap_uncached_and_flush(&mut self) {
        let mut owner = self.0.owner.lock_inner();

        mod_dev_dbg!(
            owner.dev,
            "MMU: remap as uncached {:#x}:{:#x}\n",
            self.iova(),
            self.size()
        );

        // Remap in-place as uncached.
        // Do not try to unmap the guard page (-1)
        let prot = self.0.prot.as_uncached();
        let _mutation = owner.mapping_mutation();
        if owner
            .page_table
            .reprot_pages(self.iova_range(), prot)
            .is_err()
        {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: remap {:#x}:{:#x} failed\n",
                self.iova(),
                self.size()
            );
        }
        fence(Ordering::SeqCst);

        // If we don't have (and have never had) a VM slot, just return
        let slot = match owner.slot() {
            None => return,
            Some(slot) => slot,
        };

        let flush_slot = if owner.is_kernel {
            // If this is a kernel mapping, always flush on index 64
            UAT_NUM_CTX as u32
        } else {
            // Otherwise, check if this slot is the active one, otherwise return
            // Also check that we actually own this slot
            let ttb = owner.ttb() | TTBR_VALID | (slot as u64) << TTBR_ASID_SHIFT;

            let uat_inner = self.0.uat_inner.lock();
            uat_inner.handoff().lock();
            let cur_slot = uat_inner.handoff().current_slot();
            let ttb_cur = uat_inner.ttbs()[slot as usize].ttb0.load(Ordering::Relaxed);
            uat_inner.handoff().unlock();
            if cur_slot == Some(slot) && ttb_cur == ttb {
                slot
            } else {
                return;
            }
        };

        // FIXME: There is a race here, though it'll probably never happen in practice.
        // In theory, it's possible for the ASC to finish using our slot, whatever command
        // it was processing to complete, the slot to be lost to another context, and the ASC
        // to begin using it again with a different page table, thus faulting when it gets a
        // flush request here. In practice, the chance of this happening is probably vanishingly
        // small, as all 62 other slots would have to be recycled or in use before that slot can
        // be reused, and the ASC using user contexts at all is very rare.

        // Still, the locking around UAT/Handoff/TTBs should probably be redesigned to better
        // model the interactions with the firmware and avoid these races.
        // Possibly TTB changes should be tied to slot locks:

        // Flush:
        //  - Can early check handoff here (no need to lock).
        //      If user slot and it doesn't match the active ASC slot,
        //      we can elide the flush as the ASC guarantees it flushes
        //      TLBs/caches when it switches context. We just need a
        //      barrier to ensure ordering.
        //  - Lock TTB slot
        //      - If user ctx:
        //          - Lock handoff AP-side
        //              - Lock handoff dekker
        //                  - Check TTB & handoff cur ctx
        //      - Perform flush if necessary
        //          - This implies taking the fwring lock
        //
        // TTB change:
        //  - lock TTB slot
        //      - lock handoff AP-side
        //          - lock handoff dekker
        //              change TTB

        // Lock this flush slot, and write the range to it
        let flush = self.0.uat_inner.lock_flush(flush_slot);
        let pages = self.size() >> UAT_PGBIT;
        flush.begin_flush(self.iova(), self.size() as u64);
        if pages >= 0x10000 {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: Flush too big ({:#x} pages))\n",
                pages
            );
        }

        let cmd = fw::channels::FwCtlMsg {
            addr: fw::types::U64(self.iova()),
            unk_8: 0,
            slot: flush_slot,
            page_count: pages as u16,
            unk_12: 2, // ?
        };

        // Tell the firmware to do a cache flush
        let ret = match (*owner.dev).gpu.manager() {
            Some(gpu) => gpu.fwctl(cmd),
            None => Err(ENODEV),
        };
        if let Err(e) = ret {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: ASC cache flush {:#x}:{:#x} failed (err: {:?})\n",
                self.iova(),
                self.size(),
                e
            );
        }

        // Finish the flush
        flush.end_flush();

        // Slot is unlocked here
    }
}
no_debug!(KernelMapping);

impl Drop for KernelMapping {
    fn drop(&mut self) {
        // This is the main unmap function for UAT mappings.
        // The sequence of operations here is finicky, due to the interaction
        // between cached GFX ASC mappings and the page tables. These mappings
        // always have to be flushed from the cache before being unmapped.

        // For uncached mappings, just unmapping and flushing the TLB is sufficient.

        // For cached mappings, this is the required sequence:
        // 1. Remap it as uncached
        // 2. Flush the TLB range
        // 3. If kernel VA mapping OR user VA mapping and handoff.current_slot() == slot:
        //    a. Take a lock for this slot
        //    b. Write the flush range to the right context slot in handoff area
        //    c. Issue a cache invalidation request via FwCtl queue
        //    d. Poll for completion via queue
        //    e. Check for completion flag in the handoff area
        //    f. Drop the lock
        // 4. Unmap
        // 5. Flush the TLB range again

        // Firmware that does not use the handoff protocol cannot be asked to flush its cache.
        // Such firmware only caches kernel mappings that live until its coprocessors stop.
        if self.0.prot.is_cached_noncoherent() && self.0.uat_inner.firmware == UatFirmware::Handoff
        {
            mod_pr_debug!(
                "MMU: remap as uncached {:#x}:{:#x}\n",
                self.iova(),
                self.size()
            );
            self.remap_uncached_and_flush();
        }

        // Only VM page tables/binding state are touched here. The shared
        // reservation is sufficient and avoids fallible exec allocation in Drop.
        let mut owner = self.0.owner.lock_inner();
        mod_dev_dbg!(
            owner.dev,
            "MMU: unmap {:#x}:{:#x}\n",
            self.iova(),
            self.size()
        );

        let _mutation = owner.mapping_mutation();
        if owner.page_table.unmap_pages(self.iova_range()).is_err() {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: unmap {:#x}:{:#x} failed\n",
                self.iova(),
                self.size()
            );
        }

        if let Some(asid) = owner.slot() {
            fence(Ordering::SeqCst);
            owner.tlbi_range(asid as u8, self.iova(), self.size());
            mod_dev_dbg!(
                owner.dev,
                "MMU: flush range: asid={:#x} start={:#x} len={:#x}\n",
                asid,
                self.iova(),
                self.size()
            );
            mem::sync();
        }

        // The range returns to the VA allocator once this mapping is gone, so no context that
        // names this root may keep a stale translation for it.
        owner.tlbi_contexts_naming_root(self.iova(), self.size());
    }
}

/// Shared UAT global data structures
struct UatShared {
    kernel_ttb1: u64,
    execution_contexts: u64,
    context_generations: [u8; UAT_NUM_CTX],
    /// Kernel root of the secondary firmware instance, if there is one.
    secondary_ttb1: Option<PhysicalAddr>,
    map_kernel_to_user: bool,
    handoff_rgn: UatRegion,
    ttbs_rgn: UatRegion,
}

impl UatShared {
    /// Returns the handoff region area
    fn handoff(&self) -> &Handoff {
        // SAFETY: pointer is non-null per the type invariant
        unsafe { (self.handoff_rgn.map.ptr() as *mut Handoff).as_ref() }.unwrap()
    }

    /// Returns the TTBAT area
    fn ttbs(&self) -> &[SlotTTBS; UAT_NUM_CTX] {
        // SAFETY: pointer is non-null per the type invariant
        unsafe { (self.ttbs_rgn.map.ptr() as *mut [SlotTTBS; UAT_NUM_CTX]).as_ref() }.unwrap()
    }

    /// Returns the mask of TTBAT contexts whose lower half is the root `ttb`.
    fn contexts_naming_root(&self, ttb: u64) -> u64 {
        self.ttbs()
            .iter()
            .enumerate()
            .filter(|(ctx, slot)| slot.ttb0.load(Ordering::Acquire) == tagged_root(ttb, *ctx))
            .fold(0, |mask, (ctx, _)| mask | (1 << ctx))
    }
}

/// Returns the TTBAT value naming root `ttb` in context `ctx`.
const fn tagged_root(ttb: u64, ctx: usize) -> u64 {
    ttb | TTBR_VALID | (ctx as u64) << TTBR_ASID_SHIFT
}

// SAFETY: Nothing here is unsafe to send across threads.
unsafe impl Send for UatShared {}

/// Inner data for the top-level UAT instance.
#[pin_data]
struct UatInner {
    firmware: UatFirmware,
    #[pin]
    shared: Mutex<UatShared>,
    #[pin]
    handoff_flush: [Mutex<HandoffFlush>; UAT_NUM_CTX + 1],
}

impl UatInner {
    /// Take the lock on the shared data and return the guard.
    fn lock(&self) -> Guard<'_, UatShared, MutexBackend> {
        self.shared.lock()
    }

    /// Take a lock on a handoff flush slot and return the guard.
    fn lock_flush(&self, slot: u32) -> Guard<'_, HandoffFlush, MutexBackend> {
        self.handoff_flush[slot as usize].lock()
    }
}

/// Top-level UAT manager object
pub(crate) struct Uat {
    dev: driver::AsahiDevRef,
    geometry: UatGeometry,

    inner: Arc<UatInner>,
    slots: slotalloc::SlotAllocator<SlotInner>,

    kernel_vm: Vm,
    kernel_lower_vm: Vm,
}

impl Handoff {
    /// Lock the handoff region from firmware access
    fn lock(&self) {
        self.lock_ap.store(1, Ordering::Relaxed);
        fence(Ordering::SeqCst);

        while self.lock_fw.load(Ordering::Relaxed) != 0 {
            if self.turn.load(Ordering::Relaxed) != 0 {
                self.lock_ap.store(0, Ordering::Relaxed);
                while self.turn.load(Ordering::Relaxed) != 0 {}
                self.lock_ap.store(1, Ordering::Relaxed);
                fence(Ordering::SeqCst);
            }
        }
        fence(Ordering::Acquire);
    }

    /// Unlock the handoff region, allowing firmware access
    fn unlock(&self) {
        self.turn.store(1, Ordering::Relaxed);
        self.lock_ap.store(0, Ordering::Release);
    }

    /// Returns the current Vm slot mapped by the firmware for lower/unprivileged access, if any.
    fn current_slot(&self) -> Option<u32> {
        let slot = self.cur_slot.load(Ordering::Relaxed);
        if slot == 0 || slot == u32::MAX {
            None
        } else {
            Some(slot)
        }
    }

    /// Initialize the handoff region
    fn init(&self, firmware: UatFirmware) -> Result {
        self.magic_ap.store(PPL_MAGIC, Ordering::Relaxed);
        self.cur_slot.store(0, Ordering::Relaxed);
        self.unk3.store(0, Ordering::Relaxed);
        fence(Ordering::SeqCst);

        if firmware == UatFirmware::Handoff {
            self.wait_for_firmware()?;
        }

        for i in 0..=UAT_NUM_CTX {
            self.flush[i].state.store(0, Ordering::Relaxed);
            self.flush[i].addr.store(0, Ordering::Relaxed);
            self.flush[i].size.store(0, Ordering::Relaxed);
        }
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// Wait for the firmware to publish its half of the handoff region.
    fn wait_for_firmware(&self) -> Result {
        let start = Instant::<Monotonic>::now();
        const TIMEOUT: Delta = Delta::from_millis(1000);

        self.lock();
        while start.elapsed() < TIMEOUT {
            if self.magic_fw.load(Ordering::Relaxed) == PPL_MAGIC {
                break;
            } else {
                self.unlock();
                fsleep(Delta::from_millis(10));
                self.lock();
            }
        }

        if self.magic_fw.load(Ordering::Relaxed) != PPL_MAGIC {
            self.unlock();
            pr_err!("Handoff: Failed to initialize (firmware not running?)\n");
            return Err(EIO);
        }

        self.unlock();
        Ok(())
    }
}

/// Represents a single flush info slot in the handoff region.
///
/// # Invariants
/// The pointer is valid and there is no aliasing HandoffFlush instance.
struct HandoffFlush(*const FlushInfo);

// SAFETY: These pointers are safe to send across threads.
unsafe impl Send for HandoffFlush {}

impl HandoffFlush {
    /// Set up a flush operation for the coprocessor
    fn begin_flush(&self, start: u64, size: u64) {
        // SAFETY: Per the type invariant, this is safe
        let flush = unsafe { self.0.as_ref().unwrap() };

        let state = flush.state.load(Ordering::Relaxed);
        if state != 0 {
            pr_err!("Handoff: expected flush state 0, got {}\n", state);
        }
        flush.addr.store(start, Ordering::Relaxed);
        flush.size.store(size, Ordering::Relaxed);
        flush.state.store(1, Ordering::Relaxed);
    }

    /// Complete a flush operation for the coprocessor
    fn end_flush(&self) {
        // SAFETY: Per the type invariant, this is safe
        let flush = unsafe { self.0.as_ref().unwrap() };
        let state = flush.state.load(Ordering::Relaxed);
        if state != 2 {
            pr_err!("Handoff: expected flush state 2, got {}\n", state);
        }
        flush.state.store(0, Ordering::Relaxed);
    }
}

impl Vm {
    /// Create a new virtual memory address space
    fn new(
        dev: &driver::AsahiDevice,
        uat_inner: Arc<UatInner>,
        kernel_range: Range<u64>,
        geometry: UatGeometry,
        ttb: Option<PhysicalAddr>,
        id: u64,
    ) -> Result<Vm> {
        let dummy_obj = gem::new_kernel_object(dev, UAT_PGSZ)?;
        let is_kernel = ttb.is_some();

        let page_table = if let Some(ttb) = ttb {
            UatPageTable::new_with_ttb(
                ttb,
                geometry.kernel_range(),
                geometry.ias,
                geometry.oas,
                uat_inner.firmware != UatFirmware::Handoff,
            )?
        } else {
            UatPageTable::new(geometry.ias, geometry.oas)?
        };

        let (va_range, gpuvm_range) = if is_kernel {
            (geometry.kernel_range(), kernel_range.clone())
        } else {
            let mut range = geometry.user_range();
            if uat_inner.firmware != UatFirmware::Handoff {
                range.start = 0;
            }
            (range, geometry.user_usable_range())
        };

        let mm = mm::Allocator::new(va_range.start, va_range.range(), ())?;

        let binding = Arc::pin_init(
            new_mutex!(
                VmBinding {
                    binding: None,
                    bind_token: None,
                    active_users: 0,
                    ttb: page_table.ttb(),
                },
                "VmBinding",
            ),
            GFP_KERNEL,
        )?;

        let mapping_epoch = if uat_inner.firmware != UatFirmware::Handoff {
            Some(Arc::new(MappingEpoch::new(), GFP_KERNEL)?)
        } else {
            None
        };
        let binding_clone = binding.clone();
        Ok(Vm {
            id,
            mapping_epoch: mapping_epoch.clone(),
            lifetime: None,
            context_bindings: None,
            shared_bindings: None,
            status: None,
            dummy_obj: dummy_obj.gem.clone(),
            inner: gpuvm::GpuVm::new(
                c_str!("Asahi::GpuVm"),
                // TODO: should we using DRM_GPUVM_RESV_PROTECTED as well?
                drm_gpuvm_flags_DRM_GPUVM_IMMEDIATE_MODE,
                dev,
                dummy_obj.gem.clone(),
                gpuvm_range,
                kernel_range,
                init!(VmInner {
                    dev: dev.into(),
                    va_range,
                    is_kernel,
                    page_table,
                    mapping_epoch,
                    epoch: Some(0),
                    mm,
                    uat_inner,
                    binding: binding_clone,
                    id,
                }),
            )?,
            binding,
        })
    }

    /// Attach a status before sharing a newly created user VM with queues.
    pub(crate) fn with_status(mut self) -> Result<Self> {
        self.status = Some(Arc::new(crate::g17::status::VmStatus::new(), GFP_KERNEL)?);
        self.lifetime = Some(lifetime::VmLifetime::new()?);
        self.context_bindings = Some(context::ContextBindings::new()?);
        self.shared_bindings = Some(shared::SharedBindings::new()?);
        Ok(self)
    }

    /// Status shared by every queue using this VM, if enabled by its GPU.
    pub(crate) fn status(&self) -> Option<&Arc<crate::g17::status::VmStatus>> {
        self.status.as_ref()
    }

    fn covers_range_locked(
        inner: &mut VmInner,
        address: u64,
        size: u64,
        read: bool,
        write: bool,
    ) -> bool {
        if size == 0 {
            return false;
        }
        let Some(end) = address.checked_add(size) else {
            return false;
        };
        let mask = UAT_PGMSK as u64;
        let start = address & !mask;
        let end = if end & mask == 0 {
            end
        } else {
            let Some(end) = (end | mask).checked_add(1) else {
                return false;
            };
            end
        };
        if start < inner.va_range.start || end > inner.va_range.end {
            return false;
        }
        inner
            .page_table
            .covers_range(start..end, read, write)
            .unwrap_or(false)
    }

    pub(crate) fn covers_range(&self, address: u64, size: u64, read: bool, write: bool) -> bool {
        let Ok(mut inner) = self.inner.exec_lock(None, false) else {
            return false;
        };
        Self::covers_range_locked(&mut inner, address, size, read, write)
    }

    /// Checks all writable sparse blocks and their guard coverage under one
    /// GPUVM lock. A fully mapped guard is not a valid sparse allocation.
    pub(crate) fn covers_sparse_blocks_with_guards(
        &self,
        addresses: &[u64],
        block_size: u64,
        stride: u64,
    ) -> bool {
        if stride <= block_size {
            return false;
        }
        let Ok(mut inner) = self.inner.exec_lock(None, false) else {
            return false;
        };
        addresses.iter().all(|&address| {
            let Some(guard) = address.checked_add(block_size) else {
                return false;
            };
            Self::covers_range_locked(&mut inner, address, block_size, true, true)
                && !Self::covers_range_locked(
                    &mut inner, guard, stride - block_size, false, false,
                )
        })
    }

    /// Checks all exact request tuples under one GPUVM exec lock. Access bit0
    /// requests reading and bit1 writing. The epoch names this same snapshot.
    pub(crate) fn covers_ranges_batch(&self, ranges: &[(u64, u64, u8)]) -> (bool, Option<u64>) {
        let Ok(mut inner) = self.inner.exec_lock(None, false) else {
            return (false, None);
        };
        let epoch = inner.epoch;
        let covered = ranges.iter().all(|&(address, size, access)| {
            Self::covers_range_locked(&mut inner, address, size, access & 1 != 0, access & 2 != 0)
        });
        (covered, epoch)
    }

    /// Returns None while a leaf mutation is in progress or after epoch overflow.
    pub(crate) fn mapping_validation_epoch(&self) -> Option<u64> {
        self.mapping_epoch.as_ref()?.snapshot()
    }

    /// Get the translation table base for this Vm
    fn ttb(&self) -> u64 {
        self.binding.lock().ttb
    }

    /// Map a GEM object (using its `SGTable`) into this Vm at a free address in a given range.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn map_in_range(
        &self,
        gem: &gem::Object,
        object_range: Range<usize>,
        alignment: u64,
        range: Range<u64>,
        prot: Prot,
        guard: bool,
    ) -> Result<KernelMapping> {
        self.map_in_range_with_guard_size(
            gem,
            object_range,
            alignment,
            range,
            prot,
            if guard { UAT_PGSZ } else { 0 },
        )
    }

    /// Reserves a whole-page unmapped guard without adding it to the mapped backing.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn map_in_range_with_guard_size(
        &self,
        gem: &gem::Object,
        object_range: Range<usize>,
        alignment: u64,
        range: Range<u64>,
        prot: Prot,
        guard_size: usize,
    ) -> Result<KernelMapping> {
        use kernel::drm::gem::BaseObject;
        if guard_size % UAT_PGSZ != 0 {
            return Err(EINVAL);
        }
        let size = object_range
            .end
            .checked_sub(object_range.start)
            .ok_or(EINVAL)?;
        if size == 0 || object_range.end > gem.size() {
            return Err(EINVAL);
        }
        let reserved_size = size.checked_add(guard_size).ok_or(EOVERFLOW)?;
        let sgt = gem.owned_sg_table()?;
        let mut inner = self.inner.exec_lock(Some(gem), false)?;
        let vm_bo = self.inner.obtain_bo(gem)?;

        let mut vm_bo_guard = vm_bo.inner().inner.lock();
        if vm_bo_guard.sgt.is_none() {
            vm_bo_guard.sgt.replace(sgt);
        }
        core::mem::drop(vm_bo_guard);

        let mut vm_bo = Some(MappingBo::new(vm_bo, self.inner.clone()));
        let uat_inner = inner.uat_inner.clone();
        // A failed reservation drops its payload while exec still owns GEM
        // reservations. Attach BO/GEM references only after success, since
        // their destructors may acquire the same reservation locks.
        let result = inner.mm.insert_node_in_range(
            KernelMappingInner {
                owner: self.inner.clone(),
                uat_inner,
                prot,
                bo: None,
                _gem: None,
                offset: object_range.start,
                mapped_size: size,
            },
            reserved_size as u64,
            alignment,
            0,
            range.start,
            range.end,
            mm::InsertMode::Best,
        );
        let mut node = match result {
            Ok(node) => node,
            Err(error) => {
                drop(inner);
                return Err(error);
            }
        };
        {
            let payload = node.as_mut().inner_mut();
            payload.bo = vm_bo.take();
            payload._gem = Some(gem.into());
        }

        let ret = inner.map_node(&node, prot);
        // Drop the exec_lock first, so that if map_node failed the
        // KernelMappingInner destructur does not deadlock.
        core::mem::drop(inner);
        ret?;
        Ok(KernelMapping(node))
    }

    /// Map a GEM object into this Vm at a specific address.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn map_at(
        &self,
        addr: u64,
        size: usize,
        gem: ARef<gem::Object>,
        prot: Prot,
        guard: bool,
    ) -> Result<KernelMapping> {
        let reserved_size = size
            .checked_add(if guard { UAT_PGSZ } else { 0 })
            .ok_or(EOVERFLOW)?;
        let sgt = gem.owned_sg_table()?;
        let mut inner = self.inner.exec_lock(Some(&gem), false)?;

        let vm_bo = self.inner.obtain_bo(&gem)?;

        let mut vm_bo_guard = vm_bo.inner().inner.lock();
        if vm_bo_guard.sgt.is_none() {
            vm_bo_guard.sgt.replace(sgt);
        }
        core::mem::drop(vm_bo_guard);

        let mut vm_bo = Some(MappingBo::new(vm_bo, self.inner.clone()));
        let uat_inner = inner.uat_inner.clone();
        // Keep reservation-locking destructors out of the allocator payload
        // until reservation succeeds.
        let result = inner.mm.reserve_node(
            KernelMappingInner {
                owner: self.inner.clone(),
                uat_inner,
                prot,
                bo: None,
                _gem: None,
                offset: 0,
                mapped_size: size,
            },
            addr,
            reserved_size as u64,
            0,
        );
        let mut node = match result {
            Ok(node) => node,
            Err(error) => {
                drop(inner);
                return Err(error);
            }
        };
        {
            let payload = node.as_mut().inner_mut();
            payload.bo = vm_bo.take();
            payload._gem = Some(gem.clone());
        }

        let ret = inner.map_node(&node, prot);
        // Drop the exec_lock first, so that if map_node failed the
        // KernelMappingInner destructur does not deadlock.
        core::mem::drop(inner);
        ret?;
        Ok(KernelMapping(node))
    }

    /// Map a range of a GEM object into this Vm using GPUVM.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn bind_object(
        &self,
        gem: &gem::Object,
        addr: u64,
        size: u64,
        offset: u64,
        prot: Prot,
        single_page: bool,
    ) -> Result {
        gem::validate_vm_binding(gem, self)?;
        self.wait_for_user_map_admission()?;
        // Mapping needs a complete context
        let mut ctx = StepContext {
            new_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            prev_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            next_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            prot,
            ..Default::default()
        };

        let vm_bo = self.inner.obtain_bo(gem)?;
        {
            let needs_sgt = vm_bo.inner().inner.lock().sgt.is_none();
            if needs_sgt {
                // Pin without the BO mutex: alias construction can hold the GEM
                // reservation while acquiring this mutex.
                let sgt = gem.owned_sg_table()?;
                let mut vm_bo_guard = vm_bo.inner().inner.lock();
                if vm_bo_guard.sgt.is_none() {
                    if vm_bo_guard.sg_vec.is_none() {
                        let mut sg_vec = KVVec::new();
                        let mut offset = 0;
                        for range in sgt.iter() {
                            let addr = range.dma_address() as usize;
                            let len = range.dma_len() as usize;
                            sg_vec.push((offset, addr..(addr + len)), GFP_KERNEL)?;
                            offset += len;
                        }
                        vm_bo_guard.sg_vec.replace(sg_vec);
                    }
                    vm_bo_guard.sgt.replace(sgt);
                }
                // Release the mutex before a losing initializer drops its SG
                // owner. The declaration order also preserves this on errors.
                drop(vm_bo_guard);
            }
        }

        let mut inner = self.inner.exec_lock(Some(gem), true)?;

        // Preallocate the page tables, to fail early if we ENOMEM
        inner.page_table.alloc_pages(addr..(addr + size))?;

        ctx.vm_bo = Some(vm_bo);

        if (addr | size | offset) & (UAT_PGMSK as u64) != 0 {
            dev_err!(
                inner.dev.as_ref(),
                "MMU: Map step {:#x} [{:#x}] -> {:#x} is not page-aligned\n",
                offset,
                size,
                addr
            );
            return Err(EINVAL);
        }

        let (flags, gem_range) = if single_page {
            (gpuvm::GpuVaFlags::REPEAT, UAT_PGSZ as u32)
        } else {
            (gpuvm::GpuVaFlags::NONE, 0u32)
        };

        mod_dev_dbg!(
            inner.dev,
            "MMU: sm_map: {:#x} [{:#x}] -> {:#x}\n",
            offset,
            size,
            addr
        );
        let _commit = self.mapping_commit()?;
        inner.sm_map(&mut ctx, addr, size, offset, gem_range, flags)
    }

    /// Add a direct MMIO mapping to this Vm at a free address.
    pub(crate) fn map_io(
        &self,
        iova: u64,
        phys: usize,
        size: usize,
        prot: Prot,
    ) -> Result<KernelMapping> {
        let mut inner = self.inner.exec_lock(None, false)?;

        if (iova as usize | phys | size) & UAT_PGMSK != 0 {
            dev_err!(
                inner.dev.as_ref(),
                "MMU: KernelMapping {:#x}:{:#x} -> {:#x} is not page-aligned\n",
                phys,
                size,
                iova
            );
            return Err(EINVAL);
        }

        dev_info!(
            inner.dev.as_ref(),
            "MMU: IO map: {:#x}:{:#x} -> {:#x}\n",
            phys,
            size,
            iova
        );

        let uat_inner = inner.uat_inner.clone();
        let node = inner.mm.reserve_node(
            KernelMappingInner {
                owner: self.inner.clone(),
                uat_inner,
                prot,
                bo: None,
                _gem: None,
                offset: 0,
                mapped_size: size,
            },
            iova,
            size as u64,
            0,
        )?;

        let end = iova.checked_add(size as u64).ok_or(EOVERFLOW)?;
        inner.page_table.prepare_map(iova..end)?;
        let mutation = inner.mapping_mutation();
        let ret = inner.page_table.map_pages(
            iova..end,
            phys as PhysicalAddr,
            prot,
            false,
        );
        if ret.is_err() {
            inner.rollback_failed_map(iova, size as u64);
        }
        // Drop the exec_lock first, so that if map_node failed the
        // KernelMappingInner destructur does not deadlock.
        drop(mutation);
        core::mem::drop(inner);
        ret?;
        Ok(KernelMapping(node))
    }

    /// Unmap everything in an address range.
    pub(crate) fn unmap_range(&self, iova: u64, size: u64) -> Result {
        if self.defer_unmap(iova, size)? {
            return Ok(());
        }
        self.unmap_range_commit(iova, size, true)
    }

    fn unmap_range_now(&self, iova: u64, size: u64) -> Result {
        self.unmap_range_commit(iova, size, false)
    }

    fn unmap_range_commit(&self, iova: u64, size: u64, user: bool) -> Result {
        // Unmapping a range can only do a single split, so just preallocate
        // the prev and next GpuVas
        let mut ctx = StepContext {
            prev_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            next_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            ..Default::default()
        };

        let mut inner = self.inner.exec_lock(None, false)?;

        mod_dev_dbg!(inner.dev, "MMU: sm_unmap: {:#x}:{:#x}\n", iova, size);
        let _commit = if user {
            match self.mapping_commit() {
                Ok(guard) => guard,
                Err(error) => {
                    if self.defer_unmap(iova, size)? {
                        return Ok(());
                    }
                    return Err(error);
                }
            }
        } else {
            None
        };
        inner.sm_unmap(&mut ctx, iova, size)?;
        self.untrack_shared_range(iova, size);
        Ok(())
    }

    fn drop_mappings_now(&self, gem: &gem::Object) -> Result {
        // Removing whole mappings only does unmaps, so no preallocated VAs
        let mut ctx = Default::default();

        let inner = self.inner.exec_lock(Some(gem), false)?;

        if let Some(bo) = self.inner.find_bo(gem) {
            mod_dev_dbg!(inner.dev, "MMU: bo_unmap\n");
            let result = self.inner.bo_unmap(&mut ctx, &bo);
            if result.is_ok() {
                // The close callback or deferred object list retains this GEM,
                // so clearing its candidate cannot release its final reference.
                self.untrack_context_object(gem);
                self.untrack_shared_object(gem);
            }
            mod_dev_dbg!(inner.dev, "MMU: bo_unmap done\n");
            // BO release can drain unrelated deferred GEMs, including on an
            // unmap error. Always release reservations before dropping it.
            core::mem::drop(inner);
            core::mem::drop(bo);
            result?;
        }

        Ok(())
    }

    /// Returns the dummy GEM object used to hold the shared DMA reservation locks
    pub(crate) fn get_resv_obj(&self) -> ARef<gem::Object> {
        self.dummy_obj.clone()
    }

    /// Check whether an object is external to this GpuVm
    pub(crate) fn is_extobj(&self, gem: &gem::Object) -> bool {
        self.inner.is_extobj(gem)
    }

    /// Check whether an object is external to this GpuVm
    pub(crate) fn bo_deferred_cleanup(&self) {
        self.inner.bo_deferred_cleanup()
    }
}

impl Drop for VmInner {
    fn drop(&mut self) {
        let mut binding = self.binding.lock();
        assert_eq!(binding.active_users, 0);

        mod_pr_debug!(
            "VmInner::Drop [{}]: bind_token={:?}\n",
            self.id,
            binding.bind_token
        );

        // Make sure this VM is not mapped to a TTB if it was
        if let Some(token) = binding.bind_token.take() {
            let idx = (token.last_slot() as usize) + UAT_USER_CTX_START;
            let ttb = self.ttb() | TTBR_VALID | (idx as u64) << TTBR_ASID_SHIFT;

            let uat_inner = self.uat_inner.lock();
            uat_inner.handoff().lock();
            let handoff_cur = uat_inner.handoff().current_slot();
            let ttb_cur = uat_inner.ttbs()[idx].ttb0.load(Ordering::SeqCst);
            let inval = ttb_cur == ttb;
            if inval {
                if handoff_cur == Some(idx as u32) {
                    pr_err!(
                        "VmInner::drop owning slot {}, but it is currently in use by the ASC?\n",
                        idx
                    );
                }
                uat_inner.ttbs()[idx].ttb0.store(0, Ordering::SeqCst);
                uat_inner.ttbs()[idx].ttb1.store(0, Ordering::SeqCst);
            }
            uat_inner.handoff().unlock();
            core::mem::drop(uat_inner);

            // In principle we dropped all the KernelMappings already, but we might as
            // well play it safe and invalidate the whole ASID.
            if inval {
                mod_pr_debug!(
                    "VmInner::Drop [{}]: need inval for ASID {:#x}\n",
                    self.id,
                    idx
                );
                mem::tlbi_asid(idx as u8);
                mem::sync();
            }
        }
    }
}

impl Uat {
    /// Map a bootloader-preallocated memory region
    fn map_region(
        dev: &device::Device,
        name: &CStr,
        size: usize,
        cached: bool,
    ) -> Result<UatRegion> {
        let of_node = dev.of_node().ok_or(EINVAL)?;
        let res = of_node.reserved_mem_region_to_resource_byname(name)?;
        let base = res.start();
        let res_size = res.size().try_into()?;

        if size > res_size {
            dev_err!(
                dev,
                "Region {} is too small (expected {}, got {})\n",
                name,
                size,
                res_size
            );
            return Err(ENOMEM);
        }

        let flags = if cached {
            io::mem::MemFlag::WB
        } else {
            io::mem::MemFlag::WC
        };

        // SAFETY: The safety of this operation hinges on the correctness of
        // much of this file and also the `pgtable` module, so it is difficult
        // to prove in a single safety comment. Such is life with raw GPU
        // page table management...
        let map = unsafe { io::mem::Mem::try_new(res, flags.into()) }.inspect_err(|_| {
            dev_err!(dev, "Failed to remap {} mem resource\n", name);
        })?;

        Ok(UatRegion { base, map })
    }

    /// Returns a reference to the global kernel (upper half) `Vm`
    pub(crate) fn kernel_vm(&self) -> &Vm {
        &self.kernel_vm
    }

    /// Returns a reference to the local kernel (lower half) `Vm`
    pub(crate) fn kernel_lower_vm(&self) -> &Vm {
        &self.kernel_lower_vm
    }

    /// Installed descriptor and bind contexts rooted in this user VM. A graph
    /// constructor checks this before changing a retained pool's mappings.
    pub(crate) fn vm_context_mask(&self, vm: &Vm) -> Result<u64> {
        if !matches!(self.inner.firmware, UatFirmware::PublishedRoots { .. }) {
            return Err(EINVAL);
        }
        let root = {
            let owner = vm.inner.exec_lock(None, false)?;
            if owner.is_kernel || !Arc::ptr_eq(&owner.uat_inner, &self.inner) {
                return Err(EINVAL);
            }
            owner.ttb()
        };
        Ok(self.inner.lock().contexts_naming_root(root))
    }

    /// Publish a completed alias set before its descriptors become visible.
    /// Match actual roots, including independently allocated execution contexts.
    pub(crate) fn flush_vm_contexts(&self, vm: &Vm) -> Result<u64> {
        let contexts = self.vm_context_mask(vm)?;
        if contexts == 0 {
            return Err(ENOENT);
        }
        fence(Ordering::SeqCst);
        for context in 0..UAT_NUM_CTX {
            if contexts & (1u64 << context) != 0 {
                mem::tlbi_asid(context as u8);
            }
        }
        mem::sync();
        Ok(contexts)
    }

    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn dump_kernel_pages(&self) -> Result<KVVec<pgtable::DumpedPage>> {
        let mut inner = self.kernel_vm.inner.exec_lock(None, false)?;
        inner.page_table.dump_pages(self.geometry.upper_range())
    }

    /// Returns the base physical address of the TTBAT region.
    pub(crate) fn ttb_base(&self) -> u64 {
        let inner = self.inner.lock();

        inner.ttbs_rgn.base
    }

    /// Binds a `Vm` to a slot, preferring the last used one.
    pub(crate) fn bind(&self, vm: &Vm) -> Result<VmBind> {
        let mut binding = vm.binding.lock();

        if binding.binding.is_none() {
            assert_eq!(binding.active_users, 0);

            let isolation = *module_parameters::robust_isolation.value() != 0;

            self.slots.set_limit(if isolation {
                NonZeroUsize::new(1)
            } else {
                None
            });

            let slot = self.slots.get(binding.bind_token)?;
            if slot.changed() {
                mod_pr_debug!("Vm Bind [{}]: bind_token={:?}\n", vm.id, slot.token(),);
                let idx = (slot.slot() as usize) + UAT_USER_CTX_START;
                let ttb = binding.ttb | TTBR_VALID | (idx as u64) << TTBR_ASID_SHIFT;

                let uat_inner = self.inner.lock();

                let ttb1 = if uat_inner.map_kernel_to_user {
                    uat_inner.kernel_ttb1 | TTBR_VALID | (idx as u64) << TTBR_ASID_SHIFT
                } else {
                    0
                };

                let ttbs = uat_inner.ttbs();
                uat_inner.handoff().lock();
                if uat_inner.handoff().current_slot() == Some(idx as u32) {
                    pr_err!(
                        "Vm::bind to slot {}, but it is currently in use by the ASC?\n",
                        idx
                    );
                }
                ttbs[idx].ttb0.store(ttb, Ordering::Release);
                ttbs[idx].ttb1.store(ttb1, Ordering::Release);
                uat_inner.handoff().unlock();
                core::mem::drop(uat_inner);

                // Make sure all TLB entries from the previous owner of this ASID are gone
                mem::tlbi_asid(idx as u8);
                mem::sync();
            }

            binding.bind_token = Some(slot.token());
            binding.binding = Some(slot);
        }

        binding.active_users += 1;

        let slot = binding.binding.as_ref().unwrap().slot() + UAT_USER_CTX_START as u32;
        mod_pr_debug!("MMU: slot {} active users {}\n", slot, binding.active_users);
        Ok(VmBind(vm.clone(), slot))
    }

    /// Allocates a stable application context without changing the VM bind slot.
    /// Both roots and the generation remain unchanged for the lease's lifetime.
    pub(crate) fn new_execution_context(&self, vm: &Vm) -> Result<Arc<ExecutionContext>> {
        if self.inner.firmware == UatFirmware::Handoff {
            return Err(EINVAL);
        }
        {
            let owner = vm.inner.exec_lock(None, false)?;
            if owner.is_kernel
                || !core::ptr::eq(Arc::as_ptr(&owner.uat_inner), Arc::as_ptr(&self.inner))
            {
                return Err(EINVAL);
            }
        }
        let (id, generation) = {
            let mut inner = self.inner.lock();
            let free = !inner.execution_contexts & EXECUTION_CONTEXT_MASK;
            if free == 0 {
                return Err(ENOSPC);
            }
            let id = free.trailing_zeros() as usize;
            inner.execution_contexts |= 1u64 << id;
            inner.context_generations[id] = inner.context_generations[id].wrapping_add(1);
            (id, inner.context_generations[id])
        };
        let low = tagged_root(vm.ttb(), id);
        let high = tagged_root(self.kernel_vm.ttb(), id);
        // Allocate the complete host owner before either TTBAT half is published.
        // ExecutionRoot releases an empty claimed slot if allocation fails.
        let context = Arc::new(
            ExecutionContext {
                root: ExecutionRoot {
                    released: AtomicBool::new(false),
                    inner: self.inner.clone(),
                    id: id as u8,
                    generation,
                    low,
                    high,
                },
                vm: vm.clone(),
            },
            GFP_KERNEL,
        )?;
        {
            let inner = self.inner.lock();
            let roots = &inner.ttbs()[id];
            if roots.ttb0.load(Ordering::Acquire) != 0 || roots.ttb1.load(Ordering::Acquire) != 0 {
                return Err(EBUSY);
            }
            roots.ttb0.store(low, Ordering::Release);
            roots.ttb1.store(high, Ordering::Release);
        }
        fence(Ordering::SeqCst);
        mem::tlbi_asid(id as u8);
        mem::sync();
        Ok(context)
    }

    /// Creates a new `Vm` linked to this UAT.
    pub(crate) fn new_vm(&self, id: u64, kernel_range: Range<u64>) -> Result<Vm> {
        Vm::new(
            &self.dev,
            self.inner.clone(),
            kernel_range,
            self.geometry,
            None,
            id,
        )
    }

    /// Returns the address space geometry of this UAT.
    pub(crate) fn geometry(&self) -> UatGeometry {
        self.geometry
    }

    /// Adopts the top-level entries that the primary firmware instance published in the kernel
    /// root while booting ([`UatFirmware::PublishedRoots`]).
    ///
    /// The entries below the kernel window belong to the firmware, and the kernel window entry
    /// points at a table that the bootloader reserved, so the driver maps kernel objects beneath
    /// it and never frees it. Entries above the kernel window do not belong to anyone and are
    /// cleared. Must run after the primary instance booted and before anything is mapped into the
    /// kernel `Vm`.
    pub(crate) fn adopt_firmware_root_entries(&self) -> Result<[u64; 3]> {
        let owned = IOVA_KERN_TOP_LEVEL_INDEX as usize + 1;
        let mut snapshot = [0; 3];
        // SAFETY: The kernel root is the bootloader-reserved `pagetables` region, which outlives
        // this `Uat`.
        unsafe {
            pgtable::with_root_entries(self.kernel_vm.ttb(), UAT_PGSZ / size_of::<u64>(), |ptes| {
                for (value, pte) in snapshot.iter_mut().zip(ptes) {
                    *value = pte.load(Ordering::Relaxed);
                }
                for pte in &ptes[owned..] {
                    pte.store(0, Ordering::Relaxed);
                }
            })
        }?;

        fence(Ordering::SeqCst);
        mem::tlbi_all();
        mem::sync();
        Ok(snapshot)
    }

    /// Checks that allocation under the shared subtree did not replace a firmware-owned entry.
    pub(crate) fn confirm_firmware_root_entries(&self, snapshot: [u64; 3]) -> Result {
        // SAFETY: The root is a bootloader-reserved page retained for this UAT's lifetime.
        let matches = unsafe {
            pgtable::with_root_entries(self.kernel_vm.ttb(), snapshot.len(), |ptes| {
                ptes.iter()
                    .zip(snapshot)
                    .all(|(pte, expected)| pte.load(Ordering::Relaxed) == expected)
            })
        }?;
        if !matches {
            return Err(EIO);
        }
        Ok(())
    }

    /// Copies the kernel root's top-level entries from the kernel window up into the root of the
    /// secondary firmware instance ([`UatFirmware::PublishedRoots`]), whose lower entries are
    /// private to that instance.
    pub(crate) fn mirror_secondary_root(&self) -> Result {
        let secondary = self.inner.lock().secondary_ttb1.ok_or(EINVAL)?;
        let count = UAT_PGSZ / size_of::<u64>();
        let first = IOVA_KERN_TOP_LEVEL_INDEX as usize;

        // SAFETY: Both roots are in the bootloader-reserved `pagetables` region, which outlives
        // this `Uat`.
        unsafe {
            pgtable::with_root_entries(self.kernel_vm.ttb(), count, |primary| {
                pgtable::with_root_entries(secondary, count, |secondary| {
                    for (src, dst) in primary.iter().zip(secondary).skip(first) {
                        let pte = src.load(Ordering::Relaxed);
                        if pte != 0 && dst.load(Ordering::Relaxed) != pte {
                            dst.store(pte, Ordering::Relaxed);
                        }
                    }
                })
            })
        }??;

        fence(Ordering::SeqCst);
        mem::tlbi_all();
        mem::sync();
        Ok(())
    }

    /// Activates the device-global render root in context 1 after the boot records retire.
    /// Both contexts must still contain their original kernel roots.
    pub(crate) fn activate_render_context(&self, vm: &Vm) -> Result {
        let inner = self.inner.lock();
        inner.handoff().lock();
        let ttbs = inner.ttbs();
        let ready = [0, KERNEL_ALIAS_CTX].into_iter().all(|ctx| {
            ttbs[ctx].ttb0.load(Ordering::Acquire) == tagged_root(self.kernel_lower_vm.ttb(), ctx)
                && ttbs[ctx].ttb1.load(Ordering::Acquire) == tagged_root(self.kernel_vm.ttb(), ctx)
        });
        if !ready {
            inner.handoff().unlock();
            return Err(EIO);
        }
        ttbs[KERNEL_ALIAS_CTX]
            .ttb0
            .store(tagged_root(vm.ttb(), KERNEL_ALIAS_CTX), Ordering::Release);
        inner.handoff().unlock();
        core::mem::drop(inner);
        fence(Ordering::SeqCst);
        mem::tlbi_asid(KERNEL_ALIAS_CTX as u8);
        mem::sync();
        Ok(())
    }

    /// Creates the reference-counted inner data for a new `Uat` instance.
    #[inline(never)]
    fn make_inner(dev: &driver::AsahiDevice, firmware: UatFirmware) -> Result<Arc<UatInner>> {
        let handoff_rgn = Self::map_region(dev.as_ref(), c_str!("handoff"), HANDOFF_SIZE, true)?;
        let ttbs_rgn = Self::map_region(dev.as_ref(), c_str!("ttbs"), SLOTS_SIZE, true)?;

        // SAFETY: The Handoff struct layout matches the firmware's view of memory at this address,
        // and the region is at least large enough per the size specified above.
        let handoff = unsafe { &(handoff_rgn.map.ptr() as *mut Handoff).as_ref().unwrap() };

        dev_info!(dev.as_ref(), "MMU: Initializing kernel page table\n");

        Arc::pin_init(
            try_pin_init!(UatInner {
                firmware,
                handoff_flush <- pin_init::pin_init_array_from_fn(|i| {
                    new_mutex!(HandoffFlush(&handoff.flush[i]), "handoff_flush")
                }),
                shared <- new_mutex!(
                    UatShared {
                        kernel_ttb1: 0,
                        execution_contexts: !EXECUTION_CONTEXT_MASK,
                        context_generations: [0; UAT_NUM_CTX],
                        secondary_ttb1: None,
                        map_kernel_to_user: false,
                        handoff_rgn,
                        ttbs_rgn,
                    },
                    "uat_shared"
                ),
            }),
            GFP_KERNEL,
        )
    }

    /// Creates a new `Uat` instance for the given address space geometry and firmware.
    #[inline(never)]
    pub(crate) fn new(
        dev: &driver::AsahiDevice,
        geometry: UatGeometry,
        map_kernel_to_user: bool,
        firmware: UatFirmware,
    ) -> Result<Self> {
        dev_info!(dev.as_ref(), "MMU: Initializing...\n");

        let inner = Self::make_inner(dev, firmware)?;

        let of_node = dev.as_ref().of_node().ok_or(EINVAL)?;
        let res = of_node.reserved_mem_region_to_resource_byname(c_str!("pagetables"))?;
        let ttb1 = res.start();
        let ttb1size: usize = res.size().try_into()?;

        let (pagetables_size, secondary_ttb1) = match firmware {
            UatFirmware::Handoff => (PAGETABLES_SIZE, None),
            UatFirmware::PublishedRoots {
                secondary_root_offset,
            } => (
                secondary_root_offset as usize + UAT_PGSZ,
                Some(ttb1 + secondary_root_offset),
            ),
        };
        if ttb1size < pagetables_size {
            dev_err!(dev.as_ref(), "MMU: Pagetables region is too small\n");
            return Err(ENOMEM);
        }

        dev_info!(dev.as_ref(), "MMU: Creating kernel page tables\n");
        let kernel_lower_vm =
            Vm::new(dev, inner.clone(), geometry.user_range(), geometry, None, 1)?;
        let kernel_vm = Vm::new(
            dev,
            inner.clone(),
            geometry.kernel_range(),
            geometry,
            Some(ttb1),
            0,
        )?;

        dev_info!(dev.as_ref(), "MMU: Kernel page tables created\n");

        let ttb0 = kernel_lower_vm.ttb();

        let uat = Self {
            dev: dev.into(),
            geometry,
            kernel_vm,
            kernel_lower_vm,
            inner,
            slots: slotalloc::SlotAllocator::new(
                UAT_USER_CTX as u32,
                (),
                |_inner, _slot| Some(SlotInner()),
                c_str!("Uat::SlotAllocator"),
                static_lock_class!(),
                static_lock_class!(),
            )?,
        };

        let mut inner = uat.inner.lock();

        inner.map_kernel_to_user = map_kernel_to_user;
        inner.kernel_ttb1 = ttb1;
        inner.secondary_ttb1 = secondary_ttb1;

        inner.handoff().init(firmware)?;

        dev_info!(dev.as_ref(), "MMU: Initializing TTBs\n");

        inner.handoff().lock();

        let ttbs = inner.ttbs();

        ttbs[0].ttb0.store(ttb0 | TTBR_VALID, Ordering::SeqCst);
        ttbs[0].ttb1.store(ttb1 | TTBR_VALID, Ordering::SeqCst);

        for ctx in &ttbs[1..] {
            ctx.ttb0.store(0, Ordering::Relaxed);
            ctx.ttb1.store(0, Ordering::Relaxed);
        }

        if secondary_ttb1.is_some() {
            let alias = &ttbs[KERNEL_ALIAS_CTX];
            alias
                .ttb0
                .store(tagged_root(ttb0, KERNEL_ALIAS_CTX), Ordering::SeqCst);
            alias
                .ttb1
                .store(tagged_root(ttb1, KERNEL_ALIAS_CTX), Ordering::SeqCst);
        }

        inner.handoff().unlock();

        core::mem::drop(inner);

        dev_info!(dev.as_ref(), "MMU: initialized\n");

        Ok(uat)
    }
}

impl Drop for Uat {
    fn drop(&mut self) {
        // Make sure we flush the TLBs
        fence(Ordering::SeqCst);
        mem::tlbi_all();
        mem::sync();
    }
}
