// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! `g17p_host_path` (root-only sysfs, runtime-writable bitmask).
//!
//! The only bit the driver consumes is bit11 WORD_STORES (v91): descriptor and
//! record encoders store naturally aligned 16/32/64-bit fields with one store
//! instead of byte stores (identical bytes; fewer write-combined stores). See
//! `g18::fw::store`. The other bits (0..10, 12..19) are still accepted so
//! existing `g17p_host_path=` values keep loading, but nothing reads them: the
//! module-1134 host-path paths they gated were never wired into this driver.
//! Reading the parameter prints the mask followed by the module-1134 status
//! line, whose counters stay at their initial values for the same reason.
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, Ordering};
use kernel::bindings;

pub(crate) const WORD_STORES: u32 = 1 << 11;
const ALL_BITS: u32 = (1 << 20) - 1;

/// Production default, unchanged: module-1134 bits 1-11 and 16-19
/// (FEED_PERF, EARLY_SHALLOW, STATE_WORD, SETTLE_ONCE, RING_CACHE, WORKER_TAIL,
/// OBS_SKIP, VM_STATUS_MIRROR, VALIDATE_PROOF, PENDING_EXACT, WORD_STORES,
/// WAIT_UTIL_HINT, PRODUCER_READS, PB_OWNER_REF, SHARED_FLIST).
const DEFAULT_MASK: u32 = 0xf_0ffe;
static MASK: AtomicU32 = AtomicU32::new(DEFAULT_MASK);

/// The module-1134 status fields printed after the mask. Their sources (the
/// feed-perf detector and actuator, early publication and FW_DEPS counters,
/// `g17p_feed_perf_pct{,_p,_e}` and `g17p_early_depth`) have no caller in this
/// driver, so they always hold these initial values.
const STATUS_TAIL: &str = "pct=50 applied=0 streak=0 idle=0 busy=0 starved=0 fed=0 boosts=0 \
boosted_ns=0 active=false policies=0 depth=2 early=0 settle_skip=0 ring_hits=0 tail=0 \
obs_skip=0 mirrors=0 proof_hits=0 proof_batches=0 proof_fallbacks=0 pct_p=0 pct_e=0 \
big_cpus=0x0 applied_khz=0/0 nudges=0 nudge_fails=0 fwdeps=0/0/0 early_deps=0 agg_first=0";

#[inline(always)]
pub(crate) fn enabled(bit: u32) -> bool { MASK.load(Ordering::Relaxed) & bit != 0 }

// ---------------------------------------------------------------------------
struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}

fn parse_u32(value: *const core::ffi::c_char) -> Option<u32> {
    if value.is_null() { return None; }
    let mut command = [0u8; 16];
    let mut len = 0;
    while len < command.len() {
        // SAFETY: NUL-terminated parameter string; at most 16 bytes examined.
        let byte = unsafe { *value.add(len) } as u8;
        if byte == 0 { break; }
        command[len] = byte;
        len += 1;
    }
    if len == command.len() { return None; }
    let text = core::str::from_utf8(&command[..len]).ok()?.trim();
    match text.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => text.parse::<u32>().ok(),
    }
}

unsafe extern "C" fn mask_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    let Some(mask) = parse_u32(value) else { return -22; };
    if mask & !ALL_BITS != 0 { return -22; }
    MASK.store(mask, Ordering::Release);
    0
}
unsafe extern "C" fn mask_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 1000) };
    let mut out = Output { bytes, used: 0 };
    let _ = writeln!(out, "{:#x} {}", MASK.load(Ordering::Relaxed), STATUS_TAIL);
    out.used as core::ffi::c_int
}
static MASK_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(mask_set), get: Some(mask_get), free: None,
};
crate::g18::params::g18_kernel_param!(
    MASK_PARAM,
    "g17p_host_path",
    ops: core::ptr::from_ref(&MASK_OPS),
    perm: 0o600,
    arg: core::ptr::null_mut(),
);
