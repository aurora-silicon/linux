// SPDX-License-Identifier: GPL-2.0-only OR MIT


use crate::m3_resources as agx_resources;

/// The layout of a loaded GPU firmware the runtime accepts: segment sizes and VAs, and the
/// per-boot words of the text segment that identification ignores.
#[derive(Debug)]
pub(crate) struct Layout {
    pub(crate) text_size: u64,
    pub(crate) data_size: u64,
    pub(crate) vas: [u64; 2],
    /// Offset of the 8-byte tag that precedes the 8 per-boot bytes zeroed before hashing.
    pub(crate) entropy_tag_offset: usize,
    pub(crate) entropy_tag: [u8; 8],
    /// The firmware version, for logs.
    pub(crate) version: &'static str,
}

pub(crate) static T6030_LAYOUT: Layout = Layout {
    text_size: 0x5c000,
    data_size: 0x114000,
    vas: [0xffff_fc00_0000_0000, 0xffff_fc00_0005_c000],
    entropy_tag_offset: 0x58e88,
    entropy_tag: *b"GKTS\x08\x00\x00\x00",
    version: "RTKit-2419.140.12.release",
};

/// T8122 firmware-compat 14.8.3 segment sizes, virtual addresses and entropy-tag bounds.
pub(crate) static T8122_LAYOUT: Layout = Layout {
    text_size: 0x64000,
    data_size: 0xb0000,
    vas: [0xffff_fc00_0000_0000, 0xffff_fc00_0006_4000],
    entropy_tag_offset: 0x5d54a,
    entropy_tag: *b"GKTS\x08\x00\x00\x00",
    version: "RTKit-2419.140.12.release",
};

pub(crate) const TEXT_SHA256: [u8; 32] = [
    0x11, 0xe4, 0x9f, 0x75, 0xb, 0x67, 0x1a, 0x2b, 0x13, 0xd0, 0x92, 0xdb, 0x1c, 0xbc, 0xa3, 0xa8, 0x77, 0x55, 0x86, 0xd6, 0xb1, 0xe3, 0xaa, 0xe8, 0xa6, 0xa4, 0x64, 0xa7, 0x80, 0x2, 0x93, 0xd5];

fn normalize_boot_entropy(layout: &Layout, text: &mut [u8]) -> bool {
    let tag = layout.entropy_tag_offset;
    if text.len() != layout.text_size as usize {
        return false;
    }
    let Some(end) = tag.checked_add(16) else { return false; };
    let Some(entropy) = text.get_mut(tag..end) else { return false; };
    if entropy[..8] != layout.entropy_tag {
        return false;
    }
    entropy[8..].fill(0);
    true
}

#[derive(Debug)]
pub(crate) struct Firmware {
    pub(crate) resources: agx_resources::Resources,
    /// The identified image.
    pub(crate) image: &'static crate::m3_board::KnownImage,
    /// The accepted layout.
    pub(crate) layout: &'static Layout,
}

impl Firmware {
    pub(crate) const fn version(&self) -> &'static str {
        self.layout.version
    }

    fn matching_layout(layout: &Layout, resources: &agx_resources::Resources) -> bool {
        resources.regions[4].size == layout.text_size
            && resources.regions[5].size == layout.data_size
            && resources.firmware_vas == layout.vas
    }
}

#[cfg(not(test))]
pub(crate) fn identify_loaded(
    pdev: &kernel::platform::Device<kernel::device::Core>,
    soc: &'static crate::m3_soc::Soc,
    resources: agx_resources::Resources,
) -> kernel::error::Result<Firmware> {
    use kernel::{
        bindings, c_str,
        io::mem::{Mem, MemFlag},
        prelude::*,
    };

    let layout = soc.firmware.ok_or(ENODEV)?;
    if !Firmware::matching_layout(layout, &resources) {
        dev_err!(pdev.as_ref(), "M3 {}: unsupported firmware segment layout\n", soc.gpu_name);
        return Err(ENODEV);
    }
    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    let text = agx_resources::reserved_resource(&node, c_str!("fw-text"))?;
    if text.start() != resources.regions[4].base || text.size() != layout.text_size {
        return Err(EINVAL);
    }
    let mapping = unsafe { Mem::try_new(text, MemFlag::WB.into()) }?;
    let bytes = unsafe { core::slice::from_raw_parts(mapping.ptr(), mapping.size()) };
    let mut canonical = KVec::new();
    canonical.extend_from_slice(bytes, GFP_KERNEL)?;
    if !normalize_boot_entropy(layout, &mut canonical) {
        return Err(ENODEV);
    }
    let mut digest = [0u8; 32];
    // SAFETY: the initialized private copy and distinct digest live for the
    // synchronous SHA-256 call. Normalization never writes loaded firmware.
    unsafe { bindings::sha256(canonical.as_ptr(), canonical.len(), digest.as_mut_ptr()) };
    let image = crate::m3_board::identify(pdev.as_ref(), bytes, &digest, soc.images).ok_or(ENODEV)?;
    Ok(Firmware { resources, image, layout })
}

#[cfg(test)]
mod tests {
    use super::*;
    use agx_resources::{Region, Resources};

    #[test]
    fn each_layout_rejects_the_other_chips_segments() {
        for (layout, other) in [(&T6030_LAYOUT, &T8122_LAYOUT), (&T8122_LAYOUT, &T6030_LAYOUT)] {
            let mut regions = [Region { base: 0, size: 0 }; 6];
            regions[4].size = layout.text_size;
            regions[5].size = layout.data_size;
            let mut resources = Resources { regions, firmware_vas: layout.vas };
            assert!(Firmware::matching_layout(layout, &resources));
            assert!(!Firmware::matching_layout(other, &resources));
            resources.firmware_vas[1] += 0x4000;
            assert!(!Firmware::matching_layout(layout, &resources));
        }
    }

    #[test]
    fn entropy_normalization_changes_only_the_eight_boot_bytes() {
        for layout in [&T6030_LAYOUT, &T8122_LAYOUT] {
            let mut text = vec![0xa5; layout.text_size as usize];
            let tag = layout.entropy_tag_offset;
            text[tag..tag + 8].copy_from_slice(&layout.entropy_tag);
            let before = text.clone();
            assert!(normalize_boot_entropy(layout, &mut text));
            assert_eq!(&text[..tag + 8], &before[..tag + 8]);
            assert_eq!(&text[tag + 8..tag + 16], &[0; 8]);
            assert_eq!(&text[tag + 16..], &before[tag + 16..]);
        }
    }

    #[test]
    fn bad_entropy_bounds_tags_and_lengths_do_not_mutate_text() {
        for offset in [8, 16, usize::MAX] {
            let layout = Layout { text_size: 16, entropy_tag_offset: offset, ..T8122_LAYOUT };
            let mut text = vec![0xa5; 16];
            assert!(!normalize_boot_entropy(&layout, &mut text));
            assert_eq!(text, vec![0xa5; 16]);
        }
        let layout = Layout { text_size: 16, entropy_tag_offset: 0, ..T8122_LAYOUT };
        let mut text = vec![0xa5; 16];
        assert!(!normalize_boot_entropy(&layout, &mut text));
        assert_eq!(text, vec![0xa5; 16]);
        text.pop();
        assert!(!normalize_boot_entropy(&layout, &mut text));
        assert_eq!(text, vec![0xa5; 15]);
    }
}
