// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded, opt-in power-state reads while a T6030 batch is unstamped.

pub(crate) const MAX_SAMPLES: u32 = 1200;
pub(crate) const SAMPLE_INTERVAL_NS: i64 = 100_000_000;

pub(crate) fn due(enabled: bool, chip: u32, stamped: bool, samples: u32,
                  since_last_ns: Option<i64>) -> bool {
    enabled && chip == 0x6030 && !stamped && samples < MAX_SAMPLES
        && since_last_ns.is_none_or(|ns| ns >= SAMPLE_INTERVAL_NS)
}
