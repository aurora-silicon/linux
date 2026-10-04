// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! GEM/GPUVM ownership for a serialized M4 render pass. There are no borrowed
//! lab mappings. A hardware slot retains its last owner until replacement.
use kernel::prelude::*;
use crate::{driver, mmu, g16_memory::{self, Buffer}, g16_render::{self, Geometry, Register, Stage, Utile},
    g16_render_state::{self as state, State, Program, DepthStencil}, g16_render_command as wire,
    g17_uapi::UapiRenderCommand};

const PRIVATE_BYTES: usize = 32 * 1024 * 1024;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 4096;
// The gUPM freelist needs headroom beyond the initially populated pages.
// Equal capacity faulted on a write at freelist+0x10040 with 8192 pages.
// Derive the next ring size instead of silently overflowing a full ring.
const PRIVATE_QWORDS: usize = (PRIVATE_PAGES + 1).next_power_of_two();
const PB_BLOCK_BYTES: usize = 4 * 32768;
pub(crate) const SLOTS: [u8; 2] = [0x70, 0x71];

pub(crate) struct Render {
    _command_views: KVec<mmu::KernelMapping>,
    commands: [Buffer; 2],
    sequences: [Buffer; 2],
    metadata: Buffer,
    manager: Buffer,
    pages: Buffer,
    blocks: Buffer,
    block_ring: Buffer,
    pb_memory: Buffer,
    scratch: Buffer,
    scene_list: Buffer,
    tilemap: Buffer,
    tpc: Buffer,
    unknown: Buffer,
    preemption: [Buffer; 3],
    auxiliary: Buffer,
    uma: Buffer,
    private_memory: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    binding: mmu::VmBind,
    pub(crate) stage: Stage,
    stamp: u32,
    layout: [u32; 4],
    consumed_pages: usize,
}
impl Render {
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("render-parameter-manager", &mut self.manager, 0xc0)?;
        dump.buffer("render-parameter-ring", &mut self.block_ring, 8)?;
        dump.buffer("render-scene-metadata", &mut self.metadata, 0x4000)?;
        dump.buffer("render-uma-descriptor", &mut self.uma, 0x300)?;
        dump.buffer("render-pool-state", &mut self.pool_state, PRIVATE_QWORDS * 8)?;
        for (i, stage) in [Stage::Tiling, Stage::Fragment].into_iter().enumerate() {
            dump.buffer(["render-ta-command", "render-fragment-command"][i],
                &mut self.commands[i], stage.command_size())?;
            dump.buffer(["render-ta-sequence", "render-fragment-sequence"][i],
                &mut self.sequences[i], wire::sequence_size(stage))?;
        }
        Ok(())
    }

    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, vm: &mmu::Vm,
        r: UapiRenderCommand, usc: u64, generation: u8, stamp: u32, counter: u64,
        queues: [u64; 2], notifications: [u64; 2], stats: [u64; 2],
        reusable: Option<KBox<Self>>) -> Result<KBox<Self>> {
        let utile = |n| match n {16=>Ok(Utile::Pixels16),32=>Ok(Utile::Pixels32),_=>Err(EINVAL)};
        let mut geometry = Geometry::new_layered(r.width as u32,r.height as u32,
            utile(r.utile_width)?,utile(r.utile_height)?,1,r.layers).map_err(|_| EINVAL)?;
        geometry.set_samples(r.samples).map_err(|_| EINVAL)?;
        // Initial allocation follows pass geometry, rounded to PB blocks. The
        // firmware can partially store/reload a pass. This is not a UAPI limit.
        let pb_bytes = (4*1024*1024usize).max(usize::try_from(geometry.tilemap_bytes)? * 128)
            .checked_add(PB_BLOCK_BYTES-1).ok_or(EOVERFLOW)? & !(PB_BLOCK_BYTES-1);
        let page_count = pb_bytes / 32768;
        let block_count = pb_bytes / PB_BLOCK_BYTES;
        let block_capacity = (block_count+1).next_power_of_two();
        let layout = [r.width as u32, r.height as u32,
            r.utile_width as u32, r.utile_height as u32];
        // Only an owner displaced by a completely retired replacement may
        // be recycled. Never borrow the firmware's most recently used owner,
        // and never reuse private mappings across distinct client GPUVMs.
        let reusable = reusable.filter(|job| job.binding.matches(vm));
        let reused = reusable.is_some();
        let mut job = if let Some(mut job) = reusable {
            // A render size change does not invalidate the command/context
            // storage or the USC pool. Grow only geometry-dependent backing;
            // each new pass still rebuilds its logical sizes and descriptors.
            // This owner is already displaced and retired, so replacing a
            // too-small buffer obeys the same lifetime rule as dropping it.
            let gpu = |size| Buffer::new_gpu(dev,uat.kernel_vm(),vm,size);
            if job.pages.size() < page_count * 4 {
                job.pages = gpu(page_count * 4)?;
            }
            if job.blocks.size() < block_capacity * 8 {
                job.blocks = Buffer::new(dev,uat.kernel_vm(),block_capacity * 8)?;
            }
            if job.pb_memory.size() < pb_bytes {
                job.pb_memory = Buffer::new_gpu_aligned(dev,uat.kernel_vm(),vm,
                    pb_bytes,PB_BLOCK_BYTES as u64)?;
            }
            if job.tilemap.size() < geometry.tilemap_bytes.try_into()? {
                job.tilemap = gpu(geometry.tilemap_bytes.try_into()?)?;
            }
            if job.tpc.size() < geometry.tpc_bytes.try_into()? {
                job.tpc = gpu(geometry.tpc_bytes.try_into()?)?;
            }
            job.clear()?;
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
            KBox::new(Self {
                _command_views:views, commands, sequences:[fw(0x400)?,fw(0x400)?],
                metadata:fw(0x4000)?,manager:fw(0xc0)?,pages:gpu(page_count*4)?,
                blocks:fw(block_capacity*8)?,block_ring:fw(8)?,
                pb_memory:Buffer::new_gpu_aligned(dev,uat.kernel_vm(),vm,pb_bytes,PB_BLOCK_BYTES as u64)?,
                scratch:gpu(0x4000)?,scene_list:gpu(0x4000)?,tilemap:gpu(geometry.tilemap_bytes.try_into()?)?,
                tpc:gpu(geometry.tpc_bytes.try_into()?)?,unknown:gpu(0x4000)?,
                // Sizes inherited from the qualified M4 context-save experiment.
                preemption:[gpu(0x10000)?,gpu(0x10000)?,gpu(0x4000)?],auxiliary:gpu(0x10000)?,
                uma:gpu(0x4000)?,private_memory:gpu(PRIVATE_BYTES)?,
                pool_state:gpu(PRIVATE_QWORDS*8)?,page_list:gpu(0x4000)?,
                binding:uat.bind(vm)?,stage:Stage::Tiling,stamp,layout,
                consumed_pages:PRIVATE_PAGES,
            }, GFP_KERNEL)?
        };
        let base = job.private_memory.gpu_va()?;
        {
            let owner: &mut Self = &mut *job;
            g16_memory::write_freelist(&mut owner.pool_state, &mut owner.page_list, base, PRIVATE_PAGES)?;
        }
        let mut uma = [0;0xa0];
        crate::g16_compute::encode_uma(&mut uma,job.pool_state.gpu_va()?,PRIVATE_QWORDS as u32,
            PRIVATE_PAGES as u32,job.page_list.gpu_va()?,job.metadata.va()+0x500,
            crate::g16_compute::UmaEngine::Render).map_err(|_| EINVAL)?;
        job.uma.write(0,&uma)?;
        job.metadata.u64(0x500, 2)?;
        let pb_base = state::compact(job.pb_memory.gpu_va()?).map_err(|_| EINVAL)?;
        {
            // One staged copy per list instead of a bounds-checked WC store
            // per entry; the parameter page list has thousands of entries.
            let mut pages = KVec::new();
            for i in 0..page_count {
                let page: u32 = ((pb_base+i as u64*32768)>>15).try_into()?;
                pages.extend_from_slice(&page.to_le_bytes(), GFP_KERNEL)?;
            }
            job.pages.write(0, &pages)?;
            let mut blocks = KVec::new();
            for i in 0..block_count {
                let block: u32 = ((pb_base+i as u64*PB_BLOCK_BYTES as u64)>>15).try_into()?;
                blocks.extend_from_slice(&block.to_le_bytes(), GFP_KERNEL)?;
                blocks.extend_from_slice(&[0; 4], GFP_KERNEL)?;
            }
            job.blocks.write(0, &blocks)?;
        }
        job.block_ring.u32(0,block_count.try_into()?)?;
        job.block_ring.u32(4,block_count.try_into()?)?;
        let mut manager = [0;0xc0];
        wire::parameter_manager(&mut manager,wire::ParameterManager {
            pages_fw:job.pages.va(),pages_gpu:job.pages.gpu_va()?,blocks:job.blocks.va(),ring:job.block_ring.va(),
            counter:job.metadata.va()+0x508,discard:0,list_bytes:(page_count*4).try_into()?,pages:page_count.try_into()?,
            block_capacity:block_capacity.try_into()?,write:block_count.try_into()?,read:0,
            min_pages:page_count.try_into()?,max_pages:page_count.try_into()?,id:0 }).map_err(|_| EINVAL)?;
        job.manager.write(0,&manager)?;
        let mut init = [0;0x20];
        wire::init_parameter_buffer(&mut init,job.binding.slot(),0,0,0,job.manager.va(),stamp).map_err(|_| EINVAL)?;
        job.metadata.write(0x300,&init)?;
        let mut dependency = [0;0x40];
        wire::render_dependency(&mut dependency,job.metadata.va()+0x408,
            SLOTS[0],stamp,stamp).map_err(|_| EINVAL)?;
        job.metadata.write(0x340,&dependency)?;
        // PB scene scratch has explicit GPU and firmware aliases. Statistics
        // are a distinct retained record; firmware dereferences them on retire.
        let scratch_gpu = job.scratch.gpu_va()?;
        let scratch_fw = job.scratch.va();
        let statistics = job.metadata.va()+0x200;
        job.metadata.u64(0,scratch_gpu)?;
        job.metadata.u64(8,scratch_fw)?;
        let scene_list = state::compact(job.scene_list.gpu_va()?).map_err(|_| EINVAL)?;
        job.metadata.u64(0x28,scene_list)?;
        job.metadata.u64(0x40,statistics)?;
        let mask = if r.flags & 16 != 0 {u64::MAX} else {u32::MAX as u64};
        let program = |p:crate::g17_uapi::UapiProgram| Program {
            address:usc+u64::from(p.usc & !63), resources:p.resource_spec & mask };
        let state = State { geometry,vdm:r.vdm_base,tilemap:job.tilemap.gpu_va()?,tpc:job.tpc.gpu_va()?,
            unknown:job.unknown.gpu_va()?,preemption:[job.preemption[0].gpu_va()?,job.preemption[1].gpu_va()?,job.preemption[2].gpu_va()?],
            scratch:job.scratch.gpu_va()?,scene_list:job.scene_list.gpu_va()?,auxiliary:job.auxiliary.gpu_va()?,multisample:r.multisample_control,ppp:r.ppp_control,
            // UAPI values are already validated. Merge limits are float bits,
            // not geometry counts; color storage is measured in 2 KiB blocks.
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
            let uuid = 0x16000000 | (u32::from(generation)<<8) | i as u32;
            wire::encode(&mut work[..],&mut sequence,stage,wire::Args {
                command:job.commands[i].va(),sku:job.sequences[i].va(),queue:queues[i],
                stats:stats[i],
                pb_slot:job.metadata.va(),manager:job.manager.va(),pb_aux:job.metadata.va()+0x100,
                notifier:notifications[i],shared_stamp:job.metadata.va()+0x400+i as u64*16,
                stamp_address:job.metadata.va()+0x408+i as u64*16,uma:job.uma.va(),
                uma_aux:job.uma.gpu_va()?+0x100+i as u64*0x100,
                timestamp_storage:job.metadata.va()+0x600+i as u64*0x20,
                context:job.binding.slot(),generation,counter:counter + u64::from(i == 0),uuid,stamp:stamp,
                event_slot:SLOTS[i] as u32,fragment_slot:SLOTS[1] as u32,
                fragment_stamp:stamp,pb_id:0,pass_id:0 }).map_err(|_| EINVAL)?;
            let (mut count,mode) = state::registers(&mut registers[..],stage,&state,uuid).map_err(|_| EINVAL)?;
            // Common Asahi USC execution window selectors (TA/ISP), retaining
            // the real queue base instead of relying on the boot register.
            registers[count] = Register { offset:if i==0 {0x10060} else {0x10068},flagged:true,value:usc };
            count+=1;
            g16_render::write_registers(&mut work[..],stage,job.commands[i].gpu_va()?,&registers[..count]).map_err(|_| EINVAL)?;
            state::mirrors(&mut work[..],stage,&state,job.tpc.va(),job.uma.va(),job.uma.va()+0x100+i as u64*0x100,mode).map_err(|_| EINVAL)?;
            job.commands[i].write(0,&work[..stage.command_size()])?;
            job.sequences[i].write(0,&sequence[..wire::sequence_size(stage)])?;
        }
        g16_memory::publish();
        dev_info!(dev.as_ref(), "G16G: render USC freelist={:#x} capacity={} populated={}\n",
            job.pool_state.gpu_va()?,PRIVATE_QWORDS,PRIVATE_PAGES);
        dev_info!(dev.as_ref(),"G16G: owned render {}x{} ASID={} PB={} pages tilemap={:#x} TPC={:#x} VDM={:#x} reused={}\n",
            r.width,r.height,job.binding.slot(),page_count,job.tilemap.gpu_va()?,job.tpc.gpu_va()?,r.vdm_base,reused);
        Ok(job)
    }
    pub(crate) fn set_attachments(&mut self, attachments: &[crate::g16_attachments::Attachments; 2]) -> Result {
        // StartVertex+88/count+188; StartFragment+a8/count+1a8.
        // The following UMA bindings and WFI records stay intact.
        for (i, (start, finish_flag)) in [(0x88, 0x2ec), (0xa8, 0x32c)].into_iter().enumerate() {
            self.sequences[i].write(start, &attachments[i].0)?;
            self.sequences[i].write(finish_flag, &[u8::from(attachments[i].count() != 0)])?;
        }
        g16_memory::publish();
        Ok(())
    }
    /// Reset command state while retaining GEM backing, both GPU aliases and
    /// the ASID lease. In particular, clear old completion
    /// stamps, preemption records and parameter/USC allocator state before
    /// their new event values and freelists are encoded.
    /// Parameter pages are GPU-written payload. Reset their allocator and
    /// scene metadata, retaining payload contents after retirement in this VM.
    fn clear(&mut self) -> Result {
        for buffer in &mut self.commands { buffer.clear()?; }
        for buffer in &mut self.sequences { buffer.clear()?; }
        for buffer in &mut self.preemption { buffer.clear()?; }
        // The 32 MiB USC private pool holds firmware/GPU allocator state in
        // the pages it handed out. Only those pages, recorded from the
        // retired descriptor, can be stale; clearing the whole pool through
        // the WC alias cost more than the GPU work of a typical pass.
        let bytes = self.consumed_pages.min(PRIVATE_PAGES) * 4096;
        self.private_memory.clear_range(0, bytes)?;
        for buffer in [
            &mut self.metadata, &mut self.manager, &mut self.pages,
            &mut self.blocks, &mut self.block_ring,
            &mut self.scratch, &mut self.scene_list, &mut self.tilemap, &mut self.tpc,
            &mut self.unknown, &mut self.auxiliary, &mut self.uma,
            &mut self.pool_state, &mut self.page_list,
        ] { buffer.clear()?; }
        Ok(())
    }
    /// Record the retired pass's private-page consumption for recycling.
    pub(crate) fn record_consumption(&mut self) -> Result {
        self.consumed_pages = crate::g16_memory::consumed_pages(&mut self.uma, 0, PRIVATE_PAGES)?;
        Ok(())
    }
    pub(crate) fn stamp(&self) -> u32 { self.stamp }
    pub(crate) fn index(&self) -> usize { usize::from(self.stage == Stage::Fragment) }
    pub(crate) fn address(&self) -> u64 { self.commands[self.index()].va() }
    pub(crate) fn init_address(&self) -> u64 { self.metadata.va()+0x300 }
    pub(crate) fn fragment_dependency(&self) -> u64 { self.metadata.va()+0x340 }
    pub(crate) fn fragment_address(&self) -> u64 { self.commands[1].va() }
    pub(crate) fn context(&self) -> u32 { self.binding.slot() }
    pub(crate) fn uma_completed(&mut self) -> Result<u64> {
        self.uma.read_u64(0x54)
    }
    /// Snapshot retained firmware/GPU state without acknowledging faults or
    /// changing the in-flight pass. Keep diagnostics off the submission path.
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
        let [ta_command, fragment_command] = &mut self.commands;
        let [ta_sequence, fragment_sequence] = &mut self.sequences;
        for (name, buffer, start, length) in [
            ("command-ta", ta_command, 0, Stage::Tiling.command_size()),
            ("command-fragment", fragment_command, 0, Stage::Fragment.command_size()),
            ("sequence-ta", ta_sequence, 0, wire::sequence_size(Stage::Tiling)),
            ("sequence-fragment", fragment_sequence, 0, wire::sequence_size(Stage::Fragment)),
            ("parameter-manager", &mut self.manager, 0, 0xc0),
            ("parameter-ring", &mut self.block_ring, 0, 8),
            ("scene", &mut self.metadata, 0, 0x80),
            ("scene-statistics", &mut self.scratch, 0, 0x80),
            ("uma", &mut self.uma, 0, 0xa0),
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
        Ok([self.metadata.read_u32(0x400+i*16)? as u64,self.metadata.read_u32(0x408+i*16)? as u64,
            self.commands[i].read_u32(if i==0 {0x918} else {0xc60})? as u64,
            self.manager.read_u32(0x54)? as u64])
    }
    pub(crate) fn set_user_timestamps(&mut self, addresses: [[u64; 2]; 2]) -> Result {
        for (i, pair) in addresses.into_iter().enumerate() {
            // Timestamp+24 is the optional user pointer pair, as in compute.
            // Keep the internal timestamps separate for retirement diagnostics.
            // The packet retains each validated TimestampBuffer mapping until
            // both engines retire, including failed jobs.
            let offset = 0x610 + i * 0x20;
            self.metadata.u64(offset, pair[0])?;
            self.metadata.u64(offset + 8, pair[1])?;
            let pointer = if pair == [0, 0] { 0 } else { self.metadata.va() + offset as u64 };
            let start = if i == 0 { 0x1cc } else { 0x1ec };
            self.sequences[i].u64(start + 0x24, pointer)?;
            self.sequences[i].u64(start + 0x50 + 0x24, pointer)?;
        }
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn timestamps(&mut self) -> Result<[[u64; 2]; 2]> {
        Ok([[self.metadata.read_u64(0x600)?, self.metadata.read_u64(0x608)?],
            [self.metadata.read_u64(0x620)?, self.metadata.read_u64(0x628)?]])
    }
}
