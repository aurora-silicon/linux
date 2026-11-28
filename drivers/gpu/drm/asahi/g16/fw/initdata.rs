// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! InitData for the qualified 25G83 and 26B123 classic GPU firmware.
//!
//! The encoder is written in 25G83 offsets; a [`Layout`] maps them to another firmware ABI
//! (26B123 inserts and removes words in several objects, renumbers the register map and
//! moves the FirmwareData words). This is a byte encoder for private, unpublished objects.
//! Admission, mapping, cache maintenance and publication belong to the owner. It has no kernel
//! or allocator dependency so a host harness can compare every byte with the C bring-up
//! encoder.

pub(crate) const PAGE: u64 = 0x4000;
pub(crate) const PA_MASK: u64 = 0x3ffffffc000;
pub(crate) const HOST_START: u64 = 0xfffffc2000000000;
pub(crate) const HOST_END: u64 = 0xfffffc2ffc000000;
pub(crate) const BOOTSTRAP_START: u64 = 0x7000000000;
pub(crate) const BOOTSTRAP_SIZE: u64 = 0x2000000;
pub(crate) const OBJECT_COUNT: usize = 31;
pub(crate) const LOADER_COUNT: usize = 42;
pub(crate) const LOADER_STRIDE: usize = 0x28;

/// Stable positions in the firmware's object inventory.
pub(crate) mod id {
    pub(crate) const ROOT: usize = 0;
    pub(crate) const BRN: usize = 1;
    pub(crate) const RUNTIME: usize = 2;
    pub(crate) const GLOBALS: usize = 3;
    pub(crate) const CONTROL: usize = 4;
    pub(crate) const FIRMWARE_DATA: usize = 5;
    pub(crate) const POWER: usize = 6;
    pub(crate) const DYNAMIC: usize = 7;
    pub(crate) const MAIN: usize = 8;
    pub(crate) const POWER_PERFORMANCE: usize = 9;
    pub(crate) const TIMER: usize = 10;
    pub(crate) const ACTIVITY_TILER: usize = 11;
    pub(crate) const ACTIVITY_FRAGMENT: usize = 12;
    pub(crate) const ACTIVITY_COMPUTE: usize = 13;
    pub(crate) const ACTIVITY_RECOVERY: usize = 14;
    pub(crate) const LOG_STATE: usize = 15;
    pub(crate) const LOG_ENTRIES: usize = 16;
    pub(crate) const LOG_DATA: usize = 17;
    pub(crate) const FWCTL: usize = 18;
    pub(crate) const DEVCTRL_STATE: usize = 19;
    pub(crate) const DEVCTRL_RING: usize = 20;
    pub(crate) const KTRACE_STATE: usize = 21;
    pub(crate) const KTRACE_DATA: usize = 22;
    pub(crate) const STATISTICS_STATE: usize = 23;
    pub(crate) const STATISTICS_DATA: usize = 24;
    pub(crate) const TEMPERATURE: usize = 25;
    pub(crate) const EVENT_STATE: usize = 26;
    pub(crate) const EVENT_DATA: usize = 27;
    pub(crate) const RETENTION: usize = 28;
    pub(crate) const UMA: usize = 29;
    pub(crate) const PARAMETER: usize = 30;
}

pub(crate) const OBJECT_SIZE: [usize; OBJECT_COUNT] = [
    0xc8, 0x4000, 0x4b8, 0xe48, 0x20, 0xeaf0, 0x670, 0x4000, 0x2658, 0x6578, 0x88, 0xc18, 0x1248,
    0xe10, 0x60, 0x1b0, 0x28800, 0x79800, 0x4000, 0x30, 0x4000, 0x30, 0x9000, 0x30, 0x4800, 0x4,
    0x30, 0x4800, 0x4000, 0x4000, 0x4000,
];

pub(crate) mod root {
    pub(crate) const SIZE: usize = super::OBJECT_SIZE[super::id::ROOT];
}
/// The small root.Control object is the READY acknowledgement.
pub(crate) mod ready_control {
    pub(crate) const READY: usize = 0x14;
}

/// One run of a piecewise offset map: the 25G83 offsets `start..end` of an object are at
/// `+ delta` in another firmware ABI.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Run {
    pub(crate) start: usize,
    pub(crate) end: usize,
    pub(crate) delta: usize,
}

const fn run(start: usize, end: usize, delta: usize) -> Run {
    Run { start, end, delta }
}

/// Const counterpart of [`remap`] for compile-time layout validation.
const fn remap_const(runs: &[Run], at: usize, len: usize) -> Option<usize> {
    let Some(end) = at.checked_add(len) else {
        return None;
    };
    let mut i = 0;
    while i < runs.len() {
        if runs[i].start <= at && end <= runs[i].end {
            return at.checked_add(runs[i].delta);
        }
        i += 1;
    }
    None
}

/// Where the `len`-byte field at 25G83 offset `at` lives under `runs`. A field outside every
/// run (an offset whose new position is not known, or the register map) is refused.
pub(crate) fn remap(runs: &[Run], at: usize, len: usize) -> Result<usize, ()> {
    remap_const(runs, at, len).ok_or(())
}

/// The FirmwareData (status B) words the host reads and writes.
#[derive(Clone, Copy, Debug)]
pub(crate) struct FirmwareData {
    /// u64 VAs of the FWCTL state and, 8 bytes later, its ring (state + 0x40).
    pub(crate) fwctl: usize,
    /// u64 recovery generation.
    pub(crate) generation: usize,
    /// u32 recovery phase.
    pub(crate) phase: usize,
    /// u32 scheduler import word.
    pub(crate) scheduler: usize,
    /// The affected-work snapshot: `snapshot_entries` `{flags u32, target u32}` pairs.
    pub(crate) snapshot: usize,
    pub(crate) snapshot_entries: usize,
}

/// Pointer geometry of the twelve TX channels and device-control channel.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum TxLayout {
    /// Three state pointers spaced by 0x10 and a separate record buffer.
    StateBlock,
    /// Read and completion pointers spaced by eight. For the TX channels the
    /// producer is the eight-byte header before the records.
    RingHeader,
}

impl TxLayout {
    pub(crate) const fn completion(self) -> usize {
        match self {
            Self::StateBlock => 0x10,
            Self::RingHeader => 8,
        }
    }

    pub(crate) const fn ring_header(self, control: bool) -> usize {
        match self {
            Self::RingHeader if !control => 8,
            _ => 0,
        }
    }
}

/// The InitData ABI of one firmware build: what differs from the 25G83 objects the encoder
/// writes. Offsets of every run are 25G83 offsets.
pub(crate) struct Layout {
    pub(crate) name: &'static str,
    /// Root +0: the interface version the firmware compares with its own.
    pub(crate) version: u64,
    pub(crate) object_size: [usize; OBJECT_COUNT],
    /// RUNTIME (main config) offset of the unaligned power-config (POWER_PERFORMANCE) pointer.
    pub(crate) power_config: usize,
    pub(crate) tx: TxLayout,
    /// Register-map slot in MAIN of each logical (25G83-numbered) loader record; `None` for a
    /// record that is empty, and the number of slots of the map at MAIN + 0x640.
    pub(crate) loader_slot: [Option<u8>; LOADER_COUNT],
    pub(crate) register_slots: usize,
    pub(crate) loaders: &'static [Loader; LOADER_COUNT],
    /// Slots with an empty address and length but a default write-enabled mode.
    pub(crate) empty_loader_modes: u64,
    /// Physical core slots in MAIN where the firmware expects the topology count.
    pub(crate) main_core_slots: Option<u32>,
    /// MAIN (HW data) after the register map, GLOBALS (region C) and POWER_PERFORMANCE (power
    /// config). Only runs whose new position is known are listed.
    pub(crate) main: &'static [Run],
    pub(crate) globals: &'static [Run],
    pub(crate) power_performance: &'static [Run],
    pub(crate) firmware_data: FirmwareData,
    /// GLOBALS p-state selectors the host publishes at runtime (hundredths of a state).
    pub(crate) pstate_selectors: [usize; 3],
    /// MAIN offset of the u64 size of the host's crash copy buffer, where the build has one.
    pub(crate) coredump_copy: Option<usize>,
    /// Where the RETENTION object is published.
    pub(crate) retention: Retention,
    /// MAIN (HW data) words of this build that have no 25G83 position, at their own offsets.
    pub(crate) main_policy: &'static [(usize, u32)],
    /// GLOBALS (region C) words of this build that have no 25G83 position, at their own offsets.
    pub(crate) globals_policy: &'static [(usize, u32)],
    /// POWER_PERFORMANCE (power config) words of this build that have no 25G83 position, at
    /// their own offsets.
    pub(crate) power_policy: &'static [(usize, u32)],
    /// POWER_PERFORMANCE words derived from the GPU power sample period and average-power
    /// filter time constant, where the build has them.
    pub(crate) power_sampling: Option<PowerSampling>,
    /// POWER_PERFORMANCE words of the low-peak (PPM) power limiter derived from the machine's
    /// ADT (`clpc` low-peak limiter properties and `sgx` sample period), where the build has
    /// them.
    pub(crate) power_limiter: Option<PowerLimiter>,
    /// POWER_PERFORMANCE u32 that must lie above every performance-state index, where the build
    /// has one: the number of performance rows is written there.
    pub(crate) state_bound: Option<usize>,
}

/// POWER_PERFORMANCE offsets of the words derived from the GPU power sample period and
/// average-power filter time constant.
#[derive(Clone, Copy, Debug)]
pub(crate) struct PowerSampling {
    /// The exact sample period in us (from `gpu-pwr-sample-period-aic-clks`), as u32.
    pub(crate) period: &'static [usize],
    /// The exact sample period in us, as f32.
    pub(crate) period_f32: usize,
    /// The average-power filter of `n` samples over the ADT's `cpu-avg-limiter-input-tc`
    /// (`(tc_ms * 1000) / period_us`): u32 `n * 4`, f32 `(n - 1) / n` and f32 `1 / n`.
    pub(crate) filter: [usize; 3],
}

/// POWER_PERFORMANCE offsets of the low-peak (PPM) power limiter words derived from the
/// machine's ADT (`clpc` `cpu-lowpeak-limiter-*` and `sgx` `gpu-power-sample-period`).
#[derive(Clone, Copy, Debug)]
pub(crate) struct PowerLimiter {
    /// Filter time constant in coarse sample periods, scaled by 4 (`periods * 4`).
    pub(crate) filter_periods_x4: usize,
    /// IIR filter pole `(periods - 1) / periods`, as f32.
    pub(crate) filter_a_neg: usize,
    /// IIR filter weight `1 / periods`, as f32.
    pub(crate) filter_a: usize,
    /// Integral gain scaled by the coarse sample period (`ki * period_s`), as f32.
    pub(crate) ki_dt: usize,
    /// PI integrator upper bound (`65536.0`, the fixed-point scale shared across M1-M5), as f32.
    pub(crate) clamp: usize,
    /// Proportional gain (`kp`), as f32.
    pub(crate) kp: usize,
    /// Filter time constant in ms (`cpu-lowpeak-limiter-input-tc`).
    pub(crate) filter_tc_ms: usize,
    /// Rounded filter time constant in system-counter ticks (`periods * period_ms * khz`).
    pub(crate) filter_tc_clks: usize,
}

/// IEEE-754 single-precision bits of `num / den` for `num` and `den` non-zero, rounded to
/// nearest, ties to even. The encoder runs where the kernel has no floating point. With 32-bit
/// inputs every step fits in 64 bits.
pub(crate) const fn f32_bits(num: u32, den: u32) -> u32 {
    // +0.0: a zero numerator never normalizes (`n < d` would loop forever).
    if num == 0 {
        return 0;
    }
    let (mut n, mut d) = (num as u64, den as u64);
    let mut exponent: i32 = 0;
    while n >= 2 * d {
        d *= 2;
        exponent += 1;
    }
    while n < d {
        n *= 2;
        exponent -= 1;
    }
    let scaled = n << 23;
    let (mut mantissa, rest) = (scaled / d, scaled % d);
    if 2 * rest > d || (2 * rest == d && mantissa & 1 == 1) {
        mantissa += 1;
    }
    if mantissa == 1 << 24 {
        mantissa >>= 1;
        exponent += 1;
    }
    (((exponent + 127) as u32) << 23) | (mantissa as u32 & 0x7f_ffff)
}
const _: () = assert!(f32_bits(28, 29) == 0x3f77_2c23 && f32_bits(1, 29) == 0x3d0d_3dcb);
const _: () = assert!(f32_bits(8333, 1) == 0x4602_3400 && f32_bits(65536, 1) == 0x4780_0000);
const _: () = assert!(f32_bits(0, 1) == 0 && f32_bits(1, 1) == 0x3f80_0000);

/// Where the RETENTION object (0x4000 zeroed bytes) is published.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Retention {
    /// MAIN's retention pointer (25G83 0xe88).
    Main,
    /// MAIN's retention pointer stays 0 so the firmware keeps that state in its own RAM, and
    /// the zeroed object is instead the RUNTIME table this u64 pointer names.
    Runtime(usize),
}

impl Layout {
    /// The register-map slot of logical loader record `slot`.
    pub(crate) fn slot(&self, slot: usize) -> Option<usize> {
        self.loader_slot
            .get(slot)
            .copied()
            .flatten()
            .map(usize::from)
    }

    /// Register-map records from slot 0 through the highest slot the encoder fills.
    pub(crate) fn loader_records(&self) -> usize {
        (0..LOADER_COUNT)
            .filter(|&slot| self.loaders[slot].length != 0)
            .filter_map(|slot| self.slot(slot))
            .max()
            .map_or(0, |slot| slot + 1)
    }
}

const fn identity_slots() -> [Option<u8>; LOADER_COUNT] {
    let mut out = [None; LOADER_COUNT];
    let mut slot = 0;
    while slot < LOADER_COUNT {
        out[slot] = Some(slot as u8);
        slot += 1;
    }
    out
}

/// 26B123 renumbers the register map (53 -> 60 slots): 0-7, 9-20 and 22-24 keep their numbers,
/// 26-43 move up by five (seven region types were added). Slots 8, 21 and 25 are empty here.
const fn slots_26b123() -> [Option<u8>; LOADER_COUNT] {
    let mut out = [None; LOADER_COUNT];
    let mut slot = 0;
    while slot < LOADER_COUNT {
        out[slot] = match slot {
            0..=7 | 9..=20 | 22..=24 => Some(slot as u8),
            // Logical 36 (the protected power-transition block at PA 0x300e00000) stays empty
            // on 26B123: the firmware maps that block itself and fills its own VA during
            // InitData.
            36 => None,
            26..=43 => Some(slot as u8 + 5),
            _ => None,
        };
        slot += 1;
    }
    out
}

/// The 25G83 InitData ABI.
pub(crate) const V25G83: Layout = Layout {
    name: "25G83",
    version: 0x0c89c357839204b8,
    object_size: OBJECT_SIZE,
    power_config: 0x469,
    tx: TxLayout::StateBlock,
    loader_slot: identity_slots(),
    register_slots: 53,
    loaders: &LOADERS,
    empty_loader_modes: 0,
    main_core_slots: None,
    main: &[run(0, 0x640, 0), run(0xe88, 0x2658, 0)],
    globals: &[run(0, 0xe48, 0)],
    power_performance: &[run(0, 0x6578, 0)],
    firmware_data: FirmwareData {
        fwctl: 0x4f88,
        generation: 0x4fa0,
        phase: 0x4fb0,
        scheduler: 0x4fd4,
        snapshot: 0x40b8,
        snapshot_entries: 448,
    },
    pstate_selectors: [0x88, 0x8c, 0x98],
    coredump_copy: None,
    retention: Retention::Main,
    main_policy: &[],
    globals_policy: &[],
    power_policy: &[],
    power_sampling: None,
    power_limiter: None,
    state_bound: None,
};

/// The 26B123 InitData ABI: the same 31 objects and pointers, with
/// - the main-config power-config pointer 16 bytes later;
/// - a 60-slot register map in MAIN, then the retention pointer at 0xfa0, a new u64 at 0xfa8
///   (the crash copy buffer size), the old 0xe90.. words +0x120, 0xec0.. +0x11c (one word
///   removed at 0xebc), 0x2600.. +0x120 and the last bytes +0x124 (new u32s at 0x275c, left
///   zero, and 0x277e, enabled via `main_policy`);
/// - a u32 inserted at GLOBALS 0x38, so every later word moves by four;
/// - POWER_PERFORMANCE with words inserted at 0x9c (0x10 bytes), 0x28b0 (0x18), 0x3b50 (0x28)
///   and 0x63cc (0x680), 8 bytes removed between 0x2d08 and 0x2f08 and 0x20 between 0x62c4 and
///   0x6318;
/// - FirmwareData words 0x1e0 bytes later (a table was inserted before them);
/// - POWER (left all zero) grown to 0x698 bytes.
pub(crate) const V26B123: Layout = Layout {
    name: "26B123",
    version: 0x0c8a836f039204b8,
    object_size: [
        0xc8, 0x4000, 0x4b8, 0xe4c, 0x20, 0xedd0, 0x698, 0x4000, 0x2788, 0x6c48, 0x88, 0xc18,
        0x1248, 0xe10, 0x60, 0x1b0, 0x28800, 0x79800, 0x4000, 0x30, 0x4000, 0x30, 0x9000, 0x30,
        0x4800, 0x4, 0x30, 0x4800, 0x4000, 0x4000, 0x4000,
    ],
    power_config: 0x479,
    tx: TxLayout::StateBlock,
    loader_slot: slots_26b123(),
    register_slots: 60,
    loaders: &LOADERS,
    empty_loader_modes: 0,
    main_core_slots: None,
    main: &[
        run(0, 0x640, 0),
        run(0xe88, 0xe90, 0x118),
        run(0xe90, 0xebc, 0x120),
        run(0xec0, 0x25b4, 0x11c),
        run(0x2600, 0x263c, 0x120),
        run(0x263c, 0x2658, 0x124),
    ],
    globals: &[run(0, 0x38, 0), run(0x38, 0xe48, 4)],
    power_performance: &[
        run(0, 0x9c, 0),
        run(0x9c, 0x28b0, 0x10),
        run(0x28b0, 0x2d08, 0x28),
        run(0x2f08, 0x3b50, 0x20),
        run(0x3b50, 0x62c4, 0x48),
        run(0x6318, 0x63cc, 0x28),
        run(0x63cc, 0x6578, 0x6a8),
    ],
    firmware_data: FirmwareData {
        fwctl: 0x5168,
        generation: 0x5180,
        phase: 0x5190,
        scheduler: 0x51b4,
        snapshot: 0x4298,
        snapshot_entries: 448,
    },
    pstate_selectors: [0x8c, 0x90, 0x9c],
    // HW data +0xfa8 holds the crash copy buffer's size, +0xfa0 stays 0, and the RETENTION
    // object is published at RUNTIME +0x2c8. Publishing RETENTION at +0xfa0 with the size at
    // +0xfa8 faulted on the first resume from idle power-off.
    coredump_copy: Some(0xfa8),
    retention: Retention::Runtime(0x2c8),
    // macOS 27.2 writes 1 here; with 0 the firmware falsely flags long compute dispatches as
    // stalled.
    main_policy: &[(0x277e, 1)],
    globals_policy: &[],
    // The four u32s this build inserts at +0x9c, 1 each as macOS 27.2 writes them. 0xa4 selects
    // the powered-up state; leaving 0 there keeps the GPU unpowered and triggers an SError on
    // power-up. The firmware's warm restart after a deep sleep needs the other three as well: with
    // only 0xa4 set it crashed in its power-up right after the RTKit handshake on the first wake.
    // The low-peak (PPM) limiter words at +0xa80..+0xae0 are computed from the machine's ADT in
    // `power_limiter`.
    power_policy: &[(0x9c, 1), (0xa0, 1), (0xa4, 1), (0xa8, 1)],
    // Exact power sample period (from `gpu-pwr-sample-period-aic-clks`, in us) and the
    // average-power IIR filter over the ADT's `cpu-avg-limiter-input-tc`.
    power_sampling: Some(PowerSampling {
        period: &[0x2b48, 0x2c10],
        period_f32: 0x2bbc,
        filter: [0x29e4, 0x29f0, 0x29f8],
    }),
    // Low-peak (PPM) power limiter IIR filter and PI controller words (25G83 +0xa70..+0xad0,
    // shifted +0x10 in 26B123), derived from `/arm-io/pmgr/clpc` (`cpu-lowpeak-limiter-*`)
    // and `/arm-io/sgx` (`gpu-power-sample-period`).
    power_limiter: Some(PowerLimiter {
        filter_periods_x4: 0xa80,
        filter_a_neg: 0xa8c,
        filter_a: 0xa94,
        ki_dt: 0xa9c,
        clamp: 0xaa8,
        kp: 0xaac,
        filter_tc_ms: 0xad8,
        filter_tc_clks: 0xae0,
    }),
    state_bound: None,
};

/// G15S 26B123 keeps the protocol layouts and ADT-derived power sampling/limiter encoding while
/// supplying its own HW data and register map.
pub(crate) const G15S_26B123: Layout = Layout {
    name: "26B123 G15S",
    version: 0x0c8a836f039404b8,
    tx: TxLayout::RingHeader,
    object_size: {
        let mut sizes = V26B123.object_size;
        sizes[id::MAIN] = 0x2790;
        sizes
    },
    loader_slot: slots_g15s(),
    loaders: &G15S_LOADERS,
    empty_loader_modes: (1 << 2)
        | (0xf << 5)
        | (7 << 25)
        | (1 << 32)
        | (1 << 35)
        | (1 << 43)
        | (3 << 46)
        | (0xf << 52)
        | (1 << 58),
    main_core_slots: Some(20),
    main_policy: &[(0x275c, 1), (0x2786, 1), (0x278a, 1)],
    globals: &[run(0, 0x38, 0), run(0x38, 0xe28, 4), run(0xe28, 0xe47, 5)],
    globals_policy: &[(0xe31, 1)],
    ..V26B123
};

/// G15G (T8122 C0) 26B123: the G16G 26B123 objects, register-map positions and protocol, with
/// the InitData version this image accepts (measured on the J615, linux#2781: the G16G version,
/// 0x0391, 0x0393 and 0x0395 in the differing field fault right after the root), the T8122
/// register blocks, and the words the m3 27.2 runtime adds to the performance object.
pub(crate) const G15G_26B123: Layout = Layout {
    name: "26B123 G15G",
    version: 0x0c8a836f039404b8,
    loaders: &G15G_LOADERS,
    // 26.6.2 slots 2, 5-7, 30, 33, 34, 37, 38 and 41-43 carry write mode 2 while empty
    // (`m3_mmio_26::empty_mode`); at their 26B123 positions (`slots_26b123`).
    empty_loader_modes: (1 << 2) | (7 << 5) | (1 << 35) | (3 << 38) | (3 << 42) | (7 << 46),
    // Two words this InitData family adds, 1 each as the G14 27.2 path sets them, next to the
    // four power-up words (linux#2781 `relocate_26b123`).
    power_policy: &[
        (0x9c, 1),
        (0xa0, 1),
        (0xa4, 1),
        (0xa8, 1),
        (0x2f34, 1),
        (0x6ac8, 1),
    ],
    // The shared3 block's state word (+0x62ac + 8): with the block all zero the G15G firmware
    // never takes the first device-control records.
    state_bound: Some(0x62b4),
    // GLOBALS as the G15 family's 26B123 InitData version lays it out (the same version as
    // G15S): the words from 25G83 +0xe28 on sit 5 bytes up, not G16G's 4. With G16G's runs the
    // soft-fault word (25G83 +0xe28 = 3) lands one byte low, the firmware reads 0 there and an
    // unmapped shader write faults the context instead of being dropped.
    globals: G15S_26B123.globals,
    ..V26B123
};

/// The T8122 register blocks behind each logical 26.6.2 slot (`m3_mmio_26::area` with the
/// T8122 blocks of `m3_adt_config::T8122_IO_MAPPINGS`).
const G15G_LOADERS: [Loader; LOADER_COUNT] = {
    const SGX: u64 = 0x290000000;
    let mut out = [EMPTY; LOADER_COUNT];
    out[0] = mmio(0x2d1014000, 0x4000, 0, 2);
    // The memory cache, two banks, as on T8132.
    out[3] = LOADERS[3];
    out[9] = mmio(0x2d03d0000, 0x1000, 0, 2);
    out[10] = mmio(0x2d03c0000, 0x2000, 0, 0);
    out[12] = mmio(0x31145c000, 0x4000, 0, 2);
    out[14] = mmio(0x2d0280000, 0x8000, 0, 0);
    out[17] = mmio(SGX, 0x20000, 0, 2);
    out[26] = mmio(SGX + 0xd04000, 0x8000, 0xd04000, 2);
    out[27] = mmio(SGX + 0xd0d000, 0x1000, 0xd0d000, 2);
    out[28] = mmio(SGX + 0xd50000, 0x8000, 0xd50000, 2);
    out[29] = mmio(SGX + 0xd10000, 0x4000, 0xd10000, 2);
    out[31] = mmio(SGX + 0xd40000, 0x4000, 0xd40000, 2);
    out[32] = mmio(SGX + 0xd60000, 0x8000, 0xd60000, 2);
    out[35] = mmio(SGX + 0xe00000, 0x4000, 0xe00000, 2);
    out[39] = mmio(SGX + 0xe08000, 0x8000, 0, 2);
    out[40] = mmio(SGX + 0xe1c000, 0x4000, 0xe1c000, 2);
    out
};

const fn slots_g15s() -> [Option<u8>; LOADER_COUNT] {
    let mut slots = slots_26b123();
    // The final two named registers use G15S-specific slots. Slot 41 (G16G's
    // firmware-owned power transition block) stays empty on G15S.
    slots[35] = Some(40);
    slots[41] = Some(48);
    slots
}

/// Runs are ordered, disjoint and land inside an object of `size` bytes.
const fn runs_fit(runs: &[Run], size: usize) -> bool {
    let mut i = 0;
    while i < runs.len() {
        if runs[i].start >= runs[i].end
            || runs[i].end + runs[i].delta > size
            || (i > 0 && runs[i].start < runs[i - 1].end)
        {
            return false;
        }
        i += 1;
    }
    true
}

/// Fixed 25G83 GLOBALS words remapped into every ABI; shared by `encode_for` and `layout_fits`.
const GLOBALS_FIXED: &[(usize, u32)] = &[
    (0x78, 1),
    (0xe28, 3),
    (0x24, 3000),
    (0x998, 40),
    (0x99c, 10),
    (0x9a0, 250),
    (0x9b8, 2),
    (0x9bc, 40),
    (0x9c0, 5),
    // Firmware DM2 (compute) progress-watchdog thresholds (~4 ms/tick):
    // - 0x9c8 (26B123 0x9cc): secondary DM2 soft-preempt watchdog, raised from 40 (~0.16 s)
    //   to 750 (~3.0 s) so legitimate long/contended compute dispatches (e.g. 1.5 s solo
    //   workloads) complete without premature kill while staying below the 5 s host fail-stop.
    // - 0x9cc (26B123 0x9d0): primary DM2 recovery watchdog, raised from 50 (~0.20 s) to 800
    //   (~3.2 s) so legitimate compute dispatches complete while real hangs still trigger
    //   firmware recovery well before the 5 s host fail-stop deadline.
    (0x9c8, 750),
    (0x9cc, 800),
];

/// Fixed 25G83 POWER_PERFORMANCE offsets remapped into every ABI; shared by `encode_for` and
/// `layout_fits` so compile-time collision checks cover every remapped write.
const POWER_TEMP_PTR: usize = 0x3b50;
const POWER_FIXED_U32: &[usize] = &[4, 8, 0x10, 0x14, 0x2c, 0x30];
const POWER_CEILING_U32: &[usize] = &[
    0x9f0, 0x9f4, 0xa38, 0xaa8, 0xaac, 0xaf0, 0xb78, 0xb7c, 0xb80, 0xbc0, 0x22b0, 0x22b4, 0x22f8,
    0x2210, 0x2870, 0x2a3c, 0x2b84, 0x2b88, 0x2b8c, 0x2bcc, 0x2c40, 0x2c44, 0x2c88, 0x279e,
];

const fn ranges_overlap(a: usize, a_len: usize, b: usize, b_len: usize) -> bool {
    a < b + b_len && b < a + a_len
}

/// Every `main_policy` word fits in MAIN and is disjoint from every other policy word, the
/// register map, `coredump_copy`, and every remapped 25G83 MAIN run.
const fn main_policy_fits(layout: &Layout) -> bool {
    let size = layout.object_size[id::MAIN];
    let map_len = layout.register_slots * LOADER_STRIDE;
    let mut i = 0;
    while i < layout.main_policy.len() {
        let at = layout.main_policy[i].0;
        if at + 4 > size || ranges_overlap(at, 4, 0x640, map_len) {
            return false;
        }
        if let Some(cc) = layout.coredump_copy {
            if ranges_overlap(at, 4, cc, 8) {
                return false;
            }
        }
        let mut j = 0;
        while j < i {
            if ranges_overlap(at, 4, layout.main_policy[j].0, 4) {
                return false;
            }
            j += 1;
        }
        let mut r = 0;
        while r < layout.main.len() {
            let run = layout.main[r];
            let start = run.start + run.delta;
            let len = run.end - run.start;
            if ranges_overlap(at, 4, start, len) {
                return false;
            }
            r += 1;
        }
        i += 1;
    }
    true
}

/// Every `globals_policy` word fits in GLOBALS and is disjoint from every other policy word,
/// every p-state selector, and every remapped 25G83 GLOBALS word.
const fn globals_policy_fits(layout: &Layout) -> bool {
    let size = layout.object_size[id::GLOBALS];
    let mut i = 0;
    while i < layout.globals_policy.len() {
        let at = layout.globals_policy[i].0;
        if at + 4 > size {
            return false;
        }
        let mut j = 0;
        while j < i {
            if ranges_overlap(at, 4, layout.globals_policy[j].0, 4) {
                return false;
            }
            j += 1;
        }
        let mut k = 0;
        while k < layout.pstate_selectors.len() {
            if ranges_overlap(at, 4, layout.pstate_selectors[k], 4) {
                return false;
            }
            k += 1;
        }
        let mut m = 0;
        while m < GLOBALS_FIXED.len() {
            let Some(remapped) = remap_const(layout.globals, GLOBALS_FIXED[m].0, 4) else {
                return false;
            };
            if ranges_overlap(at, 4, remapped, 4) {
                return false;
            }
            m += 1;
        }
        i += 1;
    }
    true
}

/// A 4-byte POWER_PERFORMANCE slot fits and does not collide with any remapped 25G83 word.
const fn power_slot_clear(layout: &Layout, at: usize) -> bool {
    let size = layout.object_size[id::POWER_PERFORMANCE];
    if at + 4 > size {
        return false;
    }
    let Some(temp) = remap_const(layout.power_performance, POWER_TEMP_PTR, 8) else {
        return false;
    };
    if ranges_overlap(at, 4, temp, 8) {
        return false;
    }
    let mut i = 0;
    while i < POWER_FIXED_U32.len() {
        let Some(remapped) = remap_const(layout.power_performance, POWER_FIXED_U32[i], 4) else {
            return false;
        };
        if ranges_overlap(at, 4, remapped, 4) {
            return false;
        }
        i += 1;
    }
    let mut j = 0;
    while j < POWER_CEILING_U32.len() {
        let Some(remapped) = remap_const(layout.power_performance, POWER_CEILING_U32[j], 4) else {
            return false;
        };
        if ranges_overlap(at, 4, remapped, 4) {
            return false;
        }
        j += 1;
    }
    true
}

const fn power_policy_overlaps(policy: &[(usize, u32)], at: usize) -> bool {
    let mut i = 0;
    while i < policy.len() {
        if ranges_overlap(at, 4, policy[i].0, 4) {
            return false;
        }
        i += 1;
    }
    true
}

const fn sampling_clear_of(sampling: Option<PowerSampling>, at: usize) -> bool {
    let Some(s) = sampling else {
        return true;
    };
    if ranges_overlap(at, 4, s.period_f32, 4) {
        return false;
    }
    let mut i = 0;
    while i < s.period.len() {
        if ranges_overlap(at, 4, s.period[i], 4) {
            return false;
        }
        i += 1;
    }
    let mut j = 0;
    while j < s.filter.len() {
        if ranges_overlap(at, 4, s.filter[j], 4) {
            return false;
        }
        j += 1;
    }
    true
}

/// Every `power_policy`, `power_sampling` and `power_limiter` word fits in POWER_PERFORMANCE
/// and is disjoint from every remapped 25G83 word and from every other policy/sampling/limiter
/// word.
const fn power_policy_fits(layout: &Layout) -> bool {
    let mut i = 0;
    while i < layout.power_policy.len() {
        let at = layout.power_policy[i].0;
        if !power_slot_clear(layout, at) {
            return false;
        }
        let mut j = 0;
        while j < i {
            if ranges_overlap(at, 4, layout.power_policy[j].0, 4) {
                return false;
            }
            j += 1;
        }
        i += 1;
    }
    if let Some(s) = layout.power_sampling {
        if !power_slot_clear(layout, s.period_f32)
            || !power_policy_overlaps(layout.power_policy, s.period_f32)
        {
            return false;
        }
        let mut p = 0;
        while p < s.period.len() {
            let at = s.period[p];
            if !power_slot_clear(layout, at)
                || !power_policy_overlaps(layout.power_policy, at)
                || ranges_overlap(at, 4, s.period_f32, 4)
            {
                return false;
            }
            let mut prev = 0;
            while prev < p {
                if ranges_overlap(at, 4, s.period[prev], 4) {
                    return false;
                }
                prev += 1;
            }
            p += 1;
        }
        let mut f = 0;
        while f < s.filter.len() {
            let at = s.filter[f];
            if !power_slot_clear(layout, at)
                || !power_policy_overlaps(layout.power_policy, at)
                || ranges_overlap(at, 4, s.period_f32, 4)
            {
                return false;
            }
            let mut p2 = 0;
            while p2 < s.period.len() {
                if ranges_overlap(at, 4, s.period[p2], 4) {
                    return false;
                }
                p2 += 1;
            }
            let mut prev = 0;
            while prev < f {
                if ranges_overlap(at, 4, s.filter[prev], 4) {
                    return false;
                }
                prev += 1;
            }
            f += 1;
        }
    }
    if let Some(lim) = layout.power_limiter {
        let slots = [
            lim.filter_periods_x4,
            lim.filter_a_neg,
            lim.filter_a,
            lim.ki_dt,
            lim.clamp,
            lim.kp,
            lim.filter_tc_ms,
            lim.filter_tc_clks,
        ];
        let mut k = 0;
        while k < slots.len() {
            let at = slots[k];
            if !power_slot_clear(layout, at)
                || !power_policy_overlaps(layout.power_policy, at)
                || !sampling_clear_of(layout.power_sampling, at)
            {
                return false;
            }
            let mut prev = 0;
            while prev < k {
                if ranges_overlap(at, 4, slots[prev], 4) {
                    return false;
                }
                prev += 1;
            }
            k += 1;
        }
    }
    true
}

/// A layout's words fit its objects and do not overlap each other.
pub(crate) const fn layout_fits(layout: &Layout) -> bool {
    let fd = &layout.firmware_data;
    let size = &layout.object_size;
    fd.snapshot + fd.snapshot_entries * 8 <= fd.fwctl
        && fd.fwctl + 0x10 <= fd.generation
        && fd.generation + 8 <= fd.phase
        && fd.phase < fd.scheduler
        && fd.scheduler + 4 <= size[id::FIRMWARE_DATA]
        && layout.power_config + 8 <= size[id::RUNTIME]
        // The register map ends where the MAIN words after it (the retention pointer) start.
        && 0x640 + layout.register_slots * LOADER_STRIDE
            == layout.main[1].start + layout.main[1].delta
        && runs_fit(layout.main, size[id::MAIN])
        && runs_fit(layout.globals, size[id::GLOBALS])
        && runs_fit(layout.power_performance, size[id::POWER_PERFORMANCE])
        && retention_fits(layout)
        && main_policy_fits(layout)
        && globals_policy_fits(layout)
        && power_policy_fits(layout)
}

/// A RUNTIME retention table pointer is aligned, inside RUNTIME and clear of the power-config
/// pointer.
const fn retention_fits(layout: &Layout) -> bool {
    match layout.retention {
        Retention::Main => true,
        Retention::Runtime(at) => {
            at % 8 == 0
                && at + 8 <= layout.object_size[id::RUNTIME]
                && (at + 8 <= layout.power_config || layout.power_config + 8 <= at)
        }
    }
}
const _: () = assert!(layout_fits(&V25G83) && layout_fits(&V26B123) && layout_fits(&G15S_26B123));
const _: () = assert!(layout_fits(&G15G_26B123));
const _: () = assert!(match G15G_26B123.state_bound {
    Some(at) =>
        power_slot_clear(&G15G_26B123, at) && power_policy_overlaps(G15G_26B123.power_policy, at),
    None => false,
});

/// One logical loader record, including its page offset and exact PTE class.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Loader {
    pub(crate) pa: u64,
    pub(crate) length: u32,
    pub(crate) requested: u32,
    pub(crate) tag: u64,
    pub(crate) mode: u8,
    pub(crate) attrs: u64,
}
const EMPTY: Loader = Loader {
    pa: 0,
    length: 0,
    requested: 0,
    tag: 0,
    mode: 0,
    attrs: 0,
};
const MMIO_RW: u64 = 0x00c0000000000447;
const MMIO_RO: u64 = 0x0080000000000447;
const MMIO_PROTECTED: u64 = 0x00200000000004c7;
const fn loaders() -> [Loader; LOADER_COUNT] {
    let mut out = [EMPTY; LOADER_COUNT];
    out[0] = Loader {
        pa: 0x381014000,
        length: 0x4000,
        requested: 0x4000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[3] = Loader {
        pa: 0x2201c4000,
        length: 0x30000,
        requested: 0x18000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[9] = Loader {
        pa: 0x3803d0000,
        length: 0x1000,
        requested: 0x1000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[10] = Loader {
        pa: 0x3803c0000,
        length: 0x2000,
        requested: 0x2000,
        tag: 0,
        mode: 0,
        attrs: MMIO_RO,
    };
    out[12] = Loader {
        pa: 0x50165c000,
        length: 0x4000,
        requested: 0x4000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[14] = Loader {
        pa: 0x380280000,
        length: 0x8000,
        requested: 0x8000,
        tag: 0,
        mode: 0,
        attrs: MMIO_RO,
    };
    out[15] = Loader {
        pa: 0x38c840000,
        length: 0x24000,
        requested: 0x24000,
        tag: 0,
        mode: 0,
        attrs: MMIO_RO,
    };
    out[17] = Loader {
        pa: 0x300000000,
        length: 0x20000,
        requested: 0x20000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[22] = Loader {
        pa: 0x301000000,
        length: 0x8000,
        requested: 0x8000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[26] = Loader {
        pa: 0x300d04000,
        length: 0x8000,
        requested: 0x8000,
        tag: 0xd04000,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[27] = Loader {
        pa: 0x300d0d000,
        length: 0x1000,
        requested: 0x1000,
        tag: 0xd0d000,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[28] = Loader {
        pa: 0x300d58000,
        length: 0x8000,
        requested: 0x8000,
        tag: 0xd58000,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[29] = Loader {
        pa: 0x300d10000,
        length: 0x4000,
        requested: 0x4000,
        tag: 0xd10000,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[31] = Loader {
        pa: 0x300d40000,
        length: 0x4000,
        requested: 0x4000,
        tag: 0xd40000,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[32] = Loader {
        pa: 0x300d60000,
        length: 0x8000,
        requested: 0x8000,
        tag: 0xd60000,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[36] = Loader {
        pa: 0x300e00000,
        length: 0x4000,
        requested: 0x4000,
        tag: 0xe00000,
        mode: 0,
        attrs: MMIO_PROTECTED,
    };
    out[39] = Loader {
        pa: 0x300e08000,
        length: 0x8000,
        requested: 0x8000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[40] = Loader {
        pa: 0x300e1c000,
        length: 0x4000,
        requested: 0x4000,
        tag: 0xe1c000,
        mode: 2,
        attrs: MMIO_RW,
    };
    out[41] = Loader {
        pa: 0x300e1f800,
        length: 0x4000,
        requested: 0x4000,
        tag: 0,
        mode: 2,
        attrs: MMIO_RW,
    };
    out
}
pub(crate) const LOADERS: [Loader; LOADER_COUNT] = loaders();

const G15S_LOADERS: [Loader; LOADER_COUNT] = {
    const SGX: u64 = 0x290000000;
    let mut out = [EMPTY; LOADER_COUNT];
    out[0] = mmio(0x351014000, 0x4000, 0, 2);
    out[1] = mmio(0x3502bc000, 0x4000, 0, 0);
    out[3] = LOADERS[3];
    out[4] = mmio(0x35104c000, 0x4000, 0, 0);
    out[9] = mmio(0x3503d0000, 0x4000, 0, 2);
    out[10] = mmio(0x3503c0000, 0x4000, 0, 0);
    out[11] = mmio(0x3503d8000, 0x4000, 0, 2);
    out[12] = mmio(0x30945c000, 0x4000, 0, 2);
    out[14] = mmio(0x350280000, 0x8000, 0, 0);
    out[17] = mmio(SGX, 0x20000, 0, 2);
    out[20] = mmio(0x293000000, 0x400000, 0, 2);
    out[26] = mmio(SGX + 0xd04000, 0x8000, 0xd04000, 2);
    out[28] = mmio(SGX + 0xd50000, 0x8000, 0xd50000, 2);
    out[29] = mmio(SGX + 0xd10000, 0x4000, 0xd10000, 2);
    out[31] = mmio(SGX + 0xd40000, 0x4000, 0xd40000, 2);
    out[32] = mmio(SGX + 0xd60000, 0x20000, 0xd60000, 2);
    out[33] = mmio(SGX + 0xd44000, 0x4000, 0xd44000, 2);
    out[34] = mmio(SGX + 0xd4c000, 0x200, 0xd4c000, 2);
    // The protected register block is explicitly supplied with mode 2 in G15S slot 40.
    out[35] = mmio(SGX + 0xe00000, 0x4000, 0xe00000, 2);
    out[37] = mmio(SGX + 0xe40000, 0x4000, 0xe40000, 2);
    out[39] = mmio(SGX + 0xe08000, 0x8000, 0, 2);
    out[40] = mmio(SGX + 0xe5c000, 0x4000, 0xe5c000, 2);
    out[41] = mmio(SGX + 0xe60000, 0x50, 0xe60000, 2);
    out
};

const fn mmio(pa: u64, length: u32, tag: u64, mode: u8) -> Loader {
    Loader {
        pa,
        length,
        requested: length,
        tag,
        mode,
        attrs: if mode == 2 { MMIO_RW } else { MMIO_RO },
    }
}

impl Loader {
    /// Number of banks in this window. A banked record encodes the combined span
    /// and the requested size of one bank.
    pub(crate) fn banks(&self) -> usize {
        if self.length > self.requested {
            2
        } else {
            1
        }
    }

    /// Actual mapped span. The two banks have adjacent VAs and discontiguous PAs.
    pub(crate) fn span(&self, bank: usize) -> Result<(u64, u32, u32), ()> {
        if self.length == 0 || bank >= self.banks() {
            return Err(());
        }
        let offset = self.pa & (PAGE - 1);
        let bytes = u64::from(self.requested);
        let size = (offset + bytes + PAGE - 1) & !(PAGE - 1);
        Ok((
            (self.pa & PA_MASK) + bank as u64 * 0x2000000,
            size as u32,
            bank as u32 * self.requested,
        ))
    }
}

pub(crate) fn loader_span(slot: usize, bank: usize) -> Result<(u64, u32, u32), ()> {
    LOADERS.get(slot).ok_or(())?.span(bank)
}

fn put32(out: &mut [u8], at: usize, value: u32) -> Result<(), ()> {
    out.get_mut(at..at + 4)
        .ok_or(())?
        .copy_from_slice(&value.to_le_bytes());
    Ok(())
}
fn put64(out: &mut [u8], at: usize, value: u64) -> Result<(), ()> {
    out.get_mut(at..at + 8)
        .ok_or(())?
        .copy_from_slice(&value.to_le_bytes());
    Ok(())
}

pub(crate) fn loader_encode(slot: usize, va: u64, record: &mut [u8]) -> Result<(), ()> {
    LOADERS.get(slot).ok_or(())?.encode(va, record)
}

impl Loader {
    fn encode(&self, va: u64, record: &mut [u8]) -> Result<(), ()> {
        let e = self;
        let out = record.get_mut(..LOADER_STRIDE).ok_or(())?;
        if e.length == 0 {
            if va != 0 {
                return Err(());
            }
            out.fill(0);
            return Ok(());
        }
        let offset = e.pa & (PAGE - 1);
        let span = (offset + u64::from(e.length) + PAGE - 1) & !(PAGE - 1);
        if !(HOST_START..HOST_END).contains(&va)
            || va & (PAGE - 1) != offset
            || span > HOST_END - (va - offset)
        {
            return Err(());
        }
        put64(out, 0, e.pa)?;
        put64(out, 8, va)?;
        put32(out, 0x10, e.length)?;
        put32(out, 0x14, e.requested)?;
        put64(out, 0x18, e.tag)?;
        put64(out, 0x20, u64::from(e.mode))
    }
}

/// Capacity for the largest qualified table: J713 has ten rows, J773g eleven.
pub(crate) const PERF_STATES: usize = 11;

/// Machine-specific values selected from the original ADT before firmware publication.
/// Frequencies are exact MHz, voltages are millivolts. No reference-bin defaults exist.
/// Only the first `count` rows are used; the rest are zero.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Performance {
    pub(crate) count: usize,
    pub(crate) frequency: [u32; PERF_STATES],
    pub(crate) auxiliary: [u32; PERF_STATES],
    pub(crate) voltage: [u32; PERF_STATES],
    pub(crate) sram: [u32; PERF_STATES],
}

impl Performance {
    pub(crate) fn valid(&self) -> bool {
        if !(2..=PERF_STATES).contains(&self.count) {
            return false;
        }
        for i in self.count..PERF_STATES {
            if self.frequency[i] != 0
                || self.auxiliary[i] != 0
                || self.voltage[i] != 0
                || self.sram[i] != 0
            {
                return false;
            }
        }
        for i in 0..self.count {
            if self.frequency[i] > 3000
                || self.auxiliary[i] > self.frequency[i]
                || ((i == 0) != (self.frequency[i] == 0))
                || ((i == 0) != (self.auxiliary[i] == 0))
                || !(100..=1100).contains(&self.voltage[i])
                || !(100..=1100).contains(&self.sram[i])
                || self.sram[i] < self.voltage[i]
                || (i != 0
                    && (self.frequency[i] <= self.frequency[i - 1]
                        || self.auxiliary[i] <= self.auxiliary[i - 1]
                        || self.voltage[i] < self.voltage[i - 1]
                        || self.sram[i] < self.sram[i - 1]))
            {
                return false;
            }
        }
        true
    }
}

/// Addresses and policy supplied by the owner after mapping its private objects.
pub(crate) struct Input {
    pub(crate) va: [u64; OBJECT_COUNT],
    pub(crate) channel_state: [u64; 12],
    pub(crate) channel_ring: [u64; 12],
    pub(crate) loader_va: [u64; LOADER_COUNT],
    pub(crate) scratch: u64,
    pub(crate) lower_uma: u64,
    pub(crate) lower_parameter: u64,
    pub(crate) ktrace_mask: u32,
    pub(crate) progress_recovery: u32,
    pub(crate) enabled_cores: u32,
    pub(crate) performance: Performance,
    /// Exact GPU power sample period in system-counter ticks (`gpu-pwr-sample-period-aic-clks`),
    /// written to POWER_PERFORMANCE +4/+8. Non-zero is required.
    pub(crate) power_sample_clks: u32,
    /// Exact GPU power sample period in us (`power_sample_clks * 1_000_000 / timebase_hz`),
    /// used instead of the coarse truncated ms property for the us period and average-power
    /// sample count. 0 when absent, which a layout with [`Layout::power_sampling`] refuses.
    pub(crate) power_sample_us: u32,
    /// Coarse integer millisecond GPU power sample period (`gpu-power-sample-period`). 0 when
    /// absent, which a layout with [`Layout::power_limiter`] refuses.
    pub(crate) power_sample_period_ms: u32,
    /// System counter frequency in kHz (`timebase_hz / 1000`). 0 is refused by
    /// [`Layout::power_limiter`].
    pub(crate) timebase_khz: u32,
    /// Average-power filter time constant in ms (`/arm-io/pmgr/clpc` `cpu-avg-limiter-input-tc`).
    /// 0 when absent, which a layout with [`Layout::power_sampling`] refuses.
    pub(crate) avg_power_filter_tc_ms: u32,
    /// Low-peak (PPM) limiter filter time constant in ms (`cpu-lowpeak-limiter-input-tc`).
    /// 0 when absent, which a layout with [`Layout::power_limiter`] refuses.
    pub(crate) lowpeak_limiter_tc_ms: u32,
    /// Low-peak (PPM) limiter integral gain in Q16.16 (`cpu-lowpeak-limiter-ki`).
    /// 0 when absent, which a layout with [`Layout::power_limiter`] refuses.
    pub(crate) lowpeak_limiter_ki_q16: u32,
    /// Low-peak (PPM) limiter proportional gain in Q16.16 (`cpu-lowpeak-limiter-kp`).
    /// 0 when absent, which a layout with [`Layout::power_limiter`] refuses.
    pub(crate) lowpeak_limiter_kp_q16: u32,
    /// Host crash copy buffer size in bytes (`/arm-io/gfx-asc/iop-gfx-nub`
    /// `coredump-copy-buffer`). 0 or unaligned is refused by layouts with `coredump_copy`.
    pub(crate) coredump_copy_size: u64,
}

pub(crate) fn object_va(va: u64, size: usize) -> bool {
    (HOST_START..HOST_END).contains(&va) && va & 7 == 0 && size != 0 && size as u64 <= HOST_END - va
}
fn bootstrap_alias(va: u64) -> bool {
    (BOOTSTRAP_START..=BOOTSTRAP_START + BOOTSTRAP_SIZE - 0x8000).contains(&va)
        && va & (PAGE - 1) == 0
}

/// Encodes the 25G83 root.
pub(crate) fn encode_root(out: &mut [u8], va: &[u64; OBJECT_COUNT]) -> Result<(), ()> {
    encode_root_for(&V25G83, out, va)
}

/// Encodes the root of `layout`'s ABI after checking every object VA and overlap.
pub(crate) fn encode_root_for(
    layout: &Layout,
    out: &mut [u8],
    va: &[u64; OBJECT_COUNT],
) -> Result<(), ()> {
    let size = &layout.object_size;
    for i in 0..OBJECT_COUNT {
        if !object_va(va[i], size[i]) {
            return Err(());
        }
        for j in 0..i {
            if va[i] < va[j] + size[j] as u64 && va[j] < va[i] + size[i] as u64 {
                return Err(());
            }
        }
    }
    let out = out.get_mut(..root::SIZE).ok_or(())?;
    out.fill(0);
    put64(out, 0, layout.version)?;
    for (at, i) in [
        (0x08, id::BRN),
        (0x18, id::RUNTIME),
        (0x20, id::GLOBALS),
        (0xa8, id::CONTROL),
        (0xb0, id::FIRMWARE_DATA),
        (0xb8, id::POWER),
        (0xc0, id::DYNAMIC),
    ] {
        put64(out, at, va[i])?;
    }
    put64(out, 0x28, 1u64 << 32)?;
    put32(out, 0x30, 0x030e4000)?;
    for (i, (shift, count)) in [(36u8, 64u16), (25, 2048), (14, 2048)].iter().enumerate() {
        let at = 0x34 + i * 0x20;
        let c = count.to_le_bytes();
        out[at..at + 8].copy_from_slice(&[8, 14, 14, *shift, c[0], c[1], 0, 0x40]);
        put64(out, at + 8, 1)?;
        put64(out, at + 0x10, PA_MASK)?;
        put64(out, at + 0x18, (u64::from(*count) - 1) << shift)?;
    }
    Ok(())
}

pub(crate) fn encode_channel(
    out: &mut [u8],
    state: u64,
    ring: u64,
    control: bool,
) -> Result<(), ()> {
    encode_tx(TxLayout::StateBlock, out, state, ring, control)
}

fn encode_tx(
    layout: TxLayout,
    out: &mut [u8],
    state: u64,
    ring: u64,
    control: bool,
) -> Result<(), ()> {
    let header = layout.ring_header(control) as u64;
    let bytes = if control {
        0x4000
    } else {
        0x1800 + header as usize
    };
    if !object_va(state, 0x30)
        || !object_va(ring, bytes)
        || (state < ring + bytes as u64 && ring < state + 0x30)
    {
        return Err(());
    }
    put64(out, 0, state)?;
    put64(out, 8, state + layout.completion() as u64)?;
    put64(out, 0x10, if header == 0 { state + 0x20 } else { ring })?;
    put64(out, 0x18, ring + header)
}

/// Encodes the complete private 31-object 25G83 graph, byte-for-byte matching the
/// working M4 C encoder. No mapping or publication is performed here.
pub(crate) fn encode(
    objects: &mut [&mut [u8]; OBJECT_COUNT],
    input: &Input,
    ceiling: u32,
) -> Result<(), ()> {
    encode_for(&V25G83, objects, input, ceiling)
}

/// Encodes the complete private 31-object graph of `layout`'s ABI. Every input is checked
/// before the first byte is written.
pub(crate) fn encode_for(
    layout: &Layout,
    objects: &mut [&mut [u8]; OBJECT_COUNT],
    input: &Input,
    ceiling: u32,
) -> Result<(), ()> {
    if !input.performance.valid()
        || !(1..input.performance.count as u32).contains(&ceiling)
        || input.power_sample_clks == 0
        || !object_va(input.scratch, 0x8000)
        || !bootstrap_alias(input.lower_uma)
        || !bootstrap_alias(input.lower_parameter)
        || (input.lower_uma < input.lower_parameter + 0x8000
            && input.lower_parameter < input.lower_uma + 0x8000)
    {
        return Err(());
    }
    // Validate ADT-derived power sampling and limiter inputs before touching any output byte.
    let sampling_values = if layout.power_sampling.is_some() {
        if input.power_sample_us == 0 || input.avg_power_filter_tc_ms == 0 {
            return Err(());
        }
        let samples_u64 =
            (u64::from(input.avg_power_filter_tc_ms) * 1000) / u64::from(input.power_sample_us);
        let samples = u32::try_from(samples_u64).map_err(|_| ())?;
        if samples < 2 || samples.checked_mul(4).is_none() {
            return Err(());
        }
        Some((input.power_sample_us, samples))
    } else {
        None
    };
    let limiter_values = if layout.power_limiter.is_some() {
        if input.power_sample_period_ms == 0
            || input.timebase_khz == 0
            || input.lowpeak_limiter_tc_ms == 0
            || input.lowpeak_limiter_ki_q16 == 0
            || input.lowpeak_limiter_kp_q16 == 0
        {
            return Err(());
        }
        // The low-peak IIR filter spans `periods` sample periods. One period (the ADT's
        // limiter time constant equals its sample period, as on the 15-inch M3 Air) is a valid
        // filter: a_neg = (1 - 1) / 1 = +0.0 and a = 1 / 1 = +1.0, i.e. each sample is taken
        // as is. Only a time constant shorter than one period is refused.
        let periods = input.lowpeak_limiter_tc_ms / input.power_sample_period_ms;
        if periods < 1 {
            return Err(());
        }
        let periods_x4 = periods.checked_mul(4).ok_or(())?;
        let ki_num = input
            .lowpeak_limiter_ki_q16
            .checked_mul(input.power_sample_period_ms)
            .ok_or(())?;
        let tc_clks = periods
            .checked_mul(input.power_sample_period_ms)
            .and_then(|ms| ms.checked_mul(input.timebase_khz))
            .ok_or(())?;
        Some((periods, periods_x4, ki_num, tc_clks))
    } else {
        None
    };
    if layout.coredump_copy.is_some()
        && (input.coredump_copy_size == 0
            || input.coredump_copy_size & 0xfff != 0
            || input.coredump_copy_size > 0x10_0000)
    {
        return Err(());
    }
    let size = &layout.object_size;
    for (i, obj) in objects.iter().enumerate() {
        if obj.len() < size[i] {
            return Err(());
        }
    }
    encode_root_for(layout, objects[id::ROOT], &input.va)?;
    for i in 1..OBJECT_COUNT {
        objects[i][..size[i]].fill(0);
    }
    let runtime = &mut objects[id::RUNTIME];
    for (at, i) in [
        (0x000, id::MAIN),
        (0x010, id::TIMER),
        (0x1c0, id::EVENT_STATE),
        (0x1c8, id::EVENT_DATA),
        (0x1d0, id::LOG_STATE),
        (0x1d8, id::LOG_ENTRIES),
        (0x1e0, id::KTRACE_STATE),
        (0x1e8, id::KTRACE_DATA),
        (0x1f0, id::STATISTICS_STATE),
        (0x1f8, id::STATISTICS_DATA),
        (0x200, id::LOG_DATA),
        (0x254, id::ACTIVITY_TILER),
        (0x25c, id::ACTIVITY_FRAGMENT),
        (0x264, id::ACTIVITY_COMPUTE),
        (0x26c, id::ACTIVITY_RECOVERY),
        (0x2d8, id::PARAMETER),
        (0x2e8, id::UMA),
        (layout.power_config, id::POWER_PERFORMANCE),
    ] {
        put64(runtime, at, input.va[i])?;
    }
    if layout.tx == TxLayout::RingHeader {
        put64(runtime, 0x8, input.va[id::TIMER])?;
    }
    put64(runtime, 0x18, input.scratch)?;
    put64(runtime, 0x2d0, input.lower_parameter)?;
    put64(runtime, 0x2e0, input.lower_uma)?;
    put32(runtime, 0x250, 1)?;
    put32(runtime, 0x2f8, 4)?;
    if let Retention::Runtime(at) = layout.retention {
        put64(runtime, at, input.va[id::RETENTION])?;
    }
    for i in 0..12 {
        encode_tx(
            layout.tx,
            &mut runtime[0x20 + i * 0x20..],
            input.channel_state[i],
            input.channel_ring[i],
            false,
        )?;
    }
    encode_tx(
        layout.tx,
        &mut runtime[0x1a0..],
        input.va[id::DEVCTRL_STATE],
        input.va[id::DEVCTRL_RING],
        true,
    )?;
    let fd = &layout.firmware_data;
    let fw = &mut objects[id::FIRMWARE_DATA];
    put64(fw, fd.fwctl, input.va[id::FWCTL])?;
    put64(fw, fd.fwctl + 8, input.va[id::FWCTL] + 0x40)?;
    put32(fw, fd.scheduler, 1)?;
    put32(objects[id::CONTROL], 0, input.ktrace_mask)?;
    put32(objects[id::ACTIVITY_FRAGMENT], 0xc18, 0xffffffff)?;
    put32(objects[id::ACTIVITY_FRAGMENT], 0xc30, 0xffffffff)?;
    let g = &mut objects[id::GLOBALS];
    for &(at, value) in GLOBALS_FIXED {
        put32(g, remap(layout.globals, at, 4)?, value)?;
    }
    for at in layout.pstate_selectors {
        put32(g, at, 100 * ceiling)?;
    }
    for &(at, value) in layout.globals_policy {
        put32(g, at, value)?;
    }
    let runs = layout.main;
    let main = &mut objects[id::MAIN];
    put64(main, remap(runs, 0x28, 8)?, HOST_END)?;
    if layout.retention == Retention::Main {
        put64(main, remap(runs, 0xe88, 8)?, input.va[id::RETENTION])?;
    }
    if let Some(at) = layout.coredump_copy {
        put64(main, at, input.coredump_copy_size)?;
    }
    for &(at, value) in layout.main_policy {
        put32(main, at, value)?;
    }
    let core_slots = layout.main_core_slots;
    for (at, value) in [
        (0xea0, 1),
        (0xee0, 1),
        (0xeb8, 1),
        (0xed0, 24000),
        (0xed4, 1),
        (0xed8, 8),
        (0xfc0, core_slots.unwrap_or(input.performance.count as u32)),
        (0x259c, 1),
        (0x2544, if core_slots.is_some() { 5 } else { 4 }),
        (0x2548, 6),
        (0x254c, 3),
        (0x2550, 7),
        (0x2554, 7),
        (0x2568, if core_slots.is_some() { 0 } else { 8 }),
        (0x2570, core_slots.map_or(1, |_| input.enabled_cores)),
    ] {
        put32(main, remap(runs, at, 4)?, value)?;
    }
    put32(main, remap(runs, 0xea4, 4)?, input.progress_recovery)?;
    put64(
        main,
        remap(runs, 0xf24, 8)?,
        if core_slots.is_some() {
            4
        } else {
            0x0000000607002004
        },
    )?;
    put32(main, remap(runs, 0xfc4, 4)?, ceiling)?;
    for i in 0..=ceiling as usize {
        put32(
            main,
            remap(runs, 0xfc8 + i * 4, 4)?,
            input.performance.frequency[i],
        )?;
        put32(
            main,
            remap(runs, 0x1808 + i * 4, 4)?,
            input.performance.auxiliary[i],
        )?;
        for core in 0..16 {
            put32(
                main,
                remap(runs, 0x1008 + (i * 16 + core) * 4, 4)?,
                input.performance.voltage[i],
            )?;
            put32(
                main,
                remap(runs, 0x1408 + (i * 16 + core) * 4, 4)?,
                input.performance.sram[i],
            )?;
        }
    }
    for slot in 0..LOADER_COUNT {
        // This ABI has no slot for the record: the host must not have mapped it (`build`
        // skips it), and it is not described.
        let Some(at) = layout.slot(slot) else {
            if input.loader_va[slot] != 0 {
                return Err(());
            }
            continue;
        };
        if at >= layout.register_slots {
            return Err(());
        }
        let mut record = [0u8; LOADER_STRIDE];
        layout.loaders[slot].encode(input.loader_va[slot], &mut record)?;
        let at = 0x640 + at * LOADER_STRIDE;
        main.get_mut(at..at + LOADER_STRIDE)
            .ok_or(())?
            .copy_from_slice(&record);
    }
    for slot in 0..layout.register_slots {
        if layout.empty_loader_modes & (1 << slot) != 0 {
            put64(main, 0x640 + slot * LOADER_STRIDE + 0x20, 2)?;
        }
    }
    let runs = layout.power_performance;
    let p = &mut objects[id::POWER_PERFORMANCE];
    put64(
        p,
        remap(runs, POWER_TEMP_PTR, 8)?,
        input.va[id::TEMPERATURE],
    )?;
    let power_fixed_values = [
        input.power_sample_clks,
        input.power_sample_clks,
        4,
        0x3f800000,
        ceiling,
        ceiling,
    ];
    for (&at, value) in POWER_FIXED_U32.iter().zip(power_fixed_values) {
        put32(p, remap(runs, at, 4)?, value)?;
    }
    for &at in POWER_CEILING_U32 {
        put32(p, remap(runs, at, 4)?, 100 * ceiling)?;
    }
    // Disjointness from every remapped 25G83 word, power_sampling, and power_limiter slot is
    // proved at compile time by layout_fits().
    for &(at, value) in layout.power_policy {
        put32(p, at, value)?;
    }
    if let Some(at) = layout.state_bound {
        put32(p, at, input.performance.count as u32)?;
    }
    if let (Some(sampling), Some((period, samples))) = (layout.power_sampling, sampling_values) {
        for &at in sampling.period {
            put32(p, at, period)?;
        }
        put32(p, sampling.period_f32, f32_bits(period, 1))?;
        let [count, keep, take] = sampling.filter;
        put32(p, count, samples * 4)?;
        put32(p, keep, f32_bits(samples - 1, samples))?;
        put32(p, take, f32_bits(1, samples))?;
    }
    if let (Some(limiter), Some((periods, periods_x4, ki_num, tc_clks))) =
        (layout.power_limiter, limiter_values)
    {
        put32(p, limiter.filter_periods_x4, periods_x4)?;
        put32(p, limiter.filter_a_neg, f32_bits(periods - 1, periods))?;
        put32(p, limiter.filter_a, f32_bits(1, periods))?;
        put32(p, limiter.ki_dt, f32_bits(ki_num, 65536 * 1000))?;
        put32(p, limiter.clamp, f32_bits(65536, 1))?;
        put32(p, limiter.kp, f32_bits(input.lowpeak_limiter_kp_q16, 65536))?;
        put32(p, limiter.filter_tc_ms, input.lowpeak_limiter_tc_ms)?;
        put32(p, limiter.filter_tc_clks, tc_clks)?;
    }
    Ok(())
}
