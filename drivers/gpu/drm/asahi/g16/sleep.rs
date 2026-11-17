// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! System sleep: the firmware goes idle, its domain goes off, and it restarts warm.
//!
//! The firmware isn't initialized again. It keeps its rings and state in DRAM, so a restart is
//! the RTKit handshake. That only works after a real wake: iBoot's last wake program reprograms
//! the GPU's tables with the domain on and leaves it off, and after a stop with no sleep the
//! firmware doesn't answer the handshake.
//!
//! So `.prepare` only drains and closes publication, and the stop is left to the sleep's commit,
//! which runs it with interrupts off and every refusal behind it. A sleep refused before then
//! finds the firmware never stopped. The order is G17's, which macOS's power words on T8132
//! agree with. The PMP vote stays through the sleep.
//!
//! Only a real wake restarts the firmware. A sleep refused after a stop leaves it stopped and
//! publication closed until the next real sleep, with the domain on.

use core::sync::atomic::Ordering;

use kernel::{
    bindings,
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, Delta},
    workqueue::{DelayedWork, Work},
};

use super::recovery::now_ns;
use super::regs::{self, ASC_RUN};
use super::{Firmware, Gpu, Shared};
use crate::driver::{AsahiPlatformDriverData, SleepOps};

/// Time work in flight gets to finish before the sleep is refused.
const DRAIN_NS: u64 = 3_000_000_000;
const DRAIN_TICK_MS: i64 = 2;

extern "C" {
    fn asahi_g16_tvm_enable(dev: *mut core::ffi::c_void) -> core::ffi::c_int;
    fn asahi_g16_commit_prepare(
        dev: *mut core::ffi::c_void,
        rtk: *mut bindings::apple_rtkit,
    ) -> *mut core::ffi::c_void;
    fn asahi_g17_commit_finish(commit: *mut core::ffi::c_void, saved: *mut u32) -> bool;
    fn asahi_g17_power_auto_off(dev: *mut core::ffi::c_void, saved: *mut u32) -> core::ffi::c_int;
    fn asahi_g17_power_lower(dev: *mut core::ffi::c_void) -> core::ffi::c_int;
    fn asahi_g17_power_raise(dev: *mut core::ffi::c_void, saved: u32) -> core::ffi::c_int;
    fn asahi_g17_power_auto_on(dev: *mut core::ffi::c_void, saved: u32) -> core::ffi::c_int;
}

impl Firmware {
    /// Nothing published, nothing waiting and no recovery under way.
    fn sleep_quiet(&mut self) -> Result<bool> {
        self.drain_rings()?;
        Ok(self.engine.flights.is_empty()
            && self.engine.pending.is_empty()
            && !self.engine.frozen
            && self.recovery_idle()?)
    }

    /// The run bit and the RTKit handshake, with the firmware's state as it was left.
    fn sleep_restart(&mut self) -> Result {
        self.session.rtkit.warm_restart()?;
        let control = self.session.regs.asc_control()?;
        self.session.regs.set_asc_control(control | ASC_RUN)?;
        regs::mb();
        self.session.rtkit.boot()
    }
}

impl Shared {
    fn lower_domain(&self) {
        let mut saved = 0;
        // SAFETY: `dev` is the live GPU device and `saved` is writable.
        let ret = unsafe { asahi_g17_power_auto_off(self.dev.as_raw().cast(), &mut saved) };
        if ret != 0 {
            dev_warn!(self.dev.as_ref(), "GPU domain, automatic off: {}\n", ret);
        }
        self.sleep_ps.store(saved, Ordering::Relaxed);
        // SAFETY: `dev` is the live GPU device.
        let ret = unsafe { asahi_g17_power_lower(self.dev.as_raw().cast()) };
        if ret != 0 {
            dev_warn!(self.dev.as_ref(), "GPU domain slow to go off: {}\n", ret);
        }
    }

    fn raise_domain(&self) -> Result {
        let saved = self.sleep_ps.load(Ordering::Relaxed);
        // SAFETY: `dev` is the live GPU device.
        kernel::error::to_result(unsafe { asahi_g16_tvm_enable(self.dev.as_raw().cast()) })?;
        // SAFETY: `dev` is the live GPU device.
        kernel::error::to_result(unsafe {
            asahi_g17_power_raise(self.dev.as_raw().cast(), saved)
        })?;
        // SAFETY: `dev` is the live GPU device.
        kernel::error::to_result(unsafe {
            asahi_g17_power_auto_on(self.dev.as_raw().cast(), saved)
        })
    }

    /// With the sleeping flag set, joins whatever may be reading the firmware's registers.
    fn join_workers(self: &Arc<Self>) {
        // Dropping the handle cancels an expiry that's still pending.
        drop(self.resnap_handle.lock().take());
        // SAFETY: An initialized, pinned delayed work of this Shared. A pending callback owned
        // one Arc that it can no longer reclaim. Later enqueues run into the sleeping check.
        let cancelled = unsafe {
            let work = Work::raw_get(DelayedWork::raw_as_work(&self.poll));
            let delayed = kernel::container_of!(work, bindings::delayed_work, work);
            bindings::cancel_delayed_work_sync(delayed)
        };
        if cancelled {
            // SAFETY: See above.
            drop(unsafe { Arc::from_raw(Arc::as_ptr(self)) });
        }
    }

    /// Waits for what's been published to retire. The poll work keeps running meanwhile.
    fn wait_quiet(self: &Arc<Self>, deadline: u64) -> Result {
        loop {
            self.check()?;
            let now = now_ns();
            {
                let mut state = self.state.lock();
                let firmware = state.as_deref_mut().ok_or(ENODEV)?;
                if firmware.sleep_quiet()? {
                    return Ok(());
                }
            }
            if now >= deadline {
                return Err(EBUSY);
            }
            self.poll_now();
            fsleep(Delta::from_millis(DRAIN_TICK_MS));
        }
    }

    /// Leaves the stop to the sleep's commit. False, with the reason logged, if it can't be.
    fn leave_for_commit(&self, firmware: &mut Firmware) -> bool {
        // SAFETY: `dev` is the live GPU device. The RTKit handle lasts until reboot, and
        // `.complete` frees the result.
        let commit = unsafe {
            asahi_g16_commit_prepare(self.dev.as_raw().cast(), firmware.session.rtkit.as_raw())
        };
        match kernel::error::from_err_ptr(commit) {
            Ok(commit) => {
                self.sleep_commit.store(commit, Ordering::Release);
                true
            }
            Err(error) => {
                dev_warn!(
                    self.dev.as_ref(),
                    "no GPU stop at the commit ({:?}): refusing the sleep\n",
                    error
                );
                false
            }
        }
    }

    /// Whether the commit stopped the firmware, if the stop was left to it.
    fn sleep_commit_finish(&self) -> Option<bool> {
        let commit = self.sleep_commit.swap(core::ptr::null_mut(), Ordering::AcqRel);
        if commit.is_null() {
            return None;
        }
        let mut saved = self.sleep_ps.load(Ordering::Relaxed);
        // SAFETY: `commit` came from `asahi_g16_commit_prepare` and is finished only once.
        let stopped = unsafe { asahi_g17_commit_finish(commit, &mut saved) };
        self.sleep_ps.store(saved, Ordering::Relaxed);
        Some(stopped)
    }

    /// Waits for quiet, closes publication and leaves the stop to the commit. Refuses if the
    /// commit can't take it: a stop here loses the GPU after a refused sleep. A refusal
    /// changes nothing.
    fn sleep_down(self: &Arc<Self>) -> Result {
        if self.sleeping.load(Ordering::Acquire) {
            // After a sleep refused past the stop, only the domain is left to lower.
            self.lower_domain();
            return Ok(());
        }
        if self.crashed.load(Ordering::Acquire) {
            // Nothing to save: the domain only has to be off for the sleep.
            self.sleeping.store(true, Ordering::Release);
            self.join_workers();
            self.lower_domain();
            return Ok(());
        }
        self.wait_quiet(now_ns().saturating_add(DRAIN_NS))?;
        self.sleeping.store(true, Ordering::Release);
        self.join_workers();
        let left = {
            let mut state = self.state.lock();
            match state.as_deref_mut() {
                Some(firmware) => firmware
                    .sleep_quiet()
                    .and_then(|quiet| if quiet { Ok(()) } else { Err(EBUSY) })
                    .and_then(|_| {
                        if self.leave_for_commit(firmware) {
                            Ok(())
                        } else {
                            Err(EBUSY)
                        }
                    }),
                None => Err(ENODEV),
            }
        };
        match left {
            Ok(()) => Ok(()),
            Err(error) => {
                self.sleeping.store(false, Ordering::Release);
                self.poll_now();
                Err(error)
            }
        }
    }

    /// Keeps the firmware stopped after a sleep that never reached the hardware. False if
    /// there's no domain to keep it in.
    fn sleep_hold(self: &Arc<Self>) -> bool {
        if self.crashed.load(Ordering::Acquire) || self.raise_domain().is_err() {
            return false;
        }
        dev_warn!(
            self.dev.as_ref(),
            "sleep refused: GPU firmware stays stopped until a real sleep\n"
        );
        true
    }

    /// Raises the domain and restarts the firmware. A failure loses the device.
    fn sleep_up(self: &Arc<Self>) -> Result {
        let started = now_ns();
        let result = {
            let mut state = self.state.lock();
            match state.as_deref_mut() {
                Some(firmware) if !self.crashed.load(Ordering::Acquire) => {
                    self.raise_domain().and_then(|_| firmware.sleep_restart())
                }
                _ => self.raise_domain(),
            }
        };
        self.sleeping.store(false, Ordering::Release);
        match result {
            Ok(()) => {
                dev_info!(
                    self.dev.as_ref(),
                    "GPU firmware back in {} us\n",
                    now_ns().saturating_sub(started) / 1000
                );
                self.poll_now();
                Ok(())
            }
            Err(error) => {
                dev_err!(self.dev.as_ref(), "GPU firmware did not come back: {:?}\n", error);
                self.fail_stop(error);
                Err(error)
            }
        }
    }
}

impl Gpu {
    /// Called before any device suspends. Only a sleep that powers the SoC down gets here.
    fn sleep_prepare(&self) -> Result {
        let result = self.shared.sleep_down();
        if let Err(error) = result {
            dev_warn!(self.shared.dev.as_ref(), "GPU refused sleep: {:?}\n", error);
        }
        result
    }

    /// Called after every device resumed, whether or not the sleep reached the hardware.
    fn sleep_complete(&self) {
        if !self.shared.sleeping.load(Ordering::Acquire) {
            return;
        }
        // SAFETY: Reads a global flag word.
        let real = unsafe { bindings::pm_resume_via_firmware() };
        if self.shared.sleep_commit_finish() == Some(false) && !real {
            dev_info!(
                self.shared.dev.as_ref(),
                "sleep refused before the commit: GPU firmware never stopped\n"
            );
            self.shared.sleeping.store(false, Ordering::Release);
            self.shared.poll_now();
            return;
        }
        if !real && self.shared.sleep_hold() {
            return;
        }
        let _ = self.shared.sleep_up();
    }
}

/// # Safety
///
/// `dev` must be a device this driver is bound to, as for a PM core callback.
unsafe fn sleep_gpu<'a>(dev: *mut bindings::device) -> Option<&'a Arc<Gpu>> {
    // SAFETY: Probe stored a `Pin<KBox<Option<AsahiPlatformDriverData>>>` here, and the PM core
    // keeps the device bound for the duration of a callback.
    let data = unsafe {
        &*bindings::dev_get_drvdata(dev).cast::<Option<AsahiPlatformDriverData<'a>>>()
    };
    data.as_ref()?.g16.as_ref()
}

/// Runs before any device suspends, so the GPU goes first and work still in flight can finish.
unsafe extern "C" fn sleep_prepare(dev: *mut bindings::device) -> core::ffi::c_int {
    // SAFETY: Reads a global flag word.
    if !unsafe { bindings::pm_suspend_via_firmware() } {
        return 0;
    }
    // SAFETY: Called by the PM core.
    match unsafe { sleep_gpu(dev) } {
        Some(gpu) => kernel::error::from_result(|| gpu.sleep_prepare().map(|_| 0)),
        None => EBUSY.to_errno(),
    }
}

/// Runs after every device resumed. Does nothing unless [`sleep_prepare`] closed publication.
unsafe extern "C" fn sleep_complete(dev: *mut bindings::device) {
    // SAFETY: Called by the PM core.
    if let Some(gpu) = unsafe { sleep_gpu(dev) } {
        gpu.sleep_complete();
    }
}

static SLEEP_OPS: SleepOps = SleepOps(bindings::dev_pm_ops {
    prepare: Some(sleep_prepare),
    complete: Some(sleep_complete),
    // SAFETY: All-zero is the absence of every other callback.
    ..unsafe { core::mem::zeroed() }
});

/// The G16 driver's sleep callbacks.
pub(super) fn ops() -> &'static bindings::dev_pm_ops {
    &SLEEP_OPS.0
}
