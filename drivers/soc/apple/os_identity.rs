// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Stable selection of the existing xART OS identity.
//!
//! The identity selects an anti-replay lane. Changing this policy on an
//! installed system can strand enrolments, even if host keybag files survive.
//! Never generate a replacement identity here. See tools/aurora-sep/OS-IDENTITY.md.

pub(crate) enum Selection {
    Explicit([u8; 16]),
    Chosen([u8; 16]),
    Unavailable,
}

/// Preserve the historical params -> chosen -> none ordering. A zero half is
/// valid; only two zero parameters mean "no override". Read chosen lazily so
/// an explicit identity never depends on the boot property's availability.
pub(crate) fn select(hi: u64, lo: u64, chosen: impl FnOnce() -> Option<[u8; 16]>) -> Selection {
    if hi != 0 || lo != 0 {
        let mut uuid = [0; 16];
        uuid[..8].copy_from_slice(&hi.to_be_bytes());
        uuid[8..].copy_from_slice(&lo.to_be_bytes());
        return Selection::Explicit(uuid);
    }
    match chosen() {
        Some(uuid) => Selection::Chosen(uuid),
        None => Selection::Unavailable,
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_os_identity)]
mod tests {
    use super::*;

    #[test]
    fn explicit_identity_wins_without_reading_chosen() {
        let selected = select(1, 2, || panic!("explicit identity read chosen"));
        let Selection::Explicit(uuid) = selected else {
            panic!("explicit identity lost its lane");
        };
        let mut expected = [0; 16];
        expected[7] = 1;
        expected[15] = 2;
        assert!(uuid == expected);
    }

    #[test]
    fn either_zero_half_keeps_the_explicit_identity() {
        for (hi, lo) in [(0, 1), (1, 0)] {
            let selected = select(hi, lo, || panic!("zero half read chosen"));
            let Selection::Explicit(uuid) = selected else {
                panic!("zero half lost explicit identity");
            };
            assert!(uuid[..8] == hi.to_be_bytes());
            assert!(uuid[8..] == lo.to_be_bytes());
        }
    }

    #[test]
    fn explicit_halves_keep_big_endian_wire_order() {
        let hi = u64::from_be_bytes([1, 2, 3, 4, 5, 6, 7, 8]);
        let lo = u64::from_be_bytes([9, 10, 11, 12, 13, 14, 15, 16]);
        let Selection::Explicit(uuid) = select(hi, lo, || None) else {
            panic!("explicit identity unavailable");
        };
        assert!(uuid == [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
    }

    #[test]
    fn no_override_preserves_chosen_bytes_on_each_boot() {
        for chosen in [[1; 16], [2; 16]] {
            for _ in 0..2 {
                let Selection::Chosen(uuid) = select(0, 0, || Some(chosen)) else {
                    panic!("chosen identity replaced");
                };
                assert!(uuid == chosen);
            }
        }
    }

    #[test]
    fn missing_chosen_never_invents_an_identity() {
        assert!(matches!(select(0, 0, || None), Selection::Unavailable));
    }
}
