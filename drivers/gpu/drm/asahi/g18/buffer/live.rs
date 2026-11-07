// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded volatile stores to host-owned shared firmware words.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.
use kernel::prelude::*;

// Live shared control words are read asynchronously by firmware, including while
// older kicks retire. Byte copies are not a single-copy publication on strict-
// alignment builds. The Runtime lock serializes host RMWs; volatile accesses
// preserve native aligned LDR/STR width, not an atomic RMW across processors.
#[inline]
fn g17p_live_word_bounds(bytes: &[u8], offset: usize, width: usize) -> Result {
    if !matches!(width, 4 | 8)
        || offset.checked_add(width).is_none_or(|end| end > bytes.len())
        || (bytes.as_ptr() as usize).checked_add(offset)
            .is_none_or(|address| address & (width - 1) != 0)
    {
        return Err(EINVAL);
    }
    Ok(())
}

#[inline]
pub(crate) fn g17p_live_write_u32(bytes: &mut [u8], offset: usize, value: u32) -> Result {
    g17p_live_word_bounds(bytes, offset, 4)?;
    // SAFETY: checked bounds and natural alignment; host owns this shared word.
    unsafe { core::ptr::write_volatile(bytes.as_mut_ptr().add(offset).cast::<u32>(), value.to_le()) };
    Ok(())
}
