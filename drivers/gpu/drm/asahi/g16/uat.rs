// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The adopted UAT: PPL handoff, TTBAT context 0, the host window and TLB maintenance.
//!
//! The firmware's kernel root (the TABLES region, ADT `gpu-region`) and the shared L2 page are
//! not System RAM, so the shared page-table code cannot walk them. The design:
//!
//! J713 starts with ASC stopped. Its inherited firmware-only tables are admitted before
//! any write. Client roots and the host-window shadow root are allocated in ordinary RAM by
//! shared `mmu::Uat`; only owned RAM leaf tables are linked into reserved shared L2. The cold
//! epoch publishes those links and context 0 before RUN. Later publication uses the bounded
//! PPL handoff, never replaces a foreign link, and cleans tables before they become reachable.
//! Tables remain allocated for the firmware session; uncached mapping retirement clears
//! leaves and performs by-VA TLBI, while cached firmware objects remain in the session arena.

use core::ops::Range;
use core::sync::atomic::Ordering;

use kernel::{device, io::Io, prelude::*, sync::Arc};

use super::hw;
use super::regs::{mb, Mapping};
use crate::driver::AsahiDrmDevice;
use crate::{mmu, pgtable};

/// Entries of a UAT table page.
const ENTRIES: usize = 2048;
/// Entries of the top-level table used with a 42-bit input range.
const ROOT_ENTRIES: usize = 64;
/// Input address mask of a 42-bit UAT.
const IAS_MASK: u64 = (1 << 42) - 1;
/// Valid table/leaf descriptor bits.
const VALID: u64 = 3;
/// Root entry of the host window (2).
const WINDOW_INDEX: usize = ((hw::HOST & IAS_MASK) >> 36) as usize;

/// Root, L2 and L3 indices of `va`.
const fn indices(va: u64) -> [usize; 3] {
    let va = va & IAS_MASK;
    [
        ((va >> 36) & 63) as usize,
        ((va >> 25) & 2047) as usize,
        ((va >> 14) & 2047) as usize,
    ]
}

/// Creates the adopted UAT over the ADT regions (step 2 above).
pub(crate) fn adopt(
    dev: &AsahiDrmDevice,
    cfg: &'static super::Config,
    adt: &super::adt::GpuAdt,
) -> Result<Arc<mmu::Uat>> {
    let geometry = mmu::UatGeometry {
        ias: cfg.uat_ias,
        oas: cfg.uat_oas,
    };
    let regions = mmu::AdoptedRegions {
        handoff: adt.handoff.clone(),
        ttbs: adt.ttbs.clone(),
        kernel_root: adt.tables.start,
    };
    Ok(Arc::new(
        mmu::Uat::new_adopted_with_user_upper(dev, geometry, &regions, false)?,
        GFP_KERNEL,
    )?)
}

/// Runs `f` on the entries of the RAM table page at `pa`.
fn with_table<R>(
    pa: u64,
    count: usize,
    f: impl FnOnce(&[core::sync::atomic::AtomicU64]) -> R,
) -> Result<R> {
    // SAFETY: Every table walked here is a page the shared page-table code allocated in RAM
    // (`Page::alloc_page`) and never frees while the firmware runs (tables are not freed on
    // unmap, and these VMs are retained until reboot).
    unsafe { pgtable::with_root_entries(None, pa, count, f) }
}

/// Cleans `len` bytes of the RAM table page at `pa`, from entry `first`, to the point of
/// coherency.
fn clean_entries(pa: u64, first: usize, count: usize) -> Result {
    if first + count > ENTRIES {
        return Err(EINVAL);
    }
    with_table(pa, ENTRIES, |entries| {
        let start = entries[first..].as_ptr().cast::<u8>();
        pgtable::clean_poc(start, count * 8);
        pgtable::poc_barrier();
    })
}

/// Cleans a whole RAM table page to the point of coherency.
fn clean_page(pa: u64) -> Result {
    clean_entries(pa, 0, ENTRIES)
}

/// Reads entry `index` of the RAM table page at `pa`.
fn entry(pa: u64, index: usize) -> Result<u64> {
    with_table(pa, index + 1, |entries| {
        entries[index].load(Ordering::Acquire)
    })
}

/// Reads the leaf word that translates `va` under the RAM root `root`, or `None` if a level is
/// missing.
pub(crate) fn read_leaf(root: u64, va: u64) -> Result<Option<u64>> {
    let [i0, i1, i2] = indices(va);
    let l1 = entry(root, i0)?;
    if l1 & VALID != VALID {
        return Ok(None);
    }
    let l2 = entry(l1 & hw::PA_MASK, i1)?;
    if l2 & VALID != VALID {
        return Ok(None);
    }
    Ok(Some(entry(l2 & hw::PA_MASK, i2)?))
}

/// Cleans every table of the context-0 lower tree (the kernel lower VM) to the point of
/// coherency: its root, then each second-level and leaf table. Called before TTBAT context 0
/// names the root and after every lower mapping.
pub(crate) fn clean_lower(uat: &mmu::Uat) -> Result {
    let root = uat.kernel_lower_root();
    let mut tables = KVec::new();
    for i in 0..ROOT_ENTRIES {
        let e = entry(root, i)?;
        if e & VALID == VALID {
            tables.push(e & hw::PA_MASK, GFP_KERNEL)?;
        }
    }
    let mut leaves = KVec::new();
    for &l2 in tables.iter() {
        with_table(l2, ENTRIES, |entries| -> Result {
            for e in entries {
                let e = e.load(Ordering::Acquire);
                if e & VALID == VALID {
                    leaves.push(e & hw::PA_MASK, GFP_KERNEL)?;
                }
            }
            Ok(())
        })??;
    }
    // Leaves first, then the tables naming them, then the root.
    for &pa in Iterator::chain(leaves.iter(), tables.iter()) {
        clean_page(pa)?;
    }
    clean_page(root)?;
    mb();
    Ok(())
}

/// Invalidates every TLB entry of the GPU UAT (`TLBI VMALLE1OS`).
pub(crate) fn tlbi_all() {
    crate::mem::sync();
    crate::mem::tlbi_all();
    crate::mem::sync();
}

/// The firmware's view of the host window (root entry 2 -> shared L2) and the PPL handoff.
pub(crate) struct Window {
    /// PPL handoff page.
    handoff: Mapping,
    /// TTBAT page.
    ttbs: Mapping,
    /// First page of the firmware kernel root tables.
    root: Mapping,
    /// The shared L2 page.
    shared_l2: Mapping,
    /// The shared L2 entries last written, and a snapshot buffer of the shadow L2.
    image: KVec<u64>,
    snapshot: KVec<u64>,
    /// No firmware-visible link is written until the explicit stopped bootstrap epoch.
    published: bool,
    /// Observed firmware-owned root words; only the exact qualified forward transition is
    /// accepted after RUN, never a rollback or an unrelated physical table.
    firmware_root: [u64; 2],
    text_pa: u64,
    data_pa: u64,
}

impl Window {
    /// Maps the UAT handoff regions write-combined.
    pub(crate) fn new(adt: &super::adt::GpuAdt) -> Result<Self> {
        let mut window = Self {
            handoff: Mapping::write_combined(adt.handoff.start, hw::HANDOFF_SIZE as usize)?,
            ttbs: Mapping::write_combined(adt.ttbs.start, hw::TTBS_SIZE as usize)?,
            root: Mapping::write_combined(adt.tables.start, hw::TABLES_SIZE as usize)?,
            shared_l2: Mapping::write_combined(adt.shared_l2.start, hw::SHARED_L2_SIZE as usize)?,
            image: KVec::from_elem(0u64, ENTRIES, GFP_KERNEL)?,
            snapshot: KVec::from_elem(0u64, ENTRIES, GFP_KERNEL)?,
            published: false,
            firmware_root: [0; 2],
            text_pa: adt.text.start,
            data_pa: adt.data.start,
        };
        window.admit_cold()?;
        Ok(window)
    }

    /// Copies only the admitted no-map reservations; snapshots are private diagnostics of the
    /// stopped ownership epoch, never a replacement for the boot owner's RUN=0 check.
    fn cold_snapshot(&self) -> Result<KVec<u64>> {
        let words = (hw::TABLES_SIZE / 8) as usize + ENTRIES;
        let mut image = KVec::with_capacity(words, GFP_KERNEL)?;
        for index in 0..(hw::TABLES_SIZE / 8) as usize {
            image.push(self.root.io().try_read64(index * 8)?, GFP_KERNEL)?;
        }
        for index in 0..ENTRIES {
            image.push(self.shared_l2.io().try_read64(index * 8)?, GFP_KERNEL)?;
        }
        Ok(image)
    }

    /// Read-only admission of the inherited firmware graph. Table addresses are resolved only
    /// within the fixed no-map pagetables reservation, never cast to arbitrary RAM pointers.
    fn admit_cold(&mut self) -> Result {
        let snapshot = self.cold_snapshot()?;
        if !self.ttbat_is_clear()? {
            return Err(EBUSY);
        }
        let h = self.handoff.io();
        for offset in [0, 8] {
            let magic = h.try_read64(offset)?;
            if magic != 0 && magic != hw::PPL_MAGIC {
                return Err(EBUSY);
            }
        }
        if h.try_read8(0x10)? != 0 || h.try_read8(0x11)? != 0 {
            return Err(EBUSY);
        }
        for index in 0..65 * 3 {
            if h.try_read64(0x20 + index * 8)? != 0 {
                return Err(EBUSY);
            }
        }
        for index in 0..ENTRIES {
            if self.shared_l2.io().try_read64(index * 8)? != 0 {
                return Err(EBUSY);
            }
        }
        let mut visited = 0u16;
        self.admit_table(self.root.phys(), 0, hw::UPPER, &mut visited)?;
        for table in 0..16 {
            if visited & (1 << table) == 0 {
                for index in 0..ENTRIES {
                    if self
                        .root
                        .io()
                        .try_read64(table * hw::PAGE as usize + index * 8)?
                        != 0
                    {
                        return Err(EINVAL);
                    }
                }
            }
        }
        if snapshot.as_slice() != self.cold_snapshot()?.as_slice() {
            return Err(EAGAIN);
        }
        self.firmware_root = [self.root.io().try_read64(0)?, self.root.io().try_read64(8)?];
        Ok(())
    }

    fn admit_table(&self, pa: u64, level: usize, va: u64, visited: &mut u16) -> Result {
        let offset = pa.checked_sub(self.root.phys()).ok_or(EINVAL)?;
        if level > 2 || offset >= hw::TABLES_SIZE || offset & (hw::PAGE - 1) != 0 {
            return Err(EINVAL);
        }
        let table = offset / hw::PAGE;
        if *visited & (1 << table) != 0 {
            return Err(EINVAL);
        }
        *visited |= 1 << table;
        let count = if level == 0 { ROOT_ENTRIES } else { ENTRIES };
        let shift = [36, 25, 14][level];
        for index in 0..count {
            let pte = self.root.io().try_read64(offset as usize + index * 8)?;
            if pte == 0 {
                continue;
            }
            if level == 0 && index == WINDOW_INDEX {
                if pte != self.shared_l2.phys() | 3 {
                    return Err(EINVAL);
                }
                continue;
            }
            if level < 2 {
                if index != 0 || pte & !hw::PA_MASK != 0x403 {
                    return Err(EINVAL);
                }
                self.admit_table(pte & hw::PA_MASK, level + 1, va, visited)?;
            } else {
                let address = va | ((index as u64) << shift);
                let off = address.checked_sub(hw::UPPER).ok_or(EINVAL)?;
                let expected = if off < 0x60000 {
                    (self.text_pa + off)
                        | if (0x50000..0x58000).contains(&off) {
                            0x483
                        } else {
                            0x4c3
                        }
                } else if off < 0x19c000 {
                    let data_offset = off - 0x60000;
                    (self.data_pa + data_offset)
                        | if data_offset < 0xc000 {
                            0x00200000000004c3
                        } else {
                            0x0460000000000443
                        }
                } else {
                    return Err(EINVAL);
                };
                if pte != expected {
                    return Err(EINVAL);
                }
            }
        }
        // Bits above the 42-bit root index are not part of this firmware's address space.
        if level == 0 {
            for index in count..ENTRIES {
                if self.root.io().try_read64(index * 8)? != 0 {
                    return Err(EINVAL);
                }
            }
        }
        Ok(())
    }

    /// Cold-start check: all 128 TTBAT words are zero.
    pub(crate) fn ttbat_is_clear(&self) -> Result<bool> {
        let t = self.ttbs.io();
        for i in 0..128 {
            if t.try_read64(i * 8)? != 0 {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Publishes the RAM roots and staged host-window links while ASC is stopped. The boot
    /// owner guarantees RUN=0 before this call and starts firmware only after it succeeds.
    pub(crate) fn bootstrap(&mut self, dev: &device::Device, uat: &mmu::Uat) -> Result {
        if self.published {
            return Err(EBUSY);
        }
        // Repeat admission immediately before the first write, after potentially slow GEM
        // allocation. This does not reset or repair another owner's handoff.
        self.admit_cold()?;
        clean_lower(uat)?;
        // Resolve immutable root addresses before taking the shared AP mutex: binding
        // locks precede that mutex throughout the shared MMU.
        let lower = uat.kernel_lower_root();
        let shadow_l2 = Self::shadow_l2(uat)?;
        let handoff = uat.lock_m4_handoff()?;
        let fw_magic = self.handoff.io().try_read64(8)?;
        self.handoff.io().try_write64(hw::PPL_MAGIC, 0)?;
        // Cold admission proved both interests clear. The common guard now owns AP
        // interest; never reset either participant's lock words behind that guard.
        self.handoff.io().try_write32(0xffff, 0x18)?;
        if fw_magic != hw::PPL_MAGIC {
            self.handoff.io().try_write8(1, 0x638)?;
        }
        for index in 0..65 * 3 {
            self.handoff.io().try_write64(0, 0x20 + index * 8)?;
        }
        self.handoff.io().try_write64(0, 0x640)?;
        mb();
        // From the first publication onward the boot owner retains the complete session on
        // failure. No foreign root or existing TTBAT word is rewritten.
        self.published = true;
        let result = (|| {
            self.sync_inner(shadow_l2, None)?;
            self.root
                .io()
                .try_write64(self.shared_l2.phys() | 3, WINDOW_INDEX * 8)?;
            mb();
            self.ttbs.io().try_write64(lower | 1, 0)?;
            self.ttbs.io().try_write64(self.root.phys() | 1, 8)?;
            mb();
            if self.ttbs.io().try_read64(0)? != lower | 1
                || self.ttbs.io().try_read64(8)? != self.root.phys() | 1
            {
                return Err(EIO);
            }
            Ok(())
        })();
        drop(handoff);
        result?;
        tlbi_all();
        dev_info!(
            dev,
            "G16: stopped UAT epoch published, root[0..2]={:#x} {:#x} {:#x}\n",
            self.root.io().try_read64(0)?,
            self.root.io().try_read64(8)?,
            self.root.io().try_read64(16)?
        );
        Ok(())
    }

    /// Confirms the shared root still names the driver's reserved L2 and only recognizes
    /// the qualified firmware-owned forward transitions in root[0..1].
    fn verify_root(&mut self) -> Result {
        for index in 0..2 {
            let observed = self.root.io().try_read64(index * 8)?;
            if observed == self.firmware_root[index] {
                continue;
            }
            let expected = if index == 0 {
                (self.root.phys() + hw::PAGE) | 0x403
            } else {
                (self.root.phys() + 0x10000) | 0x00200000000004cb
            };
            if self.firmware_root[index] != 0 || observed != expected {
                return Err(EIO);
            }
            self.firmware_root[index] = observed;
        }
        let root = self.root.io().try_read64(WINDOW_INDEX * 8)?;
        if root != self.shared_l2.phys() | 3 && root != self.shared_l2.phys() | 0x00200000000004cb {
            return Err(EIO);
        }
        Ok(())
    }

    /// Fixed live ownership checks before publication or polling. Firmware-owned roots may
    /// make only their qualified forward transition; every AP-owned shared-L2 word stays exact.
    pub(crate) fn check(&mut self) -> Result {
        if !self.published {
            return Err(ENODEV);
        }
        self.verify_root()?;
        for (index, &expected) in self.image.iter().enumerate() {
            if expected != 0 && self.shared_l2.io().try_read64(index * 8)? != expected {
                return Err(EIO);
            }
        }
        Ok(())
    }

    /// The RAM L2 table of the shadow window, if anything is mapped in the window.
    fn shadow_l2(uat: &mmu::Uat) -> Result<Option<u64>> {
        let e = entry(uat.kernel_window_root(), WINDOW_INDEX)?;
        Ok((e & VALID == VALID).then_some(e & hw::PA_MASK))
    }

    /// Copies the shadow window's L2 entries into the shared L2 and cleans the leaves of `range`
    /// to the point of coherency, in leaf tables the firmware can already reach. New leaf tables
    /// are always cleaned whole before they are linked.
    pub(crate) fn sync_range(&mut self, uat: &mmu::Uat, range: Range<u64>) -> Result {
        if range.start < hw::HOST || range.end > hw::HOST + hw::HOST_SIZE || range.is_empty() {
            return Err(EINVAL);
        }
        if !self.published {
            // Ordinary page tables already clean every changed leaf. Reserved shared-L2 is
            // untouched until bootstrap; all backing/mappings remain retained by Objects.
            return Ok(());
        }
        let shadow_l2 = Self::shadow_l2(uat)?;
        let handoff = uat.lock_m4_handoff()?;
        let result = self
            .verify_root()
            .and_then(|()| self.sync_inner(shadow_l2, Some(range.clone())));
        drop(handoff);
        result?;
        // This includes global final-level entries. Never replace this with ASID-only TLBI:
        // mapping retirement and address reuse require by-VA invalidation of upper leaves.
        crate::mem::sync();
        crate::mem::tlbi_range(0, range.start as usize, (range.end - range.start) as usize);
        crate::mem::sync();
        Ok(())
    }

    fn sync_inner(&mut self, shadow_l2: Option<u64>, range: Option<Range<u64>>) -> Result {
        let Some(l2) = shadow_l2 else {
            return Ok(());
        };
        let snapshot = &mut self.snapshot;
        with_table(l2, ENTRIES, |entries| {
            for (out, e) in snapshot.iter_mut().zip(entries) {
                *out = e.load(Ordering::Acquire);
            }
        })?;
        let shared = self.shared_l2.io();
        for i in 0..ENTRIES {
            if shared.try_read64(i * 8)? != self.image[i] {
                return Err(EIO);
            }
            let e = self.snapshot[i];
            if e != 0 && (e & !hw::PA_MASK != VALID || self.image[i] != 0 && self.image[i] != e) {
                return Err(EIO);
            }
        }
        for i in 0..ENTRIES {
            let e = self.snapshot[i];
            if e & VALID != VALID {
                if self.image[i] != 0 {
                    // Tables are never freed, so a linked entry cannot disappear.
                    return Err(EIO);
                }
                continue;
            }
            let l3 = e & hw::PA_MASK;
            if self.image[i] != e {
                // A new leaf table: complete it in memory before the firmware can walk into it.
                clean_page(l3)?;
                mb();
                shared.try_write64(e, i * 8)?;
                self.image[i] = e;
                continue;
            }
            match &range {
                None => clean_page(l3)?,
                Some(r) => {
                    let base = hw::HOST + ((i as u64) << 25);
                    let start = r.start.max(base);
                    let end = r.end.min(base + (1 << 25));
                    if start < end {
                        let first = ((start - base) >> 14) as usize;
                        let last = ((end - 1 - base) >> 14) as usize;
                        clean_entries(l3, first, last - first + 1)?;
                    }
                }
            }
        }
        mb();
        Ok(())
    }
}
