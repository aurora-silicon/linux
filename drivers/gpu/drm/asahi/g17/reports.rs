// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Preserve the firmware's statistics without assigning unqualified units or
//! treating another firmware's pstate report as an achieved GPU frequency.
//! Writers and readers hold the device mutex; reads never power the GPU up.

use super::{fw::channels::EVENT_WORDS, Role};
use kernel::prelude::*;

#[derive(Clone, Copy, Default)]
struct Sample {
    count: u64,
    admitted: u64,
    last_admitted: bool,
    host_ns: u64,
    words: [u32; EVENT_WORDS],
}

pub(super) struct Reports {
    samples: KVec<Sample>,
}

impl Reports {
    pub(super) fn new() -> Result<Self> {
        // Allocate before firmware starts; receive and debugfs paths allocate nothing.
        // Each role has one slot per small tag and one for the last larger tag.
        // Unknown records must remain inspectable when they stop the ring.
        let mut samples = KVec::with_capacity(66, GFP_KERNEL)?;
        for _ in 0..66 {
            samples.push(Sample::default(), GFP_KERNEL)?;
        }
        Ok(Self { samples })
    }

    pub(super) fn record(
        &mut self,
        role: Role,
        words: [u32; EVENT_WORDS],
        host_ns: u64,
        admitted: bool,
    ) {
        let tag = words[0];
        let index = if tag < u32::BITS {
            role as usize * 32 + tag as usize
        } else {
            64 + role as usize
        };
        let sample = &mut self.samples[index];
        sample.count = sample.count.saturating_add(1);
        sample.admitted = sample.admitted.saturating_add(u64::from(admitted));
        sample.last_admitted = admitted;
        sample.host_ns = host_ns;
        sample.words = words;
    }

    pub(super) fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(
            f,
            "format=2 units=raw host_time=monotonic_ns achieved_frequency=unqualified admission=transport"
        )?;
        for (index, sample) in self
            .samples
            .iter()
            .enumerate()
            .filter(|(_, s)| s.count != 0)
        {
            writeln!(
                f,
                "role={} tag={:#x} count={} admitted={} last_admitted={} host_ns={} words={:08x?}",
                if index < 64 { index / 32 } else { index - 64 },
                sample.words[0],
                sample.count,
                sample.admitted,
                sample.last_admitted,
                sample.host_ns,
                sample.words
            )?;
        }
        Ok(())
    }
}

/// The view owns the session, not a pointer into a detachable Firmware.
pub(super) struct View(pub(super) kernel::sync::Arc<super::Shared>);

impl kernel::debugfs::Writer for View {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let mut state = self.0.state.lock();
        let Some(firmware) = state.as_deref_mut() else {
            return writeln!(f, "online=0");
        };
        firmware.observe_statistics_power();
        let dev: &kernel::device::Device = &self.0.dev;
        writeln!(
            f,
            "online=1 device={} graph={:?} sampled_ns={}",
            dev.name().to_str().unwrap_or("<non-UTF8>"),
            self.0.variant.graph,
            super::now_ns()
        )?;
        write_power(firmware, f)?;
        firmware
            .rings
            .write_statistics_transport(&firmware.init, firmware.roles(), f)?;
        firmware.rings.write_reports(f)
    }
}

fn write_power(
    firmware: &super::Firmware,
    f: &mut kernel::fmt::Formatter<'_>,
) -> kernel::fmt::Result {
    if firmware.rings.statistics_suspended() {
        return writeln!(f, "power_management=firmware suspended=1");
    }
    let core_power = firmware.bridge.regs.g17_core_power();
    writeln!(f, "power_management=firmware sampled_ns={} core_power={:#x} cores_powered={} host_hold={} idle_hold_ms={}",
        super::now_ns(), core_power, core_power & 0xf != 0, firmware.hold.active(),
        super::power::IDLE_HOLD_MS.load(core::sync::atomic::Ordering::Relaxed))?;
    firmware.port.write_power(f)
}

/// Actual firmware-managed core gating, separate from generic Linux runtime PM.
pub(super) struct PowerView(pub(super) kernel::sync::Arc<super::Shared>);

impl kernel::debugfs::Writer for PowerView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let state = self.0.state.lock();
        let Some(firmware) = state.as_deref() else {
            return writeln!(f, "online=0");
        };
        write_power(firmware, f)
    }
}

#[cfg_attr(not(test), kernel::macros::kunit_tests(asahi_g17_reports))]
#[cfg(any(test, CONFIG_KUNIT))]
mod tests {
    use super::*;

    #[test]
    fn retains_all_words_and_separates_role_and_tag() {
        let mut reports = Reports::new().unwrap();
        let mut words = [0; EVENT_WORDS];
        words[0] = 16;
        words[17] = 0xdeadbeef;
        reports.record(Role::Primary, words, 100, true);
        reports.record(Role::Primary, words, 101, false);
        reports.record(Role::Secondary, words, 102, true);
        assert_eq!(reports.samples[16].count, 2);
        assert_eq!(reports.samples[16].admitted, 1);
        assert!(!reports.samples[16].last_admitted);
        assert_eq!(reports.samples[16].host_ns, 101);
        assert_eq!(reports.samples[16].words, words);
        assert_eq!(reports.samples[48].count, 1);
        words[0] = 17;
        reports.record(Role::Primary, words, 103, true);
        assert_eq!(reports.samples[17].count, 1);
        assert_eq!(reports.samples[16].count, 2);
    }

    #[test]
    fn retains_unknown_tags_without_indexing_outside_storage() {
        let mut reports = Reports::new().unwrap();
        for tag in [0, 15, 29, 31, 32, u32::MAX] {
            let mut words = [0; EVENT_WORDS];
            words[0] = tag;
            reports.record(Role::Primary, words, 1, false);
        }
        assert_eq!(reports.samples[0].count, 1);
        assert_eq!(reports.samples[31].count, 1);
        assert_eq!(reports.samples[64].count, 2);
        assert_eq!(reports.samples[64].words[0], u32::MAX);
        assert!(reports.samples.iter().all(|s| s.admitted == 0));
        assert!(reports.samples[32..64].iter().all(|s| s.count == 0));
    }
}
