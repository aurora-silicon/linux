// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The CLPC's GPU input (`demand.c`): the memory controller and fabric floor
//! while the GPU has work.

use core::ffi::c_void;
use core::ptr::NonNull;
use kernel::{error::to_result, prelude::*, sync::Arc};

extern "C" {
    // `struct device *`: the bindings type has no fields Rust can see.
    fn g18_demand_get(dev: *mut c_void, out: *mut *mut c_void) -> i32;
    fn g18_demand_begin(demand: *mut c_void);
    fn g18_demand_end(demand: *mut c_void);
    fn g18_demand_put(demand: *mut c_void);
}

/// The allocation from `g18_demand_get`, released when the last reference
/// (the session's or a running job's) goes.
struct Handle(NonNull<c_void>);

// SAFETY: the C side serializes on its own lock and queues a work item.
unsafe impl Send for Handle {}
// SAFETY: as above.
unsafe impl Sync for Handle {}

impl Drop for Handle {
    fn drop(&mut self) {
        // SAFETY: a live allocation from `g18_demand_get`, released once,
        // after every guard holding it has ended its job.
        unsafe { g18_demand_put(self.0.as_ptr()) }
    }
}

/// Absent without an `apple,clpc` provider.
pub(crate) struct G18Demand(Option<Arc<Handle>>);

impl G18Demand {
    /// `dev` is the GPU platform device, whose node may name `apple,clpc`.
    /// EPROBE_DEFER while the provider has not probed yet: call it before
    /// the probe touches the GPU.
    pub(crate) fn new<Ctx: kernel::device::DeviceContext>(
        dev: &kernel::device::Device<Ctx>,
    ) -> Result<Self> {
        let mut demand = core::ptr::null_mut();
        // SAFETY: a live device; `demand` is written before the call returns.
        to_result(unsafe { g18_demand_get(dev.as_raw().cast(), &mut demand) })?;
        Ok(Self(match NonNull::new(demand) {
            Some(demand) => Some(Arc::new(Handle(demand), GFP_KERNEL)?),
            None => None,
        }))
    }

    /// A job is running: the floor is held until the returned guard is
    /// dropped, and released 16 ms after the last job ended. The guard
    /// holds its own reference, so it may outlive this value.
    pub(crate) fn job(&self) -> G18DemandJob {
        if let Some(handle) = &self.0 {
            // SAFETY: a live allocation, held by `self`.
            unsafe { g18_demand_begin(handle.0.as_ptr()) }
        }
        G18DemandJob(self.0.clone())
    }
}

/// One job's hold on the floor, from [`G18Demand::job`] until dropped.
pub(crate) struct G18DemandJob(Option<Arc<Handle>>);

impl Drop for G18DemandJob {
    fn drop(&mut self) {
        if let Some(handle) = &self.0 {
            // SAFETY: a live allocation, held by this guard's reference.
            unsafe { g18_demand_end(handle.0.as_ptr()) }
        }
    }
}
