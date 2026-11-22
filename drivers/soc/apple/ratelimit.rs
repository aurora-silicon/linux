// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! Rate-limited kernel log messages for request paths.
//!
//! Anyone who can open a SEP character device can make the driver refuse a
//! request as often as it likes, so a message printed per request must not
//! reach the log unbounded. Each call site of these macros has its own state
//! and reports how many messages it dropped when it prints again.
//!
//! - `dev_*_ratelimited!` / `pr_*_ratelimited!` behave like the C
//!   `dev_warn_ratelimited()` family: [`BURST`] messages per [`INTERVAL_NS`].
//!   Use them for driver, firmware and transport failures.
//! - `dev_info_refusal!` prints one message per [`REFUSAL_INTERVAL_NS`] at
//!   info level. Use it for an expected refusal of something the caller
//!   supplied (a record, a sealed blob, a password, a locked bag).

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering::Relaxed};

/// Window length of `*_ratelimited!`, as `DEFAULT_RATELIMIT_INTERVAL` (5 s).
pub(crate) const INTERVAL_NS: u64 = 5_000_000_000;
/// Messages per window of `*_ratelimited!`, as `DEFAULT_RATELIMIT_BURST`.
pub(crate) const BURST: u32 = 10;
/// Window length of `dev_info_refusal!`: one message per reason per minute.
pub(crate) const REFUSAL_INTERVAL_NS: u64 = 60_000_000_000;

/// Per-call-site state. Lock-free; under concurrent callers the counts are
/// approximate, which is all a log limit needs.
pub(crate) struct RateLimit {
    interval_ns: u64,
    burst: u32,
    /// Start of the current window plus one (0: no message yet).
    begin: AtomicU64,
    printed: AtomicU32,
    missed: AtomicU32,
}

/// What a call site does with one message.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub(crate) enum Verdict {
    /// Print it; the number is how many were dropped since the last print.
    Print(u32),
    /// Drop it.
    Drop,
}

impl RateLimit {
    pub(crate) const fn new(interval_ns: u64, burst: u32) -> Self {
        Self {
            interval_ns,
            burst,
            begin: AtomicU64::new(0),
            printed: AtomicU32::new(0),
            missed: AtomicU32::new(0),
        }
    }

    /// Decide for a message at `now_ns` (any monotonic clock).
    pub(crate) fn check_at(&self, now_ns: u64) -> Verdict {
        let stamp = now_ns.saturating_add(1);
        let begin = self.begin.load(Relaxed);
        let mut dropped = 0;
        if (begin == 0 || stamp.saturating_sub(begin) >= self.interval_ns)
            && self
                .begin
                .compare_exchange(begin, stamp, Relaxed, Relaxed)
                .is_ok()
        {
            self.printed.store(0, Relaxed);
            dropped = self.missed.swap(0, Relaxed);
        }
        if self.printed.fetch_add(1, Relaxed) < self.burst {
            Verdict::Print(dropped)
        } else {
            self.printed.store(self.burst, Relaxed);
            self.missed.fetch_add(1, Relaxed);
            Verdict::Drop
        }
    }

    pub(crate) fn check(&self) -> Verdict {
        self.check_at(crate::shim::boottime_ns())
    }
}

/// Shared body of the macros below: `$print` is a `kernel` print macro.
macro_rules! sep_ratelimited {
    ($print:ident, ($($pre:expr),*), $interval:expr, $burst:expr, $($arg:tt)+) => {{
        static RATELIMIT: $crate::ratelimit::RateLimit =
            $crate::ratelimit::RateLimit::new($interval, $burst);
        if let $crate::ratelimit::Verdict::Print(dropped) = RATELIMIT.check() {
            if dropped != 0 {
                ::kernel::$print!($($pre,)* "{} similar messages suppressed\n", dropped);
            }
            ::kernel::$print!($($pre,)* $($arg)+);
        }
    }};
}

macro_rules! sep_default_ratelimited {
    ($print:ident, ($($pre:expr),*), $($arg:tt)+) => {
        sep_ratelimited!(
            $print,
            ($($pre),*),
            $crate::ratelimit::INTERVAL_NS,
            $crate::ratelimit::BURST,
            $($arg)+
        )
    };
}

macro_rules! dev_err_ratelimited {
    ($dev:expr, $($arg:tt)+) => { sep_default_ratelimited!(dev_err, ($dev), $($arg)+) };
}
macro_rules! dev_warn_ratelimited {
    ($dev:expr, $($arg:tt)+) => { sep_default_ratelimited!(dev_warn, ($dev), $($arg)+) };
}
macro_rules! dev_notice_ratelimited {
    ($dev:expr, $($arg:tt)+) => { sep_default_ratelimited!(dev_notice, ($dev), $($arg)+) };
}
macro_rules! dev_info_ratelimited {
    ($dev:expr, $($arg:tt)+) => { sep_default_ratelimited!(dev_info, ($dev), $($arg)+) };
}
macro_rules! pr_err_ratelimited {
    ($($arg:tt)+) => { sep_default_ratelimited!(pr_err, (), $($arg)+) };
}
macro_rules! pr_warn_ratelimited {
    ($($arg:tt)+) => { sep_default_ratelimited!(pr_warn, (), $($arg)+) };
}
macro_rules! pr_notice_ratelimited {
    ($($arg:tt)+) => { sep_default_ratelimited!(pr_notice, (), $($arg)+) };
}
macro_rules! pr_info_ratelimited {
    ($($arg:tt)+) => { sep_default_ratelimited!(pr_info, (), $($arg)+) };
}
macro_rules! dev_info_refusal {
    ($dev:expr, $($arg:tt)+) => {
        sep_ratelimited!(dev_info, ($dev), $crate::ratelimit::REFUSAL_INTERVAL_NS, 1, $($arg)+)
    };
}

#[kernel::macros::kunit_tests(apple_sep_ratelimit)]
mod tests {
    use super::*;

    const S: u64 = 1_000_000_000;

    fn default() -> RateLimit {
        RateLimit::new(INTERVAL_NS, BURST)
    }

    #[test]
    fn burst_then_drop() {
        let rl = default();
        for _ in 0..BURST {
            assert_eq!(rl.check_at(10 * S), Verdict::Print(0));
        }
        for _ in 0..1000 {
            assert_eq!(rl.check_at(10 * S + 1), Verdict::Drop);
        }
    }

    #[test]
    fn next_window_reports_the_drops() {
        let rl = default();
        for _ in 0..BURST + 7 {
            rl.check_at(0);
        }
        assert_eq!(rl.check_at(INTERVAL_NS - 1), Verdict::Drop);
        assert_eq!(rl.check_at(INTERVAL_NS), Verdict::Print(8));
        for _ in 1..BURST {
            assert_eq!(rl.check_at(INTERVAL_NS + 1), Verdict::Print(0));
        }
        assert_eq!(rl.check_at(INTERVAL_NS + 2), Verdict::Drop);
    }

    #[test]
    fn quiet_site_always_prints() {
        let rl = default();
        for i in 0..100 {
            assert_eq!(rl.check_at(i * INTERVAL_NS), Verdict::Print(0));
        }
    }

    #[test]
    fn a_flood_is_bounded() {
        // 100000 refusals over 60 s print at most BURST per window.
        let rl = default();
        let mut printed = 0u32;
        for i in 0..100_000u64 {
            if let Verdict::Print(_) = rl.check_at(i * 600_000) {
                printed += 1;
            }
        }
        assert!(printed <= BURST * (60 / 5 + 1));
        assert!(printed >= BURST);
    }

    #[test]
    fn refusals_print_once_a_minute() {
        let rl = RateLimit::new(REFUSAL_INTERVAL_NS, 1);
        assert_eq!(rl.check_at(5 * S), Verdict::Print(0));
        for i in 1..700u64 {
            assert_eq!(rl.check_at(5 * S + i * 80_000_000), Verdict::Drop);
        }
        assert_eq!(rl.check_at(65 * S), Verdict::Print(699));
    }
}
