// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Common code for AOP endpoint drivers

use kernel::{
    device::{Bound, Device},
    dma::Coherent,
    new_mutex,
    prelude::*,
    sync::{
        atomic::{Atomic, Relaxed},
        Arc, Mutex, //
    },
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

/// Child protocols qualified by a hardware profile, independently of EPIC framing.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum ServiceABI {
    /// Discover firmware services without exposing a child that can configure them.
    ControlOnly,
    /// The existing PDM-upload microphone child.
    Legacy,
    /// T8140 setup-port and version-4 audio child.
    T8140,
    /// J616s EPIC-v2 jack control with retained firmware MCA profiles.
    J616sJack,
}

impl ServiceABI {
    /// Control discovery is independent of child configuration qualification.
    pub fn publishes_children(self) -> bool {
        self != Self::ControlOnly
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ServiceAdmission {
    Probing,
    Ready,
    Closed,
}

/// Shared admission gate; closing covers existing and not-yet-registered children.
#[pin_data]
pub struct ServiceGate {
    #[pin]
    admission: Mutex<ServiceAdmission>,
}

impl ServiceGate {
    /// Allocate before the provider hands ownership to hardware.
    pub fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                admission <- new_mutex!(ServiceAdmission::Probing),
            }),
            GFP_KERNEL,
        )
    }

    /// Clone a safely owned provider while lookup admission remains open.
    fn acquire(&self, provider: &Arc<dyn AOP>) -> Result<Arc<dyn AOP>> {
        let admission = self.admission.lock();
        match *admission {
            ServiceAdmission::Probing => Err(EPROBE_DEFER),
            ServiceAdmission::Closed => Err(ENODEV),
            ServiceAdmission::Ready => Ok(provider.clone()),
        }
    }

    /// The provider completed startup; do not reopen a retired context.
    pub fn ready(&self) {
        let mut admission = self.admission.lock();
        if *admission == ServiceAdmission::Probing {
            *admission = ServiceAdmission::Ready;
        }
    }

    /// Wait for pending lookups to finish cloning, then refuse all new ones.
    pub fn close(&self) {
        *self.admission.lock() = ServiceAdmission::Closed;
    }
}

/// Stable child-owned context, independent of the parent's driver-data box.
#[pin_data]
pub struct ServiceContext {
    provider: Arc<dyn AOP>,
    gate: Arc<ServiceGate>,
}

impl ServiceContext {
    /// Allocate before child registration can probe or publish a DMA address.
    pub fn new(provider: Arc<dyn AOP>, gate: Arc<ServiceGate>) -> Result<Pin<KBox<Self>>> {
        KBox::pin_init(pin_init!(Self { provider, gate }), GFP_KERNEL)
    }

    /// Cloning is synchronized with provider closing; returned references are owned.
    pub fn acquire(&self) -> Result<Arc<dyn AOP>> {
        self.gate.acquire(&self.provider)
    }
}

/// Copied into a platform child's data. The service prefix preserves the
/// existing service reader layout; context storage is owned by that child.
#[repr(C)]
pub struct ServicePlatformData {
    /// Firmware service, not a provider driver-data pointer.
    pub service: EPICService,
    /// Stable until unregister; retained closed on unconfirmed shutdown.
    pub context: *const ServiceContext,
    /// The hardware profile's child protocol, not inferred from DT or EPIC framing.
    pub abi: ServiceABI,
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

/// The T8140 low-power microphone source ring: a buffer the AOP firmware produces audio frames
/// into, mapped through the audio child's IOMMU stream.
///
/// The firmware accepts one ring per boot (a second binding is answered with
/// 0x2000000000000009), so the AOP core owns the allocation and hands the same ring to every
/// probe of the audio driver.
pub struct SourceRing {
    buf: Coherent<[u8]>,
    /// The IOVA the firmware produces into.
    pub iova: u64,
}

impl SourceRing {
    /// Wraps a coherent allocation as the source ring.
    pub fn new(buf: Coherent<[u8]>) -> Self {
        let iova = buf.dma_handle();
        Self { buf, iova }
    }

    /// Size of the ring in bytes.
    pub fn size(&self) -> usize {
        self.buf.size()
    }

    /// Copies `dst.len()` bytes from offset `offset` of the ring into `dst`.
    ///
    /// The firmware writes the ring while it runs, so the copy is only meaningful for a span the
    /// firmware has published through its producer report and has not yet reused; the caller
    /// keeps within it and treats the data as untrusted.
    pub fn read(&self, offset: usize, dst: &mut [u8]) -> Result<()> {
        let end = offset.checked_add(dst.len()).ok_or(EINVAL)?;
        if end > self.buf.size() {
            return Err(EINVAL);
        }
        // SAFETY: `offset + dst.len()` does not exceed the allocation, so the source pointer and
        // the copy stay inside it; `dst` is a distinct allocation. By the protocol above the
        // firmware does not write the span while it is copied.
        unsafe {
            let src = self.buf.as_ptr().cast::<u8>().add(offset);
            core::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), dst.len());
        }
        Ok(())
    }
}

/// AOP communications manager.
pub trait AOP: Send + Sync {
    /// Negotiated RTKit protocol, after startup; no register access is performed.
    fn protocol_version(&self) -> Result<u32> {
        Err(ENODEV)
    }
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
    /// The T8140 low-power microphone source ring: `size` bytes allocated on and mapped through
    /// `dev`, the audio child, and bound to the firmware over the setup port on first use, then
    /// shared for the rest of the AOP's lifetime. Fails with `ENODEV` on firmware without a setup
    /// port, with `EBUSY` when the bound ring is smaller than `size`, and with `EIO` after a
    /// binding whose outcome is unknown.
    fn source_ring(&self, dev: &Device<Bound>, size: usize) -> Result<Arc<SourceRing>>;
    /// Internal method to detach the device.
    fn remove(&self);
}

impl dyn AOP {
    /// Looks up an audio provider only when its hardware profile allows this ABI.
    ///
    /// Checking this before any attach/property write also protects against a
    /// stale or incorrect child DT compatible. EPIC v2 alone does not qualify
    /// the legacy PDM upload and low-power channel setters.
    ///
    /// # Safety
    ///
    /// The same platform-child and lookup lifetime contract as [`Self::from_child`].
    pub unsafe fn audio_from_child(child: &Device, abi: ServiceABI) -> Result<Arc<dyn AOP>> {
        // SAFETY: The core owns the copied immutable platform payload during probe/unbind.
        let data = unsafe { (*child.as_raw()).platform_data.cast::<ServicePlatformData>().as_ref() }
            .ok_or(ENODEV)?;
        if abi == ServiceABI::ControlOnly || data.abi != abi {
            return Err(ENODEV);
        }
        // SAFETY: Caller supplies the same registered AOP child as above.
        unsafe { Self::from_child(child) }
    }

    /// Returns the AOP core that registered the service device `child`.
    ///
    /// Initial lookups defer while the provider is probing. Closing synchronizes
    /// pending lookups before unbind; admitted callers own an Arc independent
    /// of parent drvdata, and retained children refuse all later lookups.
    ///
    /// # Safety
    ///
    /// `child` must be a platform device that the AOP core driver registered for one of its
    /// services, and the caller must be probing it or bound to it. Its platform
    /// data/context remain alive until unregister serializes all child probes.
    pub unsafe fn from_child(child: &Device) -> Result<Arc<dyn AOP>> {
        // SAFETY: The platform data has the layout/lifetime in this contract.
        let data = unsafe { (*child.as_raw()).platform_data.cast::<ServicePlatformData>().as_ref() }
            .ok_or(ENODEV)?;
        // SAFETY: The child owns this stable context until unregister, or
        // retains it permanently closed together with its DMA domain.
        let context = unsafe { data.context.as_ref() }.ok_or(ENODEV)?;
        context.acquire()
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
