// SPDX-License-Identifier: GPL-2.0-only OR MIT


use crate::m3_resources as g16_resources;

const TEXT_SIZE: u64 = 0x5c000;
const DATA_SIZE: u64 = 0x114000;
pub(crate) const TEXT_SHA256: [u8; 32] = [
    0x11, 0xe4, 0x9f, 0x75, 0xb, 0x67, 0x1a, 0x2b, 0x13, 0xd0, 0x92, 0xdb, 0x1c, 0xbc, 0xa3, 0xa8, 0x77, 0x55, 0x86, 0xd6, 0xb1, 0xe3, 0xaa, 0xe8, 0xa6, 0xa4, 0x64, 0xa7, 0x80, 0x2, 0x93, 0xd5];

fn normalize_boot_entropy(text: &mut [u8]) -> bool {
    if text.len() != TEXT_SIZE as usize || text[0x58e88..0x58e90] != *b"GKTS\x08\x00\x00\x00" { return false; }
    text[0x58e90..0x58e98].fill(0);
    true
}

#[derive(Debug)]
pub(crate) struct Firmware {
    pub(crate) resources: g16_resources::Resources,
    /// The identified image.
    pub(crate) image: &'static crate::m3_board::KnownImage,
}

impl Firmware {
    pub(crate) const fn version(&self) -> &'static str {
        "RTKit-2419.140.12.release"
    }

    fn matching_layout(resources: &g16_resources::Resources) -> bool {
        resources.regions[4].size == TEXT_SIZE
            && resources.regions[5].size == DATA_SIZE
            && resources.firmware_vas == [0xffff_fc00_0000_0000, 0xffff_fc00_0005_c000]
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
        dev_err!(pdev.as_ref(), "M3 G15S: unsupported firmware segment layout\n");
        return Err(ENODEV);
    }
    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    let text = g16_resources::reserved_resource(&node, c_str!("fw-text"))?;
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
    let image = crate::m3_board::identify(pdev.as_ref(), bytes, &digest).ok_or(ENODEV)?;
    Ok(Firmware { resources, image })
}
