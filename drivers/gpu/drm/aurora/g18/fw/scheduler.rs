// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Command-specific G18 / 26A428 scheduler record layout.
//!
//! These are the 0x100-stride records referenced by compute command +0x10,
//! distinct from the queue's 0x200-stride scheduler entries. Native evidence:
//! AGXCommandBuffer::init 0x8be76b0..0x8be7a54 and
//! nextCommandBufferState 0x8be65bc..0x8be6604 / 0x8be668c..0x8be66f0.

#![allow(dead_code)]

pub(crate) const G18_SCHEDULER_RECORD_COUNT: usize = 36;
pub(crate) const G18_SCHEDULER_RECORD_STRIDE: usize = 0x100;
pub(crate) const G18_SCHEDULER_RECORD_EXTENT: usize = 0xc8;
pub(crate) const G18_SCHEDULER_RECORD_BYTES: usize =
    G18_SCHEDULER_RECORD_COUNT * G18_SCHEDULER_RECORD_STRIDE;
pub(crate) const G18_SCHEDULER_COUNTER_BYTES: usize = G18_SCHEDULER_RECORD_COUNT * 4;
/// Native G18 channel initialization 0x8c45470/0x8c45488 sets device +0x6f8.
pub(crate) const G18_SCHEDULER_DEFAULT_THRESHOLD: u32 = 80;

/// Initialize the full native-sized backing, before publishing any pointer.
/// The counter backing is a separately owned, zeroed allocation.
pub(crate) fn initialize_g18_scheduler_records(
    out: &mut [u8; G18_SCHEDULER_RECORD_BYTES],
    counter_base: u64,
) -> Option<()> {
    if counter_base == 0 || counter_base & 3 != 0 {
        return None;
    }
    counter_base.checked_add(G18_SCHEDULER_COUNTER_BYTES as u64)?;
    out.fill(0);
    for index in 0..G18_SCHEDULER_RECORD_COUNT {
        let start = index * G18_SCHEDULER_RECORD_STRIDE;
        out[start..start + 8].copy_from_slice(&(counter_base + index as u64 * 4).to_le_bytes());
    }
    Some(())
}

/// Reset an unpublished record exactly as the native selected-slot reset.
/// Only the first 0xc8 bytes are reset; the native 0x100 stride tail survives.
/// The caller separately zeros the submitted counter. Never call this while
/// firmware can still own the record or its completion/list fields.
pub(crate) fn prepare_g18_scheduler_record(
    out: &mut [u8; G18_SCHEDULER_RECORD_STRIDE],
    submitted_counter: u64,
    sequence: u32,
    fallback_threshold: u32,
    override_threshold: u32,
    cap: u32,
) -> Option<()> {
    if submitted_counter == 0 || submitted_counter & 3 != 0 {
        return None;
    }
    submitted_counter.checked_add(4)?;
    let threshold = if override_threshold == 0 {
        fallback_threshold
    } else {
        override_threshold
    };
    let threshold = if cap == 0 {
        threshold
    } else {
        threshold.min(cap)
    };
    out[..G18_SCHEDULER_RECORD_EXTENT].fill(0);
    out[..8].copy_from_slice(&submitted_counter.to_le_bytes());
    out[8..12].copy_from_slice(&sequence.to_le_bytes());
    out[0x10..0x14].copy_from_slice(&threshold.to_le_bytes());
    Some(())
}
