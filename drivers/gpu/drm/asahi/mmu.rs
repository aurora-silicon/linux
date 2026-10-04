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

use core::fmt::Debug;
use core::mem::{size_of, ManuallyDrop};
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
    page::Page,
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
    hw,
    mem,
    pgtable,
    slotalloc,
    uat::UatGeometry,
    util::RangeExt, //
};

// KernelMapping protection types
pub(crate) use crate::pgtable::Prot;
pub(crate) use pgtable::prot::*;
pub(crate) use pgtable::{
    pte_describe,
    pte_is_normal_memory,
    pte_is_uncached,
    UatPageTable,
    UAT_PGBIT,
    UAT_PGMSK,
    UAT_PGSZ, //
};

use pin_init;

const DEBUG_CLASS: DebugFlags = DebugFlags::Mmu;

/// PPL magic number for the handoff region
const PPL_MAGIC: u64 = 0x4b1d000000000002;

/// Number of supported context entries in the TTBAT
const UAT_NUM_CTX: usize = 64;
/// First context available for users
const UAT_USER_CTX_START: usize = 1;
/// Number of available user contexts
const UAT_USER_CTX: usize = UAT_NUM_CTX - UAT_USER_CTX_START;
/// Lower/user base VA
pub(crate) const IOVA_USER_BASE: u64 = UAT_PGSZ as u64;
/// Lower/user top VA for the 39-bit (AGX2) roots. Geometry-aware paths must
/// use [`iova_user_range`]/[`iova_user_usable_range`] instead; this const
/// remains for the AGX2 initdata builder, which is 39-bit-only.
pub(crate) const IOVA_USER_TOP: u64 = 1 << 39;

const AGX3_FIRMWARE_HANDOFF_VALIDATED: bool = false;

/// The t6030 G15 14.8.3 firmware completes the AGX2-style Dekker handoff when the GpuManager
/// builds the UAT before starting it: it publishes `magic_fw` and serves flush requests
/// (observed on J516S).
const G15_DEKKER_HANDOFF_OBSERVED: bool = true;

/// How the firmware-shared UAT handoff region behaves for the target SoC.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum HandoffMode {
    FirmwareDekker,
    StoppedFirmwareT6030,
    /// J514S RTKit is running before the AP publishes its host mappings.
    FirmwareT6030,

}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
struct UatConfig {
    chip_id: u32,
    ias: u8,
    oas: u32,
    /// Kernel tables are the bootloader-reserved handoff tables (T6030 runtime owners);
    /// false for the HwConfig-driven GpuManager paths.
    reserved_tables: bool,
}

impl UatConfig {
    const fn from_hw(cfg: &hw::HwConfig) -> Self {
        Self {
            chip_id: cfg.chip_id,
            ias: cfg.uat_ias,
            oas: cfg.uat_oas,
            reserved_tables: false,
        }
    }

    const T6030: Self = Self { chip_id: 0x6030, ias: 42, oas: 42, reserved_tables: true };

}

fn uat_geometry(cfg: UatConfig) -> Result<UatGeometry> {
    // 39-bit (G13/G14) and 42-bit (G15+) roots are the two recovered
    // geometries; anything else is undefined.
    UatGeometry::new(cfg.ias).ok_or(ENODEV)
}

/// Lower/user VA range for this SoC's root geometry (one page up to the
/// per-root translation top: `1 << 39` on AGX2, `1 << 42` on AGX3).
#[allow(dead_code)]
pub(crate) fn iova_user_range(cfg: &hw::HwConfig) -> Result<Range<u64>> {
    Ok(IOVA_USER_BASE..uat_geometry(UatConfig::from_hw(cfg))?.user_va_top())
}

pub(crate) fn iova_unk_page(cfg: &hw::HwConfig) -> Result<u64> {
    Ok(uat_geometry(UatConfig::from_hw(cfg))?.user_va_top() - 2 * UAT_PGSZ as u64)
}

/// Usable lower/user VA range for this SoC (excludes the unknown page).
pub(crate) fn iova_user_usable_range(cfg: &hw::HwConfig) -> Result<Range<u64>> {
    Ok(IOVA_USER_BASE..iova_unk_page(cfg)?)
}

fn iova_kern_range(cfg: UatConfig) -> Result<Range<u64>> {
    let geometry = uat_geometry(cfg)?;
    Ok(geometry.kernel_va_base()..geometry.kernel_va_top())
}

/// Upper/kernel (TTBR1) base VA of the 39-bit (G13/G14) layout. The driver's `IOVA_KERN_*`
/// constants are expressed in this layout; [`kern_iova`] rebases them for the running GPU.
const IOVA_TTBR1_BASE_39: u64 = 0xffffff8000000000;

/// Returns the upper/kernel (TTBR1) base VA for this GPU's root geometry.
pub(crate) fn iova_ttbr1_base(cfg: &hw::HwConfig) -> u64 {
    UatGeometry::new(cfg.uat_ias)
        .map(|g| g.upper_canonical_base())
        .unwrap_or(IOVA_TTBR1_BASE_39)
}

/// Rebase a kernel (TTBR1) VA from the G13/G14 layout to the layout of this GPU generation.
///
/// The kernel ranges keep the same offset from the TTBR1 base on every generation, right after
/// the 128 GiB firmware-private space: 0xffffffa0_00000000 on G13/G14 and 0xfffffc20_00000000
/// on G15. This is the identity on G13/G14.
pub(crate) fn kern_iova(cfg: &hw::HwConfig, iova: u64) -> u64 {
    iova - IOVA_TTBR1_BASE_39 + iova_ttbr1_base(cfg)
}

/// Rebase a kernel (TTBR1) VA range, see [`kern_iova`].
pub(crate) fn kern_iova_range(cfg: &hw::HwConfig, range: Range<u64>) -> Range<u64> {
    // An exclusive end of 0 would mean "top of the address space"; no range here uses it.
    kern_iova(cfg, range.start)..kern_iova(cfg, range.end)
}

const TTBR_VALID: u64 = 0x1; // BIT(0)
const TTBR_ASID_SHIFT: usize = 48;

/// Address of a special dummy page?
//const IOVA_UNK_PAGE: u64 = 0x6f_ffff8000;
pub(crate) const IOVA_UNK_PAGE: u64 = IOVA_USER_TOP - 2 * UAT_PGSZ as u64;

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

/// Read-only snapshot of the handoff region, for diagnostics.
#[derive(Debug)]
#[allow(dead_code)] // Fields are only read through Debug.
pub(crate) struct HandoffSnapshot {
    pub(crate) magic_ap: u64,
    pub(crate) magic_fw: u64,
    pub(crate) magic_fw_ok: bool,
    pub(crate) lock_ap: u8,
    pub(crate) lock_fw: u8,
    pub(crate) turn: u32,
    pub(crate) cur_slot: u32,
    pub(crate) unk2: u8,
    pub(crate) unk3: u64,
    pub(crate) flush0_state: u64,
    pub(crate) flush_kernel_state: u64,
}

/// One VM slot in the TTBAT
#[repr(C)]
struct SlotTTBS {
    ttb0: AtomicU64,
    ttb1: AtomicU64,
}

const SLOTS_SIZE: usize = UAT_NUM_CTX * size_of::<SlotTTBS>();

// We need at least page 0 (ttb0)
const PAGETABLES_SIZE: usize = UAT_PGSZ;

/// Inner data for a Vm instance. This is reference-counted by the outer Vm object.
struct VmInner {
    dev: driver::AsahiDevRef,
    is_kernel: bool,
    /// Permanently installed context without a dynamic VmBind lease.
    fixed_slot: Option<u32>,
    va_range: Range<u64>,
    page_table: UatPageTable,
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
        if self.uat_inner.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) { return Err(EIO); }
        let mut iova = op.addr();
        let mut left = op.range() as usize;
        let mut offset = op.offset() as usize;

        let bo = ctx.vm_bo.as_ref().expect("step_map with no BO");

        let one_page = op.flags().contains(gpuvm::GpuVaFlags::REPEAT);

        // GPUVM cannot see driver-owned KernelMappings.  Preflight the full
        // object range in the authoritative page table and allocate every
        // intermediate table before the first scatterlist leaf is written.
        if self.uat_inner.fault.is_some() {
            self.page_table
                .prepare_map(op.addr()..op.addr().checked_add(op.range()).ok_or(EOVERFLOW)?)?;
        }

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

            self.page_table.map_pages(
                iova..(iova + len as u64),
                addr as PhysicalAddr,
                ctx.prot,
                one_page,
            )?;

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
            return Err(EINVAL);
        }
        Ok(())
    }
    fn step_unmap(
        self: &mut gpuvm::UpdatingGpuVm<'_, Self>,
        op: &mut gpuvm::OpUnMap<Self>,
        _ctx: &mut Self::StepContext,
    ) -> Result {
        if self.uat_inner.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) { return Err(EIO); }
        let va = op.va().expect("step_unmap: missing VA");

        mod_dev_dbg!(self.dev, "MMU: unmap: {:#x}:{:#x}\n", va.addr(), va.range());

        self.page_table
            .unmap_pages(va.addr()..(va.addr() + va.range()))?;

        if let Some(asid) = self.slot() {
            fence(Ordering::SeqCst);
            mem::tlbi_range(asid as u8, va.addr() as usize, va.range() as usize);
            mod_dev_dbg!(
                self.dev,
                "MMU: flush range: asid={:#x} start={:#x} len={:#x}\n",
                asid,
                va.addr(),
                va.range(),
            );
            mem::sync();
        }

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
        if self.uat_inner.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) { return Err(EIO); }
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

        self.page_table.unmap_pages(unmap_start..unmap_end)?;

        if let Some(asid) = self.slot() {
            fence(Ordering::SeqCst);
            mem::tlbi_range(asid as u8, unmap_start as usize, unmap_range as usize);
            mod_dev_dbg!(
                self.dev,
                "MMU: flush range: asid={:#x} start={:#x} len={:#x}\n",
                asid,
                unmap_start,
                unmap_range,
            );
            mem::sync();
        }

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
    /// Returns the slot index, if this VM is bound.
    fn slot(&self) -> Option<u32> {
        if let Some(slot) = self.fixed_slot { return Some(slot); }
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

    /// Map an `mm::Node` representing an mapping in VA space.
    fn map_node(&mut self, node: &mm::Node<(), KernelMappingInner>, prot: Prot) -> Result {
        let mut iova = node.start();
        let mapping_end = if self.uat_inner.fault.is_some() {
            iova.checked_add(node.mapped_size as u64).ok_or(EOVERFLOW)?
        } else { iova + node.mapped_size as u64 };
        // The private fixed-mapping allocator cannot see GPUVM-owned leaves.
        // Reserve the complete page-table walk before installing any SG run.
        if self.uat_inner.fault.is_some() { self.page_table.prepare_map(iova..mapping_end)?; }
        let guard = node.bo.as_ref().ok_or(EINVAL)?.inner().inner.lock();
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
        Ok(())
    }
}

/// Shared reference to a virtual memory address space ([`Vm`]).
#[derive(Clone)]
pub(crate) struct Vm {
    fault: Option<Arc<AtomicBool>>,
    id: u64,
    inner: ARef<gpuvm::GpuVm<VmInner>>,
    dummy_obj: ARef<gem::Object>,
    binding: Arc<Mutex<VmBinding>>,
    driver_mappings: Option<Arc<Mutex<Option<VmDriverMappings>>>>,
    status: Option<Arc<crate::agx_status::VmStatus>>,
    job_lifetime: Option<Arc<Mutex<M3VmJobLifetime>>>,
}
impl Drop for Vm {
    fn drop(&mut self) {
        if self.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) {
            // Keep the GPUVM and its user BO bindings until a future reset
            // implementation can prove that firmware no longer references it.
            core::mem::forget(self.inner.clone());
        }
    }
}
no_debug!(Vm);

struct M3VmJobLifetime {
    active: usize,
    closed: bool,
    close_ranges: Option<(Range<u64>, Range<u64>)>,
    closed_objects: KVec<ARef<gem::Object>>,
}

/// GPU ownership outlives a DRM handle or file. Deferred user mappings retain
/// their GEM backing until all accepted jobs retire; quarantined packets keep
/// this guard until the processors have provably stopped.
pub(crate) struct M3VmJobGuard { vm: Vm }
impl Drop for M3VmJobGuard {
    fn drop(&mut self) {
        let (ranges, objects) = {
            let mut state = self.vm.job_lifetime.as_ref().expect("M3 VM job state").lock();
            state.active -= 1;
            if state.active != 0 { return; }
            (state.close_ranges.take(), core::mem::replace(&mut state.closed_objects, KVec::new()))
        };
        if let Some((user, kernel)) = ranges {
            if self.vm.unmap_user_ranges(user, kernel).is_err() {
                pr_err!("M3 VM job retirement: deferred user unmap failed\n");
            }
        }
        for object in objects {
            if self.vm.drop_mappings(&object).is_err() {
                pr_err!("M3 VM job retirement: deferred GEM unmap failed\n");
            }
        }
        self.vm.bo_deferred_cleanup();
    }
}

struct VmDriverMappings {
    mappings: KVec<KernelMapping>,
    reserved_ranges: KVec<Range<u64>>,
}
no_debug!(VmDriverMappings);

impl VmDriverMappings {
    fn iter_mappings(&self) -> impl Iterator<Item = &KernelMapping> {
        self.mappings.iter()
    }
}

fn any_range_overlaps<I>(target: Range<u64>, mut ranges: I) -> bool
where
    I: Iterator<Item = Range<u64>>,
{
    ranges.any(|range| range.overlaps(target.clone()))
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

    /// The bound `Vm` itself, for diagnostics that need to walk its tables.
    pub(crate) fn vm(&self) -> &Vm {
        &self.0
    }

    /// Whether this retained binding belongs to the supplied DRM VM.
    pub(crate) fn matches(&self, vm: &Vm) -> bool {
        core::ptr::eq(&*self.0.inner, &*vm.inner)
    }

    /// Physical base of the bound VM's page-table root.
    pub(crate) fn root(&self) -> u64 {
        self.0.ttb()
    }

    /// The bound VM's driver-assigned id.
    pub(crate) fn vm_id(&self) -> u64 {
        self.0.id
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

/// Inner data required for an object mapping into a [`Vm`].
pub(crate) struct KernelMappingInner {
    // Drop order matters:
    // - Drop the GpuVmBo first, which locks its BO GPUVA list and drops a GpuVm reference
    // - Drop the GEM BO next, since BO free can take the resv lock itself
    // - Drop the owner GpuVm last, since that again can take resv locks when the refcount drops to 0
    bo: Option<ARef<gpuvm::GpuVmBo<VmInner>>>,
    _gem: Option<ARef<gem::Object>>,
    owner: ARef<gpuvm::GpuVm<VmInner>>,
    uat_inner: Arc<UatInner>,
    prot: Prot,
    offset: usize,
    mapped_size: usize,
}

/// An object mapping into a [`Vm`], which reserves the address range from use by other mappings.
pub(crate) struct KernelMapping(ManuallyDrop<mm::Node<(), KernelMappingInner>>);

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

    fn remap_uncached_and_flush_legacy(&mut self) {
        let mut owner = self
            .0
            .owner
            .exec_lock(None, false)
            .expect("Failed to exec_lock in remap_uncached_and_flush");

        mod_dev_dbg!(
            owner.dev,
            "MMU: remap as uncached {:#x}:{:#x}\n",
            self.iova(),
            self.size()
        );

        // Remap in-place as uncached.
        // Do not try to unmap the guard page (-1)
        let prot = self.0.prot.as_uncached();
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
            let handoff_guard = uat_inner.handoff().lock_legacy();
            let cur_slot = uat_inner.handoff().current_slot();
            let ttb_cur = uat_inner.ttbs()[slot as usize].ttb0.load(Ordering::Relaxed);
            drop(handoff_guard);
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
        flush.begin_flush_legacy(self.iova(), self.size() as u64);
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
        if let Err(e) = (*owner.dev).gpu().and_then(|gpu| gpu.fwctl(cmd)) {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: ASC cache flush {:#x}:{:#x} failed (err: {:?})\n",
                self.iova(),
                self.size(),
                e
            );
        }

        // Finish the flush
        flush.end_flush_legacy();

        // Slot is unlocked here
    }

    fn drop_legacy(&mut self) {
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

        if self.0.prot.is_cached_noncoherent() {
            mod_pr_debug!(
                "MMU: remap as uncached {:#x}:{:#x}\n",
                self.iova(),
                self.size()
            );
            self.remap_uncached_and_flush_legacy();
        }

        let mut owner = self
            .0
            .owner
            .exec_lock(None, false)
            .expect("exec_lock failed in KernelMapping::drop");
        mod_dev_dbg!(
            owner.dev,
            "MMU: unmap {:#x}:{:#x}\n",
            self.iova(),
            self.size()
        );

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
            mem::tlbi_range(asid as u8, self.iova() as usize, self.size());
            mod_dev_dbg!(
                owner.dev,
                "MMU: flush range: asid={:#x} start={:#x} len={:#x}\n",
                asid,
                self.iova(),
                self.size()
            );
            mem::sync();
        }
        drop(owner);
        // SAFETY: This is the original legacy teardown order. The node was
        // automatically dropped after the old Drop callback returned.
        unsafe { ManuallyDrop::drop(&mut self.0) };
    }

    /// Remap a cached mapping as uncached, then synchronously flush that range of VAs from the
    /// coprocessor cache. This is required to safely unmap cached/private mappings.
    fn remap_uncached_and_flush(&mut self) -> Result {
        let mut owner = self
            .0
            .owner
            .exec_lock(None, false)?;

        mod_dev_dbg!(
            owner.dev,
            "MMU: remap as uncached {:#x}:{:#x}\n",
            self.iova(),
            self.size()
        );

        // Remap in-place as uncached.
        // Do not try to unmap the guard page (-1)
        let prot = self.0.prot.as_uncached();
        if let Err(error) = owner.page_table.reprot_pages(self.iova_range(), prot) {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: remap {:#x}:{:#x} failed\n",
                self.iova(),
                self.size()
            );
            return Err(error);
        }
        fence(Ordering::SeqCst);

        // M3 builds its UAT and object graph before AsahiData.gpu is
        // initialized. A constructor error can therefore drop cached mappings
        // while owner.dev has no DrmGpu yet. Those mappings were never
        // published to firmware, so the PTE downgrade above is sufficient.
        if !self.0.uat_inner.firmware_cache_flush_ready() {
            mod_dev_dbg!(
                owner.dev,
                "MMU: firmware cache flush not armed for {:#x}:{:#x}\n",
                self.iova(),
                self.size()
            );
            return Ok(());
        }

        // If we don't have (and have never had) a VM slot, just return
        let slot = match owner.slot() {
            None => return Ok(()),
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
            let handoff_guard = uat_inner.lock_handoff()?;
            let cur_slot = uat_inner.handoff().current_slot();
            let ttb_cur = uat_inner.ttbs()[slot as usize].ttb0.load(Ordering::Relaxed);
            drop(handoff_guard);
            if cur_slot == Some(slot) && ttb_cur == ttb {
                slot
            } else {
                return Ok(());
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
        if pages >= 0x10000 {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: Flush too big ({:#x} pages))\n",
                pages
            );
            return Err(E2BIG);
        }

        flush.begin_flush(self.iova(), self.size() as u64)?;
        let cmd = fw::channels::FwCtlMsg {
            addr: fw::types::U64(self.iova()),
            unk_8: 0,
            slot: flush_slot,
            page_count: pages as u16,
            unk_12: 2, // ?
        };

        // Tell the firmware to do a cache flush
        if let Err(e) = (*owner.dev).gpu().and_then(|gpu| gpu.fwctl(cmd)) {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: ASC cache flush {:#x}:{:#x} failed (err: {:?})\n",
                self.iova(),
                self.size(),
                e
            );
            return Err(e);
        }

        // Finish the flush
        flush.end_flush()?;

        // Slot is unlocked here
        Ok(())
    }
}
no_debug!(KernelMapping);

impl Drop for KernelMapping {
    fn drop(&mut self) {
        if self.0.uat_inner.fault.is_none() {
            self.drop_legacy();
            return;
        }

        if self.0.uat_inner.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) { return; }
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

        if self.0.prot.is_cached_noncoherent() {
            mod_pr_debug!(
                "MMU: remap as uncached {:#x}:{:#x}\n",
                self.iova(),
                self.size()
            );
            if let Err(error) = self.remap_uncached_and_flush() {
                if let Some(fault) = &self.0.uat_inner.fault { fault.store(true, Ordering::Release); }
                pr_err!("UAT: retaining mapping after failed firmware flush ({:?})\n", error);
                return;
            }
        }

        let mut owner = match self.0.owner.exec_lock(None, false) {
            Ok(owner) => owner,
            Err(_) => {
                if let Some(fault) = &self.0.uat_inner.fault { fault.store(true, Ordering::Release); }
                return;
            }
        };
        mod_dev_dbg!(
            owner.dev,
            "MMU: unmap {:#x}:{:#x}\n",
            self.iova(),
            self.size()
        );

        if owner.page_table.unmap_pages(self.iova_range()).is_err() {
            dev_err!(
                owner.dev.as_ref(),
                "MMU: unmap {:#x}:{:#x} failed\n",
                self.iova(),
                self.size()
            );
            if let Some(fault) = &self.0.uat_inner.fault { fault.store(true, Ordering::Release); }
            return;
        }

        if let Some(asid) = owner.slot() {
            fence(Ordering::SeqCst);
            mem::tlbi_range(asid as u8, self.iova() as usize, self.size());
            mod_dev_dbg!(
                owner.dev,
                "MMU: flush range: asid={:#x} start={:#x} len={:#x}\n",
                asid,
                self.iova(),
                self.size()
            );
            mem::sync();
        }
        drop(owner);
        // SAFETY: The PTEs are gone, required flushes completed, and the VM
        // execution lock is released before the node drops BO references.
        unsafe { ManuallyDrop::drop(&mut self.0) };
    }
}

/// Shared UAT global data structures
struct UatShared {
    fault: Option<Arc<AtomicBool>>,
    kernel_ttb1: u64,
    map_kernel_to_user: bool,
    handoff_rgn: UatRegion,
    ttbs_rgn: UatRegion,
}

impl UatShared {
    fn lock_handoff(&self) -> Result<HandoffGuard<'_>> {
        if self.fault.is_none() { return Ok(self.handoff().lock_legacy()); }

        if self.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) { return Err(EIO); }
        self.handoff().try_lock().inspect_err(|_| {
            if let Some(fault) = &self.fault { fault.store(true, Ordering::Release); }
        })
    }

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
}

// SAFETY: Nothing here is unsafe to send across threads.
unsafe impl Send for UatShared {}

/// Inner data for the top-level UAT instance.
#[pin_data]
struct UatInner {
    m3_running: bool,
    fault: Option<Arc<AtomicBool>>,
    firmware_cache_flush_ready: AtomicBool,
    #[pin]
    shared: Mutex<UatShared>,
    #[pin]
    handoff_flush: [Mutex<HandoffFlush>; UAT_NUM_CTX + 1],
}

impl UatInner {
    fn firmware_cache_flush_ready(&self) -> bool {
        self.firmware_cache_flush_ready.load(Ordering::Acquire)
    }

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
    cfg: UatConfig,

    inner: Arc<UatInner>,
    slots: slotalloc::SlotAllocator<SlotInner>,

    kernel_vm: Vm,
    kernel_lower_vm: Vm,
}

struct HandoffGuard<'a>(&'a Handoff, bool);

impl Drop for HandoffGuard<'_> {
    fn drop(&mut self) {
        if self.1 {
            self.0.turn.store(1, Ordering::Relaxed);
            self.0.lock_ap.store(0, Ordering::Release);
        } else {
            self.0.unlock();
        }
    }
}

impl Handoff {
    fn lock_legacy(&self) -> HandoffGuard<'_> {
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
        HandoffGuard(self, true)
    }

    /// Acquire firmware exclusion or return an error after a bounded wait.
    /// The guard releases exclusion on every return path, including errors.
    fn try_lock(&self) -> Result<HandoffGuard<'_>> {
        let start = Instant::<Monotonic>::now();
        if !crate::handoff_lock::acquire(&self.lock_ap, &self.lock_fw, &self.turn,
            || start.elapsed() >= Delta::from_millis(100),
            || fsleep(Delta::from_micros(20))) {
            pr_err!("UAT handoff: firmware exclusion timed out; AP interest withdrawn\n");
            return Err(ETIMEDOUT);
        }
        Ok(HandoffGuard(self, false))
    }

    /// Unlock the handoff region, allowing firmware access
    fn unlock(&self) {
        crate::handoff_lock::release(&self.lock_ap, &self.turn);
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

    /// Initialize the handoff region.
    ///
    /// In [`HandoffMode::FirmwareDekker`] this blocks (bounded) until the
    /// firmware publishes `magic_fw`. The stopped M3 owner initializes
    /// its AP state before firmware starts.
    fn init(&self, mode: HandoffMode, m3: bool) -> Result {
        self.magic_ap.store(PPL_MAGIC, Ordering::Relaxed);
        self.cur_slot.store(if m3 && mode != HandoffMode::FirmwareDekker { u32::MAX } else { 0 }, Ordering::Relaxed);
        self.unk3.store(0, Ordering::Relaxed);
        if mode == HandoffMode::StoppedFirmwareT6030 {
            // This mode is constructed only with ASC stopped. Preserve the
            // firmware magic but initialize the AP producer's lock/flush state.
            self.lock_ap.store(0, Ordering::Relaxed);
            self.lock_fw.store(0, Ordering::Relaxed);
            self.turn.store(0, Ordering::Relaxed);
            if !m3 && self.magic_fw.load(Ordering::Relaxed) != PPL_MAGIC {
                self.unk2.store(1, Ordering::Relaxed);
            }
        }
        fence(Ordering::SeqCst);

        if matches!(mode, HandoffMode::FirmwareDekker | HandoffMode::FirmwareT6030) {
            let start = Instant::<Monotonic>::now();
            const TIMEOUT: Delta = Delta::from_millis(1000);

            let mut guard = if m3 { self.try_lock()? } else { self.lock_legacy() };
            while start.elapsed() < TIMEOUT {
                if self.magic_fw.load(Ordering::Relaxed) == PPL_MAGIC {
                    break;
                } else {
                    drop(guard);
                    fsleep(Delta::from_millis(10));
                    guard = if m3 { self.try_lock()? } else { self.lock_legacy() };
                }
            }

            if self.magic_fw.load(Ordering::Relaxed) != PPL_MAGIC {
                drop(guard);
                pr_err!("Handoff: Failed to initialize (firmware not running?)\n");
                return Err(EIO);
            }

            drop(guard);
        }

        for i in 0..=UAT_NUM_CTX {
            self.flush[i].state.store(0, Ordering::Relaxed);
            self.flush[i].addr.store(0, Ordering::Relaxed);
            self.flush[i].size.store(0, Ordering::Relaxed);
        }
        fence(Ordering::SeqCst);
        Ok(())
    }
}

/// Exercise the exact guard implementation on private CPU memory, including
/// timeout unwinding. This does not touch the firmware handoff reservation.
pub(crate) fn check_handoff_guard() -> Result {
    // SAFETY: Every field is an integer atomic, for which zero is valid.
    let handoff: Handoff = unsafe { core::mem::zeroed() };
    {
        let _guard = handoff.try_lock()?;
        if handoff.lock_ap.load(Ordering::Acquire) != 1 { return Err(EIO); }
    }
    if handoff.lock_ap.load(Ordering::Acquire) != 0
        || handoff.turn.load(Ordering::Acquire) != 1 { return Err(EIO); }
    handoff.cur_slot.store(u32::MAX, Ordering::Release);
    if handoff.current_slot().is_some() { return Err(EIO); }
    handoff.lock_fw.store(1, Ordering::Release);
    for turn in [0, 1] {
        handoff.turn.store(turn, Ordering::Release);
        if handoff.try_lock().err() != Some(ETIMEDOUT)
            || handoff.lock_ap.load(Ordering::Acquire) != 0
            || handoff.lock_fw.load(Ordering::Acquire) != 1 { return Err(EIO); }
    }
    handoff.lock_fw.store(0, Ordering::Release);
    let _guard = handoff.try_lock()?;
    Ok(())
}

/// Represents a single flush info slot in the handoff region.
///
/// # Invariants
/// The pointer is valid and there is no aliasing HandoffFlush instance.
struct HandoffFlush(*const FlushInfo);

// SAFETY: These pointers are safe to send across threads.
unsafe impl Send for HandoffFlush {}

impl HandoffFlush {
    fn end_flush_legacy(&self) {
        // SAFETY: Per the type invariant, this is safe
        let flush = unsafe { self.0.as_ref().unwrap() };
        let state = flush.state.load(Ordering::Relaxed);
        if state != 2 {
            pr_err!("Handoff: expected flush state 2, got {}\n", state);
        }
        flush.state.store(0, Ordering::Relaxed);
    }

    fn begin_flush_legacy(&self, start: u64, size: u64) {
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

    /// Set up a flush operation for the coprocessor
    fn begin_flush(&self, start: u64, size: u64) -> Result {
        // SAFETY: Per the type invariant, this is safe
        let flush = unsafe { self.0.as_ref().unwrap() };

        let state = flush.state.load(Ordering::Relaxed);
        if state != 0 {
            pr_err!("Handoff: expected flush state 0, got {}\n", state);
            return Err(EBUSY);
        }
        flush.addr.store(start, Ordering::Relaxed);
        flush.size.store(size, Ordering::Relaxed);
        flush.state.store(1, Ordering::Relaxed);
        Ok(())
    }

    /// Complete a flush operation for the coprocessor
    fn end_flush(&self) -> Result {
        // SAFETY: Per the type invariant, this is safe
        let flush = unsafe { self.0.as_ref().unwrap() };
        let state = flush.state.load(Ordering::Relaxed);
        // Firmware acknowledges a consumed flush by moving state to 2.
        let expected = 2;
        if state != expected {
            pr_err!(
                "Handoff: expected flush state {}, got {}\n",
                expected,
                state
            );
            return Err(EIO);
        }
        flush.state.store(0, Ordering::Relaxed);
        Ok(())
    }
}

impl Vm {
    pub(crate) fn status(&self) -> Result<&Arc<crate::agx_status::VmStatus>> { self.status.as_ref().ok_or(EINVAL) }
    pub(crate) fn is_m3(&self) -> bool { self.fault.is_some() }

    /// Create a new virtual memory address space
    fn new(
        dev: &driver::AsahiDevice,
        uat_inner: Arc<UatInner>,
        kernel_range: Range<u64>,
        cfg: UatConfig,
        ttb: Option<PhysicalAddr>,
        id: u64,
    ) -> Result<Vm> {
        if uat_inner.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) { return Err(EIO); }
        let fault = uat_inner.fault.clone();
        let dummy_obj = gem::new_kernel_object(dev, UAT_PGSZ)?;
        let is_kernel = ttb.is_some();
        let iova_kern_range = iova_kern_range(cfg)?;

        let page_table = if let Some(ttb) = ttb {
            if cfg == UatConfig::T6030 {
                let node = dev.as_ref().of_node().ok_or(ENODEV)?;
                let mut resources = KVec::new();
                for name in [c_str!("pagetables"), c_str!("shared-l2")] {
                    resources.push(crate::m3_resources::reserved_resource(&node, name)?, GFP_KERNEL)?;
                }
                // SAFETY: The M4 constructor admits these reservations and
                // requires ASC stopped during handoff/table initialization.
                let wc = cfg == UatConfig::T6030
                    && [c_str!("pagetables"), c_str!("shared-l2")].iter().all(|name| crate::m3_board::region_is_nomap(&node, name));
                let tables = unsafe { if wc {crate::pgtable_memory::ReservedTables::new_wc(resources)} else {crate::pgtable_memory::ReservedTables::new(resources)} }?;
                if cfg == UatConfig::T6030 && uat_inner.m3_running {
                    // RTKit has switched its private mapping regime. Preserve
                    // firmware root entries 0/1; the host window is unused
                    // until this owner publishes initdata.
                    unsafe { UatPageTable::new_with_m3_live_ttb(ttb,iova_kern_range.clone(),tables,dev) }?
                } else {
                    let mut table = unsafe { UatPageTable::new_with_reserved_ttb(ttb, iova_kern_range.clone(), cfg.ias, cfg.oas, tables) }?;
                    let shared = crate::m3_resources::reserved_resource(&node, c_str!("shared-l2"))?;
                    table.install_reserved_middle(iova_kern_range.start, shared.start())?;
                    table
                }
            } else {
                UatPageTable::new_with_ttb(ttb, iova_kern_range.clone(), cfg.ias, cfg.oas)?
            }
        } else if cfg == UatConfig::T6030 {
            UatPageTable::new_m3_coherent(cfg.ias,cfg.oas,dev)?

        } else {
            UatPageTable::new(cfg.ias, cfg.oas)?
        };

        let lower_start = IOVA_USER_BASE;
        let driver_start = lower_start;
        let (va_range, gpuvm_range) = if is_kernel {
            (iova_kern_range, kernel_range.clone())
        } else {
            (
                driver_start..uat_geometry(cfg)?.user_va_top(),
                lower_start..(uat_geometry(cfg)?.user_va_top() - 2 * UAT_PGSZ as u64),
            )
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

        let binding_clone = binding.clone();
        Ok(Vm {
            fault,
            id,
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
                    fixed_slot: None,
                    page_table,
                    mm,
                    uat_inner,
                    binding: binding_clone,
                    id,
                }),
            )?,
            binding,
            driver_mappings: if cfg.chip_id == 0x6030 { Some(Arc::pin_init(new_mutex!(None, "VmDriverMappings"), GFP_KERNEL)?) } else { None },
            status: if cfg.chip_id == 0x6030 { Some(Arc::new(crate::agx_status::VmStatus::new(), GFP_KERNEL)?) } else { None },
            job_lifetime: if cfg.chip_id == 0x6030 {
                Some(Arc::pin_init(new_mutex!(M3VmJobLifetime {
                    active: 0, closed: false, close_ranges: None, closed_objects: KVec::new(),
                }, "M3VmJobLifetime"), GFP_KERNEL)?)
            } else { None },

        })
    }

    /// Get the translation table base for this Vm
    fn ttb(&self) -> u64 {
        self.binding.lock().ttb
    }

    /// Retain hardware-required fixed aliases for this VM's complete
    /// lifetime. Installing them before the VM reaches userspace also makes
    /// the GPUVM allocator reserve the addresses against user mappings.
    pub(crate) fn install_driver_mappings(
        &self,
        mappings: KVec<KernelMapping>,
        reserved_ranges: KVec<Range<u64>>,
    ) -> Result {
        let mut retained = self.driver_mappings.as_ref().ok_or(EINVAL)?.lock();
        if retained.is_some() {
            return Err(EBUSY);
        }
        *retained = Some(VmDriverMappings {
            mappings,
            reserved_ranges,
        });
        Ok(())
    }

    pub(crate) fn driver_range_overlaps(&self, range: Range<u64>) -> bool {
        let Some(driver_mappings) = &self.driver_mappings else { return false; };
        driver_mappings.lock().as_ref().is_some_and(|driver| {
            any_range_overlaps(
                range.clone(),
                driver.iter_mappings().map(KernelMapping::iova_range),
            ) || any_range_overlaps(range, driver.reserved_ranges.iter().cloned())
        })
    }

    pub(crate) fn retain_m3_job(&self) -> Result<M3VmJobGuard> {
        let mut state = self.job_lifetime.as_ref().ok_or(EINVAL)?.lock();
        if state.closed { return Err(ENOENT); }
        state.active = state.active.checked_add(1).ok_or(EOVERFLOW)?;
        Ok(M3VmJobGuard { vm: self.clone() })
    }

    /// Remove userspace mappings while preserving VM-lifetime driver aliases.
    pub(crate) fn unmap_user_ranges(
        &self,
        user_range: Range<u64>,
        kernel_range: Range<u64>,
    ) -> Result {
        if let Some(job_lifetime) = &self.job_lifetime {
            let mut state = job_lifetime.lock();
            state.closed = true;
            if state.active != 0 {
                state.close_ranges = Some((user_range, kernel_range));
                return Ok(());
            }
        }
        let Some(driver_mappings) = &self.driver_mappings else {
            let left = user_range.start..kernel_range.start;
            let right = kernel_range.end..user_range.end;
            if !left.is_empty() { self.unmap_range(left.start, left.range())?; }
            if !right.is_empty() { self.unmap_range(right.start, right.range())?; }
            return Ok(());
        };

        let retained = driver_mappings.lock();
        let driver = retained.as_ref();
        let mut cursor = user_range.start;

        while cursor < user_range.end {
            if kernel_range.contains(&cursor) {
                cursor = kernel_range.end.min(user_range.end);
                continue;
            }

            let mut next = user_range.end;
            let mut covering_end = cursor;
            if let Some(driver) = driver {
                for mapping in driver.iter_mappings() {
                    let range = mapping.iova_range();
                    if range.contains(&cursor) {
                        covering_end = covering_end.max(range.end);
                    } else if cursor < range.start {
                        next = next.min(range.start);
                    }
                }
                for range in driver.reserved_ranges.iter() {
                    if range.contains(&cursor) {
                        covering_end = covering_end.max(range.end);
                    } else if cursor < range.start {
                        next = next.min(range.start);
                    }
                }
            }
            if covering_end != cursor {
                cursor = covering_end.min(user_range.end);
                continue;
            }
            if cursor < kernel_range.start {
                next = next.min(kernel_range.start);
            }
            if next <= cursor {
                return Err(EIO);
            }
            self.unmap_range(cursor, next - cursor)?;
            cursor = next;
        }
        Ok(())
    }

    /// Check that a byte range is fully mapped with the requested GPU access.
    pub(crate) fn covers_range(
        &self,
        address: u64,
        size: u64,
        need_read: bool,
        need_write: bool,
    ) -> bool {
        if size == 0 {
            return false;
        }
        let Some(end) = address.checked_add(size) else {
            return false;
        };
        let page_mask = UAT_PGMSK as u64;
        let start = address & !page_mask;
        let aligned_end = if end & page_mask == 0 {
            end
        } else {
            match (end | page_mask).checked_add(1) {
                Some(end) => end,
                None => return false,
            }
        };
        let mut inner = match self.inner.exec_lock(None, false) {
            Ok(inner) => inner,
            Err(_) => return false,
        };
        if start < inner.va_range.start || aligned_end > inner.va_range.end {
            return false;
        }
        inner
            .page_table
            .covers_range(start..aligned_end, need_read, need_write)
            .unwrap_or(false)
    }

    /// Resolve an IOVA through this VM's current page table. Callers use this
    /// for mapping-identity checks only; it does not read the mapped page.
    pub(crate) fn translate_iova(&self, address: u64) -> Result<PhysicalAddr> {
        let mut inner = self.inner.exec_lock(None, false)?;
        inner.page_table.translate_iova(address)
    }

    /// Read GPU-visible bytes out of this VM.
    ///
    /// Fail-closed: the whole span must first pass `covers_range` with GPU
    /// read permission, and every page is then translated and borrowed
    /// individually.  This only ever touches ordinary DRAM backing a GEM
    /// object that is mapped into this VM, never GPU MMIO, so it is safe with
    /// the cores gated.
    ///
    /// M3 compute validates the client control stream before publishing it.
    pub(crate) fn read_bytes(&self, iova: u64, out: &mut [u8]) -> Result {
        if out.is_empty() {
            return Err(EINVAL);
        }
        if !self.covers_range(iova, out.len() as u64, true, false) {
            return Err(EFAULT);
        }
        let total = out.len();
        let mut done = 0usize;
        while done < total {
            let addr = iova.checked_add(done as u64).ok_or(EOVERFLOW)?;
            let phys = self.translate_iova(addr)?;
            let page_mask = (kernel::page::PAGE_SIZE as PhysicalAddr) - 1;
            let page_phys = phys & !page_mask;
            let offset = (phys - page_phys) as usize;
            let chunk = cmp::min(kernel::page::PAGE_SIZE - offset, total - done);
            // SAFETY: `translate_iova` resolved this address through a leaf
            // PTE of this VM, so it names a page of a GEM object that is
            // mapped here and therefore pinned for the duration of the
            // mapping. `borrow_phys` additionally rejects anything without a
            // struct page.
            let page = unsafe { Page::borrow_phys(&page_phys) }.ok_or(EFAULT)?;
            let dst = &mut out[done..done + chunk];
            page.with_pointer_into_page(offset, chunk, |ptr| {
                // SAFETY: `with_pointer_into_page` bounds-checked
                // `offset..offset + chunk` inside the page.
                let src = unsafe { core::slice::from_raw_parts(ptr as *const u8, chunk) };
                dst.copy_from_slice(src);
                Ok(())
            })?;
            done += chunk;
        }
        Ok(())
    }

    /// Snapshot the first `out.len()` hardware context-table (TTBAT) entries
    /// as `(ttb0, ttb1)` pairs.
    ///
    /// The TTBAT is ordinary host DRAM, so this reads no GPU MMIO
    /// and is safe with the cores gated. M3 fault diagnostics use it to
    /// identify the tables visible to each hardware context.
    pub(crate) fn context_roots(&self, out: &mut [(u64, u64)]) -> Result {
        let inner = self.inner.exec_lock(None, false)?;
        let shared = inner.uat_inner.lock();
        let ttbs = shared.ttbs();
        for (slot, entry) in out.iter_mut().enumerate() {
            if slot >= UAT_NUM_CTX {
                break;
            }
            *entry = (
                ttbs[slot].ttb0.load(Ordering::Acquire),
                ttbs[slot].ttb1.load(Ordering::Acquire),
            );
        }
        Ok(())
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
        let size = object_range.range();
        let sgt = gem.owned_sg_table()?;
        let mut inner = self.inner.exec_lock(Some(gem), false)?;
        let vm_bo = self.inner.obtain_bo(gem)?;

        let mut vm_bo_guard = vm_bo.inner().inner.lock();
        if vm_bo_guard.sgt.is_none() {
            vm_bo_guard.sgt.replace(sgt);
        }
        core::mem::drop(vm_bo_guard);

        let uat_inner = inner.uat_inner.clone();
        // Reserve the VA before attaching objects whose destructors take the
        // GEM reservation lock.  The allocator drops its payload internally
        // on an insertion error, while exec_lock still owns that lock.
        let node_result = inner.mm.insert_node_in_range(
            KernelMappingInner {
                owner: self.inner.clone(),
                uat_inner,
                prot,
                bo: None,
                _gem: None,
                offset: object_range.start,
                mapped_size: size,
            },
            (size + if guard { UAT_PGSZ } else { 0 }) as u64, // Add guard page
            alignment,
            0,
            range.start,
            range.end,
            mm::InsertMode::Best,
        );
        let mut node = match node_result {
            Ok(node) => node,
            Err(err) => {
                core::mem::drop(inner);
                return Err(err);
            }
        };
        {
            let node_inner = node.as_mut().inner_mut();
            node_inner.bo = Some(vm_bo);
            node_inner._gem = Some(gem.into());
        }

        let ret = inner.map_node(&node, prot);
        // Drop the exec_lock first, so that if map_node failed the
        // KernelMappingInner destructur does not deadlock.
        core::mem::drop(inner);
        ret?;
        Ok(KernelMapping(ManuallyDrop::new(node)))
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
        let sgt = gem.owned_sg_table()?;
        let mut inner = self.inner.exec_lock(Some(&gem), false)?;

        let vm_bo = self.inner.obtain_bo(&gem)?;

        let mut vm_bo_guard = vm_bo.inner().inner.lock();
        if vm_bo_guard.sgt.is_none() {
            vm_bo_guard.sgt.replace(sgt);
        }
        core::mem::drop(vm_bo_guard);

        let uat_inner = inner.uat_inner.clone();
        // See map_in_range(): keep dma_resv-owning references out of the
        // allocator payload until the fixed-address reservation succeeds.
        let node_result = inner.mm.reserve_node(
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
            (size + if guard { UAT_PGSZ } else { 0 }) as u64, // Add guard page
            0,
        );
        let mut node = match node_result {
            Ok(node) => node,
            Err(err) => {
                core::mem::drop(inner);
                return Err(err);
            }
        };
        {
            let node_inner = node.as_mut().inner_mut();
            node_inner.bo = Some(vm_bo);
            node_inner._gem = Some(gem.clone());
        }

        let ret = inner.map_node(&node, prot);
        // Drop the exec_lock first, so that if map_node failed the
        // KernelMappingInner destructur does not deadlock.
        core::mem::drop(inner);
        ret?;
        Ok(KernelMapping(ManuallyDrop::new(node)))
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
            let mut vm_bo_guard = vm_bo.inner().inner.lock();
            if vm_bo_guard.sgt.is_none() {
                let sgt = gem.owned_sg_table()?;

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
            core::mem::drop(vm_bo_guard);
        }

        let mut inner = self.inner.exec_lock(Some(gem), true)?;

        // Legacy preallocation happens before GPUVM can unmap an old VA.
        if self.fault.is_none() {
            inner.page_table.alloc_pages(addr..(addr + size))?;
        }

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
        self.map_io_internal(Some(iova), 0..0, phys, size, prot)
    }

    /// Allocate a firmware MMIO VA through the same DRM range allocator as GEM.
    pub(crate) fn map_io_in_range(&self, range: Range<u64>, phys: usize,
        size: usize, prot: Prot) -> Result<KernelMapping> {
        self.map_io_internal(None, range, phys, size, prot)
    }

    fn map_io_internal(&self, fixed: Option<u64>, range: Range<u64>, phys: usize,
        size: usize, prot: Prot) -> Result<KernelMapping> {
        let mut inner = self.inner.exec_lock(None, false)?;
        let iova = fixed.unwrap_or(0);
        if size == 0 || (iova as usize | phys | size) & UAT_PGMSK != 0 {
            dev_err!(
                inner.dev.as_ref(),
                "MMU: KernelMapping {:#x}:{:#x} -> {:#x} is not page-aligned\n",
                phys,
                size,
                iova
            );
            return Err(EINVAL);
        }

        let payload = KernelMappingInner {
            owner: self.inner.clone(), uat_inner: inner.uat_inner.clone(), prot,
            bo: None, _gem: None, offset: 0, mapped_size: size,
        };
        let node = if let Some(iova) = fixed {
            inner.mm.reserve_node(payload, iova, size as u64, 0)?
        } else {
            inner.mm.insert_node_in_range(payload, size as u64, UAT_PGSZ as u64,
                0, range.start, range.end, mm::InsertMode::Best)?
        };
        let iova = node.start();
        mod_dev_dbg!(inner.dev, "MMU: IO map: {:#x}:{:#x} -> {:#x}\n", phys, size, iova);

        let prepared = if self.fault.is_some() {
            inner.page_table.prepare_map(iova..iova.checked_add(size as u64).ok_or(EOVERFLOW)?)
        } else { Ok(()) };
        if let Err(error) = prepared {
            // Drop the VM lock before the reserved node and its owner refs.
            core::mem::drop(inner);
            return Err(error);
        }
        let ret = inner.page_table.map_pages(
            iova..(iova + size as u64),
            phys as PhysicalAddr,
            prot,
            false,
        );
        // Drop the exec_lock first, so that if map_node failed the
        // KernelMappingInner destructur does not deadlock.
        core::mem::drop(inner);
        ret?;
        Ok(KernelMapping(ManuallyDrop::new(node)))
    }

    /// Unmap everything in an address range.
    pub(crate) fn unmap_range(&self, iova: u64, size: u64) -> Result {
        // Unmapping a range can only do a single split, so just preallocate
        // the prev and next GpuVas
        let mut ctx = StepContext {
            prev_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            next_va: Some(gpuvm::GpuVa::<VmInner>::new(pin_init::default())?),
            ..Default::default()
        };

        let mut inner = self.inner.exec_lock(None, false)?;

        mod_dev_dbg!(inner.dev, "MMU: sm_unmap: {:#x}:{:#x}\n", iova, size);
        inner.sm_unmap(&mut ctx, iova, size)
    }

    /// Drop mappings for a given bo.
    pub(crate) fn drop_mappings(&self, gem: &gem::Object) -> Result {
        if let Some(job_lifetime) = &self.job_lifetime {
            let mut state = job_lifetime.lock();
            if state.active != 0 {
                if !state.closed_objects.iter().any(|object| core::ptr::eq(&**object, gem)) {
                    state.closed_objects.push(gem.into(), GFP_KERNEL)?;
                }
                return Ok(());
            }
        }

        // Removing whole mappings only does unmaps, so no preallocated VAs
        let mut ctx = Default::default();

        let inner = self.inner.exec_lock(Some(gem), false)?;

        if let Some(bo) = self.inner.find_bo(gem) {
            mod_dev_dbg!(inner.dev, "MMU: bo_unmap\n");
            self.inner.bo_unmap(&mut ctx, &bo)?;
            mod_dev_dbg!(inner.dev, "MMU: bo_unmap done\n");
            // We need to drop the exec_lock first, then the GpuVmBo since that will take the lock itself.
            core::mem::drop(inner);
            core::mem::drop(bo);
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
        if self.uat_inner.fault.as_ref().is_some_and(|fault| fault.load(Ordering::Acquire)) {
            self.page_table.quarantine();
            core::mem::forget(self.uat_inner.clone());
            return;
        }
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
            let handoff_guard = match uat_inner.lock_handoff() {
                Ok(guard) => guard,
                Err(_) => {
                    self.page_table.quarantine();
                    core::mem::forget(self.uat_inner.clone());
                    pr_err!("UAT: retaining VM root after handoff failure\n");
                    return;
                }
            };
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
            drop(handoff_guard);
            core::mem::drop(uat_inner);
            fence(Ordering::SeqCst);

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
        let res = crate::m3_resources::reserved_resource(&of_node, name)?;
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

        let flags = if cached || !crate::m3_board::region_is_nomap(&of_node, name) {
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

    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn dump_kernel_pages(&self) -> Result<KVVec<pgtable::DumpedPage>> {
        // Full upper-half range for this SoC's root geometry.
        let full_range = uat_geometry(self.cfg)?.upper_canonical_base()..(!UAT_PGMSK as u64);
        let mut inner = self.kernel_vm.inner.exec_lock(None, false)?;
        inner.page_table.dump_pages(full_range)
    }

    /// Read-only translation of a kernel (TTBR1) VA range through the kernel page tables, which
    /// on G15 are the bootloader-built firmware tree. Returns the physical address and leaf
    /// descriptor of every UAT page in the (UAT-page aligned) range; fails with ENOENT if any
    /// page is unmapped and with an error if a table page is not readable. Never allocates page
    /// tables or modifies a PTE.
    pub(crate) fn translate_kernel_range(
        &self,
        iova_range: Range<u64>,
    ) -> Result<KVec<(PhysicalAddr, u64)>> {
        let base = uat_geometry(self.cfg)?.upper_canonical_base();
        if iova_range.start < base || iova_range.end <= iova_range.start {
            return Err(EINVAL);
        }
        let inner = self.kernel_vm.inner.exec_lock(None, false)?;
        inner.page_table.translate_range(iova_range)
    }

    /// Read-only translation of the single kernel (TTBR1) VA `iova` (G15 bring-up diagnostics).
    /// Returns the physical address of `iova` (page PA plus offset) and the leaf descriptor.
    pub(crate) fn translate_kernel_va(&self, iova: u64) -> Result<(PhysicalAddr, u64)> {
        let pgmsk = UAT_PGMSK as u64;
        let start = iova & !pgmsk;
        let end = start.checked_add(UAT_PGSZ as u64).ok_or(EINVAL)?;
        let pages = self.translate_kernel_range(start..end)?;
        let (pa, pte) = *pages.first().ok_or(ENOENT)?;
        Ok((pa + (iova & pgmsk) as PhysicalAddr, pte))
    }

    /// Returns a read-only snapshot of the firmware handoff region (no handoff lock is taken).
    pub(crate) fn handoff_snapshot(&self) -> HandoffSnapshot {
        let inner = self.inner.lock();
        let h = inner.handoff();
        let magic_fw = h.magic_fw.load(Ordering::Relaxed);
        HandoffSnapshot {
            magic_ap: h.magic_ap.load(Ordering::Relaxed),
            magic_fw,
            magic_fw_ok: magic_fw == PPL_MAGIC,
            lock_ap: h.lock_ap.load(Ordering::Relaxed),
            lock_fw: h.lock_fw.load(Ordering::Relaxed),
            turn: h.turn.load(Ordering::Relaxed),
            cur_slot: h.cur_slot.load(Ordering::Relaxed),
            unk2: h.unk2.load(Ordering::Relaxed),
            unk3: h.unk3.load(Ordering::Relaxed),
            flush0_state: h.flush[0].state.load(Ordering::Relaxed),
            flush_kernel_state: h.flush[UAT_NUM_CTX].state.load(Ordering::Relaxed),
        }
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
                let handoff_guard = uat_inner.lock_handoff()?;
                if uat_inner.handoff().current_slot() == Some(idx as u32) {
                    pr_err!(
                        "Vm::bind to slot {}, but it is currently in use by the ASC?\n",
                        idx
                    );
                }
                ttbs[idx].ttb0.store(ttb, Ordering::Release);
                ttbs[idx].ttb1.store(ttb1, Ordering::Release);
                drop(handoff_guard);
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

    /// Creates a new `Vm` linked to this UAT.
    pub(crate) fn new_vm(&self, id: u64, kernel_range: Range<u64>) -> Result<Vm> {
        Vm::new(
            &self.dev,
            self.inner.clone(),
            kernel_range,
            self.cfg,
            None,
            id,
        )
    }

    /// Creates the reference-counted inner data for a new `Uat` instance.
    #[inline(never)]
    fn make_inner(dev: &driver::AsahiDevice, handoff_mode: HandoffMode, m3: bool) -> Result<Arc<UatInner>> {
        let cached = !matches!(handoff_mode,HandoffMode::StoppedFirmwareT6030|HandoffMode::FirmwareT6030);
        let handoff_rgn = Self::map_region(dev.as_ref(), c_str!("handoff"), HANDOFF_SIZE, cached)?;
        let ttbs_rgn = Self::map_region(dev.as_ref(), c_str!("ttbs"), SLOTS_SIZE, cached)?;

        // SAFETY: The Handoff struct layout matches the firmware's view of memory at this address,
        // and the region is at least large enough per the size specified above.
        let handoff = unsafe { &(handoff_rgn.map.ptr() as *mut Handoff).as_ref().unwrap() };

        dev_info!(dev.as_ref(), "MMU: Initializing kernel page table\n");

        let fault = if !m3 {
            None
        } else {
            Some(Arc::new(AtomicBool::new(false), GFP_KERNEL)?)
        };
        let shared_fault = fault.clone();
        Arc::pin_init(
            try_pin_init!(UatInner {
                m3_running: handoff_mode == HandoffMode::FirmwareT6030,
                fault,
                firmware_cache_flush_ready: AtomicBool::new(
                    handoff_mode == HandoffMode::FirmwareDekker
                ),
                handoff_flush <- pin_init::pin_init_array_from_fn(|i| {
                    new_mutex!(HandoffFlush(&handoff.flush[i]), "handoff_flush")
                }),
                shared <- new_mutex!(
                    UatShared {
                        fault: shared_fault,
                        kernel_ttb1: 0,
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

    /// # Safety
    /// J514S RTKit has completed wake, but has not received an initdata root.
    /// The owner retains power and excludes GPU jobs until publication.
    pub(crate) unsafe fn new_t6030_running(dev: &driver::AsahiDevice) -> Result<Self> {
        Self::new_with_config(dev,UatConfig::T6030,true,HandoffMode::FirmwareT6030)
    }

    #[inline(never)]
    pub(crate) fn new(
        dev: &driver::AsahiDevice,
        cfg: &'static hw::HwConfig,
        map_kernel_to_user: bool,
    ) -> Result<Self> {
        Self::new_with_handoff_mode(dev, cfg, map_kernel_to_user, HandoffMode::FirmwareDekker)
    }

    #[inline(never)]
    pub(crate) fn new_with_handoff_mode(
        dev: &driver::AsahiDevice,
        cfg: &'static hw::HwConfig,
        map_kernel_to_user: bool,
        handoff_mode: HandoffMode,
    ) -> Result<Self> {
        // M4 requires the stopped-ASC contract of its unsafe constructor.
        if handoff_mode == HandoffMode::StoppedFirmwareT6030 { return Err(EINVAL); }
        Self::new_with_config(
            dev,
            UatConfig::from_hw(cfg),
            map_kernel_to_user,
            handoff_mode,
        )
    }

    #[inline(never)]
    fn new_with_config(
        dev: &driver::AsahiDevice,
        cfg: UatConfig,
        map_kernel_to_user: bool,
        handoff_mode: HandoffMode,
    ) -> Result<Self> {
        dev_info!(dev.as_ref(), "MMU: Initializing...\n");

        // Validate the geometry first: 39- and 42-bit roots are the two
        // recovered layouts, and the page-table walk, kernel-half VA window,
        // and leaf PTE policy are implemented for both (the AGX3 leaf PTE
        // encoder is byte-identical to AGX2). What must remain fail-closed
        // per mode is the firmware-shared state this constructor touches
        // next: the Handoff/FlushInfo protocol is per-firmware-build ABI
        // observed on AGX2 (Dekker) and, separately, observed *absent* on
        // M3.
        let kernel_range = iova_kern_range(cfg)?;
        match handoff_mode {
            HandoffMode::FirmwareT6030 => { if cfg != UatConfig::T6030 {return Err(ENODEV);} }
            HandoffMode::StoppedFirmwareT6030 => {
                if cfg != UatConfig::T6030 { return Err(ENODEV); }
            }
            HandoffMode::FirmwareDekker => {
                if cfg.ias != 39
                    && !AGX3_FIRMWARE_HANDOFF_VALIDATED
                    && !(cfg.chip_id == 0x6030 && G15_DEKKER_HANDOFF_OBSERVED)
                {
                    dev_err!(
                        dev.as_ref(),
                        "MMU: {}-bit UAT geometry is implemented, but the AGX3 firmware Handoff ABI and kernel-VA map are unvalidated\n",
                        cfg.ias
                    );
                    return Err(ENODEV);
                }
            }

        }

        let inner = Self::make_inner(dev, handoff_mode, cfg.chip_id == 0x6030)?;

        let of_node = dev.as_ref().of_node().ok_or(EINVAL)?;
        let res = crate::m3_resources::reserved_resource(&of_node, c_str!("pagetables"))?;
        let ttb1 = res.start();
        let ttb1size: usize = res.size().try_into()?;

        let required_pagetables_size = PAGETABLES_SIZE;
        if ttb1size < required_pagetables_size {
            dev_err!(dev.as_ref(), "MMU: Pagetables region is too small\n");
            return Err(ENOMEM);
        }

        dev_info!(dev.as_ref(), "MMU: Creating kernel page tables\n");
        let kernel_lower_vm = Vm::new(
            dev,
            inner.clone(),
            IOVA_USER_BASE..uat_geometry(cfg)?.user_va_top(),
            cfg,
            None,
            1,
        )?;
        if cfg == UatConfig::T6030 {
            // M3's bootstrap command register lists are fetched through this
            // low root in context zero. It never acquires a VmBind, but its
            // mappings are live: dropping/remapping a command must invalidate
            // context-zero translations before releasing the old GEM pages.
            // Otherwise reusing the VA can fetch a previous command's ASID,
            // register list and CDM after those client mappings are gone.
            kernel_lower_vm.inner.exec_lock(None, false)?.fixed_slot = Some(0);
        }
        let kernel_vm = Vm::new(dev, inner.clone(), kernel_range, cfg, Some(ttb1), 0)?;

        dev_info!(dev.as_ref(), "MMU: Kernel page tables created\n");

        let ttb0 = kernel_lower_vm.ttb();
        let uat = Self {
            dev: dev.into(),
            cfg,
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

        inner.handoff().init(handoff_mode, cfg.chip_id == 0x6030)?;

        dev_info!(dev.as_ref(), "MMU: Initializing TTBs\n");

        let handoff_guard = inner.lock_handoff()?;

        let ttbs = inner.ttbs();

        ttbs[0].ttb0.store(ttb0 | TTBR_VALID, Ordering::SeqCst);
        ttbs[0].ttb1.store(ttb1 | TTBR_VALID, Ordering::SeqCst);

        for ctx in &ttbs[1..] {
            ctx.ttb0.store(0, Ordering::Relaxed);
            ctx.ttb1.store(0, Ordering::Relaxed);
        }

        drop(handoff_guard);

        core::mem::drop(inner);

        dev_info!(dev.as_ref(), "MMU: initialized\n");

        Ok(uat)
    }
}

impl Drop for Uat {
    fn drop(&mut self) {
        if self.cfg == UatConfig::T6030 {
            self.inner.firmware_cache_flush_ready.store(false, Ordering::Release);
            let inner = self.inner.lock();
            let _guard = match inner.lock_handoff() {
                Ok(guard) => guard,
                Err(_) => return, // Faulted VM owners retain their backing.
            };
            for slot in inner.ttbs() {
                slot.ttb0.store(0, Ordering::Release);
                slot.ttb1.store(0, Ordering::Release);
            }
        }

        // Make sure we flush the TLBs
        fence(Ordering::SeqCst);
        mem::tlbi_all();
        mem::sync();
    }
}
