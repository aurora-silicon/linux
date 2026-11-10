// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Kernel-owned, write-combined GEM objects mapped at one GPU address.

use core::ops::Range;
use kernel::prelude::*;

use crate::{driver::AsahiDrmDevice, gem, mmu};

/// A zeroed, write-combined kernel object and its owning GPU mapping.
pub(crate) struct MappedObject {
    _mapping: mmu::KernelMapping,
    object: gem::ObjectRef,
    iova: u64,
    /// Size of the object in bytes, as requested.
    pub(crate) logical_size: usize,
    /// The VM of the owning mapping.
    home: u64,
}

impl MappedObject {
    fn new_object(dev: &AsahiDrmDevice, size: usize) -> Result<gem::ObjectRef> {
        let mut object = gem::new_kernel_object_wc(dev, size)?;
        let len = object.size();
        let vmap = object.vmap()?;
        // SAFETY: the vmap covers the whole page-rounded allocation.
        unsafe { core::ptr::write_bytes(vmap.addr.cast::<u8>(), 0, len) };
        Ok(object)
    }

    fn from_mapping(
        object: gem::ObjectRef, vm: &mmu::Vm, mapping: mmu::KernelMapping, size: usize,
    ) -> Self {
        let iova = mapping.iova();
        Self { _mapping: mapping, object, iova, logical_size: size, home: vm.id() }
    }

    /// Maps a new object of `size` bytes at `address`.
    pub(crate) fn new_wc_at(
        dev: &AsahiDrmDevice, vm: &mmu::Vm, address: u64, size: usize, prot: mmu::Prot,
    ) -> Result<Self> {
        let mut object = Self::new_object(dev, size)?;
        let mapping = object.map_at(vm, address, prot, false)?;
        Ok(Self::from_mapping(object, vm, mapping, size))
    }

    /// Maps a new object of `size` bytes at a free address in `range`.
    pub(crate) fn new_wc_in_range(
        dev: &AsahiDrmDevice, vm: &mmu::Vm, range: Range<u64>, size: usize, alignment: u64,
        prot: mmu::Prot,
    ) -> Result<Self> {
        let mut object = Self::new_object(dev, size)?;
        let mapping = object.map_into_range(vm, range, alignment, prot, true)?;
        Ok(Self::from_mapping(object, vm, mapping, size))
    }

    /// As [`Self::new_wc_in_range`], followed by `guard_pages` unmapped pages.
    pub(crate) fn new_wc_in_range_with_guards(
        dev: &AsahiDrmDevice, vm: &mmu::Vm, range: Range<u64>, size: usize, alignment: u64,
        prot: mmu::Prot, guard_pages: usize,
    ) -> Result<Self> {
        let object = Self::new_object(dev, size)?;
        let guard = guard_pages.checked_mul(mmu::UAT_PGSZ).ok_or(EOVERFLOW)?;
        let mapping = vm.map_in_range_with_guard_size(
            &object.gem, 0..object.size(), alignment, range, prot, guard,
        )?;
        Ok(Self::from_mapping(object, vm, mapping, size))
    }

    /// Maps this object again at `address` of `vm`.
    pub(crate) fn map_alias_at(
        &mut self, vm: &mmu::Vm, address: u64, prot: mmu::Prot,
    ) -> Result<mmu::KernelMapping> {
        self.object.map_at(vm, address, prot, false)
    }

    /// Maps this object at its own address in another client VM, followed by `guard_pages`
    /// unmapped pages. In the VM of the owning mapping, where the object already is at that
    /// address, there is nothing to map.
    pub(crate) fn map_alias_at_with_guards(
        &mut self, vm: &mmu::Vm, address: u64, prot: mmu::Prot, guard_pages: usize,
    ) -> Result<Option<mmu::KernelMapping>> {
        if vm.id() == self.home {
            return if address == self.iova { Ok(None) } else { Err(EINVAL) };
        }
        let guard = guard_pages.checked_mul(mmu::UAT_PGSZ).ok_or(EOVERFLOW)?;
        vm.map_at_with_guard_size(address, self.object.size(), &self.object.gem, prot, guard)
            .map(Some)
    }

    /// GPU address of the object.
    pub(crate) fn iova(&self) -> u64 {
        self.iova
    }

    /// Runs `f` on the CPU view of the object's `size` bytes.
    pub(crate) fn with_bytes_mut<R>(&mut self, f: impl FnOnce(&mut [u8]) -> Result<R>) -> Result<R> {
        let size = self.logical_size;
        let vmap = self.object.vmap()?;
        // SAFETY: the vmap covers the whole allocation, which is at least `size` bytes, and
        // `&mut self` excludes other CPU views made through this object.
        let bytes = unsafe { core::slice::from_raw_parts_mut(vmap.addr.cast::<u8>(), size) };
        f(bytes)
    }

    fn offset(&self, address: u64, width: usize) -> Result<usize> {
        let offset: usize = address.checked_sub(self.iova).ok_or(EINVAL)?.try_into()?;
        let end = offset.checked_add(width).ok_or(EINVAL)?;
        if end > self.logical_size {
            return Err(EINVAL);
        }
        Ok(offset)
    }

    /// Reads the little-endian word at GPU address `address` (any alignment).
    pub(crate) fn read_u32(&mut self, address: u64) -> Result<u32> {
        let offset = self.offset(address, 4)?;
        self.with_bytes_mut(|bytes| {
            Ok(u32::from_le_bytes(bytes[offset..offset + 4].try_into().map_err(|_| EINVAL)?))
        })
    }

    /// Reads the little-endian double word at GPU address `address` (any alignment).
    pub(crate) fn read_u64(&mut self, address: u64) -> Result<u64> {
        let offset = self.offset(address, 8)?;
        self.with_bytes_mut(|bytes| {
            Ok(u64::from_le_bytes(bytes[offset..offset + 8].try_into().map_err(|_| EINVAL)?))
        })
    }

    /// Writes the naturally aligned little-endian word at GPU address `address` with one store.
    pub(crate) fn write_u32(&mut self, address: u64, value: u32) -> Result {
        let offset = self.offset(address, 4)?;
        if offset % 4 != 0 {
            return Err(EINVAL);
        }
        self.with_bytes_mut(|bytes| {
            // SAFETY: checked bounds and natural alignment.
            unsafe {
                core::ptr::write_volatile(bytes.as_mut_ptr().add(offset).cast::<u32>(), value.to_le())
            };
            Ok(())
        })
    }
}
