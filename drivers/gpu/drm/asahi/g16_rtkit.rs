// SPDX-License-Identifier: GPL-2.0-only OR MIT


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
    time::{Delta, Monotonic, hrtimer::{HrTimer, HrTimerCallback,
        HrTimerCallbackContext, HrTimerPointer, HrTimerRestart, RelativeMode}},
};

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

    pub(crate) fn wait_past(self: &Arc<Self>, seen: u64) -> bool {
        for _ in 0..64 {
            if !self.arrived.try_wait_for_completion() {
                if self.count.load(Ordering::Acquire) != seen { return true; }
                let timer = self.clone().start(Delta::from_micros(100));
                self.arrived.wait_for_completion_timeout(1);
                drop(timer);
                return self.count.load(Ordering::Acquire) != seen;
            }
        }
        true
    }
}

impl HrTimerCallback for EventWait {
    type Pointer<'a> = Arc<Self>;

    fn run(this: ArcBorrow<'_, Self>, _ctx: HrTimerCallbackContext<'_, Self>) -> HrTimerRestart {
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
    pub(crate) event_messages: AtomicU64,
    pub(crate) events: Arc<EventWait>,
}

pub(crate) struct Health {
    crashed: AtomicBool,
    mapped: AtomicBool,
    failed: AtomicBool,
    progress: crate::agx_host_progress::Progress,
    pub(crate) timing: crate::agx_timing_stats::Stats,
}

impl Health {
    pub(crate) fn healthy(&self) -> bool {
        self.mapped.load(Ordering::Acquire)
            && !self.crashed.load(Ordering::Acquire)
            && !self.failed()
    }

    pub(crate) fn set_gpu_pending(&self, pending: bool) { self.progress.set_pending(pending); }

    pub(crate) fn activity_snapshot(&self) -> (u64, u64, bool) {
        let (generation, epoch) = self.progress.activity_snapshot();
        (generation, epoch, self.healthy())
    }

    pub(crate) fn record_completion(&self) { self.progress.record_completion(); }

    pub(crate) fn progress_snapshot(&self) -> (u64, u64, u64, bool) {
        let (generation, count, timestamp) = self.progress.snapshot();
        (generation, count, timestamp, self.healthy())
    }

    pub(crate) fn failed(&self) -> bool { self.failed.load(Ordering::Acquire) }

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
                    progress: crate::agx_host_progress::Progress::new(),
                    timing: crate::agx_timing_stats::Stats::new(),
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
        state.events.record();
        crate::driver::queue_g16_completion_worker(state.drm.clone());
        dev_err!(
            state.dev.as_ref(),
            "G16G: firmware crashed, retained crashlog bytes={}\n",
            crashlog.map_or(0, |b| b.len())
        );
    }

    fn shmem_map(
        state: ArcBorrow<'_, State>,
        physical: usize,
        size: usize,
    ) -> Result<FirmwareBuffer> {
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
        let resource = node.reserved_mem_region_to_resource_byname(c_str!("fw-data"))?;
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
            "G16G: mapped firmware crash storage PA={:#x} size={:#x}\n",
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

    fn recv_message_early(state: ArcBorrow<'_, State>, endpoint: u8, message: u64) -> bool {
        if endpoint == 0x20 && message == 0x0042_0000_0000_0000 {
            state.events.record();
            state.event_messages.fetch_add(1, Ordering::Release);
            if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
                dev_info!(state.dev.as_ref(), "G16G_TIMING event monotonic_ns={} messages={}\n",
                    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get(),
                    state.event_messages.load(Ordering::Acquire));
            }
            crate::driver::queue_g16_completion_worker(state.drm.clone());
            return true;
        }
        false
    }

    fn recv_message(state: ArcBorrow<'_, State>, endpoint: u8, message: u64) {
        dev_warn!(
            state.dev.as_ref(),
            "G16G: RTKit application ep={:#x} message={:#018x}\n",
            endpoint,
            message
        );
    }
}
