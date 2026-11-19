// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Queue-info rows, queue state, scheduling contexts, notifiers and the compute UMA manager.
//!
//! M4 normalized 25G83 firmware ABI. Queue-info scheduling row 2 (class 2, used for render and compute): +0x30 = 2, +0x34 = 2,
//! +0x38 = 0xffff000000000000, +0x40 = 0, +0x44 = 0, +0x48 = 2. Notifiers are initialized
//! once before first exposure and never written by the host again; the threshold is a
//! separate host-written uncached cell.

/// Queue info (0x24c0 bytes, firmware-only, ASC-cached).
pub(crate) mod qinfo {
    pub(crate) const SIZE: usize = 0x24c0;
    /// u64 VAs of the queue state, the work ring, the job list and the queue scratch.
    pub(crate) const STATE: usize = 0x00;
    pub(crate) const RING: usize = 0x08;
    pub(crate) const JOB_LIST: usize = 0x10;
    pub(crate) const SCRATCH: usize = 0x18;
    pub(crate) const UNK_2C: usize = 0x2c;
    pub(crate) const UNK_4C: usize = 0x4c;
    pub(crate) const UUID: usize = 0x50;
    /// u64 scheduling context VA, then u64 4.
    pub(crate) const SCHED_CTX: usize = 0xa4;
    pub(crate) const UNK_AC: usize = 0xac;
}

/// Queue-info scheduling rows (the firmware's recognized policy rows).
pub(crate) mod qinfo_row2 {
    pub(crate) const CLASS: usize = 0x30;
    pub(crate) const CLASS_B: usize = 0x34;
    pub(crate) const MASK: usize = 0x38;
    pub(crate) const MASK_VALUE: u64 = 0xffff000000000000;
    pub(crate) const UNK_40: usize = 0x40;
    pub(crate) const UNK_44: usize = 0x44;
    pub(crate) const UNK_48: usize = 0x48;
    /// Row 0 (class 0): +0x38 mask, +0x40 = 1, +0x48 = 1.
    pub(crate) const ROW0_MASK_VALUE: u64 = 0xffffffffffff0000;
}

/// Queue state (0x60 bytes, firmware-only, never cached).
pub(crate) mod qstate {
    pub(crate) const SIZE: usize = 0x60;
    /// Firmware DONE cursor.
    pub(crate) const DONE: usize = 0x00;
    /// Firmware READ cursor.
    pub(crate) const READ: usize = 0x30;
    /// Host WRITE cursor.
    pub(crate) const WRITE: usize = 0x40;
    /// Ring capacity in entries.
    pub(crate) const CAP: usize = 0x50;
}

/// Scheduling context (0x40 bytes): 0xff at +0..+1, 1 at +5, 0xff at +0x1e, 2 at +0x23.
pub(crate) mod sched_ctx {
    pub(crate) const SIZE: usize = 0x38;
}

/// Notifier (0x118 bytes): u64 VA of a u32 threshold at +0, 0x50 at +0x10,
/// 8 x 0xff at +0xa8; the
/// completing works store their context at +0x24 (firmware-owned once exposed).
pub(crate) mod notifier {
    pub(crate) const SIZE: usize = 0x118;
    pub(crate) const THRESHOLD: usize = 0x00;
    pub(crate) const UNK_10: usize = 0x10;
    pub(crate) const UNK_10_VALUE: u32 = 0x50;
    /// The 8-byte mask the microsequences name (notifier + 0xa8).
    pub(crate) const MASK: usize = 0xa8;
    pub(crate) const CONTEXT: usize = 0x24;
}

/// Engine records and terminal service-stamp cells in their retained notifier page.
pub(crate) mod notifier_page {
    pub(crate) const TA_RECORD: usize = 0;
    pub(crate) const FS_RECORD: usize = 0x200;
    pub(crate) const COMPUTE_RECORD: usize = 0x400;
    pub(crate) const COMPUTE_CONTEXT: usize = COMPUTE_RECORD + super::notifier::CONTEXT;
    pub(crate) const COMPUTE_STAMP: usize = 0x648;
    pub(crate) const TA_STAMP: usize = 0x620;
    pub(crate) const FS_STAMP: usize = 0x630;
}

/// Job list (0x18 bytes): u64 self VA at +8.
pub(crate) mod job_list {
    pub(crate) const SIZE: usize = 0x18;
    pub(crate) const SELF: usize = 0x08;
}

/// Inline M4 UMA descriptor (work +0xe00 for compute; a retained page for render).
pub(crate) mod manager {
    pub(crate) const SIZE: usize = 0xa0;
    pub(crate) const WORK: usize = 0;
    pub(crate) const INDEX: usize = 8;
    pub(crate) const ONE: usize = 0x10;
    pub(crate) const FREELIST: usize = 0x14;
    pub(crate) const UNK_1C: usize = 0x1c;
    pub(crate) const UNK_1C_VALUE: u32 = 16384;
    pub(crate) const SEEDS_A: usize = 0x24;
    pub(crate) const SEEDS_B: usize = 0x2c;
    pub(crate) const POOL_TABLE: usize = 0x30;
    pub(crate) const COUNTER: usize = 0x4c;
    pub(crate) const COMPLETED: usize = 0x54;
    /// ASC compute-exclusion bookkeeping. AP initializes once; never a command serial.
    pub(crate) const COMPUTE_EXCLUSION: usize = 0x5c;
}

/// A private 32 MiB UMA pool: 8192 4 KiB freelist entries and sixteen 2 MiB rows.
pub(crate) mod pools {
    pub(crate) const TABLE_STRIDE: usize = 0x40;
    pub(crate) const TABLE_VALID: u64 = 1 << 61;
    pub(crate) const SEED_BYTES: u64 = 0x1000;
    pub(crate) const ROW_BYTES: u64 = 0x200000;
    pub(crate) const ROWS: usize = 16;
    pub(crate) const POOL_BYTES: usize = 0x2000000;
    pub(crate) const STATE_BYTES: usize = 0x20000;
    pub(crate) const TABLE_BYTES: usize = 0x4000;
    pub(crate) const SEEDS: usize = 8192;
}

/// The nonzero queue header; the rest of the fresh 0x24c0 object remains zero. Keeping the
/// scratch inside this object is part of the M4 ABI, not a separate buffer allocation.
pub(crate) fn queue_image(
    state: u64,
    ring: u64,
    list: u64,
    queue: u64,
    context: u64,
) -> [u8; 0xb4] {
    let mut q = [0; 0xb4];
    for (at, value) in [
        (0, state),
        (8, ring),
        (0x10, list),
        (0x18, queue + 0xb0),
        (0x38, qinfo_row2::MASK_VALUE),
        (0xa4, context),
    ] {
        q[at..at + 8].copy_from_slice(&value.to_le_bytes());
    }
    for (at, value) in [
        (0x2c, u32::MAX),
        (0x30, 2u32),
        (0x34, 2),
        (0x48, 2),
        (0x4c, u32::MAX),
    ] {
        q[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    q
}

/// The scheduler context is initialized once, before its first exposure.
pub(crate) fn context_image() -> [u8; sched_ctx::SIZE] {
    let mut s = [0; sched_ctx::SIZE];
    s[0] = 0xff;
    s[1] = 0xff;
    s[5] = 1;
    s[0x26] = 2;
    s[0x33] = 0xff;
    s
}

/// One engine notifier record. The page holding the three records stays non-cacheable;
/// threshold and service stamp cells occupy disjoint offsets starting at +0x600.
pub(crate) fn notifier_image(threshold: u64, context: u32) -> [u8; notifier::SIZE] {
    let mut n = [0; notifier::SIZE];
    n[..8].copy_from_slice(&threshold.to_le_bytes());
    n[0x10..0x14].copy_from_slice(&0x50u32.to_le_bytes());
    n[notifier::CONTEXT..notifier::CONTEXT + 4].copy_from_slice(&context.to_le_bytes());
    n[0xa8..0xb0].fill(0xff);
    n[0x114..0x118].copy_from_slice(&1u32.to_le_bytes());
    n
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn m4_context() {
        let s = context_image();
        assert_eq!(s.len(), 0x38);
        assert_eq!((s[0], s[1], s[5], s[0x26], s[0x33]), (255, 255, 1, 2, 255));
        assert_eq!(s.iter().filter(|&&x| x != 0).count(), 5);
    }

    #[test]
    fn m4_notifier() {
        let n = notifier_image(0xfffffc2000000610, 1);
        assert_eq!(n.len(), 0x118);
        assert_eq!(&n[..8], &0xfffffc2000000610u64.to_le_bytes());
        assert_eq!(&n[0xa8..0xb0], &[255; 8]);
        assert_eq!(n[0x114], 1);
        assert_eq!(qinfo::SIZE, 0x24c0);
    }
}
