// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G14 firmware queue and event records with no kernel or MMIO dependencies.

#[cfg(not(test))]
use kernel::prelude::kunit_tests;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum QueueError {
    Geometry,
    Full,
}

/// Compute the next producer after validating the shared cursor range and available space.
pub(crate) fn queue_head(
    capacity: u32,
    write: u32,
    retired: u32,
    count: u32,
) -> Result<u32, QueueError> {
    if capacity < 2
        || capacity > u16::MAX.into()
        || write >= capacity
        || retired >= capacity
        || count == 0
        || count >= capacity
    {
        return Err(QueueError::Geometry);
    }
    let free = (retired + capacity - write - 1) % capacity;
    if count > free {
        return Err(QueueError::Full);
    }
    Ok((write + count) % capacity)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct GrowRequest {
    pub(crate) vm: u32,
    pub(crate) buffer: u32,
    pub(crate) counter: u32,
    pub(crate) subpipe: u32,
    pub(crate) halt_count: u64,
}

impl GrowRequest {
    pub(crate) fn decode([vm, buffer, counter, subpipe, halt_low, halt_high]: [u32; 6]) -> Self {
        Self {
            vm,
            buffer,
            counter,
            subpipe,
            halt_count: u64::from(halt_low) | (u64::from(halt_high) << 32),
        }
    }

    pub(crate) fn reply(self, granted: bool) -> [u32; 16] {
        const GROW_REPLY: u32 = 8;
        let mut record = [0; 16];
        record[..8].copy_from_slice(&[
            GROW_REPLY,
            u32::from(granted),
            self.buffer,
            self.vm,
            self.counter,
            self.subpipe,
            self.halt_count as u32,
            (self.halt_count >> 32) as u32,
        ]);
        record
    }
}

/// Control-object recovery words (the recovery block G16 and M3 answer in their FirmwareData):
/// the u64 recovery generation, which growth requests also carry; 0x10 on, the recovery phase
/// (firmware 1 -> host 2 -> firmware 3 -> host 0); the snapshot of the work the firmware
/// stopped, {flags u32, target u32} pairs, at the same offset as in the M3 object, up to the
/// next recovery field. Seen on the J473: a stopped tiler {0x1f, its next stamp}, a stopped
/// compute {0x7, its next stamp}.
pub(crate) const RECOVERY_GENERATION: usize = 0x4bf0;
pub(crate) const RECOVERY_PHASE: usize = 0x4c00;
pub(crate) const RECOVERY_SNAPSHOT: usize = 0x4298;
const RECOVERY_SNAPSHOT_END: usize = 0x4b20;
pub(crate) const RECOVERY_SNAPSHOT_ENTRIES: usize = (RECOVERY_SNAPSHOT_END - RECOVERY_SNAPSHOT) / 8;

/// A snapshot entry of a stopped tiler (valid + tiler bits): its TPC storage must be cleared
/// before the host answers phase 2, as G16 does.
pub(crate) const fn stopped_tiler(flags: u32) -> bool {
    flags & 9 == 9
}

/// The tracked work a recovery snapshot names. A snapshot entry's target is the completion
/// stamp the stopped command would have written (J473: compute stamp 0x7900 written, entry
/// {0x7, 0x7a00}; tiler stamp 0x2156500 written, entry {0x1f, 0x2156600}).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Stopped {
    /// The published render pair (either of its completion stamps).
    Render,
    /// The published compute command with this completion stamp.
    Compute(u32),
}

/// Attribution of one recovery round: every valid snapshot entry must name the same tracked,
/// unretired work, else the round is ambiguous and the caller fails all work (fail-stop).
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct Attribution {
    work: Option<Stopped>,
    ambiguous: bool,
}

impl Attribution {
    /// Adds a valid entry. `render` holds the published render pair's completion stamps;
    /// `compute(stamp)` says whether an unretired compute command carries that stamp.
    pub(crate) fn add(&mut self, flags: u32, target: u32, render: Option<[u32; 2]>,
                      compute: impl Fn(u32) -> bool) {
        let as_render = render.is_some_and(|stamps| stamps.contains(&target));
        // A stopped tiler can only be the render pair.
        let as_compute = !stopped_tiler(flags) && compute(target);
        let work = match (as_render, as_compute) {
            (true, false) => Stopped::Render,
            (false, true) => Stopped::Compute(target),
            _ => {
                self.ambiguous = true;
                return;
            }
        };
        match self.work {
            None => self.work = Some(work),
            Some(seen) if seen == work => {}
            Some(_) => self.ambiguous = true,
        }
    }

    /// The stopped work, if the round named exactly one tracked command.
    pub(crate) fn result(self) -> Option<Stopped> {
        if self.ambiguous { None } else { self.work }
    }
}

/// The published compute command a snapshot target names: the one, among commands the queue
/// has not retired (`ordinal > retired`), whose completion stamp is `stamp`. `flights` yields
/// each published command's (completion stamp, ordinal) in publication order. A command that
/// already retired is never blamed, even before the host has taken it off its list.
pub(crate) fn stopped_compute(
    flights: impl IntoIterator<Item = (u32, u64)>,
    retired: u64,
    stamp: u32,
) -> Option<usize> {
    flights.into_iter().position(|(s, ordinal)| s == stamp && ordinal > retired)
}

/// What the host does when a command ran past the driver's command timeout.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum TimeoutAction {
    /// The firmware has a recovery round open (phase 1): answer it, which blames the work it
    /// stopped by stamp and fails only that client.
    Recover,
    /// No round open: the firmware reported nothing and stopped nothing that the host can
    /// answer (a silent hang), or the handshake is in a state the host does not own. Failing
    /// all work is the last resort.
    FailAll,
}

/// The driver's command timeout is only a backstop for the firmware's own recovery: it fails
/// every client only when the firmware has no round open.
pub(crate) const fn command_timeout_action(phase: u32) -> TimeoutAction {
    match phase {
        1 => TimeoutAction::Recover,
        _ => TimeoutAction::FailAll,
    }
}

/// A further recovery round while stopped work drains must carry a new generation; the same
/// generation again means the host misread the handshake.
pub(crate) const fn new_round(previous: Option<u64>, generation: u64) -> bool {
    match previous {
        None => true,
        Some(p) => generation != p,
    }
}

/// Tells a fault or timeout report on the event ring that needs a recovery from one whose round
/// the host already answered.
///
/// The firmware posts the report, then opens the round (phase 1). The host can answer a round
/// before it reads its report: a further round opened while stopped work drains, or a round
/// found at a command timeout. That report is then still in the ring, and the next service pass
/// reads it after the firmware is back at phase 0. It must be consumed, not answered with a
/// wait for a phase 1 that never comes, which would fail all work.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct RecoveryLedger {
    /// Service passes that read fault or timeout records. One pass counts once, so a fault and
    /// a timeout posted for the same stop are one report.
    reports: u64,
    /// Rounds the host answered.
    answered: u64,
    /// Generation of the last round answered.
    last: Option<u64>,
}

impl RecoveryLedger {
    /// A service pass read fault or timeout records.
    pub(crate) fn report(&mut self) {
        self.reports = self.reports.saturating_add(1);
    }

    /// The host answered round `generation` (wrote phase 0).
    pub(crate) fn answered(&mut self, generation: u64) {
        self.answered = self.answered.saturating_add(1);
        self.last = Some(generation);
    }

    /// Generation of the last round answered: a round at phase 1 must carry another one
    /// (`new_round`).
    pub(crate) const fn last(&self) -> Option<u64> {
        self.last
    }

    /// With no new round open after a report: whether every report read so far has a round the
    /// host already answered. Then the report is stale and is consumed. Otherwise the firmware
    /// reported a stop without entering recovery.
    pub(crate) const fn report_answered(&self) -> bool {
        self.reports <= self.answered
    }

    /// Reports read and rounds answered so far, for a log line.
    pub(crate) const fn counts(&self) -> (u64, u64) {
        (self.reports, self.answered)
    }
}

/// Packets with stopped work one boot may keep (`Runtime::keep`). A kept packet pins its VM,
/// and with it one user UAT context, and a render one also pins its scene. The limit is half
/// the user contexts, so innocent clients keep the rest.
pub(crate) const QUARANTINE_LIMIT: usize = 31;

/// Whether one more packet with stopped work fits next to the `kept` ones. When it does not,
/// the caller fails all work, and everything published stays owned.
pub(crate) const fn quarantine_fits(kept: usize) -> bool {
    kept < QUARANTINE_LIMIT
}

/// Whether a packet's VM job guard may be released, so its mappings may change. That needs
/// none of its commands in a queue (`live`) and no stopped command of it kept (`kept`). The
/// firmware may still reach a stopped command's memory, so a kept packet stays owned for the
/// rest of the boot, whoever asks: retirement, `withdraw` or `fail_all`.
pub(crate) const fn guard_releasable(live: bool, kept: bool) -> bool {
    !live && !kept
}

/// Event ring record kinds.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum EventKind {
    /// 0: the firmware stopped on a GPU fault and entered recovery.
    Fault,
    /// 4: the firmware timed out a command (slot at `TIMEOUT_SLOT_OFFSET`) and entered recovery.
    Timeout,
    /// 6: a buffer growth request.
    Grow,
    /// 7: the firmware rejected a channel record.
    ChannelError,
    /// Anything else (stage flags, statistics): nothing for the host to answer.
    Notification,
}

/// Byte offset of the slot a timeout record names.
pub(crate) const TIMEOUT_SLOT_OFFSET: usize = 0x0c;

pub(crate) const fn event_kind(kind: u32) -> EventKind {
    match kind {
        0 => EventKind::Fault,
        4 => EventKind::Timeout,
        6 => EventKind::Grow,
        7 => EventKind::ChannelError,
        _ => EventKind::Notification,
    }
}

/// Globals words of the firmware's per-pipe progress checks and compute watchdog, which the
/// shared 0xe48-byte 27.2 Globals hold at the same offsets as the M3 and M4 27.2 runtimes write
/// them (`m3_power_26`, `g16/fw/initdata`), with the 13.5 names: 0x24 relaxed compute kill
/// timeout (ms), 0x78 command submission enabled, 0x998/0x99c/0x9a0 progress-check intervals
/// of 3D/TA/compute (firmware polls), 0x9c8 compute context-switch timeout (ms), 0x9cc compute
/// kill timeout (ms), 0x9b8/0x9bc/0x9c0 the compute back-off words (0x9bc is the G15 layout's
/// `cdm_backoff_timeout`), 0xe28. Left zero, the firmware's progress recovery (armed in HwDataB)
/// times out TA and compute work that runs past a poll within a second or two. The TA interval
/// is the 100 polls the M3 runtime uses for long vertex loops (`m3_config`), not 10.
pub(crate) const GLOBALS_PROGRESS: [(usize, u32); 11] = [
    (0x24, 3000),
    (0x78, 1),
    (0x998, 40),
    (0x99c, 100),
    (0x9a0, 250),
    (0x9b8, 2),
    (0x9bc, 40),
    (0x9c0, 5),
    (0x9c8, 40),
    (0x9cc, 50),
    (0xe28, 3),
];

/// Size of the 27.2 Globals object.
pub(crate) const GLOBALS_BYTES: usize = 0xe48;

/// Where the 27.2 compute command (type 3, 0x358 bytes) keeps the user timestamp pair: right
/// after the command time (+0x2c4) and the internal start/end pointers (+0x2cc), the order of
/// the 13.5 compute command and of the 27.2 render commands (vertex +0x5e4, fragment +0x948).
pub(crate) const COMPUTE_USER_TIMESTAMPS: usize = 0x2dc;

/// The 27.2 compute command's JobMeta flush_stamps word, after the completion stamp, firmware
/// stamp, stamp value and slot (+0x298..+0x2b4); the render commands keep it at the same place
/// of their JobMeta (vertex +0x5a0, fragment +0x904), and the M3 27.2 compute command sets it
/// when it does not coalesce. Set, the firmware flushes the command's stamps when it completes.
pub(crate) const COMPUTE_FLUSH_STAMPS: usize = 0x2b4;
/// The 13.5 compute JobParameters1 inside the 27.2 compute command.
pub(crate) const COMPUTE_JOB_PARAMETERS1: usize = 0x68;

/// The value of a 27.2 Timestamp operation's user word: like the 13.5 and the M3 27.2
/// operations it names the command's start/end pair of user destinations, not a destination.
/// Zero when the command has no user timestamps.
pub(crate) fn user_timestamp_pointer(pair_va: u64, pair: [u64; 2]) -> u64 {
    if pair == [0; 2] {
        0
    } else {
        pair_va
    }
}

fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

/// Writes a compute helper program (binary: USC offset | 1, data, configuration) into the
/// 13.5 JobParameters1 of a 27.2 compute command, at the places the 13.5 G14 compute path
/// fills: program +0x40, argument +0x48, configuration +0x50.
pub(crate) fn encode_compute_helper(cmd: &mut [u8], binary: u32, data: u64, config: u32) {
    put32(cmd, COMPUTE_JOB_PARAMETERS1 + 0x40, binary);
    put64(cmd, COMPUTE_JOB_PARAMETERS1 + 0x48, data);
    put32(cmd, COMPUTE_JOB_PARAMETERS1 + 0x50, config);
}

/// Where the 27.2 compute command keeps its sampler heap (u64), sampler count and sampler bound:
/// the 13.5 EncoderParams sampler words, 0x10 bytes further on as in the 27.2 fragment command
/// (13.5 0x8ac, 27.2 0x8bc). The firmware loads the same three words of every command type
/// into the data master's sampler state (TA 0x558/0x560/0x564, 3D 0x8bc/0x8c4/0x8c8, compute
/// here), the count and the bound as 11-bit fields.
pub(crate) const COMPUTE_SAMPLER_HEAP: usize = 0x284;
pub(crate) const COMPUTE_SAMPLER_COUNT: usize = 0x28c;
pub(crate) const COMPUTE_SAMPLER_MAX: usize = 0x290;

/// The sampler bound the 13.5 compute command stores next to the count: the count plus one,
/// or zero without samplers.
pub(crate) fn compute_sampler_max(count: u32) -> u32 {
    if count == 0 {
        0
    } else {
        count + 1
    }
}

/// Writes the sampler heap of a zeroed 27.2 compute command. A command without samplers
/// (heap and count zero) keeps the words zero. The caller has validated the heap: at most
/// 1024 samplers, both fields set or both zero.
pub(crate) fn encode_compute_samplers(cmd: &mut [u8], heap: u64, count: u32) {
    put64(cmd, COMPUTE_SAMPLER_HEAP, heap);
    put32(cmd, COMPUTE_SAMPLER_COUNT, count);
    put32(cmd, COMPUTE_SAMPLER_MAX, compute_sampler_max(count));
}

/// Writes the user timestamp pair of a zeroed 27.2 compute command at `cmd_va` and returns the
/// Timestamp operations' user word ([`user_timestamp_pointer`]).
pub(crate) fn encode_compute_timestamps(cmd: &mut [u8], cmd_va: u64, timestamps: [u64; 2]) -> u64 {
    put64(cmd, COMPUTE_USER_TIMESTAMPS, timestamps[0]);
    put64(cmd, COMPUTE_USER_TIMESTAMPS + 8, timestamps[1]);
    user_timestamp_pointer(cmd_va + COMPUTE_USER_TIMESTAMPS as u64, timestamps)
}

/// The command buffer a pool slot uses: each slot owns two, `2 * slot` and `2 * slot + 1`,
/// and alternates between them. The firmware may still read a retired command's JobMeta for
/// stamp stores it queued after it unlinked the slot, so a slot's next command must not
/// overwrite it.
pub(crate) fn pool_command(slot: usize, second: bool) -> usize {
    2 * slot + usize::from(second)
}

/// Whether slot `slot`'s next command goes to its second buffer: the one it did not last
/// publish, given one bit per slot set when the last published command used the second.
/// Encoding does not change the bits, so a command that is encoded but never published is
/// replaced in the same spare buffer.
pub(crate) fn spare_buffer(published: u64, slot: usize) -> bool {
    published & (1 << slot) == 0
}

#[cfg_attr(not(test), kunit_tests(asahi_g14_fw_queues))]
#[cfg_attr(test, allow(dead_code))]
mod tests {
    use super::*;

    #[test]
    fn a_slot_never_rewrites_the_command_it_published_last() {
        const SLOTS: usize = 48;
        let mut owner = [usize::MAX; 2 * SLOTS];
        let mut published = 0u64;
        let mut last = [usize::MAX; SLOTS];
        for round in 0..6 {
            for slot in 0..SLOTS {
                let c = pool_command(slot, spare_buffer(published, slot));
                assert!(c < 2 * SLOTS);
                // Two buffers per slot, never shared with another slot.
                assert!(owner[c] == usize::MAX || owner[c] == slot);
                owner[c] = slot;
                // Never the buffer the slot last published.
                assert_ne!(c, last[slot]);
                // Every third slot's publication fails once: the retry takes the same spare.
                if (slot + round) % 3 == 0 {
                    assert_eq!(pool_command(slot, spare_buffer(published, slot)), c);
                }
                published ^= 1 << slot;
                last[slot] = c;
            }
        }
    }

    #[test]
    fn head_wraps_and_keeps_one_entry_free() {
        assert_eq!(queue_head(8, 7, 7, 2), Ok(1));
        assert_eq!(queue_head(8, 7, 2, 2), Ok(1));
        assert_eq!(queue_head(8, 7, 2, 3), Err(QueueError::Full));
        assert_eq!(queue_head(8, 7, 0, 1), Err(QueueError::Full));
    }

    #[test]
    fn corrupted_cursors_and_invalid_depth_fail_before_publication() {
        for (capacity, write, read, count) in [
            (0, 0, 0, 1),
            (8, 8, 0, 1),
            (8, 0, 8, 1),
            (8, 0, 0, 0),
            (8, 0, 0, 8),
            (1 << 16, 0, 0, 1),
        ] {
            assert_eq!(
                queue_head(capacity, write, read, count),
                Err(QueueError::Geometry)
            );
        }
    }

    #[test]
    fn growth_replies_echo_request_identity_and_clear_unused_words() {
        let request = GrowRequest::decode([2, 9, 31, 1, 0x789a_bcde, 0x1234_5678]);
        assert_eq!(request.halt_count, 0x1234_5678_789a_bcde);
        assert_eq!(
            request.reply(true),
            [
                8,
                1,
                9,
                2,
                31,
                1,
                0x789a_bcde,
                0x1234_5678,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0
            ]
        );
        assert_eq!(
            request.reply(false),
            [
                8,
                0,
                9,
                2,
                31,
                1,
                0x789a_bcde,
                0x1234_5678,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0
            ]
        );
    }

    #[test]
    fn recovery_events_are_decoded_and_others_stay_notifications() {
        assert_eq!(event_kind(0), EventKind::Fault);
        assert_eq!(event_kind(4), EventKind::Timeout);
        assert_eq!(event_kind(6), EventKind::Grow);
        assert_eq!(event_kind(7), EventKind::ChannelError);
        for kind in [1, 2, 3, 5, 8, u32::MAX] {
            assert_eq!(event_kind(kind), EventKind::Notification);
        }
        // The H1 record [4, 0, 0, 1, ...]: slot in the fourth word.
        assert_eq!(TIMEOUT_SLOT_OFFSET / 4, 3);
    }

    #[test]
    fn recovery_block_geometry_matches_the_g16_order() {
        // Phase 0x10 above the u64 generation, as in the G16/M3 FirmwareData.
        assert_eq!(RECOVERY_PHASE - RECOVERY_GENERATION, 0x10);
        assert_eq!(RECOVERY_SNAPSHOT_ENTRIES, 273);
        assert!(RECOVERY_SNAPSHOT + 8 * RECOVERY_SNAPSHOT_ENTRIES <= RECOVERY_SNAPSHOT_END);
        assert!(RECOVERY_SNAPSHOT_END <= RECOVERY_GENERATION);
    }

    #[test]
    fn only_a_valid_tiler_entry_clears_tiler_storage() {
        // J473 snapshots: stopped tiler {0x1f, ..}, stopped compute {0x7, ..}.
        assert!(stopped_tiler(0x1f));
        assert!(!stopped_tiler(0x7));
        assert!(!stopped_tiler(0x8));
        assert!(stopped_tiler(0x9 | (0x70 << 4)));
    }

    #[test]
    fn a_completed_front_entry_is_not_blamed_for_a_later_stop() {
        // Host list: 0x7900 completed but not yet retired by the host, 0x7a00 stopped.
        let unretired = [0x7900u32, 0x7a00, 0x7b00];
        let mut a = Attribution::default();
        a.add(0x7, 0x7a00, None, |s| unretired.contains(&s));
        assert_eq!(a.result(), Some(Stopped::Compute(0x7a00)));
    }

    #[test]
    fn render_entries_name_the_published_pair() {
        let pair = Some([0x2156600, 0x3156600]);
        let mut a = Attribution::default();
        a.add(0x1f, 0x2156600, pair, |_| false);
        a.add(0x7, 0x3156600, pair, |_| false);
        assert_eq!(a.result(), Some(Stopped::Render));
        // A stopped tiler that matches no published pair is not pinned on compute.
        let mut a = Attribution::default();
        a.add(0x1f, 0x7a00, None, |s| s == 0x7a00);
        assert_eq!(a.result(), None);
    }

    #[test]
    fn untracked_mixed_or_colliding_entries_fail_stop() {
        let mut a = Attribution::default();
        assert_eq!(a.result(), None);
        a.add(0x7, 0x7c00, None, |s| s == 0x7a00);
        assert_eq!(a.result(), None);
        let mut a = Attribution::default();
        a.add(0x7, 0x7a00, None, |s| s == 0x7a00 || s == 0x7b00);
        a.add(0x7, 0x7b00, None, |s| s == 0x7a00 || s == 0x7b00);
        assert_eq!(a.result(), None);
        let mut a = Attribution::default();
        a.add(0x7, 0x3156600, Some([0x2156600, 0x3156600]), |s| s == 0x3156600);
        assert_eq!(a.result(), None);
    }

    #[test]
    fn recovery_rounds_while_draining_need_a_new_generation() {
        assert!(new_round(None, 1));
        assert!(new_round(Some(1), 2));
        assert!(!new_round(Some(2), 2));
        assert!(new_round(Some(u64::MAX), 0));
    }

    #[test]
    fn a_report_read_before_its_round_is_answered_once() {
        // Report, then its round: balanced, nothing left over.
        let mut ledger = RecoveryLedger::default();
        ledger.report();
        ledger.answered(7);
        assert!(ledger.report_answered());
        assert_eq!(ledger.last(), Some(7));
        // A later report with no round open is a stop without recovery: not consumed.
        ledger.report();
        assert!(!ledger.report_answered());
    }

    #[test]
    fn a_stale_report_after_a_drain_round_is_consumed() {
        // Round 7 from its report; round 8 opened while 7's work drained, answered before its
        // report was read. Round 8's report then comes on the next pass, at phase 0.
        let mut ledger = RecoveryLedger::default();
        ledger.report();
        ledger.answered(7);
        ledger.answered(8);
        ledger.report();
        assert!(ledger.report_answered());
        // Round 8 is answered: phase 1 with generation 8 again is not a new round.
        assert!(!new_round(ledger.last(), 8));
        assert!(new_round(ledger.last(), 9));
        // Its credit is spent: a further report without a round is not stale.
        ledger.report();
        assert!(!ledger.report_answered());
    }

    #[test]
    fn a_stale_report_after_a_timeout_round_is_consumed() {
        // The command timeout found phase 1 and answered round 3 before the event was read.
        let mut ledger = RecoveryLedger::default();
        ledger.answered(3);
        ledger.report();
        assert!(ledger.report_answered());
    }

    #[test]
    fn a_timed_out_compute_is_blamed_by_stamp_and_spares_its_neighbours() {
        // Compute list in publication order: (owner, completion stamp, ordinal). Client A's
        // command completed and retired, but the host has not taken it off its list yet; B's
        // command is the one the firmware timed out (event 4); C's command was published after
        // it and waits behind it.
        let flights = [('A', 0x7900u32, 1u64), ('B', 0x7a00, 2), ('C', 0x7b00, 3)];
        let retired = 1;
        let unretired = |stamp| stopped_compute(flights.iter().map(|f| (f.1, f.2)), retired, stamp)
            .is_some();
        // The J473 compute snapshot: one entry {0x7, next stamp of the stopped command}.
        let mut a = Attribution::default();
        a.add(0x7, 0x7a00, None, unretired);
        assert_eq!(a.result(), Some(Stopped::Compute(0x7a00)));
        let stopped = stopped_compute(flights.iter().map(|f| (f.1, f.2)), retired, 0x7a00);
        assert_eq!(stopped, Some(1));
        // blame() dooms every listed command of the stopped command's packet, and only those.
        let guilty = flights[stopped.unwrap()].0;
        let doomed: [bool; 3] = core::array::from_fn(|i| flights[i].0 == guilty);
        assert_eq!(doomed, [false, true, false]);
        // A target naming the retired front is not blamed on it.
        assert_eq!(stopped_compute(flights.iter().map(|f| (f.1, f.2)), retired, 0x7900), None);
        let mut a = Attribution::default();
        a.add(0x7, 0x7900, None, unretired);
        assert_eq!(a.result(), None);
    }

    #[test]
    fn the_command_timeout_fails_all_only_without_a_recovery_round() {
        assert_eq!(command_timeout_action(1), TimeoutAction::Recover);
        // Silent hang: no round open.
        assert_eq!(command_timeout_action(0), TimeoutAction::FailAll);
        // The host's own phases or anything else: the handshake is not the firmware's to
        // continue, so the host does not answer it from a timeout.
        for phase in [2, 3, 4, u32::MAX] {
            assert_eq!(command_timeout_action(phase), TimeoutAction::FailAll);
        }
    }

    #[test]
    fn a_withdrawn_stopped_packet_keeps_its_guard() {
        // The scheduler withdraws a packet whose stopped command left the queue: it is no
        // longer live, but it is kept, so its guard stays.
        assert!(!guard_releasable(false, true));
        // Still in a queue, kept or not.
        assert!(!guard_releasable(true, false));
        assert!(!guard_releasable(true, true));
        // Retired innocent work.
        assert!(guard_releasable(false, false));
    }

    #[test]
    fn kept_packets_are_bounded_per_boot() {
        assert!(quarantine_fits(0));
        assert!(quarantine_fits(QUARANTINE_LIMIT - 1));
        assert!(!quarantine_fits(QUARANTINE_LIMIT));
        assert!(!quarantine_fits(usize::MAX));
        // Innocent clients keep at least half of the 63 user UAT contexts.
        assert!(2 * QUARANTINE_LIMIT <= 63);
    }

    #[test]
    fn progress_words_fit_the_globals_and_leave_power_words_alone() {
        let power = [0x58usize, 0x5a, 0x5c, 0x6e, 0x84, 0x88, 0x8c, 0x98];
        for (i, &(at, value)) in GLOBALS_PROGRESS.iter().enumerate() {
            assert_eq!(at % 4, 0);
            assert!(at + 4 <= GLOBALS_BYTES);
            assert_ne!(value, 0);
            assert!(power.iter().all(|&p| p + 4 <= at || at + 4 <= p));
            assert!(GLOBALS_PROGRESS[i + 1..].iter().all(|&(other, _)| other != at));
        }
        // Every progress interval is set: a zero interval is what stopped work in a poll.
        for at in [0x998, 0x99c, 0x9a0] {
            assert!(GLOBALS_PROGRESS.iter().any(|&(o, v)| o == at && v != 0));
        }
    }

    /// Size of the 27.2 compute command.
    const COMPUTE_COMMAND_BYTES: usize = 0x358;

    /// Byte ranges `encode_compute` (gpu/v27_2.rs) writes in the 27.2 compute command, and
    /// the words its stream hands the firmware to write (+0x264, +0x2c4, +0x304, +0x314,
    /// +0x340).
    const COMPUTE_WRITES: &[(usize, usize)] = &[
        (0, 4), (4, 8), (0xc, 4), (0x10, 8),
        (0x68, 8), (0x70, 8), (0x78, 8), (0x80, 8), (0x88, 8), (0x90, 8), (0x98, 8), (0xa0, 8),
        (0xc0, 8),
        (0x1e8, 8), (0x1f0, 4), (0x220, 8), (0x228, 8), (0x264, 8), (0x270, 4),
        (0x298, 0x20), (0x2c4, 0x18), (0x304, 8), (0x314, 8), (0x334, 0x14),
    ];

    /// Whether `len` bytes at `at` fit the command and are none the compute encoder uses.
    fn free(at: usize, len: usize) -> bool {
        at + len <= COMPUTE_COMMAND_BYTES
            && COMPUTE_WRITES.iter().all(|&(o, l)| o + l <= at || at + len <= o)
    }

    #[test]
    fn the_compute_helper_lands_in_free_job_parameters() {
        let mut cmd = [0u8; COMPUTE_COMMAND_BYTES];
        encode_compute_helper(&mut cmd, 0x1235, 0x11_2233_4400, 0x7_0000);
        let read64 = |at: usize| u64::from_le_bytes(cmd[at..at + 8].try_into().unwrap());
        let read32 = |at: usize| u32::from_le_bytes(cmd[at..at + 4].try_into().unwrap());
        assert_eq!((read32(0xa8), read64(0xb0), read32(0xb8)), (0x1235, 0x11_2233_4400, 0x7_0000));
        for (at, len) in [(0xa8, 4), (0xb0, 8), (0xb8, 4)] {
            assert!(free(at, len));
        }
    }

    #[test]
    fn the_compute_samplers_sit_between_the_encoder_mask_and_the_job_meta() {
        let mut cmd = [0u8; COMPUTE_COMMAND_BYTES];
        encode_compute_samplers(&mut cmd, 0x2d_fff4_4000, 1024);
        let read64 = |at: usize| u64::from_le_bytes(cmd[at..at + 8].try_into().unwrap());
        let read32 = |at: usize| u32::from_le_bytes(cmd[at..at + 4].try_into().unwrap());
        assert_eq!(
            (read64(0x284), read32(0x28c), read32(0x290)),
            (0x2d_fff4_4000, 1024, 1025)
        );
        // After the encoder mask (+0x270) and before the JobMeta stamps (+0x298), in no other
        // word the compute encoder writes.
        for (at, len) in [(0x284, 8), (0x28c, 4), (0x290, 4)] {
            assert!(free(at, len));
            assert!(0x270 + 4 <= at && at + len <= 0x298);
        }
        // The firmware takes the count and the bound as 11-bit fields.
        assert!(compute_sampler_max(1024) < 1 << 11);

        let mut none = [0u8; COMPUTE_COMMAND_BYTES];
        encode_compute_samplers(&mut none, 0, 0);
        assert!(none.iter().all(|&b| b == 0));
    }

    #[test]
    fn flush_stamps_follows_the_compute_job_meta_and_misses_the_user_pair() {
        // The JobMeta words the encoder writes end right before it: slot at +0x2b0.
        assert_eq!(COMPUTE_FLUSH_STAMPS, 0x2b0 + 4);
        assert!(COMPUTE_FLUSH_STAMPS + 4 <= COMPUTE_USER_TIMESTAMPS);
        assert_eq!(COMPUTE_FLUSH_STAMPS % 4, 0);
    }

    #[test]
    fn a_timestamp_operation_names_the_pair_only_when_there_is_one() {
        assert_eq!(user_timestamp_pointer(0x1000, [0, 0]), 0);
        assert_eq!(user_timestamp_pointer(0x1000, [0x40, 0]), 0x1000);
        assert_eq!(user_timestamp_pointer(0x1000, [0, 0x48]), 0x1000);
    }

    #[test]
    fn the_compute_user_pair_lands_in_a_free_place() {
        let mut cmd = [0u8; COMPUTE_COMMAND_BYTES];
        let pair = [0xffff_ffa1_8140_0010, 0xffff_ffa1_8140_0018];
        assert_eq!(encode_compute_timestamps(&mut cmd, 0x8000, pair), 0x8000 + 0x2dc);
        let read64 = |at: usize| u64::from_le_bytes(cmd[at..at + 8].try_into().unwrap());
        assert_eq!([read64(0x2dc), read64(0x2e4)], pair);
        assert!(free(0x2dc, 16));
        // No timestamps leave the command as it was.
        let mut empty = [0u8; COMPUTE_COMMAND_BYTES];
        assert_eq!(encode_compute_timestamps(&mut empty, 0x8000, [0; 2]), 0);
        assert!(empty.iter().all(|&b| b == 0));
    }
}
