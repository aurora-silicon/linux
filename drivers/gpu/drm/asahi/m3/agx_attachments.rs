// SPDX-License-Identifier: GPL-2.0-only OR MIT

pub(crate) const SIZE: usize = 0x104;
pub(crate) const CAPACITY: usize = 16;

#[derive(Clone, Copy)]
pub(crate) struct Attachments(pub(crate) [u8; SIZE]);

impl Attachments {
    pub(crate) const EMPTY: Self = Self([0; SIZE]);
    pub(crate) fn count(&self) -> u8 { self.0[0x100] }

    /// Normalize byte ranges to the firmware's 128-byte cache-line units.
    /// The caller must validate the entire normalized writable GPU range.
    pub(crate) fn range(address: u64, size: u64) -> Result<(u64, u64), ()> {
        if address == 0 || size == 0 { return Err(()); }
        let end = address.checked_add(size).and_then(|v| v.checked_add(127)).ok_or(())? & !127;
        let start = address & !127;
        if start == 0 || end > 1 << 42 || (end - start) >> 7 > u32::MAX as u64 {
            return Err(());
        }
        Ok((start, end - start))
    }

    pub(crate) fn new(ranges: &[(u64, u64)]) -> Result<Self, ()> {
        if ranges.len() > CAPACITY { return Err(()); }
        let mut out = Self::EMPTY;
        for (record, &(address, size)) in out.0[..0x100].chunks_exact_mut(16).zip(ranges) {
            let (address, size) = Self::range(address, size)?;
            record[..8].copy_from_slice(&address.to_le_bytes());
            record[8..12].copy_from_slice(&((size >> 7) as u32).to_le_bytes());
            record[12..14].copy_from_slice(&0x18u16.to_le_bytes());
            record[14..16].copy_from_slice(&1u16.to_le_bytes());
        }
        out.0[0x100..].copy_from_slice(&(ranges.len() as u32).to_le_bytes());
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn covers_both_partial_edge_lines() {
        let a = Attachments::new(&[(0x100007f, 2)]).unwrap();
        assert_eq!(u64::from_le_bytes(a.0[..8].try_into().unwrap()), 0x1000000);
        assert_eq!(u32::from_le_bytes(a.0[8..12].try_into().unwrap()), 2);
        assert_eq!(&a.0[12..16], &[0x18, 0, 1, 0]);
        assert_eq!(a.count(), 1);
        assert!(a.0[16..0x100].iter().all(|v| *v == 0));
    }
    #[test]
    fn rejects_unrepresentable_ranges_and_count() {
        for range in [(0, 128), (128, 0), (u64::MAX - 1, 8),
                      ((1 << 42) - 128, 129), (128, (u32::MAX as u64 + 1) << 7)] {
            assert!(Attachments::new(&[range]).is_err());
        }
        assert!(Attachments::new(&[(128, 128); CAPACITY + 1]).is_err());
        let full = Attachments::new(&[(128, 128); CAPACITY]).unwrap();
        assert_eq!(full.count(), 16);
        assert_eq!(&full.0[0x100..], &[16, 0, 0, 0]);
        assert_eq!(Attachments::new(&[]).unwrap().0, Attachments::EMPTY.0);
    }
    #[test]
    fn matches_native_m4_writable_buffer_descriptor() {
        let encoded = Attachments::new(&[(0x100000a8000, 0x400000)]).unwrap();
        assert_eq!(&encoded.0[..16], &[
            0, 0x80, 0x0a, 0, 0, 1, 0, 0, 0, 0x80, 0, 0, 0x18, 0, 1, 0]);
        assert_eq!(encoded.count(), 1);
    }
}
