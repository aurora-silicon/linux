// SPDX-License-Identifier: GPL-2.0-only OR MIT


#[cfg(test)]
#[path = "g16_resources.rs"]
mod g16_resources;
#[cfg(not(test))]
use crate::g16_resources;

const TEXT_SIZE: u64 = 0x60000;
const DATA_SIZE: u64 = 0x13c000;
pub(crate) const TEXT_SHA256: [u8; 32] = [
    0x5f, 0x65, 0xf6, 0x14, 0x58, 0x72, 0x54, 0x70, 0xb4, 0xb0, 0xc0, 0x20, 0x28, 0x47, 0x7b, 0xf2,
    0xea, 0x57, 0x3a, 0xfb, 0x4e, 0x0c, 0x49, 0x6e, 0xf9, 0x95, 0xe9, 0x6a, 0xff, 0xb0, 0xd8, 0x6c,
];

fn normalize_boot_entropy(text: &mut [u8]) -> bool {
    const FIELDS: [(usize, &[u8; 4]); 2] = [(0x5c8df, b"GKTS"), (0x5c9c2, b"ECAP")];
    if text.len() != TEXT_SIZE as usize
        || text[0x22c..0x234] != [0xdf, 0xc8, 5, 0, 0x31, 2, 0, 0]
        || FIELDS.iter().any(|(offset, key)| {
            text[*offset..*offset + 4] != **key || text[*offset + 4..*offset + 8] != [8, 0, 0, 0]
        })
    {
        return false;
    }
    for (offset, _) in FIELDS {
        text[offset + 8..offset + 16].fill(0);
    }
    true
}

#[derive(Debug)]
pub(crate) struct Firmware {
    pub(crate) resources: g16_resources::Resources,
}

impl Firmware {
    pub(crate) const fn version(&self) -> &'static str {
        "RTKit-3255.160.4.release"
    }

    fn matching_layout(resources: &g16_resources::Resources) -> bool {
        resources.regions[4].size == TEXT_SIZE
            && resources.regions[5].size == DATA_SIZE
            && resources.firmware_vas == [0xffff_fc00_0000_0000, 0xffff_fc00_0006_0000]
    }

    fn from_digest(resources: g16_resources::Resources, digest: [u8; 32]) -> Option<Self> {
        if !Self::matching_layout(&resources) || digest != TEXT_SHA256 {
            return None;
        }
        Some(Self { resources })
    }
}

#[cfg(not(test))]
pub(crate) fn identify_loaded(
    pdev: &kernel::platform::Device<kernel::device::Core>,
    resources: g16_resources::Resources,
) -> kernel::error::Result<Firmware> {
    use kernel::{
        bindings, c_str,
        io::mem::{Mem, MemFlag},
        prelude::*,
    };

    if !Firmware::matching_layout(&resources) {
        dev_err!(pdev.as_ref(), "G16G: unsupported firmware segment layout\n");
        return Err(ENODEV);
    }
    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    let text = node.reserved_mem_region_to_resource_byname(c_str!("fw-text"))?;
    if text.start() != resources.regions[4].base || text.size() != TEXT_SIZE {
        return Err(EINVAL);
    }
    let mapping = unsafe { Mem::try_new(text, MemFlag::WB.into()) }?;
    let bytes = unsafe { core::slice::from_raw_parts(mapping.ptr(), mapping.size()) };
    let mut canonical = KVec::new();
    canonical.extend_from_slice(bytes, GFP_KERNEL)?;
    if !normalize_boot_entropy(&mut canonical) {
        return Err(ENODEV);
    }
    let mut digest = [0u8; 32];
    // SAFETY: the initialized private copy and distinct digest live for the
    // synchronous SHA-256 call. Normalization never writes loaded firmware.
    unsafe { bindings::sha256(canonical.as_ptr(), canonical.len(), digest.as_mut_ptr()) };
    if digest != TEXT_SHA256 {
        dev_err!(
            pdev.as_ref(),
            "G16G: unsupported normalized firmware SHA-256 {:02x?}\n",
            digest
        );
    }
    Firmware::from_digest(resources, digest).ok_or(ENODEV)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn resources(delta: u64) -> g16_resources::Resources {
        let regions = [
            (0x103fffb8000, 0x4000),
            (0x103fff78000, 0x40000),
            (0x103fff70000, 0x4000),
            (0x103fff74000, 0x4000),
            (0x10000cc0000, TEXT_SIZE),
            (0x10001df4000, DATA_SIZE),
        ]
        .map(|(base, size)| g16_resources::Region {
            base: base + delta,
            size,
        });
        g16_resources::Resources::validate(
            1,
            regions,
            [0xffff_fc00_0000_0000, 0xffff_fc00_0006_0000],
            [1, 0],
        )
        .unwrap()
    }

    fn image_fixture() -> Vec<u8> {
        let mut bytes = vec![0xa5; TEXT_SIZE as usize];
        bytes[0x22c..0x234].copy_from_slice(&[0xdf, 0xc8, 5, 0, 0x31, 2, 0, 0]);
        for (offset, key) in [(0x5c8df, b"GKTS"), (0x5c9c2, b"ECAP")] {
            bytes[offset..offset + 4].copy_from_slice(key);
            bytes[offset + 4..offset + 8].copy_from_slice(&[8, 0, 0, 0]);
        }
        bytes
    }

    #[test]
    fn only_boot_entropy_is_normalized() {
        let mut a = image_fixture();
        let mut b = a.clone();
        b[0x5c8e7..0x5c8ef].fill(1);
        b[0x5c9ca..0x5c9d2].fill(2);
        assert!(normalize_boot_entropy(&mut a));
        assert!(normalize_boot_entropy(&mut b));
        assert_eq!(a, b);
        b[0x4000] ^= 1;
        assert!(normalize_boot_entropy(&mut b));
        assert_ne!(a, b, "instruction changes must remain covered by the hash");
    }

    #[test]
    fn malformed_bootarg_headers_reject_without_mutation() {
        for offset in [0x22c, 0x230, 0x5c8df, 0x5c8e3, 0x5c9c2, 0x5c9c6] {
            let mut bytes = image_fixture();
            bytes[offset] ^= 1;
            let original = bytes.clone();
            assert!(!normalize_boot_entropy(&mut bytes));
            assert_eq!(bytes, original);
        }
        assert!(!normalize_boot_entropy(&mut [0; 64]));
    }

    #[test]
    fn firmware_identity_survives_physical_relocation() {
        for delta in [0, 0x400000000] {
            let fw = Firmware::from_digest(resources(delta), TEXT_SHA256).unwrap();
            assert_eq!(fw.version(), "RTKit-3255.160.4.release");
            assert_eq!(fw.resources.regions[4].base, 0x10000cc0000 + delta);
        }
    }

    #[test]
    fn unknown_code_and_different_layout_cannot_select_this_abi() {
        let mut changed = TEXT_SHA256;
        changed[0] ^= 1;
        assert!(Firmware::from_digest(resources(0), changed).is_none());
        let mut resized = resources(0);
        resized.regions[5].size += 0x4000;
        assert!(Firmware::from_digest(resized, TEXT_SHA256).is_none());
    }
}
