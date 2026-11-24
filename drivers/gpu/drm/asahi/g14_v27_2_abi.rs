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

/// Globals words of the three performance-state selectors, in hundredths of a state. All three
/// take the ceiling of the board's performance-state table, as G14 13.5 (`initdata`), M3 27.2
/// (`m3_power_26`) and M4 27.2 (`g16/fw/initdata`) write them; a lower value here caps the GPU
/// clock below what the firmware's power and thermal controller would allow.
pub(crate) const GLOBALS_PSTATE_SELECTORS: [usize; 3] = [0x88, 0x8c, 0x98];

/// Where the 27.2 compute command (type 3, 0x358 bytes) keeps the user timestamp pair: right
/// after the command time (+0x2c4) and the internal start/end pointers (+0x2cc), the order of
/// the 13.5 compute command and of the 27.2 render commands (vertex +0x5e4, fragment +0x948).
pub(crate) const COMPUTE_USER_TIMESTAMPS: usize = 0x2dc;
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

/// Writes the user timestamp pair of a zeroed 27.2 compute command at `cmd_va` and returns the
/// Timestamp operations' user word ([`user_timestamp_pointer`]).
pub(crate) fn encode_compute_timestamps(cmd: &mut [u8], cmd_va: u64, timestamps: [u64; 2]) -> u64 {
    put64(cmd, COMPUTE_USER_TIMESTAMPS, timestamps[0]);
    put64(cmd, COMPUTE_USER_TIMESTAMPS + 8, timestamps[1]);
    user_timestamp_pointer(cmd_va + COMPUTE_USER_TIMESTAMPS as u64, timestamps)
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
    fn progress_words_fit_the_globals_and_leave_power_words_alone() {
        let power = [0x58usize, 0x5a, 0x5c, 0x6e, 0x84];
        for (i, &(at, value)) in GLOBALS_PROGRESS.iter().enumerate() {
            assert_eq!(at % 4, 0);
            assert!(at + 4 <= GLOBALS_BYTES);
            assert_ne!(value, 0);
            assert!(power.iter().chain(&GLOBALS_PSTATE_SELECTORS).all(|&p| p + 4 <= at || at + 4 <= p));
            assert!(GLOBALS_PROGRESS[i + 1..].iter().all(|&(other, _)| other != at));
        }
        // Every progress interval is set: a zero interval is what stopped work in a poll.
        for at in [0x998, 0x99c, 0x9a0] {
            assert!(GLOBALS_PROGRESS.iter().any(|&(o, v)| o == at && v != 0));
        }
    }

    #[test]
    fn pstate_selectors_are_aligned_distinct_words_of_the_globals() {
        for (i, &at) in GLOBALS_PSTATE_SELECTORS.iter().enumerate() {
            assert_eq!(at % 4, 0);
            assert!(at + 4 <= GLOBALS_BYTES);
            assert!(GLOBALS_PSTATE_SELECTORS[i + 1..].iter().all(|&o| o + 4 <= at || at + 4 <= o));
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
        (0x298, 0x1c), (0x2c4, 0x18), (0x304, 8), (0x314, 8), (0x334, 0x14),
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
