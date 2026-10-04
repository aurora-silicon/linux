// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Cumulative native retirement timings, without MMIO or runtime locks on read.
//!
//! The serialized runtime is the sole writer. Bounded seqlock-style reads use
//! atomic fields (no data races) and explicitly report contention. GPU spans
//! include firmware/engine gaps and may overlap: they are not utilization.
use core::sync::atomic::{fence, AtomicU64, Ordering};

pub(crate) struct Stats {
    sequence: AtomicU64,
    fields: [AtomicU64; 18],
}
impl Stats {
    pub(crate) fn new() -> Self {
        Self { sequence: AtomicU64::new(0), fields: [const { AtomicU64::new(0) }; 18] }
    }
    /// One call per proven native retirement (a batch on M3, a command on M4).
    /// Stages are tiling, fragment, inter-stage gap, in nanoseconds.
    pub(crate) fn record(&self, render: bool, commands: u64, prepare: i64,
                        active: i64, gpu_span: u64, stages: [u64; 3]) {
        let offset = if render { 0 } else { 9 };
        let values = [1, commands, u64::from(gpu_span != 0), prepare.max(0) as u64,
                      active.max(0) as u64, gpu_span, stages[0], stages[1], stages[2]];
        self.sequence.fetch_add(1, Ordering::AcqRel);
        for (field, value) in self.fields[offset..offset + 9].iter().zip(values) {
            field.fetch_add(value, Ordering::Relaxed);
        }
        self.sequence.fetch_add(1, Ordering::Release);
    }
    fn snapshot(&self) -> Option<[u64; 18]> {
        for _ in 0..8 {
            let before = self.sequence.load(Ordering::Acquire);
            if before & 1 != 0 { continue; }
            let values = core::array::from_fn(|i| self.fields[i].load(Ordering::Relaxed));
            fence(Ordering::Acquire);
            if before == self.sequence.load(Ordering::Relaxed) { return Some(values); }
        }
        None
    }
    pub(crate) fn write(&self, f: &mut kernel::fmt::Formatter<'_>, generation: u64,
                       healthy: bool) -> kernel::fmt::Result {
        let snapshot = self.snapshot();
        writeln!(f, "version=1 generation_ns={} healthy={} busy={} unit=ns scope=native_retirement",
                 generation, u8::from(healthy), u8::from(snapshot.is_none()))?;
        if let Some(values) = snapshot {
            for (engine, row) in ["render", "compute"].iter().zip(values.chunks_exact(9)) {
                writeln!(f, "{} retirements={} commands={} nonzero_gpu_samples={} prepare_ns={} active_ns={} gpu_span_ns={} tiling_ns={} fragment_ns={} stage_gap_ns={}",
                         engine, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7], row[8])?;
            }
        }
        Ok(())
    }
}
