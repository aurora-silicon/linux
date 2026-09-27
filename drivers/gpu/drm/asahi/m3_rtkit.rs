// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! RTKit callbacks for the identified J514S firmware. The firmware advertises
//! its crash storage by physical address inside the reserved data segment.

use crate::g16_resources::Region;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use kernel::{
    c_str, impl_has_hr_timer,
    device::Core,
    sync::Completion,
    io::mem::{Mem, MemFlag},
    iosys_map::IoSysMapRef,
    platform,
    prelude::*,
    soc::apple::rtkit,
    sync::{aref::ARef, Arc, ArcBorrow},
    time::{Delta, Monotonic, hrtimer::{HasHrTimer, HrTimer, HrTimerCallback,
        HrTimerCallbackContext, HrTimerPointer, HrTimerRestart, RelativeMode}},
};

/// IRQ-safe notification tokens for the single GPU completion worker.
#[pin_data]
pub(crate) struct EventWait {
    count: AtomicU64,
    #[pin]
    arrived: Completion,
    #[pin]
    timer: HrTimer<Self>,
}

impl EventWait {
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(EventWait {
                count: AtomicU64::new(0),
                arrived <- Completion::new(),
                timer <- HrTimer::new(),
            }),
            GFP_KERNEL,
        )
    }

    fn record(&self) {
        self.count.fetch_add(1, Ordering::Release);
        self.arrived.complete();
    }

    /// Sleep until a notification later than `seen` has been recorded or
    /// 100 us elapse. A one-jiffy fallback alone takes up to 4 ms at HZ=250,
    /// even when the GPU stamp arrives just after its notification. The timer
    /// adds a bounded resnapshot without replacing IRQ wakeups or busy-waiting.
    /// Only the single completion worker may call this method.
    pub(crate) fn wait_past(self: &Arc<Self>, seen: u64) -> bool {
        // Events can arrive while the worker is polling. Discard stale wakeup
        // tokens before checking the sequence. A concurrent event either moves
        // the sequence or leaves a token, including between the check and wait.
        // Bound draining so a busy interrupt source cannot hold this worker.
        for _ in 0..64 {
            if !self.arrived.try_wait_for_completion() {
                if self.count.load(Ordering::Acquire) != seen { return true; }
                // The handle keeps EventWait alive and cancels synchronously
                // on every return, before a subsequent wait can arm the timer.
                let timer = self.clone().start(Delta::from_micros(100));
                // Retain one tick as a backup bound; normally either the real
                // interrupt or our high-resolution timer supplies the token.
                self.arrived.wait_for_completion_timeout(1);
                drop(timer);
                return self.count.load(Ordering::Acquire) != seen;
            }
        }
        // Resnapshot GPU progress before draining another batch of tokens.
        true
    }
}

impl HrTimerCallback for EventWait {
    type Pointer<'a> = Arc<Self>;

    fn run(this: ArcBorrow<'_, Self>, _ctx: HrTimerCallbackContext<'_, Self>) -> HrTimerRestart {
        // Wake only: a timer expiry is not a firmware completion notification.
        this.arrived.complete();
        HrTimerRestart::NoRestart
    }
}

impl_has_hr_timer! {
    impl HasHrTimer<Self> for EventWait {
        mode: RelativeMode<Monotonic>, field: self.timer
    }
}

pub(crate) struct State {
    dev: ARef<platform::Device>,
    drm: crate::driver::AsahiDevRef,
    data: Region,
    pub(crate) health: Arc<Health>,
    claimed: AtomicBool,
    /// Firmware event notifications received on the application endpoint.
    /// The firmware sends one after a command's completion processing.
    pub(crate) event_messages: AtomicU64,
    pub(crate) events: Arc<EventWait>,
}

/// Shared status contains no device references, so DRM files may retain it
/// after runtime teardown without retaining MMIO or creating a DRM cycle.
pub(crate) struct Health {
    crashed: AtomicBool,
    mapped: AtomicBool,
    failed: AtomicBool,
    /// Host-verified retirement state. One serialized runtime writer; no log
    /// parsing, allocation, MMIO or firmware notification alone can advance it.
    completed: AtomicU64,
    last_completion_ns: AtomicU64,
    generation_ns: u64,
}

impl Health {
    pub(crate) fn healthy(&self) -> bool {
        self.mapped.load(Ordering::Acquire)
            && !self.crashed.load(Ordering::Acquire)
            && !self.failed()
    }

    pub(crate) fn record_completion(&self) {
        let now = <Monotonic as kernel::time::ClockSource>::ktime_get() as u64;
        // Publish the timestamp before the count. An acquiring observer of a
        // new count sees that completion's timestamp or a later REAL completion.
        // A racing writer may expose a newer timestamp with an older count;
        // readers never renew unless the count advances too.
        self.last_completion_ns.store(now, Ordering::Release);
        self.completed.fetch_add(1, Ordering::Release);
    }

    pub(crate) fn progress_snapshot(&self) -> (u64, u64, u64, bool) {
        let completed = self.completed.load(Ordering::Acquire);
        let last_ns = self.last_completion_ns.load(Ordering::Acquire);
        (self.generation_ns, completed, last_ns, self.healthy())
    }

    pub(crate) fn failed(&self) -> bool { self.failed.load(Ordering::Acquire) }

    /// Whether RTKit reported a firmware crash.
    pub(crate) fn crashed(&self) -> bool { self.crashed.load(Ordering::Acquire) }

    pub(crate) fn mark_failed(&self) { self.failed.store(true, Ordering::Release); }
}

impl State {
    pub(crate) fn new(dev: &platform::Device<Core>, drm: crate::driver::AsahiDevRef, data: Region) -> Result<Arc<Self>> {
        Ok(Arc::new(
            Self {
                dev: dev.into(),
                drm,
                data,
                health: Arc::new(Health {
                    crashed: AtomicBool::new(false),
                    mapped: AtomicBool::new(false),
                    failed: AtomicBool::new(false),
                    completed: AtomicU64::new(0),
                    last_completion_ns: AtomicU64::new(0),
                    generation_ns: <Monotonic as kernel::time::ClockSource>::ktime_get() as u64,
                }, GFP_KERNEL)?,
                claimed: AtomicBool::new(false),
                event_messages: AtomicU64::new(0),
                events: EventWait::new()?,
            },
            GFP_KERNEL,
        )?)
    }

    pub(crate) fn healthy(&self) -> bool {
        self.health.healthy()
    }
}

pub(crate) struct FirmwareBuffer {
    state: Arc<State>,
    mapping: Mem,
    physical: usize,
    offset: usize,
    size: usize,
}

impl Drop for FirmwareBuffer {
    fn drop(&mut self) {
        // Includes allocation failure in RTKit's wrapper after shmem_map
        // returned: readiness must not survive loss of the actual mapping.
        self.state.health.mapped.store(false, Ordering::Release);
    }
}

impl rtkit::Buffer for FirmwareBuffer {
    fn iova(&self) -> Result<usize> {
        Ok(self.physical)
    }
    fn buf(&mut self) -> Result<IoSysMapRef<'_, u8>> {
        self.mapping.iosys_map(self.offset, self.size)
    }
}

pub(crate) struct Operations;

#[kernel::macros::vtable]
impl rtkit::Operations for Operations {
    type Data = Arc<State>;
    type Buffer = FirmwareBuffer;

    fn crashed(state: ArcBorrow<'_, State>, crashlog: Option<&[u8]>) {
        state.health.crashed.store(true, Ordering::Release);
        // Wake the serialized worker even if the firmware can no longer send
        // its usual completion notification. Never take the runtime lock here.
        state.events.record();
        crate::driver::queue_g16_completion_worker(state.drm.clone());
        dev_err!(
            state.dev.as_ref(),
            "M3 G15S: firmware crashed, retained crashlog bytes={}\n",
            crashlog.map_or(0, |b| b.len())
        );
    }

    fn shmem_map(
        state: ArcBorrow<'_, State>,
        physical: usize,
        size: usize,
    ) -> Result<FirmwareBuffer> {
        // The only system buffer advertised by this admitted firmware is its
        // preallocated crash log. Never reinterpret an arbitrary DVA as a PA.
        let offset = physical
            .checked_sub(state.data.base as usize)
            .ok_or(EINVAL)?;
        if size == 0
            || (physical | size) & 0xfff != 0
            || offset.checked_add(size).ok_or(EOVERFLOW)? > state.data.size as usize
        {
            return Err(EINVAL);
        }
        if state.claimed.swap(true, Ordering::AcqRel) {
            return Err(EBUSY);
        }
        let node = state.dev.as_ref().of_node().ok_or(ENODEV)?;
        let resource = crate::m3_resources::reserved_resource(&node,c_str!("fw-data"))?;
        if resource.start() != state.data.base || resource.size() != state.data.size {
            return Err(EINVAL);
        }
        // SAFETY: The validated no-map data reservation belongs to this
        // firmware session. This CPU mapping is retained by RTKit until ASC
        // is stopped and callbacks drained; physical pages are never freed.
        // An uncached CPU alias lets RTKit snapshot newly written crash data.
        let mapping = unsafe { Mem::try_new(resource, MemFlag::WC.into()) }?;
        dev_info!(
            state.dev.as_ref(),
            "M3 G15S: mapped firmware crash storage PA={:#x} size={:#x}\n",
            physical,
            size
        );
        state.health.mapped.store(true, Ordering::Release);
        Ok(FirmwareBuffer {
            state: state.into(),
            mapping,
            physical,
            offset,
            size,
        })
    }

    fn recv_message(state: ArcBorrow<'_, State>, endpoint: u8, message: u64) {
        // Firmware retries its event notification until the shared rings are
        // drained, including after the final userspace job. Never take the
        // runtime mutex in the RTKit callback: enqueue the serialized worker.
        if endpoint == 0x20 && message == 0x0042_0000_0000_0000 {
            // Record the IRQ-safe token before publishing the external count.
            // A concurrent waiter sees either the moved sequence or a token.
            state.events.record();
            state.event_messages.fetch_add(1, Ordering::Release);
            if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
                dev_info!(state.dev.as_ref(), "M3 G15S_TIMING event monotonic_ns={} messages={}\n",
                    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get(),
                    state.event_messages.load(Ordering::Acquire));
            }
            crate::driver::queue_g16_completion_worker(state.drm.clone());
            return;
        }
        dev_warn!(
            state.dev.as_ref(),
            "M3 G15S: RTKit application ep={:#x} message={:#018x}\n",
            endpoint,
            message
        );
    }
}
