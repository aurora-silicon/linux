// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Prepared timestamp destinations shared by native command completion paths.

use crate::{gem, mmu, no_debug};
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
use crate::file;
use core::ops::Range;
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
use core::sync::atomic::{fence as barrier, Ordering};
use kernel::prelude::*;
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
use kernel::{sync::Arc, uapi, xarray};

/// The size of a bound range: whole pages, not empty, inside an object of `object_size` bytes.
fn bound_size(range: &Range<usize>, object_size: usize) -> Result<usize> {
    let size = range.end.checked_sub(range.start).ok_or(EINVAL)?;
    if size == 0 || (range.start | size) & mmu::UAT_PGMSK != 0 || range.end > object_size {
        return Err(EINVAL);
    }
    Ok(size)
}

/// The object offset of the timestamp at `offset` of a host buffer bound at `start` for `size`
/// bytes: the whole stamp must lie inside the bound range, which lies inside the object
/// ([`Buffer::host`]).
fn host_offset(start: usize, size: usize, offset: usize) -> Result<usize> {
    if offset.checked_add(size_of::<u64>()).ok_or(EOVERFLOW)? > size {
        return Err(ERANGE);
    }
    start.checked_add(offset).ok_or(EOVERFLOW)
}

/// Permission for a timestamp bind to alias in the backend's firmware timestamp window.
pub(crate) enum WindowPermit {
    /// No per-file window share applies (the backend has no fixed window limit).
    Unrestricted,
    /// The file's reserved share of the firmware timestamp window, held until unmap.
    Share(crate::memacct::WindowShare),
    /// The file's window share is exhausted; fall back to host or refuse with ENOSPC.
    Denied,
}

impl WindowPermit {
    pub(crate) fn is_allowed(&self) -> bool {
        !matches!(self, Self::Denied)
    }

    pub(crate) fn into_share(self) -> Option<crate::memacct::WindowShare> {
        match self {
            Self::Share(share) => Some(share),
            Self::Unrestricted | Self::Denied => None,
        }
    }
}

/// A firmware-mapped timestamp buffer with a kernel CPU mapping of its backing, kept for the
/// buffer's lifetime so that commands need no mapping of their own.
pub(crate) struct FirmwareBuffer {
    mapping: mmu::KernelMapping,
    cpu: mmu::TimestampVmap,
    /// Mapped in the firmware's timestamp window: the firmware may write stamps itself.
    window: bool,
    /// The file's share of the firmware timestamp window, held as the last field so `mapping`
    /// is unmapped on the cleanup queue before the quota is credited back.
    _window_share: Option<crate::memacct::WindowShare>,
}

/// A timestamp buffer only the host writes, through a kernel CPU mapping of its backing. It has
/// no firmware alias, so binding it takes no firmware address space.
pub(crate) struct HostBuffer {
    cpu: mmu::TimestampVmap,
    /// The bound range of the object.
    start: usize,
    size: usize,
}

/// A bound timestamp destination. Only backends which publish firmware writes need a UAT alias.
/// CPU-retired timestamps retain their backing without consuming global firmware address space.
pub(crate) enum Buffer {
    /// The last reference may be a command's, dropped where fence producers run, while
    /// releasing the CPU mapping takes the reservation lock: the deferral runs the release
    /// on the cleanup queue.
    Firmware(crate::cleanup::Deferred<FirmwareBuffer>),
    /// Host-written, with the same deferred release as [`Self::Firmware`].
    Host(crate::cleanup::Deferred<HostBuffer>),
    #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
    Cpu {
        // The GEM pin keeps the object's held SG table alive through CPU publication.
        gem: gem::KernelMappingPin,
        range: Range<usize>,
    },
}

/// A timestamp buffer bound to a file, holding the charge for the kernel memory its records take
/// (see `file::BOUND_OBJECT_BYTES`), so the charge is given back with the last reference.
pub(crate) struct Bound {
    buffer: Buffer,
    _charge: crate::memacct::Commit,
}

impl Bound {
    pub(crate) fn new(buffer: Buffer, charge: crate::memacct::Commit) -> Self {
        Self {
            buffer,
            _charge: charge,
        }
    }
}

impl core::ops::Deref for Bound {
    type Target = Buffer;

    fn deref(&self) -> &Buffer {
        &self.buffer
    }
}

no_debug!(Bound);

/// The larger of two sizes, for the heap bound below.
const fn max(a: usize, b: usize) -> usize {
    if a > b { a } else { b }
}

impl Buffer {
    /// Kernel heap bytes a buffer's own records take, at most: its deferred-release item. A CPU
    /// buffer keeps its records inline.
    pub(crate) const HEAP_BYTES: usize = max(
        crate::cleanup::Deferred::<FirmwareBuffer>::HEAP_BYTES,
        crate::cleanup::Deferred::<HostBuffer>::HEAP_BYTES,
    );

    pub(crate) fn firmware(
        mapping: mmu::KernelMapping,
        window_share: Option<crate::memacct::WindowShare>,
    ) -> Result<Self> {
        Self::new_firmware(mapping, false, window_share)
    }

    /// A buffer aliased in the firmware's timestamp window.
    pub(crate) fn firmware_window(
        mapping: mmu::KernelMapping,
        window_share: Option<crate::memacct::WindowShare>,
    ) -> Result<Self> {
        Self::new_firmware(mapping, true, window_share)
    }

    /// A buffer whose stamps the host writes: the bound range of a firmware-shareable object,
    /// as for a firmware alias, with only a CPU mapping. Not under the device mutex (the CPU
    /// mapping takes the reservation lock).
    pub(crate) fn host(bo: &gem::ObjectRef, range: Range<usize>) -> Result<Self> {
        let size = bound_size(&range, bo.size())?;
        // A firmware alias refuses VM-private objects (gem::validate_vm_binding); so does this.
        if bo.gem.is_vm_private() {
            return Err(EINVAL);
        }
        Ok(Self::Host(crate::cleanup::Deferred::new(HostBuffer {
            cpu: mmu::TimestampVmap::new(&bo.gem)?,
            start: range.start,
            size,
        })?))
    }

    fn new_firmware(
        mapping: mmu::KernelMapping,
        window: bool,
        window_share: Option<crate::memacct::WindowShare>,
    ) -> Result<Self> {
        // Keep `window_share` behind `mapping` on error paths too: `mapping` must unmap
        // before the window share is credited back.
        let cpu = match mapping.timestamp_vmap() {
            Ok(cpu) => cpu,
            Err(err) => {
                drop(mapping);
                return Err(err);
            }
        };
        Ok(Self::Firmware(crate::cleanup::Deferred::new(
            FirmwareBuffer {
                mapping,
                cpu,
                window,
                _window_share: window_share,
            },
        )?))
    }

    /// The window address of the timestamp at `offset`, for a window buffer and an aligned
    /// destination that lies wholly inside the mapping.
    pub(crate) fn window_va(&self, offset: usize) -> Option<u64> {
        match self {
            Self::Firmware(buffer) if buffer.window && offset % size_of::<u64>() == 0 => {
                let end = offset.checked_add(size_of::<u64>())?;
                (end <= buffer.mapping.size()).then(|| buffer.mapping.iova() + offset as u64)
            }
            _ => None,
        }
    }

    /// Reads the aligned timestamp at an object offset returned by [`Self::timestamp_offset`].
    pub(crate) fn read_timestamp(&self, offset: usize) -> Option<u64> {
        match self {
            Self::Firmware(buffer) => buffer.cpu.read(offset),
            Self::Host(buffer) => buffer.cpu.read(offset),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { .. } => None,
        }
    }

    /// Retain a CPU-written byte range without publishing any firmware address.
    #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
    pub(crate) fn cpu(bo: gem::ObjectRef, range: Range<usize>) -> Result<Self> {
        bound_size(&range, bo.size())?;
        let gem = gem::KernelMappingPin::new(&bo.gem);
        let registered = bo.gem.dev().registration_guard().ok_or(ENODEV)?;
        let parent: &kernel::platform::Device<kernel::device::Bound> = registered.as_ref();
        let _ = gem::held_sg_table(&gem, parent.as_ref())?;
        Ok(Self::Cpu { gem, range })
    }

    pub(crate) fn size(&self) -> usize {
        match self {
            Self::Firmware(buffer) => buffer.mapping.size(),
            Self::Host(buffer) => buffer.size,
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { range, .. } => range.end - range.start,
        }
    }

    #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
    pub(crate) fn timestamp_mapping(&self, offset: usize) -> Result<mmu::TimestampMapping> {
        match self {
            Self::Firmware(buffer) => buffer.mapping.timestamp_mapping(offset),
            Self::Host(_) => Err(EINVAL),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { gem, range, .. } => {
                if offset.checked_add(size_of::<u64>()).ok_or(EOVERFLOW)? > self.size() {
                    return Err(ERANGE);
                }
                mmu::TimestampMapping::new(gem, range.start.checked_add(offset).ok_or(EOVERFLOW)?)
            }
        }
    }

    /// The object offset of the timestamp at `offset` of a firmware or host buffer, for
    /// [`Self::write_timestamp`].
    pub(crate) fn timestamp_offset(&self, offset: usize) -> Result<usize> {
        match self {
            Self::Firmware(buffer) => buffer.mapping.timestamp_offset(offset),
            Self::Host(buffer) => host_offset(buffer.start, buffer.size, offset),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { .. } => Err(EINVAL),
        }
    }

    /// Writes a timestamp at an object offset returned by [`Self::timestamp_offset`],
    /// through the buffer's own CPU mapping.
    pub(crate) fn write_timestamp(&self, offset: usize, value: u64) {
        match self {
            Self::Firmware(buffer) => buffer.cpu.write(offset, value),
            Self::Host(buffer) => buffer.cpu.write(offset, value),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { .. } => {}
        }
    }

    /// Firmware-aliased command encoders publish firmware pointers and must never accept a
    /// host- or CPU-only buffer. Returning a borrow keeps the mapping tied to [`Bound`] so no
    /// command can retain the mapping without also keeping its bind charge and window share alive.
    pub(crate) fn firmware_mapping(&self) -> Result<&mmu::KernelMapping> {
        match self {
            Self::Firmware(buffer) => Ok(&buffer.mapping),
            Self::Host(_) => Err(EINVAL),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { .. } => Err(EINVAL),
        }
    }
}

#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
fn checked_offset(offset: u64, size: usize, aligned: bool) -> Result<usize> {
    let offset = usize::try_from(offset).map_err(|_| EINVAL)?;
    let end = offset.checked_add(size_of::<u64>()).ok_or(EINVAL)?;
    if aligned && offset % size_of::<u64>() != 0 {
        return Err(EINVAL);
    }
    if end > size {
        return Err(if aligned { EINVAL } else { ERANGE });
    }
    Ok(offset)
}

#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
struct Destination {
    // Retain the bound range and backing along with its prepared CPU view.
    _mapping: Arc<Bound>,
    cpu: mmu::TimestampMapping,
}

#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
impl Destination {
    fn resolve(
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamp: uapi::drm_asahi_timestamp,
        aligned: bool,
    ) -> Result<Option<Self>> {
        if timestamp.handle == 0 {
            return Ok(None);
        }
        let object = objects
            .lock()
            .get(timestamp.handle.try_into()?)
            .ok_or(ENOENT)?
            .clone();
        let file::Object::TimestampBuffer(mapping) = object;
        let offset = checked_offset(timestamp.offset.into(), mapping.size(), aligned)?;
        let cpu = mapping.timestamp_mapping(offset)?;
        Ok(Some(Self {
            _mapping: mapping,
            cpu,
        }))
    }

    fn write(&self, ticks: u64, frequency_hz: u64) {
        const NSEC_PER_SEC: u64 = 1_000_000_000;
        let ns = (ticks / frequency_hz)
            .wrapping_mul(NSEC_PER_SEC)
            .wrapping_add(ticks % frequency_hz * NSEC_PER_SEC / frequency_hz);
        self.cpu.write(ns);
        barrier(Ordering::Release);
    }
}

/// Timestamp mappings resolved and retained before any command is enqueued.
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
pub(crate) struct Destinations(Option<crate::cleanup::Deferred<[Option<Destination>; 4]>>);

#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
impl Destinations {
    /// Empty placeholder before timestamp destinations are resolved.
    #[cfg(CONFIG_DRM_ASAHI_G16)]
    pub(crate) const fn empty() -> Self {
        Self(None)
    }

    /// No timestamp is written for this command.
    #[allow(dead_code)]
    pub(crate) fn is_empty(&self) -> bool {
        self.0
            .as_deref()
            .is_none_or(|destinations| destinations.iter().all(Option::is_none))
    }

    /// G16 retains its eight-byte destination alignment and EINVAL contract.
    #[cfg(CONFIG_DRM_ASAHI_G16)]
    pub(crate) fn resolve_aligned(
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamps: &[uapi::drm_asahi_timestamps],
    ) -> Result<Self> {
        Self::resolve_inner(objects, timestamps, true)
    }

    /// G18 keeps module 1134's rule: any in-range offset, ERANGE beyond the buffer.
    #[cfg(CONFIG_DRM_ASAHI_G18)]
    pub(crate) fn resolve_unaligned(
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamps: &[uapi::drm_asahi_timestamps],
    ) -> Result<Self> {
        Self::resolve_inner(objects, timestamps, false)
    }

    fn resolve_inner(
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamps: &[uapi::drm_asahi_timestamps],
        aligned: bool,
    ) -> Result<Self> {
        if timestamps.is_empty() || timestamps.len() > 2 {
            return Err(EINVAL);
        }
        let mut destinations = core::array::from_fn(|_| None);
        for (index, timestamp) in timestamps.iter().enumerate() {
            destinations[2 * index] = Destination::resolve(objects, timestamp.start, aligned)?;
            destinations[2 * index + 1] = Destination::resolve(objects, timestamp.end, aligned)?;
        }
        // Empty submissions need no cleanup allocation. Timestamped commands
        // prepare the unique work item before any output fence is published.
        let retained = if destinations.iter().any(Option::is_some) {
            Some(crate::cleanup::Deferred::new(destinations)?)
        } else {
            None
        };
        Ok(Self(retained))
    }

    /// Publish measured timestamps before signalling completion or releasing dependent work.
    pub(crate) fn publish(&self, ticks: [u64; 4], frequency_hz: u64) {
        for (destination, ticks) in self.iter().zip(ticks) {
            if let Some(destination) = destination {
                destination.write(ticks, frequency_hz);
            }
        }
    }

    fn iter(&self) -> core::slice::Iter<'_, Option<Destination>> {
        let destinations: &[Option<Destination>] = self
            .0
            .as_deref()
            .map_or(&[], |destinations| destinations.as_slice());
        destinations.iter()
    }
}

#[kernel::macros::kunit_tests(asahi_timestamp_buffer)]
mod tests {
    use super::*;

    const PAGE: usize = mmu::UAT_PGSZ;

    #[test]
    fn a_bound_range_is_whole_pages_inside_the_object() {
        assert_eq!(bound_size(&(0..PAGE), PAGE), Ok(PAGE));
        assert_eq!(bound_size(&(PAGE..3 * PAGE), 3 * PAGE), Ok(2 * PAGE));
        assert_eq!(bound_size(&(0..0), PAGE), Err(EINVAL));
        assert_eq!(bound_size(&(PAGE..0), PAGE), Err(EINVAL));
        assert_eq!(bound_size(&(0..PAGE / 2), PAGE), Err(EINVAL));
        assert_eq!(bound_size(&(PAGE / 2..PAGE + PAGE / 2), 2 * PAGE), Err(EINVAL));
        assert_eq!(bound_size(&(0..2 * PAGE), PAGE), Err(EINVAL));
        assert_eq!(bound_size(&(PAGE..usize::MAX & !(PAGE - 1)), 2 * PAGE), Err(EINVAL));
    }

    #[test]
    fn a_host_stamp_lies_inside_the_bound_range() {
        // A host buffer bound at the object's second page: offsets are within the bound range
        // and come back as object offsets.
        assert_eq!(host_offset(PAGE, PAGE, 0), Ok(PAGE));
        assert_eq!(host_offset(PAGE, PAGE, PAGE - 8), Ok(2 * PAGE - 8));
        // A stamp reaching past the bound range is refused, even if the object goes on.
        assert_eq!(host_offset(PAGE, PAGE, PAGE - 4), Err(ERANGE));
        assert_eq!(host_offset(PAGE, PAGE, PAGE), Err(ERANGE));
        assert_eq!(host_offset(0, PAGE, usize::MAX - 2), Err(EOVERFLOW));
    }
}
