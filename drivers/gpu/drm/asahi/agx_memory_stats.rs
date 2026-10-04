// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounded host accounting for GEM extents and M3 coherent firmware backing.
//!
//! These are allocation extents, not resident/pinned pages or unique imported
//! physical memory. GEM construction/unwind is included. Counters are per
//! module instance and each read is observational, not a transactional snapshot.
use core::sync::atomic::{AtomicU64, Ordering};
use kernel::prelude::*;

struct Counters {
    objects: AtomicU64,
    bytes: AtomicU64,
    peak_objects: AtomicU64,
    peak_bytes: AtomicU64,
}
impl Counters {
    const fn new() -> Self {
        Self { objects: AtomicU64::new(0), bytes: AtomicU64::new(0),
            peak_objects: AtomicU64::new(0), peak_bytes: AtomicU64::new(0) }
    }
    fn snapshot(&self) -> [u64; 4] {
        let objects = self.objects.load(Ordering::Relaxed);
        let bytes = self.bytes.load(Ordering::Relaxed);
        [objects, bytes, self.peak_objects.load(Ordering::Relaxed).max(objects),
            self.peak_bytes.load(Ordering::Relaxed).max(bytes)]
    }
}
static USER_GEM: Counters = Counters::new();
static KERNEL_GEM: Counters = Counters::new();
static COHERENT: Counters = Counters::new();

/// Drop with the actual backing owner, never with an individual GEM handle or
/// its mapping. Retained fault owners therefore remain included after exit.
pub(crate) struct Allocation {
    counters: &'static Counters,
    bytes: u64,
}
impl Allocation {
    fn new(counters: &'static Counters, size: usize) -> Result<Self> {
        let bytes = size as u64;
        if bytes == 0 { return Err(EINVAL); }
        let previous = counters.bytes.fetch_update(Ordering::Relaxed, Ordering::Relaxed,
            |current| current.checked_add(bytes)).map_err(|_| EOVERFLOW)?;
        let objects = counters.objects.fetch_add(1, Ordering::Relaxed) + 1;
        counters.peak_bytes.fetch_max(previous + bytes, Ordering::Relaxed);
        counters.peak_objects.fetch_max(objects, Ordering::Relaxed);
        Ok(Self { counters, bytes })
    }
    pub(crate) fn gem(size: usize, kernel: bool) -> Result<Self> {
        Self::new(if kernel { &KERNEL_GEM } else { &USER_GEM }, size)
    }
    pub(crate) fn coherent(size: usize) -> Result<Self> { Self::new(&COHERENT, size) }
}
impl Drop for Allocation {
    fn drop(&mut self) {
        self.counters.bytes.fetch_sub(self.bytes, Ordering::Relaxed);
        self.counters.objects.fetch_sub(1, Ordering::Relaxed);
    }
}

pub(crate) struct View;
impl kernel::debugfs::Writer for View {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(f, "version=1 unit=allocation_extent_bytes")?;
        for (name, counters) in [("user_gem", &USER_GEM), ("kernel_gem", &KERNEL_GEM),
                                  ("m3_coherent", &COHERENT)] {
            let [objects, bytes, peak_objects, peak_bytes] = counters.snapshot();
            writeln!(f, "{} objects={} bytes={} peak_objects={} peak_bytes={}",
                name, objects, bytes, peak_objects, peak_bytes)?;
        }
        Ok(())
    }
}
