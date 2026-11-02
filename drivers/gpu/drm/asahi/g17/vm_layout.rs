// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Where the driver places its own objects in the G17 GPU address spaces.
//!
//! A [`VmLayout`] is one firmware profile's allocation contract
//! ([`super::fw::variant::Variant::vm_layout`]): the A18 Pro's ([`crate::hw::t8140::dynamic`]) or
//! the native M5 one ([`super::m5_vm`]). Allocators and aliases name a [`Domain`] and ask the
//! layout for its range; none restates an arena's bounds, so one profile's arenas cannot leak into
//! another's address space contract.

use core::ops::Range;

use kernel::prelude::*;

use crate::mmu;

/// An allocation domain of a [`VmLayout`].
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Domain {
    /// Dynamic firmware objects in the shared upper window, after its fixed graph.
    Upper,
    /// Canonical backing in the kernel lower root, aliased at the same address into each client
    /// VM that uses it: kick and descriptor rings, compute pools, canonical operand state.
    Canonical,
    /// Client-private aliases, placed independently in each client VM: they need not match
    /// across VMs and have no kernel-lower mapping.
    Private,
    /// Compact driver resources of a client VM, which registers name by 32-bit offsets from the
    /// driver base: parameter-buffer blocks, scene scratch, targets and per-pass state.
    Compact,
}

/// How a client VM is constructed.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Clients {
    /// The A18 Pro's: the render-global and metrics aliases, the reservations of
    /// [`crate::hw::t8140::resources`] and the canonical context views
    /// ([`crate::hw::t8140::CONTEXT_VIEWS`]).
    G17P,
    /// The native M5 one ([`super::m5_vm::client_vm`]).
    M5,
}

/// The address-space contract of one firmware profile.
pub(crate) struct VmLayout {
    /// First byte of [`Domain::Upper`], relative to the shared upper window.
    pub(crate) upper_offset: u64,
    /// [`Domain::Canonical`].
    pub(crate) canonical: Range<u64>,
    /// [`Domain::Private`].
    pub(crate) private: Range<u64>,
    /// [`Domain::Compact`].
    pub(crate) compact: Range<u64>,
    /// Kernel-lower spans whose page tables, not leaves, are allocated before either firmware
    /// processor starts: the firmware samples the lower root's top-level entries only then.
    pub(crate) pre_run_tables: &'static [Range<u64>],
    /// Top-level entries of the kernel lower root (bit `n` for entry `n`) that must link a table
    /// before either firmware processor starts; zero where the profile checks none.
    pub(crate) pre_run_links: u64,
    /// How client VMs are constructed.
    pub(crate) clients: Clients,
}

impl VmLayout {
    /// Returns the range of `domain` in a UAT of `geometry`.
    pub(crate) fn range(&self, geometry: mmu::UatGeometry, domain: Domain) -> Range<u64> {
        match domain {
            Domain::Upper => {
                let window = geometry.kernel_range();
                window.start + self.upper_offset..window.end
            }
            Domain::Canonical => self.canonical.clone(),
            Domain::Private => self.private.clone(),
            Domain::Compact => self.compact.clone(),
        }
    }

    /// Allocates the [`Self::pre_run_tables`] and returns the kernel lower root's top-level
    /// links, `None` where the profile checks none. Call before either processor starts. The
    /// tables stay allocated for the UAT's lifetime: unmapping leaves never frees a table.
    pub(crate) fn prepare_lower_root(&self, uat: &mmu::Uat) -> Result<Option<u64>> {
        let lower = uat.kernel_lower_vm();
        for span in self.pre_run_tables {
            lower.prepare_tables(span.clone())?;
        }
        if self.pre_run_links == 0 {
            return Ok(None);
        }
        lower.root_links().map(Some)
    }
}

/// The bit of the kernel lower root's top-level entry that translates `va`
/// ([`VmLayout::pre_run_links`]).
pub(crate) const fn root_entry(va: u64) -> u64 {
    1 << (va >> crate::pgtable::UAT_TOP_LEVEL_SHIFT)
}
