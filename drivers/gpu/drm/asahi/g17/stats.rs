// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! A sysfs reader owns the session, never a pointer into detachable Firmware.
//! Reads drain the statistics rings without waking the GPU or servicing work.

use super::{fw::variant::InitGraph, telemetry::Readout, Shared};
use core::sync::atomic::Ordering;
use kernel::sync::Arc;

/// The device's shared state, and the M5 board's performance states, whose frequency a
/// reported state index runs at.
pub(crate) struct Reader(
    pub(super) Arc<Shared>,
    pub(super) Option<&'static [super::fw::m5::initdata::M5PerfState; 14]>,
);

impl Reader {
    pub(crate) fn read(&self) -> Readout {
        let mut view = self.read_firmware();
        if view.pstate_status == super::telemetry::VALID {
            view.achieved_frequency_hz = self
                .1
                .and_then(|states| states.get(view.pstate as usize))
                .map_or(0, |state| u64::from(state.0) * 1_000_000);
        }
        view
    }

    fn read_firmware(&self) -> Readout {
        let m5 = self.0.variant.m5_commands;
        let mut state = self.0.state.lock();
        let Some(firmware) = state.as_deref_mut() else {
            return qualify(self.0.variant.graph, m5, Readout::default());
        };
        if self.0.crashed.load(Ordering::Acquire) || firmware.recovery.pending() {
            firmware.rings.invalidate_statistics();
        } else {
            // A statistics producer update need not coincide with an event
            // notification. Consume complete queued records on demand too,
            // using the same epoch, suspension and quarantine admission as
            // the event worker. The device mutex serializes both consumers.
            firmware.observe_statistics_power();
            for &role in firmware.roles() {
                firmware
                    .rings
                    .drain_statistics(&self.0.dev, &firmware.init, role);
            }
            // Power may have changed while copying the records. Do not
            // export samples across an observed boundary.
            firmware.observe_statistics_power();
        }
        qualify(
            self.0.variant.graph,
            m5,
            firmware.rings.read_statistics(super::now_ns()),
        )
    }

    pub(crate) fn suspend(&self, suspended: bool) {
        if let Some(firmware) = self.0.state.lock().as_deref_mut() {
            firmware.rings.suspend_statistics(suspended);
        }
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

/// Whether the firmware reports its performance state. V25G83 and V26A428
/// accept the logical pstate record (tag 0x10) but never emit it, and no other
/// record of theirs carries the current pstate, so it is unsupported by these
/// firmwares. M5 firmware (`m5`) reports it in its own records
/// ([`super::telemetry`]). A firmware that is shown to report it gets `true`
/// here and reports valid or stale.
pub(super) const fn reports_pstate(graph: InitGraph, m5: bool) -> bool {
    match graph {
        InitGraph::V25G83 | InitGraph::G17P => m5,
    }
}

/// Apply the firmware's static capabilities to a decoded readout. The decoder
/// and the debugfs diagnostic still keep any tag 0x10 that does arrive.
pub(super) fn qualify(
    graph: InitGraph,
    m5: bool,
    mut view: super::telemetry::Readout,
) -> super::telemetry::Readout {
    if !reports_pstate(graph, m5) {
        view.pstate_status = super::telemetry::UNSUPPORTED;
        view.pstate = 0;
        view.pstate_host_ns = 0;
        view.pstate_timestamp = 0;
    }
    view
}
