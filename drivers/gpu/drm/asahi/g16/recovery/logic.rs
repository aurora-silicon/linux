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
    if m == 0 || c >= m || b >= m || h >= m || t >= m {
        return false;
    }
    let dc = c.wrapping_add(m).wrapping_sub(b) % m;
    let dh = h.wrapping_add(m).wrapping_sub(b) % m;
    let dt = t.wrapping_add(m).wrapping_sub(b) % m;
    dh <= dc && dc <= dt
}

// The FirmwareData words (phase, generation, affected-work snapshot) are per firmware ABI:
// `fw::initdata::FirmwareData`.
pub(crate) const RECOVERY_BUDGET_NS: u64 = 1_000_000_000;
pub(crate) const EVENT_WAIT_NS: u64 = 50_000_000;
/// Bounded wait before dismissing a trailing phase-0 recovery notification on an idle GPU.
pub(crate) const PHANTOM_PHASE0_WAIT_NS: u64 = 50_000_000;
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

/// A type-4 or 0x16 notification drained after a completed recovery leaves phase at 0 with
/// no flights in flight. Consume it after a short bounded wait instead of spinning to fail-stop.
pub(crate) const fn phantom_phase0(
    phase: u32,
    answered: bool,
    flights_empty: bool,
    elapsed_ns: u64,
) -> bool {
    phase == 0 && !answered && flights_empty && elapsed_ns >= PHANTOM_PHASE0_WAIT_NS
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

/// Where a recovery that ran past [`RECOVERY_BUDGET_NS`] stopped, from the last phase the host
/// read and whether the host had acknowledged phase 1 ([`action`]'s inputs).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Stall {
    /// The firmware announced a recovery but never published phase 1.
    NotHalted,
    /// The firmware published phase 1 and the host had not acknowledged it.
    NotAcknowledged,
    /// The host acknowledged phase 1 and the firmware never published phase 3.
    NotResumed,
}

impl Stall {
    pub(crate) const fn of(phase: u32, answered: bool) -> Self {
        match (phase, answered) {
            (_, true) => Self::NotResumed,
            (1, false) => Self::NotAcknowledged,
            _ => Self::NotHalted,
        }
    }

    pub(crate) const fn describe(self) -> &'static str {
        match self {
            Self::NotHalted => "firmware never halted in phase 1",
            Self::NotAcknowledged => "phase 1 not acknowledged",
            Self::NotResumed => "firmware never published phase 3",
        }
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

/// Bounded per-stage history of recently retired firmware stamps.
pub(crate) const RETIRED_STAMPS_CAP: usize = 64;
/// Number of compute pipes tracked by [`RetiredStamps`] (matches `hw::sizes::COMPUTE_PIPES`).
pub(crate) const RETIRED_COMPUTE_PIPES: usize = 16;
/// Total retired-stamp stages: compute pipes `0..RETIRED_COMPUTE_PIPES`, TA, FS.
pub(crate) const RETIRED_STAGES: usize = RETIRED_COMPUTE_PIPES + 2;

/// Bounded per-stage ring of recently retired compute pipe and TA/FS firmware stamps
/// so a late recovery snapshot arriving after pipelined retirement resolves any recently
/// retired member, not only the single newest flight.
#[derive(Clone, Debug)]
pub(crate) struct RetiredStamps {
    stamps: [[u32; RETIRED_STAMPS_CAP]; RETIRED_STAGES],
    len: [u8; RETIRED_STAGES],
    next: [u8; RETIRED_STAGES],
}

impl RetiredStamps {
    pub(crate) const fn new() -> Self {
        Self {
            stamps: [[0; RETIRED_STAMPS_CAP]; RETIRED_STAGES],
            len: [0; RETIRED_STAGES],
            next: [0; RETIRED_STAGES],
        }
    }

    /// Initializes an uninitialized heap slot to the empty zero state without materializing a
    /// multi-kilobyte temporary on the caller's stack.
    #[allow(dead_code)]
    pub(crate) fn init_zero(slot: &mut core::mem::MaybeUninit<Self>) {
        // SAFETY: All fields of `RetiredStamps` are integer arrays for which all-zeroes is valid
        // and identical to `RetiredStamps::new()`.
        unsafe { core::ptr::write_bytes(slot.as_mut_ptr(), 0, 1) };
    }

    pub(crate) fn push(&mut self, stage: usize, target: u32) {
        if stage >= RETIRED_STAGES {
            return;
        }
        let idx = self.next[stage] as usize;
        self.stamps[stage][idx] = target;
        self.next[stage] = ((idx + 1) % RETIRED_STAMPS_CAP) as u8;
        if (self.len[stage] as usize) < RETIRED_STAMPS_CAP {
            self.len[stage] += 1;
        }
    }

    pub(crate) fn contains(&self, event: u32, target: u32) -> bool {
        let stage = match event {
            e if (15..15 + RETIRED_COMPUTE_PIPES as u32).contains(&e) => (e - 15) as usize,
            0x70 => RETIRED_COMPUTE_PIPES,
            0x71 => RETIRED_COMPUTE_PIPES + 1,
            _ => return false,
        };
        let len = self.len[stage] as usize;
        self.stamps[stage][..len].contains(&target)
    }
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

/// Ray traversal and acceleration-structure builds can run past one administrative deadline
/// (CTS stress). As on the M4 Pro (g17 `timeout::select`: two-second polls plus four extra
/// ones), a job with such a command gets at least ten seconds per flight. Still bounded:
/// this is an allowance, not evidence of GPU progress.
pub(crate) const LONG_DEADLINE_MS: u32 = 10_000;

/// The administrative deadline of a long job, ms: at least [`LONG_DEADLINE_MS`], unless the
/// deadline is disabled (zero).
pub(crate) const fn long_deadline_ms(milliseconds: u32) -> u32 {
    if milliseconds == 0 || milliseconds >= LONG_DEADLINE_MS {
        milliseconds
    } else {
        LONG_DEADLINE_MS
    }
}

/// The four host deadlines renewed together: ordinary and compute-only, each plain and for a
/// long job.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct Deadlines {
    pub(crate) any: u64,
    pub(crate) compute: u64,
    pub(crate) long_any: u64,
    pub(crate) long_compute: u64,
}

impl Deadlines {
    pub(crate) const EXPIRED: Self = Self {
        any: EXPIRED,
        compute: EXPIRED,
        long_any: EXPIRED,
        long_compute: EXPIRED,
    };

    /// All four from `now`.
    pub(crate) fn renew(now: u64, deadline_ms: u32, compute_deadline_ms: u32) -> Self {
        let (any, compute) = renew(now, deadline_ms, compute_deadline_ms);
        let (long_any, long_compute) = renew(
            now,
            long_deadline_ms(deadline_ms),
            long_deadline_ms(compute_deadline_ms),
        );
        Self {
            any,
            compute,
            long_any,
            long_compute,
        }
    }

    /// The deadline of the oldest flight: compute-only or not, of a long job or not.
    pub(crate) const fn select(&self, compute_only: bool, long: bool) -> u64 {
        match (compute_only, long) {
            (true, false) => self.compute,
            (false, false) => self.any,
            (true, true) => self.long_compute,
            (false, true) => self.long_any,
        }
    }
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
        let mut retired = RetiredStamps::new();
        assert!(!retired.contains(0x70, 0));
        retired.push(0, 0x100);
        retired.push(1, 0x300);
        retired.push(RETIRED_COMPUTE_PIPES - 1, 0x700);
        retired.push(RETIRED_COMPUTE_PIPES, 0);
        retired.push(RETIRED_COMPUTE_PIPES + 1, 0x200);
        for (event, target) in [
            (15, 0x100),
            (16, 0x300),
            (15 + RETIRED_COMPUTE_PIPES as u32 - 1, 0x700),
            (0x70, 0),
            (0x71, 0x200),
        ] {
            assert!(retired.contains(event, target));
            assert!(!retired.contains(event, target.wrapping_add(0x100)));
        }
        assert!(!retired.contains(0x72, 0));
        // Pipelined bursts retain all recent stamps up to RETIRED_STAMPS_CAP and evict oldest.
        let mut ring = RetiredStamps::new();
        for i in 0..(RETIRED_STAMPS_CAP as u32 + 4) {
            ring.push(0, (i + 1) << 8);
        }
        assert!(!ring.contains(15, 0x100));
        assert!(!ring.contains(15, 0x400));
        assert!(ring.contains(15, 0x500));
        assert!(ring.contains(15, (RETIRED_STAMPS_CAP as u32 + 4) << 8));
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
    fn budget_overrun_names_the_phase_that_never_arrived() {
        // dcp103b: a type-4 event and its statistics arrived, then 1000 ms passed. Which side
        // stalled follows from the last phase read and whether the host had answered.
        assert_eq!(Stall::of(0, false), Stall::NotHalted);
        assert_eq!(Stall::of(1, false), Stall::NotAcknowledged);
        assert_eq!(Stall::of(2, true), Stall::NotResumed);
        for phase in [3, 4, u32::MAX] {
            assert_eq!(Stall::of(phase, false), Stall::NotHalted);
        }
        let all = [Stall::NotHalted, Stall::NotAcknowledged, Stall::NotResumed];
        for (i, a) in all.iter().enumerate() {
            for b in &all[i + 1..] {
                assert_ne!(a.describe(), b.describe());
            }
        }
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
        let first_record_ns = 5_000_000_000;
        let deadline = first_record_ns + RECOVERY_BUDGET_NS;
        assert!(!expired(deadline, deadline));
        assert!(expired(deadline + 1, deadline));
        assert!(EVENT_WAIT_NS < RECOVERY_BUDGET_NS);
        assert!(PHANTOM_PHASE0_WAIT_NS < RECOVERY_BUDGET_NS);
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
    fn phantom_phase0_clears_only_idle_unanswered_phase0_after_wait() {
        assert!(!phantom_phase0(0, false, true, PHANTOM_PHASE0_WAIT_NS - 1));
        assert!(phantom_phase0(0, false, true, PHANTOM_PHASE0_WAIT_NS));
        assert!(!phantom_phase0(0, false, false, PHANTOM_PHASE0_WAIT_NS));
        assert!(!phantom_phase0(0, true, true, PHANTOM_PHASE0_WAIT_NS));
        assert!(!phantom_phase0(1, false, true, PHANTOM_PHASE0_WAIT_NS));
        assert!(!phantom_phase0(2, true, true, PHANTOM_PHASE0_WAIT_NS));
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
        assert!(!ring_passed(256, 0, 1, 2, 256));
        assert!(!ring_passed(257, 0, 1, 2, 256));
        assert!(!ring_passed(u32::MAX, 0, 1, 5, 256));
    }
}
