// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The CLPC's GPU input (`demand.c`): the memory controller and fabric floor
//! while the GPU has work.

use core::ffi::c_void;
use core::ptr::NonNull;

extern "C" {
    fn g18_demand_get(dev: *mut kernel::bindings::device) -> *mut c_void;
    fn g18_demand_begin(demand: *mut c_void);
    fn g18_demand_end(demand: *mut c_void);
    fn g18_demand_put(demand: *mut c_void);
}

/// Absent without an `apple,clpc` provider.
pub(crate) struct G18Demand(Option<NonNull<c_void>>);

// SAFETY: the C side serializes on its own lock and queues a work item.
unsafe impl Send for G18Demand {}
// SAFETY: as above.
unsafe impl Sync for G18Demand {}

impl G18Demand {
    /// `dev` is the GPU platform device, whose node may name `apple,clpc`.
    pub(crate) fn new<Ctx: kernel::device::DeviceContext>(
        dev: &kernel::device::Device<Ctx>,
    ) -> Self {
        // SAFETY: a live device.
        Self(NonNull::new(unsafe { g18_demand_get(dev.as_raw()) }))
    }

    /// A job is running: the floor is held until the returned guard is
    /// dropped, and released 16 ms after the last job ended.
    pub(crate) fn job(&self) -> G18DemandJob {
        if let Some(demand) = self.0 {
            // SAFETY: a live allocation from `g18_demand_get`.
            unsafe { g18_demand_begin(demand.as_ptr()) }
        }
        G18DemandJob(self.0)
    }
}

impl Drop for G18Demand {
    fn drop(&mut self) {
        if let Some(demand) = self.0 {
            // SAFETY: a live allocation from `g18_demand_get`, freed here
            // only; no job guard outlives the session that owns it.
            unsafe { g18_demand_put(demand.as_ptr()) }
        }
    }
}

/// One job's hold on the floor, from [`G18Demand::job`] until dropped.
pub(crate) struct G18DemandJob(Option<NonNull<c_void>>);

impl Drop for G18DemandJob {
    fn drop(&mut self) {
        if let Some(demand) = self.0 {
            // SAFETY: the guard lives only within the job wait, which holds
            // the session that owns the allocation.
            unsafe { g18_demand_end(demand.as_ptr()) }
        }
    }
}
