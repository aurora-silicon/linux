// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 kick, frame-pass and resource logging policy (`g18_kick_log`).
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.
use core::sync::atomic::{AtomicBool, Ordering};
use crate::g18::params;

/// Set once the G18 application DRM node is live (`G18_DRM_READY`).
static G18_DRM_LIVE: AtomicBool = AtomicBool::new(false);

/// Mark the G18 application DRM node live. Output before this is unaffected.
pub(crate) fn g18_mark_drm_live() {
    G18_DRM_LIVE.store(true, Ordering::Relaxed);
}

/// Whether a per-kick, frame-pass or per-job resource line is printed: always
/// before `G18_DRM_READY`, afterwards only with `g18_kick_log=1`. Fault,
/// recovery, refusal, wait, stall and admission lines never consult this.
#[inline(always)]
pub(crate) fn g18_kick_log() -> bool {
    params::g18_kick_log.value() != 0 || !G18_DRM_LIVE.load(Ordering::Relaxed)
}

/// dev_info!() for a G18 per-kick, frame-pass or per-job resource line
#[macro_export]
macro_rules! g18_kick_info (
    ($dev:expr, $($arg:tt)*) => (
        if $crate::g18::debug::g18_kick_log() {
            ::kernel::dev_info! ( $dev, $($arg)* );
        }
    )
);

/// pr_info!() for a G18 per-kick, frame-pass or per-job resource line
#[macro_export]
macro_rules! g18_kick_pr_info (
    ($($arg:tt)*) => (
        if $crate::g18::debug::g18_kick_log() {
            ::kernel::pr_info! ( $($arg)* );
        }
    )
);

/// dev_info!() for a firmware event record: only a `routine` record follows
/// `g18_kick_log`; every other record is always printed
#[macro_export]
macro_rules! g18_event_info (
    ($routine:expr, $dev:expr, $($arg:tt)*) => (
        if !($routine) || $crate::g18::debug::g18_kick_log() {
            ::kernel::dev_info! ( $dev, $($arg)* );
        }
    )
);
