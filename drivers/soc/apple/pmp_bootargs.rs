// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounds and record validation for PMP boot arguments.

/// Checks the argument region before allocating a buffer or accessing SRAM.
pub(super) fn region_valid(offset: usize, size: usize, sram_size: usize) -> bool {
    size != 0 && offset <= sram_size && size <= sram_size - offset
}

fn header(bytes: &[u8]) -> Option<(u32, usize)> {
    let key = u32::from_le_bytes(bytes.get(..4)?.try_into().ok()?);
    let size = u32::from_le_bytes(bytes.get(4..8)?.try_into().ok()?) as usize;
    Some((key, size))
}

/// Patches integer records only after validating the entire packed table.
///
/// Unknown records are preserved, and absent keys need no update. Integer
/// values occupy at most eight bytes and are zero-extended as needed.
pub(super) fn patch(bytes: &mut [u8], patches: &[(u32, u32)]) -> bool {
    let mut remaining: &[u8] = bytes;
    while !remaining.is_empty() {
        let Some((key, size)) = header(remaining) else {
            return false;
        };
        let body = &remaining[8..];
        if size > body.len() || (patches.iter().any(|(k, _)| *k == key) && !(1..=8).contains(&size))
        {
            return false;
        }
        remaining = &body[size..];
    }

    let mut remaining = bytes;
    while !remaining.is_empty() {
        // The first pass validated every header and payload boundary.
        let (key, size) = header(remaining).unwrap();
        let (body, rest) = remaining[8..].split_at_mut(size);
        if let Some((_, value)) = patches.iter().find(|(k, _)| *k == key) {
            body.copy_from_slice(&(*value as u64).to_le_bytes()[..size]);
        }
        remaining = rest;
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    const BDID: u32 = 0x42444944;
    const DVID: u32 = 0x44564944;
    const DCAP: u32 = 0x44434150;

    fn record(bytes: &mut Vec<u8>, key: u32, body: &[u8]) {
        bytes.extend_from_slice(&key.to_le_bytes());
        bytes.extend_from_slice(&(body.len() as u32).to_le_bytes());
        bytes.extend_from_slice(body);
    }

    #[test]
    fn region_bounds() {
        assert!(region_valid(0x300, 0x100, 0x80000));
        assert!(region_valid(0x7fff8, 8, 0x80000));
        assert!(!region_valid(0x7fff8, 9, 0x80000));
        assert!(!region_valid(0x80001, 1, 0x80000));
        assert!(!region_valid(0, 0, 0x80000));
        assert!(!region_valid(usize::MAX, 8, 0x80000));
        assert!(!region_valid(1, usize::MAX, usize::MAX));
    }

    #[test]
    fn larger_sram_uses_its_actual_extent() {
        assert!(region_valid(0x90000, 0x1000, 0x100000));
        assert!(region_valid(0xffff8, 8, 0x100000));
        assert!(!region_valid(0xffff8, 9, 0x100000));
        assert!(!region_valid(0x90000, 0x1000, 0x80000));
    }

    #[test]
    fn board_memory_and_unknown_records() {
        let mut bytes = Vec::new();
        record(&mut bytes, BDID, &[0; 4]);
        record(&mut bytes, DVID, &[0xff; 8]);
        record(&mut bytes, 123, &[9, 8, 7]);
        record(&mut bytes, DCAP, &[0; 4]);
        let mut expected = Vec::new();
        record(&mut expected, BDID, &0x30u32.to_le_bytes());
        record(&mut expected, DVID, &6u64.to_le_bytes());
        record(&mut expected, 123, &[9, 8, 7]);
        record(&mut expected, DCAP, &16u32.to_le_bytes());
        assert!(patch(&mut bytes, &[(BDID, 0x30), (DVID, 6), (DCAP, 16)]));
        assert_eq!(bytes, expected);
    }

    #[test]
    fn missing_optional_key_is_unchanged() {
        let mut bytes = Vec::new();
        record(&mut bytes, BDID, &[0; 4]);
        assert!(patch(&mut bytes, &[(BDID, 7), (DCAP, 16)]));
        assert_eq!(&bytes[8..], &7u32.to_le_bytes());
    }

    #[test]
    fn duplicate_keys_keep_existing_patch_semantics() {
        let mut bytes = Vec::new();
        record(&mut bytes, BDID, &[0; 4]);
        record(&mut bytes, BDID, &[0xff; 8]);
        assert!(patch(&mut bytes, &[(BDID, 7)]));
        assert_eq!(&bytes[8..12], &7u32.to_le_bytes());
        assert_eq!(&bytes[20..], &7u64.to_le_bytes());
    }

    #[test]
    fn invalid_tail_never_partially_patches() {
        for size in 1..8 {
            let mut bytes = Vec::new();
            record(&mut bytes, BDID, &[0; 4]);
            bytes.extend_from_slice(&[0xff; 8][..size]);
            let expected = bytes.clone();
            assert!(!patch(&mut bytes, &[(BDID, 7)]));
            assert_eq!(bytes, expected);
        }
    }

    #[test]
    fn invalid_payload_never_partially_patches() {
        for size in [1u32, 8, 9, u32::MAX] {
            let mut bytes = Vec::new();
            record(&mut bytes, BDID, &[0; 4]);
            bytes.extend_from_slice(&DVID.to_le_bytes());
            bytes.extend_from_slice(&size.to_le_bytes());
            let expected = bytes.clone();
            assert!(!patch(&mut bytes, &[(BDID, 7), (DVID, 6)]));
            assert_eq!(bytes, expected);
        }
    }

    #[test]
    fn integer_width_bounds() {
        let value = [0xd4, 0xc3, 0xb2, 0xa1, 0, 0, 0, 0];
        for size in 0..=16 {
            let mut bytes = Vec::new();
            record(&mut bytes, BDID, &vec![0xff; size]);
            let expected = bytes.clone();
            assert_eq!(
                patch(&mut bytes, &[(BDID, 0xa1b2c3d4)]),
                (1..=8).contains(&size)
            );
            if (1..=8).contains(&size) {
                assert_eq!(&bytes[8..], &value[..size]);
            } else {
                assert_eq!(bytes, expected);
            }
        }
    }

    #[test]
    fn unknown_zero_length_record_is_preserved() {
        let mut bytes = Vec::new();
        record(&mut bytes, 123, &[]);
        let expected = bytes.clone();
        assert!(patch(&mut bytes, &[(BDID, 7)]));
        assert_eq!(bytes, expected);
    }
}
