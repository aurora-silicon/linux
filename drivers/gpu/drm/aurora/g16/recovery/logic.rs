// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 recovery protocol decisions, independent of kernel objects and clocks.

/// Native 32-bit stage stamps advance by 0x100, including zero. Signed modular
/// comparisons require less than 2^23 command increments between live references.
/// Bound the receipt by the newest publication too, excluding stale/future values.
pub(crate) const fn stamp_reached(obs: u32, own: u32, tail: u32) -> bool {
    let span = tail.wrapping_sub(own);
    (obs | own | tail) & 0xff == 0
        && span < (STAMP_HALF_WINDOW as u32) << 8
        && obs.wrapping_sub(own) <= span
}

/// Cursor `c` of a ring of `m` entries passed the end `h` of entries starting at `b`, within
/// the tail `t`.
///
/// `c` is a firmware-written cursor: the u32 arithmetic wraps (a garbage cursor must not trip
/// `CONFIG_RUST_OVERFLOW_CHECKS`).
#[allow(dead_code)]
pub(crate) const fn ring_passed(c: u32, b: u32, h: u32, t: u32, m: u32) -> bool {
    let dc = c.wrapping_add(m).wrapping_sub(b) % m;
    let dh = h.wrapping_add(m).wrapping_sub(b) % m;
    let dt = t.wrapping_add(m).wrapping_sub(b) % m;
    dh <= dc && dc <= dt
}

/// FirmwareData words and affected-work snapshot geometry.
pub(crate) const SNAPSHOT: usize = 0x40b8;
pub(crate) const SNAPSHOT_ENTRIES: usize = 448;
pub(crate) const GENERATION: usize = 0x4fa0;
pub(crate) const PHASE: usize = 0x4fb0;
pub(crate) const RECOVERY_BUDGET_NS: u64 = 1_000_000_000;
pub(crate) const EVENT_WAIT_NS: u64 = 50_000_000;
pub(crate) const WINDOW_NS: u64 = 10_000_000_000;
pub(crate) const WINDOW_LIMIT: u32 = 32;
#[allow(dead_code)]
pub(crate) const ABORT_GRACE_NS: u64 = 20_000_000;
pub(crate) const EXPIRED: u64 = 0;

/// Native wire stamps repeat every 2^24 commands. Software dependencies and owner
/// frontiers use monotonic u64 ordinals, not these wrapped values. Only the current
/// serialized flight retains a pending firmware dependency; all others fully retired.
pub(crate) const FIRMWARE_STAMP_PERIOD: u64 = 1 << 24;
pub(crate) const STAMP_HALF_WINDOW: u64 = 1 << 23;

pub(crate) fn next_ordinal(previous: u64) -> Option<u64> {
    previous.checked_add(1)
}

/// Job success and storage retirement are separate facts. An affected job still fails
/// after a full normal hardware receipt, but its storage has completed every owner
/// obligation. A passed abort proves queue progress only and never permits reuse.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Retirement {
    Pending,
    Full,
    Aborted,
}

impl Retirement {
    pub(crate) const fn complete(self) -> bool {
        !matches!(self, Self::Pending)
    }

    pub(crate) const fn owner_retired(self) -> bool {
        matches!(self, Self::Full)
    }

    pub(crate) const fn job_failed(self, affected: bool) -> bool {
        affected || matches!(self, Self::Aborted)
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum RecoveryAction {
    Wait,
    MarkAndAcknowledge,
    Resume,
    ProtocolError,
}

/// Only the host writes phases 2 and 0; the firmware publishes phases 1 and 3.
/// An acknowledged recovery may not return to phase 0 or start a new phase 1.
pub(crate) fn action(phase: u32, answered: bool, generation_matches: bool) -> RecoveryAction {
    match (phase, answered) {
        (0, false) => RecoveryAction::Wait,
        (1, false) => RecoveryAction::MarkAndAcknowledge,
        (2, true) => RecoveryAction::Wait,
        (3, true) if generation_matches => RecoveryAction::Resume,
        _ => RecoveryAction::ProtocolError,
    }
}

/// One valid-prefix entry of the firmware's snapshot. Invalid entries terminate it.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct SnapshotEntry {
    pub(crate) flags: u32,
    pub(crate) event: u32,
    pub(crate) target: u32,
}

impl SnapshotEntry {
    pub(crate) fn decode(flags: u32, target: u32) -> Option<Self> {
        if flags & 1 == 0 {
            return None;
        }
        Some(Self {
            flags,
            event: (flags >> 4) & 0x7f,
            target,
        })
    }
}

/// A late snapshot can name the last complete receipt even after its flight was removed.
pub(crate) fn last_retired_target(last: &[Option<u32>; 3], event: u32, target: u32) -> bool {
    let index = match event {
        15 => 0,
        0x70 => 1,
        0x71 => 2,
        _ => return false,
    };
    last[index] == Some(target)
}

pub(crate) const fn expired(now: u64, deadline: u64) -> bool {
    now > deadline
}

/// Saturating arithmetic keeps corrupted parameter values out of overflow paths.
pub(crate) fn renew(now: u64, deadline_ms: u32, compute_deadline_ms: u32) -> (u64, u64) {
    (
        host_deadline(now, deadline_ms),
        host_deadline(now, compute_deadline_ms),
    )
}

/// Zero explicitly opts long-running work out of the host's administrative watchdog.
/// Completion stamps cannot distinguish a running shader from a firmware-silent hang.
fn host_deadline(now: u64, milliseconds: u32) -> u64 {
    if milliseconds == 0 {
        u64::MAX
    } else {
        now.saturating_add(u64::from(milliseconds.max(100)) * 1_000_000)
    }
}

/// Admit at most 32 recoveries in the fixed ten-second window, as in the
/// qualified M4 provider. Refusal must leave the GPU in fail-stop.
pub(crate) fn admit(now: u64, window_ns: &mut u64, count: &mut u32) -> bool {
    if now.saturating_sub(*window_ns) > WINDOW_NS {
        *window_ns = now;
        *count = 0;
    }
    if *count >= WINDOW_LIMIT {
        return false;
    }
    *count += 1;
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn late_snapshot_requires_exact_stage_target_and_real_full_receipt() {
        assert!(!last_retired_target(&[None; 3], 0x70, 0));
        let last = [Some(0x100), Some(0), Some(0x200)];
        for (event, target) in [(15, 0x100), (0x70, 0), (0x71, 0x200)] {
            assert!(last_retired_target(&last, event, target));
            assert!(!last_retired_target(
                &last,
                event,
                target.wrapping_add(0x100)
            ));
        }
        assert!(!last_retired_target(&last, 0x72, 0));
    }

    #[test]
    fn handshake_only_answers_owned_phases() {
        assert_eq!(action(0, false, true), RecoveryAction::Wait);
        assert_eq!(action(1, false, true), RecoveryAction::MarkAndAcknowledge);
        assert_eq!(action(2, true, true), RecoveryAction::Wait);
        assert_eq!(action(3, true, true), RecoveryAction::Resume);
        for phase in [0, 1, 4, u32::MAX] {
            assert_eq!(action(phase, true, true), RecoveryAction::ProtocolError);
        }
        for phase in [2, 3, 4] {
            assert_eq!(action(phase, false, true), RecoveryAction::ProtocolError);
        }
        assert_eq!(action(3, true, false), RecoveryAction::ProtocolError);
    }

    #[test]
    fn affected_job_failure_does_not_erase_a_full_owner_receipt() {
        assert!(!Retirement::Pending.complete());
        assert!(!Retirement::Pending.owner_retired());
        assert!(Retirement::Full.complete());
        assert!(Retirement::Full.owner_retired());
        assert!(!Retirement::Full.job_failed(false));
        assert!(Retirement::Full.job_failed(true));
        for affected in [false, true] {
            assert!(Retirement::Aborted.complete());
            assert!(Retirement::Aborted.job_failed(affected));
            assert!(!Retirement::Aborted.owner_retired());
        }
    }

    #[test]
    fn snapshot_identity_is_exact() {
        assert!(SnapshotEntry::decode(0, 0x123400).is_none());
        let flags = 1 | 2 | 8 | (0x71 << 4);
        assert_eq!(
            SnapshotEntry::decode(flags, 0x123400),
            Some(SnapshotEntry {
                flags,
                event: 0x71,
                target: 0x123400,
            })
        );
        assert_eq!(
            SnapshotEntry::decode(1 | (15 << 4), 0xffff_ff00)
                .unwrap()
                .event,
            15
        );
        assert_eq!(SNAPSHOT + SNAPSHOT_ENTRIES * 8, 0x4eb8);
        assert!(SNAPSHOT + SNAPSHOT_ENTRIES * 8 <= GENERATION);
    }

    #[test]
    fn recovery_storm_refuses_the_thirty_third() {
        let (mut window, mut count) = (1, 0);
        for _ in 0..WINDOW_LIMIT {
            assert!(admit(100, &mut window, &mut count));
        }
        assert!(!admit(101, &mut window, &mut count));
        assert_eq!(count, WINDOW_LIMIT);
        assert!(!admit(WINDOW_NS + 1, &mut window, &mut count));
        assert!(admit(WINDOW_NS + 2, &mut window, &mut count));
        assert_eq!((window, count), (WINDOW_NS + 2, 1));
    }

    #[test]
    fn deadlines_are_absolute_and_do_not_wrap() {
        assert_eq!(PHASE, 0x4fb0);
        let first_record_ns = 5_000_000_000;
        let deadline = first_record_ns + RECOVERY_BUDGET_NS;
        assert!(!expired(deadline, deadline));
        assert!(expired(deadline + 1, deadline));
        assert!(EVENT_WAIT_NS < RECOVERY_BUDGET_NS);
        assert!(ABORT_GRACE_NS < RECOVERY_BUDGET_NS);
        assert!(expired(1, EXPIRED));
        assert_eq!(renew(7, 5000, 1000), (5_000_000_007, 1_000_000_007));
        assert_eq!(renew(u64::MAX - 3, 5000, 1000), (u64::MAX, u64::MAX));
        assert_eq!(renew(7, 0, 1000), (u64::MAX, 1_000_000_007));
        assert_eq!(renew(7, 5000, 0), (5_000_000_007, u64::MAX));
        assert_eq!(renew(7, 1, 1), (100_000_007, 100_000_007));
    }

    #[test]
    fn logical_ordinals_remain_monotonic_across_native_stamp_wrap() {
        assert_eq!(next_ordinal(0), Some(1));
        assert_eq!(
            next_ordinal(FIRMWARE_STAMP_PERIOD - 1),
            Some(FIRMWARE_STAMP_PERIOD)
        );
        assert_eq!(
            next_ordinal(FIRMWARE_STAMP_PERIOD),
            Some(FIRMWARE_STAMP_PERIOD + 1)
        );
        assert_eq!(next_ordinal(u64::MAX), None);
        assert_eq!(FIRMWARE_STAMP_PERIOD, 2 * STAMP_HALF_WINDOW);
    }

    #[test]
    fn zero_is_a_real_completion_and_modular_distance_is_bounded() {
        assert!(stamp_reached(0, 0, 0));
        assert!(!stamp_reached(0xffff_ff00, 0, 0));
        assert!(stamp_reached(0, 0xffff_ff00, 0x100));
        assert!(stamp_reached(0x100, 0, 0x100));
        assert!(!stamp_reached(0x200, 0, 0x100));
        assert!(!stamp_reached(0, 0, 0x8000_0000));
        for start in [0x3fff_ff00u32, 0x7fff_ff00, 0xbfff_ff00, 0xffff_ff00] {
            let end = start.wrapping_add(0x100);
            assert!(!stamp_reached(start, end, end));
            assert!(stamp_reached(end, end, end));
        }
    }

    #[test]
    fn stamps_and_wrapping_receipts() {
        assert!(!stamp_reached(0, 0x100, 0x300));
        assert!(!stamp_reached(0x101, 0x100, 0x300));
        assert!(stamp_reached(0x200, 0x100, 0x300));
        assert!(!stamp_reached(0x400, 0x100, 0x300));
        assert!(stamp_reached(0x100, 0xffff_ff00, 0x200));
        assert!(ring_passed(2, 0xfe, 1, 5, 256));
        assert!(!ring_passed(0xff, 0xfe, 1, 5, 256));
        assert!(!ring_passed(u32::MAX, 0, 1, 5, 256));
    }
}
