// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::m3_queue_layout::{Error, FirmwareVa};
pub(crate) const SIZE: usize = 0x80;
const GPU_APERTURE: u64 = 0x10_0000_0000;
/// A compact pointer is an offset in the qualified low GPU aperture, not a
/// firmware VA. Both subobjects must lie in the same owned GPU allocation.
#[derive(Clone, Copy)]
pub(crate) struct UserBuffer { header: u32, auxiliary: u32 }
impl UserBuffer {
    pub(crate) fn new(gpu: u64, size: usize) -> Result<Self, Error> {
        let base = gpu.checked_sub(GPU_APERTURE).ok_or(Error::Address)?;
        if gpu & 0x3f != 0 || size < 0x904 { return Err(Error::Address); }
        gpu.checked_add(size as u64).ok_or(Error::Address)?;
        let header = u32::try_from(base.checked_add(0x40).ok_or(Error::Address)?)
            .map_err(|_| Error::Address)?;
        let auxiliary = u32::try_from(base.checked_add(0x900).ok_or(Error::Address)?)
            .map_err(|_| Error::Address)?;
        Ok(Self { header, auxiliary })
    }
}
pub(crate) struct Scene {
    /// One u32 entry in the shared 48-entry buffer-manager scene list.
    pub(crate) manager_entry: FirmwareVa,
    pub(crate) manager_misc: FirmwareVa,
    pub(crate) user_buffer: UserBuffer,
}
impl Scene {
    /// All validation precedes mutation; trailing caller storage is untouched.
    /// Called after per-pass reset, never while this slot is firmware-owned.
    pub(crate) fn encode(&self, out: &mut [u8]) -> Result<(), Error> {
        let bytes = out.get_mut(..SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        for offset in [0, 8] {
            bytes[offset..offset + 8].copy_from_slice(&self.manager_entry.get().to_le_bytes());
        }
        bytes[0x28..0x30].copy_from_slice(&u64::from(self.user_buffer.header).to_le_bytes());
        bytes[0x30..0x34].copy_from_slice(&self.user_buffer.auxiliary.to_le_bytes());
        bytes[0x40..0x48].copy_from_slice(&self.manager_misc.get().to_le_bytes());
        Ok(())
    }
}
