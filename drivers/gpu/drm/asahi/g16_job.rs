// SPDX-License-Identifier: GPL-2.0-only OR MIT


use kernel::prelude::*;
use crate::{driver, g16_dispatch, g16_compute::{self, Addresses, layout, uma_layout}, g16_memory::{self, Buffer}, mmu};

pub(crate) struct Compute {
    _command_client: mmu::KernelMapping,
    _uma_client: Option<mmu::KernelMapping>,
    command: Buffer,
    cdm: Buffer,
    shader: Buffer,
    esl: Buffer,
    output: Buffer,
    preemption: Buffer,
    auxiliary: Buffer,
    context_store: Buffer,
    private_memory: g16_memory::GpuBuffer,
    pool_state: Buffer,
    page_list: Buffer,
    counter: Buffer,
    binding: mmu::VmBind,
    notifier_owner: Option<crate::g16_queue::Client>,
    pool_slot: u8,
    stamp: u32,
    consumed_pages: usize,
    uma_submitted: u64,
    freelist: KVec<u8>,
    page_list_image: KVec<u8>,
}

const PRIVATE_BYTES: usize = 0x2000000;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 0x1000;

impl Compute {
    pub(crate) fn resident_bytes(&self) -> usize {
        let buffers = [&self.command, &self.cdm, &self.shader, &self.esl, &self.output,
            &self.preemption, &self.auxiliary, &self.context_store, &self.pool_state,
            &self.page_list, &self.counter];
        buffers.iter().map(|b| crate::util::align(b.size(), crate::mmu::UAT_PGSZ)).sum::<usize>()
            + crate::util::align(self.private_memory.size(), crate::mmu::UAT_PGSZ)
    }
    pub(crate) fn retain_notifier(&mut self, notifier: crate::g16_queue::Client) {
        self.notifier_owner = Some(notifier);
    }
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, config: &mut crate::g16_config::Config, work_queue: u64) -> Result<KBox<Self>> {
        let vm = uat.new_vm(0x81320020, 0x70_0000_0000..0x80_0000_0000)?;
        Self::in_vm(dev, uat, config, &vm, work_queue, None, None, g16_compute::STAMP, None, 0, 1)
    }
    pub(crate) fn in_vm(dev: &driver::AsahiDevice, uat: &mmu::Uat,
        config: &mut crate::g16_config::Config, vm: &mmu::Vm, work_queue: u64,
        control: Option<g16_compute::Control>, notification: Option<g16_compute::Notification>, stamp: u32,
        reusable: Option<KBox<Self>>, event_slot: u8, pool_slot: u8) -> Result<KBox<Self>> {
        if pool_slot == 0 || usize::from(pool_slot) > crate::g16_runtime::POOL_SLOTS { return Err(EINVAL); }
        let reusable = reusable.filter(|job| control.is_some() && job.binding.matches(vm));
        let mut saved_uma = [0; uma_layout::SIZE];
        let mut boxed = if let Some(mut job) = reusable {
            let owner: &mut Self = &mut *job;
            if owner.uma_completed()? != owner.uma_submitted { return Err(EBUSY); }
            owner.command.read(layout::UMA, &mut saved_uma)?;
            for buffer in [&mut owner.command, &mut owner.output, &mut owner.preemption,
                &mut owner.auxiliary] { buffer.clear()?; }
            owner.stamp = stamp;
            owner.pool_slot = pool_slot;
            job
        } else {
        let uma_client = if control.is_none() { Some(config.bind_uma_table(vm)?) } else { None };
        let gpu = |size| Buffer::new_gpu(dev, uat.kernel_vm(), &vm, size);
        let mut command = Buffer::new_command(dev, uat.kernel_vm(), uat.kernel_lower_vm(), layout::ALLOCATION_SIZE)?;
        let command_client = command.map_gpu_view(&vm)?;
        let private_memory = g16_memory::GpuBuffer::new(dev, vm, PRIVATE_BYTES)?;
        let (freelist, page_list_image) = g16_memory::freelist_images(private_memory.va(), PRIVATE_PAGES)?;
        let job = KBox::new(Self {
            _command_client: command_client,
            _uma_client: uma_client,
            command,
            cdm: Buffer::new_gpu_readonly(dev, uat.kernel_vm(), &vm, 0x4000)?,
            shader: Buffer::new_gpu_readonly(dev, uat.kernel_vm(), &vm, 0x4000)?,
            esl: Buffer::new_gpu_readonly(dev, uat.kernel_vm(), &vm, 0x4000)?,
            output: gpu(0x4000)?,
            preemption: gpu(0x4000)?,
            auxiliary: gpu(0x4000)?,
            context_store: gpu(0x40000)?,
            private_memory,
            pool_state: gpu(0x20000)?,
            page_list: gpu(0x4000)?,
            counter: gpu(8)?,
            binding: uat.bind(&vm)?,
                notifier_owner: None,
            pool_slot,
            stamp,
            consumed_pages: PRIVATE_PAGES,
            uma_submitted: 0,
            freelist,
            page_list_image,
        }, GFP_KERNEL)?;
        crate::cls_dev_dbg!(Compute, dev, "G16G compute memory: command={:#x} preemption={:#x} auxiliary={:#x} context={:#x} private={:#x}+{:#x} pool={:#x} pages={:#x}\n",
            job.command.gpu_va()?, job.preemption.gpu_va()?, job.auxiliary.gpu_va()?,
            job.context_store.gpu_va()?, job.private_memory.va(), PRIVATE_BYTES,
            job.pool_state.gpu_va()?, job.page_list.gpu_va()?);
        job
        };
        let job: &mut Self = &mut *boxed;
        let mut bytes = KBox::new([0; g16_compute::SIZE], GFP_KERNEL)?;
        let addresses = Addresses {
            command: job.command.va(), command_gpu: job.command.gpu_va()?,
            cdm_gpu: job.cdm.gpu_va()?,
            preemption_gpu: job.preemption.gpu_va()?, metrics: job.auxiliary.va(),
            auxiliary_gpu: job.auxiliary.gpu_va()?,
            work_queue, stats: config.compute_stats(), context_store_gpu: job.context_store.gpu_va()?,
            pool_state_gpu: job.pool_state.gpu_va()?, page_list_gpu: job.page_list.gpu_va()?,
            counter: job.counter.va(),
            context: job.binding.slot(), generation: job.binding.generation(), stamp, event_slot,
        };
        if let Some(control) = control {
            g16_compute::encode_dispatch(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32, control, notification).map_err(|_| EINVAL)?;
        } else {
            g16_compute::encode(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32).map_err(|_| EINVAL)?;
        }
        if job.uma_submitted == 0 {
            job.pool_state.write(0, &job.freelist)?;
            job.page_list.write(0, &job.page_list_image)?;
        }
        g16_compute::bind_uma_slot(&mut bytes[..],
            (job.uma_submitted != 0).then_some(&saved_uma), job.pool_slot, job.command.va());
        job.command.write(0, &bytes[..])?;
        job.uma_submitted = job.uma_submitted.checked_add(1).ok_or(EOVERFLOW)?;
        let submitted = job.uma_submitted;
        job.counter.u64(0, submitted)?;
        job.shader.write(0, &g16_dispatch::image(job.output.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.esl.write(0, &g16_dispatch::esl(job.shader.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.cdm.write(0, &g16_dispatch::cdm(job.esl.gpu_va()?).map_err(|_| EINVAL)?)?;
        if *crate::module_parameters::g16_qualify.value() != 0 && control.is_none() && crate::g16_board::get()?.chip_id == 0x8122 {
            let mut image = KBox::new([0u8; 0x4000], GFP_KERNEL)?;
            image[..0x400].copy_from_slice(&g16_dispatch::image(job.output.gpu_va()?).map_err(|_| EINVAL)?);
            let original = g16_dispatch::shader(job.output.gpu_va()?).map_err(|_| EINVAL)?;
            let start = g16_dispatch::ENTRY;
            image[start..start+24].copy_from_slice(&original[..24]);
            for i in 0..1024 { image[start+24+i*8..start+32+i*8].copy_from_slice(&[0x29,5,6,0xb0,0x21,0x80,0,2]); }
            image[start+24+8192..start+42+8192].copy_from_slice(&original[24..]);
            job.shader.write(0, &image[..])?;
            let mut launch = g16_dispatch::cdm(job.esl.gpu_va()?).map_err(|_| EINVAL)?;
            launch[16..20].copy_from_slice(&1048576u32.to_le_bytes());
            launch[20..24].copy_from_slice(&1u32.to_le_bytes());
            launch[28..32].copy_from_slice(&32u32.to_le_bytes());
            for i in 0..128 { job.cdm.write(i*44, &launch[..44])?; }
            job.cdm.u32(128*44, 0x40000000)?;
            job.command.u64(layout::CDM_LAST, job.cdm.gpu_va()? + (128*44) as u64)?;
            dev_info!(dev.as_ref(), "G16G: J613 bounded burn launches=128 global_x=1048576 global_y=1 local=32 dependent_ffma=1024 total_lane_ffma=137438953472 watchdog_ms=2000\n");
        }
        job.output.u32(0, g16_dispatch::SENTINEL)?;
        g16_memory::publish();
        job.log_context(dev)?;
        Ok(boxed)
    }
    pub(crate) fn dependency(&mut self, firmware_stamp: u64, slot: u8, previous: u32) -> Result<u64> {
        let mut bytes = [0; crate::g16_render_command::dependency::SIZE];
        crate::g16_render_command::render_dependency(&mut bytes, firmware_stamp,
            slot, previous, self.stamp).map_err(|_| EINVAL)?;
        bytes[crate::g16_render_command::dependency::INTERNAL..crate::g16_render_command::dependency::INTERNAL+4].copy_from_slice(&1u32.to_le_bytes());
        self.command.write(layout::DEPENDENCY, &bytes)?;
        Ok(self.command.va() + layout::DEPENDENCY as u64)
    }

    pub(crate) fn set_user_timestamps(&mut self, addresses: [u64; 2]) -> Result {
        if addresses == [0, 0] { return Ok(()); }
        self.command.u64(layout::USER_TIME_PAIR, addresses[0])?;
        self.command.u64(layout::USER_TIME_PAIR_END, addresses[1])?;
        self.command.u64(layout::TIMESTAMP_START + g16_compute::timestamp::USER_PAIR, self.command.va() + layout::USER_TIME_PAIR as u64)?;
        self.command.u64(layout::TIMESTAMP_END + g16_compute::timestamp::USER_PAIR, self.command.va() + layout::USER_TIME_PAIR as u64)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn set_timestamp_copies(&mut self, addresses: [u64; 8]) -> Result {
        let mut pairs = [0u64; 8];
        let mut count = 0;
        for address in addresses.into_iter().filter(|a| *a != 0) {
            let offset = layout::FANOUT_PAIRS + count * 16;
            self.command.u64(offset, 0)?;
            self.command.u64(offset + 8, address)?;
            pairs[count] = self.command.va() + offset as u64;
            count += 1;
        }
        if count == 0 { return Ok(()); }
        let mut sequence = [0u8; 0x600];
        self.command.read(layout::SEQUENCE, &mut sequence[..layout::SEQUENCE_SIZE])?;
        let size = g16_compute::timestamp_copies(&mut sequence, &pairs[..count])
            .map_err(|_| EINVAL)?;
        self.command.write(layout::FANOUT_SEQUENCE, &sequence[..size])?;
        self.command.u64(layout::SEQUENCE_POINTER, self.command.va() + layout::FANOUT_SEQUENCE as u64)?;
        self.command.u32(layout::SEQUENCE_LENGTH, size.try_into()?)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn coalesce_stamp_flush(&mut self) -> Result {
        self.command.u32(layout::FLUSH_STAMPS, 0)
    }
    pub(crate) fn uma_submitted(&self) -> u64 { self.uma_submitted }
    pub(crate) fn uma_completed(&mut self) -> Result<u64> {
        self.command.read_u64(layout::UMA + uma_layout::COMPLETED)
    }
    pub(crate) fn set_attachments(&mut self, attachments: &crate::g16_attachments::Attachments) -> Result {
        self.command.write(layout::ATTACHMENTS, &attachments.0)?;
        self.command.write(layout::FINALIZE_HAS_ATTACHMENTS, &[u8::from(attachments.count() != 0)])?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn log_context(&self, dev: &driver::AsahiDevice) -> Result {
        if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
            let mut roots = [(0, 0); 2];
            self.binding.vm().context_roots(&mut roots)?;
            dev_info!(dev.as_ref(), "G16G: owned compute user context={} roots={:x?}\n", self.binding.slot(), roots);
        }
        Ok(())
    }
    pub(crate) fn log_progress(&mut self, dev: &driver::AsahiDevice) -> Result {
        let mut cells = [0u64; 4];
        for (index, cell) in cells.iter_mut().enumerate() {
            *cell = self.preemption.read_u64(0x1480 + index * 8)?;
        }
        dev_info!(dev.as_ref(), "G16G: compute preemption progress={:x?} private-pages={}\n", cells, PRIVATE_PAGES);
        Ok(())
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump,
        bootstrap: &mmu::Vm) -> Result {
        let gpu = self.command.gpu_va()?;
        let mut mappings = [0u8; 24];
        mappings[..8].copy_from_slice(&gpu.to_le_bytes());
        mappings[8..16].copy_from_slice(&(bootstrap.translate_iova(gpu)? as u64).to_le_bytes());
        mappings[16..24].copy_from_slice(&(self.binding.vm().translate_iova(gpu)? as u64).to_le_bytes());
        dump.record("compute-command-mappings", gpu, mappings.len(),
            |out| { out.copy_from_slice(&mappings); Ok(()) })?;
        dump.buffer("compute-command", &mut self.command, layout::ALLOCATION_SIZE)?;
        dump.buffer("compute-preemption", &mut self.preemption, 0x4000)?;
        dump.buffer("compute-auxiliary", &mut self.auxiliary, 0x4000)?;
        dump.buffer("compute-context-store", &mut self.context_store, 0x40000)?;
        dump.buffer("compute-pool-state", &mut self.pool_state, 0x20000)?;
        dump.buffer("compute-page-list", &mut self.page_list, 0x4000)?;
        dump.buffer("compute-counter", &mut self.counter, 8)
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_retired_command(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("compute-retired-command", &mut self.command, layout::ALLOCATION_SIZE)
    }
    pub(crate) fn record_consumption(&mut self) -> Result {
        self.consumed_pages = g16_memory::consumed_pages(&mut self.command, layout::UMA, PRIVATE_PAGES)?;
        Ok(())
    }
    pub(crate) fn pool_slot(&self) -> u8 { self.pool_slot }
    pub(crate) fn same_vm(&self, vm: &mmu::Vm) -> bool { self.binding.matches(vm) }
    pub(crate) fn stamp(&self) -> u32 { self.stamp }
    pub(crate) fn address(&self) -> u64 { self.command.va() }
    pub(crate) fn status(&mut self) -> Result<[u64; 10]> {
        Ok([self.command.read_u32(layout::FIRMWARE_STAMP)? as u64, self.command.read_u32(layout::USER_STAMP)? as u64,
            self.command.read_u64(layout::TIME_START_CELL)?, self.command.read_u64(layout::TIME_END_CELL)?,
            self.command.read_u32(layout::STATUS)? as u64, self.command.read_u64(layout::THRESHOLD)?,
            self.command.read_u32(layout::UMA + uma_layout::UNKNOWN_44)? as u64, self.command.read_u64(layout::TIME_AUXILIARY)?,
            self.command.read_u32(layout::ENGINE_STATE)? as u64, self.output.read_u32(0)? as u64])
    }
}
