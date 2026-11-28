// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! A sysfs reader owns the session, never a pointer into detachable Firmware.
//! Reads drain the statistics rings without waking the GPU or servicing work.

use super::{fw::variant::InitGraph, telemetry::Readout, Shared};
use core::sync::atomic::Ordering;
use kernel::sync::Arc;

/// Frequency of each performance state the firmware reports, in MHz; state 0 is off.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StateFrequencies {
    mhz: [u32; Self::MAX],
    states: usize,
}

impl StateFrequencies {
    const MAX: usize = 16;

    /// `mhz` holds states 0 to `mhz.len() - 1`; `None` for more than [`Self::MAX`] states.
    pub(crate) fn new(mhz: impl ExactSizeIterator<Item = u32>) -> Option<Self> {
        let states = mhz.len();
        if states > Self::MAX {
            return None;
        }
        let mut table = [0; Self::MAX];
        for (slot, value) in table.iter_mut().zip(mhz) {
            *slot = value;
        }
        Some(Self { mhz: table, states })
    }

    /// Highest valid performance state index in this table (`0` when empty or off-only).
    pub(crate) fn max_pstate(&self) -> u32 {
        self.states.saturating_sub(1) as u32
    }

    /// The frequency of `pstate` in Hz; `None` beyond the table or where it is unknown (0).
    fn hz(&self, pstate: u32) -> Option<u64> {
        let mhz = *self.mhz[..self.states].get(pstate as usize)?;
        (mhz != 0).then_some(u64::from(mhz) * 1_000_000)
    }

    fn max_hz(&self) -> Option<u64> {
        let mhz = *self.mhz[..self.states].iter().max()?;
        (mhz != 0).then_some(u64::from(mhz) * 1_000_000)
    }
}

/// The device's shared state, and the frequency of each performance state the firmware
/// reports.
pub(crate) struct Reader(pub(super) Arc<Shared>, pub(super) Option<StateFrequencies>);

impl Reader {
    pub(crate) fn read(&self) -> Readout {
        let mut view = self.read_firmware();
        if view.pstate_status == super::telemetry::VALID {
            view.achieved_frequency_hz = self
                .1
                .as_ref()
                .and_then(|states| states.hz(view.pstate))
                .unwrap_or(0);
        }
        view
    }

    /// The frequency of the state the GPU runs at, while the firmware reports one, and the
    /// highest frequency of its states, in Hz; `None` without the states' frequencies.
    pub(crate) fn frequency(&self) -> Option<(Option<u64>, u64)> {
        let max = self.1.as_ref()?.max_hz()?;
        let view = self.read();
        // State 0 is off: the gated GPU runs at no frequency.
        let current = (view.pstate_status == super::telemetry::VALID
            && (view.achieved_frequency_hz != 0 || view.pstate == 0))
            .then_some(view.achieved_frequency_hz);
        Some((current, max))
    }

    fn read_firmware(&self) -> Readout {
        let protocol = self.0.variant.telemetry_protocol;
        let mut state = self.0.state.lock();
        let Some(firmware) = state.as_deref_mut() else {
            return qualify(protocol, Readout::default());
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
        qualify(protocol, firmware.rings.read_statistics(super::now_ns()))
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

/// Whether the firmware reports its performance state. V25G83 and V26A428 accept
/// the logical pstate record (tag 0x10) but never emit it. V26A428 (A18 Pro and
/// M5) and T6041's 25G83 report the state in tags 0x19 and 0x1b instead
/// (`Variant::telemetry_protocol`); T6040's 25G83 is left on `Protocol::NONE`
/// because it has not been measured yet, so it is unsupported there. A firmware
/// that reports it reads valid or stale.
pub(super) const fn reports_pstate(protocol: super::telemetry::Protocol) -> bool {
    protocol.reports_pstate()
}

/// Apply the firmware's static capabilities to a decoded readout. The decoder
/// and the debugfs diagnostic still keep any tag 0x10 that does arrive.
pub(super) fn qualify(
    protocol: super::telemetry::Protocol,
    mut view: super::telemetry::Readout,
) -> super::telemetry::Readout {
    if !reports_pstate(protocol) {
        view.pstate_status = super::telemetry::UNSUPPORTED;
        view.pstate = 0;
        view.pstate_host_ns = 0;
        view.pstate_timestamp = 0;
    }
    view
}
