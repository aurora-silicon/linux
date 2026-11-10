// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! A sysfs reader owns the session, never a pointer into detachable Firmware.
//! Reads drain the statistics rings without waking the GPU or servicing work.

use super::{fw::variant::InitGraph, telemetry::Readout, Shared};
use core::sync::atomic::Ordering;
use kernel::sync::Arc;

pub(crate) struct Reader(pub(super) Arc<Shared>);

impl Reader {
    pub(crate) fn read(&self) -> Readout {
        let mut state = self.0.state.lock();
        let Some(firmware) = state.as_deref_mut() else {
            return qualify(self.0.variant.graph, Readout::default());
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

/// Whether the firmware emits the logical pstate record (tag 0x10). V25G83
/// and V26A428 accept the tag but never emit it, and no other record carries
/// the current pstate, so it is unsupported by these firmwares. A firmware
/// that is shown to emit it gets `true` here and reports valid or stale.
pub(super) const fn reports_pstate(graph: InitGraph) -> bool {
    match graph {
        InitGraph::V25G83 | InitGraph::G17P => false,
    }
}

/// Apply the firmware's static capabilities to a decoded readout. The decoder
/// and the debugfs diagnostic still keep any tag 0x10 that does arrive.
pub(super) fn qualify(
    graph: InitGraph,
    mut view: super::telemetry::Readout,
) -> super::telemetry::Readout {
    if !reports_pstate(graph) {
        view.pstate_status = super::telemetry::UNSUPPORTED;
        view.pstate = 0;
        view.pstate_host_ns = 0;
        view.pstate_timestamp = 0;
    }
    view
}
