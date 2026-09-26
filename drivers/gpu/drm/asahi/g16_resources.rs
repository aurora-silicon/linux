// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713 bootloader resource handoff. Reading this metadata does not map memory,
//! claim UAT ownership, authenticate firmware, or grant permission to start it.
//! Firmware ABI admission and an owned runtime are separate steps.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct Region {
    pub(crate) base: u64,
    pub(crate) size: u64,
}

impl Region {
    fn end(self) -> Option<u64> {
        self.base.checked_add(self.size)
    }

    fn valid(self) -> bool {
        self.base != 0
            && self.size != 0
            && (self.base | self.size) & 0x3fff == 0
            && self.end().is_some_and(|end| end <= 1 << 42)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ResourceError {
    Version,
    Region,
    Overlap,
    FirmwareLayout,
}

#[derive(Debug)]
pub(crate) struct Resources {
    /// ttbs, pagetables, handoff, shared-l2, fw-text, fw-data.
    pub(crate) regions: [Region; 6],
    pub(crate) firmware_vas: [u64; 2],
}

impl Resources {
    pub(crate) fn validate(
        version: u32,
        regions: [Region; 6],
        firmware_vas: [u64; 2],
        firmware_flags: [u32; 2],
    ) -> Result<Self, ResourceError> {
        if version != 1 {
            return Err(ResourceError::Version);
        }
        for (i, region) in regions.iter().enumerate() {
            if !region.valid() {
                return Err(ResourceError::Region);
            }
            for other in &regions[..i] {
                if region.base < other.end().unwrap() && other.base < region.end().unwrap() {
                    return Err(ResourceError::Overlap);
                }
            }
        }
        // The inherited 42-bit page-table roots and all 64 16-byte TTBAT
        // entries fit their reserved regions. No panel-sized allocation pool.
        if firmware_flags != [1, 0]
            || firmware_vas[0] != 0xffff_fc00_0000_0000
            || firmware_vas[0].checked_add(regions[4].size) != Some(firmware_vas[1])
            || firmware_vas[1].checked_add(regions[5].size).is_none()
        {
            return Err(ResourceError::FirmwareLayout);
        }
        Ok(Self {
            regions,
            firmware_vas,
        })
    }
}

#[cfg(not(test))]
pub(crate) fn from_device(
    pdev: &kernel::platform::Device<kernel::device::Core>,
) -> kernel::error::Result<Resources> {
    use kernel::{c_str, prelude::*};

    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    let root = kernel::of::root().ok_or(ENODEV)?;
    let board: KVec<u8> = root.get_property(c_str!("compatible"))?;
    let compatible: KVec<u8> = node.get_property(c_str!("compatible"))?;
    if !board.split(|b| *b == 0).any(|s| s == b"apple,j713")
        || !board.split(|b| *b == 0).any(|s| s == b"apple,t8132")
        || compatible.as_slice() != b"apple,agx-t8132\0"
    {
        return Err(ENODEV);
    }

    let names: KVec<u8> = node.get_property(c_str!("memory-region-names"))?;
    let handles: KVec<u32> = node.get_property(c_str!("memory-region"))?;
    let reg_names: KVec<u8> = node.get_property(c_str!("reg-names"))?;
    if names.as_slice() != b"ttbs\0pagetables\0handoff\0shared-l2\0fw-text\0fw-data\0"
        || handles.len() != 6
        || reg_names.as_slice() != b"asc\0sgx\0"
    {
        return Err(EINVAL);
    }
    for (i, handle) in handles.iter().enumerate() {
        if *handle == 0 || handles[..i].contains(handle) {
            return Err(EINVAL);
        }
        let region = node
            .parse_phandle(c_str!("memory-region"), i.try_into()?)
            .ok_or(EINVAL)?;
        let no_map: KVec<u8> = region.get_property(c_str!("no-map"))?;
        if !no_map.is_empty() {
            return Err(EINVAL);
        }
    }
    for (name, base, size) in [
        (c_str!("asc"), 0x302600000, 0x4000),
        (c_str!("sgx"), 0x300000000, 0x4000000),
    ] {
        let res = pdev.resource_by_name(name).ok_or(EINVAL)?;
        if res.start() != base || res.size() != size {
            return Err(EINVAL);
        }
    }

    let mboxes: KVec<u32> = node.get_property(c_str!("mboxes"))?;
    if mboxes.len() != 1 {
        return Err(EINVAL);
    }
    let mbox = node.parse_phandle(c_str!("mboxes"), 0).ok_or(EINVAL)?;
    let compat: KVec<u8> = mbox.get_property(c_str!("compatible"))?;
    let mbox_reg: KVec<u32> = mbox.get_property(c_str!("reg"))?;
    let irq_names: KVec<u8> = mbox.get_property(c_str!("interrupt-names"))?;
    let irqs: KVec<u32> = mbox.get_property(c_str!("interrupts"))?;
    let cells: u32 = mbox.get_property(c_str!("#mbox-cells"))?;
    if compat.as_slice() != b"apple,t8132-asc-mailbox\0apple,asc-mailbox-v4\0"
        || mbox_reg.as_slice() != [3, 0x02608000, 0, 0x4000]
        || cells != 0
        || irq_names.as_slice() != b"send-empty\0send-not-empty\0recv-empty\0recv-not-empty\0"
        || irqs.as_slice() != [0, 993, 4, 0, 994, 4, 0, 995, 4, 0, 996, 4]
    {
        return Err(EINVAL);
    }

    let mut regions = [Region { base: 0, size: 0 }; 6];
    for (i, name) in [
        c_str!("ttbs"),
        c_str!("pagetables"),
        c_str!("handoff"),
        c_str!("shared-l2"),
        c_str!("fw-text"),
        c_str!("fw-data"),
    ]
    .iter()
    .enumerate()
    {
        let res = node.reserved_mem_region_to_resource_byname(name)?;
        regions[i] = Region {
            base: res.start(),
            size: res.size(),
        };
    }
    let version: u32 = node.get_property(c_str!("apple,g16-handoff-version"))?;
    let vas: KVec<u64> = node.get_property(c_str!("apple,firmware-segment-vas"))?;
    let flags: KVec<u32> = node.get_property(c_str!("apple,firmware-segment-flags"))?;
    Resources::validate(
        version,
        regions,
        vas.as_slice().try_into().map_err(|_| EINVAL)?,
        flags.as_slice().try_into().map_err(|_| EINVAL)?,
    )
    .map_err(|_| EINVAL)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn observed() -> [Region; 6] {
        [
            (0x103fffb8000, 0x4000),
            (0x103fff78000, 0x40000),
            (0x103fff70000, 0x4000),
            (0x103fff74000, 0x4000),
            (0x10000cc0000, 0x60000),
            (0x10001df4000, 0x13c000),
        ]
        .map(|(base, size)| Region { base, size })
    }
    const VAS: [u64; 2] = [0xfffffc0000000000, 0xfffffc0000060000];

    #[test]
    fn observed_and_relocated_boot_addresses() {
        let res = Resources::validate(1, observed(), VAS, [1, 0]).unwrap();
        assert_eq!(res.firmware_vas, VAS);
        assert_eq!(res.regions[2].base, 0x103fff70000);
        let relocated = observed().map(|r| Region {
            base: r.base + 0x400000000,
            ..r
        });
        assert!(Resources::validate(1, relocated, VAS, [1, 0]).is_ok());
    }

    #[test]
    fn rejects_missing_overlapping_unaligned_and_out_of_range_memory() {
        for r in [
            Region { base: 0, size: 0 },
            Region {
                base: 0x12345,
                size: 0x4000,
            },
            Region {
                base: 1 << 42,
                size: 0x4000,
            },
            Region {
                base: u64::MAX & !0x3fff,
                size: 0x4000,
            },
        ] {
            let mut bad = observed();
            bad[0] = r;
            assert!(matches!(
                Resources::validate(1, bad, VAS, [1, 0]),
                Err(ResourceError::Region)
            ));
        }
        let mut bad = observed();
        bad[0] = bad[1];
        assert!(matches!(
            Resources::validate(1, bad, VAS, [1, 0]),
            Err(ResourceError::Overlap)
        ));
    }

    #[test]
    fn rejects_unknown_handoff_and_segment_layouts() {
        assert!(matches!(
            Resources::validate(0, observed(), VAS, [1, 0]),
            Err(ResourceError::Version)
        ));
        for (vas, flags) in [
            (VAS, [0, 1]),
            ([VAS[0], VAS[1] + 0x4000], [1, 0]),
            ([0xfffffe0000000000, VAS[1]], [1, 0]),
        ] {
            assert!(matches!(
                Resources::validate(1, observed(), vas, flags),
                Err(ResourceError::FirmwareLayout)
            ));
        }
    }
}
