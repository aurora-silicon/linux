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
