// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The T8122 (M3, G15G) start experiment, `asahi.t8122_start=1`.
//!
//! The T8122 table (`m3_soc::T8122`) lacks the InitData version of its firmware image, the
//! runtime IO maps, the SGX setup write, the runtime HwDataB words and unit masks, the runtime
//! allocation layout and a conservative performance-state ceiling, so the runtime refuses the
//! SoC. With `asahi.t8122_start=1`, on a T8122 whose GPU the boot loader handed over (resource
//! admission passed), the runtime starts it with the experiment values below instead. None of
//! them is validated on G15G. Each value that is not known is a parameter, so that another
//! value can be tried by rebooting:
//!
//! | Parameter | Default | Others |
//! |---|---|---|
//! | `asahi.t8122_initdata_version` | `0x0c08e21e83800490`, the G15 14.8.3 version | any |
//! | `asahi.t8122_fender` | `0x104000` (`rule`) | `0x12c000` (`adt`) |
//! | `asahi.t8122_clkgen` | `e1c`: SGX+0xe1c000, read-only | `e5c`: SGX+0xe5c000, read-only; `none` |
//! | `asahi.t8122_sgx_setup` | `none` | `t6030`: SGX+0xd14000 = 0x70001 |
//! | `asahi.t8122_unit_mask_a` | `0x700000001` | nonzero, within `0x700000001` |
//! | `asahi.t8122_unit_mask_b` | `0x3` | nonzero, within `0x7` |
//! | `asahi.t8122_pstate_cap` | `2` | 1 to 15 (at or above the table's highest state: all of it) |
//!
//! The other HwDataB words, the runtime allocation layout and the fixed control words are the
//! T6030 ones. A parameter given with a value it does not accept refuses the start. Without
//! `asahi.t8122_start=1` nothing here runs, and T8122 is refused as before. Nothing here applies
//! to any other SoC.

use core::mem::offset_of;

use kernel::{device, prelude::*};

use crate::{
    fw::initdata::raw,
    initdata::G15RuntimeHwDataB,
    m3_adt_config::T8122_IO_MAPPINGS,
    m3_init_storage::{self as storage, IoMap},
    m3_soc::{IoMapping, Soc, T6030_HWDATA_B},
    pgtable::{prot, Prot},
    t8122_knobs::{
        self as knobs, ClockGen, Start, Values, CLOCK_GEN_E1C, CLOCK_GEN_E5C, FENDER_ADT,
        FENDER_RULE,
    },
};

/// The GPU register window (SGX) of T8122.
const SGX: u64 = 0x2_9000_0000;
/// Index of the Fender window and of the GPU clock generator in [`T8122_IO_MAPPINGS`].
const FENDER: usize = 0;
const CLOCK_GEN: usize = 11;
/// The HwDataB slot of the GPU clock generator.
const CLOCK_GEN_SLOT: usize = 29;

/// The runtime HwDataB words of the experiment: T6030's, with the default unit masks of
/// `t8122_knobs`. The unit masks uploaded are the parameters' ([`Experiment::unit_masks`]).
pub(crate) static T8122_HWDATA_B: G15RuntimeHwDataB = G15RuntimeHwDataB {
    unit_mask_a: knobs::UNIT_MASK_A,
    unit_mask_b: knobs::UNIT_MASK_B,
    ..T6030_HWDATA_B
};

/// The firmware IO mappings with the Fender window `fender` and the clock generator at
/// SGX + `clock_gen`.
const fn mappings(fender: u32, clock_gen: u64) -> [IoMapping; 12] {
    let mut m = T8122_IO_MAPPINGS;
    m[FENDER].2 = fender;
    m[FENDER].3 = fender;
    m[CLOCK_GEN].1 = SGX + clock_gen;
    m
}

/// The runtime IO maps, laid out once for the larger Fender window so that no slot's firmware
/// VA depends on the parameters.
const LAYOUT: [IoMap; 12] =
    storage::pack_iomaps(&mappings(FENDER_ADT, CLOCK_GEN_E1C), storage::IOMAP_BASE);

const fn iomaps(fender: u32, clock_gen: u64) -> [IoMap; 12] {
    let mut m = LAYOUT;
    m[FENDER].size = fender as usize;
    m[CLOCK_GEN].physical = SGX + clock_gen;
    m
}

/// Every Fender window and clock-generator address, in [`Experiment::variant`] order.
static IO_MAPPINGS: [[IoMapping; 12]; 4] = [
    mappings(FENDER_RULE, CLOCK_GEN_E5C),
    mappings(FENDER_RULE, CLOCK_GEN_E1C),
    mappings(FENDER_ADT, CLOCK_GEN_E5C),
    mappings(FENDER_ADT, CLOCK_GEN_E1C),
];
static IOMAPS: [[IoMap; 12]; 4] = [
    iomaps(FENDER_RULE, CLOCK_GEN_E5C),
    iomaps(FENDER_RULE, CLOCK_GEN_E1C),
    iomaps(FENDER_ADT, CLOCK_GEN_E5C),
    iomaps(FENDER_ADT, CLOCK_GEN_E1C),
];

/// Whether each IO map holds its IO mapping: same slot, the mapping's 16 KiB-aligned physical
/// base, its subpage offset, and room for its total size.
const fn maps_cover(mappings: &[IoMapping; 12], maps: &[IoMap; 12]) -> bool {
    let mut i = 0;
    while i < 12 {
        let (slot, phys, total, _, _) = mappings[i];
        let io = maps[i];
        if io.slot != slot
            || io.physical + io.offset as u64 != phys
            || io.offset + total as usize > io.size
            || io.size % 0x4000 != 0
        {
            return false;
        }
        i += 1;
    }
    true
}

const _: () = {
    // The ported table: the Fender window at the rule size, the clock generator last, at
    // SGX+0xe1c000, 16 KiB, read-only.
    assert!(T8122_IO_MAPPINGS[FENDER].0 == 0 && T8122_IO_MAPPINGS[FENDER].1 == SGX + 0xd0_0000);
    assert!(T8122_IO_MAPPINGS[FENDER].2 == FENDER_RULE);
    assert!(T8122_IO_MAPPINGS[CLOCK_GEN].0 == CLOCK_GEN_SLOT);
    assert!(T8122_IO_MAPPINGS[CLOCK_GEN].1 == SGX + CLOCK_GEN_E1C);
    assert!(T8122_IO_MAPPINGS[CLOCK_GEN].2 == 0x4000 && !T8122_IO_MAPPINGS[CLOCK_GEN].4);
    assert!(CLOCK_GEN + 1 == T8122_IO_MAPPINGS.len());
    // Both windows are whole pages and hold the Fender scratch area (+0x60000..+0x80000).
    assert!(FENDER_RULE % 0x4000 == 0 && FENDER_ADT % 0x4000 == 0 && FENDER_RULE >= 0x8_0000);
    assert!(FENDER_RULE < FENDER_ADT);
    // Both clock-generator blocks are whole pages inside the first 16 MiB of SGX.
    assert!(CLOCK_GEN_E5C % 0x4000 == 0 && CLOCK_GEN_E1C % 0x4000 == 0);
    assert!(CLOCK_GEN_E5C + 0x4000 <= 0x100_0000 && CLOCK_GEN_E1C + 0x4000 <= 0x100_0000);
    let mut i = 0;
    while i < 4 {
        assert!(maps_cover(&IO_MAPPINGS[i], &IOMAPS[i]));
        i += 1;
    }
    // The unit-mask limits are T6030's masks, less the second cluster's bit of mask A.
    assert!(T6030_HWDATA_B.unit_mask_a == knobs::UNIT_MASK_A_LIMIT | 0x2);
    assert!(T6030_HWDATA_B.unit_mask_b as u64 == knobs::UNIT_MASK_B_LIMIT);
    assert!(offset_of!(raw::HwDataBG15V14_8_3, unit_mask_a) == 0x17c0);
    assert!(offset_of!(raw::HwDataBG15V14_8_3, unit_mask_b) == 0x17c8);
};

/// The values of one armed boot.
#[derive(Copy, Clone, Debug)]
pub(crate) struct Experiment(Values);

/// Whether `soc` is the T8122 table.
pub(crate) fn is_t8122(soc: &Soc) -> bool {
    core::ptr::eq(soc, &crate::m3_soc::T8122)
}

impl Experiment {
    /// Index of this boot's IO-mapping table in [`IO_MAPPINGS`] and [`IOMAPS`].
    fn variant(&self) -> usize {
        let adt = usize::from(self.0.fender == FENDER_ADT);
        let e1c = usize::from(self.0.clock_gen == ClockGen::At(CLOCK_GEN_E1C));
        2 * adt + e1c
    }

    /// How many IO mappings this boot gives the firmware: all, or all but the clock generator.
    fn mapping_count(&self) -> usize {
        match self.0.clock_gen {
            ClockGen::At(_) => CLOCK_GEN + 1,
            ClockGen::Absent => CLOCK_GEN,
        }
    }

    /// The InitData version given to the firmware (`asahi.t8122_initdata_version`).
    pub(crate) fn initdata_version(&self) -> u64 {
        self.0.initdata_version
    }

    /// The firmware IO mappings of the InitData.
    pub(crate) fn io_mappings(&self) -> &'static [IoMapping] {
        &IO_MAPPINGS[self.variant()][..self.mapping_count()]
    }

    /// The runtime's mapping of each of them.
    pub(crate) fn iomaps(&self) -> &'static [IoMap] {
        &IOMAPS[self.variant()][..self.mapping_count()]
    }

    /// The register block the IO mapping of `slot` must lie in, given the HwConfig's block
    /// `base` for it: the selected clock generator for slot 29.
    pub(crate) fn io_block(&self, slot: usize, base: u64) -> u64 {
        match self.0.clock_gen {
            ClockGen::At(offset) if slot == CLOCK_GEN_SLOT => SGX + offset,
            _ => base,
        }
    }

    /// The HwDataB slots the firmware maps read-only, as a bit mask: the clock generator's.
    pub(crate) fn read_only_slots(&self) -> u32 {
        match self.0.clock_gen {
            ClockGen::At(_) => 1 << CLOCK_GEN_SLOT,
            ClockGen::Absent => 0,
        }
    }

    /// The runtime HwDataB words. The unit masks are written over them ([`Self::unit_masks`]).
    pub(crate) fn hwdata_b(&self) -> &'static G15RuntimeHwDataB {
        &T8122_HWDATA_B
    }

    /// The HwDataB unit masks A (+0x17c0) and B (+0x17c8).
    pub(crate) fn unit_masks(&self) -> (u64, u32) {
        (self.0.unit_mask_a, self.0.unit_mask_b)
    }

    /// The SGX write (offset, value) made before the firmware starts, if any.
    pub(crate) fn sgx_setup(&self) -> Option<(usize, u32)> {
        self.0.sgx_setup
    }

    /// The highest performance state the firmware may use (`asahi.t8122_pstate_cap`).
    pub(crate) fn pstate_cap(&self) -> u32 {
        self.0.pstate_cap
    }
}

/// Firmware page protection of the IO map of `slot`, with the read-only slots `read_only`.
pub(crate) fn mmio_prot(read_only: u32, slot: usize) -> Prot {
    if slot < 32 && read_only & (1 << slot) != 0 {
        prot::PROT_FW_MMIO_RO
    } else {
        prot::PROT_FW_MMIO_RW
    }
}

/// Decide, after resource admission, whether `soc` starts with the experiment values.
///
/// Any SoC but T8122: Ok(None), silently. T8122 without `asahi.t8122_start=1`: Ok(None), so the
/// caller refuses it as before (one extra line when experiment parameters were given). T8122
/// with `asahi.t8122_start=1`: the experiment, after logging every value, or a refusal for a
/// parameter value it does not accept.
pub(crate) fn arm(dev: &device::Device, soc: &Soc) -> Result<Option<Experiment>> {
    if !is_t8122(soc) {
        return Ok(None);
    }
    let raw = crate::m3_params::t8122_params();
    match knobs::start(&raw) {
        Start::On => {}
        Start::Off => {
            if raw.values_given() {
                dev_info!(
                    dev,
                    "M3 G15G start: asahi.t8122_* values given without asahi.t8122_start=1; ignored\n"
                );
            }
            return Ok(None);
        }
        Start::Invalid => {
            dev_err!(
                dev,
                "M3 G15G start: refused: asahi.t8122_start has a value it does not accept (0 or 1); GPU startup disabled\n"
            );
            return Err(ENODEV);
        }
    }
    // Admission has required the boot loader's handoff: its firmware segments and reserved
    // regions, and on this SoC its power configuration.
    if !soc.power_from_boot_loader || soc.firmware.is_none() || soc.hwcfg.is_none() {
        dev_err!(dev, "M3 G15G start: refused: the T8122 table is not the one this experiment expects\n");
        return Err(ENODEV);
    }
    let v = knobs::resolve(&raw).map_err(|refusal| {
        dev_err!(
            dev,
            "M3 G15G start: refused: asahi.{} has a value it does not accept ({}); GPU startup disabled\n",
            refusal.name,
            refusal.accepts
        );
        ENODEV
    })?;
    let e = Experiment(v);
    dev_warn!(
        dev,
        "M3 G15G start: armed (asahi.t8122_start=1, boot loader handoff admitted): initdata_version={:#018x} fender={:#x} clkgen={} sgx_setup={} unit_mask_a={:#x} unit_mask_b={:#x} pstate_cap={}\n",
        v.initdata_version,
        v.fender,
        match v.clock_gen {
            ClockGen::At(CLOCK_GEN_E1C) => "e1c(sgx+0xe1c000,ro)",
            ClockGen::At(_) => "e5c(sgx+0xe5c000,ro)",
            ClockGen::Absent => "none",
        },
        match v.sgx_setup {
            Some(_) => "t6030(sgx+0xd14000=0x70001)",
            None => "none",
        },
        v.unit_mask_a,
        v.unit_mask_b,
        v.pstate_cap
    );
    dev_warn!(
        dev,
        "M3 G15G start: unvalidated on G15G: {} IO mappings at firmware VA {:#x}; the other HwDataB words, the runtime allocation layout and the fixed control words are T6030's\n",
        e.mapping_count(),
        storage::IOMAP_BASE
    );
    Ok(Some(e))
}

/// Log, in an armed start, that the probe stopped at `stage` before the firmware started.
pub(crate) fn refused(dev: &device::Device, experiment: Option<&Experiment>, stage: &str, error: Error) {
    if experiment.is_some() {
        dev_err!(
            dev,
            "M3 G15G start: refused at {} ({:?}); the firmware was not started, see the lines above\n",
            stage,
            error
        );
    }
}

/// Log, on a T8122 armed with `asahi.t8122_start=1`, that resource admission refused it.
pub(crate) fn not_admitted(dev: &device::Device, soc: &Soc) {
    if is_t8122(soc) && knobs::start(&crate::m3_params::t8122_params()) == Start::On {
        dev_err!(
            dev,
            "M3 G15G start: not armed: resource admission refused the GPU (see the lines above); the boot loader did not hand it over completely\n"
        );
    }
}

// One `M3 G15G verdict:` line per outcome of an armed start, so that a tester can tell them
// apart from the log alone. They run on T8122 only (callers check `is_t8122`); nothing here
// changes what the runtime does.

/// The GPU coprocessor's start or the driver's InitData upload failed (`m3_runtime::Runtime::new`).
/// `started`: the coprocessor ran and offered its endpoints.
pub(crate) fn prepare_verdict(dev: &device::Device, soc: &Soc, started: bool, error: Error) {
    if !is_t8122(soc) {
        return;
    }
    if started {
        dev_err!(
            dev,
            "M3 G15G verdict: driver-refused ({:?}): the GPU firmware is up, but the driver did not publish the InitData (UAT, upload checks or thermal setup, see above)\n",
            error
        );
    } else {
        dev_err!(
            dev,
            "M3 G15G verdict: firmware-boot-failed ({:?}): the GPU coprocessor did not start, or its RTKit did not offer endpoints 0x20/0x21; no InitData was published\n",
            error
        );
    }
}

/// The firmware's answer to the published InitData (`m3_runtime::Runtime::boot`). `accepted`:
/// the firmware wrote its ready words.
pub(crate) fn boot_verdict(
    dev: &device::Device,
    version: u64,
    accepted: bool,
    crashed: bool,
    result: Result,
) {
    match result {
        Ok(()) => dev_info!(
            dev,
            "M3 G15G verdict: firmware-running: the firmware accepted the InitData (version {:#x}) and the device controls; no job has run yet\n",
            version
        ),
        Err(e) if accepted => dev_err!(
            dev,
            "M3 G15G verdict: firmware-running-check-failed ({:?}): the firmware accepted the InitData (version {:#x}), then a check after boot failed (see above)\n",
            e,
            version
        ),
        Err(e) if crashed => dev_err!(
            dev,
            "M3 G15G verdict: initdata-rejected ({:?}): the firmware crashed after the InitData (version {:#x}) was published; see the crash lines above\n",
            e,
            version
        ),
        Err(e) => dev_err!(
            dev,
            "M3 G15G verdict: initdata-rejected ({:?}): the firmware did not accept the InitData (version {:#x}) within 2 s; see M3 firmware readiness above\n",
            e,
            version
        ),
    }
}

/// A job that failed (`m3_runtime::Inner::fail`): `primary` is ETIMEDOUT when it did not finish
/// within the runtime's 2 s bound; `pstate` is the GPU performance-state register, if readable.
pub(crate) fn job_failed_verdict(dev: &device::Device, primary: Error, pstate: Option<u32>, crashed: bool) {
    if primary != ETIMEDOUT || crashed {
        dev_err!(
            dev,
            "M3 G15G verdict: job-faulted ({:?}): the firmware crashed, reported an error, or the GPU reported a fault while the job ran; see the lines around this one\n",
            primary
        );
        return;
    }
    match pstate {
        Some(p) if p & 0xf == 0 => dev_err!(
            dev,
            "M3 G15G verdict: job-accepted-never-dispatched: the job did not finish within 2 s and the GPU reads powered down (pstate register {:#x})\n",
            p
        ),
        Some(p) => dev_err!(
            dev,
            "M3 G15G verdict: job-timed-out-powered: the job did not finish within 2 s with the GPU powered (pstate register {:#x}); see the engine snapshot below\n",
            p
        ),
        None => dev_err!(
            dev,
            "M3 G15G verdict: job-timed-out: the job did not finish within 2 s (pstate register unreadable)\n"
        ),
    }
}

/// The first job of a kind (0 render, 1 compute) retired (`m3_runtime::Runtime::execute`):
/// `span` is its first start and last end GPU timestamp (24 MHz ticks), if readable.
pub(crate) fn job_completed_verdict(dev: &device::Device, kind: usize, gpu_ns: u64, span: Option<[u64; 2]>) {
    let what = if kind == 0 { "render" } else { "compute" };
    let [start, end] = span.unwrap_or([0, 0]);
    if start != 0 && end != 0 {
        dev_info!(
            dev,
            "M3 G15G verdict: job-completed: the first {} job finished, GPU timestamps {:#x}..{:#x}, {} ns of GPU time\n",
            what,
            start,
            end,
            gpu_ns
        );
    } else {
        dev_err!(
            dev,
            "M3 G15G verdict: job-retired-without-timestamps: the first {} job retired, but its GPU timestamps are {:#x}..{:#x}\n",
            what,
            start,
            end
        );
    }
}
