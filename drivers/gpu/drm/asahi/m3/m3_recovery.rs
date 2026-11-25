// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M3 Extended (26.x / 27.2) per-client GPU fault recovery.
//!
//! Shares the 27.2 recovery phase state machine, snapshot decoder, and storm
//! admission window directly from `g16/recovery/logic.rs` (the same protocol
//! G16 and G14 27.2 answer) so no phase logic is duplicated across backends.

#[path = "../g16/recovery/logic.rs"]
#[allow(dead_code)]
pub(crate) mod logic;

pub(crate) use logic::{RecoveryAction, SnapshotEntry};

/// Maximum quarantined aborted flights per boot (half of the 63 user UAT
/// contexts). Exceeding this cap fails closed into device fail-stop rather
/// than leaking bindings/packets without bound across storm windows.
pub(crate) const QUARANTINE_LIMIT: usize = 31;

/// Additional recovery rounds answered while stopped work drains from queues.
pub(crate) const FURTHER_RECOVERIES: u32 = 4;

/// Maximum time to wait for stopped work to drain from work rings after phase 0.
pub(crate) const RECOVERY_DRAIN_MS: i64 = 1000;

/// Overall wall-clock budget for starting recovery rounds under the runtime lock.
pub(crate) const RECOVERY_BUDGET_MS: i64 = 5000;

/// Recovery control-object offsets derived from the `m3_init_26` layout shift
/// and validated against the exact expected `CONTROL_REGION` byte size.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct RecoveryOffsets {
    pub(crate) snapshot: usize,
    pub(crate) snapshot_entries: usize,
    pub(crate) generation: usize,
    pub(crate) phase: usize,
    pub(crate) control_bytes: usize,
}

/// 26.6.2 extended FirmwareData recovery offsets and affected-work snapshot geometry.
const EXTENDED_SNAPSHOT: usize = 0x40b8;
const EXTENDED_SNAPSHOT_ENTRIES: usize = 448;
const EXTENDED_GENERATION: usize = 0x4fa0;
const EXTENDED_PHASE: usize = 0x4fb0;

impl RecoveryOffsets {
    /// Extended control region offsets shifted by the profile's `m3_init_26`
    /// control-table delta (`0` on 26.6.2 / `0xeaf0`, `0x1e0` on 27.2 / `0xedd0`).
    pub(crate) const fn extended(shift: usize, control_bytes: usize) -> Self {
        Self {
            snapshot: EXTENDED_SNAPSHOT + shift,
            snapshot_entries: EXTENDED_SNAPSHOT_ENTRIES,
            generation: EXTENDED_GENERATION + shift,
            phase: EXTENDED_PHASE + shift,
            control_bytes,
        }
    }

    /// Verify `control_bytes` matches the expected layout size and every recovery
    /// word and snapshot entry fits inside it (failing closed on mismatched layouts).
    pub(crate) const fn valid_for(self, control_bytes: usize) -> bool {
        if control_bytes != self.control_bytes {
            return false;
        }
        let Some(snap_bytes) = self.snapshot_entries.checked_mul(8) else {
            return false;
        };
        let Some(snap_end) = self.snapshot.checked_add(snap_bytes) else {
            return false;
        };
        let Some(gen_end) = self.generation.checked_add(8) else {
            return false;
        };
        let Some(phase_end) = self.phase.checked_add(4) else {
            return false;
        };
        snap_end <= self.generation
            && gen_end <= self.phase
            && phase_end <= control_bytes
    }
}

/// A snapshot entry for a stopped tiler (valid + tiler bits): TPC storage must
/// be cleared before answering phase 2, matching G16 and G14 27.2.
pub(crate) const fn stopped_tiler(flags: u32) -> bool {
    flags & 9 == 9
}

/// Whether the recovery snapshot proves a tiler-only stop (at least one stopped
/// tiler entry and no non-tiler/fragment entry). Only in this case does 27.2
/// firmware discard the paired fragment command without advancing `FS.DONE`.
pub(crate) const fn tiler_only_stop(stopped_tiler_seen: bool, non_tiler_seen: bool) -> bool {
    stopped_tiler_seen && !non_tiler_seen
}

/// Trailing `STATS 0x16` notifications can arrive after the host already wrote
/// phase 0 for that recovery generation; do not treat them as a new fault.
pub(crate) const fn is_trailing_stats(
    phase: u32,
    current_gen: u64,
    last_recovered_gen: Option<u64>,
) -> bool {
    if phase != 0 {
        return false;
    }
    match last_recovered_gen {
        Some(prev) => prev == current_gen,
        None => false,
    }
}

/// Cumulative per-boot bound on quarantined flights across recovery windows.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct QuarantineBudget {
    count: usize,
    limit: usize,
}

impl QuarantineBudget {
    pub(crate) const fn new(limit: usize) -> Self {
        Self { count: 0, limit }
    }

    pub(crate) const fn count(&self) -> usize {
        self.count
    }

    pub(crate) const fn can_admit(&self, flights: usize) -> bool {
        match self.count.checked_add(flights) {
            Some(total) => total <= self.limit,
            None => false,
        }
    }

    pub(crate) fn charge(&mut self, flights: usize) -> bool {
        if !self.can_admit(flights) {
            return false;
        }
        self.count += flights;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extended_offsets_match_shared_g16_and_272_layouts() {
        assert!(FURTHER_RECOVERIES > 0);
        assert!(RECOVERY_DRAIN_MS > 0);
        assert!(RECOVERY_BUDGET_MS >= RECOVERY_DRAIN_MS);

        let base = RecoveryOffsets::extended(0, 0xeaf0);
        assert_eq!(base.snapshot, 0x40b8);
        assert_eq!(base.generation, 0x4fa0);
        assert_eq!(base.phase, 0x4fb0);
        assert!(base.valid_for(0xeaf0));
        assert!(!base.valid_for(0x4fb0));
        // Must fail closed if CONTROL_REGION grows to 27.2 without updating the shift.
        assert!(!base.valid_for(0xedd0));

        let v272 = RecoveryOffsets::extended(0x1e0, 0xedd0);
        assert_eq!(v272.snapshot, 0x4298);
        assert_eq!(v272.generation, 0x5180);
        assert_eq!(v272.phase, 0x5190);
        assert!(v272.valid_for(0xedd0));
        assert!(!v272.valid_for(0xeaf0));
        assert!(!v272.valid_for(0x5190));
    }

    #[test]
    fn stopped_tiler_matches_observed_g15g_and_g14_flags() {
        assert!(stopped_tiler(0xd));
        assert!(stopped_tiler(0x1f));
        assert!(!stopped_tiler(0x15));
        assert!(!stopped_tiler(0x27));
        assert!(!stopped_tiler(0x7));
        assert!(!stopped_tiler(0x8));
        assert!(!stopped_tiler(0x0));

        assert!(tiler_only_stop(true, false));
        assert!(!tiler_only_stop(true, true));
        assert!(!tiler_only_stop(false, true));
        assert!(!tiler_only_stop(false, false));
    }

    #[test]
    fn quarantine_budget_bounds_cumulative_retention() {
        let mut budget = QuarantineBudget::new(QUARANTINE_LIMIT);
        assert!(budget.can_admit(1));
        for i in 0..QUARANTINE_LIMIT {
            assert_eq!(budget.count(), i);
            assert!(budget.charge(1));
        }
        assert_eq!(budget.count(), QUARANTINE_LIMIT);
        assert!(!budget.can_admit(1));
        assert!(!budget.charge(1));
        assert_eq!(budget.count(), QUARANTINE_LIMIT);
    }

    #[test]
    fn trailing_stats_ignored_only_for_same_generation_at_phase_zero() {
        assert!(!is_trailing_stats(0, 1, None));
        assert!(is_trailing_stats(0, 1, Some(1)));
        assert!(!is_trailing_stats(0, 2, Some(1)));
        assert!(!is_trailing_stats(1, 1, Some(1)));
        assert!(!is_trailing_stats(1, 2, Some(1)));
    }

    #[test]
    fn shared_g16_phase_table_and_storm_window_work() {
        assert_eq!(logic::action(0, false, true), RecoveryAction::Wait);
        assert_eq!(
            logic::action(1, false, true),
            RecoveryAction::MarkAndAcknowledge
        );
        assert_eq!(logic::action(2, true, true), RecoveryAction::Wait);
        assert_eq!(logic::action(3, true, true), RecoveryAction::Resume);
        assert_eq!(logic::action(3, true, false), RecoveryAction::ProtocolError);
        assert_eq!(
            SnapshotEntry::decode(0xd, 0x7a00_0200),
            Some(SnapshotEntry {
                flags: 0xd,
                event: 0,
                target: 0x7a00_0200,
            })
        );
        let (mut win, mut count) = (0, 0);
        for _ in 0..logic::WINDOW_LIMIT {
            assert!(logic::admit(10, &mut win, &mut count));
        }
        assert!(!logic::admit(11, &mut win, &mut count));
    }
}
