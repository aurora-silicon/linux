// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! J514S resources admitted against the measured M3 boot contract.
pub(crate) use crate::g16_resources::{Region, Resources};

use kernel::{bindings, c_str, io::resource::Resource, of, prelude::*};

/// Live overlays are not inserted in OF's boot-time reserved_mem registry.
/// Keep the ordinary lookup for every static handoff; the J514S experiment
/// can use no-map nodes only if their ranges are outside all Linux System RAM.
pub(crate) fn reserved_resource(node: &of::Node, name: &CStr) -> Result<Resource> {
    if let Ok(r) = node.reserved_mem_region_to_resource_byname(name) {
        return Ok(r);
    }
    if let Some(found) = crate::m3_board::static_region(node, name) {
        return found.map(|(r, _)| r);
    }
    let compatible: KVec<u8> = node.get_property(c_str!("compatible"))?;
    if compatible.as_slice() != b"apple,agx-t6030\0" { return Err(EINVAL); }
    let names: KVec<u8> = node.get_property(c_str!("memory-region-names"))?;
    let index = names.split(|b| *b == 0).position(|n| n == name.to_bytes()).ok_or(EINVAL)?;
    let region = node.parse_phandle(c_str!("memory-region"), index).ok_or(EINVAL)?;
    let nomap: KVec<u8> = region.get_property(c_str!("no-map"))?;
    let reg: KVec<u64> = region.get_property(c_str!("reg"))?;
    if !nomap.is_empty() || reg.len()!=2 { return Err(EINVAL); }
    let (base,size)=(reg[0],reg[1]);
    if base < 1<<40 || size == 0 || (base|size)&0x3fff != 0
        || base.checked_add(size).ok_or(EOVERFLOW)? > 1<<42 { return Err(EINVAL); }
    // SAFETY: read-only resource-tree query with checked numeric bounds.
    if unsafe { bindings::region_intersects(base, size.try_into()?, bindings::IORESOURCE_SYSTEM_RAM as _, 0) } != bindings::REGION_DISJOINT as i32 {
        return Err(EBUSY);
    }
    let raw = bindings::resource {
        start: base, end: base+size-1, flags: bindings::IORESOURCE_MEM as _,
        ..Default::default()
    };
    // SAFETY: Resource is repr(transparent) over Opaque<resource>, which
    // preserves layout. The initialized descriptor is owned, with no pointers
    // into temporary data; its range is checked above.
    Ok(unsafe { core::mem::transmute::<bindings::resource, Resource>(raw) })
}

pub(crate) fn from_device(
    pdev: &kernel::platform::Device<kernel::device::Core>,
) -> kernel::error::Result<Resources> {
    crate::m3_board::admit(pdev)
}
