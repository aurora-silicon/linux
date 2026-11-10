// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Module 1134 G17P items read by carried translation and diagnostic code.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.
use core::ops::Range;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
#[cfg(CONFIG_DEV_COREDUMP)]
use core::ptr::NonNull;
#[cfg(CONFIG_DEV_COREDUMP)]
use kernel::{bindings, ffi::c_void};
use kernel::{addr::PhysicalAddr, prelude::*};
use crate::pgtable::{UAT_PGMSK, UAT_PGSZ};
// diagnostic_accounting_clocks names the probe as `pgtable::LiveFirmwareU64Probe`.
use crate::g18::compat::g17p as pgtable;

// Pinned J700 T8140 GFX A000 scheduler root used by the exact EP type-4
// thunk. The extracted image has SHA-256
// 3e2821bb40d3abffe30488a06ede5f523cfa47544d6a0ad7654a08be6b5dc30f.
// Its __DATA vmaddr/size are 0xfffffc000004c000/0x134000, exactly matching
// the J700 gfx-data carveout size. These addresses are diagnostic only.
const G17P_GFX_LINK_DATA_BASE: u64 = 0xffff_fc00_0004_c000;

const G17P_GFX_LINK_DATA_END: u64 = 0xffff_fc00_0018_0000;

const G17P_GFX_PHYSICAL_DATA_BASE: u64 = 0x0000_0100_01d7_c000;

/// Translate a GFX firmware LINK address into the physical address the host
/// can map. Same arithmetic the render status-page probes use.
pub(crate) fn g17p_gfx_link_to_physical(link: u64) -> u64 {
    G17P_GFX_PHYSICAL_DATA_BASE + (link - G17P_GFX_LINK_DATA_BASE)
}

fn g17p_binding_row_address(base: u64, id: u32, stride: u64) -> Option<u64> {
    if stride == 0 || stride & 7 != 0 { return None; }
    let start = base.checked_add(u64::from(id).checked_mul(stride)?)?;
    let end = start.checked_add(stride)?;
    if start < G17P_GFX_LINK_DATA_BASE || start & 7 != 0 || end > G17P_GFX_LINK_DATA_END {
        return None;
    }
    Some(start)
}

/// Explicit diagnostic read of retained GFX accounting clocks. No MMIO and no
/// firmware writes. Caller requires a frozen, nonempty G17 timing capture.
/// Sample timestamps can advance independently; frequency/epoch fields must
/// remain stable throughout the read or the result is rejected.
pub(crate) fn diagnostic_accounting_clocks() -> Result<[u64; 8]> {
    const OFFSETS: [u64; 8] = [
        0x124ad8, 0x124af8, 0x124b18, 0x124b38,
        0x6e598, 0x6e5b0, 0x6e5d0, 0x179320,
    ];
    let read = |index: usize| -> Result<u64> {
        let link = 0xffff_fc00_0000_0000u64.checked_add(OFFSETS[index]).ok_or(EOVERFLOW)?;
        let start = g17p_binding_row_address(link, 0, 8).ok_or(EINVAL)?;
        pgtable::LiveFirmwareU64Probe::new(g17p_gfx_link_to_physical(start))
            .and_then(|probe| probe.observe_mask(u64::MAX, 1))
            .map(|sample| sample.first)
    };
    let mut values = [0; 8];
    for index in 0..8 { values[index] = read(index)?; }
    for index in 4..8 {
        if read(index)? != values[index] { return Err(EAGAIN); }
    }
    Ok(values)
}

/// Platform-derived SKSM scratch offsets within the mapped accelerator
/// register window.
///
/// Apple's host loads these three values from accelerator object fields
/// `+0x10730`, `+0x10734`, and `+0x10738`. Those are object offsets, not SGX
/// MMIO offsets. This type carries the loaded values and no physical base.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmScratchGeometry {
    pub(crate) write0_base: u32,
    pub(crate) write1_base: u32,
    pub(crate) stride: u32,
}

/// The two offsets used by the fixed G17P scratch FIFO slot.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmScratchOffsets {
    pub(crate) write0: u32,
    pub(crate) write1: u32,
}

impl G17SksmScratchGeometry {
    /// Apply the exact host equation for fixed FIFO slot 9.
    ///
    /// The slot is independent of the SKSM queue ID carried in each write.
    pub(crate) const fn fifo_offsets(
        self,
    ) -> core::result::Result<G17SksmScratchOffsets, ConstructionError> {
        const FIFO_SLOT: u32 = 9;

        let fifo_offset = match self.stride.checked_mul(FIFO_SLOT) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };
        let write0 = match self.write0_base.checked_add(fifo_offset) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };
        let write1 = match self.write1_base.checked_add(fifo_offset) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };

        Ok(G17SksmScratchOffsets { write0, write1 })
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum ConstructionError {
    WrongChip,
    WrongDeclaredIdentity,
    WrongHardwareIdentity,
    WrongUatGeometry,
    UndecodableIdentity,
    WrongDecodedIdentity,
    WrongSubmissionBackend,
    NullUatOwner,
    SplitUatOwner,
    WrongRoleTopology,
    WrongRootPlacement,
    WrongSharedClusterSize,
    WrongSksmScratchGeometry,
    WrongSksmQueueGeometry,
    SksmScratchOffsetOverflow,
}

pub(crate) const T8140_TIMESTAMP_ALIAS_RANGE: Range<u64> =
    crate::g18::compat::g17p::CLIENT_LOW_VA_START..0x71_0000_0000;

/// Two GiB boundary inside the existing four GiB runtime reservation. In
/// addition to descriptor arrays, each independently executing CL graph owns
/// a 23-MiB private-memory pool. Keep room for every advertised queue without
/// reducing concurrency; the remaining 2 GiB hold per-VM private aliases.
pub(crate) const CLIENT_LOW_VA_START: u64 = 0x70_8000_0000;

pub(crate) const QID_COUNT: usize = 128;

/// Device-wide QID gauges for `g17p_queue_slots`, indexed by [`Engine`].
/// Kept by every table mutation and undone by `Drop`, so replacing the table
/// at processor stop leaves only the new table's leases counted.
pub(crate) static LEASED: [AtomicU32; 3] = [AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0)];

pub(crate) static PUBLISHED: [AtomicU32; 3] = [AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0)];

/// Most QIDs leased at once since load.
pub(crate) static HIGH_WATER: AtomicU32 = AtomicU32::new(0);

/// `acquire` calls that found no free QID (the ENOSPC of queue creation and
/// of a new render pair).
pub(crate) static EXHAUSTED: AtomicU64 = AtomicU64::new(0);

/// New compute graphs refused to keep [`COMPUTE_RENDER_RESERVE`] QIDs free.
pub(crate) static RESERVE_REFUSED: AtomicU64 = AtomicU64::new(0);

/// QIDs a new compute graph may never take: kept for render pairs.
///
/// A published QID is only returned at processor stop, and a quarantined or
/// exit-killed compute graph is never reused, so compute faults consume the
/// table one QID at a time. Without a reserve the last free QID goes to a
/// compute graph, and from then on every client that needs a new render
/// pair -- every new desktop client -- fails its first render with ENOSPC.
/// With it, exhaustion fails the next compute queue creation instead. 16 is
/// eight render pairs.
pub(crate) const COMPUTE_RENDER_RESERVE: usize = 16;

/// Total QIDs currently leased, all engines.
pub(crate) fn leased_total() -> u32 {
    LEASED.iter().map(|count| count.load(Ordering::Relaxed)).sum()
}

/// bit0: publish a work-tied CLPC deadline control effort at RegionC+0xe4.
pub(crate) const PARITY_CE: u32 = 1 << 0;

/// bit1: native submit-in-progress bracket order (+0x7c, then +0x80).
pub(crate) const PARITY_BRACKET: u32 = 1 << 1;

/// bit2: native-gated 0x87 note inside the bracket (requires bit1).
pub(crate) const PARITY_KICK: u32 = 1 << 2;

pub(crate) const PARITY_MASK_ALL: u32 = PARITY_CE | PARITY_BRACKET | PARITY_KICK;

/// 1.0 in the kext's 16.16 effort encoding; larger values are rejected by
/// `updateCLPCDeadlineInterfaceCE(u32)` (`cmp w1, #0x10, lsl #12; b.ls`).
pub(crate) const CE_ONE: u32 = 0x1_0000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Config {
    pub(crate) mask: u32,
    pub(crate) hold_ns: u64,
    pub(crate) ramp_ns: u64,
    pub(crate) ce_max: u32,
}

pub(crate) const DEFAULT_HOLD_US: u64 = 16_000;

pub(crate) const MIN_HOLD_US: u64 = 100;

pub(crate) const MAX_HOLD_US: u64 = 1_000_000;

pub(crate) const MAX_RAMP_US: u64 = 5_000_000;

pub(crate) const DEFAULT_CONFIG: Config = Config {
    // v84: native CLPC deadline-effort parity (bit0) is the production
    // default (v83 hardware-qualified). Bracket/kick (bits 1/2) stay opt-in.
    mask: PARITY_CE,
    hold_ns: DEFAULT_HOLD_US * 1000,
    ramp_ns: 0,
    ce_max: CE_ONE,
};

fn parse_number(text: &str) -> Option<u64> {
    match text.strip_prefix("0x") {
        Some(hex) => u64::from_str_radix(hex, 16).ok(),
        None => text.parse::<u64>().ok(),
    }
}

/// Parse `MASK [HOLD_US [RAMP_US [CE_MAX]]]`. Omitted fields keep defaults.
pub(crate) fn parse(text: &str) -> Option<Config> {
    let mut fields = text.split_ascii_whitespace();
    let mask = parse_number(fields.next()?)?;
    if mask > u64::from(PARITY_MASK_ALL) {
        return None;
    }
    let mut config = DEFAULT_CONFIG;
    config.mask = mask as u32;
    if config.mask & PARITY_KICK != 0 && config.mask & PARITY_BRACKET == 0 {
        // Native only sends the note from inside updateSubmitInProgress(true).
        return None;
    }
    if let Some(hold) = fields.next() {
        let hold = parse_number(hold)?;
        if !(MIN_HOLD_US..=MAX_HOLD_US).contains(&hold) {
            return None;
        }
        config.hold_ns = hold * 1000;
    }
    if let Some(ramp) = fields.next() {
        let ramp = parse_number(ramp)?;
        if ramp > MAX_RAMP_US {
            return None;
        }
        config.ramp_ns = ramp * 1000;
    }
    if let Some(max) = fields.next() {
        let max = parse_number(max)?;
        if max == 0 || max > u64::from(CE_ONE) {
            return None;
        }
        config.ce_max = max as u32;
    }
    if fields.next().is_some() {
        return None;
    }
    Some(config)
}

#[cfg(CONFIG_DEV_COREDUMP)]
struct PhysPageMapping {
    ptr: NonNull<c_void>,
}

/// A bounded read-only probe for one qword in live firmware-owned physical
/// memory.
///
/// Construction maps the containing page once. [`Self::observe_mask`] then
/// invalidates only the sampled AP cacheline before each load, so a short
/// firmware transition can be observed without remapping, writing, or holding
/// any firmware lock.
#[cfg(CONFIG_DEV_COREDUMP)]
pub(crate) struct LiveFirmwareU64Probe {
    page: PhysPageMapping,
    offset: usize,
}

/// Result of one bounded live-mask observation.
#[cfg(CONFIG_DEV_COREDUMP)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct LiveFirmwareMaskObservation {
    pub(crate) first: u64,
    pub(crate) last: u64,
    pub(crate) observed_or: u64,
    pub(crate) mask_seen: bool,
    pub(crate) polls: usize,
}

#[cfg(CONFIG_DEV_COREDUMP)]
impl LiveFirmwareU64Probe {
    const MAX_POLLS: usize = 256;

    pub(crate) fn new(physical: PhysicalAddr) -> Result<Self> {
        if physical & 7 != 0 {
            return Err(EINVAL);
        }
        let page_mask = UAT_PGMSK as u64;
        let page = PhysPageMapping::new(physical & !page_mask)?;
        let offset = (physical & page_mask) as usize;
        if offset.checked_add(size_of::<u64>()).ok_or(EOVERFLOW)? > UAT_PGSZ {
            return Err(EINVAL);
        }
        Ok(Self { page, offset })
    }

    /// Observe one mask for at most `polls` cache-invalidated qword loads.
    /// DIAGNOSTIC. Set bits in the probed qword; returns (before, after).
    pub(crate) fn set_bits(&self, bits: u64) -> Result<(u64, u64)> {
        self.page.set_bits_u64(self.offset, bits)
    }

    /// DIAGNOSTIC (v93r). `set_bits` on a freshly invalidated line, then
    /// clean it to the point of coherency so the coprocessor sees the bits.
    pub(crate) fn set_bits_visible(&self, bits: u64) -> Result<(u64, u64)> {
        self.page.invalidate_live_range(self.offset, size_of::<u64>())?;
        let result = self.page.set_bits_u64(self.offset, bits)?;
        self.page.clean_written_range(self.offset, size_of::<u64>())?;
        Ok(result)
    }

    /// DIAGNOSTIC. Read `out.len()` consecutive qwords from the probed qword
    /// onwards, within its page, after invalidating their AP cachelines. Not
    /// an atomic snapshot of the run.
    pub(crate) fn read_run(&self, out: &mut [u64]) -> Result {
        let size = out.len().checked_mul(size_of::<u64>()).ok_or(EOVERFLOW)?;
        self.page.invalidate_live_range(self.offset, size)?;
        for (index, value) in out.iter_mut().enumerate() {
            *value = self.page.read_u64(self.offset + index * size_of::<u64>())?;
        }
        Ok(())
    }

    pub(crate) fn observe_mask(
        &self,
        mask: u64,
        polls: usize,
    ) -> Result<LiveFirmwareMaskObservation> {
        if mask == 0 || polls == 0 || polls > Self::MAX_POLLS {
            return Err(EINVAL);
        }

        let mut first = 0;
        let mut last = 0;
        let mut observed_or = 0;
        let mut mask_seen = false;
        let mut completed = 0;
        for index in 0..polls {
            self.page.invalidate_live_range(self.offset, size_of::<u64>())?;
            let value = self.page.read_u64(self.offset)?;
            if index == 0 {
                first = value;
            }
            last = value;
            observed_or |= value;
            completed = index + 1;
            if value & mask != 0 {
                mask_seen = true;
                break;
            }
            core::hint::spin_loop();
        }

        Ok(LiveFirmwareMaskObservation {
            first,
            last,
            observed_or,
            mask_seen,
            polls: completed,
        })
    }
}

#[cfg(CONFIG_DEV_COREDUMP)]
impl PhysPageMapping {
    fn new(phys: PhysicalAddr) -> Result<Self> {
        if phys & UAT_PGMSK as u64 != 0 {
            return Err(EINVAL);
        }
        let ptr = unsafe {
            // SAFETY: the caller supplies a page-aligned physical address
            // obtained from a live firmware UAT PTE. The mapping is used only
            // for bounded reads and is released by Drop.
            bindings::memremap(
                phys,
                UAT_PGSZ,
                bindings::MEMREMAP_WB as _,
            )
        };
        let ptr = NonNull::new(ptr).ok_or_else(|| {
            pr_err!("UAT crash walker: memremap failed for {:#x}\n", phys);
            ENOMEM
        })?;
        Ok(Self { ptr })
    }

    fn invalidate_live_range(&self, offset: usize, size: usize) -> Result {
        let end = offset.checked_add(size).ok_or(EOVERFLOW)?;
        if size == 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        let ctr_el0: u64;
        unsafe {
            // SAFETY: CTR_EL0 is readable at EL1 and reports DminLine as
            // log2(words). The resulting line size controls only bounded cache
            // maintenance on this read-only mapping.
            core::arch::asm!(
                "mrs {ctr}, ctr_el0",
                ctr = out(reg) ctr_el0,
                options(nomem, nostack, preserves_flags),
            );
        }
        let line_size = 4usize
            .checked_shl(((ctr_el0 >> 16) & 0xf) as u32)
            .ok_or(EOVERFLOW)?;
        if !line_size.is_power_of_two() {
            return Err(EINVAL);
        }
        let start_address = self.ptr.as_ptr() as usize + offset;
        let end_address = self.ptr.as_ptr() as usize + end;
        let mut address = start_address & !(line_size - 1);
        unsafe {
            // SAFETY: the page remains mapped for the whole loop. `dc ivac`
            // invalidates AP cachelines only; this diagnostic never writes the
            // firmware-owned page. The barriers order invalidation before the
            // atomic loads in `copy_atomic()`.
            core::arch::asm!("dsb osh", options(nostack, preserves_flags));
            while address < end_address {
                core::arch::asm!(
                    "dc ivac, {address}",
                    address = in(reg) address,
                    options(nostack, preserves_flags),
                );
                address = address.checked_add(line_size).ok_or(EOVERFLOW)?;
            }
            core::arch::asm!("dsb osh", options(nostack, preserves_flags));
        }
        Ok(())
    }

    fn read_u64(&self, offset: usize) -> Result<u64> {
        let end = offset.checked_add(8).ok_or(EOVERFLOW)?;
        if offset & 7 != 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        let pointer = unsafe {
            // SAFETY: offset is qword-aligned and bounded to the mapped page.
            self.ptr.as_ptr().cast::<u8>().add(offset).cast::<AtomicU64>()
        };
        Ok(unsafe { &*pointer }.load(Ordering::Acquire))
    }

    /// DIAGNOSTIC. Set bits in one firmware-DRAM qword.
    ///
    /// The b1 image gates KTrace emission on class bytes at 0x17780c/0x17780d:
    /// bit 0 is the only one this driver has ever had set, which is why the
    /// halt-decision records (0x909 with the scan's own inputs, 0x113 at the
    /// thread-4 wake, 0x105, 0x110) have never appeared in a capture. Arming
    /// them is a read-modify-write of a debug-enable byte the firmware only
    /// reads; it steers no scheduling decision.
    fn set_bits_u64(&self, offset: usize, bits: u64) -> Result<(u64, u64)> {
        let end = offset.checked_add(8).ok_or(EOVERFLOW)?;
        if offset & 7 != 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        let pointer = unsafe {
            // SAFETY: offset is qword-aligned and bounded to the mapped page,
            // exactly as read_u64 checks above.
            self.ptr.as_ptr().cast::<u8>().add(offset).cast::<AtomicU64>()
        };
        let cell = unsafe { &*pointer };
        let before = cell.load(Ordering::Acquire);
        cell.store(before | bits, Ordering::Release);
        let after = cell.load(Ordering::Acquire);
        Ok((before, after))
    }

    /// Clean AP cachelines covering a range this mapping has just written.
    ///
    /// The mapping is write-back, so a store can sit in an AP cacheline the
    /// coprocessor's fetch path will not see. `invalidate_live_range()` above
    /// is the read-side mirror of this; keep the two symmetric.
    fn clean_written_range(&self, offset: usize, size: usize) -> Result {
        let end = offset.checked_add(size).ok_or(EOVERFLOW)?;
        if size == 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        let ctr_el0: u64;
        unsafe {
            // SAFETY: CTR_EL0 is readable at EL1 and reports DminLine as
            // log2(words); the value bounds cache maintenance only.
            core::arch::asm!(
                "mrs {ctr}, ctr_el0",
                ctr = out(reg) ctr_el0,
                options(nomem, nostack, preserves_flags),
            );
        }
        let line_size = 4usize
            .checked_shl(((ctr_el0 >> 16) & 0xf) as u32)
            .ok_or(EOVERFLOW)?;
        if !line_size.is_power_of_two() {
            return Err(EINVAL);
        }
        let start_address = self.ptr.as_ptr() as usize + offset;
        let end_address = self.ptr.as_ptr() as usize + end;
        let mut address = start_address & !(line_size - 1);
        unsafe {
            // SAFETY: the page stays mapped for the whole loop. `dc cvac`
            // cleans AP cachelines to the point of coherency; the barriers
            // order the preceding stores before the clean and the clean before
            // anything the coprocessor is subsequently told to do.
            core::arch::asm!("dsb osh", options(nostack, preserves_flags));
            while address < end_address {
                core::arch::asm!(
                    "dc cvac, {address}",
                    address = in(reg) address,
                    options(nostack, preserves_flags),
                );
                address = address.checked_add(line_size).ok_or(EOVERFLOW)?;
            }
            core::arch::asm!("dsb osh", options(nostack, preserves_flags));
        }
        Ok(())
    }

    fn write_atomic(&self, offset: usize, data: &[u8]) -> Result {
        let end = offset.checked_add(data.len()).ok_or(EOVERFLOW)?;
        if (offset | data.len()) & 7 != 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        for (index, source) in data.chunks_exact(size_of::<u64>()).enumerate() {
            let at = offset + index * size_of::<u64>();
            let pointer = unsafe {
                // SAFETY: `at` is qword-aligned and bounded to the mapped page
                // by the check above.
                self.ptr.as_ptr().cast::<u8>().add(at).cast::<AtomicU64>()
            };
            let mut qword = [0u8; size_of::<u64>()];
            qword.copy_from_slice(source);
            unsafe { &*pointer }.store(u64::from_le_bytes(qword), Ordering::Release);
        }
        self.clean_written_range(offset, data.len())
    }

    fn copy_atomic(&self, offset: usize, out: &mut [u8]) -> Result {
        let end = offset.checked_add(out.len()).ok_or(EOVERFLOW)?;
        if (offset | out.len()) & 7 != 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        for (index, destination) in out.chunks_exact_mut(size_of::<u64>()).enumerate() {
            destination.copy_from_slice(
                &self
                    .read_u64(offset + index * size_of::<u64>())?
                    .to_le_bytes(),
            );
        }
        Ok(())
    }
}

#[cfg(CONFIG_DEV_COREDUMP)]
impl Drop for PhysPageMapping {
    fn drop(&mut self) {
        unsafe {
            // SAFETY: ptr was returned by memremap and has not been unmapped.
            bindings::memunmap(self.ptr.as_ptr());
        }
    }
}
