// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Common code for AOP endpoint drivers

use kernel::{
    device::{Bound, Device},
    dma::Coherent,
    prelude::*,
    sync::Arc,
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

/// Listener for the "HID" events sent by aop
pub trait FakehidListener {
    /// Process the event.
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()>;
}

/// Listener for any other EPIC report subtype on a service (e.g. the T8140
/// low-power microphone producer reports, subtype 0x20).
pub trait ReportListener {
    /// Process the report payload (everything after the EPIC headers).
    fn process_report(&self, subtype: u16, data: &[u8]) -> Result<()>;
}

/// T8140: the low-power microphone source ring.  The firmware accepts one
/// binding for its lifetime (a second bind on the same boot answers
/// 0x2000000000000009), so the AOP driver owns the allocation and hands the
/// same ring to every audio driver probe.
pub struct SourceRing {
    /// The ring, mapped through the audio child's IOMMU stream.
    pub buf: Coherent<[u8]>,
    /// IOVA the firmware produces into.
    pub iova: u64,
}

impl SourceRing {
    /// Size of the ring in bytes.
    pub fn size(&self) -> usize {
        self.buf.size()
    }
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

    /// Adds the listener for the specified service
    fn add_fakehid_listener(
        &self,
        svc: EPICService,
        listener: Arc<dyn FakehidListener>,
    ) -> Result<()>;
    /// Remove the listener for the specified service
    fn remove_fakehid_listener(&self, svc: &EPICService) -> bool;
    /// Adds a report listener for one subtype on the specified service
    fn add_report_listener(
        &self,
        svc: EPICService,
        subtype: u16,
        listener: Arc<dyn ReportListener>,
    ) -> Result<()>;
    /// Remove the report listener for the specified service and subtype
    fn remove_report_listener(&self, svc: &EPICService, subtype: u16) -> bool;
    /// T8140: the low-power microphone source ring, `size` bytes mapped
    /// through `dev` (the audio child).  Allocated and bound to the firmware
    /// over the setup port on first use, then shared for the AOP's lifetime.
    /// Returns ENODEV on other generations.
    fn source_ring(&self, dev: &Device<Bound>, size: usize) -> Result<Arc<SourceRing>>;
    /// Internal method to detach the device.
    fn remove(&self);
}

/// Converts a text representation of a FourCC to u32
pub const fn from_fourcc(b: &[u8]) -> u32 {
    b[3] as u32 | (b[2] as u32) << 8 | (b[1] as u32) << 16 | (b[0] as u32) << 24
}
