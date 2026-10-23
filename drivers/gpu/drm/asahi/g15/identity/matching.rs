// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Exact firmware metadata admission, shared with the host regression checks.

pub(crate) fn exact(
    expected_sizes: (u64, u64), actual_sizes: (u64, u64),
    expected_marker: u32, actual_marker: usize,
    expected_digest: &[u8; 32], actual_digest: &[u8; 32],
    machine_matches: bool,
) -> bool {
    machine_matches && expected_sizes == actual_sizes &&
        expected_marker as usize == actual_marker && expected_digest == actual_digest
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requires_board_full_digest_sizes_and_marker() {
        let sizes = (0x5c000, 0x114000);
        let hash = [0x5a; 32];
        assert!(exact(sizes, sizes, 0x58e88, 0x58e88, &hash, &hash, true));
        assert!(!exact(sizes, sizes, 0x58e88, 0x58e88, &hash, &hash, false));
        assert!(!exact(sizes, (sizes.0 + 1, sizes.1), 0x58e88, 0x58e88, &hash, &hash, true));
        assert!(!exact(sizes, (sizes.0, sizes.1 + 1), 0x58e88, 0x58e88, &hash, &hash, true));
        assert!(!exact(sizes, sizes, 0x58e88, 0x58e80, &hash, &hash, true));
        let mut same_prefix = hash;
        same_prefix[31] ^= 1;
        assert!(!exact(sizes, sizes, 0x58e88, 0x58e88, &hash, &same_prefix, true));
    }
}
