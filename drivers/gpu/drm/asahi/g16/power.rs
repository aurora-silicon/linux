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

pub(crate) mod policy;

use core::{ffi::c_void, ptr::NonNull};
use kernel::{device, prelude::*, sync::aref::ARef};

use super::fw::initdata::globals;
use super::recovery::now_ns;
use super::regs::wmb;
use super::Firmware;
use policy::{COOLING_MAX, PSTATE_MAX};

static_assert!(globals::PSTATE_SELECTORS[0] == 0x88);
static_assert!(globals::PSTATE_SELECTORS[1] == 0x8c);
static_assert!(globals::PSTATE_SELECTORS[2] == 0x98);

extern "C" {
    fn asahi_g16_cooling_register(dev: *mut c_void) -> *mut c_void;
    fn asahi_g16_cooling_unregister(cooling: *mut c_void);
    fn asahi_g16_cooling_sample(
        cooling: *mut c_void,
        now: u64,
        temperature: *mut core::ffi::c_long,
        governor: *mut u32,
        power: *mut u32,
        backstop: *mut u32,
    ) -> i32;
}

/// Owns the cooling-device registration and PMP consumer. The C helpers only
/// bridge the PMP provider and shared cooling policy, with atomic governor requests.
struct Cooling {
    inner: NonNull<c_void>,
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
        Ok(Self {
            inner: NonNull::new(inner).ok_or(ENOMEM)?,
            _dev: dev.into(),
        })
    }

    fn sample(&mut self, now: u64, power: &mut Sample) -> Result {
        let mut mc: core::ffi::c_long = 0;
        // SAFETY: The C allocation and PMP phandle outlive this call, all output
        // pointers are valid, and the caller holds the GPU device mutex.
        let ret = unsafe {
            asahi_g16_cooling_sample(
                self.inner.as_ptr(),
                now,
                &mut mc,
                &mut power.governor,
                &mut power.budget,
                &mut power.backstop,
            )
        };
        power.temperature_mc = mc as i64;
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
}

/// All fields are protected by the device mutex. Before the first valid
/// provider sample the shared cooling policy keeps the ceiling at S1.
pub(crate) struct Power {
    cooling: Cooling,
    sample: Sample,
    last_sample_ns: Option<u64>,
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
    /// The Main image initializes all nine active states. Initdata publishes
    /// startup S1 before INIT; this object starts with that same effective state.
    pub(crate) fn new(dev: &device::Device) -> Result<Self> {
        Ok(Self {
            cooling: Cooling::new(dev)?,
            sample: Sample {
                temperature_mc: 0,
                governor: COOLING_MAX,
                budget: COOLING_MAX,
                backstop: COOLING_MAX,
            },
            last_sample_ns: None,
            configured: PSTATE_MAX,
            user: PSTATE_MAX,
            effective: 1,
            temperature_mc: 0,
            governor: COOLING_MAX,
            budget: COOLING_MAX,
            sensor_error: None,
            updates: 0,
            sensor_errors: 0,
            decisions: 0,
            last_decision: None,
            utilization: policy::Governor::new(),
        })
    }

    fn ceiling(&mut self, now: u64) -> (u32, bool) {
        if policy::read_due(self.last_sample_ns, now) {
            self.last_sample_ns = Some(now);
            self.sensor_error = self.cooling.sample(now, &mut self.sample).err();
            if self.sensor_error.is_some() {
                self.sensor_errors += 1;
            }
            self.temperature_mc = self.sample.temperature_mc;
            self.governor = self.sample.governor;
            self.budget = self.sample.budget;
        }
        let level = self.governor.max(self.budget).max(self.sample.backstop);
        let ceiling = PSTATE_MAX - level.min(COOLING_MAX);
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
        let (limit, decision) = self.power.ceiling(now);
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
        for offset in globals::PSTATE_SELECTORS {
            globals.w32(offset, target * 100)?;
        }
        wmb();
        self.power.effective = target;
        self.power.updates += 1;
        Ok(())
    }
}
