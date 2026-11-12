// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! v91 little-endian field stores for descriptor and record encoders.
//!
//! The encoders' `copy_from_slice` field writes compile to byte stores under
//! this target's strict alignment (one STRB per byte, measured in the v90
//! module's `encode_registers`). Most fields land in write-combined
//! firmware-visible memory, where every store is a separate transaction for
//! the combining buffers. With `g17p_host_path` bit11 (WORD_STORES) a field
//! whose address is naturally aligned for a wider access is written with the
//! widest aligned stores that exactly cover it; the bytes written are
//! identical. Unaligned fields keep byte stores. Aligned word stores are
//! single-copy atomic, which byte stores are not, so no reader can observe a
//! state the byte sequence could not also have produced.
#[cfg(not(test))]
#[inline(always)]
fn word_stores() -> bool {
    crate::g18::compat::host_path::enabled(crate::g18::compat::host_path::WORD_STORES)
}
#[cfg(test)]
pub(crate) static TEST_WORD_STORES: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(true);
#[cfg(test)]
fn word_stores() -> bool { TEST_WORD_STORES.load(core::sync::atomic::Ordering::Relaxed) }

#[inline(always)]
fn store_u16(dst: &mut [u8], value: u16) {
    let at = dst.as_mut_ptr();
    if at as usize & 1 == 0 {
        // SAFETY: `dst` is exactly two in-bounds bytes and `at` is 2-aligned.
        unsafe { at.cast::<u16>().write(value.to_le()) }
    } else {
        dst.copy_from_slice(&value.to_le_bytes());
    }
}

#[inline(always)]
fn store_u32(dst: &mut [u8], value: u32) {
    let at = dst.as_mut_ptr();
    if at as usize & 3 == 0 {
        // SAFETY: `dst` is exactly four in-bounds bytes and `at` is 4-aligned.
        unsafe { at.cast::<u32>().write(value.to_le()) }
    } else if at as usize & 1 == 0 {
        let (low, high) = dst.split_at_mut(2);
        store_u16(low, value as u16);
        store_u16(high, (value >> 16) as u16);
    } else {
        dst.copy_from_slice(&value.to_le_bytes());
    }
}

/// Store a little-endian u16 at `raw[offset..offset + 2]` (panics out of
/// bounds, like the slice copy it replaces).
#[inline(always)]
pub(crate) fn put_u16(raw: &mut [u8], offset: usize, value: u16) {
    let dst = &mut raw[offset..offset + 2];
    if word_stores() { store_u16(dst, value) } else { dst.copy_from_slice(&value.to_le_bytes()) }
}

/// Store a little-endian u32 at `raw[offset..offset + 4]`.
#[inline(always)]
pub(crate) fn put_u32(raw: &mut [u8], offset: usize, value: u32) {
    let dst = &mut raw[offset..offset + 4];
    if word_stores() { store_u32(dst, value) } else { dst.copy_from_slice(&value.to_le_bytes()) }
}

/// Store a little-endian u64 at `raw[offset..offset + 8]`.
#[inline(always)]
pub(crate) fn put_u64(raw: &mut [u8], offset: usize, value: u64) {
    let dst = &mut raw[offset..offset + 8];
    if !word_stores() {
        dst.copy_from_slice(&value.to_le_bytes());
        return;
    }
    let at = dst.as_mut_ptr();
    if at as usize & 7 == 0 {
        // SAFETY: `dst` is exactly eight in-bounds bytes and `at` is 8-aligned.
        unsafe { at.cast::<u64>().write(value.to_le()) }
    } else {
        let (low, high) = dst.split_at_mut(4);
        store_u32(low, value as u32);
        store_u32(high, (value >> 32) as u32);
    }
}
