// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Validation and scalar overrides for the AOP's boot-argument records.

use core::ops::Range;

#[derive(Debug, PartialEq, Eq)]
pub(super) enum Error {
    TruncatedHeader,
    TruncatedValue,
    DuplicateOverride(u32),
    MissingField(u32),
    DuplicateField(u32),
    InvalidScalarSize(u32),
    ValueTooLarge(u32),
}

fn entry(bytes: &[u8], offset: usize) -> Result<(u32, Range<usize>), Error> {
    let data_start = offset.checked_add(8).ok_or(Error::TruncatedHeader)?;
    let header = bytes
        .get(offset..data_start)
        .ok_or(Error::TruncatedHeader)?;
    let key = u32::from_le_bytes(header[..4].try_into().unwrap());
    let size = u32::from_le_bytes(header[4..].try_into().unwrap()) as usize;
    let data_end = data_start.checked_add(size).ok_or(Error::TruncatedValue)?;
    bytes
        .get(data_start..data_end)
        .ok_or(Error::TruncatedValue)?;
    Ok((key, data_start..data_end))
}

/// Validate the complete record layout and every required override first.
///
/// Unknown fields retain their original bytes. Each overridden field must
/// appear exactly once and hold a nonempty scalar of at most eight bytes.
/// The value must fit its recorded width, rather than silently truncating.
/// An error leaves the buffer unchanged.
pub(super) fn patch(bytes: &mut [u8], overrides: &[(u32, u64)]) -> Result<(), Error> {
    if overrides.is_empty() {
        return Ok(());
    }

    for (index, &(key, value)) in overrides.iter().enumerate() {
        if overrides[..index].iter().any(|&(other, _)| other == key) {
            return Err(Error::DuplicateOverride(key));
        }

        let mut offset = 0;
        let mut found = false;
        while offset < bytes.len() {
            let (field, data) = entry(bytes, offset)?;
            offset = data.end;
            if field != key {
                continue;
            }
            if found {
                return Err(Error::DuplicateField(key));
            }
            if !(1..=8).contains(&data.len()) {
                return Err(Error::InvalidScalarSize(key));
            }
            if value.to_le_bytes()[data.len()..]
                .iter()
                .any(|&byte| byte != 0)
            {
                return Err(Error::ValueTooLarge(key));
            }
            found = true;
        }
        if !found {
            return Err(Error::MissingField(key));
        }
    }

    let mut offset = 0;
    while offset < bytes.len() {
        // The immutable validation passes checked every record; writes below
        // modify only payload bytes, leaving the keys and lengths unchanged.
        let (key, data) = entry(bytes, offset).unwrap();
        offset = data.end;
        if let Some(&(_, value)) = overrides.iter().find(|&&(field, _)| field == key) {
            bytes[data.clone()].copy_from_slice(&value.to_le_bytes()[..data.len()]);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: u32 = 0x45433070;
    const OTHER: u32 = 0x616c6967;

    fn record(key: u32, data: &[u8]) -> Vec<u8> {
        let mut bytes = key.to_le_bytes().to_vec();
        bytes.extend_from_slice(&(data.len() as u32).to_le_bytes());
        bytes.extend_from_slice(data);
        bytes
    }

    fn rejects_without_mutation(mut bytes: Vec<u8>, overrides: &[(u32, u64)], error: Error) {
        let original = bytes.clone();
        assert_eq!(patch(&mut bytes, overrides), Err(error));
        assert_eq!(bytes, original);
    }

    #[test]
    fn one_override_preserves_other_payloads_and_record_headers() {
        let mut bytes = record(KEY, &[0xaa; 8]);
        let other = record(OTHER, &[0xbb; 12]);
        bytes.extend_from_slice(&other);
        let expected_header = bytes[..8].to_vec();
        patch(&mut bytes, &[(KEY, 0x0100_00000000)]).unwrap();
        assert_eq!(&bytes[..8], expected_header);
        assert_eq!(&bytes[8..16], &0x0100_00000000u64.to_le_bytes());
        assert_eq!(&bytes[16..], other);
    }

    #[test]
    fn late_truncated_record_does_not_patch_an_earlier_field() {
        let mut bytes = record(KEY, &[0xaa; 8]);
        bytes.extend_from_slice(&[1, 2, 3]);
        rejects_without_mutation(bytes, &[(KEY, 1)], Error::TruncatedHeader);
    }

    #[test]
    fn oversized_record_length_is_rejected() {
        let mut bytes = KEY.to_le_bytes().to_vec();
        bytes.extend_from_slice(&u32::MAX.to_le_bytes());
        rejects_without_mutation(bytes, &[(KEY, 1)], Error::TruncatedValue);
    }

    #[test]
    fn missing_later_override_does_not_patch_an_earlier_field() {
        rejects_without_mutation(
            record(KEY, &[0xaa; 8]),
            &[(KEY, 1), (OTHER, 2)],
            Error::MissingField(OTHER),
        );
    }

    #[test]
    fn duplicate_fields_and_overrides_are_rejected() {
        let mut bytes = record(KEY, &[0xaa; 8]);
        bytes.extend_from_slice(&record(KEY, &[0xbb; 8]));
        rejects_without_mutation(bytes, &[(KEY, 1)], Error::DuplicateField(KEY));
        rejects_without_mutation(
            record(KEY, &[0xaa; 8]),
            &[(KEY, 1), (KEY, 2)],
            Error::DuplicateOverride(KEY),
        );
    }

    #[test]
    fn overridden_scalar_width_is_checked() {
        for size in [0, 9] {
            rejects_without_mutation(
                record(KEY, &vec![0xaa; size]),
                &[(KEY, 1)],
                Error::InvalidScalarSize(KEY),
            );
        }
    }

    #[test]
    fn value_cannot_be_truncated_to_a_narrow_field() {
        rejects_without_mutation(
            record(KEY, &[0xaa; 4]),
            &[(KEY, 0x0100_00000000)],
            Error::ValueTooLarge(KEY),
        );
    }

    #[test]
    fn scalar_widths_encode_little_endian() {
        for size in 1..=8 {
            let value = if size == 8 {
                u64::MAX
            } else {
                (1u64 << (size * 8)) - 1
            };
            let mut bytes = record(KEY, &vec![0xaa; size]);
            patch(&mut bytes, &[(KEY, value)]).unwrap();
            assert_eq!(&bytes[8..], &value.to_le_bytes()[..size]);
        }
    }

    #[test]
    fn unknown_empty_or_large_fields_are_unchanged() {
        let mut bytes = record(OTHER, &[]);
        bytes.extend_from_slice(&record(OTHER, &[0xbb; 16]));
        let prefix = bytes.clone();
        bytes.extend_from_slice(&record(KEY, &[0xaa; 8]));
        patch(&mut bytes, &[(KEY, 1)]).unwrap();
        assert_eq!(&bytes[..prefix.len()], prefix);
    }

    #[test]
    fn empty_profile_does_not_touch_or_parse_bootargs() {
        let mut bytes = vec![0xaa; 3];
        patch(&mut bytes, &[]).unwrap();
        assert_eq!(bytes, vec![0xaa; 3]);
    }
}
