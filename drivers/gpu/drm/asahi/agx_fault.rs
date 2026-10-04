// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Immutable fault-only snapshots of owned M3 firmware memory.
//!
//! Records contain name[32], firmware VA, length, begin/end monotonic ns,
//! then exactly `length` bytes. No MMIO or application BOs are read. Reads
//! are sequential observations, not atomic with running firmware.

use kernel::{prelude::*, time::{ClockSource, Monotonic}};

pub(crate) struct Dump {
    bytes: KVVec<u8>,
    count: u32,
    limit: usize,
}

impl Dump {

    /// M3 snapshots have no scheduler job ID yet and contain only bounded
    /// driver-owned firmware configuration and channel storage.
    pub(crate) fn new_m3() -> Result<Self> {
        Self::with_format(0, b"M3FWD001", 2 * 1024 * 1024)
    }

    fn with_format(job: u64, magic: &[u8; 8], limit: usize) -> Result<Self> {
        // Reserve before firmware/job publication, while healthy. Record appends
        // must never enter reclaim waiting on the GPU that just faulted.
        let mut bytes = KVVec::with_capacity(limit, GFP_KERNEL)?;
        bytes.resize(40, 0, GFP_KERNEL)?;
        bytes[..8].copy_from_slice(magic);
        bytes[16..24].copy_from_slice(&job.to_le_bytes());
        bytes[24..32].copy_from_slice(&(Monotonic::ktime_get() as u64).to_le_bytes());
        Ok(Self { bytes, count: 0, limit })
    }

    /// Start the one-shot capture; construction may precede the fault by hours.
    pub(crate) fn begin(&mut self, job: u64) {
        self.bytes[16..24].copy_from_slice(&job.to_le_bytes());
        self.bytes[24..32].copy_from_slice(&(Monotonic::ktime_get() as u64).to_le_bytes());
    }

    pub(crate) fn record(&mut self, name: &str, va: u64, size: usize,
        read: impl FnOnce(&mut [u8]) -> Result) -> Result {
        if name.is_empty() || name.len() >= 32 || size == 0 { return Err(EINVAL); }
        let count = self.count.checked_add(1).ok_or(EOVERFLOW)?;
        let start = self.bytes.len();
        let end = start.checked_add(64).and_then(|x| x.checked_add(size)).ok_or(EOVERFLOW)?;
        if end > self.limit { return Err(E2BIG); }
        // Capacity was reserved at construction. NOWAIT is defense in depth;
        // the checked limit above guarantees resize cannot allocate here.
        self.bytes.resize(end, 0, GFP_NOWAIT)?;
        self.bytes[start..start+name.len()].copy_from_slice(name.as_bytes());
        self.bytes[start+32..start+40].copy_from_slice(&va.to_le_bytes());
        self.bytes[start+40..start+48].copy_from_slice(&(size as u64).to_le_bytes());
        let begin = Monotonic::ktime_get() as u64;
        if let Err(error) = read(&mut self.bytes[start+64..end]) {
            self.bytes.truncate(start);
            return Err(error);
        }
        let finish = Monotonic::ktime_get() as u64;
        self.bytes[start+48..start+56].copy_from_slice(&begin.to_le_bytes());
        self.bytes[start+56..start+64].copy_from_slice(&finish.to_le_bytes());
        self.count = count;
        Ok(())
    }

    pub(crate) fn publish(mut self, dev: &kernel::device::Device) -> Result {
        let length = self.bytes.len();
        self.bytes[8..12].copy_from_slice(&self.count.to_le_bytes());
        self.bytes[12..16].copy_from_slice(&1u32.to_le_bytes());
        self.bytes[32..40].copy_from_slice(&(length as u64).to_le_bytes());
        let format = "M3FWD001";
        let owned = KBox::new(self, GFP_NOWAIT)?;
        kernel::devcoredump::dev_coredump(dev, &crate::THIS_MODULE, owned, GFP_NOWAIT,
            kernel::devcoredump::DEFAULT_TIMEOUT);
        dev_info!(dev, "Asahi: sealed {}-byte firmware fault snapshot offered to devcoredump ({})\n", length, format);
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
