// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Live GEM object accounting (`gem_stats`, root-only, read-only sysfs).
//!
//! Every shmem object this driver creates is counted from creation until its
//! last reference drops, split by user and kernel ownership. Once a GEM object
//! is mapped into a GPU VM its pages stay pinned (and unevictable) until the
//! object is freed, so `user_bytes + kernel_bytes` is an upper bound on the
//! GPU share of /proc/meminfo `Unevictable`, and a value that keeps growing
//! after clients exit identifies a reference leak in the driver. The
//! `*_created` / `*_freed` totals show allocation churn.
use core::fmt::Write;
use core::sync::atomic::{AtomicU64, Ordering};
use kernel::{bindings, module_param::KernelParam};

static USER_OBJECTS: AtomicU64 = AtomicU64::new(0);
static USER_BYTES: AtomicU64 = AtomicU64::new(0);
static USER_CREATED: AtomicU64 = AtomicU64::new(0);
static KERNEL_OBJECTS: AtomicU64 = AtomicU64::new(0);
static KERNEL_BYTES: AtomicU64 = AtomicU64::new(0);
static KERNEL_CREATED: AtomicU64 = AtomicU64::new(0);

fn counters(kernel: bool) -> (&'static AtomicU64, &'static AtomicU64, &'static AtomicU64) {
    if kernel {
        (&KERNEL_OBJECTS, &KERNEL_BYTES, &KERNEL_CREATED)
    } else {
        (&USER_OBJECTS, &USER_BYTES, &USER_CREATED)
    }
}

/// Count one new GEM object.
pub(crate) fn note_new(kernel: bool, size: usize) {
    let (objects, bytes, created) = counters(kernel);
    objects.fetch_add(1, Ordering::Relaxed);
    bytes.fetch_add(size as u64, Ordering::Relaxed);
    created.fetch_add(1, Ordering::Relaxed);
}

/// Count one freed GEM object.
pub(crate) fn note_free(kernel: bool, size: usize) {
    let (objects, bytes, _) = counters(kernel);
    objects.fetch_sub(1, Ordering::Relaxed);
    bytes.fetch_sub(size as u64, Ordering::Relaxed);
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
    let uc = r(&USER_CREATED);
    let kc = r(&KERNEL_CREATED);
    let (uo, ko) = (r(&USER_OBJECTS), r(&KERNEL_OBJECTS));
    let _ = writeln!(out,
        "user_objects={} user_bytes={} user_created={} user_freed={} \
         kernel_objects={} kernel_bytes={} kernel_created={} kernel_freed={}",
        uo, r(&USER_BYTES), uc, uc.wrapping_sub(uo),
        ko, r(&KERNEL_BYTES), kc, kc.wrapping_sub(ko));
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
        c"gem_stats"
    } else { c"asahi.gem_stats" }),
    #[cfg(MODULE)]
    mod_: core::ptr::addr_of_mut!(bindings::__this_module),
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&STATS_OPS), perm: 0o400, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
