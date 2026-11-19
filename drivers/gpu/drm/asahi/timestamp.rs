// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Prepared timestamp destinations shared by native command completion paths.

use crate::mmu;
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
use crate::{file, gem};
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
use core::ops::Range;
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
use core::sync::atomic::{fence as barrier, Ordering};
use kernel::{prelude::*, sync::Arc};
#[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
use kernel::{uapi, xarray};

/// A firmware-mapped timestamp buffer with a kernel CPU mapping of its backing, kept for the
/// buffer's lifetime so that commands need no mapping of their own.
pub(crate) struct FirmwareBuffer {
    mapping: Arc<mmu::KernelMapping>,
    cpu: mmu::TimestampVmap,
    /// Mapped in the firmware's timestamp window: the firmware may write stamps itself.
    window: bool,
}

/// A bound timestamp destination. Only backends which publish firmware writes need a UAT alias.
/// CPU-retired timestamps retain their backing without consuming global firmware address space.
pub(crate) enum Buffer {
    /// The last reference may be a command's, dropped where fence producers run, while
    /// releasing the CPU mapping takes the reservation lock: the deferral runs the release
    /// on the cleanup queue.
    Firmware(crate::cleanup::Deferred<FirmwareBuffer>),
    #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
    Cpu {
        // The GEM pin keeps the object's held SG table alive through CPU publication.
        gem: gem::KernelMappingPin,
        range: Range<usize>,
    },
}

impl Buffer {
    pub(crate) fn firmware(mapping: mmu::KernelMapping) -> Result<Self> {
        Self::new_firmware(mapping, false)
    }

    /// A buffer aliased in the firmware's timestamp window.
    pub(crate) fn firmware_window(mapping: mmu::KernelMapping) -> Result<Self> {
        Self::new_firmware(mapping, true)
    }

    fn new_firmware(mapping: mmu::KernelMapping, window: bool) -> Result<Self> {
        let cpu = mapping.timestamp_vmap()?;
        Ok(Self::Firmware(crate::cleanup::Deferred::new(FirmwareBuffer {
            mapping: Arc::new(mapping, GFP_KERNEL)?,
            cpu,
            window,
        })?))
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
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { .. } => None,
        }
    }

    /// Retain a CPU-written byte range without publishing any firmware address.
    #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
    pub(crate) fn cpu(bo: gem::ObjectRef, range: Range<usize>) -> Result<Self> {
        let size = range.end.checked_sub(range.start).ok_or(EINVAL)?;
        if size == 0 || (range.start | size) & mmu::UAT_PGMSK != 0 || range.end > bo.size() {
            return Err(EINVAL);
        }
        let gem = gem::KernelMappingPin::new(&bo.gem);
        let registered = bo.gem.dev().registration_guard().ok_or(ENODEV)?;
        let parent: &kernel::platform::Device<kernel::device::Bound> = registered.as_ref();
        let _ = gem::held_sg_table(&gem, parent.as_ref())?;
        Ok(Self::Cpu { gem, range })
    }

    pub(crate) fn size(&self) -> usize {
        match self {
            Self::Firmware(buffer) => buffer.mapping.size(),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { range, .. } => range.end - range.start,
        }
    }

    #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
    pub(crate) fn timestamp_mapping(&self, offset: usize) -> Result<mmu::TimestampMapping> {
        match self {
            Self::Firmware(buffer) => buffer.mapping.timestamp_mapping(offset),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { gem, range, .. } => {
                if offset.checked_add(size_of::<u64>()).ok_or(EOVERFLOW)? > self.size() {
                    return Err(ERANGE);
                }
                mmu::TimestampMapping::new(gem, range.start.checked_add(offset).ok_or(EOVERFLOW)?)
            }
        }
    }

    /// The object offset of the timestamp at `offset` of a firmware buffer, for
    /// [`Self::write_timestamp`].
    pub(crate) fn timestamp_offset(&self, offset: usize) -> Result<usize> {
        match self {
            Self::Firmware(buffer) => buffer.mapping.timestamp_offset(offset),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { .. } => Err(EINVAL),
        }
    }

    /// Writes a timestamp at an object offset returned by [`Self::timestamp_offset`],
    /// through the buffer's own CPU mapping.
    pub(crate) fn write_timestamp(&self, offset: usize, value: u64) {
        match self {
            Self::Firmware(buffer) => buffer.cpu.write(offset, value),
            #[cfg(any(CONFIG_DRM_ASAHI_G16, CONFIG_DRM_ASAHI_G18))]
            Self::Cpu { .. } => {}
        }
    }

    /// Legacy command encoders publish firmware pointers and must never accept a CPU-only buffer.
    pub(crate) fn firmware_mapping(&self) -> Result<Arc<mmu::KernelMapping>> {
        match self {
            Self::Firmware(buffer) => Ok(buffer.mapping.clone()),
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
    _mapping: Arc<Buffer>,
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
