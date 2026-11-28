// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! PMP-owned cooling requests, retained-session performance ceiling and the
//! utilization governor.
//!
//! The firmware owns voltage and clock transitions through PMP. The host
//! consumes PMP-owner telemetry and publishes only three Globals selectors,
//! in hundredths of a performance state. Firmware power tables and policy
//! words from other GPU generations are never modified here.
//!
//! The firmware runs at the published state, busy or not, so the host requests
//! one from the GPU's busy share of each window, bounded by the ceiling.
//!
//! The firmware keeps GFX powered while the GPU idles, so the host reports
//! when it has work in flight to the DT-selected CLPC, which holds the memory
//! and fabric floor for that work.

pub(crate) mod policy;

use core::{ffi::c_void, ptr::NonNull};
use kernel::{device, prelude::*, sync::aref::ARef};

use super::fw::initdata;
use super::recovery::now_ns;
use super::regs::wmb;
use super::Firmware;
use policy::{COOLING_MAX, TOP_DEFAULT};

// The three Globals p-state selectors: 0x88, 0x8c and 0x98 in 25G83; four bytes later in
// 26B123, which inserted a word at 0x38.
static_assert!(initdata::V25G83.pstate_selectors[0] == 0x88);
static_assert!(initdata::V25G83.pstate_selectors[1] == 0x8c);
static_assert!(initdata::V25G83.pstate_selectors[2] == 0x98);
static_assert!(initdata::V26B123.pstate_selectors[0] == 0x8c);
static_assert!(initdata::V26B123.pstate_selectors[1] == 0x90);
static_assert!(initdata::V26B123.pstate_selectors[2] == 0x9c);

extern "C" {
    fn asahi_g16_clpc_gpu_get(dev: *mut c_void, out: *mut *mut c_void) -> core::ffi::c_int;
    fn asahi_g16_clpc_gpu_set_active(gpu: *mut c_void, active: bool);
    fn asahi_g16_clpc_gpu_set_busy(gpu: *mut c_void, permille: core::ffi::c_uint);
    fn asahi_g16_clpc_gpu_clear_busy(gpu: *mut c_void);
    fn asahi_g16_clpc_gpu_put(gpu: *mut c_void);
    fn asahi_g16_cooling_register(dev: *mut c_void) -> *mut c_void;
    fn asahi_g16_cooling_unregister(cooling: *mut c_void);
    fn asahi_g16_cooling_has_pmp(cooling: *const c_void) -> bool;
    fn asahi_g16_cooling_sample(
        cooling: *mut c_void,
        now: u64,
        idle: bool,
        temperature: *mut core::ffi::c_long,
        governor: *mut u32,
        power: *mut u32,
        backstop: *mut u32,
        zone_seq: *mut u64,
        release_mc: *mut core::ffi::c_long,
        hot_mc: *mut core::ffi::c_long,
        critical_mc: *mut core::ffi::c_long,
        limits_valid: *mut bool,
    ) -> i32;
}

/// Owns the cooling-device registration and thermal telemetry consumer (PMP when `apple,pmp`
/// is present, or SoC die thermal zones otherwise). The C helpers only bridge the provider and
/// atomic governor requests.
struct Cooling {
    inner: NonNull<c_void>,
    is_pmp: bool,
    _dev: ARef<device::Device>,
}

// SAFETY: C callbacks and readers use atomics. Unregistration requires unique
// ownership in Drop and drains the thermal callbacks before freeing their data.
unsafe impl Send for Cooling {}
// SAFETY: Shared access cannot mutate the C policy. Sampling requires exclusive
// access; the independently running thermal callbacks only update an atomic.
unsafe impl Sync for Cooling {}

impl Cooling {
    fn new(dev: &device::Device) -> Result<Self> {
        // SAFETY: `dev` is valid and the returned allocation is owned until Drop.
        let inner = kernel::error::from_err_ptr(unsafe {
            asahi_g16_cooling_register(dev.as_raw().cast())
        })?;
        let inner = NonNull::new(inner).ok_or(ENOMEM)?;
        // SAFETY: `inner` is the live cooling object returned above.
        let is_pmp = unsafe { asahi_g16_cooling_has_pmp(inner.as_ptr()) };
        Ok(Self {
            inner,
            is_pmp,
            _dev: dev.into(),
        })
    }

    fn sample(&mut self, now: u64, idle: bool, power: &mut Sample) -> Result {
        let mut mc: core::ffi::c_long = 0;
        let mut seq: u64 = 0;
        let mut release_mc: core::ffi::c_long = 0;
        let mut hot_mc: core::ffi::c_long = 0;
        let mut critical_mc: core::ffi::c_long = 0;
        let mut limits_valid = false;
        // SAFETY: The C allocation and PMP phandle outlive this call, all output
        // pointers are valid, and the caller holds the GPU device mutex.
        let ret = unsafe {
            asahi_g16_cooling_sample(
                self.inner.as_ptr(),
                now,
                idle,
                &mut mc,
                &mut power.governor,
                &mut power.budget,
                &mut power.backstop,
                &mut seq,
                &mut release_mc,
                &mut hot_mc,
                &mut critical_mc,
                &mut limits_valid,
            )
        };
        power.temperature_mc = mc as i64;
        power.zone_seq = seq;
        power.zone_limits = limits_valid.then_some(policy::ZoneLimits {
            release_mc: release_mc as i64,
            hot_mc: hot_mc as i64,
            critical_mc: critical_mc as i64,
        });
        kernel::error::to_result(ret)
    }
}

impl Drop for Cooling {
    fn drop(&mut self) {
        // SAFETY: This is the unique owner; the helpers drain callbacks first.
        unsafe { asahi_g16_cooling_unregister(self.inner.as_ptr()) };
    }
}

/// PMP-provider observations, cached between policy samples. No pointer into
/// the provider or its removable thermal policy is retained here.
struct Sample {
    temperature_mc: i64,
    governor: u32,
    budget: u32,
    backstop: u32,
    zone_seq: u64,
    zone_limits: Option<policy::ZoneLimits>,
}

/// DT-selected memory-demand provider (`apple,clpc`), or none. Its
/// independent allocation outlives a detached provider; a device link orders
/// supplier removal and suspend.
struct Demand(*mut c_void);

// SAFETY: the provider's leaf spinlock serializes all access; no GPU callback.
unsafe impl Send for Demand {}
// SAFETY: shared access only changes that locked request, never its lifetime.
unsafe impl Sync for Demand {}

impl Demand {
    fn new(dev: &device::Device) -> Result<Self> {
        let mut out = core::ptr::null_mut();
        // SAFETY: live platform device and writable result; C returns an owned
        // reference, or NULL without an `apple,clpc` provider.
        let ret = unsafe { asahi_g16_clpc_gpu_get(dev.as_raw().cast(), &mut out) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(Self(out))
        }
    }

    fn set_active(&self, active: bool) {
        // SAFETY: the retained reference is live; NULL denotes no provider.
        unsafe { asahi_g16_clpc_gpu_set_active(self.0, active) };
    }

    fn set_busy(&self, permille: u32) {
        // SAFETY: the retained reference is live; NULL denotes no provider.
        unsafe { asahi_g16_clpc_gpu_set_busy(self.0, permille.min(1000) as core::ffi::c_uint) };
    }

    fn clear_busy(&self) {
        // SAFETY: the retained reference is live; NULL denotes no provider.
        unsafe { asahi_g16_clpc_gpu_clear_busy(self.0) };
    }
}

impl Drop for Demand {
    fn drop(&mut self) {
        // SAFETY: releases exactly the reference returned by get, clearing
        // the demand first.
        unsafe { asahi_g16_clpc_gpu_put(self.0) };
    }
}

/// All fields are protected by the device mutex. Before the first valid
/// provider sample the shared cooling policy keeps the ceiling at S1.
pub(crate) struct Power {
    clpc: Demand,
    /// Work-in-flight state and 2 ms idle-hold hysteresis reported to `clpc`.
    clpc_hold: policy::ClpcHold,
    /// Per-window GPU busy share reported to `clpc` for DT memory-state ladders.
    clpc_busy: policy::ClpcBusyShare,
    cooling: Cooling,
    /// Fail-closed thermal-zone backstop used when the GPU DT node has no `apple,pmp`.
    zone_backstop: policy::ZoneBackstop,
    sample: Sample,
    last_sample_ns: Option<u64>,
    /// The board's top state: S9 on the ten-row tables, S10 on the J773g table.
    pub(crate) top: u32,
    pub(crate) configured: u32,
    pub(crate) user: u32,
    pub(crate) effective: u32,
    pub(crate) temperature_mc: i64,
    pub(crate) governor: u32,
    pub(crate) budget: u32,
    sensor_error: Option<Error>,
    pub(crate) updates: u64,
    pub(crate) sensor_errors: u64,
    pub(crate) decisions: u64,
    last_decision: Option<(u32, u32, u32, u32, Option<Error>)>,
    pub(crate) utilization: policy::Governor,
}

impl Power {
    /// The Main image initializes every active state of the board's table. Initdata publishes
    /// startup S1 before INIT; this object starts with that same effective state until live
    /// thermal telemetry confirms a safe ceiling.
    pub(crate) fn new(dev: &device::Device) -> Result<Self> {
        // First: a CLPC that has not probed yet defers before the PMP vote.
        let clpc = Demand::new(dev)?;
        let cooling = Cooling::new(dev)?;
        let pmp_initial = if cooling.is_pmp { COOLING_MAX } else { 0 };
        Ok(Self {
            clpc,
            clpc_hold: policy::ClpcHold::new(),
            clpc_busy: policy::ClpcBusyShare::new(),
            cooling,
            zone_backstop: policy::ZoneBackstop::new(),
            sample: Sample {
                temperature_mc: 0,
                governor: pmp_initial,
                budget: pmp_initial,
                backstop: COOLING_MAX,
                zone_seq: 0,
                zone_limits: None,
            },
            last_sample_ns: None,
            top: TOP_DEFAULT,
            configured: TOP_DEFAULT,
            user: TOP_DEFAULT,
            effective: 1,
            temperature_mc: 0,
            governor: pmp_initial,
            budget: pmp_initial,
            sensor_error: None,
            updates: 0,
            sensor_errors: 0,
            decisions: 0,
            last_decision: None,
            utilization: policy::Governor::new(),
        })
    }

    /// Adopts the board's firmware table, `rows` rows including off, before the first
    /// governed publication: its top row is the governor's top state and configured
    /// ceiling. The J773g table has eleven rows, so its top state is S10.
    pub(crate) fn set_table(&mut self, rows: u32) {
        let top = policy::top_of(rows);
        self.top = top;
        self.configured = top;
        self.user = top;
        self.utilization.top = top;
    }

    fn apply_clpc_busy(&self, action: policy::ClpcBusyAction) {
        match action {
            policy::ClpcBusyAction::Set(permille) => self.clpc.set_busy(permille),
            policy::ClpcBusyAction::Clear => self.clpc.clear_busy(),
        }
    }

    fn resume_clpc_busy(&mut self, now: u64) {
        if self.clpc_busy.resume(now) {
            if self.clpc_hold.release_now() {
                self.clpc.set_active(false);
            }
            self.clpc.clear_busy();
        }
    }

    /// Records GPU publication at `now` for CLPC busy-share tracking.
    pub(crate) fn note_clpc_busy(&mut self, now: u64) {
        self.resume_clpc_busy(now);
        if let Some(permille) = self.clpc_busy.busy(now) {
            self.clpc.set_busy(permille);
        }
        if let Some(action) = self.clpc_busy.sample(now, true) {
            self.apply_clpc_busy(action);
        }
    }

    /// Records GPU drain at `now` for CLPC busy-share tracking.
    pub(crate) fn note_clpc_idle(&mut self, now: u64) {
        self.clpc_busy.idle(now);
    }

    /// Reports a change of work in flight to the CLPC: the memory and fabric
    /// floor follows it, held for [`policy::CLPC_IDLE_HOLD_NS`] after drain.
    pub(crate) fn report_work(&mut self, in_flight: bool) {
        let now = now_ns();
        if in_flight {
            self.resume_clpc_busy(now);
            if let Some(action) = self.clpc_busy.sample(now, true) {
                self.apply_clpc_busy(action);
            }
        }
        if let Some(active) = self.clpc_hold.report(in_flight, now) {
            self.clpc.set_active(active);
            if !active {
                self.clpc_busy.note_inactive(now);
            }
        }
    }

    /// Immediately drops the CLPC memory and fabric floor (fail-stop or sleep).
    pub(crate) fn release_clpc_now(&mut self) {
        self.clpc_busy.clear_active();
        if self.clpc_hold.release_now() {
            self.clpc.set_active(false);
        }
        self.clpc.clear_busy();
    }

    /// Whether the CLPC floor is held across an idle gap awaiting expiry.
    pub(crate) fn clpc_holding(&self) -> bool {
        self.clpc_hold.holding()
    }

    /// Whether an active burst below the current ceiling can ramp early.
    pub(crate) fn needs_ramp(&self, now: u64) -> bool {
        let level = self.governor.max(self.budget).max(self.sample.backstop);
        let ceiling = policy::cooling_ceiling(self.top, level);
        let limit = policy::limit(self.configured, ceiling, self.user);
        self.utilization.ramp_due(now, limit)
    }

    fn ceiling(&mut self, now: u64, idle: bool) -> (u32, bool) {
        if policy::read_due(self.last_sample_ns, now) {
            self.last_sample_ns = Some(now);
            self.sensor_error = self.cooling.sample(now, idle, &mut self.sample).err();
            if self.sensor_error.is_some() {
                self.sensor_errors += 1;
            }
            if !self.cooling.is_pmp {
                self.zone_backstop.sample(
                    now,
                    self.sample.zone_seq,
                    self.sample.temperature_mc,
                    self.sample.zone_limits,
                    self.sensor_error.is_some(),
                );
                self.zone_backstop.observe(now, self.sample.governor);
                self.sample.backstop = self.zone_backstop.level();
            }
            self.temperature_mc = self.sample.temperature_mc;
            self.governor = self.sample.governor;
            self.budget = self.sample.budget;
        }
        let level = self.governor.max(self.budget).max(self.sample.backstop);
        let ceiling = policy::cooling_ceiling(self.top, level);
        let limit = policy::limit(self.configured, ceiling, self.user);
        let decision = (
            limit,
            self.governor,
            self.budget,
            self.sample.backstop,
            self.sensor_error,
        );
        let changed = self.last_decision != Some(decision);
        if changed {
            self.decisions += 1;
            self.last_decision = Some(decision);
        }
        (limit, changed)
    }
}

impl Firmware {
    /// Runs after firmware acceptance, before publication and while polling
    /// active work. Provider, governor and budget reads occur at most every
    /// 5 ms, independently of completion polling. Only the three
    /// host-owned Globals ceiling selectors are written, ordered before any
    /// later queue or doorbell publication.
    pub(crate) fn service_power(&mut self) -> Result {
        if let Some(error) = self.recovery.failed() {
            return Err(error);
        }
        let now = now_ns();
        let idle = self.engine.flights.is_empty();
        if idle && self.power.clpc_hold.expire(now) {
            self.power.clpc.set_active(false);
            self.power.clpc_busy.note_inactive(now);
        }
        if let Some(action) = self.power.clpc_busy.sample(now, !idle) {
            self.power.apply_clpc_busy(action);
        }
        let (limit, decision) = self.power.ceiling(now, idle);
        let target = self
            .power
            .utilization
            .govern(now, limit, !self.engine.flights.is_empty());
        if decision {
            dev_info!(
                self.dev(),
                "M4 cooling: sample_ns={:?} mc={} sensor={:?} governor={} power={} backstop={} limit=S{}\n",
                self.power.last_sample_ns,
                self.power.temperature_mc,
                self.power.sensor_error,
                self.power.governor,
                self.power.budget,
                self.power.sample.backstop,
                limit
            );
        }
        if target == self.power.effective {
            return Ok(());
        }
        let globals = self.init.globals.as_ref().ok_or(ENODEV)?;
        for offset in self.init.layout.pstate_selectors {
            globals.w32(offset, target * 100)?;
        }
        wmb();
        self.power.effective = target;
        self.power.updates += 1;
        Ok(())
    }
}
