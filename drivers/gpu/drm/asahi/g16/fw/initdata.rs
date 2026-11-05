// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713/T8132 initdata for the qualified 25G83 firmware.
//!
//! This is a byte encoder for private, unpublished objects. Admission, mapping,
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
pub(crate) const LOADER_BYTES: usize = LOADER_COUNT * LOADER_STRIDE;

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
    pub(crate) const MAGIC: u64 = 0x0c89c357839204b8;
}
/// The small root.Control object is the READY acknowledgement.
pub(crate) mod ready_control {
    pub(crate) const READY: usize = 0x14;
}
/// Runtime is a distinct pointer graph, not the READY object.
pub(crate) mod runtime {
    pub(crate) const SIZE: usize = super::OBJECT_SIZE[super::id::RUNTIME];
}
/// Recovery words belong to FirmwareData, not root.Control.
pub(crate) mod control {
    pub(crate) const SIZE: usize = super::OBJECT_SIZE[super::id::FIRMWARE_DATA];
    pub(crate) const GENERATION: usize = 0x4fa0;
    pub(crate) const PHASE: usize = 0x4fb0;
    pub(crate) const SCHEDULER: usize = 0x4fd4;
}
pub(crate) mod globals {
    pub(crate) const SIZE: usize = super::OBJECT_SIZE[super::id::GLOBALS];
    pub(crate) const PSTATE_SELECTORS: [usize; 3] = [0x88, 0x8c, 0x98];
}
pub(crate) mod hwdata {
    pub(crate) const SIZE: usize = super::OBJECT_SIZE[super::id::MAIN];
}

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

pub(crate) const PERF_STATES: usize = 10;

/// Machine-specific values selected from the original ADT before firmware publication.
/// Frequencies are exact MHz, voltages are millivolts. No reference-bin defaults exist.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Performance {
    pub(crate) frequency: [u32; PERF_STATES],
    pub(crate) auxiliary: [u32; PERF_STATES],
    pub(crate) voltage: [u32; PERF_STATES],
    pub(crate) sram: [u32; PERF_STATES],
}

impl Performance {
    pub(crate) fn valid(&self) -> bool {
        for i in 0..PERF_STATES {
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

pub(crate) fn encode_root(out: &mut [u8], va: &[u64; OBJECT_COUNT]) -> Result<(), ()> {
    for i in 0..OBJECT_COUNT {
        if !object_va(va[i], OBJECT_SIZE[i]) {
            return Err(());
        }
        for j in 0..i {
            if va[i] < va[j] + OBJECT_SIZE[j] as u64 && va[j] < va[i] + OBJECT_SIZE[i] as u64 {
                return Err(());
            }
        }
    }
    let out = out.get_mut(..root::SIZE).ok_or(())?;
    out.fill(0);
    put64(out, 0, root::MAGIC)?;
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

/// Encodes the complete private 31-object graph, byte-for-byte matching the
/// working M4 C encoder. No mapping or publication is performed here.
pub(crate) fn encode(
    objects: &mut [&mut [u8]; OBJECT_COUNT],
    input: &Input,
    ceiling: u32,
) -> Result<(), ()> {
    if !(1..=9).contains(&ceiling)
        || !input.performance.valid()
        || !object_va(input.scratch, 0x8000)
        || !bootstrap_alias(input.lower_uma)
        || !bootstrap_alias(input.lower_parameter)
        || (input.lower_uma < input.lower_parameter + 0x8000
            && input.lower_parameter < input.lower_uma + 0x8000)
    {
        return Err(());
    }
    for (i, obj) in objects.iter().enumerate() {
        if obj.len() < OBJECT_SIZE[i] {
            return Err(());
        }
    }
    encode_root(objects[id::ROOT], &input.va)?;
    for i in 1..OBJECT_COUNT {
        objects[i][..OBJECT_SIZE[i]].fill(0);
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
        (0x469, id::POWER_PERFORMANCE),
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
    let fw = &mut objects[id::FIRMWARE_DATA];
    put64(fw, 0x4f88, input.va[id::FWCTL])?;
    put64(fw, 0x4f90, input.va[id::FWCTL] + 0x40)?;
    put32(fw, 0x4fd4, 1)?;
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
        put32(g, at, value)?;
    }
    for at in globals::PSTATE_SELECTORS {
        put32(g, at, 100 * ceiling)?;
    }
    let main = &mut objects[id::MAIN];
    put64(main, 0x28, HOST_END)?;
    put64(main, 0xe88, input.va[id::RETENTION])?;
    for (at, value) in [
        (0xea0, 1),
        (0xee0, 1),
        (0xeb8, 1),
        (0xed0, 24000),
        (0xed4, 1),
        (0xed8, 8),
        (0xfc0, 10),
        (0x259c, 1),
        (0x2544, 4),
        (0x2548, 6),
        (0x254c, 3),
        (0x2550, 7),
        (0x2554, 7),
        (0x2568, 8),
        (0x2570, 1),
    ] {
        put32(main, at, value)?;
    }
    put32(main, 0xea4, input.progress_recovery)?;
    put64(main, 0xf24, 0x0000000607002004)?;
    put32(main, 0xfc4, ceiling)?;
    for i in 0..=ceiling as usize {
        put32(main, 0xfc8 + i * 4, input.performance.frequency[i])?;
        put32(main, 0x1808 + i * 4, input.performance.auxiliary[i])?;
        for core in 0..16 {
            put32(
                main,
                0x1008 + (i * 16 + core) * 4,
                input.performance.voltage[i],
            )?;
            put32(
                main,
                0x1408 + (i * 16 + core) * 4,
                input.performance.sram[i],
            )?;
        }
    }
    for slot in 0..LOADER_COUNT {
        loader_encode(
            slot,
            input.loader_va[slot],
            &mut main[0x640 + slot * LOADER_STRIDE..],
        )?;
    }
    let p = &mut objects[id::POWER_PERFORMANCE];
    put64(p, 0x3b50, input.va[id::TEMPERATURE])?;
    for (at, value) in [
        (4, 200000),
        (8, 200000),
        (0x10, 4),
        (0x14, 0x3f800000),
        (0x2c, ceiling),
        (0x30, ceiling),
    ] {
        put32(p, at, value)?;
    }
    for at in [
        0x9f0, 0x9f4, 0xa38, 0xaa8, 0xaac, 0xaf0, 0xb78, 0xb7c, 0xb80, 0xbc0, 0x22b0, 0x22b4,
        0x22f8, 0x2210, 0x2870, 0x2a3c, 0x2b84, 0x2b88, 0x2b8c, 0x2bcc, 0x2c40, 0x2c44, 0x2c88,
        0x279e,
    ] {
        put32(p, at, 100 * ceiling)?;
    }
    Ok(())
}
