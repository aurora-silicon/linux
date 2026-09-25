// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Common code for AOP endpoint drivers

use kernel::{
    device::Device,
    prelude::*,
    sync::{
        atomic::{Atomic, Relaxed},
        Arc, //
    },
    types::ForeignOwnable,
};

/// Representation of an "EPIC" service.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct EPICService {
    /// Channel id
    pub channel: u32,
    /// RTKit endpoint
    pub endpoint: u8,
}

/// Listener for the "HID" events sent by aop.
///
/// The AOP driver keeps the listener in an [`Arc`] shared with the child driver that registered
/// it and invokes it from its own RTKit receive worker, so listeners have to be `Send + Sync`.
/// The callback runs with the AOP's RTKit lock, the endpoint's lock and the listener list lock
/// held: it must not sleep for long, must not call into the AOP (no [`AOP::epic_call`] and no
/// listener registration), and it keeps running until the listener is removed.
pub trait FakehidListener: Send + Sync {
    /// Process the event.
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()>;
}

/// Listener for the EPIC reports of a service other than the fake-HID ones, such as the producer
/// reports (subtype 0x20) of the T8140 low-power microphone service.
///
/// The same rules as for [`FakehidListener`] apply: the callback runs from the AOP's RTKit
/// receive worker with the RTKit lock, the endpoint's lock and the listener list lock held, so
/// it must not sleep for long or call into the AOP, and it runs until the listener is removed.
pub trait ReportListener: Send + Sync {
    /// Process the report payload (everything after the EPIC headers).
    fn process_report(&self, subtype: u16, data: &[u8]) -> Result<()>;
}

/// AOP communications manager.
pub trait AOP: Send + Sync {
    /// Calls a method on a specified service
    fn epic_call(&self, svc: &EPICService, subtype: u16, msg_bytes: &[u8]) -> Result<u32>;
    /// Just like epic_call, but also returns a value
    fn epic_call_ret(
        &self,
        svc: &EPICService,
        subtype: u16,
        msg_bytes: &[u8],
        ret_len: usize,
    ) -> Result<(u32, KVec<u8>)>;

    /// Adds the listener for the specified service. A service takes one listener at a time;
    /// a second registration fails with `EBUSY`.
    fn add_fakehid_listener(
        &self,
        svc: EPICService,
        listener: Arc<dyn FakehidListener>,
    ) -> Result<()>;
    /// Removes the listener for the specified service. Returns once no callback into the
    /// listener is running any more.
    fn remove_fakehid_listener(&self, svc: &EPICService) -> bool;
    /// Adds a report listener for one report subtype of the specified service. A service and
    /// subtype take one listener at a time; a second registration fails with `EBUSY`.
    fn add_report_listener(
        &self,
        svc: EPICService,
        subtype: u16,
        listener: Arc<dyn ReportListener>,
    ) -> Result<()>;
    /// Removes the report listener for the specified service and subtype. Returns once no
    /// callback into the listener is running any more.
    fn remove_report_listener(&self, svc: &EPICService, subtype: u16) -> bool;
    /// Internal method to detach the device.
    fn remove(&self);
}

impl dyn AOP {
    /// Returns the AOP core that registered the service device `child`.
    ///
    /// The core registers its service devices from a workqueue while it is still probing, and the
    /// driver core publishes a driver's data only once its probe has returned, so a child can be
    /// probed before the core's data exists. This returns `EPROBE_DEFER` in that case; the child's
    /// probe is retried once the core has bound.
    ///
    /// # Safety
    ///
    /// `child` must be a platform device that the AOP core driver registered for one of its
    /// services, and the caller must be probing it or be bound to it. The core unregisters its
    /// children before it releases its driver data, so that data is valid for the lookup.
    pub unsafe fn from_child(child: &Device) -> Result<Arc<dyn AOP>> {
        let parent = child.parent().ok_or(ENODEV)?;
        let ptr = parent.get_drvdata::<c_void>();
        if ptr.is_null() {
            return Err(EPROBE_DEFER);
        }
        // SAFETY: By this function's contract `ptr` is the AOP core's driver data: a pinned box
        // holding the core's driver type, which is `#[repr(transparent)]` over an `Arc<dyn AOP>`,
        // and it stays valid while `child` is registered.
        let aop = unsafe { Pin::<KBox<Arc<dyn AOP>>>::borrow(ptr) };
        Ok((*aop).clone())
    }
}

/// A child driver's fake-HID subscription on an AOP service.
///
/// The AOP core calls the listener from its receive worker for as long as the subscription
/// exists, so the child has to end it before it releases anything the listener uses. Call
/// [`FakehidRegistration::unregister`] from the driver's `unbind`, while those resources are
/// still alive; dropping the registration unregisters as well, as a fallback for a failed probe.
pub struct FakehidRegistration {
    aop: Arc<dyn AOP>,
    service: EPICService,
    registered: Atomic<bool>,
}

impl FakehidRegistration {
    /// Subscribes `listener` to the fake-HID reports of `service`.
    pub fn new(
        aop: Arc<dyn AOP>,
        service: EPICService,
        listener: Arc<dyn FakehidListener>,
    ) -> Result<Self> {
        aop.add_fakehid_listener(service, listener)?;
        Ok(Self {
            aop,
            service,
            registered: Atomic::new(true),
        })
    }

    /// Ends the subscription. Returns once no callback into the listener is running any more;
    /// repeated calls do nothing.
    pub fn unregister(&self) {
        if self.registered.xchg(false, Relaxed) {
            self.aop.remove_fakehid_listener(&self.service);
        }
    }
}

impl Drop for FakehidRegistration {
    fn drop(&mut self) {
        self.unregister();
    }
}

/// Converts a text representation of a FourCC to u32
pub const fn from_fourcc(b: &[u8]) -> u32 {
    b[3] as u32 | (b[2] as u32) << 8 | (b[1] as u32) << 16 | (b[0] as u32) << 24
}
