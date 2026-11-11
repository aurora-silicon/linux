// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! v94 memory-path controls and stall counters (`g17p_mem_path`).
//!
//! Root-only sysfs, runtime-writable bitmask; default 0 is exactly v93f.
//!   bit0 ASYNC_CLOSED_DRAIN: GEM objects whose handles closed while the VM
//!        had accepted jobs are parked (their mappings and backing retained)
//!        until the VM's job count reaches zero. v93f then unmapped and freed
//!        all of them inline in the last job's `T8140VmJobGuard` drop, which
//!        runs inside that job's completion before its output fence signals,
//!        and it held the VM's `draining` gate for the whole drain, stalling
//!        every new submission (`retain_t8140_job`) and every new map
//!        (`wait_for_user_map_admission`). With this bit, when the parked work
//!        is only closed objects on a healthy VM (not quarantined or closed, no
//!        deferred unmaps or close ranges), the list captured at that idle
//!        instant is released on the unbound workqueue without the gate. Jobs
//!        accepted later cannot name a closed handle, and a reused VA has
//!        already been remapped by GPUVM, so no accepted job can reach what the
//!        worker unmaps. Quarantine and VM close keep the gated path.
//!   bit1 MEM_STATS: relaxed counters and maxima for the paths above, GEM
//!        creation, VM_BIND phases and admission waits (printed on read).
//!   bit2 COMPUTE_TS_POOL: compute completion-timestamp pages are recycled
//!        per queue instead of allocating, zeroing, GPU-mapping and later
//!        unmapping a fresh GEM page for every compute command.
//!   bit3 REACH_LOCKLESS: the retained-graph reachability revalidation that
//!        every render publication runs (`first_unreachable_cached`) walks
//!        the app and kernel VM change histories without taking either
//!        GPUVM exec lock (a drm_exec transaction with a 16 KiB kvmalloc and
//!        the VM reservation ww_mutex). Writers record the history lock-free
//!        while the bit is set; a refused lock-free walk (writer active,
//!        history lost, entry not yet recorded) takes the v93f locked walk,
//!        which, with this bit, also serves a publication that found a writer
//!        in progress instead of a full page-table walk.
//! MEM_STATS additionally counts VM mapping changes per class and the
//! reachability paths taken (exact / lock-free walk / locked walk / full).
//! Boot-only `g17p_gem_huge` (asahi.rs) is reported here too.
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use kernel::{bindings, module_param::KernelParam};

pub(crate) const ASYNC_CLOSED_DRAIN: u32 = 1 << 0;
pub(crate) const MEM_STATS: u32 = 1 << 1;
pub(crate) const COMPUTE_TS_POOL: u32 = 1 << 2;
pub(crate) const REACH_LOCKLESS: u32 = 1 << 3;
const ALL_BITS: u32 = (1 << 4) - 1;

static MASK: AtomicU32 = AtomicU32::new(0);
/// Set at probe when the huge GEM mount was created.
pub(crate) static HUGE_MOUNT: AtomicU32 = AtomicU32::new(0);

#[inline(always)]
pub(crate) fn enabled(bit: u32) -> bool { MASK.load(Ordering::Relaxed) & bit != 0 }

/// CNTPCT (1 GHz on Neo) when MEM_STATS is set, else 0.
#[inline(always)]
pub(crate) fn stamp() -> u64 {
    if enabled(MEM_STATS) { crate::g18::compat::timing_buffer::arch_counter() } else { 0 }
}

/// One counted event with a duration: count, total and maximum.
pub(crate) struct Stat { count: AtomicU64, total: AtomicU64, max: AtomicU64 }
impl Stat {
    pub(crate) const fn new() -> Self {
        Self { count: AtomicU64::new(0), total: AtomicU64::new(0), max: AtomicU64::new(0) }
    }
    /// Record `now - start` when `start` came from `stamp()` (0 = disabled).
    #[inline(always)]
    pub(crate) fn since(&self, start: u64) {
        if start == 0 { return; }
        self.add(crate::g18::compat::timing_buffer::arch_counter().saturating_sub(start));
    }
    pub(crate) fn add(&self, ticks: u64) {
        self.count.fetch_add(1, Ordering::Relaxed);
        self.total.fetch_add(ticks, Ordering::Relaxed);
        self.max.fetch_max(ticks, Ordering::Relaxed);
    }
    fn reset(&self) {
        self.count.store(0, Ordering::Relaxed);
        self.total.store(0, Ordering::Relaxed);
        self.max.store(0, Ordering::Relaxed);
    }
    fn print(&self, out: &mut Output<'_>, name: &str) {
        let _ = write!(out, " {}={}/{}/{}", name, self.count.load(Ordering::Relaxed),
            self.total.load(Ordering::Relaxed), self.max.load(Ordering::Relaxed));
    }
}

/// Inline (v93f) and asynchronous closed-object drains, per drain.
pub(crate) static DRAIN_INLINE: Stat = Stat::new();
pub(crate) static DRAIN_ASYNC: Stat = Stat::new();
/// Objects released by either drain.
pub(crate) static DRAIN_OBJECTS: AtomicU64 = AtomicU64::new(0);
/// Objects parked at close because jobs were active.
pub(crate) static PARKED: AtomicU64 = AtomicU64::new(0);
/// Waits for the VM `draining` gate: new submissions and new maps.
pub(crate) static RETAIN_WAIT: Stat = Stat::new();
pub(crate) static ADMISSION_WAIT: Stat = Stat::new();
/// VM_BIND ioctl: whole call, validation+backing (pages) phase, commit phase.
pub(crate) static BIND_CALL: Stat = Stat::new();
pub(crate) static BIND_PREPARE: Stat = Stat::new();
pub(crate) static BIND_COMMIT: Stat = Stat::new();
pub(crate) static BIND_OPS: AtomicU64 = AtomicU64::new(0);
pub(crate) static BIND_BYTES: AtomicU64 = AtomicU64::new(0);
/// GEM_CREATE ioctl.
pub(crate) static GEM_CREATE: Stat = Stat::new();
pub(crate) static GEM_BYTES: AtomicU64 = AtomicU64::new(0);
/// COMPUTE_TS_POOL: pages reused, and freshly allocated.
pub(crate) static TS_REUSED: AtomicU64 = AtomicU64::new(0);
pub(crate) static TS_ALLOCATED: AtomicU64 = AtomicU64::new(0);

/// Retained-graph reachability (per render publication): exact epoch match,
/// lock-free history walk, locked history walk, full page-table walk.
pub(crate) static REACH_EXACT: Stat = Stat::new();
pub(crate) static REACH_LOCKLESS_WALK: Stat = Stat::new();
pub(crate) static REACH_LOCKED_WALK: Stat = Stat::new();
pub(crate) static REACH_FULL: Stat = Stat::new();
/// History entries examined by either walk.
pub(crate) static REACH_WALKED: AtomicU64 = AtomicU64::new(0);
/// Why a full walk ran: no cached proof for the generation, a writer was in
/// progress (no key), the history walk refused, a change touched the graph.
pub(crate) static REACH_FULL_NO_PROOF: AtomicU64 = AtomicU64::new(0);
pub(crate) static REACH_FULL_NO_KEY: AtomicU64 = AtomicU64::new(0);
pub(crate) static REACH_FULL_REFUSED: AtomicU64 = AtomicU64::new(0);
pub(crate) static REACH_FULL_TOUCHED: AtomicU64 = AtomicU64::new(0);
/// Mapping-validation epoch bumps: kernel VM; user VM inside the kernel's
/// timestamp-alias window (compute completion pages); user VM elsewhere.
pub(crate) static VM_CHG_KERNEL: AtomicU64 = AtomicU64::new(0);
pub(crate) static VM_CHG_TS_ALIAS: AtomicU64 = AtomicU64::new(0);
pub(crate) static VM_CHG_USER: AtomicU64 = AtomicU64::new(0);

/// Called from `VmInner::note_mapping_validation_change` when MEM_STATS is set.
pub(crate) fn note_vm_change(kernel: bool, start: u64) {
    let counter = if kernel {
        &VM_CHG_KERNEL
    } else if crate::g18::compat::g17p::T8140_TIMESTAMP_ALIAS_RANGE.contains(&start) {
        &VM_CHG_TS_ALIAS
    } else {
        &VM_CHG_USER
    };
    counter.fetch_add(1, Ordering::Relaxed);
}

#[inline(always)]
pub(crate) fn count(counter: &AtomicU64, value: u64) {
    if enabled(MEM_STATS) { counter.fetch_add(value, Ordering::Relaxed); }
}

struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}

fn parse<'a>(value: *const core::ffi::c_char) -> Option<&'a str> {
    if value.is_null() { return None; }
    // SAFETY: the parameter core supplies a NUL-terminated string.
    let text = unsafe { core::ffi::CStr::from_ptr(value) }.to_str().ok()?;
    Some(text.trim())
}

unsafe extern "C" fn mask_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    let Some(text) = parse(value) else { return -22; };
    if text == "reset" {
        for stat in [&DRAIN_INLINE, &DRAIN_ASYNC, &RETAIN_WAIT, &ADMISSION_WAIT, &BIND_CALL,
                     &BIND_PREPARE, &BIND_COMMIT, &GEM_CREATE, &REACH_EXACT, &REACH_LOCKLESS_WALK,
                     &REACH_LOCKED_WALK, &REACH_FULL] {
            stat.reset();
        }
        for counter in [&DRAIN_OBJECTS, &PARKED, &BIND_OPS, &BIND_BYTES, &GEM_BYTES, &TS_REUSED,
                        &TS_ALLOCATED, &REACH_WALKED, &REACH_FULL_NO_PROOF, &REACH_FULL_NO_KEY,
                        &REACH_FULL_REFUSED, &REACH_FULL_TOUCHED, &VM_CHG_KERNEL, &VM_CHG_TS_ALIAS,
                        &VM_CHG_USER] {
            counter.store(0, Ordering::Relaxed);
        }
        return 0;
    }
    let parsed = match text.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => text.parse::<u32>().ok(),
    };
    let Some(mask) = parsed else { return -22; };
    if mask & !ALL_BITS != 0 { return -22; }
    MASK.store(mask, Ordering::Release);
    0
}

unsafe extern "C" fn mask_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4000) };
    let mut out = Output { bytes, used: 0 };
    let _ = write!(out, "{:#x} huge_mount={} parked={} drained_objects={} bind_ops={} bind_bytes={} gem_bytes={} ts_reused={} ts_allocated={} (stats: count/total_ns/max_ns)",
        MASK.load(Ordering::Relaxed), HUGE_MOUNT.load(Ordering::Relaxed),
        PARKED.load(Ordering::Relaxed), DRAIN_OBJECTS.load(Ordering::Relaxed),
        BIND_OPS.load(Ordering::Relaxed), BIND_BYTES.load(Ordering::Relaxed),
        GEM_BYTES.load(Ordering::Relaxed), TS_REUSED.load(Ordering::Relaxed),
        TS_ALLOCATED.load(Ordering::Relaxed));
    DRAIN_INLINE.print(&mut out, "drain_inline");
    DRAIN_ASYNC.print(&mut out, "drain_async");
    RETAIN_WAIT.print(&mut out, "retain_wait");
    ADMISSION_WAIT.print(&mut out, "admission_wait");
    BIND_CALL.print(&mut out, "bind_call");
    BIND_PREPARE.print(&mut out, "bind_prepare");
    BIND_COMMIT.print(&mut out, "bind_commit");
    GEM_CREATE.print(&mut out, "gem_create");
    let _ = write!(out, " vm_chg_kernel={} vm_chg_ts_alias={} vm_chg_user={} reach_walked={} full_no_proof={} full_no_key={} full_refused={} full_touched={}",
        VM_CHG_KERNEL.load(Ordering::Relaxed), VM_CHG_TS_ALIAS.load(Ordering::Relaxed),
        VM_CHG_USER.load(Ordering::Relaxed), REACH_WALKED.load(Ordering::Relaxed),
        REACH_FULL_NO_PROOF.load(Ordering::Relaxed), REACH_FULL_NO_KEY.load(Ordering::Relaxed),
        REACH_FULL_REFUSED.load(Ordering::Relaxed), REACH_FULL_TOUCHED.load(Ordering::Relaxed));
    REACH_EXACT.print(&mut out, "reach_exact");
    REACH_LOCKLESS_WALK.print(&mut out, "reach_lockless");
    REACH_LOCKED_WALK.print(&mut out, "reach_locked");
    REACH_FULL.print(&mut out, "reach_full");
    let _ = writeln!(out);
    out.used as core::ffi::c_int
}

static MASK_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(mask_set), get: Some(mask_get), free: None,
};
#[link_section = "__param"]
#[used]
static MASK_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_mem_path"
    } else { c"aurora.g17p_mem_path" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&MASK_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
