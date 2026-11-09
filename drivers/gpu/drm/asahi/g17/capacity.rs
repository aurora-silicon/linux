// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Name capacity refusals at their source, before a common submit or fence
//! failure handler loses their provenance. Counters are device-lifetime
//! independent diagnostics, never admission or ownership state.

use core::sync::atomic::{AtomicU64, Ordering};
use kernel::prelude::*;

#[derive(Clone, Copy, Debug)]
pub(super) enum Site {
    ComputeReserve,
    PhysicalQids,
    RenderSlots,
    LogicalQueues,
    ComputeSlots,
    QosIds,
    ComputeItems,
    RetiredRenderBindings,
    RenderPoolIds,
    RenderWorkSlots,
    ParameterBuffers,
    MetricsIds,
    LowerVa,
    UpperVa,
    AliasVa,
    ExecutionContexts,
}

const SITE_COUNT: usize = Site::ExecutionContexts as usize + 1;
static REFUSALS: [AtomicU64; SITE_COUNT] = [const { AtomicU64::new(0) }; SITE_COUNT];

/// First and power-of-two refusals per site keep a pressure storm bounded in
/// the log. This does not change whether the caller retries or rejects work.
#[track_caller]
pub(super) fn refused(site: Site) -> Error {
    let count = REFUSALS[site as usize].fetch_add(1, Ordering::Relaxed).wrapping_add(1);
    if count.is_power_of_two() {
        let at = core::panic::Location::caller();
        pr_warn!("G17 capacity refusal {:?} count={} at {}:{}\n", site, count,
            at.file().rsplit("asahi/").next().unwrap_or(at.file()), at.line());
    }
    ENOSPC
}

/// Read-only diagnostics hold the live device session while sampling its pools.
/// They never wake firmware, reclaim a resource, or change admission state.
pub(super) struct View(pub(super) kernel::sync::Arc<super::Shared>);

impl kernel::debugfs::Writer for View {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(f, "version=1 sampled_ns={} crashed={}", super::now_ns(),
                 u8::from(self.0.crashed.load(Ordering::Acquire)))?;
        let uat = {
            let state = self.0.state.lock();
            let Some(firmware) = state.as_deref() else { return writeln!(f, "online=0"); };
            writeln!(f, "online=1")?;
            let (converted, published) = super::sync_input::counters();
            writeln!(f, "fw_inputs converted={} published={} native={} pending={}",
                converted, published, super::sync_input::native_count(),
                super::sync_input::pending_count())?;
            writeln!(f, "compute_admission_demand={}", self.0.compute_demand.load(Ordering::Acquire))?;
            firmware.queues.write_capacity(f)?;
            firmware.pool.write_capacity(f)?;
            writeln!(f, "render_pool_ids used={} total=127", firmware.render_ids.used())?;
            firmware.uat.clone()
        };
        // Mapping destruction can issue firmware flushes while holding VM
        // locks. Never take a VM/TTBAT lock with the device mutex held here.
        // The retained UAT owns valid allocator metadata even during shutdown.
        writeln!(f, "execution_contexts used={} total={}", uat.execution_contexts_used(), crate::mmu::MAX_EXECUTION_CONTEXTS)?;
        let (fenced, overruns) = uat.execution_contexts_fenced(super::channel::control_witnessed());
        writeln!(f, "execution_contexts_fenced free_waiting={} overruns={}", fenced, overruns)?;
        let mut upper = uat.geometry().kernel_range();
        upper.start += crate::hw::t8140::dynamic::KERNEL_OFFSET;
        for (name, vm, range) in [
            ("dynamic_lower_bytes", uat.kernel_lower_vm(), crate::hw::t8140::dynamic::LOWER),
            ("dynamic_upper_bytes", uat.kernel_vm(), upper),
        ] {
            match vm.capacity(range.clone()) {
                Ok((used, largest)) => writeln!(f, "{} used={} total={} largest_hole={}", name, used, range.end - range.start, largest)?,
                Err(error) => writeln!(f, "{} unavailable={:?}", name, error)?,
            }
        }
        for (site, count) in [
            Site::ComputeReserve, Site::PhysicalQids, Site::RenderSlots, Site::LogicalQueues,
            Site::ComputeSlots, Site::QosIds, Site::ComputeItems, Site::RetiredRenderBindings,
            Site::RenderPoolIds, Site::RenderWorkSlots, Site::ParameterBuffers, Site::MetricsIds,
            Site::LowerVa, Site::UpperVa, Site::AliasVa, Site::ExecutionContexts,
        ].iter().zip(REFUSALS.iter()) {
            writeln!(f, "refusals site={:?} count={}", site, count.load(Ordering::Relaxed))?;
        }
        Ok(())
    }
}

// GPULOST diagnostic (test kernel only): where a publication's EBUSY came from.
const BUSY_SLOTS: usize = 256;
const BUSY_DEPTH: usize = 4;
struct BusySlot {
    pid: core::sync::atomic::AtomicI32,
    begin: AtomicU64,
    next: core::sync::atomic::AtomicUsize,
    at: [core::sync::atomic::AtomicPtr<core::panic::Location<'static>>; BUSY_DEPTH],
    ns: [AtomicU64; BUSY_DEPTH],
}
static BUSY: [BusySlot; BUSY_SLOTS] = [const { BusySlot {
    pid: core::sync::atomic::AtomicI32::new(0),
    begin: AtomicU64::new(0),
    next: core::sync::atomic::AtomicUsize::new(0),
    at: [const { core::sync::atomic::AtomicPtr::new(core::ptr::null_mut()) }; BUSY_DEPTH],
    ns: [const { AtomicU64::new(0) }; BUSY_DEPTH],
} }; BUSY_SLOTS];
static BUSY_TOTAL: AtomicU64 = AtomicU64::new(0);

fn busy_slot() -> (&'static BusySlot, i32) {
    let pid = kernel::current!().pid();
    (&BUSY[(pid as usize) % BUSY_SLOTS], pid)
}

/// Every EBUSY produced in g17 comes from here in the diagnostic kernel.
#[track_caller]
pub(crate) fn busy() -> Error {
    BUSY_TOTAL.fetch_add(1, Ordering::Relaxed);
    let (slot, pid) = busy_slot();
    if slot.pid.load(Ordering::Relaxed) == pid {
        let i = slot.next.fetch_add(1, Ordering::Relaxed) % BUSY_DEPTH;
        let at = core::panic::Location::caller() as *const _ as *mut core::panic::Location<'static>;
        slot.at[i].store(at, Ordering::Relaxed);
        slot.ns[i].store(super::now_ns(), Ordering::Relaxed);
    }
    EBUSY
}

/// Start of one scheduler publication by this task.
pub(crate) fn busy_begin() {
    let (slot, pid) = busy_slot();
    slot.pid.store(pid, Ordering::Relaxed);
    slot.begin.store(super::now_ns(), Ordering::Relaxed);
    slot.next.store(0, Ordering::Relaxed);
    for a in slot.at.iter() { a.store(core::ptr::null_mut(), Ordering::Relaxed); }
}

/// An unpublished packet failed with EBUSY: print its recorded origins (newest first).
pub(crate) fn busy_report(what: &str, error: Error) {
    let (slot, pid) = busy_slot();
    let now = super::now_ns();
    let begin = slot.begin.load(Ordering::Relaxed);
    let waited_us = if slot.pid.load(Ordering::Relaxed) == pid && begin != 0 { (now - begin) / 1000 } else { 0 };
    let next = slot.next.load(Ordering::Relaxed);
    let mut sites: [(&str, u32, u64); BUSY_DEPTH] = [("-", 0, 0); BUSY_DEPTH];
    for k in 0..BUSY_DEPTH.min(next) {
        let i = (next - 1 - k) % BUSY_DEPTH;
        let at = slot.at[i].load(Ordering::Relaxed);
        if at.is_null() { continue; }
        // SAFETY: only &'static Location values are stored.
        let at: &'static core::panic::Location<'static> = unsafe { &*at };
        let ago = now.saturating_sub(slot.ns[i].load(Ordering::Relaxed)) / 1000;
        sites[k] = (at.file().rsplit("asahi/").next().unwrap_or(at.file()), at.line(), ago);
    }
    pr_warn!("GPULOST {} {:?} pid={} waited_us={} busy_total={} n={} origin={}:{}(-{}us) prev={}:{}(-{}us) {}:{}(-{}us) {}:{}(-{}us)\n",
        what, error, pid, waited_us, BUSY_TOTAL.load(Ordering::Relaxed), next,
        sites[0].0, sites[0].1, sites[0].2, sites[1].0, sites[1].1, sites[1].2,
        sites[2].0, sites[2].1, sites[2].2, sites[3].0, sites[3].1, sites[3].2);
}
