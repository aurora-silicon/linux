// SPDX-License-Identifier: GPL-2.0-only OR MIT


pub(crate) const ROOT_SIZE: usize = 0xc8;
pub(crate) const ROOT_MAGIC: u64 = 0x0c89_c357_8392_04b8;

mod layout {
    pub(crate) const BRN: usize = 8;
    pub(crate) const RUNTIME: usize = 0x18;
    pub(crate) const GLOBALS: usize = 0x20;
    pub(crate) const FLAGS: usize = 0x28;
    pub(crate) const UAT_HEADER: usize = 0x30;
    pub(crate) const UAT_LEVELS: usize = 0x34;
    pub(crate) const UAT_LEVEL_SIZE: usize = 32;
    pub(crate) const CONTROL: usize = 0xa8;
    pub(crate) const FIRMWARE_DATA: usize = 0xb0;
    pub(crate) const POWER: usize = 0xb8;
    pub(crate) const DYNAMIC: usize = 0xc0;
}

#[derive(Clone, Copy)]
pub(crate) struct RootPointers {
    pub(crate) brn: u64,
    pub(crate) runtime: u64,
    pub(crate) globals: u64,
    pub(crate) control: u64,
    pub(crate) firmware_data: u64,
    pub(crate) power: u64,
    pub(crate) dynamic: u64,
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum EncodeError { Size, Address }

pub(crate) fn encode_root(out: &mut [u8], pointers: RootPointers) -> Result<(), EncodeError> {
    let fields = [(layout::BRN, pointers.brn), (layout::RUNTIME, pointers.runtime), (layout::GLOBALS, pointers.globals),
        (layout::CONTROL, pointers.control), (layout::FIRMWARE_DATA, pointers.firmware_data),
        (layout::POWER, pointers.power), (layout::DYNAMIC, pointers.dynamic)];
    if out.len() != ROOT_SIZE { return Err(EncodeError::Size); }
    if fields.iter().any(|(_, address)| address & 7 != 0 || *address < 0xffff_fc00_0000_0000) {
        return Err(EncodeError::Address);
    }
    out.fill(0);
    out[..8].copy_from_slice(&ROOT_MAGIC.to_le_bytes());
    out[layout::FLAGS..layout::FLAGS+8].copy_from_slice(&(1u64 << 32).to_le_bytes());
    out[layout::UAT_HEADER..layout::UAT_HEADER+4].copy_from_slice(&[0, 0x40, 14, 3]);
    for (index, (shift, count)) in [(36u8, 64u16), (25, 2048), (14, 2048)].into_iter().enumerate() {
        let start = layout::UAT_LEVELS + index * layout::UAT_LEVEL_SIZE;
        out[start..start + 4].copy_from_slice(&[8, 14, 14, shift]);
        out[start + 4..start + 6].copy_from_slice(&count.to_le_bytes());
        out[start + 6..start + 8].copy_from_slice(&0x4000u16.to_le_bytes());
        out[start + 8..start + 16].copy_from_slice(&1u64.to_le_bytes());
        out[start + 16..start + 24].copy_from_slice(&(((1u64 << 42) - 1) & !0x3fff).to_le_bytes());
        out[start + 24..start + 32].copy_from_slice(&(((count - 1) as u64) << shift).to_le_bytes());
    }
    for (offset, value) in fields { out[offset..offset + 8].copy_from_slice(&value.to_le_bytes()); }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn pointers() -> RootPointers {
        let base = 0xffff_fc20_0000_0000;
        RootPointers { brn: base, runtime: base + 0x4000, globals: base + 0x8000,
            control: base + 0xc000, firmware_data: base + 0x10000,
            power: base + 0x20000, dynamic: base + 0x24000 }
    }
    #[test]
    fn matches_original_c_root() {
        let reference: [u8; ROOT_SIZE] = [
            0xb8, 0x04, 0x92, 0x83, 0x57, 0xc3, 0x89, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x20, 0xfc, 0xff, 0xff,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x20, 0xfc, 0xff, 0xff,
            0x00, 0x80, 0x00, 0x00, 0x20, 0xfc, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x40, 0x0e, 0x03, 0x08, 0x0e, 0x0e, 0x24, 0x40, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xf0, 0x03, 0x00, 0x00, 0x08, 0x0e, 0x0e, 0x19, 0x00, 0x08, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe,
            0x0f, 0x00, 0x00, 0x00, 0x08, 0x0e, 0x0e, 0x0e, 0x00, 0x08, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0xc0, 0xff, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x20, 0xfc, 0xff, 0xff,
            0x00, 0x00, 0x01, 0x00, 0x20, 0xfc, 0xff, 0xff, 0x00, 0x00, 0x02, 0x00, 0x20, 0xfc, 0xff, 0xff,
            0x00, 0x40, 0x02, 0x00, 0x20, 0xfc, 0xff, 0xff,
        ];
        let mut actual = [0; ROOT_SIZE];
        encode_root(&mut actual, pointers()).unwrap();
        assert_eq!(actual, reference);
    }
    #[test]
    fn rejects_invalid_buffers_and_addresses_before_mutation() {
        let mut short = [0xa5; ROOT_SIZE - 1];
        assert_eq!(encode_root(&mut short, pointers()), Err(EncodeError::Size));
        assert_eq!(short, [0xa5; ROOT_SIZE - 1]);
        for address in [0, 0x10000, 0xffff_fc20_0000_0001] {
            let mut root = [0xa5; ROOT_SIZE];
            let mut p = pointers(); p.runtime = address;
            assert_eq!(encode_root(&mut root, p), Err(EncodeError::Address));
            assert_eq!(root, [0xa5; ROOT_SIZE]);
        }
    }
    #[test]
    fn relocated_pointers_leave_geometry_and_reserved_bytes_unchanged() {
        let mut a = [0; ROOT_SIZE]; let mut b = [0; ROOT_SIZE];
        encode_root(&mut a, pointers()).unwrap();
        let mut p = pointers(); p.runtime += 0x12340000;
        encode_root(&mut b, p).unwrap();
        assert_eq!(&a[..0x18], &b[..0x18]);
        assert_eq!(&a[0x20..], &b[0x20..]);
        assert_ne!(&a[0x18..0x20], &b[0x18..0x20]);
        assert_eq!(&a[0x94..0xa8], &[0; 0x14]);
        assert_eq!(&a[0x30..0x34], &[0, 0x40, 14, 3]);
    }
}
