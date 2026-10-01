// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounded M3 compute and graphics submissions through the common DRM scheduler/syncobjs.
use kernel::{c_str,dma_fence::*,drm::sched,prelude::*,sync::{Arc,Mutex},new_mutex,xarray};
use crate::{file,g17_uapi,mmu,queue,m3_drm::Shared};
use core::sync::atomic::{AtomicBool, Ordering};
#[derive(Default)]
pub(crate) struct Completion;
#[vtable]
impl FenceOps for Completion {
    fn get_driver_name<'a>(self:&'a FenceObject<Self>)->&'a CStr {c_str!("asahi")}
    fn get_timeline_name<'a>(self:&'a FenceObject<Self>)->&'a CStr {c_str!("m3-native")}
}
pub(crate) struct Destination { mapping: Arc<mmu::KernelMapping>, offset: usize }
impl Destination {
    fn resolve(objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamp: g17_uapi::UapiTimestamp) -> Result<Option<Self>> {
        if timestamp.handle == 0 { return Ok(None); }
        let guard = objects.lock();
        let object = guard.get(timestamp.handle.try_into()?).ok_or(ENOENT)?;
        match &*object {
            file::Object::TimestampBuffer(mapping) => {
                let offset = timestamp.offset as usize;
                if offset & 7 != 0 || offset.checked_add(8).ok_or(EOVERFLOW)? > mapping.size() {
                    return Err(EINVAL);
                }
                Ok(Some(Self { mapping: mapping.clone(), offset }))
            }
        }
    }
    fn firmware_address(&self) -> u64 { self.mapping.iova() + self.offset as u64 }
}

#[derive(Copy,Clone)]
pub(crate) enum Command {
    Compute(crate::g16_compute::Control),
    Render{command:g17_uapi::UapiRenderCommand,usc:u64},
}
pub(crate) struct Packet {
    pub(crate) vm:mmu::Vm,
    pub(crate) commands:KVec<Command>,
    timestamps:KVec<[Option<Destination>;4]>,
    pub(crate) attachments:KVec<crate::g16_attachments::Attachments>,
    pub(crate) wide_visibility:KVec<bool>,
    completion:UserFence<Completion>,
    finish_claimed: AtomicBool,
    vm_job:Pin<KBox<Mutex<Option<mmu::T8140VmJobGuard>>>>,
}
impl Packet {
    pub(crate) fn timestamp_addresses(&self,index:usize)->[u64;2] {
        core::array::from_fn(|i|self.timestamps[index][i].as_ref().map_or(0,Destination::firmware_address))
    }
    pub(crate) fn render_timestamp_addresses(&self,index:usize)->[[u64;2];2] {
        core::array::from_fn(|stage| core::array::from_fn(|i|
            self.timestamps[index][stage*2+i].as_ref().map_or(0,Destination::firmware_address)))
    }
    fn finish(&self,result:Result) {
        // Retirement, cancellation and a queued scheduler timeout can race.
        // Claim error/signal ownership once; this flag is NOT proof that the
        // fence is signaled or that hardware DMA has stopped.
        if self.finish_claimed.swap(true, Ordering::Relaxed) { return; }
        if let Err(e)=result {self.vm.status().record(e.to_errno());self.completion.set_error(e);}
        // A scheduler job/fence and the runtime's last packet can outlive
        // hardware retirement. They must not keep the VM's active-job count
        // nonzero: GEM closes otherwise accumulate across completed frames.
        // On failure the runtime quarantines this guard with the packet until
        // firmware has provably stopped; signalling an error is not retirement.
        if result.is_ok() {
            let retired = self.vm_job.lock().take();
            core::mem::drop(retired);
        }
        crate::g16_memory::publish();self.completion.signal();
    }
}
pub(crate) struct Job {shared:Shared,packet:Arc<Packet>}
impl sched::JobImpl for Job {
    fn run(job:&mut sched::Job<Self>)->Result<Option<Fence>> {
        let result=crate::m3_runtime::Runtime::execute(&job.shared, job.packet.clone());
        if let Err(e)=result {pr_err!("M3 scheduler: execution failed {:?}\n",e);}
        job.packet.finish(result);
        Ok(Some(Fence::from_fence(&job.packet.completion)))
    }
    fn timed_out(job:&mut sched::Job<Self>)->sched::Status {
        // Retain the publishing branch's explicit diagnostic override.
        if !crate::m3_params::timeout_nohang() {
            if let Some(r)=Option::as_mut(&mut *job.shared.lock()) {r.health().mark_failed();}
            job.packet.finish(Err(ETIMEDOUT));return sched::Status::NoDevice;
        }
        let guard=job.shared.lock();
        let Some(runtime)=guard.as_ref() else {
            job.packet.finish(Err(ENODEV));
            return sched::Status::NoDevice;
        };
        // The scheduler's deadline covers the whole packet, not one batch.
        // execute() owns each batch's bounded hardware-completion checks;
        // waiting for its mutex can itself cross the scheduler deadline.
        // Reinsert a healthy packet instead of turning a late timeout into
        // a global device loss or corrupting an already signaled fence.
        if runtime.health().healthy() { return sched::Status::NoHang; }
        job.packet.finish(Err(EIO));
        sched::Status::NoDevice
    }
    fn cancel(job:&mut sched::Job<Self>) {job.packet.finish(Err(ECANCELED));}
}
struct AddressSpace<'a>(&'a mmu::Vm);
impl g17_uapi::GpuAddressSpace for AddressSpace<'_> {
    fn covers(&self,address:u64,size:u64,access:g17_uapi::GpuAccess)->bool {
        let Some(end)=address.checked_add(size) else {return false;};
        let (r,w)=match access {g17_uapi::GpuAccess::Read=>(true,false),
            g17_uapi::GpuAccess::Write=>(false,true),g17_uapi::GpuAccess::ReadWrite=>(true,true)};
        address>=0x4000 && end<=(1u64<<42)-0x8000
            && !self.0.driver_range_overlaps(address..end) && self.0.covers_range(address,size,r,w)
    }
}
pub(crate) struct Queue {
    entity:sched::Entity<Job>,_scheduler:Arc<sched::Scheduler<Job>>,
    shared:Shared,vm:mmu::Vm,usc:u64,fences:FenceContexts,
}
impl Queue {
    pub(crate) fn new(shared:Shared,scheduler:Arc<sched::Scheduler<Job>>,vm:mmu::Vm,priority:u32,usc:u64)->Result<Self> {
        g17_uapi::QueueUscWindow{base:usc,user_start:0x4000,user_end:(1u64<<42)-0x8000}.validate().map_err(|_|EINVAL)?;
        // file.rs passes REALTIME - UAPI priority: 3 is UAPI LOW, 0 is UAPI REALTIME.
        let priority=match priority {3=>sched::Priority::Low,2=>sched::Priority::Normal,
            1=>sched::Priority::High,0=>sched::Priority::Kernel,_=>return Err(EINVAL)};
        Ok(Self {entity:sched::Entity::new(&scheduler,priority)?,_scheduler:scheduler,
            shared,vm,usc,fences:FenceContexts::new(1,c_str!("asahi_m3_queue"),kernel::static_lock_class!())?})
    }
}
impl queue::Queue for Queue {
    fn submit(&mut self,_id:u64,mut syncs:KVec<file::SyncItem>,in_sync_count:usize,
        raw:&[u8],objects:Pin<&xarray::XArray<KBox<file::Object>>>)->Result {
        let vm_job=self.vm.retain_t8140_job()?;
        let mut parser=g17_uapi::UapiCommandParser::new(raw);let mut commands=KVec::new();let mut timestamps=KVec::new();let mut flushes=KVec::new();let mut visibility=KVec::new();
        while let Some(command)=parser.next_hardware().map_err(|_|EINVAL)? {
            if let g17_uapi::ParsedHardwareCommand::Render{payload,vertex_attachments,fragment_attachments,..}=command {
                if let Err(e)=validate_render(payload,self.usc,&AddressSpace(&self.vm)) {
                    pr_err!("M3 render rejected {:?}: flags={:#x} size={}x{} layers={} samples={} utile={}x{} sample_size={} depth={:#x} stencil={:#x} zls={:#x} samplers={} heap={:#x}\n",e,payload.flags,payload.width,payload.height,payload.layers,payload.samples,payload.utile_width,payload.utile_height,payload.sample_size,payload.depth.base,payload.stencil.base,payload.zls_control,payload.sampler_count,payload.sampler_heap);
                    return Err(e);
                }
                g17_uapi::validate_attachments(&AddressSpace(&self.vm),&vertex_attachments).map_err(|_|EINVAL)?;
                g17_uapi::validate_attachments(&AddressSpace(&self.vm),&fragment_attachments).map_err(|_|EINVAL)?;
                commands.push(Command::Render{command:payload,usc:self.usc},GFP_KERNEL)?;
                timestamps.push([Destination::resolve(objects,payload.vertex_timestamps.start)?,
                    Destination::resolve(objects,payload.vertex_timestamps.end)?,
                    Destination::resolve(objects,payload.fragment_timestamps.start)?,
                    Destination::resolve(objects,payload.fragment_timestamps.end)?],GFP_KERNEL)?;
                flushes.push(crate::g16_attachments::Attachments::EMPTY,GFP_KERNEL)?;
                visibility.push(false,GFP_KERNEL)?;
                continue;
            }
            let g17_uapi::ParsedHardwareCommand::Compute{mut payload,attachments,..}=command else {return Err(ENOTSUPP);};
            // Other engines and helpers need their own M3 firmware translation.
            let wide=kernel::uapi::drm_asahi_compute_flags_DRM_ASAHI_COMPUTE_WIDE_VISIBILITY as u32;
            if payload.flags & !wide !=0 || payload.sampler_count!=0 || payload.sampler_heap!=0 {
                pr_err!("M3 compute rejected: flags={:#x} samplers={} heap={:#x} attachments={} timestamps={}/{}\n",payload.flags,payload.sampler_count,payload.sampler_heap,attachments.as_slice().len(),payload.timestamps.start.handle,payload.timestamps.end.handle);
                return Err(ENOTSUPP);
            }
            visibility.push(payload.flags & wide !=0,GFP_KERNEL)?;
            // The common translator validates the original command/attachment
            // addresses; this M3 scheduling hint changes none of them.
            payload.flags &= !wide;
            flushes.push(encode_attachments(&self.vm,&attachments)?,GFP_KERNEL)?;
            let c=g17_uapi::translate_compute_command(payload,attachments,
                g17_uapi::QueueUscWindow{base:self.usc,user_start:0x4000,user_end:(1u64<<42)-0x8000},
                &AddressSpace(&self.vm)).map_err(|e| {
                    pr_err!("M3 compute translation rejected {:?}: stream={:#x}..{:#x} helper={:?}\n",e,payload.control_stream_base,payload.control_stream_end,payload.helper);
                    EINVAL
                })?;
            crate::m3_client::check_compute_launches(&self.vm,&AddressSpace(&self.vm),c.control_stream_base,c.control_stream_end)?;
            timestamps.push([Destination::resolve(objects,payload.timestamps.start)?,
                Destination::resolve(objects,payload.timestamps.end)?,None,None],GFP_KERNEL)?;
            commands.push(Command::Compute(crate::g16_compute::Control{base:c.control_stream_base,end:c.control_stream_end,usc_base:self.usc}),GFP_KERNEL)?;
        }
        parser.finish().map_err(|_|EINVAL)?;
        let packet=Arc::new(Packet{vm:self.vm.clone(),commands,timestamps,attachments:flushes,wide_visibility:visibility,
            completion:self.fences.new_fence(0,Completion)?.into(),
            finish_claimed:AtomicBool::new(false),
            vm_job:KBox::pin_init(new_mutex!(Some(vm_job)),GFP_KERNEL)?},GFP_KERNEL)?;
        let mut job=self.entity.new_job(1,Job{shared:self.shared.clone(),packet})?;
        for sync in syncs.drain(0..in_sync_count) {if let Some(f)=sync.fence {job.add_dependency(f)?;}}
        let mut job=job.arm();let finished=job.fences().finished();job.push();
        for mut sync in syncs {if let Some(chain)=sync.chain_fence.take() {
            sync.syncobj.add_point(chain,&finished,sync.timeline_value);
        } else {sync.syncobj.replace_fence(Some(&finished));}}
        Ok(())
    }
}

fn encode_attachments(vm: &mmu::Vm, list: &g17_uapi::UapiAttachmentList)
    -> Result<crate::g16_attachments::Attachments> {
    use crate::g16_attachments::{Attachments, CAPACITY};
    use g17_uapi::{GpuAddressSpace, GpuAccess};
    if list.as_slice().len()>CAPACITY {return Err(E2BIG);}
    let mut ranges = [(0, 0); CAPACITY];
    for (range, entry) in ranges.iter_mut().zip(list.as_slice()) {
        *range = Attachments::range(entry.address, entry.size).map_err(|_| EINVAL)?;
        if !AddressSpace(vm).covers(range.0, range.1, GpuAccess::Write) {
            return Err(EINVAL);
        }
    }
    let mut out=Attachments::new(&ranges[..list.as_slice().len()]).map_err(|_| EINVAL)?;
    // G15S RTKit-2419 uses the legacy 0x17 cache attribute; retain the
    // M4 range normalization and ownership checks, not its 0x18 attribute.
    for i in 0..list.as_slice().len() {out.0[i*16+12..i*16+14].copy_from_slice(&0x17u16.to_le_bytes());}
    Ok(out)
}

fn validate_render(r: g17_uapi::UapiRenderCommand, usc: u64, space: &AddressSpace<'_>) -> Result {
    use g17_uapi::{GpuAddressSpace, GpuAccess};
    if r.sample_size % 8 != 0 {return Err(ENOTSUPP);}
    let empty = g17_uapi::UapiHelperProgram { binary:0, config:0, data:0 };
    let stage_scope=if *crate::module_parameters::m3_early_tiling.value()!=0 {1<<5} else {0};
    if r.flags & !(2|16|(1<<18)|stage_scope) != 0 || !matches!(r.samples, 1 | 2 | 4)
        || r.sampler_count != 0 || r.sampler_heap != 0
        || r.vertex_helper != empty || r.fragment_helper != empty
        || r.depth.compression_base != 0 || r.stencil.compression_base != 0
        || r.depth.compression_stride != 0 || r.stencil.compression_stride != 0
        || r.ppp_control & !0x203 != 0 { return Err(ENOTSUPP); }
    // sample_size describes color tilebuffer storage. Depth/stencil-only
    // passes legitimately require zero bytes of color storage.
    if r.layers == 0 || r.layers > 2048 || r.width == 0 || r.height == 0 || r.width > 16384 || r.height > 16384
        || !matches!(r.utile_width,16|32) || !matches!(r.utile_height,16|32)
        || u32::from(r.sample_size)*u32::from(r.utile_width)*u32::from(r.utile_height)*u32::from(r.samples)>32768 {
        return Err(EINVAL);
    }
    crate::g16_render_state::compact(r.vdm_base).map_err(|_| EINVAL)?;
    if r.vdm_base & 3 != 0 || !space.covers(r.vdm_base,4,GpuAccess::Read)
        || r.scissor_base == 0 || r.scissor_base & 7 != 0 || !space.covers(r.scissor_base,8,GpuAccess::Read) {
        return Err(EINVAL);
    }
    for (p,access) in [(r.depth_bias_base,GpuAccess::Read),(r.occlusion_query_base,GpuAccess::Write)] {
        if p != 0 && (p & 7 != 0 || !space.covers(p,8,access)) { return Err(EINVAL); }
    }
    for zls in [r.depth,r.stencil] { g17_uapi::validate_zls(space,zls,r.layers).map_err(|_| EINVAL)?; }
    let window = g17_uapi::QueueUscWindow { base:usc,user_start:0x4000,user_end:crate::g16_drm::USER_TOP };
    for p in [r.background,r.end_of_tile,r.partial_background,r.partial_end_of_tile] {
        window.full_program(space,p.usc).map_err(|_| EINVAL)?;
        if !space.covers(usc+u64::from(p.usc & !63),64,GpuAccess::Read) { return Err(EINVAL); }
    }
    Ok(())
}
