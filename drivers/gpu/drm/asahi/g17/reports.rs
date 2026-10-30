// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Preserve the firmware's statistics without assigning unqualified units or
//! treating another firmware's pstate report as an achieved GPU frequency.
//! Writers and readers hold the device mutex; reads never power the GPU up.
//!
//! Two record types are understood and also reach `agx_stats`, both from the
//! primary instance, which runs all work channels:
//!
//! - [`STATISTICS_UTILIZATION`], words 1-2 its timestamp and words 3-6 the
//!   percentage of the window since the previous record that the tiler, the
//!   renderer, compute and the device were busy. The firmware derives each as
//!   `100 - idle * 100 / window` and restarts the window, and counts the device
//!   idle only while all three engines are idle, so the device word is their
//!   exact union and integrates to busy time. macOS averages words 3, 4 and 6
//!   into its Tiler, Renderer and Device Utilization, and ignores word 5.
//! - [`STATISTICS_SOFTWARE_PSTATE`], word 3 the state index that macOS calls
//!   GPU_SW, 0 while the GPU is off. It is a request, not an achieved frequency.

use super::{
    fw::channels::{
        EVENT_WORDS, STATISTICS_SOFTWARE_PSTATE, STATISTICS_TYPES, STATISTICS_UTILIZATION,
    },
    Role,
};
use crate::{hw::t8140::qos::CLOCK_HZ, module_parameters, stats::StatsSnapshot};
use kernel::{prelude::*, sync::Arc};

#[derive(Clone, Copy, Default)]
struct Sample {
    count: u64,
    host_ns: u64,
    words: [u32; EVENT_WORDS],
}

pub(super) struct Reports {
    samples: KVec<Sample>,
    stats: Arc<StatsSnapshot>,
    last_utilization: u64,
}

impl Reports {
    pub(super) fn new(stats: Arc<StatsSnapshot>) -> Result<Self> {
        // Allocate before firmware starts; receive and debugfs paths allocate nothing.
        let mut samples = KVec::with_capacity(64, GFP_KERNEL)?;
        for _ in 0..64 {
            samples.push(Sample::default(), GFP_KERNEL)?;
        }
        Ok(Self { samples, stats, last_utilization: 0 })
    }

    pub(super) fn record(&mut self, role: Role, words: [u32; EVENT_WORDS], host_ns: u64) {
        let tag = words[0];
        if tag >= u32::BITS || STATISTICS_TYPES & (1 << tag) == 0 {
            return;
        }
        let sample = &mut self.samples[role as usize * 32 + tag as usize];
        sample.count = sample.count.saturating_add(1);
        sample.host_ns = host_ns;
        sample.words = words;
        if role == Role::Primary && module_parameters::stats_export.value() != 0 {
            self.export(&words);
        }
    }

    fn export(&mut self, words: &[u32; EVENT_WORDS]) {
        match words[0] {
            STATISTICS_UTILIZATION => self.stats.note_utilization(
                &mut self.last_utilization,
                u64::from(words[1]) | u64::from(words[2]) << 32,
                [words[3], words[4], words[5], words[6]],
                CLOCK_HZ,
            ),
            STATISTICS_SOFTWARE_PSTATE => self.stats.note_pstate(words[3]),
            _ => {}
        }
    }

    pub(super) fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(f, "format=1 units=raw host_time=monotonic_ns achieved_frequency=unqualified")?;
        for (index, sample) in self.samples.iter().enumerate().filter(|(_, s)| s.count != 0) {
            writeln!(f, "role={} tag={:#x} count={} host_ns={} words={:08x?}",
                index / 32, index % 32, sample.count, sample.host_ns, sample.words)?;
        }
        Ok(())
    }
}

/// The view owns the session, not a pointer into a detachable Firmware.
pub(super) struct View(pub(super) kernel::sync::Arc<super::Shared>);

impl kernel::debugfs::Writer for View {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let state = self.0.state.lock();
        let Some(firmware) = state.as_deref() else { return writeln!(f, "online=0"); };
        writeln!(f, "online=1 graph={:?} sampled_ns={}", self.0.variant.graph, super::now_ns())?;
        write_power(firmware, f)?;
        firmware.rings.write_reports(f)
    }
}

fn write_power(firmware: &super::Firmware, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
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
        let Some(firmware) = state.as_deref() else { return writeln!(f, "online=0"); };
        write_power(firmware, f)
    }
}

#[kernel::macros::kunit_tests(asahi_g17_reports)]
mod tests {
    use super::*;
    use crate::stats::{ABSENT_POWER, ABSENT_PSTATE, ABSENT_TEMPERATURE};
    use core::sync::atomic::Ordering;

    fn reports() -> Reports {
        let stats = Arc::new(StatsSnapshot::utilization_only(), GFP_KERNEL).unwrap();
        Reports::new(stats).unwrap()
    }

    fn utilization(ticks: u64, percent: [u32; 4]) -> [u32; EVENT_WORDS] {
        let mut words = [0; EVENT_WORDS];
        words[0] = 0x13;
        words[1] = ticks as u32;
        words[2] = (ticks >> 32) as u32;
        words[3..7].copy_from_slice(&percent);
        // The firmware reuses its record buffer, so stale words surround the payload.
        words[7] = 9;
        words[11] = 1000;
        words
    }

    #[test]
    fn utilization_records_integrate_to_busy_time() {
        let mut reports = reports();
        // The low word wraps every three minutes, here between the two records.
        let start = 0x42_ffff_0000;
        reports.record(Role::Primary, utilization(start, [5, 10, 0, 10]), 1);
        reports.record(Role::Primary, utilization(start + CLOCK_HZ, [26, 41, 0, 41]), 2);
        assert_eq!(reports.stats.busy_ns.load(Ordering::Relaxed), 410_000_000);
        assert_eq!(reports.stats.sampled_ns.load(Ordering::Relaxed), 1_000_000_000);
        assert_eq!(reports.stats.util1.load(Ordering::Relaxed), 26);
        assert_eq!(reports.stats.util2.load(Ordering::Relaxed), 41);
        assert_eq!(reports.stats.util3.load(Ordering::Relaxed), 0);
        assert_eq!(reports.stats.util4.load(Ordering::Relaxed), 41);
    }

    #[test]
    fn secondary_instance_does_not_reach_the_snapshot() {
        let mut reports = reports();
        reports.record(Role::Secondary, utilization(CLOCK_HZ, [100; 4]), 1);
        reports.record(Role::Secondary, utilization(2 * CLOCK_HZ, [100; 4]), 2);
        assert_eq!(reports.stats.busy_ns.load(Ordering::Relaxed), 0);
        assert_eq!(reports.stats.util4.load(Ordering::Relaxed), 0);
        assert_eq!(reports.samples[32 + STATISTICS_UTILIZATION as usize].count, 2);
    }

    #[test]
    fn pstate_key_appears_with_its_first_record() {
        let mut reports = reports();
        let stats = reports.stats.clone();
        let absent = ABSENT_PSTATE | ABSENT_POWER | ABSENT_TEMPERATURE;
        assert_eq!(stats.absent.load(Ordering::Relaxed), absent);
        let mut words = [0; EVENT_WORDS];
        words[0] = 0x1c;
        words[3] = 10;
        reports.record(Role::Primary, words, 1);
        assert_eq!(stats.pstate.load(Ordering::Relaxed), 10);
        assert_eq!(stats.absent.load(Ordering::Relaxed), absent & !ABSENT_PSTATE);
    }

    #[test]
    fn retains_all_words_and_separates_role_and_tag() {
        let mut reports = reports();
        let mut words = [0; EVENT_WORDS];
        words[0] = 16;
        words[17] = 0xdeadbeef;
        reports.record(Role::Primary, words, 100);
        reports.record(Role::Primary, words, 101);
        reports.record(Role::Secondary, words, 102);
        assert_eq!(reports.samples[16].count, 2);
        assert_eq!(reports.samples[16].host_ns, 101);
        assert_eq!(reports.samples[16].words, words);
        assert_eq!(reports.samples[48].count, 1);
        words[0] = 17;
        reports.record(Role::Primary, words, 103);
        assert_eq!(reports.samples[17].count, 1);
        assert_eq!(reports.samples[16].count, 2);
    }

    #[test]
    fn rejects_invalid_tags_before_indexing() {
        let mut reports = reports();
        for tag in [0, 15, 29, 31, 32, u32::MAX] {
            let mut words = [0; EVENT_WORDS];
            words[0] = tag;
            reports.record(Role::Primary, words, 1);
        }
        assert!(reports.samples.iter().all(|s| s.count == 0));
    }
}
