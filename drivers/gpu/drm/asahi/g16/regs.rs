// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G16 MMIO: SGX identification, power, core and status windows, and ASC control.
//!
//! Windows inside `sgx` (accessed through shared [`crate::regs::G16Resources`] runtime-offset
//! accessors): id `+0xd04000` (`+0` version, `+0x10` counts), core `+0xe00000`
//! (`+0x1500` core mask, `+0x1000` pstate), status `+0xc000`
//! (`+0x20`/`+0x120` busy, `+0x1800` bank select, `+0x18c0` fault status, `+0x18c8` fault
//! address >> 6). ASC `+0x44` bit 4 is RUN (mapped from `asc` here; the shared layer does not
//! map it). No blind reads of unpowered blocks: read only after the AGX vote.
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

/// Engine receipts: busy bit and the two fault banks.
#[derive(Copy, Clone, Default)]
pub(crate) struct EngineState {
    /// Bit 0 of `+0xc020 | +0xc120`.
    #[allow(dead_code)]
    pub(crate) busy: u64,
    /// `+0xd8c0` under fault-bank selectors 0 and 1.
    pub(crate) status: [u64; 2],
    /// `+0xd8c8 << 6` under fault-bank selectors 0 and 1.
    pub(crate) address: [u64; 2],
}

impl EngineState {
    /// Fault status of both banks, OR-ed together.
    #[allow(dead_code)]
    pub(crate) fn fault(&self) -> u64 {
        self.status[0] | self.status[1]
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
    /// Host-owned generation register page (SGX +0xd60000).
    generation: Mapping,
}

impl Regs {
    /// Maps the ASC control page and the SGX status window.
    pub(crate) fn new(pdev: &platform::Device<Core<'_>>, res: G16Resources) -> Result<Self> {
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
            generation: window(c_str!("sgx"), 0xd60000)?,
            res,
        })
    }

    /// Reads the identification words and the enabled-core mask (after the AGX vote).
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

    /// Actual cold or running firmware pstate (zero is valid while stopped).
    pub(crate) fn pstate(&self) -> Result<u32> {
        let state = self.res.sgx_read32_at(SGX_CORE + 0x1000)? & 0xf;
        if state > 9 {
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

    /// ASC control word (`asc + 0x44`).
    pub(crate) fn asc_control(&self) -> Result<u32> {
        self.asc.io().try_read32(ASC_CONTROL)
    }

    /// Writes the ASC control word (`asc + 0x44`).
    pub(crate) fn set_asc_control(&self, value: u32) -> Result {
        self.asc.io().try_write32(value, ASC_CONTROL)
    }

    /// Engine receipts, under the device lock: busy is bit 0 of `C020 | C120`; the fault status
    /// is `D8C0` under fault-bank selectors 0 and 1 (`D800`), with the saved selector restored.
    /// 64-bit accesses.
    pub(crate) fn engine_state(&self) -> Result<EngineState> {
        let r = self.status.io();
        let mut state = EngineState {
            busy: (r.try_read64(0x20)? | r.try_read64(0x120)?) & 1,
            ..Default::default()
        };
        let sel = r.try_read64(0x1800)?;
        for bank in 0..2 {
            r.try_write64(bank as u64, 0x1800)?;
            state.status[bank] = r.try_read64(0x18c0)?;
            state.address[bank] = r.try_read64(0x18c8)? << 6;
        }
        r.try_write64(sel, 0x1800)?;
        Ok(state)
    }
}

/// Size of each MMIO window mapped here.
const WINDOW: usize = 0x4000;

pub(crate) const SGX_ID: usize = 0xd04000;

pub(crate) const SGX_CORE: usize = 0xe00000;
pub(crate) const SGX_STATUS: usize = 0xc000;
pub(crate) const ASC_CONTROL: usize = 0x44;
pub(crate) const ASC_RUN: u32 = 1 << 4;
