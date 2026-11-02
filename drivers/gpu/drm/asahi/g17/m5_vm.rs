// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The native M5 address-space policy: the one definition of the ranges
//! the driver keeps apart from Mesa's M5 layout (`src/asahi/lib/agx_m5_va.h`, which names this
//! file), of the driver's fixed objects and allocation domains ([`LAYOUT`]), of what a client VM
//! reserves ([`client_vm`]), and of the kernel lower-root links that exist before RUN. Every range
//! is half-open.
//!
//! A collision is a matter of root and address, not address alone. The firmware's fixed lower
//! objects ([`fixed`]) lie numerically inside Mesa's read-only shadow, but only in the kernel lower
//! root: a client VM has its own lower root and holds only its own queues' aliases, never the
//! canonical subtree. The canonical and client-private arenas are installed into client VMs, so
//! they lie outside every Mesa window.
//!
//! The UAPI has VM bounds and a requested driver reservation but no reserved-hole query. Within
//! the bounds, a client VM rejects binds in its driver reservation, the canonical arena and the
//! client-private arena; the data heap, its shadow and the USC window are the client's.

use core::ops::Range;

use kernel::prelude::*;

use super::fw::m5::initdata as m;
use super::vm_layout::{root_entry, Clients, VmLayout};
use crate::mmu;

const PAGE: u64 = mmu::UAT_PGSZ as u64;

/// Architectural base of the driver window: compact TA, deflake and VDM pointers are offsets from
/// it (`AGX_M5_DRIVER_BASE`). A client's driver reservation starts here.
pub(crate) const DRIVER_BASE: u64 = 0x10_0000_0000;
/// The driver reservation Mesa requests (`AGX_M5_DRIVER_SIZE`, 32 GiB): driver-owned VA in each
/// client VM, never a user bind.
pub(crate) const DRIVER_RESERVATION: Range<u64> = DRIVER_BASE..DRIVER_BASE + 0x8_0000_0000;
/// Compact driver resources: their 32-bit offsets from [`DRIVER_BASE`] reach only the first 4 GiB
/// (`AGX_M5_COMPACT_SIZE`).
const COMPACT: Range<u64> = DRIVER_BASE..DRIVER_BASE + 0x1_0000_0000;
/// Mesa's main data heap (`AGX_M5_DATA_START`) and its read-only shadow 0x20_0000_0000 above it,
/// up to `AGX_M5_DATA_END`. Mesa binds the shadow of every memory allocation.
const DATA: Range<u64> = 0x40_0000_0000..0x60_0000_0000;
const SHADOW: Range<u64> = 0x60_0000_0000..0x80_0000_0000;
/// Mesa's USC window (`AGX_M5_SHADER_BASE`, `AGX_M5_SHADER_SIZE`): helper tables, shaders and
/// their containers, all user mappings.
pub(crate) const USC: Range<u64> = 0x100_0000_0000..0x101_0000_0000;

/// [`super::vm_layout::Domain::Canonical`]: kernel lower-root entry 8, after Mesa's shadow.
const CANONICAL: Range<u64> = 0x80_0000_0000..0x90_0000_0000;
/// [`super::vm_layout::Domain::Private`]: entry 9. It has no kernel-lower mapping and so needs no
/// pre-RUN link.
const CLIENT_PRIVATE: Range<u64> = 0x90_0000_0000..0xa0_0000_0000;
/// [`super::vm_layout::Domain::Upper`] starts 8 GiB into the upper window, past the fixed graph
/// and the firmware register aperture, as linux-aurora's M5 layout does.
const UPPER_OFFSET: u64 = 0x2_0000_0000;

/// Fixed objects of the M5 graph ([`super::initdata`]'s `build_m5` and `prepare_m5_lower`).
pub(crate) mod fixed {
    /// The shared upper window: upper-root entry 2, whose table the firmware publishes and both
    /// roles share.
    pub(crate) const WINDOW: u64 = 0xffff_fc20_0000_0000;
    /// Private cluster of both roles.
    pub(crate) const PRIVATE: u64 = WINDOW + 0x2_0000;
    pub(crate) const REGION_C: u64 = WINDOW + 0x1a_0000;
    /// Primary and secondary InitData roots, 0x8000 apart.
    pub(crate) const ROOTS: u64 = WINDOW + 0x1a_8000;
    pub(crate) const REGION_A: u64 = WINDOW + 0x1b_8000;
    pub(crate) const FWCTL: u64 = WINDOW + 0x1c_0000;
    pub(crate) const QOS: u64 = WINDOW + 0x1c_8000;
    pub(crate) const AON: u64 = WINDOW + 0x1d_0000;
    pub(crate) const KICK_SHARED: u64 = WINDOW + 0x1d_8000;
    /// Shared bundle: hardware data, policy, main configurations, work and control rings.
    pub(crate) const BUNDLE: u64 = WINDOW + 0xc078_8000;
    /// Primary-view sentinel, zero-backed.
    pub(crate) const SENTINEL: u64 = WINDOW + 0xc07e_0000;
    /// Firmware aliases of the four completion rings, [`COMPLETION_STRIDE`] apart.
    pub(crate) const COMPLETION_UPPER: u64 = WINDOW + 0xc080_0000;
    /// Firmware aliases of the parameter-buffer and page-pool (UMA) descriptor tables.
    pub(crate) const PB_UPPER: u64 = WINDOW + 0x15e_0000;
    pub(crate) const UMA_UPPER: u64 = WINDOW + 0x15e_8000;

    /// The bootstrap metrics placeholder of the shared boot path, in the kernel lower root only
    /// (lower-root entry 1). Numerically inside the driver window, but in a different root.
    pub(crate) const BOOT_METRICS: u64 = 0x10_0008_0000;
    pub(crate) const BOOT_METRICS_SIZE: usize = 0x8000;
    /// The completion rings' backing in the kernel lower root (entry 7, with the descriptor
    /// tables at `REGION_VIEW_LOW_ADDRS`). Compute completes into the first.
    pub(crate) const COMPLETION_LOWER: u64 = 0x70_0184_8000;
    pub(crate) const COMPLETION_STRIDE: u64 = 0x2_8000;
}

/// The canonical arena's table link, prepared at its first page: page tables only, no backing.
const PRE_RUN_TABLES: [Range<u64>; 1] = [CANONICAL.start..CANONICAL.start + PAGE];

/// Every kernel lower-root entry the firmware reaches: the bootstrap metrics, the descriptor
/// tables and completion rings, and the canonical arena that queues are later placed in.
const PRE_RUN_LINKS: u64 = {
    let mut links = root_entry(fixed::BOOT_METRICS)
        | root_entry(m::REGION_VIEW_LOW_ADDRS[0])
        | root_entry(m::REGION_VIEW_LOW_ADDRS[1])
        | root_entry(PRE_RUN_TABLES[0].start);
    let mut ring = 0;
    while ring < m::M5_25G76_COMPLETION_QUEUE_COUNT {
        links |= root_entry(fixed::COMPLETION_LOWER + ring as u64 * fixed::COMPLETION_STRIDE);
        ring += 1;
    }
    links
};

/// The native M5 layout.
pub(crate) const LAYOUT: VmLayout = VmLayout {
    upper_offset: UPPER_OFFSET,
    canonical: CANONICAL,
    private: CLIENT_PRIVATE,
    pre_run_tables: &PRE_RUN_TABLES,
    pre_run_links: PRE_RUN_LINKS,
    clients: Clients::M5,
};

const fn disjoint(a: &Range<u64>, b: &Range<u64>) -> bool {
    a.end <= b.start || b.end <= a.start
}

const fn within(inner: &Range<u64>, outer: &Range<u64>) -> bool {
    outer.start <= inner.start && inner.end <= outer.end
}

const _: () = {
    // Mesa's windows, in its order.
    assert!(within(&COMPACT, &DRIVER_RESERVATION));
    assert!(DRIVER_RESERVATION.end <= DATA.start && DATA.end == SHADOW.start);
    assert!(SHADOW.start - DATA.start == 0x20_0000_0000 && SHADOW.end <= USC.start);
    assert!(USC.end <= 1 << 42);
    // The arenas installed into client VMs miss every Mesa window and each other.
    let windows = [DRIVER_RESERVATION, DATA, SHADOW, USC];
    let mut index = 0;
    while index < windows.len() {
        assert!(disjoint(&CANONICAL, &windows[index]));
        assert!(disjoint(&CLIENT_PRIVATE, &windows[index]));
        index += 1;
    }
    assert!(disjoint(&CANONICAL, &CLIENT_PRIVATE));
    // The lower-root links before RUN: entries 1, 7 and 8.
    assert!(PRE_RUN_LINKS == 1 << 1 | 1 << 7 | 1 << 8);
    assert!(root_entry(CLIENT_PRIVATE.start) & PRE_RUN_LINKS == 0);
    // The upper window: fixed graph, register aperture, then the dynamic allocations.
    let rings = m::M5_25G76_COMPLETION_QUEUE_COUNT as u64;
    let completions_end = fixed::COMPLETION_UPPER
        + (rings - 1) * fixed::COMPLETION_STRIDE
        + m::M5_25G76_COMPLETION_BACKING_SIZE as u64;
    assert!(fixed::SENTINEL + PAGE <= fixed::COMPLETION_UPPER);
    assert!(completions_end <= m::HW_REGISTER_APERTURE.start);
    assert!(m::HW_REGISTER_APERTURE.end <= fixed::WINDOW + UPPER_OFFSET);
    assert!(UPPER_OFFSET < 1 << crate::pgtable::UAT_TOP_LEVEL_SHIFT);
};

/// Checks a client's requested driver reservation against the driver window: at [`DRIVER_BASE`],
/// page aligned, at least the UAPI minimum and ending no later than Mesa's data heap. Mesa
/// normally requests [`DRIVER_RESERVATION`].
pub(crate) fn driver_reservation(requested: Range<u64>) -> Result<Range<u64>> {
    if requested.start != DRIVER_BASE
        || requested.end & (PAGE - 1) != 0
        || requested.end > DATA.start
        || requested.end.saturating_sub(requested.start) < crate::file::VM_KERNEL_MIN_SIZE
    {
        return Err(EINVAL);
    }
    Ok(requested)
}

/// Creates an M5 client VM: the one constructor of `Gpu::new_vm` and the arithmetic qualification
/// alike. It takes the requested driver reservation, attaches the VM's status and lifetime owners,
/// and reserves the canonical and client-private arenas against userspace before the VM is
/// returned, even while they hold nothing. Nothing of the A18 Pro's client profile
/// ([`Clients::G17P`]) is inherited: no render-global or metrics alias, no A18 reservation, no
/// context view. A queue installs its own aliases before it is published.
pub(crate) fn client_vm(uat: &mmu::Uat, id: u64, requested: Range<u64>) -> Result<mmu::Vm> {
    let reservation = driver_reservation(requested)?;
    let vm = uat
        .new_vm(id, reservation)?
        .with_status()?
        .without_context_views();
    let mut reserved = KVec::with_capacity(2, GFP_KERNEL)?;
    for arena in [CANONICAL, CLIENT_PRIVATE] {
        reserved.push(arena, GFP_KERNEL)?;
    }
    vm.install_driver_mappings(KVec::new(), reserved)?;
    Ok(vm)
}
