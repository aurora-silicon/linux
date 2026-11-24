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

/// A further recovery round while stopped work drains must carry a new generation; the same
/// generation again means the host misread the handshake.
pub(crate) const fn new_round(previous: Option<u64>, generation: u64) -> bool {
    match previous {
        None => true,
        Some(p) => generation != p,
    }
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

#[cfg_attr(not(test), kunit_tests(asahi_g14_fw_queues))]
#[cfg_attr(test, allow(dead_code))]
mod tests {
    use super::*;

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
}
