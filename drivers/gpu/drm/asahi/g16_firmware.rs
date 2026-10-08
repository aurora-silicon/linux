// SPDX-License-Identifier: GPL-2.0-only OR MIT


#[cfg(test)]
#[path = "g16_resources.rs"]
mod g16_resources;
#[cfg(not(test))]
use crate::g16_resources;

const TEXT_SIZE: u64 = 0x60000;
const DATA_SIZE: u64 = 0x12c000;
pub(crate) const TEXT_SHA256: [u8; 32] = [
    0x3a, 0x6b, 0x8b, 0x5e, 0x83, 0x38, 0x4d, 0x5c, 0xc1, 0x92, 0x9d, 0x91, 0xb6, 0x36, 0x40, 0xfe,
    0xe8, 0xf5, 0x52, 0xb5, 0x10, 0x8c, 0x4e, 0xa6, 0x20, 0xbe, 0xdb, 0x7b, 0xbc, 0xa0, 0x31, 0x64,
];

pub(crate) struct Image {
    pub(crate) text_size: u64,
    pub(crate) data_size: u64,
    pub(crate) text_sha256: [u8; 32],
    pub(crate) tlv_header: [u8; 8],
    pub(crate) gkts: usize,
    pub(crate) ecap: usize,
}

#[cfg_attr(not(test), allow(dead_code))]
const J613_IMAGE: Image = Image {
    text_size: TEXT_SIZE,
    data_size: DATA_SIZE,
    text_sha256: TEXT_SHA256,
    tlv_header: [0x4c, 0xce, 5, 0, 0x31, 2, 0, 0],
    gkts: 0x5ce4c,
    ecap: 0x5cf2f,
};

fn normalize_boot_entropy(text: &mut [u8], image: &Image) -> bool {
    let fields: [(usize, &[u8; 4]); 2] = [(image.gkts, b"GKTS"), (image.ecap, b"ECAP")];
    if text.len() != image.text_size as usize
        || text[0x22c..0x234] != image.tlv_header
        || fields.iter().any(|(offset, key)| {
            text[*offset..*offset + 4] != **key || text[*offset + 4..*offset + 8] != [8, 0, 0, 0]
        })
    {
        return false;
    }
    for (offset, _) in fields {
        text[offset + 8..offset + 16].fill(0);
    }
    true
}

#[derive(Debug)]
pub(crate) struct Firmware {
    pub(crate) resources: g16_resources::Resources,
    #[cfg(not(test))]
    pub(crate) board: &'static crate::g16_board::Board,
}

impl Firmware {
    pub(crate) const fn version(&self) -> &'static str {
        "RTKit-3255.160.4.release"
    }

    fn matching_layout(resources: &g16_resources::Resources, image: &Image) -> bool {
        resources.regions[4].size == image.text_size
            && resources.regions[5].size == image.data_size
            && resources.firmware_vas == [0xffff_fc00_0000_0000, 0xffff_fc00_0000_0000 + image.text_size]
    }

    #[cfg(test)]
    fn from_digest(resources: g16_resources::Resources, digest: [u8; 32]) -> Option<Self> {
        if !Self::matching_layout(&resources, &J613_IMAGE) || digest != J613_IMAGE.text_sha256 {
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

    let board = crate::g16_board::get()?;
    let f = &board.firmware;
    let image = Image { text_size: f.text_size, data_size: f.data_size, text_sha256: f.text_sha256,
        tlv_header: f.tlv_header, gkts: f.gkts, ecap: f.ecap };
    if !Firmware::matching_layout(&resources, &image) {
        dev_err!(pdev.as_ref(), "G16G: unsupported firmware segment layout\n");
        return Err(ENODEV);
    }
    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    let text = node.reserved_mem_region_to_resource_byname(c_str!("fw-text"))?;
    if text.start() != resources.regions[4].base || text.size() != image.text_size {
        return Err(EINVAL);
    }
    // SAFETY: from_device checked the bootloader's no-map, nonoverlapping
    // firmware __TEXT region and its read-only segment flag. The mapping is
    // used only for hashing here and is dropped before returning. No mutable
    // reference, firmware patch, MMIO write or DMA is exposed by this function.
    let mapping = unsafe { Mem::try_new(text, MemFlag::WB.into()) }?;
    // SAFETY: the mapping covers the complete readable reserved __TEXT.
    let bytes = unsafe { core::slice::from_raw_parts(mapping.ptr(), mapping.size()) };
    let mut canonical = KVec::new();
    canonical.extend_from_slice(bytes, GFP_KERNEL)?;
    if !normalize_boot_entropy(&mut canonical, &image) {
        return Err(ENODEV);
    }
    let mut digest = [0u8; 32];
    // SAFETY: the initialized private copy and distinct digest live for the
    // synchronous SHA-256 call. Normalization never writes loaded firmware.
    unsafe { bindings::sha256(canonical.as_ptr(), canonical.len(), digest.as_mut_ptr()) };
    if digest != image.text_sha256 {
        dev_err!(
            pdev.as_ref(),
            "G16G: {}: unsupported normalized firmware SHA-256 {:02x?}\n",
            board.name, digest
        );
        return Err(ENODEV);
    }
    dev_info!(pdev.as_ref(), "G16G: {} firmware identified ({})\n", board.name, "RTKit-3255.160.4.release");
    Ok(Firmware { resources, board })
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
        bytes[0x22c..0x234].copy_from_slice(&[0x4c, 0xce, 5, 0, 0x31, 2, 0, 0]);
        for (offset, key) in [(0x5ce4c, b"GKTS"), (0x5cf2f, b"ECAP")] {
            bytes[offset..offset + 4].copy_from_slice(key);
            bytes[offset + 4..offset + 8].copy_from_slice(&[8, 0, 0, 0]);
        }
        bytes
    }

    #[test]
    fn only_boot_entropy_is_normalized() {
        let mut a = image_fixture();
        let mut b = a.clone();
        b[J613_IMAGE.gkts+8..J613_IMAGE.gkts+16].fill(1);
        b[J613_IMAGE.ecap+8..J613_IMAGE.ecap+16].fill(2);
        assert!(normalize_boot_entropy(&mut a, &J613_IMAGE));
        assert!(normalize_boot_entropy(&mut b, &J613_IMAGE));
        assert!(a == b);
        b[0x4000] ^= 1;
        assert!(normalize_boot_entropy(&mut b, &J613_IMAGE));
        assert_ne!(a, b, "instruction changes must remain covered by the hash");
    }

    #[test]
    fn malformed_bootarg_headers_reject_without_mutation() {
        for offset in [0x22c, 0x230, J613_IMAGE.gkts, J613_IMAGE.gkts+4, J613_IMAGE.ecap, J613_IMAGE.ecap+4] {
            let mut bytes = image_fixture();
            bytes[offset] ^= 1;
            let original = bytes.clone();
            assert!(!normalize_boot_entropy(&mut bytes, &J613_IMAGE));
            assert!(bytes == original);
        }
        assert!(!normalize_boot_entropy(&mut [0; 64], &J613_IMAGE));
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
