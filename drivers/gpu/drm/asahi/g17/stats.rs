// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! A sysfs reader owns the session, never a pointer into detachable Firmware.

use super::{fw::variant::InitGraph, telemetry::Readout, Shared};
use core::sync::atomic::Ordering;
use kernel::sync::Arc;

pub(crate) struct Reader(pub(super) Arc<Shared>);

impl Reader {
    pub(crate) fn read(&self) -> Readout {
        let mut state = self.0.state.lock();
        let Some(firmware) = state.as_deref_mut() else {
            return Readout::default();
        };
        if self.0.crashed.load(Ordering::Acquire) || firmware.recovery.pending() {
            firmware.rings.invalidate_statistics();
            return firmware.rings.read_statistics(super::now_ns());
        }
        firmware.observe_statistics_power();
        firmware.rings.read_statistics(super::now_ns())
    }

    pub(crate) fn suspend(&self, suspended: bool) {
        if let Some(firmware) = self.0.state.lock().as_deref_mut() {
            firmware.rings.suspend_statistics(suspended);
        }
    }

    /// PM_HIBERNATION_PREPARE: drain the GPU, or refuse the hibernation.
    pub(crate) fn freeze(&self) -> kernel::error::Result {
        self.0.freeze_for_hibernation()
    }

    /// PM_POST_HIBERNATION: re-boot a cold firmware against the restored host session, or
    /// resume a firmware that kept running.
    pub(crate) fn thaw(&self) {
        self.0.thaw_after_hibernation();
    }
}

/// Two nominal periods is a conservative host age/window policy. The firmware
/// does not guarantee a packet duration or a pstate report cadence. Logical
/// pstate validity is controlled by records and epoch boundaries, not this age.
pub(super) const fn max_age_ns(graph: InitGraph) -> u64 {
    match graph {
        InitGraph::V25G83 => 16_000_000,
        InitGraph::G17P => 32_000_000,
    }
}
