// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G16 MMIO: SGX identification, power, core and status windows, and ASC control.
//!
//! Windows inside `sgx` (accessed through shared [`crate::regs::G16Resources`] runtime-offset
//! accessors): id `+0xd04000` (`+0` version, `+0x10` counts), core `+0xe00000`
//! (`+0x1500` core mask, `+0x1000` pstate), status `+0xc000`
//! (`+0x20`/`+0x120` busy, `+0x1800` bank select, `+0x18c0` fault status, `+0x18c8` fault
//! address >> 6). ASC `+0x44` bit 4 is RUN (mapped from `asc` here; the shared layer does not
//! map it). No blind reads of unpowered blocks: read only after the AGX vote (25G83) or, on
//! 26B123, after admission holds the GPU's power domain and GFXCAXI2AF is enabled.
//!
//! The status window needs 64-bit stores (the fault bank select), which the shared accessors do
//! not offer, so it is mapped here, as is `asc`. This module also holds the [`Mapping`] type the
//! UAT regions use and the memory barriers.

use kernel::{
    bindings, c_str,
    device::Core,
    io::{Io, Mmio, MmioRaw, Region},
    platform,
    prelude::*,
};

use core::sync::atomic::{AtomicU32, Ordering};

use crate::regs::G16Resources;

/// Write memory barrier (`wmb()`): `dsb st`.
#[inline(always)]
pub(crate) fn wmb() {
    // SAFETY: A barrier has no memory-safety requirements.
    unsafe { core::arch::asm!("dsb st", options(nostack, preserves_flags)) };
}

/// Read memory barrier (`rmb()`): `dsb ld`.
#[inline(always)]
pub(crate) fn rmb() {
    // SAFETY: A barrier has no memory-safety requirements.
    unsafe { core::arch::asm!("dsb ld", options(nostack, preserves_flags)) };
}

/// Full memory barrier (`mb()`): `dsb sy`.
#[inline(always)]
pub(crate) fn mb() {
    // SAFETY: A barrier has no memory-safety requirements.
    unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) };
}

/// How a [`Mapping`] was created.
#[derive(Copy, Clone, PartialEq, Eq)]
enum Kind {
    /// `ioremap`/`ioremap_np`: device memory (registers).
    Device,
    /// `memremap(MEMREMAP_WC)`: normal non-cacheable memory.
    WriteCombined,
}

/// A CPU mapping of a physical range that is not System RAM, accessed with the `readl`/`writel`
/// family (the accessors of [`Mmio`], with their barriers).
///
/// # Invariants
///
/// `raw` describes a live mapping of `raw.size()` bytes created as `kind` says, which is
/// unmapped only on drop.
pub(crate) struct Mapping {
    raw: MmioRaw<Region>,
    phys: u64,
    kind: Kind,
}

// SAFETY: The mapping is plain memory or registers; all accesses are single volatile accesses
// through `Mmio`, and callers serialize the protocols built on them.
unsafe impl Send for Mapping {}
// SAFETY: See `Send`.
unsafe impl Sync for Mapping {}

impl Mapping {
    /// Maps device registers (non-posted if `nonposted`).
    pub(crate) fn device(phys: u64, size: usize, nonposted: bool) -> Result<Self> {
        if size == 0 {
            return Err(EINVAL);
        }
        // SAFETY: Mapping a physical range has no Rust-visible side effects; the range belongs to
        // this device's `reg` resources.
        let addr = unsafe {
            if nonposted {
                bindings::ioremap_np(phys, size)
            } else {
                bindings::ioremap(phys, size)
            }
        };
        if addr.is_null() {
            return Err(ENOMEM);
        }
        Ok(Self {
            raw: MmioRaw::new_region(addr as usize, size)?,
            phys,
            kind: Kind::Device,
        })
    }

    /// Maps memory that is not System RAM write-combined.
    pub(crate) fn write_combined(phys: u64, size: usize) -> Result<Self> {
        if size == 0 {
            return Err(EINVAL);
        }
        // SAFETY: memremap() of a range outside System RAM creates a new mapping; the range is
        // an ADT-described GPU region that the caller admitted.
        let addr = unsafe { bindings::memremap(phys, size, bindings::MEMREMAP_WC as usize) };
        if addr.is_null() {
            return Err(ENOMEM);
        }
        Ok(Self {
            raw: MmioRaw::new_region(addr as usize, size)?,
            phys,
            kind: Kind::WriteCombined,
        })
    }

    /// The `readl`/`writel` accessors of this mapping.
    pub(crate) fn io(&self) -> Mmio<'_, Region> {
        // SAFETY: By the type invariant `raw` describes a live mapping of `maxsize` bytes.
        unsafe { Mmio::from_raw(self.raw) }
    }

    /// Physical address of byte 0.
    pub(crate) fn phys(&self) -> u64 {
        self.phys
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        // SAFETY: By the type invariant the mapping is live and was created as `kind` says.
        unsafe {
            match self.kind {
                Kind::Device => bindings::iounmap(self.raw.addr() as *mut _),
                Kind::WriteCombined => bindings::memunmap(self.raw.addr() as *mut _),
            }
        }
    }
}

/// Identification read at start.
#[derive(Copy, Clone, Default)]
pub(crate) struct Identity {
    pub(crate) version: u32,
    pub(crate) counts: u32,
    /// Raw enabled-core mask and its per-cluster split (10 bits each).
    pub(crate) cores: u32,
    pub(crate) core_masks: [u32; 2],
}

/// Maximum number of fault banks across the supported SGX status windows.
const MAX_FAULT_BANKS: usize = 8;

/// Engine receipts: busy bit and one fault bank.
#[derive(Copy, Clone, Default)]
pub(crate) struct EngineState {
    /// Bit 0 of `+0xc020 | +0xc120`.
    #[allow(dead_code)]
    pub(crate) busy: u64,
    /// The fault-bank selector (`+0xd800`) the status and address were read under.
    pub(crate) bank: u64,
    /// `+0xd8c0`: bit 0 set when the bank holds a fault.
    pub(crate) status: u64,
    /// `+0xd8c8 << 6`.
    pub(crate) address: u64,
}

impl EngineState {
    /// Fault status of the bank read.
    #[allow(dead_code)]
    pub(crate) fn fault(&self) -> u64 {
        self.status
    }
}

/// Fault-register snapshot taken while firmware is halted for recovery.
#[derive(Copy, Clone, Default)]
pub(crate) struct FaultScan {
    /// Bit 0 of `+0xc020 | +0xc120`.
    pub(crate) busy: u64,
    /// Selector (`+0xd800`) observed before scanning and restored afterwards.
    pub(crate) saved_bank: u64,
    bank_count: usize,
    /// Raw status (`+0xd8c0`) and address (`+0xd8c8 << 6`) of each fault bank.
    banks: [EngineState; MAX_FAULT_BANKS],
}

impl FaultScan {
    pub(crate) fn banks(&self) -> &[EngineState] {
        &self.banks[..self.bank_count]
    }

    /// First bank whose latched fault bit (bit 0) is set.
    pub(crate) fn first_latched(&self) -> Option<&EngineState> {
        self.banks().iter().find(|entry| entry.status & 1 != 0)
    }
}

/// G16 register access.
pub(crate) struct Regs {
    /// The shared SGX mapping.
    pub(crate) res: G16Resources,
    /// ASC control (`asc`, 0x4000).
    asc: Mapping,
    /// SGX status window (`sgx + 0xc000`, 0x4000).
    status: Mapping,
    fault_banks: usize,
    /// Host-owned generation register page (SGX +0xd60000).
    generation: Mapping,
    /// GFXCAXI2AF registers (SGX +0x1000000), written only by the 27.2 start.
    axi2af: Option<Mapping>,
    /// Highest firmware performance state the board's table holds: S9 until the table is
    /// known (the ten-row J713/J604 tables), S10 on the eleven-row J773g table.
    top: AtomicU32,
}

impl Regs {
    /// Maps the ASC control page and the SGX status window.
    pub(crate) fn new(
        pdev: &platform::Device<Core<'_>>,
        res: G16Resources,
        cfg: &super::Config,
    ) -> Result<Self> {
        if !(1..=MAX_FAULT_BANKS).contains(&cfg.fault_banks) {
            return Err(EINVAL);
        }
        let window = |name: &CStr, offset: u64| -> Result<Mapping> {
            let r = pdev.resource_by_name(name).ok_or(EINVAL)?;
            let nonposted = r
                .flags()
                .contains(kernel::io::resource::Flags::IORESOURCE_MEM_NONPOSTED);
            if offset + WINDOW as u64 > r.size() {
                return Err(EINVAL);
            }
            Mapping::device(r.start() + offset, WINDOW, nonposted)
        };
        Ok(Self {
            asc: window(c_str!("asc"), 0)?,
            status: window(c_str!("sgx"), SGX_STATUS as u64)?,
            fault_banks: cfg.fault_banks,
            generation: window(c_str!("sgx"), super::hw::HOST_BLOCK)?,
            axi2af: cfg
                .axi2af
                .then(|| window(c_str!("sgx"), super::hw::start_27_2::AXI2AF as u64))
                .transpose()?,
            top: AtomicU32::new(9),
            res,
        })
    }

    /// Physical base of the SGX resource.
    pub(crate) fn sgx_pa(&self) -> u64 {
        self.generation.phys - super::hw::HOST_BLOCK
    }

    /// Reads the identification words and the enabled-core mask (after the AGX vote on 25G83).
    pub(crate) fn identity(&self) -> Result<Identity> {
        let version = self.res.sgx_read32_at(SGX_ID)?;
        let counts = self.res.sgx_read32_at(SGX_ID + 0x10)?;
        let cores = self.res.sgx_read32_at(SGX_CORE + 0x1500)?;
        let mask = (1u32 << super::hw::CLUSTER_BITS) - 1;
        Ok(Identity {
            version,
            counts,
            cores,
            core_masks: [cores & mask, (cores >> super::hw::CLUSTER_BITS) & mask],
        })
    }

    /// Adopts the board's firmware performance table, `rows` rows including off.
    pub(crate) fn set_table(&self, rows: u32) {
        let top = rows
            .saturating_sub(1)
            .clamp(1, super::fw::initdata::PERF_STATES as u32 - 1);
        self.top.store(top, Ordering::Relaxed);
    }

    /// Highest performance state the firmware may run in.
    pub(crate) fn top(&self) -> u32 {
        self.top.load(Ordering::Relaxed)
    }

    /// Actual cold or running firmware pstate (zero is valid while stopped). A state above the
    /// board table's top row is a protocol error.
    pub(crate) fn pstate(&self) -> Result<u32> {
        let state = self.res.sgx_read32_at(SGX_CORE + 0x1000)? & 0xf;
        if state > self.top() {
            Err(super::boot::eproto())
        } else {
            Ok(state)
        }
    }

    /// Publishes the host-owned device-control generation and verifies the receipt.
    pub(crate) fn set_generation(&self, value: u32) -> Result {
        self.generation.io().try_write32(value, 0)?;
        mb();
        if self.generation.io().try_read32(0)? != value {
            return Err(EIO);
        }
        Ok(())
    }

    /// 26B123 before RUN: sets bit 0 of the two GFXCAXI2AF enables, each read first.
    pub(crate) fn enable_axi2af(&self) -> Result {
        let Some(axi2af) = &self.axi2af else {
            return Ok(());
        };
        let r = axi2af.io();
        for offset in super::hw::start_27_2::AXI2AF_ENABLES {
            let value = r.try_read32(offset)?;
            r.try_write32(value | 1, offset)?;
        }
        mb();
        Ok(())
    }

    /// 26B123 after AP power-on and before the GPU endpoints start: reads then zeroes each
    /// host-block control word, and on G16G reads the scratch word and raises the ready word.
    pub(crate) fn publish_host_block(&self, cfg: &super::Config) -> Result<Option<u32>> {
        use super::hw::start_27_2 as s;
        let g = self.generation.io();
        for &offset in cfg.host_block_zero_order {
            let _ = g.try_read32(offset)?;
            g.try_write32(0, offset)?;
        }
        let probe = if cfg.host_block_ready {
            let probe = self.res.sgx_read32_at(s::SCRATCH_PROBE)?;
            let _ = g.try_read32(s::HOST_READY)?;
            g.try_write32(1, s::HOST_READY)?;
            Some(probe)
        } else {
            None
        };
        mb();
        Ok(probe)
    }

    /// ASC control word (`asc + 0x44`).
    pub(crate) fn asc_control(&self) -> Result<u32> {
        self.asc.io().try_read32(ASC_CONTROL)
    }

    /// Writes the ASC control word (`asc + 0x44`).
    pub(crate) fn set_asc_control(&self, value: u32) -> Result {
        self.asc.io().try_write32(value, ASC_CONTROL)
    }

    /// Engine receipts for a running firmware, read only: busy is bit 0 of `C020 | C120`; the
    /// fault status and address are `D8C0`/`D8C8` under the selector `D800` already holds. A
    /// firmware that powers the GPU down while idle writes `D800` itself on the way down, so the
    /// selector is not written here. 64-bit accesses.
    pub(crate) fn engine_state(&self) -> Result<EngineState> {
        let r = self.status.io();
        Ok(EngineState {
            busy: (r.try_read64(0x20)? | r.try_read64(0x120)?) & 1,
            bank: r.try_read64(0x1800)?,
            status: r.try_read64(0x18c0)?,
            address: r.try_read64(0x18c8)? << 6,
        })
    }

    /// Engine receipts for a firmware halted for recovery and waiting on the host: reads all
    /// supported fault banks in order, capturing status and address for every
    /// non-zero bank, and restores the saved selector.
    #[inline(never)]
    pub(crate) fn fault_scan(&self) -> Result<FaultScan> {
        let r = self.status.io();
        let saved = r.try_read64(0x1800)?;
        let busy = (r.try_read64(0x20)? | r.try_read64(0x120)?) & 1;
        let mut scan = FaultScan {
            busy,
            saved_bank: saved,
            bank_count: self.fault_banks,
            banks: [EngineState::default(); MAX_FAULT_BANKS],
        };
        let result = (|| -> Result {
            for (idx, entry) in scan.banks[..scan.bank_count].iter_mut().enumerate() {
                let bank = idx as u64;
                r.try_write64(bank, 0x1800)?;
                let status = r.try_read64(0x18c0)?;
                let address = if status != 0 {
                    r.try_read64(0x18c8)? << 6
                } else {
                    0
                };
                *entry = EngineState {
                    busy,
                    bank,
                    status,
                    address,
                };
            }
            Ok(())
        })();
        let restore = r.try_write64(saved, 0x1800);
        result.and(restore)?;
        Ok(scan)
    }
}

/// Size of each MMIO window mapped here.
const WINDOW: usize = 0x4000;

pub(crate) const SGX_ID: usize = 0xd04000;

pub(crate) const SGX_CORE: usize = 0xe00000;
pub(crate) const SGX_STATUS: usize = 0xc000;
pub(crate) const ASC_CONTROL: usize = 0x44;
pub(crate) const ASC_RUN: u32 = 1 << 4;
