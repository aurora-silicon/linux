// SPDX-License-Identifier: GPL-2.0-only OR MIT

pub(crate) const ROOT_SIZE: usize = 0xc0;
pub(crate) const RUNTIME_SIZE: usize = 0x4b3;
pub(crate) const CONTROL_SIZE: usize = 0xc3d0;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Error { Address, Bounds, Size }

/// An owned firmware region, not a physical, GPU or compact address. Unlike
/// queue references, byte alignment is intentional here: the qualified runtime
/// record starts at ...7b4d. Packed references must not become aligned loads.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Region { base: u64, size: usize }
impl Region {
    pub(crate) fn new(base: u64, size: usize) -> Result<Self, Error> {
        if base < 0xffff_fc00_0000_0000 || size == 0 || base.checked_add(size as u64).is_none() {
            return Err(Error::Address);
        }
        Ok(Self { base, size })
    }
    pub(crate) fn at(self, offset: usize, width: usize) -> Result<u64, Error> {
        if offset.checked_add(width).ok_or(Error::Bounds)? > self.size { return Err(Error::Bounds); }
        Ok(self.base + offset as u64)
    }
}
fn word(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn pointer(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
mod root {
    pub const VERSION: usize = 0;
    pub const REGION_A: usize = 8;
    pub const RUNTIME: usize = 0x18;
    pub const GLOBALS: usize = 0x20;
    pub const HOST_ALLOCATIONS: usize = 0x2c;
    pub const PAGE_SIZE: usize = 0x30;
    pub const PAGE_BITS: usize = 0x32;
    pub const LEVEL_COUNT: usize = 0x33;
    pub const LEVELS: usize = 0x34;
    pub const LEVEL_STRIDE: usize = 0x20;
    pub const NEW_A: usize = 0xa8;
    pub const CONTROL: usize = 0xb0;
    pub const POWER: usize = 0xb8;
}
pub(crate) struct Root {
    pub(crate) region_a: Region,
    pub(crate) runtime: Region,
    pub(crate) globals: Region,
    pub(crate) new_a: Region,
    pub(crate) control: Region,
    pub(crate) power: Region,
}
impl Root {
    /// All bounds are checked before mutation; trailing storage is preserved.
    pub(crate) fn encode(&self, out: &mut [u8]) -> Result<(), Error> {
        let references = [
            (root::REGION_A, self.region_a.at(0, 0x4000)?),
            (root::RUNTIME, self.runtime.at(0, RUNTIME_SIZE)?),
            (root::GLOBALS, self.globals.at(0, 15868)?),
            (root::NEW_A, self.new_a.at(0, 0x40)?),
            (root::CONTROL, self.control.at(0, CONTROL_SIZE)?),
            (root::POWER, self.power.at(0, 2292)?),
        ];
        let bytes = out.get_mut(..ROOT_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        // Opaque qualified G15/V14_8_3 version tuple, not a semantic version.
        for (i, value) in [0x490u16, 0x8380, 0xe21e, 0xc08].iter().enumerate() {
            bytes[root::VERSION + i * 2..root::VERSION + i * 2 + 2].copy_from_slice(&value.to_le_bytes());
        }
        for (offset, value) in references { pointer(bytes, offset, value); }
        word(bytes, root::HOST_ALLOCATIONS, 1);
        bytes[root::PAGE_SIZE..root::PAGE_SIZE + 2].copy_from_slice(&0x4000u16.to_le_bytes());
        bytes[root::PAGE_BITS] = 14;
        bytes[root::LEVEL_COUNT] = 3;
        for (i, (shift, entries)) in [(36u8, 64u16), (25, 2048), (14, 2048)].iter().enumerate() {
            let start = root::LEVELS + i * root::LEVEL_STRIDE;
            bytes[start..start + 4].copy_from_slice(&[8, 14, 14, *shift]);
            bytes[start + 4..start + 6].copy_from_slice(&entries.to_le_bytes());
            bytes[start + 6..start + 8].copy_from_slice(&0x4000u16.to_le_bytes());
            pointer(bytes, start + 8, 1); // UatLevelInfo.unk_8
            pointer(bytes, start + 16, 0x3ffffffc000); // G15 physical page mask.
            pointer(bytes, start + 24, (u64::from(*entries) - 1) << shift);
        }
        Ok(())
    }
}

#[derive(Clone, Copy)]
pub(crate) struct Channel { pub(crate) state: Region, pub(crate) ring: Region }
mod runtime {
    pub const HARDWARE: usize = 0;
    pub const UNKNOWN_PAIR: [usize; 2] = [8, 16];
    pub const HOST_CHANNELS: usize = 0x18;
    pub const HOST_CHANNEL_COUNT: usize = 13;
    pub const HOST_CHANNEL_STRIDE: usize = 0x20;
    pub const FIRMWARE_CHANNELS: usize = 0x1b8;
    pub const FIRMWARE_CHANNEL_STRIDE: usize = 0x10;
    pub const UNKNOWN_SMALL: usize = 0x1f8;
    pub const HARDWARE_B_VIEWS: [usize; 4] = [0x234, 0x23c, 0x244, 0x24c];
    pub const UNKNOWN_C0: usize = 0x2a8;
    pub const UNKNOWN_C1: [usize; 2] = [0x2b0, 0x2b8];
    pub const UNKNOWN_C3: [usize; 2] = [0x2c0, 0x2c8];
    pub const UNKNOWN_2D0: usize = 0x2d0;
    pub const UNKNOWN_3B0: usize = 0x3b0;
    pub const HARDWARE_A: usize = 0x441;
}
pub(crate) struct RuntimePointers {
    pub(crate) hardware: Region,
    pub(crate) unknown_pair: Region,
    pub(crate) unknown_small: Region,
    pub(crate) unknown_c0: Region,
    pub(crate) unknown_c1: Region,
    pub(crate) unknown_c3: Region,
    /// Twelve TA/3D/CL host channels, DevCtrl, then Event/FWLog/KTrace/Stats.
    pub(crate) channels: [Channel; 17],
}
impl RuntimePointers {
    pub(crate) fn encode(&self, out: &mut [u8]) -> Result<(), Error> {
        let hardware = self.hardware.at(0, 35332)?;
        let pair = self.unknown_pair.at(0, 0x100)?;
        let small = self.unknown_small.at(0, 0x10)?;
        let c0 = self.unknown_c0.at(0, 0x1000)?;
        let c1 = self.unknown_c1.at(0, 0x4000)?;
        let c3 = self.unknown_c3.at(0, 0x4000)?;
        let mut channels = [[0u64; 4]; 17];
        for (index, channel) in self.channels.iter().enumerate() {
            channels[index] = [channel.state.at(0, 0x30)?, channel.state.at(0x10, 4)?,
                channel.state.at(0x20, 4)?, channel.ring.at(0, 1)?];
        }
        let bytes = out.get_mut(..RUNTIME_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        pointer(bytes, runtime::HARDWARE, hardware);
        for offset in runtime::UNKNOWN_PAIR { pointer(bytes, offset, pair); }
        for (index, values) in channels.iter().enumerate() {
            if index < runtime::HOST_CHANNEL_COUNT {
                let start = runtime::HOST_CHANNELS + index * runtime::HOST_CHANNEL_STRIDE;
                for (field, value) in values.iter().enumerate() { pointer(bytes, start + field * 8, *value); }
            } else {
                let start = runtime::FIRMWARE_CHANNELS + (index - runtime::HOST_CHANNEL_COUNT) * runtime::FIRMWARE_CHANNEL_STRIDE;
                pointer(bytes, start, values[0]);
                pointer(bytes, start + 8, values[3]);
            }
        }
        pointer(bytes, runtime::UNKNOWN_SMALL, small);
        // These are views of the retained hardware data, not new allocations.
        for (offset, delta) in runtime::HARDWARE_B_VIEWS.iter().zip([0x1880u64, 0x24c0, 0x3740, 0x8900]) {
            pointer(bytes, *offset, hardware + delta);
        }
        pointer(bytes, runtime::UNKNOWN_C0, c0);
        for offset in runtime::UNKNOWN_C1 { pointer(bytes, offset, c1); }
        for offset in runtime::UNKNOWN_C3 { pointer(bytes, offset, c3); }
        word(bytes, runtime::UNKNOWN_2D0, 4);
        word(bytes, runtime::UNKNOWN_3B0, 0xff);
        pointer(bytes, runtime::HARDWARE_A, hardware + 0x4580);
        Ok(())
    }
}

mod control {
    pub const UNKNOWN_4: usize = 4;
    pub const CHANNEL_STATE: usize = 0x4568;
    pub const CHANNEL_RING: usize = 0x4570;
    pub const UNKNOWN_45C4: usize = 0x45c4;
}
pub(crate) struct ControlRegion { pub(crate) channel: Channel }
impl ControlRegion {
    pub(crate) fn encode(&self, out: &mut [u8]) -> Result<(), Error> {
        let state = self.channel.state.at(0, 0x30)?;
        let ring = self.channel.ring.at(0, 1)?;
        let bytes = out.get_mut(..CONTROL_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        word(bytes, control::UNKNOWN_4, 1);
        pointer(bytes, control::CHANNEL_STATE, state);
        pointer(bytes, control::CHANNEL_RING, ring);
        word(bytes, control::UNKNOWN_45C4, 1);
        Ok(())
    }
}
