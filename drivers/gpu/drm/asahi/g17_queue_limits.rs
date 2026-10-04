// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Allocation contract for independent G17P queues.
//! Every accepted DRM queue owns its scarce execution resources at creation.
pub(crate) const FEATURE_INDEPENDENT_QUEUES: u64 = 1 << 2;
pub(crate) const PARAM_GROUP_QUEUE_LIMITS: u32 = 2;
pub(crate) const QUEUE_LIMITS_SIZE: usize = 16;
pub(crate) const TTBAT_CONTEXT_COUNT: u32 = 64;
pub(crate) const COMPUTE_CONTEXT_FIRST: u32 = 5;
/// The current exclusive renderer needs one duplicate app-context root.
pub(crate) const RENDER_APP_CONTEXT: u32 = TTBAT_CONTEXT_COUNT - 1;
pub(crate) const COMPUTE_CONTEXT_MASK: u64 =
    ((1u64 << RENDER_APP_CONTEXT) - 1) & !((1u64 << COMPUTE_CONTEXT_FIRST) - 1);
pub(crate) const MAX_QUEUES: u32 = COMPUTE_CONTEXT_MASK.count_ones();
pub(crate) const MAX_IN_FLIGHT_PER_QUEUE: u32 = 1;
/// One GiB boundary inside the existing four GiB runtime reservation. The
/// canonical portion has room for all58 queues'256-descriptor backing arrays
/// and auxiliary mappings; the remaining3GiB hold per-VM private aliases.
pub(crate) const CLIENT_LOW_VA_START: u64 = 0x70_4000_0000;

pub(crate) fn encode_limits() -> [u8; QUEUE_LIMITS_SIZE] {
    let mut bytes = [0; QUEUE_LIMITS_SIZE];
    bytes[0..4].copy_from_slice(&MAX_QUEUES.to_ne_bytes());
    bytes[4..8].copy_from_slice(&MAX_IN_FLIGHT_PER_QUEUE.to_ne_bytes());
    bytes
}
