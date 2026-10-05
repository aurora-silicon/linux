// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Firmware queue-slot accounting (`g17p_queue_slots`, root-only, read-only
//! sysfs). Relaxed counters; exhaustion logging is rate-limited.
//!
//! Every render pair and compute queue leases QIDs from one 128-entry table
//! (`kick::QueueIds`). A published QID is kept until processor stop, and a
//! quarantined or exit-killed compute graph is never handed out again, so a
//! boot can run out of QIDs while memory is fine. Queue creation then fails
//! with ENOSPC, and so does a new render pair (`render-warm-pairs`). The
//! `qids` fields are exact (kept by the table itself); compute
//! graph ownership gauges track structural changes. Pending teardown and
//! draining-pair counts are sampled in their existing service passes.
//!
//! Every ENOSPC from one of these pools is counted by pool and named in
//! dmesg (the first one, then each power of two), so a failed QUEUE_CREATE
//! or submit says which pool ran out.
use core::fmt::Write;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use kernel::{bindings, module_param::KernelParam, prelude::*};

use super::kick as g17_channel;
use g17_channel::QID_COUNT;

static COMPUTE_GRAPHS: AtomicU32 = AtomicU32::new(0);
static COMPUTE_OWNED: AtomicU32 = AtomicU32::new(0);
static COMPUTE_RELEASED: AtomicU32 = AtomicU32::new(0);
static COMPUTE_QUARANTINED: AtomicU32 = AtomicU32::new(0);
static COMPUTE_EXIT_KILLED: AtomicU32 = AtomicU32::new(0);
static RETIRED_GRAPHS: AtomicU32 = AtomicU32::new(0);
static TEARDOWNS: AtomicU32 = AtomicU32::new(0);
static RENDER_QUARANTINED: AtomicU32 = AtomicU32::new(0);

/// Pools whose exhaustion returns ENOSPC to a client.
#[derive(Clone, Copy)]
pub(crate) enum Pool {
    /// A render slot's first use leases two QIDs (`render-warm-pairs`).
    QidRenderPair,
    /// A new compute graph leases one QID (queue creation / first compute).
    QidCompute,
    /// A new logical queue context (QUEUE_CREATE): its only 128-wide pool
    /// is the QoS hardware-buffer IDs, held by logical queues and installed
    /// graph bindings.
    QueueContextQos,
}

const POOL_COUNT: usize = 3;
const POOL_NAMES: [&str; POOL_COUNT] = ["qid-render-pair", "qid-compute", "queue-context-qos"];
static POOL_ENOSPC: [AtomicU64; POOL_COUNT] =
    [AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)];
/// Submits that failed with ENOSPC, whichever pool.
static SUBMIT_ENOSPC: AtomicU64 = AtomicU64::new(0);

fn log_enospc(pool: &str, stage: &str, count: u64) {
    // First one, then each power of two: a stuck pool cannot flood dmesg.
    if count.is_power_of_two() {
        pr_warn!(
            "G17P ENOSPC pool={} stage={} (#{}): qids leased={}/{} compute={} compute-quarantined={} \
             reserve-refused={}\n",
            pool, stage, count, g17_channel::leased_total(), QID_COUNT,
            g17_channel::LEASED[2].load(Ordering::Relaxed),
            COMPUTE_QUARANTINED.load(Ordering::Relaxed),
            g17_channel::RESERVE_REFUSED.load(Ordering::Relaxed),
        );
    }
}

/// Count and name one ENOSPC from `pool`, and return it.
#[cold]
pub(crate) fn enospc(pool: Pool) -> Error {
    let count = POOL_ENOSPC[pool as usize].fetch_add(1, Ordering::Relaxed) + 1;
    log_enospc(POOL_NAMES[pool as usize], "-", count);
    ENOSPC
}

/// A submit failed with ENOSPC at `stage`. Pools tagged at their source
/// were already counted and named; this names the rest by stage.
#[cold]
pub(crate) fn note_submit_enospc(stage: &str) {
    let count = SUBMIT_ENOSPC.fetch_add(1, Ordering::Relaxed) + 1;
    // Source-specific QID/QoS failures already counted their actual pool.
    // This boundary knows the engine stage, not which nested allocator failed.
    log_enospc("submit", stage, count);
}

/// Incremental graph ownership accounting. Normal submissions do not touch
/// these counters; only graph creation, ownership transitions and drop do.
pub(crate) struct ComputeAccount { flags: u8 }
impl ComputeAccount {
    pub(crate) fn new() -> Self {
        COMPUTE_GRAPHS.fetch_add(1, Ordering::Relaxed);
        COMPUTE_OWNED.fetch_add(1, Ordering::Relaxed);
        Self { flags: 1 }
    }
    pub(crate) fn update(&mut self, owned: bool, released: bool, quarantined: bool, exit_killed: bool) {
        let next = u8::from(owned) | (u8::from(released) << 1)
            | (u8::from(quarantined) << 2) | (u8::from(exit_killed) << 3);
        let changed = next ^ self.flags;
        for (bit, counter) in [&COMPUTE_OWNED, &COMPUTE_RELEASED,
            &COMPUTE_QUARANTINED, &COMPUTE_EXIT_KILLED].into_iter().enumerate()
        {
            if changed & (1 << bit) != 0 {
                if next & (1 << bit) != 0 { counter.fetch_add(1, Ordering::Relaxed); }
                else { counter.fetch_sub(1, Ordering::Relaxed); }
            }
        }
        self.flags = next;
    }
}
impl Drop for ComputeAccount {
    fn drop(&mut self) {
        self.update(false, false, false, false);
        COMPUTE_GRAPHS.fetch_sub(1, Ordering::Relaxed);
    }
}

pub(crate) fn note_teardowns(count: usize) {
    TEARDOWNS.store(count as u32, Ordering::Relaxed);
}
pub(crate) fn note_draining_pairs(count: usize) {
    RENDER_QUARANTINED.store(count as u32, Ordering::Relaxed);
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

unsafe extern "C" fn slots_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 1024) };
    let mut out = Output { bytes, used: 0 };
    let r = |c: &AtomicU32| c.load(Ordering::Relaxed);
    let e = |pool: usize| POOL_ENOSPC[pool].load(Ordering::Relaxed);
    let leased = g17_channel::leased_total();
    let published: u32 = g17_channel::PUBLISHED.iter().map(r).sum();
    let _ = writeln!(out,
        "qids leased={} free={} published={} tiling={} fragment={} compute={} high_water={} exhausted={} \
         reserve={} reserve_refused={} \
         compute graphs={} owned={} released={} quarantined={} exit_killed={} retired_graphs={} \
         teardowns={} render_draining_pairs={} \
         enospc qid-render-pair={} qid-compute={} queue-context-qos={} submits={}",
        leased, (QID_COUNT as u32).saturating_sub(leased), published,
        r(&g17_channel::LEASED[0]), r(&g17_channel::LEASED[1]), r(&g17_channel::LEASED[2]),
        r(&g17_channel::HIGH_WATER), g17_channel::EXHAUSTED.load(Ordering::Relaxed),
        g17_channel::COMPUTE_RENDER_RESERVE, g17_channel::RESERVE_REFUSED.load(Ordering::Relaxed),
        r(&COMPUTE_GRAPHS), r(&COMPUTE_OWNED), r(&COMPUTE_RELEASED), r(&COMPUTE_QUARANTINED),
        r(&COMPUTE_EXIT_KILLED), r(&RETIRED_GRAPHS), r(&TEARDOWNS), r(&RENDER_QUARANTINED),
        e(0), e(1), e(2), SUBMIT_ENOSPC.load(Ordering::Relaxed));
    out.used as core::ffi::c_int
}

unsafe extern "C" fn slots_set(_: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    -1 // EPERM: read-only counters
}

static SLOTS_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(slots_set), get: Some(slots_get), free: None,
};
#[link_section = "__param"]
#[used]
static SLOTS_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_queue_slots"
    } else { c"asahi.g17p_queue_slots" }),
    #[cfg(MODULE)]
    mod_: core::ptr::addr_of_mut!(bindings::__this_module),
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&SLOTS_OPS), perm: 0o400, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
