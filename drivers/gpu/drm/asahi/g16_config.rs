// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::{c_str, device::Core, io::mem::{Mem, MemFlag}, platform, prelude::*};
use crate::{driver, g16_initdata::{self, RootPointers}, g16_memory::{self, Buffer, KERNEL_RANGE},
    g16_firmware::Firmware, mmu, pgtable::prot};

#[path = "g16_config_layout.rs"]
pub(crate) mod layout;
use layout::{Object,OBJECTS,IO_TABLE_SIZE,FirmwareVa,Protection};
use layout::{runtime,channel,globals,main_data,power_data,firmware_data};

pub(crate) struct Config {
    objects: KVec<Buffer>,
    pipes: KVec<Buffer>,
    iomaps: KVec<mmu::KernelMapping>,
    loader: Mem,
    trace_seen: [u32; 2],
    cached_programs: Option<Buffer>,
    cached_program_cursor: usize,
    iomap_bytes: KBox<[u8; IO_TABLE_SIZE]>,
    thermal_limit: u32,
    board: &'static crate::g16_board::Board,
    thermal_observed: u32,
}

// SAFETY: `loader` maps retained normal firmware RAM, not thread-local memory.
// All accesses require &mut Config and use bounded volatile reads/writes. A
// runtime mutex serializes transfer/use; firmware synchronization is unchanged.
unsafe impl Send for Config {}

impl Config {
    pub(crate) fn cache_program(&mut self, dev: &driver::AsahiDevice,
        vm: &mmu::Vm, bytes: &mut [u8], stage: crate::g16_render::Stage) -> Result<u64> {
        const CAPACITY: usize = 16 * 1024 * 1024;
        if bytes.len() > mmu::UAT_PGSZ { return Err(EINVAL); }
        let next = self.cached_program_cursor.checked_add(mmu::UAT_PGSZ).ok_or(EOVERFLOW)?;
        if next > CAPACITY { return Err(ENOSPC); }
        if self.cached_programs.is_none() {
            self.cached_programs = Some(Buffer::new_immutable_private(dev, vm, CAPACITY)?);
        }
        let arena = self.cached_programs.as_mut().ok_or(EIO)?;
        let address = arena.va() + self.cached_program_cursor as u64;
        crate::g16_render_command::relocate_program(bytes, stage, address).map_err(|_| EINVAL)?;
        arena.write(self.cached_program_cursor, bytes)?;
        self.cached_program_cursor = next;
        g16_memory::publish();
        crate::cls_dev_dbg!(SubmitTiming, dev, "G16G: immutable cached program va={:#x} bytes={} retained={}\n", address, bytes.len(), next);
        Ok(address)
    }

    pub(crate) fn new(pdev: &platform::Device<Core>, dev: &driver::AsahiDevice,
        vm: &mmu::Vm, gpu_vm: &mmu::Vm, firmware: &Firmware) -> Result<Self> {
        let resource = pdev.as_ref().of_node().ok_or(ENODEV)?
            .reserved_mem_region_to_resource_byname(c_str!("fw-data"))?;
        let data = firmware.resources.regions[5];
        let board = firmware.board;
        let loader_end = board.firmware.loader_iomaps + board.io_slots * layout::IO_ENTRY_SIZE;
        if resource.start() != data.base || resource.size() != data.size || (data.size as usize) < loader_end {
            return Err(EINVAL);
        }
        // SAFETY: This admitted firmware data reservation is normal no-map RAM.
        // Runtime stops ASC before destroying Config. The boot-only loader
        // descriptor table at data+0x1332f0 is host-owned until initdata is sent.
        let loader = unsafe { Mem::try_new(resource, MemFlag::WC.into()) }?;
        let mut config = Self { objects: KVec::new(), pipes: KVec::new(), iomaps: KVec::new(),
            loader, trace_seen: [0; 2], cached_programs: None, cached_program_cursor: 0, iomap_bytes: KBox::new([0; IO_TABLE_SIZE], GFP_KERNEL)?,
            thermal_limit: 0, thermal_observed: u32::MAX, board };
        dev_info!(pdev.as_ref(), "G16G: {} board constructor ({} I/O slots, loader DATA+{:#x}, max pstate {})\n",
            board.name, board.io_slots, board.firmware.loader_iomaps, board.max_pstate());
        for object in OBJECTS {
            let cached = board.cached_hw_objects
                && matches!(object, Object::Main | Object::PowerPerformance);
            config.objects.push(if object.needs_client_mapping() {
                Buffer::new_gpu(dev, vm, gpu_vm, object.size())?
            } else if cached {
                Buffer::new_cached(dev, vm, object.size())?
            } else { Buffer::new(dev, vm, object.size())? }, GFP_KERNEL)?;
        }
        for _ in 0..channel::COUNT {
            config.pipes.push(Buffer::new(dev, vm, channel::STATE_SIZE)?, GFP_KERNEL)?;
            config.pipes.push(Buffer::new(dev, vm, channel::SUBMIT_RING_SIZE)?, GFP_KERNEL)?;
        }
        for io in board.iomaps.iter().copied() {
            let protection=match io.protection() {
                Protection::Protected=>prot::PROT_FW_PROTECTED_MMIO,
                Protection::ReadOnly=>prot::PROT_FW_MMIO_RO,
                Protection::ReadWrite=>prot::PROT_FW_MMIO_RW,
            };
            let mapping=vm.map_io_in_range(KERNEL_RANGE,io.physical.page_base().try_into()?,
                io.mapping_size().map_err(|_|EOVERFLOW)?,protection)?;
            if let Some((offset,physical,size))=io.second_bank().map_err(|_|EOVERFLOW)? {
                let second=vm.map_io(mapping.iova().checked_add(offset).ok_or(EOVERFLOW)?,
                    physical.0.try_into()?,size,protection)?;
                config.iomaps.push(second,GFP_KERNEL)?;
            }
            let va=FirmwareVa::new(mapping.iova().checked_add(io.physical.page_offset() as u64).ok_or(EOVERFLOW)?)
                .map_err(|_|EINVAL)?;
            let entry=io.encode(va).map_err(|_|EINVAL)?;
            config.iomap_bytes[io.slot*layout::IO_ENTRY_SIZE..(io.slot+1)*layout::IO_ENTRY_SIZE].copy_from_slice(&entry);
            config.iomaps.push(mapping,GFP_KERNEL)?;
        }
        for slot in board.empty_mode2 {
            config.iomap_bytes[slot*layout::IO_ENTRY_SIZE+32..slot*layout::IO_ENTRY_SIZE+40].copy_from_slice(&2u64.to_le_bytes());
        }
        let scratch = vm.map_io_in_range(KERNEL_RANGE, board.scratch.try_into()?, 0x8000, prot::PROT_FW_SHARED_RW)?;
        let scratch_va = scratch.iova();
        config.iomaps.push(scratch, GFP_KERNEL)?;
        config.initialize(scratch_va)?;
        if board.chip_id == 0x8122 {
            for (object, offset, value) in crate::g16_j613_power::inputs(dev)? {
                config.object(object).u32(offset, value)?;
            }
            g16_memory::publish();
        }
        Ok(config)
    }

    pub(crate) fn bind_uma_table(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        self.object(Object::UmaTable).map_gpu_view(vm)
    }
    pub(crate) fn bind_parameter_table(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        self.object(Object::ParameterTable).map_gpu_view(vm)
    }
    pub(crate) fn compute_stats(&self) -> u64 { self.va(Object::Activity264) }
    pub(crate) fn render_stats(&self) -> [u64; 2] {
        [self.va(Object::Activity254) + 4, self.va(Object::Activity25c) + 8]
    }
    fn va(&self, object: Object) -> u64 { self.objects[object as usize].va() }
    fn object(&mut self, object: Object) -> &mut Buffer { &mut self.objects[object as usize] }

    pub(crate) fn update_thermal_limit(&mut self, dev: &driver::AsahiDevice) -> Result {
        let requested = crate::g16_power::requested_state()?;
        let limit = requested;
        if limit == self.thermal_limit { return Ok(()); }
        for offset in globals::PERFORMANCE_CEILINGS {
            self.object(Object::Globals).u32(offset, limit * 100)?;
        }
        g16_memory::publish();
        self.thermal_limit = limit;
        dev_info!(dev, "G16G: thermal ceiling state={} frequency={} MHz\n",
            limit, self.board.frequencies_mhz[limit as usize]);
        Ok(())
    }
    pub(crate) fn observe_thermal_state(&mut self, dev: &driver::AsahiDevice, state: u32) {
        if state != self.thermal_observed {
            self.thermal_observed = state;
            // Every observed DVFS transition is normal telemetry, not a
            // thermal warning. Keep the state tracking, but opt printk in
            // through the GPU debug class. This is a nominal table value,
            // not a measurement of the engine clock or gating state.
            crate::cls_dev_dbg!(Gpu, dev, "G16G: observed pstate={} ceiling={} nominal_frequency={} MHz\n",
                state, self.thermal_limit, self.board.frequencies_mhz.get(state as usize).copied().unwrap_or(0));
        }
    }
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
        for (offset, target) in [(runtime::MAIN, Main), (runtime::TIMER, Timer), (runtime::EVENT_STATE, EventState), (runtime::EVENT_DATA, EventData),
            (runtime::LOG_STATE, LogState), (runtime::LOG_ENTRIES, LogEntries), (runtime::KTRACE_STATE, KtraceState), (runtime::KTRACE_DATA, KtraceData),
            (runtime::STATISTICS_STATE, StatisticsState), (runtime::STATISTICS_DATA, StatisticsData), (runtime::LOG_DATA, LogData),
            (runtime::TILER_ENGINE, Activity254), (runtime::FRAGMENT_ENGINE, Activity25c), (runtime::COMPUTE_ENGINE, Activity264),
            (runtime::RECOVERY_ENGINE, Activity26c), (runtime::POWER_PERFORMANCE, PowerPerformance)] {
            self.ptr(Runtime, offset, target, 0)?;
        }
        self.object(Runtime).u64(runtime::SCRATCH, scratch)?;
        self.object(Runtime).u32(runtime::UNKNOWN_250, 1)?;
        self.object(Runtime).u32(runtime::UNKNOWN_2F8, 4)?;
        let pb_gpu = self.object(ParameterTable).gpu_va()?;
        self.object(Runtime).u64(runtime::PARAMETER_GPU, pb_gpu)?;
        self.ptr(Runtime, runtime::PARAMETER_FW, ParameterTable, 0)?;
        let uma_gpu = self.object(UmaTable).gpu_va()?;
        self.object(Runtime).u64(runtime::UMA_GPU, uma_gpu)?;
        self.ptr(Runtime, runtime::UMA_FW, UmaTable, 0)?;
        self.object(Activity25c).u32(0xc18, u32::MAX)?;
        self.object(Activity25c).u32(0xc30, u32::MAX)?;
        for pipe in 0..channel::COUNT {
            let state = self.pipes[pipe * 2].va();
            let ring = self.pipes[pipe * 2 + 1].va();
            let offset = channel::RUNTIME_SUBMIT + pipe * channel::DESCRIPTOR_SIZE;
            for (off, va) in [(0, state), (8, state + channel::CFI as u64), (16, state + channel::WRITE as u64), (24, ring)] {
                self.object(Runtime).u64(offset + off, va)?;
            }
        }
        for (offset, target, delta) in [(channel::RUNTIME_CONTROL, DevctrlState, channel::READ as u64), (channel::RUNTIME_CONTROL+8, DevctrlState, channel::CFI as u64),
            (channel::RUNTIME_CONTROL+16, DevctrlState, channel::WRITE as u64), (channel::RUNTIME_CONTROL+24, DevctrlRing, 0)] {
            self.ptr(Runtime, offset, target, delta)?;
        }
        self.ptr(FirmwareData, firmware_data::FWCTL_STATE, Fwctl, 0)?;
        self.ptr(FirmwareData, firmware_data::FWCTL_RING, Fwctl, 0x40)?;
        self.object(FirmwareData).u32(firmware_data::UNKNOWN_4FD4, 1)?;
        self.object(Globals).u32(globals::UNKNOWN_78, 1)?;
        self.object(Globals).u32(globals::UNKNOWN_E28, 3)?;
        for (offset, value) in [(globals::PROGRESS_INTERVAL_MS, 3000), (globals::UNKNOWN_998, 40), (globals::UNKNOWN_99C, 10),
            (globals::UNKNOWN_9A0, 250), (globals::IDLE_OFF_DELAY_MS, 2), (globals::UNKNOWN_9BC, 40), (globals::UNKNOWN_9C0, 5),
            (globals::UNKNOWN_9C8, 40), (globals::UNKNOWN_9CC, 50)] {
            self.object(Globals).u32(offset, value)?;
        }
        self.ptr(PowerPerformance, power_data::TEMPERATURE_POINTER, Temperature, 0)?;
        self.object(PowerPerformance).u32(power_data::UNKNOWN_04, 200000)?;
        self.object(PowerPerformance).u32(power_data::UNKNOWN_08, 200000)?;
        self.object(Main).u64(main_data::TIMESTAMP_WINDOW, g16_memory::TIMESTAMP_RANGE.start)?;
        let (objects, iomaps) = (&mut self.objects, &self.iomap_bytes);
        let io_bytes = self.board.io_slots * layout::IO_ENTRY_SIZE;
        objects[Main as usize].write(main_data::IOMAPS, &iomaps[..io_bytes])?;
        self.object(Main).u32(main_data::ACCOUNTING_ENABLE, 1)?;
        self.object(Main).u32(main_data::UNKNOWN_EB8, 1)?;
        self.object(Main).u32(main_data::REFERENCE_CLOCK_KHZ, 24_000)?;
        self.object(Main).u32(main_data::MAINTENANCE_INTERVAL, 1)?;
        self.object(Main).u32(main_data::POWER_SAMPLE_INTERVAL, 8)?;
        let board = self.board;
        self.object(Main).u64(main_data::UNKNOWN_F24, board.main_f24)?;
        self.object(Main).u32(main_data::CORE_POSITIONS, board.cores)?;
        let pstate = crate::g16_power::requested_state()?;
        self.object(Main).u32(main_data::MAX_PSTATE, pstate)?;
        for state in 0..board.pstate_count {
            self.object(Main).u32(main_data::FREQUENCIES_MHZ + state * 4,
                board.frequencies_mhz[state])?;
            self.object(Main).u32(main_data::AUX_FREQUENCIES_MHZ + state * 4,
                board.aux_frequencies_mhz[state])?;
            for core in 0..16 {
                self.object(Main).u32(main_data::VOLTAGES_MV + state * 64 + core * 4,
                    board.voltages_mv[state])?;
                self.object(Main).u32(main_data::SRAM_VOLTAGES_MV + state * 64 + core * 4,
                    board.sram_voltages_mv[state])?;
            }
        }
        let units = pstate * 100;
        for offset in globals::PERFORMANCE_CEILINGS {
            self.object(Globals).u32(offset, units)?;
        }
        self.object(PowerPerformance).u32(power_data::UNKNOWN_10, 4)?;
        self.object(PowerPerformance).u32(power_data::UNKNOWN_14, 0x3f800000)?;
        let initial_pstate = if board.name == "J613" { 1 } else { pstate };
        self.object(PowerPerformance).u32(power_data::INITIAL_ACTUAL, initial_pstate)?;
        self.object(PowerPerformance).u32(power_data::INITIAL_TARGET, initial_pstate)?;
        if board.chip_id != 0x8122 {
            for offset in power_data::CEILING_INPUTS {
                self.object(PowerPerformance).u32(offset, units)?;
            }
            self.object(PowerPerformance).write(power_data::PACKED_SELECTOR, &units.to_le_bytes())?;
        }
        self.object(Main).u32(main_data::UNKNOWN_259C, 1)?;
        self.ptr(Main, main_data::RETENTION, Retention, 0)?;
        self.object(Main).u32(main_data::ACCOUNTING_BUCKETS_ENABLE, 1)?;
        for (offset, value) in board.main_2540 {
            self.object(Main).u32(main_data::UNKNOWN_2540 + offset, value)?;
        }
        if *crate::module_parameters::g16_main_overlay.value() != 0 {
            for &(offset, value) in board.main_overlay {
                self.object(Main).u32(offset, value)?;
            }
        }
        for &(offset, value) in board.power_overlay {
            self.object(PowerPerformance).u32(offset, value)?;
        }
        let trace = if self.board.name == "J613" {
            *crate::module_parameters::g16_trace_mask.value()
        } else { 0 };
        let base_trace = if self.board.chip_id == 0x8122 { 0 } else { 0x33 };
        self.object(Control).u32(0, base_trace | (trace & 0xff))?;
        Ok(())
    }

    pub(crate) fn publish_loader(&mut self) -> Result {
        let io_bytes = self.board.io_slots * layout::IO_ENTRY_SIZE;
        let mut map = self.loader.iosys_map(self.board.firmware.loader_iomaps, io_bytes)?;
        for index in 0..io_bytes {
            // SAFETY: bounded byte in retained WC firmware data reservation.
            if unsafe { map.as_ptr().add(index).read_volatile() } != 0 { return Err(EBUSY); }
        }
        map.write(&self.iomap_bytes[..io_bytes], 0)?;
        g16_memory::publish();
        Ok(())
    }

    pub(crate) fn apply_trace_mask(&mut self) -> Result<Option<(u8, u8)>> {
        let bits = *crate::module_parameters::g16_trace_mask.value() as u8;
        if bits == 0 || self.board.name != "J613" { return Ok(None); }
        let map = self.loader.iosys_map(0x1823a4 - 0x60000, 1)?;
        // SAFETY: one byte inside the retained firmware data reservation.
        let before = unsafe { map.as_ptr().read_volatile() };
        // SAFETY: as above; the firmware only reads this byte.
        unsafe { (map.as_ptr() as *mut u8).write_volatile(before | bits) };
        g16_memory::publish();
        // SAFETY: as above.
        Ok(Some((before, unsafe { map.as_ptr().read_volatile() })))
    }

    fn log_recovery(&mut self, dev: &driver::AsahiDevice) -> Result {
        for (object, name, offset, count) in [
            (Object::Activity26c, "engine-recovery", 0usize, 24usize),
            (Object::FirmwareData, "recovery-summary", firmware_data::RECOVERY_SUMMARY, 64)] {
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
        let producer = self.object(Object::KtraceState).read_u32(channel::WRITE)?;
        if producer >= 512 { return Err(EIO); }
        for age in (1..=48).rev() {
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
        let image_addresses: &[(usize, usize)] = &[(0x1823d0, 1)];
        for &(address, count) in image_addresses {
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
        let power = self.object(Object::PowerPerformance).read_u32(power_data::HARDWARE_STATE)?;
        dev_info!(dev.as_ref(), "G16G: firmware GPU power state={:#x}\n", power);
        for start in [0usize, 0x20, 0x40, 0xb80, 0xbc0, 0x21f0, 0x2208, 0x2260, 0x2270, 0x22c0, 0x22e0, 0x22f0, 0x6510, 0x6550] {
            let mut words = [0u32; 8];
            for (index, word) in words.iter_mut().enumerate() {
                *word = self.object(Object::PowerPerformance).read_u32(start + index * 4)?;
            }
            dev_info!(dev.as_ref(), "G16G: power policy +{:#x}={:x?}\n", start, words);
        }
        Ok(())
    }

    pub(crate) fn enqueue_j613_native_control(&mut self, opcode: u32,
        pipe: u32, kind: u32) -> Result<u32> {
        if self.board.chip_id != 0x8122 ||
            !((opcode == 0x16 && pipe == 0 && kind == 0) ||
              (opcode == 0x1b && pipe < 3 && matches!(kind, 1 | 2))) {
            return Err(EINVAL);
        }
        let [read, cfi, write] = self.control_indices()?;
        if read != write || cfi != write { return Err(EBUSY); }
        let mut entry = [0; channel::CONTROL_RECORD_SIZE];
        entry[..4].copy_from_slice(&opcode.to_le_bytes());
        entry[4..8].copy_from_slice(&pipe.to_le_bytes());
        entry[8..12].copy_from_slice(&kind.to_le_bytes());
        self.object(Object::DevctrlRing).write(write as usize * channel::CONTROL_RECORD_SIZE, &entry)?;
        g16_memory::publish();
        let next = (write + 1) % channel::CONTROL_CAPACITY;
        self.object(Object::DevctrlState).u32(channel::WRITE, next)?;
        g16_memory::publish();
        Ok(next)
    }

    pub(crate) fn enqueue_j613_idle_policy(&mut self, generation: &mut u32, enable: bool) -> Result<u32> {
        if self.board.chip_id != 0x8122 { return Err(EINVAL); }
        let [read, cfi, write] = self.control_indices()?;
        if read != write || cfi != write { return Err(EBUSY); }
        let mut entry = [0; channel::CONTROL_RECORD_SIZE];
        entry[..4].copy_from_slice(&0x0au32.to_le_bytes());
        entry[4..8].copy_from_slice(&(enable as u32).to_le_bytes());
        self.object(Object::DevctrlRing).write(write as usize * channel::CONTROL_RECORD_SIZE, &entry)?;
        *generation = (*generation).max(self.consumed_generation()?).checked_add(1).ok_or(EOVERFLOW)?;
        self.object(Object::Globals).write(0xe41, &generation.to_le_bytes())?;
        g16_memory::publish();
        let next = (write + 1) % channel::CONTROL_CAPACITY;
        self.object(Object::DevctrlState).u32(channel::WRITE, next)?;
        g16_memory::publish();
        Ok(next)
    }

    pub(crate) fn enqueue_j613_context_release(&mut self, args: [u8; 4], pointer: u64,
        generation: &mut u32) -> Result<u32> {
        if self.board.chip_id != 0x8122 || args[1] == 255 || args[2] == 255
            || args[3] != 4 || pointer & 7 != 0 { return Err(EINVAL); }
        let [read, cfi, write] = self.control_indices()?;
        if read != write || cfi != write { return Err(EBUSY); }
        let mut entry = [0; channel::CONTROL_RECORD_SIZE];
        entry[..4].copy_from_slice(&0x14u32.to_le_bytes());
        entry[8..12].copy_from_slice(&args);
        entry[12..20].copy_from_slice(&pointer.to_le_bytes());
        self.object(Object::DevctrlRing).write(write as usize * channel::CONTROL_RECORD_SIZE, &entry)?;
        *generation = generation.checked_add(1).ok_or(EOVERFLOW)?;
        self.object(Object::Globals).write(0xe41, &generation.to_le_bytes())?;
        g16_memory::publish();
        let next = (write + 1) % channel::CONTROL_CAPACITY;
        self.object(Object::DevctrlState).u32(channel::WRITE, next)?;
        g16_memory::publish();
        Ok(next)
    }

    pub(crate) fn enqueue_control(&mut self, opcode: u32, payload: u32,
        generation: &mut u32, device: &crate::g16_device::Device) -> Result<u32> {
        if !matches!(opcode, 0x1a | 0x34 | 0x0a) { return Err(EINVAL); }
        let [read, cfi, write] = self.control_indices()?;
        if read != write || cfi != write { return Err(EBUSY); }
        let mut entry = [0; channel::CONTROL_RECORD_SIZE];
        entry[..4].copy_from_slice(&opcode.to_le_bytes());
        entry[4..8].copy_from_slice(&payload.to_le_bytes());
        self.object(Object::DevctrlRing).write(write as usize * channel::CONTROL_RECORD_SIZE, &entry)?;
        *generation = generation.wrapping_add(1);
        device.set_power_generation(*generation)?;
        if self.board.chip_id == 0x8122 {
            self.object(Object::Globals).write(0xe41, &generation.to_le_bytes())?;
        }
        g16_memory::publish();
        let next = (write + 1) % channel::CONTROL_CAPACITY;
        self.object(Object::DevctrlState).u32(channel::WRITE, next)?;
        g16_memory::publish();
        Ok(next)
    }
    pub(crate) fn control_indices(&mut self) -> Result<[u32; 3]> {
        let state = self.object(Object::DevctrlState);
        let indices = [state.read_u32(channel::READ)?, state.read_u32(channel::CFI)?, state.read_u32(channel::WRITE)?];
        if indices.iter().any(|index| *index >= channel::CONTROL_CAPACITY) { return Err(EIO); }
        Ok(indices)
    }
    pub(crate) fn log_control_publication(&mut self, dev: &driver::AsahiDevice) -> Result {
        let mut packed = [0u8; 4];
        self.object(Object::Globals).read(0xe41, &mut packed)?;
        let mut entry = [0u8; 8];
        self.object(Object::DevctrlRing).read(0, &mut entry)?;
        dev_info!(dev.as_ref(), "G16G: device-control publication Globals+0xe41={} entry0={:x?} state={:#x} ring={:#x}\n",
            u32::from_le_bytes(packed), entry, self.object(Object::DevctrlState).va(),
            self.object(Object::DevctrlRing).va());
        Ok(())
    }
    pub(crate) fn consumed_generation(&mut self) -> Result<u32> {
        self.object(Object::FirmwareData).read_u32(firmware_data::CONSUMED_GENERATION)
    }
    pub(crate) fn reset_trace_budget(&mut self) { self.trace_seen = [0; 2]; }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_probe_data(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        let size = self.board.firmware.data_size as usize;
        let map = self.loader.iosys_map(0, size)?;
        dump.record("firmware-data-post-probe", 0xfffffc0000000000 + self.board.firmware.text_size,
            size, |out| {
                for (i, byte) in out.iter_mut().enumerate() {
                    // SAFETY: bounded read of the retained WC firmware RAM mapping.
                    *byte = unsafe { map.as_ptr().add(i).read_volatile() };
                }
                Ok(())
            })
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        use Object::*;
        if let Some(arena) = self.cached_programs.as_mut() {
            let count = self.cached_program_cursor.min(4 * 1024 * 1024);
            if count != 0 { dump.buffer("cached-program-prefix", arena, count)?; }
        }
        let data_size = self.board.firmware.data_size as usize;
        let map = self.loader.iosys_map(0, data_size)?;
        dump.record("firmware-data", 0xfffffc0000000000 + self.board.firmware.text_size, data_size, |out| {
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
            (KtraceData, "trace-data"), (DevctrlState, "device-control-state"),
            (DevctrlRing, "device-control-ring")] {
            dump.buffer(name, self.object(object), object.size())?;
        }
        Ok(())
    }
    pub(crate) fn rearm_scheduler(&mut self) -> Result {
        if self.object(Object::FirmwareData).read_u32(firmware_data::UNKNOWN_4FD4)? != 0 { return Err(EBUSY); }
        self.object(Object::FirmwareData).u32(firmware_data::UNKNOWN_4FD4, 1)?;
        g16_memory::publish();
        Ok(())
    }

    pub(crate) fn publish_queue(&mut self, pipe: usize, queue: u64, wptr: u16, slot: u8, new: bool) -> Result<u32> {
        if pipe >= channel::COUNT || slot >= 128 || queue & 7 != 0 { return Err(EINVAL); }
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
        if pipe >= channel::COUNT { return Err(EINVAL); }
        let state = &mut self.pipes[pipe * 2];
        Ok([state.read_u32(channel::READ)?, state.read_u32(channel::CFI)?, state.read_u32(channel::WRITE)?])
    }

    pub(crate) fn root(&self) -> u64 { self.va(Object::Root) }
    pub(crate) fn ready(&mut self) -> Result<bool> { Ok(self.object(Object::Control).read_u32(0x14)? == 1) }

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
