// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::m3_queue_layout::{Error, FirmwareVa};

pub(crate) const INIT_BM_SIZE: usize = 0x20;
pub(crate) const BARRIER_SIZE: usize = 0x3e;
mod init {
    pub const OPCODE: usize = 0;
    pub const CONTEXT: usize = 4;
    pub const SLOT: usize = 8;
    // 0xc: unresolved block_count2, zero in the qualified producer.
    pub const BLOCK_COUNT: usize = 0x10;
    pub const MANAGER: usize = 0x14;
    pub const STAMP: usize = 0x1c;
}
mod barrier {
    pub const OPCODE: usize = 0;
    pub const STAMP: usize = 4;
    pub const STAMP2: usize = 0xc;
    pub const WAIT_VALUE: usize = 0x14;
    pub const EVENT: usize = 0x18;
    pub const SELF_VALUE: usize = 0x1c;
    pub const UUID: usize = 0x20;
    // 0x24: unresolved u16, 0x26: external-barrier u32, both zero.
    pub const INTERNAL_TYPE: usize = 0x2a;
}
fn u32_at(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn address_at(bytes: &mut [u8], offset: usize, value: FirmwareVa) {
    bytes[offset..offset + 8].copy_from_slice(&value.get().to_le_bytes());
}

pub(crate) struct InitBufferManager {
    pub(crate) context: u32,
    pub(crate) slot: u32,
    pub(crate) block_count: u32,
    pub(crate) manager: FirmwareVa,
    pub(crate) stamp: u32,
}
impl InitBufferManager {
    /// Check the output extent before mutation and preserve trailing storage.
    /// Capacity validation belongs to the owner of the block/page allocations.
    pub(crate) fn encode(&self, output: &mut [u8]) -> Result<(), Error> {
        let bytes = output.get_mut(..INIT_BM_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        for (offset, value) in [(init::OPCODE, 6), (init::CONTEXT, self.context),
            (init::SLOT, self.slot), (init::BLOCK_COUNT, self.block_count),
            (init::STAMP, self.stamp)] { u32_at(bytes, offset, value); }
        address_at(bytes, init::MANAGER, self.manager);
        Ok(())
    }
}

#[derive(Clone, Copy)]
pub(crate) enum Dependency {
    /// The fragment queue waits for this pass's tiler completion.
    TilerToFragment,
    /// A queue waits for an earlier pass; qualified G15 internal type is one.
    PreviousPass,
}
pub(crate) struct Barrier {
    pub(crate) stamp: FirmwareVa,
    pub(crate) wait_value: u32,
    pub(crate) event: u32,
    pub(crate) self_value: u32,
    pub(crate) uuid: u32,
    pub(crate) dependency: Dependency,
}
impl Barrier {
    /// Both stamp pointers refer to the same owned firmware stamp, as in all
    /// qualified render paths. No external barrier or arbitrary type is exposed.
    pub(crate) fn encode(&self, output: &mut [u8]) -> Result<(), Error> {
        let bytes = output.get_mut(..BARRIER_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        address_at(bytes, barrier::STAMP, self.stamp);
        address_at(bytes, barrier::STAMP2, self.stamp);
        let kind = match self.dependency { Dependency::TilerToFragment => 0, Dependency::PreviousPass => 1 };
        for (offset, value) in [(barrier::OPCODE, 4), (barrier::WAIT_VALUE, self.wait_value),
            (barrier::EVENT, self.event), (barrier::SELF_VALUE, self.self_value),
            (barrier::UUID, self.uuid), (barrier::INTERNAL_TYPE, kind)] {
            u32_at(bytes, offset, value);
        }
        Ok(())
    }
}
