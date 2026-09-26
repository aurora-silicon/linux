// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::{c_str, device::Core, io::mem::{Mem, MemFlag}, platform, prelude::*};
use crate::{driver, g16_initdata::{self, RootPointers}, g16_memory::{self, Buffer, KERNEL_RANGE},
    g16_firmware::Firmware, mmu, pgtable::prot};

#[derive(Clone, Copy)]
#[repr(usize)]
enum Object {
    Root, Brn, Runtime, Globals, Control, FirmwareData, Power, Dynamic, Main,
    PowerPerformance, Timer, Activity254, Activity25c, Activity264, Activity26c,
    LogState, LogEntries, LogData, Fwctl, DevctrlState, DevctrlRing,
    KtraceState, KtraceData, StatisticsState, StatisticsData, Temperature,
    EventState, EventData, Retention, UmaTable, ParameterTable,
}
const SIZES: &[usize] = &[
    0xc8, 0x4000, 0x4b8, 0xe48, 0x20, 0xeaf0, 0x670, 0x4000, 0x2658,
    0x6578, 0x88, 0xc18, 0x1248, 0xe10, 0x60,
    0x1b0, 0x28800, 0x79800, 0x4000, 0x30, 0x4000,
    0x30, 0x9000, 0x30, 0x4800, 4, 0x30, 0x4800, 0x4000, 0x4000, 0x4000,
];

// (slot, physical register range, mapped/requested length, relative tag, mode).
// Empty slots remain zero. Virtual addresses are deliberately absent.
const IOMAPS: &[(usize, usize, usize, usize, u64, u64)] = &[
    (0, 0x381014000, 0x4000, 0x4000, 0, 2),
    (3, 0x2201c4000, 0x30000, 0x18000, 0, 2),
    (9, 0x3803d0000, 0x1000, 0x1000, 0, 2),
    (10, 0x3803c0000, 0x2000, 0x2000, 0, 0),
    (12, 0x50165c000, 0x4000, 0x4000, 0, 2),
    (14, 0x380280000, 0x8000, 0x8000, 0, 0),
    (15, 0x38c840000, 0x24000, 0x24000, 0, 0),
    (17, 0x300000000, 0x20000, 0x20000, 0, 2),
    (22, 0x301000000, 0x8000, 0x8000, 0, 2),
    (26, 0x300d04000, 0x8000, 0x8000, 0xd04000, 2),
    (27, 0x300d0d000, 0x1000, 0x1000, 0xd0d000, 2),
    (28, 0x300d58000, 0x8000, 0x8000, 0xd58000, 2),
    (29, 0x300d10000, 0x4000, 0x4000, 0xd10000, 2),
    (31, 0x300d40000, 0x4000, 0x4000, 0xd40000, 2),
    (32, 0x300d60000, 0x8000, 0x8000, 0xd60000, 2),
    (36, 0x300e00000, 0x4000, 0x4000, 0xe00000, 0),
    (39, 0x300e08000, 0x8000, 0x8000, 0, 2),
    (40, 0x300e1c000, 0x4000, 0x4000, 0xe1c000, 2),
    (41, 0x300e1f800, 0x4000, 0x4000, 0, 2),
];

pub(crate) struct Config {
    objects: KVec<Buffer>,
    pipes: KVec<Buffer>,
    iomaps: KVec<mmu::KernelMapping>,
    loader: Mem,
    trace_seen: [u32; 2],
    iomap_bytes: KBox<[u8; 42 * 0x28]>,
}

// SAFETY: `loader` maps retained normal firmware RAM, not thread-local memory.
// All accesses require &mut Config and use bounded volatile reads/writes. A
// runtime mutex serializes transfer/use; firmware synchronization is unchanged.
unsafe impl Send for Config {}

impl Config {
    pub(crate) fn new(pdev: &platform::Device<Core>, dev: &driver::AsahiDevice,
        vm: &mmu::Vm, gpu_vm: &mmu::Vm, firmware: &Firmware) -> Result<Self> {
        let resource = pdev.as_ref().of_node().ok_or(ENODEV)?
            .reserved_mem_region_to_resource_byname(c_str!("fw-data"))?;
        let data = firmware.resources.regions[5];
        if resource.start() != data.base || resource.size() != data.size || data.size < 0x134000 {
            return Err(EINVAL);
        }
        // SAFETY: This admitted firmware data reservation is normal no-map RAM.
        // Runtime stops ASC before destroying Config. The boot-only loader
        // descriptor table at data+0x1332f0 is host-owned until initdata is sent.
        let loader = unsafe { Mem::try_new(resource, MemFlag::WC.into()) }?;
        let mut config = Self { objects: KVec::new(), pipes: KVec::new(), iomaps: KVec::new(),
            loader, trace_seen: [0; 2], iomap_bytes: KBox::new([0; 42 * 0x28], GFP_KERNEL)? };
        for (index, size) in SIZES.iter().enumerate() {
            config.objects.push(if index == Object::UmaTable as usize || index == Object::ParameterTable as usize {
                Buffer::new_gpu(dev, vm, gpu_vm, *size)?
            } else { Buffer::new(dev, vm, *size)? }, GFP_KERNEL)?;
        }
        // Twelve firmware submission channels: TA/3D/compute at four priorities.
        // Empty independent rings avoid aliasing producer/consumer state.
        for _ in 0..12 {
            config.pipes.push(Buffer::new(dev, vm, 0x30)?, GFP_KERNEL)?;
            config.pipes.push(Buffer::new(dev, vm, 0x1800)?, GFP_KERNEL)?;
        }
        for &(slot, physical, mapped, requested, tag, mode) in IOMAPS {
            let offset = physical & mmu::UAT_PGMSK;
            let size = (mapped + offset + mmu::UAT_PGMSK) & !mmu::UAT_PGMSK;
            let protection = if slot == 36 { prot::PROT_FW_PROTECTED_MMIO }
                else if mode == 0 { prot::PROT_FW_MMIO_RO } else { prot::PROT_FW_MMIO_RW };
            // MCC slot 3 concatenates two six-page banks 32 MiB apart.
            // Their single descriptor does not imply contiguous physical MMIO.
            let mapping = vm.map_io_in_range(KERNEL_RANGE, physical - offset,
                if slot == 3 { 0x18000 } else { size }, protection)?;
            if slot == 3 {
                let second = vm.map_io(mapping.iova() + 0x18000, physical + 0x2000000,
                    0x18000, protection)?;
                config.iomaps.push(second, GFP_KERNEL)?;
            }
            let va = mapping.iova() + offset as u64;
            let entry = &mut config.iomap_bytes[slot * 0x28..(slot + 1) * 0x28];
            entry[0..8].copy_from_slice(&(physical as u64).to_le_bytes());
            entry[8..16].copy_from_slice(&va.to_le_bytes());
            entry[16..20].copy_from_slice(&(mapped as u32).to_le_bytes());
            entry[20..24].copy_from_slice(&(requested as u32).to_le_bytes());
            entry[24..32].copy_from_slice(&tag.to_le_bytes());
            entry[32..40].copy_from_slice(&mode.to_le_bytes());
            config.iomaps.push(mapping, GFP_KERNEL)?;
        }
        let scratch = vm.map_io_in_range(KERNEL_RANGE, 0x300d60000, 0x8000, prot::PROT_FW_SHARED_RW)?;
        let scratch_va = scratch.iova();
        config.iomaps.push(scratch, GFP_KERNEL)?;
        config.initialize(scratch_va)?;
        Ok(config)
    }

    /// The global hardware UMA descriptor table is consumed after the engine
    /// switches to a client ASID. Reserve its one owned GEM view in that VM
    /// before client resources are allocated; preserve the bootstrap mapping.
    pub(crate) fn bind_uma_table(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        self.object(Object::UmaTable).map_gpu_view(vm)
    }
    pub(crate) fn bind_parameter_table(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        self.object(Object::ParameterTable).map_gpu_view(vm)
    }
    /// Same global engine state passed in Runtime+0x264. Firmware indexes
    /// scheduler records here; a notifier or per-job scratch is not equivalent.
    pub(crate) fn compute_stats(&self) -> u64 { self.va(Object::Activity264) }
    pub(crate) fn render_stats(&self) -> [u64; 2] {
        [self.va(Object::Activity254) + 4, self.va(Object::Activity25c) + 8]
    }
    fn va(&self, object: Object) -> u64 { self.objects[object as usize].va() }
    fn object(&mut self, object: Object) -> &mut Buffer { &mut self.objects[object as usize] }
    fn ptr(&mut self, owner: Object, offset: usize, target: Object, delta: u64) -> Result {
        let va = self.va(target).checked_add(delta).ok_or(EOVERFLOW)?;
        self.object(owner).u64(offset, va)
    }
    fn initialize(&mut self, scratch: u64) -> Result {
        use Object::*;
        let mut root = [0; g16_initdata::ROOT_SIZE];
        g16_initdata::encode_root(&mut root, RootPointers {
            brn: self.va(Brn), runtime: self.va(Runtime), globals: self.va(Globals),
            control: self.va(Control), firmware_data: self.va(FirmwareData),
            power: self.va(Power), dynamic: self.va(Dynamic),
        }).map_err(|_| EINVAL)?;
        self.object(Root).write(0, &root)?;
        for (offset, target) in [(0, Main), (0x10, Timer), (0x1c0, EventState), (0x1c8, EventData),
            (0x1d0, LogState), (0x1d8, LogEntries), (0x1e0, KtraceState), (0x1e8, KtraceData),
            (0x1f0, StatisticsState), (0x1f8, StatisticsData), (0x200, LogData),
            (0x254, Activity254), (0x25c, Activity25c), (0x264, Activity264),
            (0x26c, Activity26c), (0x469, PowerPerformance)] {
            self.ptr(Runtime, offset, target, 0)?;
        }
        self.object(Runtime).u64(0x18, scratch)?;
        self.object(Runtime).u32(0x250, 1)?;
        self.object(Runtime).u32(0x2f8, 4)?;
        let pb_gpu = self.object(ParameterTable).gpu_va()?;
        self.object(Runtime).u64(0x2d0, pb_gpu)?;
        self.ptr(Runtime, 0x2d8, ParameterTable, 0)?;
        let uma_gpu = self.object(UmaTable).gpu_va()?;
        self.object(Runtime).u64(0x2e0, uma_gpu)?;
        self.ptr(Runtime, 0x2e8, UmaTable, 0)?;
        self.object(Activity25c).u32(0xc18, u32::MAX)?;
        self.object(Activity25c).u32(0xc30, u32::MAX)?;
        for pipe in 0..12 {
            let state = self.pipes[pipe * 2].va();
            let ring = self.pipes[pipe * 2 + 1].va();
            let offset = 0x20 + pipe * 0x20;
            for (off, va) in [(0, state), (8, state + 0x10), (16, state + 0x20), (24, ring)] {
                self.object(Runtime).u64(offset + off, va)?;
            }
        }
        for (offset, target, delta) in [(0x1a0, DevctrlState, 0), (0x1a8, DevctrlState, 0x10),
            (0x1b0, DevctrlState, 0x20), (0x1b8, DevctrlRing, 0)] {
            self.ptr(Runtime, offset, target, delta)?;
        }
        self.ptr(FirmwareData, 0x4f88, Fwctl, 0)?;
        self.ptr(FirmwareData, 0x4f90, Fwctl, 0x40)?;
        self.object(FirmwareData).u32(0x4fd4, 1)?;
        self.object(Globals).u32(0x78, 1)?;
        self.object(Globals).u32(0xe28, 3)?;
        for (offset, value) in [(0x24, 3000), (0x998, 40), (0x99c, 10),
            (0x9a0, 250), (0x9b8, 2), (0x9bc, 40), (0x9c0, 5),
            (0x9c8, 40), (0x9cc, 50)] {
            self.object(Globals).u32(offset, value)?;
        }
        self.ptr(PowerPerformance, 0x3b50, Temperature, 0)?;
        self.object(PowerPerformance).u32(4, 200000)?;
        self.object(PowerPerformance).u32(8, 200000)?;
        self.object(Main).u64(0x28, g16_memory::TIMESTAMP_RANGE.start)?;
        let (objects, iomaps) = (&mut self.objects, &self.iomap_bytes);
        objects[Main as usize].write(0x640, &iomaps[..])?;
        self.object(Main).u32(0xea0, 1)?;
        self.object(Main).u32(0xeb8, 1)?;
        self.object(Main).u32(0xed0, 24_000)?;
        self.object(Main).u32(0xed4, 1)?;
        self.object(Main).u32(0xed8, 8)?;
        self.object(Main).u64(0xf24, 0x0000000607002004)?;
        self.object(Main).u32(0xfc0, 10)?;
        // Publish the J713 compacted tables, including SRAM and auxiliary
        // accounting frequencies. Hardware voltage sequencing stays in firmware.
        let pstate = crate::g16_power::requested_state()?;
        self.object(Main).u32(0xfc4, pstate)?;
        for state in 0..=pstate as usize {
            self.object(Main).u32(0xfc8 + state * 4,
                crate::g16_power::FREQUENCIES_MHZ[state])?;
            self.object(Main).u32(0x1808 + state * 4,
                crate::g16_power::AUX_FREQUENCIES_MHZ[state])?;
            for core in 0..16 {
                self.object(Main).u32(0x1008 + state * 64 + core * 4,
                    crate::g16_power::VOLTAGES_MV[state])?;
                self.object(Main).u32(0x1408 + state * 64 + core * 4,
                    crate::g16_power::SRAM_VOLTAGES_MV[state])?;
            }
        }
        let units = pstate * 100;
        for offset in [0x88, 0x8c, 0x98] {
            self.object(Globals).u32(offset, units)?;
        }
        self.object(PowerPerformance).u32(0x10, 4)?;
        self.object(PowerPerformance).u32(0x14, 0x3f800000)?;
        self.object(PowerPerformance).u32(0x2c, pstate)?;
        self.object(PowerPerformance).u32(0x30, pstate)?;
        // Keep current hardware state at zero until firmware acknowledges
        // the real transition. Initial actual/target are constructor inputs.
        for offset in [
            0x9f0, 0x9f4, 0xa38, // power PI bounds/output
            0xaa8, 0xaac, 0xaf0, // PPM PI bounds/output
            0xb78, 0xb7c, 0xb80, 0xbc0, // performance bounds/base/output
            0x22b0, 0x22b4, 0x22f8, // temperature PI bounds/output
            0x2210, 0x2870, 0x2a3c, // additional selector inputs
            0x2b84, 0x2b88, 0x2b8c, 0x2bcc, // secondary performance
            0x2c40, 0x2c44, 0x2c88, // average power PI bounds/output
        ] {
            self.object(PowerPerformance).u32(offset, units)?;
        }
        self.object(PowerPerformance).write(0x279e, &units.to_le_bytes())?;
        self.object(Main).u32(0x259c, 1)?;
        self.ptr(Main, 0xe88, Retention, 0)?;
        self.object(Main).u32(0xee0, 1)?;
        for (offset, value) in [(4, 4), (8, 6), (12, 3), (16, 7), (20, 7), (40, 8), (48, 1)] {
            self.object(Main).u32(0x2540 + offset, value)?;
        }
        self.object(Control).u32(0, 0x33)?;
        Ok(())
    }

    /// Publish the loader's host-supplied MMIO table after RTKit has reached
    /// its application wait state, before sending initdata. FW never consumes
    /// this table concurrently with publication under that boot protocol.
    pub(crate) fn publish_loader(&mut self) -> Result {
        let mut map = self.loader.iosys_map(0x1332f0, 42 * 0x28)?;
        for index in 0..42 * 0x28 {
            // SAFETY: bounded byte in retained WC firmware data reservation.
            if unsafe { map.as_ptr().add(index).read_volatile() } != 0 { return Err(EBUSY); }
        }
        map.write(&self.iomap_bytes[..], 0)?;
        g16_memory::publish();
        Ok(())
    }

    fn log_recovery(&mut self, dev: &driver::AsahiDevice) -> Result {
        for (object, name, offset, count) in [
            (Object::Activity26c, "engine-recovery", 0usize, 24usize),
            (Object::FirmwareData, "recovery-summary", 0x4ebc, 64)] {
            for start in (0..count).step_by(8) {
                let mut words = [0u32; 8];
                for (i, word) in words.iter_mut().enumerate() {
                    *word = self.object(object).read_u32(offset + (start+i)*4)?;
                }
                dev_err!(dev.as_ref(), "G16G: {} +{:#x}={:x?}\n", name, offset+start*4, words);
            }
        }
        self.log_firmware_state(dev)
    }
    pub(crate) fn log_firmware_state(&mut self, dev: &driver::AsahiDevice) -> Result {
        // Normal draining counts late trace records without printk. Preserve
        // the most recent records on failure, even after thousands of jobs.
        // This snapshot neither moves the consumer nor acknowledges events.
        let producer = self.object(Object::KtraceState).read_u32(0x20)?;
        if producer >= 512 { return Err(EIO); }
        for age in (1..=16).rev() {
            let index = (producer + 512 - age) % 512;
            let mut record = [0u8; 0x48];
            self.object(Object::KtraceData).read(index as usize * 0x48, &mut record)?;
            if u32::from_le_bytes(record[..4].try_into().unwrap()) != 5 { continue; }
            let tag = u32::from_le_bytes(record[44..48].try_into().unwrap());
            let phase = u32::from_le_bytes(record[48..52].try_into().unwrap());
            let mut args = [0u64; 4];
            for (i, arg) in args.iter_mut().enumerate() {
                *arg = u64::from_le_bytes(record[12+i*8..20+i*8].try_into().unwrap());
            }
            dev_info!(dev.as_ref(), "G16G: recent trace index={} thread={} id={:#x} phase={} args={:x?}\n",
                index, tag >> 24, tag & 0xffffff, phase, args);
        }
        for (address, count) in [(0x68158usize, 1usize), (0x128d60, 8),
            (0x129b20, 4), (0x129bc8, 8), (0x129d00, 8), (0x147368, 6), (0x192900, 5), (0x1929d0, 8), (0x1935a0, 1)] {
            let map = self.loader.iosys_map(address.checked_sub(0x60000).ok_or(EINVAL)?, count * 8)?;
            let mut words = [0u64; 8];
            for (index, word) in words[..count].iter_mut().enumerate() {
                let mut bytes = [0u8; 8];
                for (j, byte) in bytes.iter_mut().enumerate() {
                    // SAFETY: checked retained firmware data mapping, byte access
                    // avoids assuming atomicity of packed firmware fields.
                    *byte = unsafe { map.as_ptr().add(index * 8 + j).read_volatile() };
                }
                *word = u64::from_le_bytes(bytes);
            }
            dev_info!(dev.as_ref(), "G16G: firmware RAM +{:#x}={:x?}\n", address, &words[..count]);
        }
        // Global engine state consumed by the microsequence and scheduler.
        // Record only normal firmware RAM, without acknowledging GPU status.
        for (object, name) in [(Object::Activity254, "vertex"),
            (Object::Activity25c, "fragment"), (Object::Activity264, "compute")] {
            let mut words = [0u32; 16];
            for (i, word) in words.iter_mut().enumerate() {
                *word = self.object(object).read_u32(i * 4)?;
            }
            dev_info!(dev.as_ref(), "G16G: {} engine state={:x?}\n", name, words);
        }
        let mut uma = [0u64; 4];
        for (index, word) in uma.iter_mut().enumerate() {
            *word = self.object(Object::UmaTable).read_u64(32 + index * 8)?;
        }
        dev_info!(dev.as_ref(), "G16G: hardware UMA slot1={:x?}\n", uma);
        let power = self.object(Object::PowerPerformance).read_u32(0x38)?;
        dev_info!(dev.as_ref(), "G16G: firmware GPU power state={:#x}\n", power);
        for start in [0usize, 0x20, 0x40, 0xb80, 0xbc0, 0x6510, 0x6550] {
            let mut words = [0u32; 8];
            for (index, word) in words.iter_mut().enumerate() {
                *word = self.object(Object::PowerPerformance).read_u32(start + index * 4)?;
            }
            dev_info!(dev.as_ref(), "G16G: power policy +{:#x}={:x?}\n", start, words);
        }
        Ok(())
    }

    /// Serialized device-control producer. Ring geometry is a firmware ABI
    /// constraint; no queue or allocation count is limited by a lab pool.
    pub(crate) fn enqueue_control(&mut self, opcode: u32, payload: u32,
        generation: &mut u32, device: &crate::g16_device::Device) -> Result<u32> {
        if !matches!(opcode, 0x1a | 0x34 | 0x0a) { return Err(EINVAL); }
        let [read, cfi, write] = self.control_indices()?;
        if read != write || cfi != write { return Err(EBUSY); }
        let mut entry = [0; 0x40];
        entry[..4].copy_from_slice(&opcode.to_le_bytes());
        entry[4..8].copy_from_slice(&payload.to_le_bytes());
        self.object(Object::DevctrlRing).write(write as usize * 0x40, &entry)?;
        // All three admitted opcodes require a power assertion in the M4 ABI.
        *generation = generation.wrapping_add(1);
        device.set_power_generation(*generation)?;
        g16_memory::publish();
        let next = (write + 1) % 256;
        self.object(Object::DevctrlState).u32(0x20, next)?;
        g16_memory::publish();
        Ok(next)
    }
    pub(crate) fn control_indices(&mut self) -> Result<[u32; 3]> {
        let state = self.object(Object::DevctrlState);
        let indices = [state.read_u32(0)?, state.read_u32(0x10)?, state.read_u32(0x20)?];
        if indices.iter().any(|index| *index >= 256) { return Err(EIO); }
        Ok(indices)
    }
    pub(crate) fn consumed_generation(&mut self) -> Result<u32> {
        self.object(Object::FirmwareData).read_u32(0xeae8)
    }
    pub(crate) fn reset_trace_budget(&mut self) { self.trace_seen = [0; 2]; }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        use Object::*;
        // The loader mapping contains only the admitted firmware data RAM.
        let map = self.loader.iosys_map(0, 0x13c000)?;
        dump.record("firmware-data", 0xfffffc0000060000, 0x13c000, |out| {
            for (i, byte) in out.iter_mut().enumerate() {
                // SAFETY: bounded read of the retained WC RAM mapping above.
                *byte = unsafe { map.as_ptr().add(i).read_volatile() };
            }
            Ok(())
        })?;
        for (object, name) in [(Root, "initdata-root"), (Runtime, "runtime"),
            (Globals, "globals"), (FirmwareData, "host-firmware-data"),
            (Main, "main"), (PowerPerformance, "power-performance"), (Timer, "timer"),
            (Activity254, "vertex-engine"), (Activity25c, "fragment-engine"),
            (Activity264, "compute-engine"), (UmaTable, "uma-table"),
            (ParameterTable, "parameter-table"), (KtraceState, "trace-state"),
            (KtraceData, "trace-data")] {
            dump.buffer(name, self.object(object), SIZES[object as usize])?;
        }
        Ok(())
    }
    pub(crate) fn rearm_scheduler(&mut self) -> Result {
        if self.object(Object::FirmwareData).read_u32(0x4fd4)? != 0 { return Err(EBUSY); }
        self.object(Object::FirmwareData).u32(0x4fd4, 1)?;
        g16_memory::publish();
        Ok(())
    }

    pub(crate) fn publish_queue(&mut self, pipe: usize, queue: u64, wptr: u16, slot: u8, new: bool) -> Result<u32> {
        if pipe >= 12 || slot >= 128 || queue & 7 != 0 { return Err(EINVAL); }
        let state = &mut self.pipes[pipe * 2];
        let consumer = state.read_u32(0)?;
        let cfi = state.read_u32(0x10)?;
        let producer = state.read_u32(0x20)?;
        if consumer >= 256 || cfi >= 256 || producer >= 256 { return Err(EIO); }
        let next = (producer + 1) % 256;
        if next == consumer || next == cfi { return Err(EBUSY); }
        let timestamp: u64;
        // SAFETY: architectural counter is readable at EL1.
        unsafe { core::arch::asm!("mrs {t}, cntpct_el0", t = out(reg) timestamp, options(nomem, nostack, preserves_flags)) };
        let mut entry = [0; 0x18];
        entry[..8].copy_from_slice(&timestamp.to_le_bytes());
        entry[8..16].copy_from_slice(&queue.to_le_bytes());
        entry[16..20].copy_from_slice(&((pipe % 3) as u32).to_le_bytes());
        entry[20..22].copy_from_slice(&wptr.to_le_bytes());
        entry[22] = slot; // must match command metadata scheduler slot
        entry[23] = u8::from(new);
        self.pipes[pipe * 2 + 1].write(producer as usize * 0x18, &entry)?;
        g16_memory::publish();
        self.pipes[pipe * 2].u32(0x20, next)?;
        g16_memory::publish();
        Ok(next)
    }
    /// Diagnostic: word snapshot of shared firmware objects that the
    /// completion processing may update after the stamps are visible.
    pub(crate) fn late_snapshot(&mut self, out: &mut [u32]) -> Result<usize> {
        let mut n = 0;
        for (object, words) in [(Object::Activity254, 0xc18 / 4), (Object::Activity25c, 0x1248 / 4),
            (Object::Activity264, 0xe10 / 4), (Object::PowerPerformance, 0x60 / 4),
            (Object::Runtime, 0x4b8 / 4), (Object::Globals, 0xe48 / 4)] {
            for i in 0..words {
                if n >= out.len() { return Ok(n); }
                out[n] = self.object(object).read_u32(i * 4)?;
                n += 1;
            }
        }
        Ok(n)
    }
    pub(crate) fn pipe_indices(&mut self, pipe: usize) -> Result<[u32; 3]> {
        if pipe >= 12 { return Err(EINVAL); }
        let state = &mut self.pipes[pipe * 2];
        Ok([state.read_u32(0)?, state.read_u32(0x10)?, state.read_u32(0x20)?])
    }

    pub(crate) fn root(&self) -> u64 { self.va(Object::Root) }
    pub(crate) fn ready(&mut self) -> Result<bool> { Ok(self.object(Object::Control).read_u32(0x14)? == 1) }

    /// Bounded drains of firmware-owned rings. Logs are diagnostics, never
    /// interpreted as proof of command completion.
    pub(crate) fn drain(&mut self, dev: &driver::AsahiDevice) -> Result {
        use Object::*;
        for thread in 0..9 {
            let offset = thread * 0x30;
            let mut consumer = self.object(LogState).read_u32(offset)?;
            let producer = self.object(LogState).read_u32(offset + 0x20)?;
            if consumer >= 256 || producer >= 256 { return Err(EIO); }
            while consumer != producer {
                let entry_offset = (thread * 256 + consumer as usize) * 0x48;
                let payload = self.object(LogEntries).read_u32(entry_offset + 8)? as usize & 255;
                let mut text = [0u8; 200];
                self.object(LogData).read((thread * 256 + payload) * 0xd8 + 0x10, &mut text)?;
                let length = text.iter().position(|b| *b == 0).unwrap_or(text.len());
                dev_info!(dev.as_ref(), "G16G FW[{}]: {}\n", thread,
                    core::str::from_utf8(&text[..length]).unwrap_or("<non-UTF8 firmware log>"));
                consumer = (consumer + 1) & 255;
            }
            self.object(LogState).u32(offset, consumer)?;
        }
        for (state, data, count, trace) in [(KtraceState, KtraceData, 512, true),
            (StatisticsState, StatisticsData, 256, false), (EventState, EventData, 256, false)] {
            let mut consumer = self.object(state).read_u32(0)?;
            let producer = self.object(state).read_u32(0x20)?;
            if consumer >= count || producer >= count { return Err(EIO); }
            while consumer != producer {
                let mut record = [0; 0x48];
                self.object(data).read(consumer as usize * 0x48, &mut record)?;
                if trace {
                    if u32::from_le_bytes(record[..4].try_into().unwrap()) != 5 { return Err(EIO); }
                    let tag = u32::from_le_bytes(record[44..48].try_into().unwrap());
                    let phase = u32::from_le_bytes(record[48..52].try_into().unwrap());
                    let mut args = [0u64; 4];
                    for (i, arg) in args.iter_mut().enumerate() {
                        *arg = u64::from_le_bytes(record[12 + i * 8..20 + i * 8].try_into().unwrap());
                    }
                    let class = usize::from((tag & 0xffffff) >= 0x400);
                    let seen = &mut self.trace_seen[class];
                    *seen = seen.saturating_add(1);
                    // Firmware tracing is diagnostic, not normal desktop
                    // output. A busy scene can produce a megabyte of printk
                    // text per second. Continue draining and validating all
                    // records; opt in through the common KTraceCh debug bit.
                    let print_trace = crate::debug::debug_enabled(crate::debug::DebugFlags::KTraceCh);
                    if print_trace && *seen <= 768 {
                        dev_info!(dev.as_ref(), "G16G trace thread={} id={:#x} phase={} args={:x?} tick={}\n", tag >> 24, tag & 0xffffff, phase, args, u64::from_le_bytes(record[4..12].try_into().unwrap()));
                    } else if print_trace && *seen == 769 {
                        dev_info!(dev.as_ref(), "G16G trace class={} subsequent records counted without printk\n", class);
                    }
                    if tag & 0xffffff == 0x11c {
                        dev_warn!(dev.as_ref(), "G16G: firmware trace lost {} records over {} ticks; diagnostics incomplete\n",
                            args[0], args[1]);
                    }
                } else if matches!(state, EventState) {
                    if u32::from_le_bytes(record[..4].try_into().unwrap()) != 1 {
                        dev_err!(dev.as_ref(), "G16G unexpected event: {:02x?}\n", record);
                        return Err(EIO);
                    }
                    // Normal event payloads duplicate the per-job retirement
                    // log. Keep raw bytes available for channel diagnostics,
                    // without formatting 72 bytes twice for every render.
                    if crate::debug::debug_enabled(crate::debug::DebugFlags::KTraceCh) {
                        dev_info!(dev.as_ref(), "G16G event: {:02x?}\n", record);
                    }
                } else if u32::from_le_bytes(record[..4].try_into().unwrap()) == 0x16 {
                    dev_err!(dev.as_ref(), "G16G recovery statistics: {:02x?}\n", record);
                    self.log_recovery(dev)?;
                    return Err(EIO);
                }
                consumer = (consumer + 1) % count;
            }
            self.object(state).u32(0, consumer)?;
        }
        g16_memory::publish();
        Ok(())
    }
}
