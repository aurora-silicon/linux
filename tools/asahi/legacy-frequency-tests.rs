// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Exercise the production snapshot lookup without kernel allocation adapters.
#![allow(dead_code)]
extern crate self as kernel;
mod prelude {
    pub type KVec<T> = Vec<T>;
}
mod g17 {
    pub mod stats {
        pub struct Reader;
    }
}
#[path = "../../drivers/gpu/drm/asahi/stats.rs"]
mod stats;

#[test]
fn firmware_order_zero_and_invalid_indices() {
    let snapshot = stats::StatsSnapshot {
        frequencies_hz: vec![0, 338_000_000, 618_000_000, 492_000_000],
        ..Default::default()
    };
    assert_eq!(snapshot.requested_frequency(0), Some(0));
    assert_eq!(snapshot.requested_frequency(2), Some(618_000_000));
    assert_eq!(snapshot.requested_frequency(3), Some(492_000_000));
    assert_eq!(snapshot.requested_frequency(4), None);
    assert_eq!(snapshot.requested_frequency(u32::MAX), None);
    assert_eq!(stats::StatsSnapshot::default().requested_frequency(0), None);
}
