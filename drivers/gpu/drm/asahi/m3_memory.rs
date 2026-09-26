// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Owned firmware/engine buffers with the qualified M3 page offsets.
use kernel::prelude::*;
use crate::{driver, gem, mmu, pgtable::prot};

enum Backing {
    Coherent(kernel::dma::Coherent<[u8]>),
    Paged(gem::ObjectRef),
}

pub(crate) struct Buffer {
    mapping: mmu::KernelMapping,
    gpu_mapping: Option<mmu::KernelMapping>,
    object: Backing,
    offset: usize,
    size: usize,
}

impl Buffer {
    pub(crate) fn at(dev: &driver::AsahiDevice, fw: &mmu::Vm,
        gpu: Option<(&mmu::Vm,u64)>, address: Option<u64>, size: usize,
        gpu_shared: bool) -> Result<Self> {
        Self::at_prot(dev,fw,gpu,address,size,
            if gpu_shared {prot::PROT_GPU_FW_SHARED_RW} else {prot::PROT_FW_SHARED_RW},
            prot::PROT_GPU_SHARED_RW)
    }
    pub(crate) fn at_prot(dev: &driver::AsahiDevice, fw: &mmu::Vm,
        gpu: Option<(&mmu::Vm,u64)>, address: Option<u64>, size: usize,
        protection: prot::Prot, gpu_protection: prot::Prot) -> Result<Self> {
        let offset=(address.or(gpu.map(|(_,a)|a)).unwrap_or(0)&0x3fff) as usize;
        if size==0 || gpu.is_some_and(|(_,a)| a&0x3fff!=offset as u64) {return Err(EINVAL);}
        let allocated=(size.checked_add(offset).and_then(|n|n.checked_add(0x3fff)).ok_or(EOVERFLOW)?)&!0x3fff;
        // The DMA core tries the default CMA area first and falls back to the
        // page allocator. The graphs outgrow that area, so a CMA miss is the
        // normal case here and is not reported; only a failed fallback is.
        // SAFETY: M3 runtime owns the bound platform device for the entire
        // diagnostic; all mappings retire before normal DMA allocation Drop.
        let object=kernel::dma::Coherent::zeroed_slice_with_attrs(unsafe {dev.as_ref().as_bound()},allocated,GFP_KERNEL,
            kernel::dma::attrs::DMA_ATTR_NO_WARN).inspect_err(|_| dev_err!(dev.as_ref(),"M3: cannot allocate {} KiB of GPU memory\n",allocated/1024))?;
        let pa:usize=object.dma_handle().try_into()?;
        let mapping=if let Some(a)=address {fw.map_io(a&!0x3fff,pa,allocated,protection)?}
            else {fw.map_io_in_range(0xffff_fc2d_0000_0000..0xffff_fc2e_0000_0000,pa,allocated,protection)?};
        let gpu_mapping=if let Some((vm,a))=gpu {Some(vm.map_io(a&!0x3fff,pa,allocated,gpu_protection)?)} else {None};
        Ok(Self {mapping,gpu_mapping,object:Backing::Coherent(object),offset,size})
    }
    /// Allocate owned tiler storage in the driver-reserved GPU range. Keep the
    /// old allocation mapped until the replacement has been fully allocated.
    pub(crate) fn tiler(dev:&driver::AsahiDevice,fw:&mmu::Vm,vm:&mmu::Vm,size:usize)->Result<Self> {
        if size==0 || size.checked_add(mmu::UAT_PGMSK).is_none() {return Err(EINVAL);}
        // Large render targets can require 128 MiB of tiler storage. Engines
        // and firmware address it through UAT, so physically contiguous DMA
        // backing is unnecessary and can exceed the entire CMA reservation.
        // GEM provides zeroed pages and the same WC CPU/device coherency used
        // by M4. Keep both mappings ahead of their backing in drop order.
        let mut object=gem::new_kernel_object_wc(dev,size)?;
        let mapping=object.map_into_range(fw,0xffff_fc2d_0000_0000..0xffff_fc2e_0000_0000,
            mmu::UAT_PGSZ as u64,prot::PROT_GPU_FW_SHARED_RW,true)?;
        let gpu_mapping=Some(object.map_into_range(vm,0x74_0000_0000..0x78_0000_0000,
            mmu::UAT_PGSZ as u64,prot::PROT_GPU_SHARED_RW,true)?);
        Ok(Self {mapping,gpu_mapping,object:Backing::Paged(object),offset:0,size})
    }
    pub(crate) fn va(&self)->u64 {self.mapping.iova()+self.offset as u64}
    pub(crate) fn gpu_va(&self)->Result<u64> {Ok(self.gpu_mapping.as_ref().ok_or(EINVAL)?.iova()+self.offset as u64)}
    pub(crate) fn size(&self)->usize {self.size}
    pub(crate) fn map_gpu_view(&mut self,vm:&mmu::Vm)->Result<mmu::KernelMapping> {
        let address=self.gpu_va()?&!0x3fff;
        match &mut self.object {
            Backing::Coherent(object)=>vm.map_io(address,object.dma_handle().try_into()?,object.size(),prot::PROT_GPU_SHARED_RW),
            Backing::Paged(object)=>object.map_at(vm,address,prot::PROT_GPU_SHARED_RW,true),
        }
    }
    /// The backing retains its CPU mapping until Buffer is dropped. Callers
    /// check the range and do not let this pointer escape the current access.
    fn cpu_ptr(&mut self)->Result<*mut u8> {
        match &mut self.object {
            Backing::Coherent(object)=>Ok(object.as_mut_ptr().cast::<u8>()),
            Backing::Paged(object)=>Ok(object.cached_vmap()?.as_mut_ptr()),
        }
    }
    pub(crate) fn replace_gpu_view(&mut self, mapping: mmu::KernelMapping) {
        self.gpu_mapping = Some(mapping);
    }
    pub(crate) fn swap_gpu_view(&mut self, mapping: mmu::KernelMapping)->Result<mmu::KernelMapping> {
        self.gpu_mapping.replace(mapping).ok_or(EINVAL)
    }
    pub(crate) fn write(&mut self,offset:usize,data:&[u8])->Result {
        if offset.checked_add(data.len()).ok_or(EOVERFLOW)?>self.size {return Err(ERANGE);}
        let ptr=self.cpu_ptr()?;
        unsafe {core::ptr::copy_nonoverlapping(data.as_ptr(),ptr.add(self.offset+offset),data.len())}; Ok(())
    }
    /// Initialize the complete owned span through its retained CPU mapping.
    /// The caller must own the storage or have retired all device consumers.
    pub(crate) fn fill(&mut self, value: u8) -> Result {
        self.fill_range(0,self.size,value)
    }
    /// Reset one unpublished slot without touching other queued device owners.
    pub(crate) fn fill_range(&mut self,offset:usize,size:usize,value:u8)->Result {
        if offset.checked_add(size).ok_or(EOVERFLOW)?>self.size {return Err(ERANGE);}
        let ptr = self.cpu_ptr()?;
        // SAFETY: allocation validates offset + size against its backing, and
        // cpu_ptr retains that entire coherent/WC mapping for this access.
        // Resolve the mapping once, as the G16 buffer clear path does, instead
        // of repeating cached_vmap and memcpy for every 256-byte chunk.
        unsafe { core::ptr::write_bytes(ptr.add(self.offset+offset), value, size) };
        Ok(())
    }
    pub(crate) fn u32(&mut self,offset:usize,value:u32)->Result {
        if (self.offset+offset)&3!=0 || offset.checked_add(4).ok_or(EOVERFLOW)?>self.size {return Err(ERANGE);}
        let ptr=self.cpu_ptr()?;
        // SAFETY: aligned, bounded WC GEM storage retained by this owner.
        unsafe {ptr.add(self.offset+offset).cast::<u32>().write_volatile(value.to_le())};Ok(())
    }
    pub(crate) fn u64(&mut self,offset:usize,value:u64)->Result {self.write(offset,&value.to_le_bytes())}
    pub(crate) fn read(&mut self,offset:usize,data:&mut[u8])->Result {
        if offset.checked_add(data.len()).ok_or(EOVERFLOW)?>self.size {return Err(ERANGE);}
        let ptr=self.cpu_ptr()?;
        // SAFETY: checked WC GEM span; firmware may change the shared bytes.
        unsafe {
            core::arch::asm!("dmb osh",options(nostack,preserves_flags));
            for (i,b) in data.iter_mut().enumerate() {*b=ptr.cast_const().add(self.offset+offset+i).read_volatile();}
        } Ok(())
    }
    pub(crate) fn read_u32(&mut self,offset:usize)->Result<u32> {
        if (self.offset+offset)&3!=0 {let mut b=[0;4];self.read(offset,&mut b)?;return Ok(u32::from_le_bytes(b));}
        if (self.offset+offset)&3!=0 || offset.checked_add(4).ok_or(EOVERFLOW)?>self.size {return Err(ERANGE);}
        let ptr=self.cpu_ptr()?;
        // SAFETY: aligned, checked atomic producer/consumer word in WC RAM.
        let value=unsafe {ptr.cast_const().add(self.offset+offset).cast::<u32>().read_volatile()};
        unsafe {core::arch::asm!("dmb oshld",options(nostack,preserves_flags))};
        Ok(u32::from_le(value))
    }
    pub(crate) fn read_u64(&mut self,offset:usize)->Result<u64> {let mut b=[0;8];self.read(offset,&mut b)?;Ok(u64::from_le_bytes(b))}
}
