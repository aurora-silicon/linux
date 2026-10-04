// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned resources for the first compute-engine command. The runtime retains
//! this graph on any unproven retirement; a queue pointer is never ownership.

use kernel::prelude::*;
use crate::{driver, g16_dispatch, g16_compute::{self, Addresses}, g16_memory::{self, Buffer}, mmu};

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
    stamp: u32,
    consumed_pages: usize,
    /// Pool-state and page-list bytes for this owner's private pool.
    freelist: KVec<u8>,
    page_list_image: KVec<u8>,
}

const PRIVATE_BYTES: usize = 0x2000000;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 0x1000;

impl Compute {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, config: &mut crate::g16_config::Config, work_queue: u64) -> Result<KBox<Self>> {
        let vm = uat.new_vm(0x81320020, 0x70_0000_0000..0x80_0000_0000)?;
        Self::in_vm(dev, uat, config, &vm, work_queue, None, None, 1, g16_compute::STAMP, None, 0)
    }
    pub(crate) fn in_vm(dev: &driver::AsahiDevice, uat: &mmu::Uat,
        config: &mut crate::g16_config::Config, vm: &mmu::Vm, work_queue: u64,
        control: Option<g16_compute::Control>, notification: Option<g16_compute::Notification>, generation: u8, stamp: u32,
        reusable: Option<KBox<Self>>, event_slot: u8) -> Result<KBox<Self>> {
        // Only an owner displaced by a completely retired replacement may be
        // recycled, and only inside the same client GPUVM. Its GEM backing,
        // both command aliases, the 32 MiB private pool and the ASID lease
        // are retained; every firmware-visible record is rebuilt below.
        let reusable = reusable.filter(|job| control.is_some() && job.binding.matches(vm));
        let mut boxed = if let Some(mut job) = reusable {
            let owner: &mut Self = &mut *job;
            // Every firmware- or GPU-written record is reset before the new
            // command is encoded. The pool keeps its contents except the
            // pages the retired dispatch consumed from the freelist. The
            // freelist ring and page list are rewritten in full below, so
            // only their tails are cleared. The context store is written by
            // a preemption and read back only through the preemption record,
            // which is rebuilt from zero here.
            for buffer in [&mut owner.command, &mut owner.output, &mut owner.preemption,
                &mut owner.auxiliary, &mut owner.counter] { buffer.clear()?; }
            let pool_len = owner.freelist.len();
            owner.pool_state.clear_range(pool_len, owner.pool_state.size() - pool_len)?;
            let list_len = owner.page_list_image.len();
            owner.page_list.clear_range(list_len, owner.page_list.size() - list_len)?;
            let bytes = owner.consumed_pages.min(PRIVATE_PAGES) * 0x1000;
            owner.private_memory.clear_range(0, bytes)?;
            owner.stamp = stamp;
            job
        } else {
        let uma_client = if control.is_none() { Some(config.bind_uma_table(vm)?) } else { None };
        let gpu = |size| Buffer::new_gpu(dev, uat.kernel_vm(), &vm, size);
        // The engine fetches the register list in bootstrap context zero, then
        // switches to the client ASID. Retain exactly this command GEM in both
        // contexts; no user mappings or unrelated bootstrap pages are mirrored.
        let mut command = Buffer::new_command(dev, uat.kernel_vm(), uat.kernel_lower_vm(), g16_compute::SIZE)?;
        let command_client = command.map_gpu_view(&vm)?;
        let private_memory = g16_memory::GpuBuffer::new(dev, vm, PRIVATE_BYTES)?;
        let (freelist, page_list_image) = g16_memory::freelist_images(private_memory.va(), PRIVATE_PAGES)?;
        let mut job = KBox::new(Self {
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
            stamp,
            consumed_pages: PRIVATE_PAGES,
            freelist,
            page_list_image,
        }, GFP_KERNEL)?;
        dev_info!(dev.as_ref(), "G16G compute memory: command={:#x} preemption={:#x} auxiliary={:#x} context={:#x} private={:#x}+{:#x} pool={:#x} pages={:#x}\n",
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
            context: job.binding.slot(), generation, stamp, event_slot,
        };
        if let Some(control) = control {
            g16_compute::encode_dispatch(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32, control, notification).map_err(|_| EINVAL)?;
        } else {
            g16_compute::encode(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32).map_err(|_| EINVAL)?;
        }
        job.pool_state.write(0, &job.freelist)?;
        job.page_list.write(0, &job.page_list_image)?;
        job.command.write(0, &bytes[..])?;
        // One submitted engine uses this freshly initialized UMA owner.
        // Match firmware's UMA+54 completion count so TEXT 2b518 drains the
        // cached gUPM slot before another client's pool is installed.
        job.counter.u64(0, 1)?;
        job.shader.write(0, &g16_dispatch::image(job.output.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.esl.write(0, &g16_dispatch::esl(job.shader.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.cdm.write(0, &g16_dispatch::cdm(job.esl.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.output.u32(0, g16_dispatch::SENTINEL)?;
        g16_memory::publish();
        job.log_context(dev)?;
        Ok(boxed)
    }
    pub(crate) fn set_user_timestamps(&mut self, addresses: [u64; 2]) -> Result {
        if addresses == [0, 0] { return Ok(()); }
        // Timestamp reads the retained pointer pair and publishes nanoseconds.
        self.command.u64(0x844, addresses[0])?;
        self.command.u64(0x84c, addresses[1])?;
        self.command.u64(0xabc + 36, self.command.va() + 0x844)?;
        self.command.u64(0xb0c + 36, self.command.va() + 0x844)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn uma_completed(&mut self) -> Result<u64> {
        self.command.read_u64(0xe54)
    }
    pub(crate) fn set_attachments(&mut self, attachments: &crate::g16_attachments::Attachments) -> Result {
        self.command.write(0x958, &attachments.0)?;
        self.command.write(0xbc0, &[u8::from(attachments.count() != 0)])?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn log_context(&self, dev: &driver::AsahiDevice) -> Result {
        let mut roots = [(0, 0); 2];
        self.binding.vm().context_roots(&mut roots)?;
        dev_info!(dev.as_ref(), "G16G: owned compute user context={} roots={:x?}\n", self.binding.slot(), roots);
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
    /// Fault-only owned records: never walk application addresses or copy the
    /// private pool. These sequential observations preserve the command's UMA
    /// descriptor beside the hardware slot after retirement has failed.
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
        dump.buffer("compute-command", &mut self.command, g16_compute::SIZE)?;
        dump.buffer("compute-preemption", &mut self.preemption, 0x4000)?;
        dump.buffer("compute-auxiliary", &mut self.auxiliary, 0x4000)?;
        dump.buffer("compute-context-store", &mut self.context_store, 0x40000)?;
        dump.buffer("compute-pool-state", &mut self.pool_state, 0x20000)?;
        dump.buffer("compute-page-list", &mut self.page_list, 0x4000)?;
        dump.buffer("compute-counter", &mut self.counter, 8)
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_retired_command(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("compute-retired-command", &mut self.command, g16_compute::SIZE)
    }
    /// Record the retired dispatch's private-page consumption for recycling.
    pub(crate) fn record_consumption(&mut self) -> Result {
        self.consumed_pages = g16_memory::consumed_pages(&mut self.command, 0xe00, PRIVATE_PAGES)?;
        Ok(())
    }
    pub(crate) fn same_vm(&self, vm: &mmu::Vm) -> bool { self.binding.matches(vm) }
    pub(crate) fn stamp(&self) -> u32 { self.stamp }
    pub(crate) fn address(&self) -> u64 { self.command.va() }
    pub(crate) fn status(&mut self) -> Result<[u64; 10]> {
        Ok([self.command.read_u32(0xee0)? as u64, self.command.read_u32(0xee4)? as u64,
            self.command.read_u64(0x8c0)?, self.command.read_u64(0x8c8)?,
            self.command.read_u32(0x8ac)? as u64, self.command.read_u64(0xf00)?,
            self.command.read_u32(0xe44)? as u64, self.command.read_u64(0x89c)?,
            self.command.read_u32(0x76c)? as u64, self.output.read_u32(0)? as u64])
    }
}
