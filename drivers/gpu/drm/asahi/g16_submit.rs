// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! DRM scheduling and syncobj ownership for 25G83 submissions.
use kernel::{c_str, dma_fence::*, drm::sched, new_mutex, prelude::*,
    sync::{Arc, Mutex}, xarray};
use crate::{driver, file, g16_drm::Shared, g16_uapi, mmu, queue};
use core::sync::atomic::{AtomicBool, AtomicI32, Ordering};

#[derive(Default)]
pub(crate) struct Completion;
#[vtable]
impl FenceOps for Completion {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("asahi") }
    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("m4-gpu") }
}

pub(crate) struct Destination { mapping: Arc<mmu::KernelMapping>, offset: usize }
impl Destination {
    fn resolve(objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamp: g16_uapi::UapiTimestamp) -> Result<Option<Self>> {
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

#[derive(Clone, Copy)]
pub(crate) enum Payload {
    Compute(crate::g16_compute::Control),
    Render { command: g16_uapi::UapiRenderCommand, usc: u64 },
}
pub(crate) struct Command {
    pub(crate) payload: Payload,
    pub(crate) attachments: [crate::g16_attachments::Attachments; 2],
    timestamps: [Option<Destination>; 4],
    end_copies: [Option<Destination>; 8],
}
impl Command {
    pub(crate) fn end_copy_addresses(&self) -> [u64; 8] {
        core::array::from_fn(|i| self.end_copies[i].as_ref()
            .map_or(0, Destination::firmware_address))
    }
    pub(crate) fn timestamp_addresses(&self) -> [u64; 2] {
        core::array::from_fn(|i| self.timestamps[i].as_ref().map_or(0, Destination::firmware_address))
    }
    pub(crate) fn render_timestamp_addresses(&self) -> [[u64; 2]; 2] {
        core::array::from_fn(|stage| core::array::from_fn(|i|
            self.timestamps[stage * 2 + i].as_ref().map_or(0, Destination::firmware_address)))
    }
}

pub(crate) type RenderCache = Arc<Mutex<Option<crate::g16_owner_cache::OwnerCache<KBox<crate::g16_render_job::Render>>>>>;
pub(crate) type ComputeCache = Arc<Mutex<Option<crate::g16_owner_cache::OwnerCache<KBox<crate::g16_job::Compute>>>>>;

#[derive(Clone)]
pub(crate) enum IdleCache { Render(RenderCache), Compute(ComputeCache) }
impl crate::g16_owner_cache::CacheControl for IdleCache {
    fn same(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Render(a), Self::Render(b)) => Arc::ptr_eq(a, b),
            (Self::Compute(a), Self::Compute(b)) => Arc::ptr_eq(a, b),
            _ => false,
        }
    }
    fn closed(&self) -> bool {
        match self {
            Self::Render(c) => c.lock().is_none(),
            Self::Compute(c) => c.lock().is_none(),
        }
    }
    fn trim_for(&self, bytes: usize) {
        match self {
            Self::Render(c) => { if let Some(c) = Option::as_mut(&mut *c.lock()) { c.trim_for(bytes); } },
            Self::Compute(c) => { if let Some(c) = Option::as_mut(&mut *c.lock()) { c.trim_for(bytes); } },
        }
    }
    fn evict_one(&self) -> bool {
        match self {
            Self::Render(c) => Option::as_mut(&mut *c.lock()).is_some_and(|c| c.evict_one()),
            Self::Compute(c) => Option::as_mut(&mut *c.lock()).is_some_and(|c| c.evict_one()),
        }
    }
}

pub(crate) struct Packet {
    pub(crate) id: u64,
    pub(crate) vm: mmu::Vm,
    pub(crate) notifications: crate::g16_queue::Client,
    pub(crate) commands: KVec<Command>,
    pub(crate) render_cache: RenderCache,
    pub(crate) compute_cache: ComputeCache,
    pub(crate) completion: UserFence<Completion>,
    finish_claimed: AtomicBool,
    failure: Arc<AtomicI32>,
    vm_job: Pin<KBox<Mutex<Option<mmu::M3VmJobGuard>>>>,
}
impl Packet {
    pub(crate) fn previous_result(&self) -> Result {
        let failure = self.failure.load(Ordering::Relaxed);
        if failure == 0 { Ok(()) } else { Err(Error::from_errno(failure)) }
    }
    pub(crate) fn finish(&self, result: Result) {
        if self.finish_claimed.swap(true, Ordering::Relaxed) {
            return;
        }
        if let Err(error) = result {
            let _ = self.failure.compare_exchange(0, error.to_errno(),
                Ordering::Relaxed, Ordering::Relaxed);
            if let Ok(status) = self.vm.status() { status.record(error.to_errno()); }
            self.completion.set_error(error);
        }
        if result.is_ok() {
            let retired = self.vm_job.lock().take();
            core::mem::drop(retired);
        }
        crate::g16_memory::publish();
        let timing = crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming);
        let start = if timing { <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get() } else { 0 };
        self.completion.signal();
        if timing {
            pr_info!("G16G_TIMING fence job={} start_ns={} end_ns={}\n", self.id,
                start, <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get());
        }
    }
}

pub(crate) struct Job { shared: Shared, packet: Arc<Packet>, dev: driver::AsahiDevRef }
impl sched::JobImpl for Job {
    fn run(job: &mut sched::Job<Self>) -> Result<Option<Fence>> {
        let failure = job.packet.failure.load(Ordering::Relaxed);
        if failure != 0 {
            let error = Error::from_errno(failure);
            job.packet.finish(Err(error));
            return Err(error);
        }
        let mut guard = job.shared.lock();
        let result = Option::as_mut(&mut *guard).ok_or(ENODEV)
            .and_then(|runtime| runtime.publish(job.packet.clone()));
        if let Err(error) = result {
            job.packet.finish(Err(error));
            return Err(error);
        }
        drop(guard);
        driver::queue_g16_completion_worker(job.dev.clone());
        Ok(Some(Fence::from_fence(&job.packet.completion)))
    }
    fn timed_out(job: &mut sched::Job<Self>) -> sched::Status {
        let mut guard = job.shared.lock();
        let Some(runtime) = Option::as_mut(&mut *guard) else {
            job.packet.finish(Err(ENODEV));
            return sched::Status::NoDevice;
        };
        runtime.service_job();
        if runtime.healthy() {
            dev_info!(job.dev.as_ref(), "G16G: scheduler deadline rechecked, GPU healthy, job {} retained\n", job.packet.id);
            drop(guard);
            driver::queue_g16_completion_worker(job.dev.clone());
            return sched::Status::NoHang;
        }
        runtime.fail_job(job.packet.id, ETIMEDOUT);
        sched::Status::NoDevice
    }
    fn cancel(job: &mut sched::Job<Self>) {
        let mut guard = job.shared.lock();
        if let Some(runtime) = Option::as_mut(&mut *guard) {
            runtime.fail_job(job.packet.id, ECANCELED);
        }
        job.packet.finish(Err(ECANCELED));
    }
}

struct AddressSpace<'a>(&'a mmu::Vm);
impl g16_uapi::GpuAddressSpace for AddressSpace<'_> {
    fn covers(&self, address: u64, size: u64, access: g16_uapi::GpuAccess) -> bool {
        let Some(end) = address.checked_add(size) else { return false; };
        let (read, write) = match access {
            g16_uapi::GpuAccess::Read => (true, false),
            g16_uapi::GpuAccess::Write => (false, true),
            g16_uapi::GpuAccess::ReadWrite => (true, true),
        };
        address >= 0x4000 && end <= crate::g16_drm::USER_TOP
            && !self.0.driver_range_overlaps(address..end)
            && self.0.covers_range(address, size, read, write)
    }
}

pub(crate) struct Queue {
    entity: sched::Entity<Job>,
    _scheduler: Arc<sched::Scheduler<Job>>,
    shared: Shared,
    dev: driver::AsahiDevRef,
    vm: mmu::Vm,
    notifications: crate::g16_queue::Client,
    usc: u64,
    fences: FenceContexts,
    render_cache: RenderCache,
    compute_cache: ComputeCache,
}

impl Drop for Queue {
    fn drop(&mut self) {
        let renders = self.render_cache.lock().take();
        let computes = self.compute_cache.lock().take();
        drop(renders);
        drop(computes);
    }
}

impl Queue {
    pub(crate) fn new(shared: Shared, scheduler: Arc<sched::Scheduler<Job>>,
        dev: driver::AsahiDevRef, vm: mmu::Vm, priority: u32, usc: u64) -> Result<Self> {
        g16_uapi::QueueUscWindow { base: usc, user_start: 0x4000,
            user_end: crate::g16_drm::USER_TOP }.validate().map_err(|_| EINVAL)?;
        let priority = match priority {
            0 => sched::Priority::Kernel, 1 => sched::Priority::High,
            2 => sched::Priority::Normal, 3 => sched::Priority::Low,
            _ => return Err(EINVAL),
        };
        let notifications = {
            let guard = shared.lock();
            Option::as_ref(&*guard).ok_or(ENODEV)?.new_client_notifications(&vm)?
        };
        Ok(Self { entity: sched::Entity::new(&scheduler, priority)?, _scheduler: scheduler,
            shared, dev, vm, usc, notifications,
            render_cache: Arc::pin_init(new_mutex!(Some(crate::g16_owner_cache::OwnerCache::new())), GFP_KERNEL)?,
            compute_cache: Arc::pin_init(new_mutex!(Some(crate::g16_owner_cache::OwnerCache::new())), GFP_KERNEL)?,
            fences: FenceContexts::new(1, c_str!("asahi_m4_queue"), kernel::static_lock_class!())? })
    }
}
impl queue::Queue for Queue {
    fn submit(&mut self, id: u64, mut syncs: KVec<file::SyncItem>, in_sync_count: usize,
        raw: &[u8], objects: Pin<&xarray::XArray<KBox<file::Object>>>) -> Result {
        let mut parser = g16_uapi::UapiCommandParser::new(raw);
        let mut commands = KVec::new();
        while let Some(command) = parser.next_hardware().map_err(|_| EINVAL)? {
            if let g16_uapi::ParsedHardwareCommand::Render { payload, vertex_attachments, fragment_attachments, .. } = command {
                validate_render(payload, self.usc, &AddressSpace(&self.vm))?;
                g16_uapi::validate_attachments(&AddressSpace(&self.vm), &vertex_attachments).map_err(|_| EINVAL)?;
                g16_uapi::validate_attachments(&AddressSpace(&self.vm), &fragment_attachments).map_err(|_| EINVAL)?;
                let mut copies = core::array::from_fn(|_| None);
                for (target, request) in copies.iter_mut().zip(payload.fragment_end_copies) {
                    *target = Destination::resolve(objects, request)?;
                }
                if copies.iter().any(Option::is_some) && payload.fragment_timestamps.end.handle == 0 {
                    return Err(EINVAL);
                }
                commands.push(Command { payload: Payload::Render { command:payload, usc:self.usc },
                    attachments: [encode_attachments(&self.vm, &vertex_attachments)?,
                                  encode_attachments(&self.vm, &fragment_attachments)?],
                    end_copies: copies,
                    timestamps:[Destination::resolve(objects, payload.vertex_timestamps.start)?,
                        Destination::resolve(objects, payload.vertex_timestamps.end)?,
                        Destination::resolve(objects, payload.fragment_timestamps.start)?,
                        Destination::resolve(objects, payload.fragment_timestamps.end)?] }, GFP_KERNEL)?;
                continue;
            }
            let g16_uapi::ParsedHardwareCommand::Compute { payload, attachments, .. } = command
                else { return Err(EINVAL); };
            let wide_flag = kernel::uapi::drm_asahi_compute_flags_DRM_ASAHI_COMPUTE_WIDE_VISIBILITY as u32;
            if payload.flags & !wide_flag != 0 || payload.sampler_count != 0 || payload.sampler_heap != 0 {
                return Err(ENOTSUPP);
            }
            let mut copies = core::array::from_fn(|_| None);
            for (target, request) in copies.iter_mut().zip(payload.end_copies) {
                if request.handle == 0 && request.offset != 0 { return Err(EINVAL); }
                *target = Destination::resolve(objects, request)?;
            }
            if copies.iter().any(Option::is_some) && payload.timestamps.end.handle == 0 {
                return Err(EINVAL);
            }
            let mut base_payload = payload;
            base_payload.flags &= !wide_flag;
            base_payload.end_copies = [g16_uapi::UapiTimestamp { handle: 0, offset: 0 }; 8];
            let cmd = g16_uapi::translate_compute_command(base_payload, attachments,
                g16_uapi::QueueUscWindow { base: self.usc, user_start: 0x4000,
                    user_end: crate::g16_drm::USER_TOP }, &AddressSpace(&self.vm))
                .map_err(|_| EINVAL)?;
            commands.push(Command {
                payload: Payload::Compute(crate::g16_compute::Control { base: cmd.control_stream_base,
                    end: cmd.control_stream_end, usc_base: self.usc,
                    wide_visibility: payload.flags & wide_flag != 0 }),
                attachments: [encode_attachments(&self.vm, &cmd.attachments)?,
                              crate::g16_attachments::Attachments::EMPTY],
                end_copies: copies,
                timestamps: [Destination::resolve(objects, cmd.timestamps.start)?,
                    Destination::resolve(objects, cmd.timestamps.end)?, None, None],
            }, GFP_KERNEL)?;
        }
        parser.finish().map_err(|_| EINVAL)?;
        let mut jobs = KVec::with_capacity(commands.len(), GFP_KERNEL)?;
        let failure = Arc::new(AtomicI32::new(0), GFP_KERNEL)?;
        let mut commands = commands.into_iter().peekable();
        let packet_limit = crate::g16_runtime::packet_limit();
        while let Some(command) = commands.next() {
            let mut single = KVec::with_capacity(packet_limit, GFP_KERNEL)?;
            let engine = usize::from(matches!(command.payload, Payload::Compute(_)));
            let mut engine_counts = [0usize; 2];
            engine_counts[engine] = 1;
            single.push(command, GFP_KERNEL)?;
            while single.len() < packet_limit {
                let Some(next) = commands.peek() else { break; };
                let engine = usize::from(matches!(next.payload, Payload::Compute(_)));
                if engine_counts[engine] == crate::g16_runtime::COMPUTE_DEPTH { break; }
                let command = commands.next().expect("peeked command");
                engine_counts[engine] += 1;
                single.push(command, GFP_KERNEL)?;
            }
            let packet = Arc::new(Packet { id, vm: self.vm.clone(), commands: single,
                notifications: self.notifications.clone(),
                render_cache: self.render_cache.clone(),
                compute_cache: self.compute_cache.clone(),
                finish_claimed: AtomicBool::new(false),
                failure: failure.clone(),
                completion: self.fences.new_fence(0, Completion)?.into(),
                vm_job: KBox::pin_init(new_mutex!(Some(self.vm.retain_m3_job()?)),
                    GFP_KERNEL)? }, GFP_KERNEL)?;
            jobs.push(Job { shared: self.shared.clone(), packet, dev: self.dev.clone() }, GFP_KERNEL)?;
        }
        let mut dependencies = KVec::with_capacity(in_sync_count, GFP_KERNEL)?;
        for sync in syncs.drain(0..in_sync_count) {
            if let Some(fence) = sync.fence { dependencies.push(fence, GFP_KERNEL)?; }
        }
        let finished = self.entity.submit_serialized_batch(1, jobs, dependencies)?;
        for mut sync in syncs {
            if let Some(chain) = sync.chain_fence.take() {
                sync.syncobj.add_point(chain, &finished, sync.timeline_value);
            } else { sync.syncobj.replace_fence(Some(&finished)); }
        }
        Ok(())
    }
}

fn encode_attachments(vm: &mmu::Vm, list: &g16_uapi::UapiAttachmentList)
    -> Result<crate::g16_attachments::Attachments> {
    use crate::g16_attachments::{Attachments, CAPACITY};
    use g16_uapi::{GpuAddressSpace, GpuAccess};
    let mut ranges = [(0, 0); CAPACITY];
    for (range, entry) in ranges.iter_mut().zip(list.as_slice()) {
        *range = Attachments::range(entry.address, entry.size).map_err(|_| EINVAL)?;
        if !AddressSpace(vm).covers(range.0, range.1, GpuAccess::Write) {
            return Err(EINVAL);
        }
    }
    Attachments::new(&ranges[..list.as_slice().len()]).map_err(|_| EINVAL)
}

fn validate_render(r: g16_uapi::UapiRenderCommand, usc: u64, space: &AddressSpace<'_>) -> Result {
    use g16_uapi::{GpuAddressSpace, GpuAccess};
    let empty = g16_uapi::UapiHelperProgram { binary:0, config:0, data:0 };
    if r.flags & !(2|16|(1<<5)|(1<<6)|(1<<7)|(1<<18)) != 0 || !matches!(r.samples, 1 | 2 | 4)
        || r.sampler_count != 0 || r.sampler_heap != 0
        || r.vertex_helper != empty || r.fragment_helper != empty
        || r.depth.compression_base != 0 || r.stencil.compression_base != 0
        || r.depth.compression_stride != 0 || r.stencil.compression_stride != 0
        || r.ppp_control & !0x203 != 0 { return Err(ENOTSUPP); }
    if r.flags & (1 << 6) != 0 && r.flags & (1 << 5) == 0 { return Err(EINVAL); }
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
    for zls in [r.depth,r.stencil] { g16_uapi::validate_zls(space,zls,r.layers).map_err(|_| EINVAL)?; }
    let window = g16_uapi::QueueUscWindow { base:usc,user_start:0x4000,user_end:crate::g16_drm::USER_TOP };
    for p in [r.background,r.end_of_tile,r.partial_background,r.partial_end_of_tile] {
        window.full_program(space,p.usc).map_err(|_| EINVAL)?;
        if !space.covers(usc+u64::from(p.usc & !63),64,GpuAccess::Read) { return Err(EINVAL); }
    }
    Ok(())
}
