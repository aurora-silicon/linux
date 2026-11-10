// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Live render-dependency resolution counters (`g17p_render_dep_stats`,
//! root-only, read-only sysfs). Counter-only: relaxed atomics, no printk.
//!
//! `g17p_live_render_dependency` turns a still-pending predecessor's native
//! token into an explicit kick dependency. Each failure is counted by
//! `SubmissionError` variant, with the last failure's predecessor kick word,
//! fence status and decoded QID. `high_qid` counts dependencies resolved on
//! a predecessor whose fragment QID is 64..127: its kick word carries QID bit
//! 6 in bit 46, the firmware's valid-queue mask-word selector (ubfx x6,x5,
//! #46,#1), which the resolver used to refuse as unmodeled bits.
use core::fmt::Write;
use core::sync::atomic::{AtomicI64, AtomicU64, Ordering};
use kernel::{bindings, module_param::KernelParam};

use crate::g18::fw::submission::SubmissionError;

pub(crate) static RESOLVED: AtomicU64 = AtomicU64::new(0);
pub(crate) static HIGH_QID: AtomicU64 = AtomicU64::new(0);
static PREDECESSOR_FAILED: AtomicU64 = AtomicU64::new(0);
static MISSING_TOKEN: AtomicU64 = AtomicU64::new(0);
static UNMODELED_BITS: AtomicU64 = AtomicU64::new(0);
static TIMESTAMP_RANGE: AtomicU64 = AtomicU64::new(0);
static QID_RANGE: AtomicU64 = AtomicU64::new(0);
static OTHER: AtomicU64 = AtomicU64::new(0);
static LAST_WORD: AtomicU64 = AtomicU64::new(0);
static LAST_STATUS: AtomicI64 = AtomicI64::new(0);
static LAST_VARIANT: AtomicU64 = AtomicU64::new(0);

/// Count one resolved dependency (Ok with a dependency).
#[inline]
pub(crate) fn note_resolved(native_word: u64) {
    RESOLVED.fetch_add(1, Ordering::Relaxed);
    if (native_word >> crate::g18::fw::submission::KICK_SELECTOR_BIT) & 1 != 0 {
        HIGH_QID.fetch_add(1, Ordering::Relaxed);
    }
}

/// Count one resolver failure. Cold: only failing submissions reach it.
#[cold]
#[inline(never)]
pub(crate) fn note_error(error: SubmissionError, status: i32, native_word: u64) {
    let (counter, variant) = match error {
        SubmissionError::RenderBarrierPredecessorFailed => (&PREDECESSOR_FAILED, 1),
        SubmissionError::RenderBarrierMissingToken => (&MISSING_TOKEN, 2),
        SubmissionError::KickWordUnmodeledBits => (&UNMODELED_BITS, 3),
        SubmissionError::KickTimestampOutOfRange => (&TIMESTAMP_RANGE, 4),
        SubmissionError::KickDependencyQueueIdOutOfRange => (&QID_RANGE, 5),
        _ => (&OTHER, 6),
    };
    counter.fetch_add(1, Ordering::Relaxed);
    LAST_WORD.store(native_word, Ordering::Relaxed);
    LAST_STATUS.store(i64::from(status), Ordering::Relaxed);
    LAST_VARIANT.store(variant, Ordering::Relaxed);
}

struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> core::fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(core::fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len();
        Ok(())
    }
}

unsafe extern "C" fn stats_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 1024) };
    let mut out = Output { bytes, used: 0 };
    let r = |c: &AtomicU64| c.load(Ordering::Relaxed);
    let word = LAST_WORD.load(Ordering::Relaxed);
    let _ = writeln!(out,
        "resolved={} high_qid={} predecessor_failed={} missing_token={} unmodeled_bits={} \
         timestamp_range={} qid_range={} other={} last_variant={} last_status={} \
         last_word={:#x} last_qid={}",
        r(&RESOLVED), r(&HIGH_QID), r(&PREDECESSOR_FAILED), r(&MISSING_TOKEN),
        r(&UNMODELED_BITS), r(&TIMESTAMP_RANGE), r(&QID_RANGE), r(&OTHER),
        r(&LAST_VARIANT), LAST_STATUS.load(Ordering::Relaxed), word,
        (word >> crate::g18::fw::submission::KICK_QID_SHIFT) & u64::from(crate::g18::fw::submission::KICK_QID_MASK));
    out.used as core::ffi::c_int
}

unsafe extern "C" fn stats_set(_: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    -1 // EPERM: read-only counters
}

static STATS_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(stats_set), get: Some(stats_get), free: None,
};
#[link_section = "__param"]
#[used]
static STATS_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_render_dep_stats"
    } else { c"aurora.g17p_render_dep_stats" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&STATS_OPS), perm: 0o400, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
