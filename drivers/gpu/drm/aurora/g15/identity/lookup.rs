// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Pure firmware-build identification: the GKTS marker search, the lookup of a measured build in
//! the qualified list, the `g15_accept_text_prefix` escape and the refusal hint.
//!
//! A qualified build pins only through [`super::matching::exact`]: its sizes, marker and digest,
//! and the machine it names.
//!
//! Core-only (no `kernel` imports): this file is compiled by the host test harness as well.

use super::matching;

/// The GKTS marker: the 8 bytes after it are per-boot entropy.
pub(crate) const MARKER: [u8; 8] = [0x47, 0x4b, 0x54, 0x53, 0x08, 0, 0, 0];

/// Appended to the refusal of an unknown build: what a user reports so that it can be pinned.
pub(crate) const REPORT_HINT: &str =
    "to have it supported, report this line and the macOS version of the boot chain that loaded it";

/// The identity of a loaded firmware build: segment sizes, marker offset and the TEXT digest with
/// the marker's entropy zeroed.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct Measured {
    pub(crate) text_size: u64,
    pub(crate) data_size: u64,
    pub(crate) marker: u32,
    pub(crate) sha256: [u8; 32],
}

/// The outcome of identifying a measured build.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Verdict {
    /// The qualified build at this index.
    Pinned(usize),
    /// Not qualified, admitted for this boot because its digest prefix was named.
    Admitted,
    /// Not qualified and not admitted.
    Refused,
}

/// Offsets of the 8-byte-aligned GKTS markers in `text` that have their 8 entropy bytes in
/// range: `Ok(offset)` for exactly one, `Err(count)` otherwise.
pub(crate) fn marker(text: &[u8]) -> Result<usize, usize> {
    let mut at = 0usize;
    let mut found = 0usize;
    let mut off = 0usize;
    while off + 16 <= text.len() {
        if text[off..off + 8] == MARKER {
            at = off;
            found += 1;
        }
        off += 8;
    }
    if found == 1 {
        Ok(at)
    } else {
        Err(found)
    }
}

/// The first 8 digest bytes, big-endian, as `g15_accept_text_prefix` names them.
pub(crate) fn prefix(sha256: &[u8; 32]) -> u64 {
    let mut first = [0u8; 8];
    first.copy_from_slice(&sha256[..8]);
    u64::from_be_bytes(first)
}

/// Looks `m` up in `qualified`, each build with whether this machine is the one it names; an
/// unqualified build is admitted only when `accept_prefix` is nonzero and equals its digest
/// prefix.
pub(crate) fn verdict(
    m: &Measured,
    qualified: impl IntoIterator<Item = (Measured, bool)>,
    accept_prefix: u64,
) -> Verdict {
    if let Some(i) = qualified.into_iter().position(|(q, machine)| {
        matching::exact(
            (q.text_size, q.data_size),
            (m.text_size, m.data_size),
            q.marker,
            m.marker as usize,
            &q.sha256,
            &m.sha256,
            machine,
        )
    }) {
        return Verdict::Pinned(i);
    }
    if accept_prefix != 0 && accept_prefix == prefix(&m.sha256) {
        Verdict::Admitted
    } else {
        Verdict::Refused
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn build(n: u8) -> Measured {
        let mut sha256 = [0u8; 32];
        sha256[0] = 0x1b;
        sha256[7] = n;
        sha256[31] = n;
        Measured {
            text_size: 0x64000,
            data_size: 0x140000,
            marker: 0x5cf28,
            sha256,
        }
    }

    #[test]
    fn finds_one_marker() {
        let mut text = [0u8; 64];
        text[16..24].copy_from_slice(&MARKER);
        assert_eq!(marker(&text), Ok(16));
    }

    #[test]
    fn refuses_none_two_or_misaligned_markers() {
        let mut text = [0u8; 64];
        assert_eq!(marker(&text), Err(0));
        text[12..20].copy_from_slice(&MARKER);
        assert_eq!(marker(&text), Err(0));
        text = [0u8; 64];
        text[8..16].copy_from_slice(&MARKER);
        text[32..40].copy_from_slice(&MARKER);
        assert_eq!(marker(&text), Err(2));
    }

    #[test]
    fn needs_the_entropy_bytes_in_range() {
        let mut text = [0u8; 64];
        text[56..64].copy_from_slice(&MARKER);
        assert_eq!(marker(&text), Err(0));
    }

    #[test]
    fn prefix_is_the_first_eight_bytes_big_endian() {
        let mut sha256 = [0u8; 32];
        sha256[..8].copy_from_slice(&[0x1b, 0xf2, 0xfd, 0x34, 0x76, 0xbe, 0xfe, 0x73]);
        sha256[8] = 0xff;
        assert_eq!(prefix(&sha256), 0x1bf2fd3476befe73);
    }

    #[test]
    fn pins_the_matching_build() {
        assert_eq!(
            verdict(&build(2), [(build(1), true), (build(2), true)], 0),
            Verdict::Pinned(1)
        );
    }

    #[test]
    fn pinned_wins_over_the_prefix() {
        let m = build(1);
        assert_eq!(verdict(&m, [(m, true)], prefix(&m.sha256)), Verdict::Pinned(0));
    }

    #[test]
    fn every_field_must_match() {
        let q = build(1);
        let mut m = q;
        m.text_size += 0x1000;
        assert_eq!(verdict(&m, [(q, true)], 0), Verdict::Refused);
        m = q;
        m.data_size += 0x1000;
        assert_eq!(verdict(&m, [(q, true)], 0), Verdict::Refused);
        m = q;
        m.marker += 8;
        assert_eq!(verdict(&m, [(q, true)], 0), Verdict::Refused);
        m = q;
        m.sha256[31] ^= 1;
        assert_eq!(verdict(&m, [(q, true)], 0), Verdict::Refused);
    }

    #[test]
    fn another_machines_build_is_not_pinned() {
        let m = build(1);
        assert_eq!(verdict(&m, [(m, false)], 0), Verdict::Refused);
        assert_eq!(
            verdict(&m, [(m, false), (m, true)], 0),
            Verdict::Pinned(1)
        );
        // The digest-prefix escape still applies to it, as to any unqualified build.
        assert_eq!(
            verdict(&m, [(m, false)], prefix(&m.sha256)),
            Verdict::Admitted
        );
    }

    #[test]
    fn admits_only_the_named_prefix() {
        let m = build(3);
        assert_eq!(
            verdict(&m, [(build(1), true)], prefix(&m.sha256)),
            Verdict::Admitted
        );
        assert_eq!(
            verdict(&m, [(build(1), true)], prefix(&build(4).sha256)),
            Verdict::Refused
        );
    }

    #[test]
    fn zero_prefix_admits_nothing() {
        let m = Measured {
            sha256: [0u8; 32],
            ..build(0)
        };
        assert_eq!(verdict(&m, [(build(1), true)], 0), Verdict::Refused);
    }

    #[test]
    fn hint_says_what_to_report() {
        assert!(REPORT_HINT.contains("report this line"));
        assert!(REPORT_HINT.contains("macOS version"));
    }
}
