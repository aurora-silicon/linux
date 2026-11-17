// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Firmware stamp progression and idle epoch boundaries (#1935).
//! Run: rustc --edition=2021 --test tools/asahi/m3-stamp-tests.rs -o /tmp/m3-stamp-tests
#![allow(dead_code)]
#[path = "../../drivers/gpu/drm/asahi/m3/m3_timeline.rs"]
mod timeline;

const TA: u32 = 0x7a00_0000;
const FS: u32 = 0x3d00_0000;
const COMPUTE: u32 = 0xc100_0000;

#[test]
fn reported_fragment_rollovers_must_not_skip_zero() {
    // Independent values from both reports: the old period (0xffffff)
    // produces the same 0xffffff00 -> 0x100 jump at each of these draws.
    let old = |n: u64| (((((FS >> 8) as u64 - 1 + n % 0xffffff) % 0xffffff) + 1) as u32) << 8;
    for n in [12_779_520, 29_556_735] {
        assert_eq!(old(n - 2), 0xffff_fe00);
        assert_eq!(old(n - 1), 0xffff_ff00);
        assert_eq!(old(n), 0x100);
        assert_eq!(old(n).wrapping_sub(old(n - 1)), 0x200);
    }
    // Wire arithmetic itself is modulo 2^32. The runtime avoids zero by
    // reseeding at idle, rather than removing a step from that sequence.
    assert_eq!(timeline::stamp(FS, 12_779_519), 0xffff_ff00);
    assert_eq!(timeline::stamp(FS, 12_779_520), 0);
    assert_eq!(timeline::stamp(FS, 12_779_521), 0x100);
    for seed in [TA, FS, COMPUTE] {
        let wrap = (1u64 << 24) - (seed >> 8) as u64;
        for n in wrap - 16..=wrap + 16 {
            assert_eq!(timeline::stamp(seed, n).wrapping_sub(timeline::stamp(seed, n - 1)), 0x100);
        }
    }
}

#[test]
fn reseed_boundary_leaves_room_for_maximum_batch_and_draining_pipeline() {
    for seed in [TA, COMPUTE] {
        let mut epoch = timeline::StampEpoch::default();
        // Render reseeds before its earlier TA wrap, so FS cannot reach its wrap.
        let boundary = if seed == TA { 0x76_0000 } else { 0x2f_0000 };
        assert!(!epoch.needs_reseed(seed, boundary - 1));
        assert!(epoch.needs_reseed(seed, boundary));
        // A publication can cross admission's boundary by at most one batch.
        // Existing batches then drain without any further publication.
        for passes in 1..=16 {
            let retired = boundary - 1 + passes;
            assert!(epoch.needs_reseed(seed, retired));
            assert_ne!(epoch.value(seed, retired), 0);
            if seed == TA { assert_ne!(epoch.value(FS, retired), 0); }
        }
        let retired = boundary + 15;
        let counters = (timeline::previous(retired + 1), timeline::events(retired + 1, 2));
        epoch.reseed(retired);
        assert!(!epoch.needs_reseed(seed, retired));
        assert_eq!(epoch.value(seed, retired), seed);
        assert_eq!(epoch.value(seed, retired + 1), seed + 0x100);
        if seed == TA {
            // The next TA/FS barriers wait on exactly the newly written seeds.
            assert_eq!(epoch.value(FS, retired), FS);
            assert_eq!(epoch.value(FS, retired + 1), FS + 0x100);
        }
        assert_eq!((timeline::previous(retired + 1), timeline::events(retired + 1, 2)), counters);
    }
}

#[test]
fn repeated_epochs_keep_host_ordinals_monotonic_past_u32() {
    let mut epoch = timeline::StampEpoch::default();
    let mut retired = 0;
    for _ in 0..1024 {
        retired += 0x76_000f;
        assert!(epoch.needs_reseed(TA, retired));
        epoch.reseed(retired);
        for n in 1..=16 {
            assert_eq!(epoch.value(TA, retired + n), TA + n as u32 * 0x100);
            assert_eq!(epoch.value(FS, retired + n), FS + n as u32 * 0x100);
        }
    }
    assert!(retired > u32::MAX as u64);
    assert_eq!(timeline::previous(retired + 1), retired as u32);
    assert_eq!(timeline::events(retired + 1, 2), ((retired + 1) as u32).wrapping_mul(2));
}
