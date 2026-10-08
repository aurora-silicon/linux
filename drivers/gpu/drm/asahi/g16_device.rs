// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::{
    bindings, c_str,
    device::Core,
    devres::Devres,
    error::to_result,
    io::{
        mem::{ExclusiveIoMem, IoMem},
        Io,
    },
    platform,
    prelude::*,
    sync::aref::ARef,
    time::{delay::fsleep, Delta, Instant, Monotonic},
};

use crate::g16_firmware::Firmware;

const ASC_CPU_CONTROL: usize = 0x44;
const ASC_CPU_RUN: u32 = 1 << 4;

pub(crate) struct Device {
    dev: ARef<platform::Device>,
    asc: Pin<KBox<Devres<ExclusiveIoMem<0x4000>>>>,
    sgx: Pin<KBox<Devres<IoMem>>>,
    firmware: Firmware,
    power_vote: bool,
    core_mask: u32,
}

impl Device {
    pub(crate) fn new(pdev: &platform::Device<Core>, firmware: Firmware) -> Result<Self> {
        let asc = KBox::pin_init(
            pdev.io_request_by_name(c_str!("asc"))
                .ok_or(EINVAL)?
                .iomap_exclusive_sized::<0x4000>(),
            GFP_KERNEL,
        )?;
        let sgx = KBox::pin_init(
            pdev.io_request_by_name(c_str!("sgx"))
                .ok_or(EINVAL)?
                .iomap(),
            GFP_KERNEL,
        )?;
        // SAFETY: the platform device is live for the entire call. The C API
        // creates a managed supplier link to the declared, ready PMP driver.
        to_result(unsafe { bindings::apple_pmp_link_device(pdev.as_ref().as_raw()) })?;
        if asc.access(pdev.as_ref())?.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            dev_err!(
                pdev.as_ref(),
                "G16G: ASC already running; refusing to take ownership\n"
            );
            return Err(EBUSY);
        }
        let mut device = Self {
            dev: pdev.into(),
            asc,
            sgx,
            firmware,
            power_vote: false,
            core_mask: 0,
        };
        // SAFETY: PMP lifetime is linked above. Command 0xf, logical device 5
        // selects GFX; the bridge waits for ACK.
        let board = device.firmware.board;
        to_result(unsafe { bindings::apple_pmp_set_device_power(0x0f, board.pmp_device, 1) })?;
        device.power_vote = true;
        let registers = device.sgx.access(pdev.as_ref())?;
        let version = registers.try_read32(0xd04000)?;
        let counts = registers.try_read32(0xd04010)?;
        let core_mask = registers.try_read32(0xe01500)?;
        dev_info!(pdev.as_ref(), "G16G: {} power acknowledged, version={:#010x} counts={:#010x} core-mask={:#x}, firmware={}\n",
            board.name, version, counts, core_mask, device.firmware.version());
        if version != board.sgx_version
            || counts != board.sgx_counts
            || core_mask == 0
            || core_mask & !board.core_mask != 0
        {
            return Err(ENODEV);
        }
        device.core_mask = core_mask;
        Ok(device)
    }

    pub(crate) fn core_mask(&self) -> u32 { self.core_mask }

    pub(crate) fn check_drm(&self, pdev: &platform::Device<Core>) -> Result {
        use kernel::dma::{Device as _, DmaMask};
        // SAFETY: This is the admitted T8132's 42-bit physical DMA capability.
        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(42)?)? };
        crate::mmu::check_handoff_guard()?;
        dev_info!(pdev.as_ref(), "G16G: private-memory handoff guard passed success and both timeout paths\n");
        for _ in 0..2 {
            let drm: ARef<crate::driver::AsahiDevice> = kernel::drm::Device::new(
                pdev.as_ref(), crate::driver::AsahiData::new(pdev, None, true))?;
            if drm.gpu().is_ok() { return Err(EIO); }
            let mut object = crate::gem::new_kernel_object(&drm, 0x4000)?;
            object.vmap()?.memset(0);
            drop(object);
            drop(drm);
        }
        dev_info!(pdev.as_ref(), "G16G: initialized DRM data and GEM allocation/cleanup passed twice; backend absent, device unregistered\n");
        Ok(())
    }

    pub(crate) fn check_mmu(&self, pdev: &platform::Device<Core>) -> Result {
        use crate::{driver, gem, mmu, pgtable::prot};
        if self.asc.access(pdev.as_ref())?.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 { return Err(EBUSY); }
        let drm: ARef<driver::AsahiDevice> = kernel::drm::Device::new(
            pdev.as_ref(), driver::AsahiData::new(pdev, None, true))?;
        for _ in 0..2 {
            // SAFETY: Device owns power and ASC control, ASC is stopped, and
            // this UAT and every mapping are destroyed before starting RTKit.
            let uat = unsafe { mmu::Uat::new_j613_25g83(&drm, &self.firmware) }?;
            let range = 0x1_0000_0000..0x1_2000_0000;
            let vm_a = uat.new_vm(0x8132_0010, range.clone())?;
            let vm_b = uat.new_vm(0x8132_0011, range.clone())?;
            let object_a = gem::new_kernel_object(&drm, 0x4000)?;
            let object_b = gem::new_kernel_object(&drm, 0x4000)?;
            let map_a = vm_a.map_at(range.start, 0x4000, object_a.gem.clone(), prot::PROT_GPU_SHARED_RW, false)?;
            let map_b = vm_b.map_at(range.start, 0x4000, object_b.gem.clone(), prot::PROT_GPU_SHARED_RO, false)?;
            let bind_a = uat.bind(&vm_a)?;
            let bind_b = uat.bind(&vm_b)?;
            let mut roots = [(0, 0); 64];
            vm_a.context_roots(&mut roots)?;
            let mask = ((1u64 << 42) - 1) & !0x3fff;
            if bind_a.slot() == bind_b.slot()
                || bind_a.root() == bind_b.root()
                || roots[bind_a.slot() as usize].0 & mask != bind_a.root()
                || roots[bind_b.slot() as usize].0 & mask != bind_b.root()
                || vm_a.translate_iova(range.start)? == vm_b.translate_iova(range.start)?
                || !vm_a.covers_range(range.start, 0x4000, true, true)
                || !vm_b.covers_range(range.start, 0x4000, true, false)
                || vm_b.covers_range(range.start, 0x4000, false, true) { return Err(EIO); }
            dev_info!(pdev.as_ref(), "G16G: common VM isolation verified at VA={:#x}, distinct roots and contexts {}/{}\n", range.start, bind_a.slot(), bind_b.slot());
            drop(map_a);
            drop(map_b);
            if vm_a.covers_range(range.start, 0x4000, false, false)
                || vm_b.covers_range(range.start, 0x4000, false, false) { return Err(EIO); }
            drop(bind_a);
            drop(bind_b);
            drop(vm_a);
            drop(vm_b);
            drop(object_a);
            drop(object_b);
            drop(uat);
        }
        dev_info!(pdev.as_ref(), "G16G: common UAT/GEM/VM lifecycle passed twice; ASC remained stopped\n");
        Ok(())
    }

    pub(crate) fn check_tables(&self, pdev: &platform::Device<Core>) -> Result {
        use crate::{pgtable::{UatPageTable, prot}, pgtable_memory::ReservedTables, uat::UatGeometry};
        use kernel::{io::mem::{Mem, MemFlag}, page::Page};
        use core::sync::atomic::{AtomicU64, Ordering};
        if self.asc.access(pdev.as_ref())?.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            return Err(EBUSY);
        }
        let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
        // SAFETY: Validated reserved RAM; ASC is stopped. This is a read-only
        // check that no context has been installed before touching tables.
        let ttbs = unsafe { Mem::try_new(node.reserved_mem_region_to_resource_byname(c_str!("ttbs"))?, MemFlag::WB.into()) }?;
        let contexts = unsafe { core::slice::from_raw_parts(ttbs.ptr().cast::<AtomicU64>(), 128) };
        if contexts.iter().any(|entry| entry.load(Ordering::Acquire) != 0) {
            return Err(EBUSY);
        }
        let make_tables = || -> Result<ReservedTables> {
            let mut resources = KVec::new();
            for name in [c_str!("pagetables"), c_str!("shared-l2")] {
                resources.push(node.reserved_mem_region_to_resource_byname(name)?, GFP_KERNEL)?;
            }
            // SAFETY: Device::new admitted the board's reserved no-map regions
            // and exclusive ASC control. No context or running firmware can
            // modify the tables throughout this synchronous check.
            unsafe { ReservedTables::new(resources) }
        };
        let snapshot = |tables: &ReservedTables| -> Result<KVec<u64>> {
            let mut words = KVec::new();
            for index in [1, 3] {
                let region = self.firmware.resources.regions[index];
                for address in (region.base..region.base + region.size).step_by(0x4000) {
                    tables.with_page(address, |entries| {
                        for entry in entries {
                            words.push(entry.load(Ordering::Acquire), GFP_KERNEL)?;
                        }
                        Ok(())
                    })?;
                }
            }
            Ok(words)
        };
        let geometry = UatGeometry::new(42).ok_or(EINVAL)?;
        let tables = make_tables()?;
        let before = snapshot(&tables)?;
        drop(tables);
        let payload = Page::alloc_page(GFP_KERNEL | __GFP_ZERO)?;
        for _ in 0..2 {
            // SAFETY: All TTBAT entries are zero and ASC remains stopped.
            // The table owner is dropped here before any firmware startup.
            let mut table = unsafe { UatPageTable::new_with_reserved_ttb(
                self.firmware.resources.regions[1].base,
                geometry.kernel_va_base()..geometry.kernel_va_top(), 42, 42, make_tables()?) }?;
            let boundary = geometry.kernel_va_base() + (1 << 25);
            let spans = [boundary - 0x8000..boundary + 0x18000,
                geometry.kernel_va_top() - 0x4000..geometry.kernel_va_top()];
            for span in &spans {
                table.prepare_map(span.clone())?;
                table.map_pages(span.clone(), payload.phys(), prot::PROT_GPU_SHARED_RW, true)?;
                if table.prepare_map(span.clone()) != Err(EBUSY)
                    || !table.covers_range(span.clone(), true, true)? {
                    return Err(EIO);
                }
                for va in (span.start..span.end).step_by(0x4000) {
                    if table.translate_iova(va + 13)? != payload.phys() + 13 { return Err(EIO); }
                }
                table.reprot_pages(span.clone(), prot::PROT_GPU_SHARED_RO)?;
                if !table.covers_range(span.clone(), true, false)?
                    || table.covers_range(span.clone(), false, true)? { return Err(EIO); }
                table.unmap_pages(span.clone())?;
                if table.covers_range(span.clone(), false, false)? { return Err(EIO); }
            }
            drop(table);
            if snapshot(&make_tables()?)? != before { return Err(EIO); }
        }
        dev_info!(pdev.as_ref(), "G16G: reserved UAT walker passed two map/protect/unmap/drop cycles across leaf boundaries; inherited tables unchanged, contexts disabled\n");
        Ok(())
    }

    pub(crate) fn set_power_generation(&self, generation: u32) -> Result {
        let registers = self.sgx.try_access().ok_or(ENODEV)?;
        registers.try_write32(generation, 0xd60000)?;
        if registers.try_read32(0xd60000)? != generation { return Err(EIO); }
        Ok(())
    }

    pub(crate) fn log_power_handshake(&self) -> Result {
        let sgx = self.sgx.try_access().ok_or(ENODEV)?;
        for offset in [0xe01000, 0xe01004, 0xe01008, 0xe0100c, 0xe01010, 0xe01014,
            0xe01018, 0xe0101c, 0xe01500] {
            dev_info!(self.dev.as_ref(), "G16G: power handshake SGX +{:#x}={:#010x}\n",
                offset, sgx.try_read32(offset)?);
        }
        Ok(())
    }

    pub(crate) fn check_pstate(&self) -> Result<u32> {
        let value = self.sgx.try_access().ok_or(ENODEV)?.try_read32(0xe01000)?;
        if value & 0xf > crate::g16_power::requested_state()? {
            dev_err!(self.dev.as_ref(), "G16G: unexpected performance state {:#x} exceeds configured ceiling\n", value);
            return Err(ERANGE);
        }
        Ok(value & 0xf)
    }

    pub(crate) fn log_engine_state(&self) -> Result {
        if self.firmware.board.chip_id == 0x8122 {
            dev_info!(self.dev.as_ref(), "G16G: J613 adaptive DVFS: physical engine snapshot skipped; firmware buffers retained\n");
            return Ok(());
        }
        let sgx = self.sgx.try_access().ok_or(ENODEV)?;
        let pstate = sgx.try_read32(0xe01000)? & 0xf;
        if pstate == 0 {
            dev_info!(self.dev.as_ref(), "G16G: GPU is powered down; skipping engine/MMU register snapshot\n");
            return Ok(());
        }
        dev_info!(self.dev.as_ref(), "G16G: reading engine/MMU snapshot at pstate={}\n", pstate);
        let mut mmu = [0u32; 13];
        for (i, word) in mmu.iter_mut().enumerate() { *word = sgx.try_read32(0xd08000 + i * 4)?; }
        dev_info!(self.dev.as_ref(), "G16G: GPU MMU configuration={:x?}\n", mmu);
        for offset in [0xc040, 0xc048, 0xc050, 0xc058, 0xc120, 0xc140, 0xc148,
            0xd8c0, 0xd8c8] {
            dev_info!(self.dev.as_ref(), "G16G: SGX +{:#x}={:#018x}\n", offset, sgx.try_read64(offset)?);
        }
        Ok(())

    }

    pub(crate) fn clock_state(&self) -> Result<[u32; 4]> {
        if self.firmware.board.chip_id != 0x8122 { return Err(ENOTSUPP); }
        let sgx = self.sgx.try_access().ok_or(ENODEV)?;
        let state = sgx.try_read32(0xe01000)? & 0xf;
        if state == 0 { return Ok([0; 4]); }
        Ok([state, sgx.try_read32(0xe1c014)?, sgx.try_read32(0xe1c800)?, sgx.try_read32(0xe1e01c)?])
    }

    pub(crate) fn log_clock_transitions(&self, transitions: &[(u64, [u32; 4])], overflow: u32) {
        for &(elapsed, sample) in transitions {
            dev_info!(self.dev.as_ref(), "G16G: J613 load-following transition elapsed_ns={} pstate={} clkgen14={:#x} nibble={:#x} profile_word={:#x} pll={:#x}\n", elapsed, sample[0], sample[1], sample[1]&15, sample[2], sample[3]);
        }
        dev_info!(self.dev.as_ref(), "G16G: J613 transition overflow={} (idle disabled during clock MMIO only)\n", overflow);
    }

    pub(crate) fn log_clock_census(&self, samples: &[[u32; 4]], counts: &[u32], overflow: u32) {
        for (sample, count) in samples.iter().zip(counts) {
            dev_info!(self.dev.as_ref(), "G16G: J613 compute clock census pstate={} clkgen+14={:#010x} low-nibble={:#x} +800={:#010x} +201c={:#010x} count={}\n",
                sample[0], sample[1], sample[1] & 0xf, sample[2], sample[3], count);
        }
        dev_info!(self.dev.as_ref(), "G16G: J613 compute clock census overflow={} (bounded polling observation, ceiling unchanged)\n", overflow);
    }

    pub(crate) fn firmware(&self) -> &Firmware { &self.firmware }

    pub(crate) fn require_stopped(&self, pdev: &platform::Device<Core>) -> Result {
        if self.asc.access(pdev.as_ref())?.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            return Err(EBUSY);
        }
        Ok(())
    }

    pub(crate) fn start_asc(&self, pdev: &platform::Device<Core>) -> Result {
        self.require_stopped(pdev)?;
        let asc = self.asc.access(pdev.as_ref())?;
        asc.write32(asc.read32(ASC_CPU_CONTROL) | ASC_CPU_RUN, ASC_CPU_CONTROL);
        Ok(())
    }

    pub(crate) fn stop_asc(&self) -> Result {
        let asc = self.asc.try_access().ok_or(ENODEV)?;
        asc.write32(asc.read32(ASC_CPU_CONTROL) & !ASC_CPU_RUN, ASC_CPU_CONTROL);
        let start = Instant::<Monotonic>::now();
        while asc.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            if start.elapsed() >= Delta::from_millis(100) { return Err(ETIMEDOUT); }
            fsleep(Delta::from_micros(10));
        }
        Ok(())
    }

}

impl Drop for Device {
    fn drop(&mut self) {
        if !self.power_vote {
            return;
        }
        match self.asc.try_access() {
            Some(asc) if asc.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN == 0 => {}
            _ => {
                dev_err!(
                    self.dev.as_ref(),
                    "G16G: cannot prove ASC stopped; retaining PMP vote\n"
                );
                return;
            }
        }
        // SAFETY: the managed device link keeps PMP bound through remove.
        if let Err(error) = to_result(unsafe { bindings::apple_pmp_set_device_power(0x0f, self.firmware.board.pmp_device, 0) }) {
            dev_err!(
                self.dev.as_ref(),
                "G16G: PMP power release failed: {:?}\n",
                error
            );
        }
    }
}
