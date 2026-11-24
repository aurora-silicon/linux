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

/// Globals words of the firmware's per-pipe progress checks and compute watchdog, which the
/// shared 0xe48-byte 27.2 Globals hold at the same offsets as the M3 and M4 27.2 runtimes write
/// them (`m3_power_26`, `g16/fw/initdata`), with the 13.5 names: 0x24 relaxed compute kill
/// timeout (ms), 0x78 command submission enabled, 0x998/0x99c/0x9a0 progress-check intervals
/// of 3D/TA/compute (firmware polls), 0x9c8 compute context-switch timeout (ms), 0x9cc compute
/// kill timeout (ms), 0xe28. Left zero, the firmware's progress recovery (armed in HwDataB)
/// times out TA and compute work that runs past a poll within a second or two. The TA interval
/// is the 100 polls the M3 runtime uses for long vertex loops (`m3_config`), not 10.
pub(crate) const GLOBALS_PROGRESS: [(usize, u32); 8] = [
    (0x24, 3000),
    (0x78, 1),
    (0x998, 40),
    (0x99c, 100),
    (0x9a0, 250),
    (0x9c8, 40),
    (0x9cc, 50),
    (0xe28, 3),
];

/// Size of the 27.2 Globals object.
pub(crate) const GLOBALS_BYTES: usize = 0xe48;

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
}
