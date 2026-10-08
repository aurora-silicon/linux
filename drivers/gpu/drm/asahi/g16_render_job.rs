// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! GEM/GPUVM ownership for a 25G83 render pass in a bounded GPU batch. There are no borrowed
//! lab mappings. A hardware slot retains its last owner until replacement.
use kernel::prelude::*;
use crate::{driver, mmu, g16_memory::{self, Buffer, GpuBuffer}, g16_render::{self, Register, Stage},
    g16_render_state::{self as state, State, Program, DepthStencil}, g16_render_command as wire,
    g16_uapi::UapiRenderCommand};
use wire::{metadata, dependency, init_buffer};
use crate::g16_compute::uma_layout;

const PRIVATE_BYTES: usize = 32 * 1024 * 1024;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 4096;
const PRIVATE_QWORDS: usize = (PRIVATE_PAGES + 1).next_power_of_two();
pub(crate) const SLOTS: [u8; 2] = [0x70, 0x71];

pub(crate) struct Render {
    _command_views: KVec<mmu::KernelMapping>,
    commands: [Buffer; 2],
    sequences: [Buffer; 2],
    metadata: Buffer,
    parameters: crate::g16_parameter::Pool,
    init_parameters: bool,
    scratch: Buffer,
    scene_list: Buffer,
    tilemap: Buffer,
    tpc: Buffer,
    unknown: Buffer,
    preemption: [Buffer; 3],
    auxiliary: Buffer,
    uma: Buffer,
    private_memory: GpuBuffer,
    pool_state: Buffer,
    page_list: Buffer,
    binding: mmu::VmBind,
    notifier_owner: Option<crate::g16_queue::Client>,
    pub(crate) stage: Stage,
    stamp: u32,
    pool_slot: u8,
    compute_dependency: bool,
    layout: [u32; 4],
    consumed_pages: usize,
    uma_submitted: u64,
    freelist: KVec<u8>,
    page_list_image: KVec<u8>,
    pub(crate) clear_metrics: [u64; 3],
}
impl Render {
    pub(crate) fn resident_bytes(&self) -> usize {
        let buffers = [&self.commands[0], &self.commands[1], &self.sequences[0],
            &self.sequences[1], &self.metadata, &self.scratch, &self.scene_list,
            &self.tilemap, &self.tpc, &self.unknown, &self.preemption[0],
            &self.preemption[1], &self.preemption[2], &self.auxiliary, &self.uma,
            &self.pool_state, &self.page_list];
        buffers.iter().map(|b| crate::util::align(b.size(), crate::mmu::UAT_PGSZ)).sum::<usize>()
            + crate::util::align(self.private_memory.size(), crate::mmu::UAT_PGSZ)
            + self.parameters.lock().resident_bytes()
    }
    pub(crate) fn request_completion_flush(&mut self) -> Result {
        self.commands[0].u32(wire::layout(Stage::Tiling).command_flush_stamps, 1)?;
        self.commands[1].u32(wire::layout(Stage::Fragment).command_flush_stamps, 1)?;
        Ok(())
    }
    pub(crate) fn retain_notifier(&mut self, notifier: crate::g16_queue::Client) {
        self.notifier_owner = Some(notifier);
    }
    pub(crate) fn notifier(&self) -> Result<&crate::g16_queue::Client> {
        self.notifier_owner.as_ref().ok_or(EIO)
    }
    pub(crate) fn profiling_key(&self) -> [u32; 3] {
        [self.binding.slot(), self.layout[0], self.layout[1]]
    }

    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        self.notifier()?.lock().capture_render_fault(dump)?;
        let mut parameters = self.parameters.lock();
        dump.buffer("render-parameter-manager", &mut parameters.manager, wire::parameter::SIZE)?;
        dump.buffer("render-parameter-ring", &mut parameters.ring, 8)?;
        drop(parameters);
        dump.buffer("render-scene-metadata", &mut self.metadata, metadata::SIZE)?;
        dump.buffer("render-uma-descriptor", &mut self.uma, 0x300)?;
        dump.buffer("render-pool-state", &mut self.pool_state, PRIVATE_QWORDS * 8)?;
        for (i, stage) in [Stage::Tiling, Stage::Fragment].into_iter().enumerate() {
            dump.buffer(["render-ta-command", "render-fragment-command"][i],
                &mut self.commands[i], stage.command_size())?;
            dump.buffer(["render-ta-sequence", "render-fragment-sequence"][i],
                &mut self.sequences[i], if i == 0 { 0x400 } else { 0x800 })?;
        }
        Ok(())
    }

    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, vm: &mmu::Vm,
        r: UapiRenderCommand, usc: u64, label: u8, stamp: u32, counter: u64,
        queues: [u64; 2], notifications: [crate::g16_compute::Notification; 2], stats: [u64; 2],
        pool_slot: u8, reusable: Option<KBox<Self>>,
        shared: Option<crate::g16_parameter::Pool>) -> Result<KBox<Self>> {
        if usize::from(pool_slot) >= crate::g16_runtime::POOL_SLOTS { return Err(EINVAL); }
        let geometry = crate::g16_parameter::geometry(r)?;
        let pb_bytes = crate::g16_parameter::bytes(&geometry)?;
        let layout = [r.width as u32, r.height as u32,
            r.utile_width as u32, r.utile_height as u32];
        let reusable = reusable.filter(|job| job.binding.matches(vm));
        let retargeted = reusable.as_ref().is_some_and(|job| job.pool_slot != pool_slot);
        let reused = reusable.is_some();
        let mut job = if let Some(mut job) = reusable {
            let gpu = |size| Buffer::new_gpu(dev,uat.kernel_vm(),vm,size);
            if job.tilemap.size() < geometry.tilemap_bytes.try_into()? {
                job.tilemap = gpu(geometry.tilemap_bytes.try_into()?)?;
            }
            if job.tpc.size() < geometry.tpc_bytes.try_into()? {
                job.tpc = gpu(geometry.tpc_bytes.try_into()?)?;
            }
            job.clear()?;
            if retargeted {
                job.uma.u32(8, 1 + crate::g16_runtime::POOL_SLOTS as u32 + u32::from(pool_slot))?;
                job.pool_slot = pool_slot;
            }
            if let Some(shared) = shared {
                job.parameters = shared;
            } else if job.parameters.lock().shared {
                job.parameters = crate::g16_parameter::Parameters::new(
                    dev, uat, vm, pb_bytes, u32::from(pool_slot), false)?;
            } else {
                let mut parameters = job.parameters.lock();
                parameters.id = u32::from(pool_slot);
                parameters.rebuild(dev, uat, vm, pb_bytes)?;
            }
            job.layout = layout;
            job.stage = Stage::Tiling;
            job.stamp = stamp;
            job
        } else {
            let gpu = |size| Buffer::new_gpu(dev,uat.kernel_vm(),vm,size);
            let fw = |size| Buffer::new(dev,uat.kernel_vm(),size);
            let mut commands = [
                Buffer::new_command(dev,uat.kernel_vm(),uat.kernel_lower_vm(),Stage::Tiling.command_size())?,
                Buffer::new_command(dev,uat.kernel_vm(),uat.kernel_lower_vm(),Stage::Fragment.command_size())?];
            let mut views = KVec::new();
            for command in &mut commands { views.push(command.map_gpu_view(vm)?,GFP_KERNEL)?; }
            let private_memory = GpuBuffer::new(dev, vm, PRIVATE_BYTES)?;
            let (freelist, page_list_image) = g16_memory::freelist_images(private_memory.va(), PRIVATE_PAGES)?;
            KBox::new(Self {
                _command_views:views, commands, sequences:[fw(0x400)?,fw(0x800)?],
                metadata:fw(metadata::SIZE)?,
                parameters: match shared {
                    Some(shared) => shared,
                    None => crate::g16_parameter::Parameters::new(
                        dev, uat, vm, pb_bytes, u32::from(pool_slot), false)?,
                }, init_parameters:false,
                scratch:gpu(0x4000)?,scene_list:gpu(0x4000)?,tilemap:gpu(geometry.tilemap_bytes.try_into()?)?,
                tpc:gpu(geometry.tpc_bytes.try_into()?)?,unknown:gpu(0x4000)?,
                preemption:[gpu(0x10000)?,gpu(0x10000)?,gpu(0x4000)?],auxiliary:gpu(0x10000)?,
                uma:gpu(0x4000)?,private_memory,
                pool_state:gpu(PRIVATE_QWORDS*8)?,page_list:gpu(0x4000)?,
                binding:uat.bind(vm)?,
                notifier_owner: None,stage:Stage::Tiling,stamp,pool_slot,compute_dependency:false,layout,
                consumed_pages:PRIVATE_PAGES, uma_submitted:0, freelist, page_list_image, clear_metrics:[0; 3],
            }, GFP_KERNEL)?
        };
        if job.uma_submitted == 0 {
            let owner: &mut Self = &mut *job;
            owner.pool_state.write(0, &owner.freelist)?;
            owner.page_list.write(0, &owner.page_list_image)?;
        }
        if job.uma_submitted == 0 {
            let mut uma = [0;uma_layout::SIZE];
            crate::g16_compute::encode_uma(&mut uma,job.pool_state.gpu_va()?,PRIVATE_QWORDS as u32,
                PRIVATE_PAGES as u32,job.page_list.gpu_va()?,job.metadata.va()+metadata::UMA_SUBMITTED as u64,
                crate::g16_compute::UmaEngine::Render).map_err(|_| EINVAL)?;
            uma[uma_layout::SLOT..uma_layout::SLOT+4].copy_from_slice(&(1 + crate::g16_runtime::POOL_SLOTS as u32 + u32::from(pool_slot)).to_le_bytes());
            uma[uma_layout::OWNER..uma_layout::OWNER+8].copy_from_slice(&job.uma.va().to_le_bytes());
            job.uma.write(0,&uma)?;
        }
        job.compute_dependency = false;
        job.uma_submitted = job.uma_submitted.checked_add(2).ok_or(EOVERFLOW)?;
        let submitted = job.uma_submitted;
        job.metadata.u64(metadata::UMA_SUBMITTED, submitted)?;
        let (parameter_manager, parameter_id, parameter_pages, shared_pool, init_parameters) = {
            let mut pool = job.parameters.lock();
            (pool.manager.va(), pool.id, pool.pages(), pool.shared, pool.claim_init())
        };
        job.init_parameters = init_parameters;
        let mut init = [0;init_buffer::SIZE];
        wire::init_parameter_buffer(&mut init,job.binding.slot(),parameter_id,0,0,parameter_manager,stamp).map_err(|_| EINVAL)?;
        job.metadata.write(metadata::INIT_BUFFER,&init)?;
        let mut dependency = [0;dependency::SIZE];
        wire::render_dependency(&mut dependency,notifications[0].fw_stamp,
            SLOTS[0],stamp,stamp).map_err(|_| EINVAL)?;
        job.metadata.write(metadata::TILER_DEPENDENCY,&dependency)?;
        let scratch_gpu = job.scratch.gpu_va()?;
        let scratch_fw = job.scratch.va();
        let statistics = job.metadata.va()+metadata::STATISTICS as u64;
        job.metadata.u64(metadata::SCRATCH_GPU,scratch_gpu)?;
        job.metadata.u64(metadata::SCRATCH_FW,scratch_fw)?;
        let scene_list = state::compact(job.scene_list.gpu_va()?).map_err(|_| EINVAL)?;
        job.metadata.u64(metadata::COMPACT_SCENE_LIST,scene_list)?;
        job.metadata.u64(metadata::STATISTICS_POINTER,statistics)?;
        let mask = if r.flags & 16 != 0 {u64::MAX} else {u32::MAX as u64};
        let program = |p:crate::g16_uapi::UapiProgram| Program {
            address:usc+u64::from(p.usc & !63), resources:p.resource_spec & mask };
        let state = State { geometry,parameter_buffer_id:parameter_id,vdm:r.vdm_base,tilemap:job.tilemap.gpu_va()?,tpc:job.tpc.gpu_va()?,
            unknown:job.unknown.gpu_va()?,preemption:[job.preemption[0].gpu_va()?,job.preemption[1].gpu_va()?,job.preemption[2].gpu_va()?],
            scratch:job.scratch.gpu_va()?,scene_list:job.scene_list.gpu_va()?,auxiliary:job.auxiliary.gpu_va()?,multisample:r.multisample_control,ppp:r.ppp_control,
            merge_upper:[r.merge_upper_x,r.merge_upper_y],
            tilebuffer_blocks:(u32::from(r.sample_size)*u32::from(r.utile_width)*u32::from(r.utile_height)*u32::from(r.samples)).div_ceil(2048),
            background:program(r.background),eot:program(r.end_of_tile),
            partial_background:program(r.partial_background),partial_eot:program(r.partial_end_of_tile),
            depth_bias:r.depth_bias_base,scissor:r.scissor_base,query:r.occlusion_query_base,
            depth:DepthStencil{base:r.depth.base,stride:r.depth.stride as u64},
            stencil:DepthStencil{base:r.stencil.base,stride:r.stencil.stride as u64},
            depth_dimensions:r.depth_dimensions as u64,zls_control:r.zls_control,
            depth_clear:r.depth_clear,stencil_clear:r.stencil_clear,process_empty_tiles:r.flags&2!=0,
            integer_depth_bias:r.flags&(1<<18)!=0 };
        let mut work = KBox::new([0;0xca0],GFP_KERNEL)?;
        let mut sequence = [0;0x400];
        let mut registers = KBox::new([Register::default();128],GFP_KERNEL)?;
        for (i,stage) in [Stage::Tiling,Stage::Fragment].into_iter().enumerate() {
            let uuid = 0x16000000 | (u32::from(label)<<8) | i as u32;
            wire::encode(&mut work[..],&mut sequence,stage,wire::Args {
                command:job.commands[i].va(),sku:job.sequences[i].va(),queue:queues[i],
                stats:stats[i],
                pb_slot:job.metadata.va(),manager:parameter_manager,pb_aux:job.metadata.va()+metadata::PB_AUX as u64,
                notifier:notifications[i].address,shared_stamp:notifications[i].fw_stamp,
                stamp_address:notifications[i].stamp,uma:job.uma.va(),
                uma_aux:job.uma.gpu_va()?+metadata::UMA_AUX[i] as u64,
                timestamp_storage:job.metadata.va()+metadata::TIME_PAIRS[i] as u64,
                context:job.binding.slot(),generation:job.binding.generation(),counter:counter + u64::from(i == 0),uuid,stamp:stamp,
                event_slot:SLOTS[i] as u32,fragment_slot:SLOTS[1] as u32,
                fragment_stamp:stamp,pb_id:parameter_id,pass_id:0 }).map_err(|_| EINVAL)?;
            let (mut count,mode) = state::registers(&mut registers[..],stage,&state,uuid).map_err(|_| EINVAL)?;
            registers[count] = Register { offset:if i==0 {0x10060} else {0x10068},flagged:true,value:usc };
            count+=1;
            g16_render::write_registers(&mut work[..],stage,job.commands[i].gpu_va()?,&registers[..count]).map_err(|_| EINVAL)?;
            state::mirrors(&mut work[..],stage,&state,job.tpc.va(),job.uma.va(),job.uma.va()+metadata::UMA_AUX[i] as u64,mode).map_err(|_| EINVAL)?;
            job.commands[i].write(0,&work[..stage.command_size()])?;
            job.sequences[i].write(0,&sequence[..wire::sequence_size(stage)])?;
        }
        g16_memory::publish();
        crate::cls_dev_dbg!(Render, dev, "G16G: render USC freelist={:#x} capacity={} populated={}\n",
            job.pool_state.gpu_va()?,PRIVATE_QWORDS,PRIVATE_PAGES);
        crate::cls_dev_dbg!(SubmitTiming, dev,"G16G: owned render {}x{} ASID={} PB={} pages parameter_id={} shared_pool={} init_pool={} tilemap={:#x} TPC={:#x} VDM={:#x} reused={} retargeted={}\n",
            r.width,r.height,job.binding.slot(),parameter_pages,parameter_id,shared_pool,init_parameters,job.tilemap.gpu_va()?,job.tpc.gpu_va()?,r.vdm_base,reused,retargeted);
        Ok(job)
    }
    pub(crate) fn set_attachments(&mut self, attachments: &[crate::g16_attachments::Attachments; 2]) -> Result {
        for (i, stage) in [Stage::Tiling, Stage::Fragment].into_iter().enumerate() {
            let layout = wire::layout(stage);
            let (start, finish_flag) = (layout.attachments, layout.finalize_has_attachments);
            self.sequences[i].write(start, &attachments[i].0)?;
            self.sequences[i].write(finish_flag, &[u8::from(attachments[i].count() != 0)])?;
        }
        g16_memory::publish();
        Ok(())
    }
    fn clear(&mut self) -> Result {
        if self.uma_completed()? != self.uma_submitted { return Err(EBUSY); }
        let profiling = crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary);
        let now = || <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get();
        let start = if profiling { now() } else { 0 };
        for buffer in &mut self.commands { buffer.clear()?; }
        for buffer in &mut self.sequences { buffer.clear()?; }
        for buffer in &mut self.preemption { buffer.clear()?; }
        for buffer in [
            &mut self.metadata,
            &mut self.scratch, &mut self.scene_list, &mut self.tilemap, &mut self.tpc,
            &mut self.unknown, &mut self.auxiliary,
        ] { buffer.clear()?; }
        self.clear_metrics = if profiling {
            [0, 0, (now() - start).max(0) as u64]
        } else { [0; 3] };
        Ok(())
    }
    pub(crate) fn record_consumption(&mut self) -> Result {
        self.consumed_pages = crate::g16_memory::consumed_pages(&mut self.uma, 0, PRIVATE_PAGES)?;
        Ok(())
    }
    pub(crate) fn stamp(&self) -> u32 { self.stamp }
    pub(crate) fn index(&self) -> usize { usize::from(self.stage == Stage::Fragment) }
    pub(crate) fn address(&self) -> u64 { self.commands[self.index()].va() }
    pub(crate) fn wait_for_prior(&mut self, stamp_address: u64, slot: u8, value: u32) -> Result {
        let mut barrier = [0; dependency::SIZE];
        wire::render_dependency(&mut barrier, stamp_address, slot,
            value, self.stamp).map_err(|_| EINVAL)?;
        barrier[dependency::INTERNAL..dependency::INTERNAL+4].copy_from_slice(&1u32.to_le_bytes());
        self.metadata.write(metadata::COMPUTE_DEPENDENCY, &barrier)?;
        self.compute_dependency = true;
        Ok(())
    }
    pub(crate) fn compute_dependency(&self) -> Option<u64> {
        self.compute_dependency.then_some(self.metadata.va() + metadata::COMPUTE_DEPENDENCY as u64)
    }
    pub(crate) fn wait_for_prior_fragment(&mut self, stamp_address: u64,
        slot: u8, value: u32) -> Result<u64> {
        let mut barrier = [0; dependency::SIZE];
        wire::render_dependency(&mut barrier, stamp_address, slot,
            value, self.stamp).map_err(|_| EINVAL)?;
        barrier[dependency::INTERNAL..dependency::INTERNAL+4].copy_from_slice(&1u32.to_le_bytes());
        self.metadata.write(metadata::PRIOR_FRAGMENT, &barrier)?;
        Ok(self.metadata.va() + metadata::PRIOR_FRAGMENT as u64)
    }
    pub(crate) fn init_address(&self) -> Option<u64> {
        self.init_parameters.then_some(self.metadata.va()+metadata::INIT_BUFFER as u64)
    }
    pub(crate) fn fragment_dependency(&self) -> u64 { self.metadata.va()+metadata::TILER_DEPENDENCY as u64 }
    pub(crate) fn fragment_address(&self) -> u64 { self.commands[1].va() }
    pub(crate) fn pool_slot(&self) -> u8 { self.pool_slot }
    pub(crate) fn same_vm(&self, vm: &mmu::Vm) -> bool { self.binding.matches(vm) }
    pub(crate) fn context(&self) -> u32 { self.binding.slot() }
    pub(crate) fn uma_submitted(&self) -> u64 { self.uma_submitted }
    pub(crate) fn uma_completed(&mut self) -> Result<u64> {
        self.uma.read_u64(uma_layout::COMPLETED)
    }
    pub(crate) fn log_progress(&mut self, dev: &driver::AsahiDevice) -> Result {
        dev_info!(dev.as_ref(), "G16G: stalled render stage={:?} context={}\n",
            self.stage, self.context());
        dev_info!(dev.as_ref(), "G16G: render GPU timestamps={:x?}\n", self.timestamps()?);
        for (stage, offset) in [("ta", 0x100), ("fragment", 0x200)] {
            let mut words = [0u64; 4];
            for (index, word) in words.iter_mut().enumerate() {
                *word = self.uma.read_u64(offset+index*8)?;
            }
            dev_info!(dev.as_ref(), "G16G: render UMA metrics {}={:x?}\n", stage, words);
        }
        let mut parameters = self.parameters.lock();
        let crate::g16_parameter::Parameters { manager, ring, .. } = &mut *parameters;
        let [ta_command, fragment_command] = &mut self.commands;
        let [ta_sequence, fragment_sequence] = &mut self.sequences;
        for (name, buffer, start, length) in [
            ("command-ta", ta_command, 0, Stage::Tiling.command_size()),
            ("command-fragment", fragment_command, 0, Stage::Fragment.command_size()),
            ("sequence-ta", ta_sequence, 0, wire::sequence_size(Stage::Tiling)),
            ("sequence-fragment", fragment_sequence, 0, wire::sequence_size(Stage::Fragment)),
            ("parameter-manager", manager, 0, wire::parameter::SIZE),
            ("parameter-ring", ring, 0, 8),
            ("scene", &mut self.metadata, 0, 0x80),
            ("scene-statistics", &mut self.scratch, 0, 0x80),
            ("uma", &mut self.uma, 0, uma_layout::SIZE),
        ] {
            for offset in (start..start+length).step_by(32) {
                let count = (start+length-offset).min(32)/8;
                let mut words = [0u64; 4];
                for (index, word) in words[..count].iter_mut().enumerate() {
                    *word = buffer.read_u64(offset+index*8)?;
                }
                dev_info!(dev.as_ref(), "G16G: render {} +{:#x}={:x?}\n",
                    name, offset, &words[..count]);
            }
        }
        Ok(())
    }
    pub(crate) fn status(&mut self) -> Result<[u64;4]> {
        let i = self.index();
        Ok([self.metadata.read_u32(metadata::LEGACY_FW_STAMPS[i])? as u64,self.metadata.read_u32(metadata::LEGACY_USER_STAMPS[i])? as u64,
            self.commands[i].read_u32(wire::layout(self.stage).command_status)? as u64,
            self.parameters.lock().last_page()? as u64])
    }
    pub(crate) fn set_user_timestamps(&mut self, addresses: [[u64; 2]; 2]) -> Result {
        for (i, pair) in addresses.into_iter().enumerate() {
            let offset = metadata::USER_TIME_PAIRS[i];
            self.metadata.u64(offset, pair[0])?;
            self.metadata.u64(offset + 8, pair[1])?;
            let pointer = if pair == [0, 0] { 0 } else { self.metadata.va() + offset as u64 };
            let start = wire::layout(if i == 0 { Stage::Tiling } else { Stage::Fragment }).timestamp_start;
            self.sequences[i].u64(start + wire::timestamp::USER_PAIR, pointer)?;
            self.sequences[i].u64(start + wire::timestamp::END_DELTA + wire::timestamp::USER_PAIR, pointer)?;
        }
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn set_fragment_timestamp_copies(&mut self, addresses: [u64; 8]) -> Result {
        let mut pairs = [0u64; 8];
        let mut count = 0;
        for address in addresses.into_iter().filter(|address| *address != 0) {
            let offset = metadata::FANOUT_PAIRS + count * 16;
            self.metadata.u64(offset, 0)?;
            self.metadata.u64(offset + 8, address)?;
            pairs[count] = self.metadata.va() + offset as u64;
            count += 1;
        }
        if count == 0 { return Ok(()); }
        let mut sequence = [0u8; 0x800];
        self.sequences[1].read(0, &mut sequence[..wire::sequence_size(Stage::Fragment)])?;
        let size = wire::fragment_timestamp_copies(&mut sequence, &pairs[..count]).map_err(|_| EINVAL)?;
        self.sequences[1].write(0, &sequence[..size])?;
        self.commands[1].u32(wire::layout(Stage::Fragment).command_sequence_size, size.try_into()?)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn omit_internal_timestamps(&mut self, omit: [bool; 2]) -> Result {
        for (i, stage) in [Stage::Tiling, Stage::Fragment].into_iter().enumerate() {
            if !omit[i] { continue; }
            let mut sequence = [0u8; 0x380];
            let total = wire::sequence_size(stage);
            self.sequences[i].read(0, &mut sequence[..total])?;
            let size = crate::g16_compute::omit_timestamps(&mut sequence,
                wire::layout(stage).timestamp_start, total,
                wire::layout(stage).finalize_restart).map_err(|_| EINVAL)?;
            self.sequences[i].write(0, &sequence[..total])?;
            self.commands[i].u32(wire::layout(stage).command_sequence_size, size.try_into()?)?;
        }
        Ok(())
    }
    pub(crate) fn select_fragment_start_abi(&mut self, chip: u32) -> Result {
        if chip != 0x8122 { return Ok(()); }
        let wire = wire::layout(Stage::Fragment);
        if self.commands[1].read_u64(wire.command_sequence)? != self.sequences[1].va()
            || self.commands[1].read_u32(wire.command_sequence_size)? != 0x380 {
            return Err(EINVAL);
        }
        let mut bytes = [0u8; 0x380];
        self.sequences[1].read(0, &mut bytes)?;
        let size = wire::j613_fragment_program(&mut bytes).map_err(|_| EINVAL)?;
        self.sequences[1].write(0, &bytes)?;
        self.commands[1].u32(wire.command_sequence_size, size.try_into()?)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn cache_programs(&mut self, config: &mut crate::g16_config::Config,
        dev: &driver::AsahiDevice, vm: &mmu::Vm) -> Result {
        if crate::debug::debug_enabled(crate::debug::DebugFlags::M3PassTiming) {
            return Err(EINVAL);
        }
        for (i, stage) in [Stage::Tiling, Stage::Fragment].into_iter().enumerate() {
            let length = self.commands[i].read_u32(wire::layout(stage).command_sequence_size)? as usize;
            if length > 0x800 { return Err(EINVAL); }
            let mut bytes = [0u8; 0x800];
            self.sequences[i].read(0, &mut bytes[..length])?;
            let address = config.cache_program(dev, vm, &mut bytes[..length], stage)?;
            self.commands[i].u64(wire::layout(stage).command_sequence, address)?;
        }
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn timestamps(&mut self) -> Result<[[u64; 2]; 2]> {
        Ok([[self.metadata.read_u64(metadata::TIME_PAIRS[0])?, self.metadata.read_u64(metadata::TIME_PAIRS[0]+8)?],
            [self.metadata.read_u64(metadata::TIME_PAIRS[1])?, self.metadata.read_u64(metadata::TIME_PAIRS[1]+8)?]])
    }
}
