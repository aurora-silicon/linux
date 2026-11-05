// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! DIAGNOSTIC (default off, read through sysfs only).
//!
//! `g17p_stack_watch` (root-only, runtime or boot "1"/"0"/"reset"): kernel
//! stack high-water mark of render publication. With it set, publish_with
//! paints the unused part of the task stack below the current SP and, after
//! publication returns, scans for the deepest overwritten word. The result
//! includes every callee (kernel printk, scheduler, exception entry). Kept
//! separately for the submit-ioctl early path and the scheduler worker path,
//! and for publications during which a first-kick KSMFE configure or a
//! registration power bracket ran. THREAD_SIZE is 32 KiB (MIN_THREAD_SHIFT 15,
//! 16K pages, VMAP_STACK). Never a default.
use core::fmt::Write;
use core::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering};
use kernel::{bindings, module_param::KernelParam};

const THREAD_SIZE: usize = 1 << 15;
const GUARD: usize = 256;
const PATTERN: u64 = 0xa5a5_5a5a_c3c3_3c3c;

// ---- stack watch ----
static SW_ENABLED: AtomicU32 = AtomicU32::new(0);
/// [early, scheduler, first-install, bracket]: samples, max used, max SP depth at entry.
static SW_SAMPLES: [AtomicU64; 4] = [const { AtomicU64::new(0) }; 4];
static SW_MAX_USED: [AtomicUsize; 4] = [const { AtomicUsize::new(0) }; 4];
static SW_MAX_ENTRY: [AtomicUsize; 4] = [const { AtomicUsize::new(0) }; 4];
static SW_SKIPPED: AtomicU64 = AtomicU64::new(0);
pub(crate) static FIRST_INSTALL_SEQ: AtomicU64 = AtomicU64::new(0);
pub(crate) static BRACKET_SEQ: AtomicU64 = AtomicU64::new(0);

pub(crate) struct StackWatch { base: usize, top: usize, entry_depth: usize, seq: [u64; 2] }

#[inline(always)]
fn current_sp() -> usize {
    let sp: usize;
    // SAFETY: reads the stack pointer; no memory access.
    unsafe { core::arch::asm!("mov {}, sp", out(reg) sp, options(nomem, nostack, preserves_flags)) };
    sp
}

/// Paint the unused stack below this frame (only while enabled).
#[inline(never)]
pub(crate) fn begin() -> Option<StackWatch> {
    if SW_ENABLED.load(Ordering::Relaxed) == 0 { return None; }
    // SAFETY: `current` is always valid in process/kthread context.
    let base = unsafe { (*bindings::get_current()).stack } as usize;
    let sp = current_sp();
    let top = base.checked_add(THREAD_SIZE)?;
    if base == 0 || sp <= base + GUARD + 64 || sp >= top {
        SW_SKIPPED.fetch_add(1, Ordering::Relaxed);
        return None;
    }
    let mut at = (base + GUARD + 7) & !7;
    let end = (sp - 64) & !7;
    while at < end {
        // SAFETY: [base+GUARD, sp-64) is this task's own unused stack; the
        // arm64 kernel ABI has no red zone and nothing live sits below SP.
        unsafe { core::ptr::write_volatile(at as *mut u64, PATTERN) };
        at += 8;
    }
    Some(StackWatch { base, top, entry_depth: top - sp,
        seq: [FIRST_INSTALL_SEQ.load(Ordering::Relaxed), BRACKET_SEQ.load(Ordering::Relaxed)] })
}

#[inline(never)]
pub(crate) fn end(watch: Option<StackWatch>, early: bool) {
    let Some(w) = watch else { return };
    let mut at = (w.base + GUARD + 7) & !7;
    // SAFETY: scanning this task's own stack between the guard and the top.
    while at < w.top && unsafe { core::ptr::read_volatile(at as *const u64) } == PATTERN { at += 8; }
    let used = w.top - at;
    let mut classes = [false; 4];
    classes[if early { 0 } else { 1 }] = true;
    classes[2] = FIRST_INSTALL_SEQ.load(Ordering::Relaxed) != w.seq[0];
    classes[3] = BRACKET_SEQ.load(Ordering::Relaxed) != w.seq[1];
    for (index, set) in classes.into_iter().enumerate() {
        if !set { continue; }
        SW_SAMPLES[index].fetch_add(1, Ordering::Relaxed);
        SW_MAX_USED[index].fetch_max(used, Ordering::Relaxed);
        SW_MAX_ENTRY[index].fetch_max(w.entry_depth, Ordering::Relaxed);
    }
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

fn param_text<'a>(value: *const core::ffi::c_char) -> Option<&'a str> {
    if value.is_null() { return None; }
    // SAFETY: the parameter core supplies a NUL-terminated string.
    unsafe { core::ffi::CStr::from_ptr(value) }.to_str().ok().map(str::trim)
}

unsafe extern "C" fn sw_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    match param_text(value) {
        Some("reset") => {
            for i in 0..4 {
                SW_SAMPLES[i].store(0, Ordering::Relaxed);
                SW_MAX_USED[i].store(0, Ordering::Relaxed);
                SW_MAX_ENTRY[i].store(0, Ordering::Relaxed);
            }
            0
        }
        Some("1") => { SW_ENABLED.store(1, Ordering::Relaxed); 0 }
        Some("0") => { SW_ENABLED.store(0, Ordering::Relaxed); 0 }
        _ => -22,
    }
}

unsafe extern "C" fn sw_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 1024) };
    let mut out = Output { bytes, used: 0 };
    let _ = write!(out, "enabled={} thread_size={} skipped={}", SW_ENABLED.load(Ordering::Relaxed),
        THREAD_SIZE, SW_SKIPPED.load(Ordering::Relaxed));
    for (index, name) in ["early", "sched", "first_install", "bracket"].iter().enumerate() {
        let used = SW_MAX_USED[index].load(Ordering::Relaxed);
        let _ = write!(out, " {}:samples={}/max_used={}/margin={}/max_entry_depth={}", name,
            SW_SAMPLES[index].load(Ordering::Relaxed), used,
            if used == 0 { 0 } else { THREAD_SIZE as i64 - used as i64 },
            SW_MAX_ENTRY[index].load(Ordering::Relaxed));
    }
    let _ = writeln!(out, " first_install_seq={} bracket_seq={}",
        FIRST_INSTALL_SEQ.load(Ordering::Relaxed), BRACKET_SEQ.load(Ordering::Relaxed));
    out.used as core::ffi::c_int
}

static SW_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(sw_set), get: Some(sw_get), free: None,
};
#[link_section = "__param"]
#[used]
static SW_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_stack_watch"
    } else { c"asahi.g17p_stack_watch" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&SW_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
