// SPDX-License-Identifier: GPL-2.0-only OR MIT

fn put_u32_le(raw: &mut [u8], offset: usize, value: u32) {
    raw[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64_le(raw: &mut [u8], offset: usize, value: u64) {
    raw[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

/// M5 G17G / macOS 25G76 resets six pairs of 128-bit vectors, covering 0xc0
/// bytes. Its firmware consumes through +0xa7. The common 0x80-byte reset
/// would omit the moved fields and the firmware-visible tail.
pub(crate) const M5_25G76_CHANNEL_STATE_RESET_SIZE: usize = 0xc0;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ChannelStateResetValues {
    pub(crate) uncached_state: u64,
    pub(crate) command_pointer_ring: u64,
    pub(crate) gpu_va_10: u64,
    pub(crate) word_48: u32,
    /// Opaque host field +0xf8 copied to the unaligned firmware field +0x9c.
    pub(crate) gpu_va_9c: u64,
}

/// Exact reset stores from M5 AGXChannel::resetChannelState at
/// 0xfffffe000897d36c..0xfffffe000897d410 (AGXG17G 353.14, 25G76).
/// Uncached stores are separately shared with apply_g17_uncached_channel_reset.
/// This codec alone does not admit M5 submission or a Linux GPU runtime.
pub(crate) fn encode_m5_25g76_channel_state_reset(
    values: M5ChannelStateResetValues,
) -> [u8; M5_25G76_CHANNEL_STATE_RESET_SIZE] {
    let mut raw = [0u8; M5_25G76_CHANNEL_STATE_RESET_SIZE];
    put_u64_le(&mut raw, 0x00, values.uncached_state);
    put_u64_le(&mut raw, 0x08, values.command_pointer_ring);
    put_u64_le(&mut raw, 0x10, values.gpu_va_10);
    put_u32_le(&mut raw, 0x24, u32::MAX);
    put_u32_le(&mut raw, 0x28, 4);
    put_u32_le(&mut raw, 0x44, u32::MAX);
    put_u32_le(&mut raw, 0x48, values.word_48);
    put_u64_le(&mut raw, 0x9c, values.gpu_va_9c);
    raw
}

/// Logical render prefix, resolved at ioctl admission before asynchronous work.
#[derive(Debug, Copy, Clone, PartialEq, Eq, Default)]
pub(crate) struct G17PRenderOrder {
    pub(crate) sequence: u64,
    pub(crate) wait_through: Option<u64>,
}

