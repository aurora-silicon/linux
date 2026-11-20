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
    /// `__TEXT` and `__DATA`.
    pub(crate) firmware_vas: [u64; 2],
    /// Bytes at the end of fw-data that are the read-only `__DATA_SHARED_RO` segment
    /// (handoff version 2, the three-segment layout); 0 for version 1.
    pub(crate) shared_ro_size: u64,
}

impl Resources {
    /// Version 1: `__TEXT;__DATA`, flags `<1 0>`. Version 2: `__TEXT;__DATA;__DATA_SHARED_RO`,
    /// flags `<1 0x10 0>`, with fw-data covering `__DATA` and the read-only tail right after it,
    /// as the boot loader publishes the three-segment layout (G15G on firmware 27.2).
    pub(crate) fn validate_segments(
        version: u32,
        regions: [Region; 6],
        vas: &[u64],
        flags: &[u32],
    ) -> Result<Self, ResourceError> {
        match (version, vas, flags) {
            (1, &[text, data], &[a, b]) => Self::validate(1, regions, [text, data], [a, b]),
            (2, &[text, data, ro], &[1, 0x10, 0]) => {
                let mut res = Self::validate_regions(regions)?;
                let data_size = ro.checked_sub(data).ok_or(ResourceError::FirmwareLayout)?;
                let ro_size = regions[5]
                    .size
                    .checked_sub(data_size)
                    .ok_or(ResourceError::FirmwareLayout)?;
                if text != 0xffff_fc00_0000_0000
                    || text.checked_add(regions[4].size) != Some(data)
                    || data_size == 0
                    || data_size & 0x3fff != 0
                    || ro_size == 0
                    || ro.checked_add(ro_size).is_none()
                {
                    return Err(ResourceError::FirmwareLayout);
                }
                res.firmware_vas = [text, data];
                res.shared_ro_size = ro_size;
                Ok(res)
            }
            (1 | 2, _, _) => Err(ResourceError::FirmwareLayout),
            _ => Err(ResourceError::Version),
        }
    }

    fn validate_regions(regions: [Region; 6]) -> Result<Self, ResourceError> {
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
        Ok(Self {
            regions,
            firmware_vas: [0; 2],
            shared_ro_size: 0,
        })
    }

    pub(crate) fn validate(
        version: u32,
        regions: [Region; 6],
        firmware_vas: [u64; 2],
        firmware_flags: [u32; 2],
    ) -> Result<Self, ResourceError> {
        if version != 1 {
            return Err(ResourceError::Version);
        }
        let mut res = Self::validate_regions(regions)?;
        // The inherited 42-bit page-table roots and all 64 16-byte TTBAT
        // entries fit their reserved regions. No panel-sized allocation pool.
        if firmware_flags != [1, 0]
            || firmware_vas[0] != 0xffff_fc00_0000_0000
            || firmware_vas[0].checked_add(regions[4].size) != Some(firmware_vas[1])
            || firmware_vas[1].checked_add(regions[5].size).is_none()
        {
            return Err(ResourceError::FirmwareLayout);
        }
        res.firmware_vas = firmware_vas;
        Ok(res)
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

    /// Relocating the UAT reservations preserves their sizes and ownership checks.
    #[test]
    fn relocated_uat_regions_keep_firmware_segments() {
        let mut regions = observed();
        for region in &mut regions[..4] {
            region.base -= 0x40000;
        }
        let res = Resources::validate(1, regions, VAS, [1, 0]).unwrap();
        assert_eq!(res.regions[0].base, 0x103fff78000);
        assert_eq!(res.regions[4], observed()[4]);
        assert_eq!(res.regions[5], observed()[5]);
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

    /// The three-segment layout iBoot uses for G15G on 27.2: TEXT 0x64000, DATA 0xb4000 and a
    /// 0x8000 read-only tail right after DATA, which fw-data covers. Addresses as the boot
    /// loader published them on a J615 (M3 Air 15") on the 27.2 stub.
    fn observed_v2() -> [Region; 6] {
        let mut r = [
            (0x101fffb8000, 0x4000),
            (0x101fff78000, 0x40000),
            (0x101fff70000, 0x4000),
            (0x101fff74000, 0x4000),
            (0, 0),
            (0, 0),
        ]
        .map(|(base, size)| Region { base, size });
        r[4] = Region {
            base: 0x10000944000,
            size: 0x64000,
        };
        r[5] = Region {
            base: 0x10001878000,
            size: 0xb4000 + 0x8000,
        };
        r
    }
    const VAS3: [u64; 3] = [0xfffffc0000000000, 0xfffffc0000064000, 0xfffffc0000118000];

    #[test]
    fn version_2_three_segments() {
        let res = Resources::validate_segments(2, observed_v2(), &VAS3, &[1, 0x10, 0]).unwrap();
        assert_eq!(res.firmware_vas, [VAS3[0], VAS3[1]]);
        assert_eq!(res.shared_ro_size, 0x8000);
        // Version 1 through the same entry point is unchanged.
        let v1 = Resources::validate_segments(1, observed(), &VAS, &[1, 0]).unwrap();
        assert_eq!((v1.firmware_vas, v1.shared_ro_size), (VAS, 0));
    }

    #[test]
    fn version_2_refuses_other_shapes() {
        let bad = |version, regions, vas: &[u64], flags: &[u32]| {
            Resources::validate_segments(version, regions, vas, flags).is_err()
        };
        // A version-1 description of three segments, or version 2 with two.
        assert!(bad(1, observed_v2(), &VAS3, &[1, 0x10, 0]));
        assert!(bad(2, observed(), &VAS, &[1, 0]));
        // Flags, a gap between TEXT and DATA, no read-only tail, the tail past fw-data.
        assert!(bad(2, observed_v2(), &VAS3, &[1, 0, 0]));
        assert!(bad(
            2,
            observed_v2(),
            &[VAS3[0], VAS3[1] + 0x4000, VAS3[2]],
            &[1, 0x10, 0]
        ));
        let mut no_tail = observed_v2();
        no_tail[5].size = 0xb4000;
        assert!(bad(2, no_tail, &VAS3, &[1, 0x10, 0]));
        assert!(bad(
            2,
            observed_v2(),
            &[VAS3[0], VAS3[1], VAS3[2] + 0x10000],
            &[1, 0x10, 0]
        ));
        assert!(bad(
            2,
            observed_v2(),
            &[VAS3[0], VAS3[1], VAS3[1]],
            &[1, 0x10, 0]
        ));
        assert!(matches!(
            Resources::validate_segments(3, observed_v2(), &VAS3, &[1, 0x10, 0]),
            Err(ResourceError::Version)
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
