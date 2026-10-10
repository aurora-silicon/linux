// SPDX-License-Identifier: GPL-2.0-only OR MIT


#[cfg(test)]
#[path = "g16_resources.rs"]
mod g16_resources;
#[cfg(not(test))]
use crate::g16_resources;

const TEXT_SIZE: u64 = 0x60000;
const DATA_SIZE: u64 = 0x12c000;
/// The loaded 25G83 G15G C0 __TEXT (82C6019B-BE1A-3338-9DB9-30C20BBBCEB5)
/// with its boot entropy (GKTS, ECAP) and the four physical GPU carveout bases
/// in its patchbay (BtpG, BPTP, BlpP, S2xG) zeroed. iBoot puts the carveouts at
/// the top of DRAM, so they move with installed RAM; each is first checked
/// against the region m1n1 reserved for it (`normalize_carveouts`). Every other
/// byte stays pinned, including the rest of the patchbay and iBoot's GPU
/// setup records. Firmware identity is independent of the four physical
/// carveout bases, which must match this boot's reservations.
pub(crate) const TEXT_SHA256: [u8; 32] = [
    0x2d, 0x18, 0x02, 0x2c, 0xbc, 0xd2, 0x34, 0x3c, 0xd7, 0x47, 0xaa, 0xd0, 0x43, 0xd8, 0xdc, 0xdc,
    0x4a, 0x3b, 0x7e, 0xd3, 0xec, 0x16, 0x58, 0x45, 0x23, 0x6d, 0x44, 0xd3, 0x67, 0x0f, 0x9e, 0x96,
];
/// The same image with everything iBoot writes into it zeroed (the whole
/// patchbay and `SETUP_WRITES`). The c000 __TEXT of Apple's 26.6.2
/// Firmware/agx/armfw_g15g.im4p has this digest after the same normalization.
/// This diagnostic digest never admits an image; it distinguishes changes
/// in iBoot-written values from changes in the firmware code.
#[cfg_attr(test, allow(dead_code))]
const IMAGE_FILE_TEXT_SHA256: [u8; 32] = [
    0xc7, 0xec, 0x1a, 0xb3, 0x31, 0x8b, 0xe5, 0xa1, 0x0d, 0x4a, 0x2e, 0xeb, 0x5e, 0x25, 0x77, 0xbd,
    0xc3, 0xd4, 0xd1, 0xa5, 0xc0, 0xe9, 0x49, 0x06, 0xd3, 0xe5, 0x1b, 0xab, 0x22, 0x2e, 0xc1, 0x95,
];
/// The image-info header at __TEXT+0x200: words 0-2 (branch, 'uuid', version
/// 5) and 11-13 (patchbay offset, patchbay size, text size) of the 25G83 C0
/// image; its UUID is words 5-8.
const INFO_HEADER: usize = 0x200;
const INFO_WORDS: [(usize, u32); 6] = [(0, 0x1400021a), (1, 0x6469_7575), (2, 5),
    (11, 0x5ce4c), (12, 0x231), (13, 0x60000)];
const IMAGE_UUID: [u8; 16] = [0x82, 0xc6, 0x01, 0x9b, 0xbe, 0x1a, 0x33, 0x38,
    0x9d, 0xb9, 0x30, 0xc2, 0x0b, 0xbb, 0xce, 0xb5];
/// The tagged values iBoot fills at load (tag, u32 length, value).
const PATCHBAY: core::ops::Range<usize> = 0x5ce4c..0x5ce4c + 0x231;
/// iBoot's other writes: header words 15-16 and the GPU setup records after
/// the patchbay (placeholders in the file). Pinned; used only by
/// `normalize_iboot_writes`.
const SETUP_WRITES: [core::ops::Range<usize>; 2] = [0x23c..0x244, 0x5d07d..0x5d630];

fn le32(text: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(text.get(offset..offset + 4)?.try_into().ok()?))
}

/// Check the image-info header, walk the patchbay to its exact end, require
/// each carveout and layout value in it to be the one this boot reserved
/// (`regions`: ttbs, pagetables, handoff, shared-l2 as base and size), then
/// zero the four carveout bases. SPTP and BVTP are checked and stay pinned.
/// On any refusal the copy is left as it was.
fn normalize_carveouts(text: &mut [u8], regions: &[(u64, u64); 4], firmware_va: u64) -> bool {
    if text.len() != TEXT_SIZE as usize
        || INFO_WORDS.iter().any(|&(i, v)| le32(text, INFO_HEADER + 4 * i) != Some(v))
        || text[INFO_HEADER + 0x14..INFO_HEADER + 0x24] != IMAGE_UUID
    {
        return false;
    }
    // Tag (as stored), the value it must hold, whether it is zeroed, and
    // where its value was found (each exactly once).
    let mut expected: [(&[u8; 4], u64, bool, Option<usize>); 6] = [
        (b"BtpG", regions[0].0, true, None),  // ttbs
        (b"BPTP", regions[1].0, true, None),  // pagetables
        (b"SPTP", regions[1].1, false, None), // pagetables size
        (b"BlpP", regions[2].0, true, None),  // handoff
        (b"S2xG", regions[3].0, true, None),  // shared-l2
        (b"BVTP", firmware_va, false, None),  // firmware __TEXT VA
    ];
    let mut at = PATCHBAY.start;
    while at < PATCHBAY.end {
        let Some(length) = le32(text, at + 4) else { return false };
        let value = at + 8;
        let Some(end) = value.checked_add(length as usize) else { return false };
        if end > PATCHBAY.end {
            return false;
        }
        for item in expected.iter_mut() {
            if text[at..at + 4] == item.0[..] {
                let Ok(bytes) = <[u8; 8]>::try_from(&text[value..end]) else { return false };
                if u64::from_le_bytes(bytes) != item.1 || item.3.is_some() {
                    return false;
                }
                item.3 = Some(value);
            }
        }
        at = end;
    }
    if at != PATCHBAY.end || expected.iter().any(|item| item.3.is_none()) {
        return false;
    }
    for (_, _, zero, value) in expected {
        if let (true, Some(value)) = (zero, value) {
            text[value..value + 8].fill(0);
        }
    }
    true
}

/// Zero everything iBoot writes into the image (diagnosis only, after
/// `normalize_carveouts`): what is left of the 26.6.2 image is the file's.
#[cfg_attr(test, allow(dead_code))]
fn normalize_iboot_writes(text: &mut [u8]) -> bool {
    if text.len() != TEXT_SIZE as usize {
        return false;
    }
    text[PATCHBAY].fill(0);
    for range in SETUP_WRITES {
        text[range].fill(0);
    }
    true
}

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
    let r = &resources.regions;
    let regions = [(r[0].base, r[0].size), (r[1].base, r[1].size),
        (r[2].base, r[2].size), (r[3].base, r[3].size)];
    if !normalize_carveouts(&mut canonical, &regions, resources.firmware_vas[0]) {
        dev_err!(pdev.as_ref(),
            "G16G: {}: firmware image header or patchbay carveouts differ from this boot's reservations\n",
            board.name);
        return Err(ENODEV);
    }
    let mut digest = [0u8; 32];
    // SAFETY: the initialized private copy and distinct digest live for the
    // synchronous SHA-256 call. Normalization never writes loaded firmware.
    unsafe { bindings::sha256(canonical.as_ptr(), canonical.len(), digest.as_mut_ptr()) };
    if digest != image.text_sha256 {
        let mut file = [0u8; 32];
        if normalize_iboot_writes(&mut canonical) {
            // SAFETY: as above.
            unsafe { bindings::sha256(canonical.as_ptr(), canonical.len(), file.as_mut_ptr()) };
        }
        if file == IMAGE_FILE_TEXT_SHA256 {
            dev_err!(
                pdev.as_ref(),
                "G16G: {}: the 26.6.2 GPU image, but iBoot wrote other values into it than on the Mac it was pinned on (normalized SHA-256 {:02x?}); not started, please report this line\n",
                board.name, digest
            );
        } else {
            dev_err!(
                pdev.as_ref(),
                "G16G: {}: unsupported normalized firmware SHA-256 {:02x?}\n",
                board.name, digest
            );
        }
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

    /// The 25G83 C0 patchbay's tags and lengths, in order.
    const PATCHBAY_TAGS: [(&[u8; 4], usize); 39] = [(b"GKTS", 8), (b"BPxG", 8), (b"SPxG", 8),
        (b"DILS", 8), (b"SRSA", 8), (b"GFCM", 8), (b"SSSC", 8), (b"qF8v", 4), (b"LRSD", 1),
        (b"SVSD", 8), (b"LCSD", 8), (b"ZSTR", 4), (b"BPTP", 8), (b"TNGI", 1), (b"oeNS", 1),
        (b"SEBC", 8), (b"ECAP", 8), (b"BVTP", 8), (b"sP1T", 8), (b"BtpG", 8), (b"StpG", 8),
        (b"BlpP", 8), (b"S2xG", 8), (b"SPTP", 8), (b"SZSD", 8), (b"LZSD", 8), (b"LLTR", 1),
        (b"SNUT", 8), (b"ZNUT", 4), (b"OTTR", 8), (b"ARcM", 4), (b"_COS", 4), (b"RCOS", 4),
        (b"dApC", 8), (b"dArW", 8), (b"fVED", 1), (b"GLFp", 8), (b"ABOI", 8), (b"ZSOI", 4)];
    const VA: u64 = 0xffff_fc00_0000_0000;
    /// Synthetic ttbs, pagetables, handoff and shared-l2 regions for 16 GiB.
    const GIB16: [(u64, u64); 4] = [(0x103fffb8000, 0x4000), (0x103fff78000, 0x40000),
        (0x103fff70000, 0x4000), (0x103fff74000, 0x4000)];

    /// The same carveouts below the top of 8 GiB (`-8`) or 24 GiB (`8`) of DRAM.
    fn moved(gib: i64) -> [(u64, u64); 4] {
        GIB16.map(|(base, size)| ((base as i64 + (gib << 30)) as u64, size))
    }

    fn patchbay_fixture(regions: &[(u64, u64); 4]) -> Vec<u8> {
        let mut bytes = image_fixture();
        for &(i, v) in INFO_WORDS.iter() {
            bytes[INFO_HEADER + 4 * i..INFO_HEADER + 4 * i + 4].copy_from_slice(&v.to_le_bytes());
        }
        bytes[INFO_HEADER + 0x14..INFO_HEADER + 0x24].copy_from_slice(&IMAGE_UUID);
        let mut at = PATCHBAY.start;
        for (tag, len) in PATCHBAY_TAGS {
            bytes[at..at + 4].copy_from_slice(tag);
            bytes[at + 4..at + 8].copy_from_slice(&(len as u32).to_le_bytes());
            let value: u64 = match tag {
                b"BtpG" => regions[0].0, b"BPTP" => regions[1].0, b"SPTP" => regions[1].1,
                b"BlpP" => regions[2].0, b"S2xG" => regions[3].0, b"BVTP" => VA,
                _ => 0x5a5a_5a5a_5a5a_5a5a,
            };
            bytes[at + 8..at + 8 + len].copy_from_slice(&value.to_le_bytes()[..len]);
            at += 8 + len;
        }
        assert_eq!(at, PATCHBAY.end);
        bytes
    }

    fn canonical(mut bytes: Vec<u8>, regions: &[(u64, u64); 4]) -> Option<Vec<u8>> {
        (normalize_boot_entropy(&mut bytes, &J613_IMAGE) && normalize_carveouts(&mut bytes, regions, VA))
            .then_some(bytes)
    }

    #[test]
    fn fixture_matches_the_j613_record() {
        assert_eq!(PATCHBAY.start, J613_IMAGE.gkts);
        let bytes = patchbay_fixture(&GIB16);
        assert_eq!(&bytes[J613_IMAGE.ecap..J613_IMAGE.ecap + 4], b"ECAP");
        assert_eq!(bytes[0x22c..0x234], J613_IMAGE.tlv_header);
    }

    #[test]
    fn carveout_identity_ignores_ram_size_but_checks_each_carveout() {
        let a = canonical(patchbay_fixture(&GIB16), &GIB16).unwrap();
        for gib in [-8, 8] {
            let b = canonical(patchbay_fixture(&moved(gib)), &moved(gib)).unwrap();
            assert!(a == b, "{gib}: one identity for every RAM size");
            // Carveouts that are not this boot's reservations refuse, unchanged.
            let mut c = patchbay_fixture(&GIB16);
            let original = c.clone();
            assert!(!normalize_carveouts(&mut c, &moved(gib), VA));
            assert!(c == original);
        }
        // One reservation off is enough to refuse, as are another VA and SPTP size.
        for i in 0..4 {
            let mut regions = GIB16;
            regions[i].0 += 0x4000;
            assert!(canonical(patchbay_fixture(&GIB16), &regions).is_none(), "{i}");
        }
        let mut regions = GIB16;
        regions[1].1 += 0x4000;
        assert!(canonical(patchbay_fixture(&GIB16), &regions).is_none());
        let mut c = patchbay_fixture(&GIB16);
        assert!(!normalize_carveouts(&mut c, &GIB16, VA + 0x40000));
    }

    #[test]
    fn carveout_identity_zeroes_only_the_four_bases() {
        let a = canonical(patchbay_fixture(&GIB16), &GIB16).unwrap();
        let fixture = patchbay_fixture(&GIB16);
        let changed: Vec<usize> = (0..a.len()).filter(|&i| a[i] != fixture[i]).collect();
        // GKTS and ECAP (normalize_boot_entropy) and BtpG, BPTP, BlpP, S2xG: 6 x 8 bytes.
        assert!(changed.len() <= 48);
        for i in changed {
            let tag = PATCHBAY_TAGS.iter().scan(PATCHBAY.start, |at, (tag, len)| {
                let found = (*at + 8..*at + 8 + len).contains(&i).then_some(*tag);
                *at += 8 + len;
                Some(found)
            }).flatten().next();
            assert!(matches!(tag, Some(b"GKTS" | b"ECAP" | b"BtpG" | b"BPTP" | b"BlpP" | b"S2xG")), "{i:#x}");
        }
        // The rest of the patchbay, the setup records and the code stay covered.
        // (SPTP and BVTP are checked against the reservations above.)
        for offset in [PATCHBAY.start + 0x10, 0x5ce6c + 8, 0x5d010 + 8, 0x5d028 + 8, 0x5d051 + 8, 0x240, 0x5d100, 0x4000] {
            let mut b = patchbay_fixture(&GIB16);
            b[offset] ^= 1;
            assert!(canonical(b, &GIB16).is_some_and(|b| b != a), "{offset:#x}");
        }
    }

    #[test]
    fn carveout_identity_rejects_other_headers_and_broken_lists() {
        for offset in [INFO_HEADER, INFO_HEADER + 4, INFO_HEADER + 8, INFO_HEADER + 0x14,
            INFO_HEADER + 0x2c, INFO_HEADER + 0x30, INFO_HEADER + 0x34] {
            let mut bytes = patchbay_fixture(&GIB16);
            bytes[offset] ^= 1;
            assert!(!normalize_carveouts(&mut bytes, &GIB16, VA), "{offset:x}");
        }
        // A length running past the patchbay, a missing and a repeated carveout tag.
        let mut long = patchbay_fixture(&GIB16);
        long[PATCHBAY.start + 4] = 0xff;
        assert!(!normalize_carveouts(&mut long, &GIB16, VA));
        let mut renamed = patchbay_fixture(&GIB16);
        let at = renamed.windows(4).position(|w| w == b"BlpP").unwrap();
        renamed[at] = b'X';
        assert!(!normalize_carveouts(&mut renamed, &GIB16, VA));
        let mut repeated = patchbay_fixture(&GIB16);
        let at = repeated.windows(4).position(|w| w == b"StpG").unwrap();
        repeated[at..at + 4].copy_from_slice(b"BtpG");
        repeated[at + 8..at + 16].copy_from_slice(&GIB16[0].0.to_le_bytes());
        assert!(!normalize_carveouts(&mut repeated, &GIB16, VA));
        assert!(!normalize_carveouts(&mut [0; 64], &GIB16, VA));
    }

    #[test]
    fn iboot_writes_are_outside_the_identifying_header_and_the_code() {
        for range in SETUP_WRITES.iter() {
            assert!(range.end <= PATCHBAY.start || range.start >= PATCHBAY.end);
        }
        assert!(SETUP_WRITES[0].start >= INFO_HEADER + 4 * 14);
        let mut a = canonical(patchbay_fixture(&GIB16), &GIB16).unwrap();
        let mut b = a.clone();
        b[0x5d100] ^= 1;
        b[0x240] ^= 1;
        b[0x5d010 + 8] ^= 1;
        assert!(normalize_iboot_writes(&mut a) && normalize_iboot_writes(&mut b));
        assert!(a == b);
        b[0x4000] ^= 1;
        assert!(normalize_iboot_writes(&mut b));
        assert!(a != b);
        assert!(!normalize_iboot_writes(&mut [0; 64]));
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
