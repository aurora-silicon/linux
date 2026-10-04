// SPDX-License-Identifier: GPL-2.0-only OR MIT

pub(crate) const INFO_SIZE: usize = 0xb4;
pub(crate) const STATE_SIZE: usize = 0x60;
pub(crate) const DONE: usize = 0x00;
pub(crate) const READ: usize = 0x30;
pub(crate) const WRITE: usize = 0x40;
pub(crate) const CAPACITY: usize = 0x50;
pub(crate) const QUALIFIED_CAPACITY: u32 = 0x500;

mod info {
    pub const STATE: usize = 0x00;
    pub const RING: usize = 0x08;
    pub const JOB_LIST: usize = 0x10;
    pub const GPU_SCRATCH: usize = 0x18;
    pub const EVENT_ID: usize = 0x2c;
    pub const PRIORITY_UNKNOWN_38: usize = 0x38;
    pub const PRIORITY_UNKNOWN_40: usize = 0x40;
    pub const PRIORITY_5: usize = 0x48;
    pub const UNKNOWN_4C: usize = 0x4c;
    pub const UUID: usize = 0x50;
    // V14_8_3 adds a four-byte field before this unaligned pointer.
    pub const CONTEXT: usize = 0xa4;
    pub const UNKNOWN_TAIL: usize = 0xac;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Error { Address, Size, Index }

/// Aligned, non-null firmware reference. Construction at the allocation
/// boundary prevents accidentally passing a low user GPU or compact address.
#[derive(Clone, Copy, Debug)]
pub(crate) struct FirmwareVa(u64);
impl FirmwareVa {
    pub(crate) fn get(self) -> u64 { self.0 }
    pub(crate) fn new(value: u64) -> Result<Self, Error> {
        if value < 0xffff_fc00_0000_0000 || value & 3 != 0 {
            return Err(Error::Address);
        }
        Ok(Self(value))
    }
}

pub(crate) struct QueueInfo {
    pub(crate) state: FirmwareVa,
    pub(crate) ring: FirmwareVa,
    pub(crate) job_list: FirmwareVa,
    pub(crate) gpu_scratch: FirmwareVa,
    pub(crate) context: FirmwareVa,
    pub(crate) uuid: u32,
}

impl QueueInfo {
    /// Initialize exactly INFO_SIZE bytes, preserving any trailing storage.
    /// All validation precedes writes: failure leaves the buffer unchanged.
    pub(crate) fn encode(&self, output: &mut [u8]) -> Result<(), Error> {
        let bytes = output.get_mut(..INFO_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        for (offset, value) in [
            (info::STATE, self.state.0), (info::RING, self.ring.0),
            (info::JOB_LIST, self.job_list.0), (info::GPU_SCRATCH, self.gpu_scratch.0),
            (info::CONTEXT, self.context.0),
            (info::PRIORITY_UNKNOWN_38, 0xffff_ffff_ffff_0000),
            (info::UNKNOWN_TAIL, 4),
        ] { bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes()); }
        for (offset, value) in [
            (info::EVENT_ID, u32::MAX), (info::UNKNOWN_4C, u32::MAX),
            (info::PRIORITY_UNKNOWN_40, 1), (info::PRIORITY_5, 1),
            (info::UUID, self.uuid),
        ] { bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes()); }
        Ok(())
    }
}

pub(crate) struct RingState {
    pub(crate) capacity: u32,
    pub(crate) done: u32,
    pub(crate) read: u32,
    pub(crate) write: u32,
}
impl RingState {
    /// Same no-mutation-on-error and trailing-storage contract as QueueInfo.
    pub(crate) fn encode(&self, output: &mut [u8]) -> Result<(), Error> {
        if self.capacity == 0 || self.capacity > 65536
            || self.done >= self.capacity || self.read >= self.capacity || self.write >= self.capacity {
            return Err(Error::Index);
        }
        let bytes = output.get_mut(..STATE_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        for (offset, value) in [(DONE, self.done), (READ, self.read),
            (WRITE, self.write), (CAPACITY, self.capacity)] {
            bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        }
        Ok(())
    }
}

/// Preserve the offline builder's initial entries and poison unused
/// entries. Runtime starts with write=0 and publishes live entries before
/// advancing WRITE, so no poisoned or seed entry is dispatched accidentally.
pub(crate) fn ring_entry(index: u32, capacity: u32, seeds: &[FirmwareVa]) -> Result<[u8; 8], Error> {
    if seeds.is_empty() || seeds.len() > capacity as usize || capacity > 65536 || index >= capacity { return Err(Error::Index); }
    Ok(if (index as usize) < seeds.len() { seeds[index as usize].0.to_le_bytes() } else { [0xcc; 8] })
}
