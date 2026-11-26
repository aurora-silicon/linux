// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Explicit boot-loader GPU handoff.
//!
//! The boot loader supplies six named no-map reservations and an explicit
//! segment/handoff ABI. Admission checks the reservations before mapping.
//! The boot loader preserves the original ADT in the named phram reservation.
//! The driver copies and parses it before any firmware-visible write.

#[path = "adt/perf.rs"]
pub(crate) mod perf;

use core::ops::Range;
use kernel::{bindings, c_str, device, of, prelude::*};

use super::{boot::glue, hw, Config};

/// Independently owned reservations; none may intersect System RAM or another reservation.
pub(crate) struct GpuAdt {
    pub(crate) ttbs: Range<u64>,
    pub(crate) tables: Range<u64>,
    pub(crate) handoff: Range<u64>,
    pub(crate) shared_l2: Range<u64>,
    pub(crate) text: Range<u64>,
    pub(crate) data: Range<u64>,
    pub(crate) performance: perf::Source,
    /// The firmware layout the boot loader's handoff describes.
    pub(crate) layout: &'static hw::FwLayout,
}

/// Find the one original-ADT phram export, with explicit 64-bit reservation cells.
/// Unlike the GPU's no-map ranges, this reserved boot-loader copy is System RAM and must be
/// mapped WB. Never accept a userspace supplied address or a generic physical-memory fallback.
fn performance_source(regions: &[Range<u64>]) -> Result<(KVVec<u8>, u64)> {
    let root = of::root().ok_or(ENODEV)?;
    let mut source = None;
    for reserved in root.children() {
        let name: KVec<u8> = reserved.get_property(c_str!("name"))?;
        if name.as_slice() != b"reserved-memory\0" {
            continue;
        }
        if reserved.get_property::<u32>(c_str!("#address-cells"))? != 2
            || reserved.get_property::<u32>(c_str!("#size-cells"))? != 2
            || !reserved
                .get_property::<KVec<u8>>(c_str!("ranges"))?
                .is_empty()
        {
            return Err(EINVAL);
        }
        for node in reserved.children() {
            let Some(compatible) = node.get_opt_property::<KVec<u8>>(c_str!("compatible"))? else {
                continue;
            };
            if compatible.as_slice() != b"phram\0" {
                continue;
            }
            if node
                .get_opt_property::<KVec<u8>>(c_str!("label"))?
                .as_deref()
                != Some(b"adt\0")
            {
                continue;
            }
            if source.is_some()
                || node
                    .get_opt_property::<KVec<u8>>(c_str!("reusable"))?
                    .is_some()
                || node
                    .get_opt_property::<KVec<u8>>(c_str!("status"))?
                    .is_some_and(|value| {
                        value.as_slice() != b"okay\0" && value.as_slice() != b"ok\0"
                    })
            {
                return Err(EINVAL);
            }
            let reg: KVec<u64> = node.get_property(c_str!("reg"))?;
            if reg.len() != 2 {
                return Err(EINVAL);
            }
            let (start, size) = (reg[0], reg[1]);
            let end = start.checked_add(size).ok_or(EINVAL)?;
            if start == 0
                || size == 0
                || size > 0x20_0000
                || (start | size) & (hw::PAGE - 1) != 0
                || end - 1 > (hw::PA_MASK | (hw::PAGE - 1))
                || regions
                    .iter()
                    .any(|range| start < range.end && range.start < end)
            {
                return Err(EINVAL);
            }
            source = Some((start, usize::try_from(size)?));
        }
    }
    let (start, size) = source.ok_or(ENODEV)?;
    // SAFETY: The sole accepted DT source above is the bounded reserved original ADT export.
    // WB matches its System RAM mapping; no writes are made through this temporary mapping.
    let mapping = unsafe { bindings::memremap(start, size, bindings::MEMREMAP_WB as usize) };
    if mapping.is_null() {
        return Err(ENOMEM);
    }
    let copy = (|| {
        let mut copy = KVVec::from_elem(0u8, size, GFP_KERNEL)?;
        let mut second = KVVec::from_elem(0u8, size, GFP_KERNEL)?;
        // SAFETY: Both destinations are owned, nonoverlapping allocations of size bytes; the
        // mapping is live for this entire scope. Comparing complete copies refuses mutation.
        unsafe {
            core::ptr::copy_nonoverlapping(mapping.cast::<u8>(), copy.as_mut_ptr(), size);
            core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
            core::ptr::copy_nonoverlapping(mapping.cast::<u8>(), second.as_mut_ptr(), size);
        }
        if copy.as_slice() != second.as_slice() {
            return Err(EAGAIN);
        }
        Ok(copy)
    })();
    // SAFETY: The mapping is not accessed after this call, including allocation-error paths.
    unsafe { bindings::memunmap(mapping) };
    Ok((copy?, start))
}

/// Whether the boot loader selected this backend. Presence is enough to route probe;
/// admission independently validates the property and the complete handoff.
pub(super) fn has_handoff(dev: &device::Device, cfg: &Config) -> bool {
    dev.of_node()
        .is_some_and(|node| !matches!(node.get_opt_property::<u32>(cfg.handoff_property), Ok(None)))
}

/// Whether this backend owns `dev` under `cfg`: the handoff marker is present and, where `cfg`
/// names `firmware_compat` values, the handoff also describes one of its layouts and
/// `apple,firmware-compat` is one of those values. A compatible that another backend also
/// serves (`needs_handoff_marker`) is probed only then.
pub(crate) fn claims(dev: &device::Device, cfg: &Config) -> bool {
    if !has_handoff(dev, cfg) {
        return false;
    }
    if cfg.firmware_compat.is_empty() {
        return true;
    }
    if layout(dev, cfg).is_none() {
        return false;
    }
    let Some(node) = dev.of_node() else {
        return false;
    };
    let compat: Result<KVec<u32>> = node.get_property(c_str!("apple,firmware-compat"));
    compat.is_ok_and(|compat| cfg.firmware_compat.iter().any(|c| compat.as_slice() == c))
}

/// The boot loader's handoff for the loaded firmware: its version, segment VAs and flags.
fn handoff(dev: &device::Device, cfg: &Config) -> Result<(u32, KVec<u64>, KVec<u32>)> {
    let node = dev.of_node().ok_or(EINVAL)?;
    let version: u32 = node.get_property(cfg.handoff_property)?;
    let vas: KVec<u64> = node.get_property(c_str!("apple,firmware-segment-vas"))?;
    let flags: KVec<u32> = node.get_property(c_str!("apple,firmware-segment-flags"))?;
    Ok((version, vas, flags))
}

/// The firmware layout the boot loader's handoff describes, if it describes a known one.
pub(crate) fn layout(dev: &device::Device, cfg: &Config) -> Option<&'static hw::FwLayout> {
    let (version, vas, flags) = handoff(dev, cfg).ok()?;
    cfg.layouts
        .iter()
        .copied()
        .find(|layout| layout.matches(version, &vas, &flags))
}

/// Validates the FDT contract and returns the reserved physical regions, read-only.
#[inline(never)]
pub(crate) fn read(dev: &device::Device, cfg: &Config) -> Result<KBox<GpuAdt>> {
    let node = dev.of_node().ok_or(EINVAL)?;
    let (version, vas, flags) = handoff(dev, cfg)?;
    let names: KVec<u8> = node.get_property(c_str!("memory-region-names"))?;
    let phandles: KVec<u32> = node.get_property(c_str!("memory-region"))?;
    let layout = cfg
        .layouts
        .iter()
        .copied()
        .find(|layout| layout.matches(version, &vas, &flags));
    let Some(layout) = layout else {
        dev_err!(
            dev,
            "G16: unsupported boot-loader handoff (version {}, {} segments)\n",
            version,
            vas.len()
        );
        return Err(EINVAL);
    };
    const CANONICAL: &[u8] = b"ttbs\0pagetables\0handoff\0shared-l2\0fw-text\0fw-data\0";
    // A boot loader that also serves the older M3 runtime names the shared L2 table a second
    // time, first, as "pagetables-l2". Accept that entry only as an alias: it must reference
    // the very reservation "shared-l2" references. Anything else (a second or unknown
    // reservation) keeps the contract refused. The six canonical regions are then looked up
    // and validated by name below, exactly as without the alias.
    const L2_ALIAS: &[u8] = b"pagetables-l2\0";
    let canonical = phandles.len() == 6 && names.as_slice() == CANONICAL;
    let aliased = phandles.len() == 7
        && names.as_slice().len() == L2_ALIAS.len() + CANONICAL.len()
        && names.as_slice().starts_with(L2_ALIAS)
        && &names.as_slice()[L2_ALIAS.len()..] == CANONICAL
        && phandles[0] == phandles[4];
    if !canonical && !aliased {
        dev_err!(dev, "G16: unsupported boot-loader handoff contract\n");
        return Err(EINVAL);
    }
    if aliased {
        dev_info!(dev, "G16: pagetables-l2 is an alias of shared-l2 (same reservation)\n");
    }
    let names = [
        c_str!("ttbs"),
        c_str!("pagetables"),
        c_str!("handoff"),
        c_str!("shared-l2"),
        c_str!("fw-text"),
        c_str!("fw-data"),
    ];
    let sizes = [
        hw::TTBS_SIZE,
        hw::TABLES_SIZE,
        hw::HANDOFF_SIZE,
        hw::SHARED_L2_SIZE,
        layout.text_size,
        layout.data_size,
    ];
    let mut regions: [Range<u64>; 6] = Default::default();
    for (index, (name, expected)) in names.iter().zip(sizes).enumerate() {
        // The glue validates no-map, enabled/non-reusable reservation ownership, resource /
        // reserved_mem equality; RAM disjointness is independently required below.
        let (start, size) = glue::reserved_region(dev, name)?;
        let end = start.checked_add(size).ok_or(EINVAL)?;
        if start == 0
            || size != expected
            || (start | size) & (hw::PAGE - 1) != 0
            || end - 1 > (hw::PA_MASK | (hw::PAGE - 1))
            || !glue::ram_disjoint(start, size)
            || regions[..index]
                .iter()
                .any(|r| start < r.end && r.start < end)
        {
            dev_err!(
                dev,
                "G16: invalid reserved region {} at {:#x}+{:#x}\n",
                name,
                start,
                size
            );
            return Err(EINVAL);
        }
        regions[index] = start..end;
    }
    let (copy, source_pa) = performance_source(&regions)?;
    let performance = perf::parse(&copy, cfg.adt_compatible).map_err(|error| {
        dev_err!(
            dev,
            "M4 original ADT performance data rejected: {:?}\n",
            error
        );
        EINVAL
    })?;
    dev_info!(
        dev,
        "M4 original ADT performance source PA={:#x} bytes={:#x} sha256={} states={}\n",
        source_pa,
        copy.len(),
        crate::fwid::Hex(&crate::fwid::sha256(&copy)),
        performance.count
    );
    let [ttbs, tables, handoff, shared_l2, text, data] = regions;
    Ok(KBox::init(
        init!(GpuAdt {
            ttbs,
            tables,
            handoff,
            shared_l2,
            text,
            data,
            performance,
            layout,
        }),
        GFP_KERNEL,
    )?)
}
