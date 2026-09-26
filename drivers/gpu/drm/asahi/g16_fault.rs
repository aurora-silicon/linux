// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Immutable fault-only snapshots of owned G16 firmware memory.
//!
//! Records contain name[32], firmware VA, length, begin/end monotonic ns,
//! then exactly `length` bytes. No MMIO or application BOs are read. Reads
//! are sequential observations, not atomic with running firmware.

use kernel::{prelude::*, time::{ClockSource, Monotonic}};
use crate::g16_memory::Buffer;

pub(crate) struct Dump {
    bytes: KVVec<u8>,
    count: u32,
}

impl Dump {
    pub(crate) fn new(job: u64) -> Result<Self> {
        let mut bytes = KVVec::from_elem(0, 40, GFP_KERNEL)?;
        bytes[..8].copy_from_slice(b"M4FWD001");
        bytes[16..24].copy_from_slice(&job.to_le_bytes());
        bytes[24..32].copy_from_slice(&(Monotonic::ktime_get() as u64).to_le_bytes());
        Ok(Self { bytes, count: 0 })
    }

    pub(crate) fn record(&mut self, name: &str, va: u64, size: usize,
        read: impl FnOnce(&mut [u8]) -> Result) -> Result {
        if name.is_empty() || name.len() >= 32 || size == 0 { return Err(EINVAL); }
        let start = self.bytes.len();
        let end = start.checked_add(64).and_then(|x| x.checked_add(size)).ok_or(EOVERFLOW)?;
        self.bytes.resize(end, 0, GFP_KERNEL)?;
        self.bytes[start..start+name.len()].copy_from_slice(name.as_bytes());
        self.bytes[start+32..start+40].copy_from_slice(&va.to_le_bytes());
        self.bytes[start+40..start+48].copy_from_slice(&(size as u64).to_le_bytes());
        let begin = Monotonic::ktime_get() as u64;
        read(&mut self.bytes[start+64..end])?;
        let finish = Monotonic::ktime_get() as u64;
        self.bytes[start+48..start+56].copy_from_slice(&begin.to_le_bytes());
        self.bytes[start+56..start+64].copy_from_slice(&finish.to_le_bytes());
        self.count = self.count.checked_add(1).ok_or(EOVERFLOW)?;
        Ok(())
    }

    pub(crate) fn buffer(&mut self, name: &str, buffer: &mut Buffer, size: usize) -> Result {
        self.record(name, buffer.va(), size, |out| buffer.read(0, out))
    }

    pub(crate) fn publish(mut self, dev: &kernel::device::Device) -> Result {
        let length = self.bytes.len();
        self.bytes[8..12].copy_from_slice(&self.count.to_le_bytes());
        self.bytes[12..16].copy_from_slice(&1u32.to_le_bytes());
        self.bytes[32..40].copy_from_slice(&(length as u64).to_le_bytes());
        let owned = KBox::new(self, GFP_KERNEL)?;
        kernel::devcoredump::dev_coredump(dev, &crate::THIS_MODULE, owned, GFP_KERNEL,
            kernel::devcoredump::DEFAULT_TIMEOUT);
        dev_info!(dev, "G16G: sealed {}-byte firmware fault snapshot offered to devcoredump (M4FWD001)\n", length);
        Ok(())
    }
}

impl kernel::devcoredump::DevCoreDump for Dump {
    fn read(&self, output: &mut [u8], offset: usize) -> Result<usize> {
        if offset >= self.bytes.len() { return Ok(0); }
        let length = output.len().min(self.bytes.len()-offset);
        output[..length].copy_from_slice(&self.bytes[offset..offset+length]);
        Ok(length)
    }
}
