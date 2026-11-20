// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713/T8132 initdata for the qualified 25G83 firmware and the 26B123 firmware.
//!
//! The encoder is written in 25G83 offsets; a [`Layout`] maps them to another firmware ABI
//! (26B123 inserts and removes words in several objects, renumbers the register map and
//! moves the FirmwareData words). This is a byte encoder for private, unpublished objects. Admission, mapping,
//! cache maintenance and publication belong to the owner. It has no kernel or
//! allocator dependency so a host harness can compare every byte with the C
//! bring-up encoder.

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

/// Where the `len`-byte field at 25G83 offset `at` lives under `runs`. A field outside every
/// run (an offset whose new position is not known, or the register map) is refused.
pub(crate) fn remap(runs: &[Run], at: usize, len: usize) -> Result<usize, ()> {
    let end = at.checked_add(len).ok_or(())?;
    runs.iter()
        .find(|r| r.start <= at && end <= r.end)
        .map(|r| at + r.delta)
        .ok_or(())
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

/// The InitData ABI of one firmware build: what differs from the 25G83 objects the encoder
/// writes. Offsets of every run are 25G83 offsets.
pub(crate) struct Layout {
    pub(crate) name: &'static str,
    /// Root +0: the interface version the firmware compares with its own.
    pub(crate) version: u64,
    pub(crate) object_size: [usize; OBJECT_COUNT],
    /// RUNTIME (main config) offset of the unaligned power-config (POWER_PERFORMANCE) pointer.
    pub(crate) power_config: usize,
    /// Register-map slot in MAIN of each logical (25G83-numbered) loader record; `None` for a
    /// record that is empty, and the number of slots of the map at MAIN + 0x640.
    pub(crate) loader_slot: [Option<u8>; LOADER_COUNT],
    pub(crate) register_slots: usize,
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
}

/// Size of the host's crash copy buffer (`coredump-copy-buffer` in the 27.2 nub); macOS 27.2
/// hands the J773g firmware a buffer of this size on the crash-log endpoint.
pub(crate) const COREDUMP_COPY_SIZE: u64 = 0x8000;

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
            .filter(|&slot| LOADERS[slot].length != 0)
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
    loader_slot: identity_slots(),
    register_slots: 53,
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
};

/// The 26B123 InitData ABI (macOS 27.2): the same 31 objects and pointers, with
/// - the main-config power-config pointer 16 bytes later;
/// - a 60-slot register map in MAIN, then the retention pointer at 0xfa0, a new u64 at 0xfa8
///   (left zero), the old 0xe90.. words +0x120, 0xec0.. +0x11c (one word removed at 0xebc),
///   0x2600.. +0x120 and the last bytes +0x124 (new u32s at 0x275c and 0x277e, left zero);
/// - a u32 inserted at GLOBALS 0x38 (left zero), so every later word moves by four;
/// - POWER_PERFORMANCE with words inserted at 0x9c (0x10 bytes, left zero), 0x28b0 (0x18),
///   0x3b50 (0x28) and 0x63cc (0x680), 8 bytes removed between 0x2d08 and 0x2f08 and 0x20
///   between 0x62c4 and 0x6318 (positions inferred from the T6041 build of this firmware);
/// - FirmwareData words 0x1e0 bytes later (a table was inserted before them);
/// - POWER (left all zero) grown to 0x694 bytes.
pub(crate) const V26B123: Layout = Layout {
    name: "26B123",
    version: 0x0c8a836f039204b8,
    object_size: [
        0xc8, 0x4000, 0x4b8, 0xe4c, 0x20, 0xedd0, 0x698, 0x4000, 0x2788, 0x6c48, 0x88, 0xc18,
        0x1248, 0xe10, 0x60, 0x1b0, 0x28800, 0x79800, 0x4000, 0x30, 0x4000, 0x30, 0x9000, 0x30,
        0x4800, 0x4, 0x30, 0x4800, 0x4000, 0x4000, 0x4000,
    ],
    power_config: 0x479,
    loader_slot: slots_26b123(),
    register_slots: 60,
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
    // HW data +0xfa8 stays zero on G16G. Non-zero, it turns on the firmware's RTKit power
    // path method (0x2c7a8 -> the auth pointer at DATA 0x8c0c8, TEXT 0x43e4c): on the J713 the
    // firmware then faulted on resume at fw-text PA + 0x6f8, the start code's first fetch
    // after MMU-on, as soon as a doorbell woke it. The T6041 build pairs 0x8000 there with a
    // dedicated 0x8000 window at +0xfa0, where G16G keeps its RAM retention object; what macOS
    // writes here on J773g is open (trace list).
    coredump_copy: None,
};

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

/// A layout's words fit its objects and do not overlap each other.
const fn layout_fits(layout: &Layout) -> bool {
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
}
const _: () = assert!(layout_fits(&V25G83) && layout_fits(&V26B123));

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

/// Actual mapped span: slot 3 has two discontiguous physical banks.
pub(crate) fn loader_span(slot: usize, bank: usize) -> Result<(u64, u32, u32), ()> {
    let e = LOADERS.get(slot).ok_or(())?;
    if e.length == 0 || bank > if slot == 3 { 1 } else { 0 } {
        return Err(());
    }
    let offset = e.pa & (PAGE - 1);
    let bytes = if slot == 3 {
        0x18000
    } else {
        u64::from(e.length)
    };
    let size = (offset + bytes + PAGE - 1) & !(PAGE - 1);
    Ok((
        (e.pa & PA_MASK) + if bank != 0 { 0x2000000 } else { 0 },
        size as u32,
        if bank != 0 { 0x18000 } else { 0 },
    ))
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
    let e = LOADERS.get(slot).ok_or(())?;
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
    pub(crate) performance: Performance,
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
    let bytes = if control { 0x4000 } else { 0x1800 };
    if !object_va(state, 0x30)
        || !object_va(ring, bytes)
        || (state < ring + bytes as u64 && ring < state + 0x30)
    {
        return Err(());
    }
    put64(out, 0, state)?;
    put64(out, 8, state + 0x10)?;
    put64(out, 0x10, state + 0x20)?;
    put64(out, 0x18, ring)
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
        || !object_va(input.scratch, 0x8000)
        || !bootstrap_alias(input.lower_uma)
        || !bootstrap_alias(input.lower_parameter)
        || (input.lower_uma < input.lower_parameter + 0x8000
            && input.lower_parameter < input.lower_uma + 0x8000)
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
    put64(runtime, 0x18, input.scratch)?;
    put64(runtime, 0x2d0, input.lower_parameter)?;
    put64(runtime, 0x2e0, input.lower_uma)?;
    put32(runtime, 0x250, 1)?;
    put32(runtime, 0x2f8, 4)?;
    for i in 0..12 {
        encode_channel(
            &mut runtime[0x20 + i * 0x20..],
            input.channel_state[i],
            input.channel_ring[i],
            false,
        )?;
    }
    encode_channel(
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
    for (at, value) in [
        (0x78, 1),
        (0xe28, 3),
        (0x24, 3000),
        (0x998, 40),
        (0x99c, 10),
        (0x9a0, 250),
        (0x9b8, 2),
        (0x9bc, 40),
        (0x9c0, 5),
        (0x9c8, 40),
        (0x9cc, 50),
    ] {
        put32(g, remap(layout.globals, at, 4)?, value)?;
    }
    for at in layout.pstate_selectors {
        put32(g, at, 100 * ceiling)?;
    }
    let runs = layout.main;
    let main = &mut objects[id::MAIN];
    put64(main, remap(runs, 0x28, 8)?, HOST_END)?;
    put64(main, remap(runs, 0xe88, 8)?, input.va[id::RETENTION])?;
    if let Some(at) = layout.coredump_copy {
        put64(main, at, COREDUMP_COPY_SIZE)?;
    }
    for (at, value) in [
        (0xea0, 1),
        (0xee0, 1),
        (0xeb8, 1),
        (0xed0, 24000),
        (0xed4, 1),
        (0xed8, 8),
        (0xfc0, input.performance.count as u32),
        (0x259c, 1),
        (0x2544, 4),
        (0x2548, 6),
        (0x254c, 3),
        (0x2550, 7),
        (0x2554, 7),
        (0x2568, 8),
        (0x2570, 1),
    ] {
        put32(main, remap(runs, at, 4)?, value)?;
    }
    put32(main, remap(runs, 0xea4, 4)?, input.progress_recovery)?;
    put64(main, remap(runs, 0xf24, 8)?, 0x0000000607002004)?;
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
        let mut record = [0u8; LOADER_STRIDE];
        loader_encode(slot, input.loader_va[slot], &mut record)?;
        match layout.slot(slot) {
            Some(at) if at < layout.register_slots => {
                let at = 0x640 + at * LOADER_STRIDE;
                main.get_mut(at..at + LOADER_STRIDE)
                    .ok_or(())?
                    .copy_from_slice(&record);
            }
            // A record this ABI has no slot for must be empty.
            _ if record == [0; LOADER_STRIDE] => {}
            _ => return Err(()),
        }
    }
    let runs = layout.power_performance;
    let p = &mut objects[id::POWER_PERFORMANCE];
    put64(p, remap(runs, 0x3b50, 8)?, input.va[id::TEMPERATURE])?;
    for (at, value) in [
        (4, 200000),
        (8, 200000),
        (0x10, 4),
        (0x14, 0x3f800000),
        (0x2c, ceiling),
        (0x30, ceiling),
    ] {
        put32(p, remap(runs, at, 4)?, value)?;
    }
    for at in [
        0x9f0, 0x9f4, 0xa38, 0xaa8, 0xaac, 0xaf0, 0xb78, 0xb7c, 0xb80, 0xbc0, 0x22b0, 0x22b4,
        0x22f8, 0x2210, 0x2870, 0x2a3c, 0x2b84, 0x2b88, 0x2b8c, 0x2bcc, 0x2c40, 0x2c44, 0x2c88,
        0x279e,
    ] {
        put32(p, remap(runs, at, 4)?, 100 * ceiling)?;
    }
    Ok(())
}
