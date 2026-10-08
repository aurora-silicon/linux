// SPDX-License-Identifier: GPL-2.0-only OR MIT


use core::mem::ManuallyDrop;
use kernel::{device::Core, platform, prelude::*, soc::apple::rtkit, sync::Arc,
    time::{delay::fsleep, Delta}};
use crate::{driver, g16_device::Device, g16_rtkit, mmu};

struct Inner {
    transport: rtkit::RtKit<g16_rtkit::Operations>,
    state: Arc<g16_rtkit::State>,
    queue: crate::g16_queue::Queue,
    compute_queue: crate::g16_queue::Queue,
    fragment_queue: crate::g16_queue::Queue,
    queue_context: crate::g16_queue::Context,
    fragment_registered: bool,
    retired_renders: KVec<(KBox<crate::g16_render_job::Render>, crate::g16_submit::RenderCache)>,
    retired_computes: KVec<(KBox<crate::g16_job::Compute>, crate::g16_submit::ComputeCache)>,
    idle_caches: crate::g16_owner_cache::Registry<crate::g16_submit::IdleCache>,
    compute: KBox<crate::g16_job::Compute>,
    config: crate::g16_config::Config,
    uat: mmu::Uat,
    drm: driver::AsahiDevRef,
    device: Device,
    generation: u32,
    gpu_pending: bool,
    #[cfg(CONFIG_DEV_COREDUMP)]
    fault_captured: bool,
    #[cfg(CONFIG_DEV_COREDUMP)]
    fault_reserve: Option<crate::g16_fault::Dump>,
    busy_since_ns: Option<i64>,
    command_label: u8,
    render_stamp: u32,
    compute_stamp: u32,
    inflight: KVec<Active>,
    pending: KVec<KVec<Active>>,
    messages_at_wait: u64,
    waits: u64,
    late_retirements: u64,
    wait_timeouts: u64,
    summary: SubmitSummary,
    last_retired: Option<(u64, i64)>,
    last_gpu_end: Option<(u64, u64, usize)>,
}

struct SubmitSummary {
    since_ns: i64,
    commands: [u64; 2],
    packets: [u64; MAX_PACKET_COMMANDS + 1],
    packet_active_ns: u64,
    preparation_ns: [u64; 2],
    active_ns: [u64; 2],
    gpu_ns: [u64; 2],
    render_clear: [u64; 3],
    render_stage_ns: [i64; 3],
    geometry: [RenderSummary; 16],
    geometry_overflow: u64,
    consecutive_gap_ns: u64,
    consecutive_gap_count: u64,
    wait_phases: [[u64; 6]; 3],
    pstate_samples: [u64; 16],
    engine_gap_ticks: [u64; 2],
    engine_gap_count: [u64; 2],
    classified_gap_ticks: [u64; 8],
    classified_gap_counts: [u64; 8],
    publication_gap_ticks: [[u64; 3]; 8],
    final_retirement_ticks: [u64; 3], // count, total, maximum
    publication_invalid: u64,
    gpu_overlaps: u64,
}
impl Default for SubmitSummary {
    fn default() -> Self {
        Self {
            since_ns: Default::default(),
            commands: Default::default(),
            packets: [0; MAX_PACKET_COMMANDS + 1],
            packet_active_ns: Default::default(),
            preparation_ns: Default::default(),
            active_ns: Default::default(),
            gpu_ns: Default::default(),
            render_clear: Default::default(),
            render_stage_ns: Default::default(),
            geometry: Default::default(),
            geometry_overflow: Default::default(),
            consecutive_gap_ns: Default::default(),
            consecutive_gap_count: Default::default(),
            wait_phases: Default::default(),
            pstate_samples: Default::default(),
            engine_gap_ticks: Default::default(),
            engine_gap_count: Default::default(),
            classified_gap_ticks: Default::default(),
            classified_gap_counts: Default::default(),
            publication_gap_ticks: Default::default(),
            final_retirement_ticks: Default::default(),
            publication_invalid: 0,
            gpu_overlaps: Default::default(),
        }
    }
}

#[derive(Clone, Copy, Default)]
struct RenderSummary {
    key: [u32; 3],
    count: u64,
    preparation_ns: u64,
    active_ns: u64,
    fragment_ns: u64,
    max_fragment_ns: u64,
}
impl SubmitSummary {
    fn record(&mut self, render: bool, preparation: i64, active: i64, gpu: u64) {
        let i = usize::from(!render);
        self.commands[i] += 1;
        self.preparation_ns[i] += preparation.max(0) as u64;
        self.active_ns[i] += active.max(0) as u64;
        self.gpu_ns[i] += gpu;
    }
}

pub(crate) const COMPUTE_DEPTH: usize = 16;
pub(crate) const MAX_PACKET_COMMANDS: usize = COMPUTE_DEPTH * 2;
pub(crate) fn packet_limit() -> usize {
    (*crate::module_parameters::g16_packet_commands.value() as usize).clamp(1, MAX_PACKET_COMMANDS)
}
pub(crate) const POOL_SLOTS: usize = COMPUTE_DEPTH * 2;

fn stamp_reached(observed: u32, target: u32) -> bool {
    observed != 0 && observed.wrapping_sub(target) < 2 * MAX_PACKET_COMMANDS as u32 * 0x100
}


#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Service {
    Idle,
    Progress,
    Waiting(u64),
}

enum Work {
    Compute(KBox<crate::g16_job::Compute>),
    Render(KBox<crate::g16_render_job::Render>),
}
impl Work {
    fn pipe(&self) -> usize { match self { Self::Compute(_) => 2, Self::Render(r) => r.index() } }
    fn stamp(&self) -> u32 {
        match self { Self::Compute(c) => c.stamp(), Self::Render(r) => r.stamp() }
    }
    fn status(&mut self) -> Result<[u64; 4]> {
        match self { Self::Compute(c) => { let s=c.status()?; Ok([s[0],s[1],s[2],s[3]]) }, Self::Render(r)=>r.status() }
    }
}
impl Inner {
    fn publish_active(&mut self, active: &mut Active) -> Result {
        self.config.update_thermal_limit(&self.drm)?;
        if let Work::Render(r) = &active.work {
            r.notifier()?.lock().publish(true)?;
            let mut ta_commands = [0; 3];
            let mut count = 0;
            if let Some(address) = r.compute_dependency() {
                ta_commands[count] = address;
                count += 1;
            }
            if let Some(address) = r.init_address() {
                ta_commands[count] = address;
                count += 1;
            }
            ta_commands[count] = r.address();
            let (ta,ta_next) = self.queue.append_batch_idle(&ta_commands[..count+1])?;
            let (frag,frag_next) = self.fragment_queue.append_batch_idle(
                &[r.fragment_dependency(),r.fragment_address()])?;
            active.queue_next[0] = ta_next;
            active.queue_next[1] = frag_next;
            active.pipe_next[0] = self.config.publish_queue(0,ta,ta_next.try_into()?,
                crate::g16_render_job::SLOTS[0],false)?;
            active.pipe_next[1] = self.config.publish_queue(1,frag,frag_next.try_into()?,
                crate::g16_render_job::SLOTS[1],!self.fragment_registered)?;
            Pin::new(&mut self.transport).send_message(0x21,0x0083000000000001)?;
            Pin::new(&mut self.transport).send_message(0x21,0x0083000000000000)?;
            self.fragment_registered = true;
            return Ok(());
        }
        let Work::Compute(c) = &active.work else { return Err(EIO); };
        active.packet.notifications.lock().publish(false)?;
        let (address,next) = self.compute_queue.append_batch(&[c.address()])?;
        active.queue_next[2] = next;
        active.pipe_next[2] = self.config.publish_queue(2,address,next.try_into()?,
            crate::g16_compute::SLOT,false)?;
        Pin::new(&mut self.transport).send_message(0x21,0x0083000000000002)?;
        Ok(())
    }
}
struct Active {
    packet: Arc<crate::g16_submit::Packet>,
    bank: usize,
    work: Work,
    index: usize,
    queue_next: [u32; 3],
    pipe_next: [u32; 3],
    started: kernel::time::Instant<kernel::time::Monotonic>,
    preparation_ns: i64,
    published: kernel::time::Instant<kernel::time::Monotonic>,
    publication_tick: u64,
}

pub(crate) struct Runtime {
    inner: ManuallyDrop<Inner>,
}

impl Runtime {
    pub(crate) fn drm(&self) -> driver::AsahiDevRef { self.inner.drm.clone() }
    pub(crate) fn core_mask(&self) -> u32 { self.inner.device.core_mask() }
    pub(crate) fn health(&self) -> Arc<g16_rtkit::Health> { self.inner.state.health.clone() }
    pub(crate) fn events(&self) -> Arc<g16_rtkit::EventWait> { self.inner.state.events.clone() }
    pub(crate) fn note_wait_timeout(&mut self) { self.inner.wait_timeouts += 1; }

    pub(crate) fn healthy(&self) -> bool { self.inner.state.healthy() }
    pub(crate) fn new_client_notifications(&self, vm: &mmu::Vm) -> Result<crate::g16_queue::Client> {
        crate::g16_queue::ClientNotifications::new(&self.inner.drm, &self.inner.uat, vm)
    }
    pub(crate) fn new_user_vm(&mut self, id: u64, range: core::ops::Range<u64>) -> Result<mmu::Vm> {
        use crate::util::RangeExt;
        let driver_range = 0x70_0000_0000..0x80_0000_0000;
        if range.overlaps(driver_range.clone()) { return Err(EINVAL); }
        let vm = self.inner.uat.new_vm(id, range)?;
        let mut reserved = KVec::new();
        reserved.push(driver_range, GFP_KERNEL)?;
        let mut mappings = KVec::new();
        mappings.push(self.inner.config.bind_uma_table(&vm)?, GFP_KERNEL)?;
        mappings.push(self.inner.config.bind_parameter_table(&vm)?, GFP_KERNEL)?;
        let scratch_range = 0x10_0000_0000..0x10_0001_0000;
        if vm.driver_range_overlaps(scratch_range.clone()) { return Err(EINVAL); }
        let mut scratch = crate::gem::new_kernel_object_wc(&self.inner.drm, 0x10000)?;
        scratch.vmap()?.memset(0);
        mappings.push(scratch.map_at(&vm, scratch_range.start, mmu::PROT_GPU_SHARED_RW, true)?, GFP_KERNEL)?;
        reserved.push(scratch_range, GFP_KERNEL)?;
        vm.install_driver_mappings(mappings, reserved)?;
        Ok(vm)
    }

    pub(crate) fn map_timestamp(&self, mut bo: crate::gem::ObjectRef,
        range: core::ops::Range<usize>) -> Result<mmu::KernelMapping> {
        bo.map_range_into_range(self.inner.uat.kernel_vm(), range,
            crate::g16_memory::TIMESTAMP_RANGE, mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW, false)
    }

    pub(crate) fn publish(&mut self, packet: Arc<crate::g16_submit::Packet>) -> Result {
        if !self.healthy() { return Err(EIO); }
        if packet.commands.is_empty() { return Err(EINVAL); }
        let count = packet.commands.len();
        if count > MAX_PACKET_COMMANDS { return Err(EINVAL); }
        let mut engine_counts = [0usize; 2];
        for command in &packet.commands {
            let engine = usize::from(matches!(command.payload, crate::g16_submit::Payload::Compute(_)));
            engine_counts[engine] += 1;
            if engine_counts[engine] > COMPUTE_DEPTH { return Err(EINVAL); }
        }
        let bank = (0..2).find(|bank|
            !self.inner.inflight.iter().any(|active| active.bank == *bank) &&
            !self.inner.pending.iter().any(|batch| batch[0].bank == *bank))
            .ok_or(EBUSY)?;
        let mut batch = KVec::with_capacity(count, GFP_KERNEL)?;
        self.inner.pending.reserve(1, GFP_KERNEL)?;
        let notifier_checkpoint = packet.notifications.lock().checkpoint();
        let stamp_checkpoint = (self.inner.render_stamp, self.inner.compute_stamp);
        let share_parameters = packet.commands.iter().any(|command| matches!(
            command.payload, crate::g16_submit::Payload::Render { command, .. }
                if command.flags & (1 << 6) != 0));
        let shared_parameters = if share_parameters {
            let mut bytes = 0usize;
            for command in &packet.commands {
                if let crate::g16_submit::Payload::Render { command, .. } = command.payload {
                    let geometry = crate::g16_parameter::geometry(command)?;
                    bytes = bytes.checked_add(crate::g16_parameter::bytes(&geometry)?)
                        .ok_or(EOVERFLOW)?;
                }
            }
            Some(crate::g16_parameter::Parameters::new(&self.inner.drm,
                &self.inner.uat, &packet.vm, bytes, (bank*COMPUTE_DEPTH) as u32, true)?)
        } else { None };
        let mut engine_offsets = [0usize; 2];
        for offset in 0..count {
            let engine = usize::from(matches!(packet.commands[offset].payload,
                crate::g16_submit::Payload::Compute(_)));
            let slot = bank * COMPUTE_DEPTH + engine_offsets[engine];
            engine_offsets[engine] += 1;
            let active = match self.prepare_command(packet.clone(), offset,
                slot as u64, shared_parameters.clone()) {
                Ok(active) => active,
                Err(error) => {
                    self.inner.render_stamp = stamp_checkpoint.0;
                    self.inner.compute_stamp = stamp_checkpoint.1;
                    if let Err(restore_error) = packet.notifications.lock().restore(notifier_checkpoint) {
                        self.inner.state.health.mark_failed();
                        dev_err!(self.inner.drm.as_ref(), "G16G: reservation rollback failed {:?}\n", restore_error);
                        return Err(error);
                    }
                    dev_err!(self.inner.drm.as_ref(),
                        "G16G: packet preparation failed job={} bank={} index={} count={} error={:?}\n",
                        packet.id, bank, offset, count, error);
                    return Err(error);
                }
            };
            batch.push(active, GFP_KERNEL).expect("reserved prepared packet capacity");
        }
        self.inner.pending.push(batch, GFP_KERNEL).expect("reserved prepared packet");
        self.pump()
    }

    fn pump(&mut self) -> Result {
        while !self.inner.pending.is_empty() {
            let batch = self.inner.pending.remove(0).map_err(|_| EIO)?;
            let packet = batch[0].packet.clone();
            if let Err(error) = packet.previous_result() {
                packet.finish(Err(error));
                continue;
            }
            if let Err(error) = self.start_batch(batch) {
                packet.finish(Err(error));
                if self.inner.state.health.failed() { self.fail_all(error); }
                return Err(error);
            }
        }
        Ok(())
    }

    fn start_batch(&mut self, mut batch: KVec<Active>) -> Result<()> {
        let count = batch.len();
        self.inner.inflight.reserve(count, GFP_KERNEL)?;
        self.inner.retired_computes.reserve(count, GFP_KERNEL)?;
        self.inner.retired_renders.reserve(count, GFP_KERNEL)?;
        let inner = &mut *self.inner;
        if inner.inflight.is_empty() {
            inner.busy_since_ns = Some(<kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get());
        }
        inner.state.health.set_gpu_pending(true);
        inner.gpu_pending = true;
        if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
            let tick = physical_counter();
            for active in batch.iter_mut() { active.publication_tick = tick; }
        }
        let publish = if count == 1 && inner.inflight.is_empty() {
            batch[0].published = kernel::time::Instant::<kernel::time::Monotonic>::now();
            batch[0].started = kernel::time::Instant::<kernel::time::Monotonic>::now();
            inner.publish_active(&mut batch[0])
        } else {
            (|| -> Result {
                let mut addresses = [[0u64; COMPUTE_DEPTH * 3]; 3];
                let mut entries = [0usize; 3];
                let mut previous_render = None;
                let mut ordered_tiling = None;
                let mut compute_since_render = false;
                let shared_tiling = batch.iter().any(|active| matches!(
                    active.packet.commands[active.index].payload,
                    crate::g16_submit::Payload::Render { command, .. }
                        if command.flags & (1 << 6) != 0));
                let mut previous = inner.inflight.last().map(|active| -> Result<_> { Ok(match &active.work {
                    Work::Compute(c) => (active.packet.notifications.lock().notification(2).fw_stamp,
                        crate::g16_compute::SLOT, c.stamp()),
                    Work::Render(r) => (r.notifier()?.lock().notification(1).fw_stamp,
                        crate::g16_render_job::SLOTS[1], r.stamp()),
                }) }).transpose()?;
                for active in batch.iter_mut() {
                    match &mut active.work {
                        Work::Compute(c) => {
                            compute_since_render = true;
                            if let Some((address, slot, stamp)) = previous {
                                addresses[2][entries[2]] = c.dependency(address, slot, stamp)?;
                                entries[2] += 1;
                            }
                            addresses[2][entries[2]] = c.address();
                            entries[2] += 1;
                            previous = Some((active.packet.notifications.lock().notification(2).fw_stamp,
                                crate::g16_compute::SLOT, c.stamp()));
                            active.packet.notifications.lock().publish(false)?;
                        }
                        Work::Render(r) => {
                            let fragment_only = match active.packet.commands[active.index].payload {
                                crate::g16_submit::Payload::Render { command, .. } =>
                                    command.flags & (1 << 5) != 0 &&
                                    command.vertex_timestamps.start.handle == 0 &&
                                    command.fragment_timestamps.start.handle == 0,
                                _ => false,
                            };
                            let across_compute = match active.packet.commands[active.index].payload {
                                crate::g16_submit::Payload::Render { command, .. } =>
                                    command.flags & (1 << 6) != 0,
                                _ => false,
                            };
                            let early_tiling = previous_render.filter(|(packet, _, _)|
                                fragment_only && *packet == active.packet.id &&
                                    (!compute_since_render || across_compute));
                            let ta_predecessor = if shared_tiling && early_tiling.is_some() {
                                ordered_tiling.or(early_tiling)
                            } else { early_tiling };
                            let tiling_dependency = ta_predecessor.map(|(_, address, stamp)|
                                (address, crate::g16_render_job::SLOTS[0], stamp)).or(previous);
                            if let Some((address, slot, stamp)) = tiling_dependency {
                                r.wait_for_prior(address, slot, stamp)?;
                                addresses[0][entries[0]] = r.compute_dependency().ok_or(EIO)?;
                                entries[0] += 1;
                            }
                            if let Some(address) = r.init_address() {
                                addresses[0][entries[0]] = address;
                                entries[0] += 1;
                            }
                            addresses[0][entries[0]] = r.address();
                            entries[0] += 1;
                            if early_tiling.is_some() {
                                let (address, slot, stamp) = previous.ok_or(EIO)?;
                                addresses[1][entries[1]] = r.wait_for_prior_fragment(address, slot, stamp)?;
                                entries[1] += 1;
                            }
                            addresses[1][entries[1]] = r.fragment_dependency();
                            addresses[1][entries[1] + 1] = r.fragment_address();
                            entries[1] += 2;
                            r.notifier()?.lock().publish(true)?;
                            previous = Some((r.notifier()?.lock().notification(1).fw_stamp,
                                crate::g16_render_job::SLOTS[1], r.stamp()));
                            compute_since_render = false;
                            previous_render = Some((active.packet.id,
                                r.notifier()?.lock().notification(0).fw_stamp, r.stamp()));
                            if early_tiling.is_none() { ordered_tiling = previous_render; }
                        }
                    }
                }
                let mut next = [0u32; 3];
                let mut pipe_next = [0u32; 3];
                for i in 0..3 {
                    if entries[i] == 0 { continue; }
                    let queue = match i { 0 => &mut inner.queue, 1 => &mut inner.fragment_queue,
                        _ => &mut inner.compute_queue };
                    let (address, index) = queue.append_batch(&addresses[i][..entries[i]])?;
                    next[i] = index;
                    let slot = if i == 2 { crate::g16_compute::SLOT }
                        else { crate::g16_render_job::SLOTS[i] };
                    pipe_next[i] = inner.config.publish_queue(i, address, index.try_into()?,
                        slot, i == 1 && !inner.fragment_registered)?;
                }
                for active in batch.iter_mut() {
                    active.queue_next = next;
                    active.pipe_next = pipe_next;
                    active.published = kernel::time::Instant::<kernel::time::Monotonic>::now();
                    active.started = kernel::time::Instant::<kernel::time::Monotonic>::now();
                }
                for i in [1, 0, 2] {
                    if entries[i] != 0 {
                        Pin::new(&mut inner.transport).send_message(0x21, 0x0083000000000000 | i as u64)?;
                    }
                }
                if entries[1] != 0 { inner.fragment_registered = true; }
                Ok(())
            })()
        };
        for active in batch { inner.inflight.push(active, GFP_KERNEL).expect("reserved batch capacity"); }
        if let Err(error) = publish {
            inner.state.health.mark_failed();
            dev_err!(inner.drm.as_ref(), "G16G: GPU retirement unproven after batch publication error {:?}\n", error);
            return Err(error);
        }
        Ok(())
    }

    fn prepare_command(&mut self, packet: Arc<crate::g16_submit::Packet>, index: usize,
        batch_offset: u64, shared_parameters: Option<crate::g16_parameter::Pool>) -> Result<Active> {
        let preparing = kernel::time::Instant::<kernel::time::Monotonic>::now();
        let inner = &mut *self.inner;
        if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
            if let Some((id, retired)) = inner.last_retired {
                if id == packet.id {
                    let now = <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get();
                    inner.summary.consecutive_gap_ns += (now - retired).max(0) as u64;
                    inner.summary.consecutive_gap_count += 1;
                }
            }
        }
        inner.config.reset_trace_budget();
        let payload = packet.commands.get(index).ok_or(EINVAL)?.payload;
        let label = inner.command_label.wrapping_add(1).max(1);
        if matches!(payload, crate::g16_submit::Payload::Render { .. }) {
            inner.render_stamp = inner.render_stamp.wrapping_add(0x100).max(0x100);
        } else {
            inner.compute_stamp = inner.compute_stamp.wrapping_add(0x100).max(0x100);
        }
        let render = matches!(payload, crate::g16_submit::Payload::Render { .. });
        let notifier = if shared_parameters.is_some() &&
            matches!(payload, crate::g16_submit::Payload::Render { .. }) {
            let client = crate::g16_queue::ClientNotifications::new(&inner.drm, &inner.uat, &packet.vm)?;
            client
        } else { packet.notifications.clone() };
        let (notifications, render_counter) = {
            let mut client = notifier.lock();
            let previous = (if render { inner.render_stamp } else { inner.compute_stamp })
                .wrapping_sub(0x100);
            client.prime_unused_stamps(render, previous)?;
            client.reserve(render)?
        };
        let pool_slot: u8 = (batch_offset + 1).try_into()?;
        let compute_owner = if matches!(payload, crate::g16_submit::Payload::Compute(_)) {
            let mut cache = packet.compute_cache.lock();
            Option::as_mut(&mut *cache).and_then(|cache| {
                cache.take(|owner| owner.pool_slot() == pool_slot && owner.same_vm(&packet.vm))
                    .or_else(|| cache.take(|owner| owner.same_vm(&packet.vm)))
            })
        } else { None };
        let render_owner = if matches!(payload, crate::g16_submit::Payload::Render { .. }) {
            let mut cache = packet.render_cache.lock();
            Option::as_mut(&mut *cache).and_then(|cache| {
                cache.take(|owner| owner.pool_slot() == batch_offset as u8 && owner.same_vm(&packet.vm))
                    .or_else(|| cache.take(|owner| owner.same_vm(&packet.vm)))
            })
        } else { None };
        let mut work = match payload {
            crate::g16_submit::Payload::Compute(control) => Work::Compute(crate::g16_job::Compute::in_vm(
                &inner.drm,&inner.uat,&mut inner.config,&packet.vm,inner.compute_queue.address(),Some(control),
                Some(notifications[2]),inner.compute_stamp,
                compute_owner, 0, pool_slot)?),
            crate::g16_submit::Payload::Render { command,usc } => Work::Render(crate::g16_render_job::Render::new(
                &inner.drm,&inner.uat,&packet.vm,command,usc,label,inner.render_stamp,render_counter,
                [inner.queue.address(),inner.fragment_queue.address()],
                [notifications[0],notifications[1]],
                inner.config.render_stats(), batch_offset.try_into()?, render_owner, shared_parameters)?),
        };
        let attachments = &packet.commands[index].attachments;
        let minimal = crate::debug::debug_enabled(crate::debug::DebugFlags::M3PassTiming)
            && index != 0 && index + 1 != packet.commands.len();
        match &mut work {
            Work::Compute(compute) => {
                compute.retain_notifier(packet.notifications.clone());
                if matches!(payload, crate::g16_submit::Payload::Compute(c) if c.wide_visibility) &&
                    packet.commands[index].timestamp_addresses() == [0, 0] &&
                    packet.commands[index].end_copy_addresses() == [0; 8] &&
                    packet.commands[index + 1..].iter().any(|command|
                        matches!(command.payload, crate::g16_submit::Payload::Compute(_))) {
                    compute.coalesce_stamp_flush()?;
                }
                compute.set_attachments(&attachments[0])?;
                compute.set_user_timestamps(packet.commands[index].timestamp_addresses())?;
                compute.set_timestamp_copies(packet.commands[index].end_copy_addresses())?;
            }
            Work::Render(render) => {
                render.retain_notifier(notifier);
                if !packet.commands[index + 1..].iter().any(|command|
                    matches!(command.payload, crate::g16_submit::Payload::Render { .. })) {
                    render.request_completion_flush()?;
                }
                render.set_attachments(attachments)?;
                render.set_user_timestamps(packet.commands[index].render_timestamp_addresses())?;
                render.set_fragment_timestamp_copies(packet.commands[index].end_copy_addresses())?;
                if matches!(payload, crate::g16_submit::Payload::Render { command, .. }
                    if command.flags & (1 << 7) != 0) {
                    render.cache_programs(&mut inner.config, &inner.drm, inner.uat.kernel_vm())?;
                }
                if minimal {
                    let pairs = packet.commands[index].render_timestamp_addresses();
                    render.omit_internal_timestamps([pairs[0] == [0, 0],
                        pairs[1] == [0, 0] && packet.commands[index].end_copy_addresses() == [0; 8]])?;
                }
                render.select_fragment_start_abi(inner.device.firmware().board.chip_id)?;
            }
        }
        inner.command_label = label;
        inner.device.check_pstate()?;
        Ok(Active { packet, bank: batch_offset as usize / COMPUTE_DEPTH, work, index, queue_next:[0;3],pipe_next:[0;3],
            preparation_ns: preparing.elapsed().as_nanos(),
            published: kernel::time::Instant::<kernel::time::Monotonic>::now(), publication_tick: 0,
            started:kernel::time::Instant::<kernel::time::Monotonic>::now() })
    }

    pub(crate) fn fail_job(&mut self, id: u64, error: Error) {
        let inner = &mut *self.inner;
        if inner.inflight.iter().any(|entry| entry.packet.id == id) {
            dev_err!(inner.drm.as_ref(), "G16G: GPU retirement unproven, job {} error {:?}; retaining DMA owners\n", id, error);
            self.fail_all(error);
            return;
        }
        if let Some(position) = inner.pending.iter().position(|batch| batch[0].packet.id == id) {
            if let Ok(batch) = inner.pending.remove(position) {
                batch[0].packet.finish(Err(error));
            }
        }
    }

    fn fail_all(&mut self, error: Error) {
        let inner = &mut *self.inner;
        inner.state.health.mark_failed();
        for active in &inner.inflight {
            active.packet.finish(Err(error));
        }
        for batch in &inner.pending {
            if let Some(active) = batch.first() {
                active.packet.finish(Err(error));
            }
        }
        #[cfg(CONFIG_DEV_COREDUMP)]
        self.capture_fault(error);
        self.inner.pending.clear();
    }

    pub(crate) fn service_job(&mut self) -> Service {
        if !self.inner.state.healthy() {
            self.fail_all(EIO);
            return Service::Idle;
        }
        let inner = &mut *self.inner;
        if let Err(error) = inner.config.update_thermal_limit(&inner.drm) {
            dev_err!(self.inner.drm.as_ref(), "G16G: cannot publish thermal ceiling {:?}\n", error);
            self.fail_all(error);
            return Service::Idle;
        }
        if self.inner.inflight.is_empty() {
            let inner = &mut *self.inner;
            if let Err(error) = inner.config.drain(&inner.drm) {
                inner.state.health.set_gpu_pending(true);
                inner.gpu_pending = true;
                dev_err!(inner.drm.as_ref(), "G16G: GPU retirement unproven after idle firmware error {:?}; retaining DMA owners\n", error);
                self.fail_all(error);
                let _ = self.inner.device.log_engine_state();
            }
            return Service::Idle;
        }
        let id = self.inner.inflight[0].packet.id;
        let messages = self.inner.state.event_messages.load(core::sync::atomic::Ordering::Acquire);
        match self.poll_job() {
            Ok(true) => {
                if messages == self.inner.messages_at_wait { self.inner.late_retirements += 1; }
                if self.inner.inflight.is_empty() { Service::Idle } else { Service::Progress }
            }
            Ok(false) => {
                self.inner.messages_at_wait = messages;
                self.inner.waits += 1;
                Service::Waiting(messages)
            }
            Err(error) => {
                let _ = self.inner.device.log_engine_state();
                self.fail_job(id, error); Service::Idle
            }
        }
    }

    fn poll_job(&mut self) -> Result<bool> {
        let inner = &mut *self.inner;
        inner.config.drain(&inner.drm)?;
        if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
        let pstate = inner.device.check_pstate()?;
        inner.config.observe_thermal_state(&inner.drm, pstate);
        if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
            inner.summary.pstate_samples[pstate as usize] += 1;
        }
        let alone = inner.inflight.len() == 1;
        let active = inner.inflight.first_mut().ok_or(EIO)?;
        let snapshot_after_deadline = active.started.elapsed() >= Delta::from_secs(2);
        let mut status = active.work.status()?;
        let pipe_index = active.work.pipe();
        let engine = match pipe_index { 0=>&mut inner.queue,1=>&mut inner.fragment_queue,_=>&mut inner.compute_queue };
        let [firmware, shared] = match &active.work {
            Work::Render(r) => r.notifier()?.lock().stamps(pipe_index)?,
            Work::Compute(_) => active.packet.notifications.lock().stamps(pipe_index)?,
        };
        if stamp_reached(shared, active.work.stamp()) {
            status[1] = active.work.stamp() as u64;
        }
        if stamp_reached(firmware, active.work.stamp()) {
            status[0] = active.work.stamp() as u64;
        }
        if pipe_index == 0 {
            if let Work::Render(render) = &mut active.work {
                let fragment = render.notifier()?.lock().stamps(1)?;
                if fragment == [render.stamp(); 2] &&
                    render.uma_completed()? == render.uma_submitted() {
                    status[1] = render.stamp() as u64;
                }
            }
        }
        let queue = engine.status()?;
        let pipe = inner.config.pipe_indices(pipe_index)?;
        let next = active.queue_next[pipe_index];
        let mut gpu_span = [0u64; 2];
        let mut render_stage_ns = [0i64; 3];
        let pipe_next = active.pipe_next[pipe_index];
        let queue_done = if alone { queue == [next, next, next, next, next, 0] }
            else { engine.consumed_through(&queue, next) };
        let pipe_done = if alone { pipe == [pipe_next; 3] }
            else { pipe[..2].iter().all(|index| (index.wrapping_sub(pipe_next) % 256) < 128) };
        let private_done = match &mut active.work {
            Work::Render(render) if pipe_index == 1 =>
                render.uma_completed()? == render.uma_submitted(),
            Work::Compute(compute) => compute.uma_completed()? == compute.uma_submitted(),
            _ => true,
        };
        if status[0] == active.work.stamp() as u64
            && status[1] == active.work.stamp() as u64
            && (pipe_index != 2 || (status[2] != 0 && status[3] > status[2]))
            && queue_done && pipe_done && private_done {
            if let Work::Render(render) = &mut active.work {
                if crate::debug::debug_enabled(crate::debug::DebugFlags::KTraceCh) {
                    dev_info!(inner.drm.as_ref(),"G16G: userspace render stage {:?} retired stamps={:x?} queue={:?}\n",render.stage,status,queue);
                }
                if render.stage == crate::g16_render::Stage::Tiling {
                    render.stage = crate::g16_render::Stage::Fragment;
                    active.started = kernel::time::Instant::<kernel::time::Monotonic>::now();
                    return self.poll_job();
                }
                let timestamps = render.timestamps()?;
                gpu_span = [timestamps[0][0], timestamps[1][1]];
                let [[ta_start, ta_end], [fs_start, fs_end]] = timestamps;
                render_stage_ns = [if ta_start != 0 { ta_end as i64 - ta_start as i64 } else { 0 },
                    if fs_start != 0 && ta_end != 0 { fs_start as i64 - ta_end as i64 } else { 0 },
                    if fs_start != 0 { fs_end as i64 - fs_start as i64 } else { 0 }]
                    .map(|ticks| ticks * 1000 / 24);
                crate::cls_dev_dbg!(SubmitTiming, inner.drm, "G16G: render GPU timestamps={:x?} UMA completed={}/{}\n", timestamps, render.uma_completed()?, render.uma_submitted());
            } else {
                if let Work::Compute(compute) = &mut active.work {
                    gpu_span = [status[2], status[3]];
                    crate::cls_dev_dbg!(SubmitTiming, inner.drm, "G16G: compute completion status={:x?} UMA completed={}/{}\n", compute.status()?, compute.uma_completed()?, compute.uma_submitted());
                }
            }
            match &mut active.work {
                Work::Render(r) => r.record_consumption()?,
                Work::Compute(c) => c.record_consumption()?,
            }
            let active = inner.inflight.remove(0).map_err(|_| EIO)?;
            let active_ns = active.published.elapsed().as_nanos();
            if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
                dev_info!(inner.drm.as_ref(), "G16G_TIMING command job={} index={} pipe={} monotonic_ns={} prepare_ns={} active_ns={}\n",
                    active.packet.id, active.index, pipe_index,
                    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get(),
                    active.preparation_ns, active_ns);
                if let (Work::Render(render), crate::g16_submit::Payload::Render { command, .. }) =
                    (&active.work, &active.packet.commands[active.index].payload) {
                    let [asid, width, height] = render.profiling_key();
                    dev_info!(inner.drm.as_ref(), "G16G_PASS job={} index={} asid={} width={} height={} cs={:#x} gpu_start={} gpu_end={} ta_ns={} gap_ns={} fs_ns={}\n",
                        active.packet.id, active.index, asid, width, height,
                        command.vdm_base, gpu_span[0], gpu_span[1],
                        render_stage_ns[0], render_stage_ns[1], render_stage_ns[2]);
                }
            }
            let measured = gpu_span[0] != 0 && gpu_span[1] >= gpu_span[0];
            let gpu_ns = if measured { (gpu_span[1] - gpu_span[0]).saturating_mul(1000) / 24 } else { 0 };
            inner.state.health.timing.record(matches!(&active.work, Work::Render(_)),
                1, active.preparation_ns, active_ns, gpu_ns,
                [render_stage_ns[0], render_stage_ns[2], render_stage_ns[1]]
                    .map(|ns| ns.max(0) as u64));
            if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
                if let Some((previous, packet_id, previous_engine)) = inner.last_gpu_end {
                    if measured && gpu_span[0] >= previous {
                        let engine = usize::from(pipe_index == 2);
                        inner.summary.engine_gap_ticks[engine] += gpu_span[0] - previous;
                        inner.summary.engine_gap_count[engine] += 1;
                        let boundary = usize::from(packet_id != active.packet.id || active.index == 0);
                        let class = boundary * 4 + previous_engine * 2 + engine;
                        inner.summary.classified_gap_ticks[class] += gpu_span[0] - previous;
                        inner.summary.classified_gap_counts[class] += 1;
                        let publish = active.publication_tick;
                        if publish == 0 || publish > gpu_span[0] {
                            inner.summary.publication_invalid += 1;
                        } else if publish <= previous {
                            inner.summary.publication_gap_ticks[class][0] += gpu_span[0] - previous;
                        } else {
                            inner.summary.publication_gap_ticks[class][1] += publish - previous;
                            inner.summary.publication_gap_ticks[class][2] += gpu_span[0] - publish;
                        }
                    } else if measured {
                        inner.summary.gpu_overlaps += 1;
                    }
                }
                inner.last_gpu_end = measured.then_some((gpu_span[1], active.packet.id, usize::from(pipe_index == 2)));
                inner.summary.record(matches!(&active.work, Work::Render(_)),
                    active.preparation_ns, active_ns, gpu_ns);
                if let Work::Render(r) = &active.work {
                    let key = r.profiling_key();
                    if let Some(g) = inner.summary.geometry.iter_mut().find(|g| g.count == 0 || g.key == key) {
                        g.key = key;
                        g.count += 1;
                        g.preparation_ns += active.preparation_ns.max(0) as u64;
                        g.active_ns += active_ns.max(0) as u64;
                        let ns = render_stage_ns[2].max(0) as u64;
                        g.fragment_ns += ns;
                        g.max_fragment_ns = g.max_fragment_ns.max(ns);
                    } else {
                        inner.summary.geometry_overflow += 1;
                    }
                    for (sum, value) in inner.summary.render_stage_ns.iter_mut().zip(render_stage_ns) {
                        *sum += value;
                    }
                    for (sum, value) in inner.summary.render_clear.iter_mut().zip(r.clear_metrics) {
                        *sum += value;
                    }
                }
            }
            let cleanup = kernel::time::Instant::<kernel::time::Monotonic>::now();
            inner.gpu_pending = !inner.inflight.is_empty();
            inner.state.health.set_gpu_pending(inner.gpu_pending);
            if inner.inflight.is_empty() {
                if let Some(start) = inner.busy_since_ns.take() {
                    let end = <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get();
                    inner.summary.packet_active_ns += (end - start).max(0) as u64;
                }
            }
            let packet = active.packet.clone();
            let index = active.index + 1;
            match active.work {
                Work::Render(render) => {
                    let replaced = inner.retired_renders.iter().position(|(previous, _)|
                        previous.pool_slot() == render.pool_slot());
                    if let Some(position) = replaced {
                        let (previous, cache) = inner.retired_renders.remove(position).map_err(|_| EIO)?;
                        let bytes = previous.resident_bytes();
                        if inner.idle_caches.prepare(crate::g16_submit::IdleCache::Render(cache.clone()), bytes) {
                            if let Some(cache) = Option::as_mut(&mut *cache.lock()) {
                                cache.put(previous, bytes);
                            }
                        }
                    }
                    inner.retired_renders.push((render, packet.render_cache.clone()), GFP_KERNEL)
                        .expect("reserved render retirement capacity");
                }
                Work::Compute(compute) => {
                    let replaced = inner.retired_computes.iter().position(|(previous, _)|
                        previous.pool_slot() == compute.pool_slot());
                    if let Some(position) = replaced {
                        let (previous, cache) = inner.retired_computes.remove(position).map_err(|_| EIO)?;
                        let bytes = previous.resident_bytes();
                        if inner.idle_caches.prepare(crate::g16_submit::IdleCache::Compute(cache.clone()), bytes) {
                            if let Some(cache) = Option::as_mut(&mut *cache.lock()) {
                                cache.put(previous, bytes);
                            }
                        }
                    }
                    inner.retired_computes.push((compute, packet.compute_cache.clone()), GFP_KERNEL)?;
                }
            }
            inner.last_retired = Some((packet.id,
                <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get()));
            if index == packet.commands.len() {
                if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
                    let tick = physical_counter();
                    if tick >= gpu_span[1] && gpu_span[1] != 0 {
                        let delta = tick - gpu_span[1];
                        let totals = &mut inner.summary.final_retirement_ticks;
                        totals[0] += 1;
                        totals[1] += delta;
                        totals[2] = totals[2].max(delta);
                    } else { inner.summary.publication_invalid += 1; }
                }
                crate::cls_dev_dbg!(SubmitTiming, inner.drm, "G16G: userspace GPU job {} retired ({} commands) monotonic_ns={} last_prepare_ns={} last_active_ns={} last_cleanup_ns={} last_gpu_ns={} messages={} waits={} late={} timeouts={}\n", packet.id, index, <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get(), active.preparation_ns, active_ns, cleanup.elapsed().as_nanos(), gpu_ns, inner.state.event_messages.load(core::sync::atomic::Ordering::Acquire), inner.waits, inner.late_retirements, inner.wait_timeouts);
                inner.state.health.record_completion();
                packet.finish(Ok(()));
                if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
                    inner.summary.packets[index] += 1;
                }
                if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
                    let now = <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get();
                    if now - inner.summary.since_ns >= 1_000_000_000 {
                        let s = &inner.summary;
                        dev_info!(inner.drm.as_ref(), "G16G: idle owner cache bytes_hits_search_misses_puts_evictions_oversized_global_rejects={:?}\n",
                            crate::g16_owner_cache::statistics());
                        dev_info!(inner.drm.as_ref(), "G16G: userspace GPU job {} retired ({} commands) monotonic_ns={} summary_span_ns={} render_commands={} compute_commands={} render_prepare_ns={} compute_prepare_ns={} render_active_ns={} compute_active_ns={} render_gpu_ns={} compute_gpu_ns={} render_private_clear_bytes={} render_private_clear_ns={} render_clear_ns={} render_tiling_ns={} render_stage_gap_ns={} render_fragment_ns={}\n",
                            packet.id, index, now, now - s.since_ns,
                            s.commands[0], s.commands[1], s.preparation_ns[0], s.preparation_ns[1],
                            s.active_ns[0], s.active_ns[1], s.gpu_ns[0], s.gpu_ns[1],
                            s.render_clear[0], s.render_clear[1], s.render_clear[2],
                            s.render_stage_ns[0], s.render_stage_ns[1], s.render_stage_ns[2]);
                        dev_info!(inner.drm.as_ref(), "G16G: batch summary monotonic_ns={} packet_counts={:?} packet_active_ns={} union=1\n",
                            now, s.packets, s.packet_active_ns);
                        dev_info!(inner.drm.as_ref(), "G16G: completion wait totals monotonic_ns={} messages={} waits={} late={} timeouts={} geometry_overflow={} consecutive_gap_ns={} consecutive_gap_count={}\n",
                            now, inner.state.event_messages.load(core::sync::atomic::Ordering::Acquire),
                            inner.waits, inner.late_retirements, inner.wait_timeouts, s.geometry_overflow,
                            s.consecutive_gap_ns, s.consecutive_gap_count);
                        dev_info!(inner.drm.as_ref(), "G16G: GPU timeline summary monotonic_ns={} pstate_samples={:?} gap_ticks={:?} gap_counts={:?} overlaps={}\n",
                            now, s.pstate_samples, s.engine_gap_ticks,
                            s.engine_gap_count, s.gpu_overlaps);
                        dev_info!(inner.drm.as_ref(), "G16G: GPU boundary summary monotonic_ns={} gap_ticks={:?} gap_counts={:?}\n",
                            now, s.classified_gap_ticks, s.classified_gap_counts);
                        dev_info!(inner.drm.as_ref(), "G16G: GPU publication summary monotonic_ns={} gap_ticks={:?} final_retire={:?} invalid={}\n",
                            now, s.publication_gap_ticks, s.final_retirement_ticks, s.publication_invalid);
                        for (pipe, counts) in s.wait_phases.iter().enumerate() {
                            dev_info!(inner.drm.as_ref(), "G16G: completion phase samples monotonic_ns={} pipe={} before_start={} executing={} end_before_stamps={} stamps_before_queue={} queue_before_pipe={} missing_compute_timestamps={}\n",
                                now, pipe, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
                        }
                        for g in s.geometry.iter().filter(|g| g.count != 0) {
                            dev_info!(inner.drm.as_ref(), "G16G: render geometry summary monotonic_ns={} asid={} width={} height={} commands={} prepare_ns={} active_ns={} fragment_ns={} max_fragment_ns={}\n",
                                now, g.key[0], g.key[1], g.key[2], g.count,
                                g.preparation_ns, g.active_ns, g.fragment_ns, g.max_fragment_ns);
                        }
                        inner.summary = SubmitSummary { since_ns: now, ..Default::default() };
                    }
                }
            }
            self.pump()?;
            return Ok(true);
        }
        if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
            let phase = if status[0] == active.work.stamp() as u64 &&
                           status[1] == active.work.stamp() as u64 {
                if !queue_done { 3 } else if !pipe_done { 4 } else { 5 }
            } else {
                let [start, end] = match &mut active.work {
                    Work::Compute(_) => [status[2], status[3]],
                    Work::Render(r) => r.timestamps()?[pipe_index],
                };
                if end != 0 { 2 } else if start != 0 { 1 } else { 0 }
            };
            inner.summary.wait_phases[pipe_index][phase] += 1;
        }
        if snapshot_after_deadline {
            dev_err!(inner.drm.as_ref(), "G16G: userspace GPU timeout job={} status={:x?} queue={:?} pipe={:?}\n",
                active.packet.id, status, queue, pipe);
            let _ = inner.device.log_engine_state();
            let _ = match &mut active.work {
                Work::Compute(c) => c.log_progress(&inner.drm),
                Work::Render(r) => r.log_progress(&inner.drm),
            };
            let _ = inner.config.log_firmware_state(&inner.drm);
            return Err(ETIMEDOUT);
        }
        Ok(false)
    }

    #[cfg(CONFIG_DEV_COREDUMP)]
    fn capture_fault(&mut self, primary: Error) {
        let inner = &mut *self.inner;
        if inner.fault_captured { return; }
        inner.fault_captured = true;
        dev_err!(inner.drm.as_ref(), "G16G: first runtime error errno={}\n", primary.to_errno());
        if let Err(error) = (|| -> Result {
            let job = inner.inflight.first().map_or(0, |active| active.packet.id);
            let mut dump = inner.fault_reserve.take().ok_or(ENOMEM)?;
            dump.begin(job);
            let mut cause = [0u8; 32];
            cause[..4].copy_from_slice(&1u32.to_le_bytes()); // cause ABI version
            cause[4..8].copy_from_slice(&primary.to_errno().to_le_bytes());
            cause[8..16].copy_from_slice(&(inner.inflight.len() as u64).to_le_bytes());
            cause[16..24].copy_from_slice(&(inner.pending.len() as u64).to_le_bytes());
            cause[24..32].copy_from_slice(&u64::from(inner.gpu_pending).to_le_bytes());
            dump.record("host-first-error", 0, cause.len(), |out| {
                out.copy_from_slice(&cause); Ok(())
            })?;
            inner.config.capture_fault(&mut dump)?;
            if inner.device.firmware().board.chip_id == 0x8122 && primary == ETIMEDOUT
                && inner.inflight.is_empty() && !inner.gpu_pending {
                dev_info!(inner.drm.as_ref(), "G18_PROBE: preserved timeout RAM; requesting RTKit EP1 0x22 dump-and-stop\n");
                match Pin::new(&mut inner.transport).send_message(1, 0x22) {
                    Ok(()) => {
                        fsleep(Delta::from_millis(1000));
                        if let Err(error) = inner.config.capture_probe_data(&mut dump) {
                            dev_warn!(inner.drm.as_ref(), "G18_PROBE: RAM capture failed: {:?}\n", error);
                        }
                    }
                    Err(error) => dev_warn!(inner.drm.as_ref(), "G18_PROBE: send failed: {:?}\n", error),
                }
            }
            inner.queue_context.capture_fault(&mut dump)?;
            inner.queue.capture_fault(&mut dump, ["queue-ta", "ring-state-ta"])?;
            inner.fragment_queue.capture_fault(&mut dump,
                ["queue-fragment", "ring-state-fragment"])?;
            inner.compute_queue.capture_fault(&mut dump,
                ["queue-compute", "ring-state-compute"])?;
            if let Some(active) = inner.inflight.first_mut() {
                active.packet.notifications.lock().capture_fault(&mut dump)?;
                match &mut active.work {
                    Work::Render(render) => render.capture_fault(&mut dump)?,
                    Work::Compute(compute) => compute.capture_fault(&mut dump,
                        inner.uat.kernel_lower_vm())?,
                }
            }
            if let Some((compute, _)) = inner.retired_computes.last_mut() {
                compute.capture_retired_command(&mut dump)?;
            }
            dump.publish(inner.drm.as_ref())
        })() {
            dev_warn!(inner.drm.as_ref(), "G16G: firmware fault snapshot failed: {:?}\n", error);
        }
    }

    pub(crate) fn new(pdev: &platform::Device<Core>, device: Device) -> Result<Self> {
        device.require_stopped(pdev)?;
        let drm: driver::AsahiDevRef = kernel::drm::Device::new(
            pdev.as_ref(), driver::AsahiData::new(pdev, None, true))?;
        // SAFETY: Device exclusively owns ASC control and power. Construction
        // is stopped, and Runtime::drop stops ASC before dropping this UAT.
        let uat = unsafe { mmu::Uat::new_j613_25g83(&drm, device.firmware()) }?;
        let mut config = crate::g16_config::Config::new(pdev, &drm, uat.kernel_vm(), uat.kernel_lower_vm(), device.firmware())?;
        let queue_context = crate::g16_queue::Context::new(&drm, uat.kernel_vm())?;
        let queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), &queue_context, 256)?;
        let compute_queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), &queue_context, 256)?;
        let fragment_queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), &queue_context, 256)?;
        let compute = crate::g16_job::Compute::new(&drm, &uat, &mut config, compute_queue.address())?;
        let state = g16_rtkit::State::new(pdev, drm.clone(), device.firmware().resources.regions[5])?;
        let transport = rtkit::RtKit::new(pdev.as_ref(), None, 0, state.clone())?;
        #[cfg(CONFIG_DEV_COREDUMP)]
        let fault_reserve = match crate::g16_fault::Dump::new(0) {
            Ok(dump) => Some(dump),
            Err(error) => {
                dev_warn!(drm.as_ref(), "G16G: fault snapshot reserve unavailable: {:?}\n", error);
                None
            }
        };
        Ok(Self { inner: ManuallyDrop::new(Inner { transport, state, queue, compute_queue, fragment_queue, queue_context, fragment_registered:false, retired_renders:KVec::new(),retired_computes:KVec::new(), idle_caches:crate::g16_owner_cache::Registry::new(), compute, config, uat, drm, device, generation: 0, gpu_pending: false, #[cfg(CONFIG_DEV_COREDUMP)] fault_captured: false, #[cfg(CONFIG_DEV_COREDUMP)] fault_reserve, busy_since_ns: None, command_label: 1, render_stamp: 0, compute_stamp: crate::g16_compute::STAMP, inflight: KVec::new(), pending: KVec::new(), messages_at_wait: 0, waits: 0, late_retirements: 0, wait_timeouts: 0, last_retired: None, last_gpu_end: None, summary: SubmitSummary { since_ns: <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get(), ..Default::default() } }) })
    }

    pub(crate) fn boot(&mut self, pdev: &platform::Device<Core>) -> Result {
        let result = self.boot_inner(pdev);
        if let Err(error) = result { self.fail_all(error); }
        result
    }

    fn boot_inner(&mut self, pdev: &platform::Device<Core>) -> Result {
        self.inner.device.start_asc(pdev)?;
        Pin::new(&mut self.inner.transport).wake()?;
        if !Pin::new(&self.inner.transport).is_running() || !self.inner.state.healthy() {
            return Err(EIO);
        }
        for endpoint in [0x20, 0x21] {
            if !Pin::new(&mut self.inner.transport).has_endpoint(endpoint) { return Err(ENODEV); }
            Pin::new(&mut self.inner.transport).start_endpoint(endpoint)?;
        }
        fsleep(Delta::from_millis(100));
        if !self.inner.state.healthy() { return Err(EIO); }
        let mut roots = [(0, 0); 1];
        self.inner.uat.kernel_vm().context_roots(&mut roots)?;
        dev_info!(pdev.as_ref(), "G16G: RTKit IOP/AP running with owned common UAT, context0=[{:#x},{:#x}]; application endpoints started, no initdata sent\n", roots[0].0, roots[0].1);
        self.inner.device.check_pstate()?;
        let j613 = self.inner.device.firmware().board.chip_id == 0x8122;
        if j613 {
            self.inner.config.enqueue_j613_native_control(0x16, 0, 0)?;
            dev_info!(pdev.as_ref(), "G16G: J613 native pre-initdata control opcode=0x16 generation=0\n");
        } else {
            self.inner.device.set_power_generation(0)?;
            let inner = &mut *self.inner;
            inner.config.enqueue_control(0x1a, 0, &mut inner.generation, &inner.device)?;
        }
        self.inner.config.publish_loader()?;
        if let Some((before, after)) = self.inner.config.apply_trace_mask()? {
            dev_info!(pdev.as_ref(), "G16G: firmware trace mask {:#x} -> {:#x}\n", before, after);
        }
        let root = self.inner.config.root();
        dev_info!(pdev.as_ref(), "G16G: sending relocated GEM initdata root={:#x}\n", root);
        Pin::new(&mut self.inner.transport).send_message(0x20, 0x0081000000000000 | (root & ((1u64 << 44) - 1)))?;
        if self.inner.device.firmware().board.chip_id != 0x8122 {
            Pin::new(&mut self.inner.transport).send_message(0x21, 0x0083000000000011)?;
        }
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        loop {
            let inner = &mut *self.inner;
            inner.config.drain(&inner.drm)?;
            if !inner.state.healthy() { return Err(EIO); }
            if inner.config.ready()? {
                dev_info!(pdev.as_ref(), "G16G: firmware accepted relocated GEM initdata, control-ready=1; no GPU work queued\n");
                if let Some((before, after)) = inner.config.apply_trace_mask()? {
                    dev_info!(pdev.as_ref(), "G16G: firmware trace mask after ready {:#x} -> {:#x}\n", before, after);
                }
                break;
            }
            if start.elapsed() >= Delta::from_secs(5) { return Err(ETIMEDOUT); }
            fsleep(Delta::from_millis(10));
        }
        let handoff_start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        loop {
            match self.inner.uat.enable_g16_flushes() {
                Ok(()) => break,
                Err(_) if handoff_start.elapsed() < Delta::from_secs(1) => fsleep(Delta::from_millis(1)),
                Err(error) => return Err(error),
            }
        }
        if j613 {
            self.wait_control(1)?;
            for pipe in 0..3 {
                for kind in 1..=2 {
                    let next = self.inner.config.enqueue_j613_native_control(0x1b, pipe, kind)?;
                    Pin::new(&mut self.inner.transport).send_message(0x21, 0x0084000000000011)?;
                    self.wait_control(next)?;
                    dev_info!(pdev.as_ref(), "G16G: J613 native control retired opcode=0x1b pipe={} kind={} index={}\n", pipe, kind, next);
                }
            }
            let next = {
                let inner = &mut *self.inner;
                inner.config.enqueue_j613_idle_policy(&mut inner.generation, true)?
            };
            Pin::new(&mut self.inner.transport).send_message(0x21, 0x0084000000000011)?;
            self.wait_control(next)?;
            dev_info!(pdev.as_ref(), "G16G: J613 idle power-off enabled opcode=0xa value=1 index={}\n", next);
            if *crate::module_parameters::g16_qualify.value() == 0 {
                return Ok(());
            }
            let inner = &mut *self.inner;
            Pin::new(&mut inner.transport).send_message(0x21, 0x008300000000000a)?;
            fsleep(Delta::from_millis(100));
            inner.config.drain(&inner.drm)?;
            let before = inner.device.check_pstate()?;
            Pin::new(&mut inner.transport).send_message(0x21, 0x0087000000000010)?;
            let idle_start = kernel::time::Instant::<kernel::time::Monotonic>::now();
            let mut off_since = None;
            loop {
                inner.config.drain(&inner.drm)?;
                if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
                let state = inner.device.check_pstate()?;
                if state == 0 {
                    let off = off_since.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                    if off.elapsed() >= Delta::from_millis(50) {
                        dev_info!(pdev.as_ref(), "G16G: J613 native empty-channel wake/idle passed pstate={}->0 indices={:?} generation={}/{}; continuing to barrier\n",
                            before, inner.config.control_indices()?, inner.config.consumed_generation()?, inner.generation);
                        break;
                    }
                } else { off_since = None; }
                if idle_start.elapsed() >= Delta::from_secs(2) {
                    dev_err!(pdev.as_ref(), "G16G: J613 native idle timeout pstate={} before={} generation={}/{}\n",
                        state, before, inner.config.consumed_generation()?, inner.generation);
                    inner.config.log_firmware_state(&inner.drm)?;
                    return Err(ETIMEDOUT);
                }
                fsleep(Delta::from_millis(10));
            }
            self.check_barrier()?;
            if !self.inner.device.firmware().board.compute_self_test { return Err(ENOTSUPP); }
            let next = {
                let inner = &mut *self.inner;
                inner.config.enqueue_j613_idle_policy(&mut inner.generation, false)?
            };
            Pin::new(&mut self.inner.transport).send_message(0x21, 0x0084000000000011)?;
            self.wait_control(next)?;
            dev_info!(pdev.as_ref(), "G16G: J613 qualification clock-read hold idle_off=0 index={}\n", next);
            self.check_compute()?;
            let next = {
                let inner = &mut *self.inner;
                inner.config.enqueue_j613_idle_policy(&mut inner.generation, true)?
            };
            Pin::new(&mut self.inner.transport).send_message(0x21, 0x0084000000000011)?;
            self.wait_control(next)?;
            let idle_start = kernel::time::Instant::<kernel::time::Monotonic>::now();
            let mut off_since = None;
            loop {
                let inner = &mut *self.inner;
                inner.config.drain(&inner.drm)?;
                if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
                let state = inner.device.check_pstate()?;
                if state == 0 {
                    let off = off_since.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                    if off.elapsed() >= Delta::from_millis(50) {
                        dev_info!(pdev.as_ref(), "G16G: J613 post-load idle passed pstate=0 idle_off=1 generation={}/{}\n", inner.config.consumed_generation()?, inner.generation);
                        break;
                    }
                } else { off_since = None; }
                if idle_start.elapsed() >= Delta::from_secs(2) { return Err(ETIMEDOUT); }
                fsleep(Delta::from_millis(1));
            }
            dev_info!(pdev.as_ref(), "G16G: J613 compute self-test complete; continuing to DRM registration\n");
            return Ok(());
        }
        self.wait_control(1)?;
        self.send_control(0x34)?;
        self.inner.config.rearm_scheduler()?;
        self.send_control(0x0a)?;
        Pin::new(&mut self.inner.transport).send_message(0x21, 0x0083000000000010)?;
        fsleep(Delta::from_millis(100));
        let inner = &mut *self.inner;
        inner.config.drain(&inner.drm)?;
        inner.device.check_pstate()?;
        if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
        dev_info!(pdev.as_ref(), "G16G: device-control NOP, RIARTT and idle-power policy consumed; firmware ready, performance-state ceiling verified\n");
        inner.device.log_engine_state()?;
        inner.config.log_firmware_state(&inner.drm)?;
        self.check_barrier()?;
        if !self.inner.device.firmware().board.compute_self_test {
            dev_info!(pdev.as_ref(), "G16G: {}: compute self-test skipped (G16 shader); barrier passed\n",
                self.inner.device.firmware().board.name);
            return Ok(());
        }
        self.check_compute()
    }

    fn check_barrier(&mut self) -> Result {
        let inner = &mut *self.inner;
        let queue = inner.queue.prepare_barrier()?;
        let next = inner.config.publish_queue(0, queue, 1, 0, true)?;
        Pin::new(&mut inner.transport).send_message(0x21, 0x0083000000000000)?;
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        let mut completed = None;
        loop {
            let inner = &mut *self.inner;
            inner.config.drain(&inner.drm)?;
            if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
            inner.device.check_pstate()?;
            let pipe = inner.config.pipe_indices(0)?;
            let queue = inner.queue.status()?;
            if pipe == [next; 3] && queue == [1, 1, 1, 1, 1, 0] && inner.queue_context.registered()? {
                let completion = completed.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                if completion.elapsed() >= Delta::from_millis(50) {
                    dev_info!(inner.drm.as_ref(), "G16G: owned GEM queue barrier retired, pipe={:?} queue={:?}, context registered, firmware ready\n", pipe, queue);
                    return Ok(());
                }
            } else if completed.is_some() { return Err(EIO); }
            if start.elapsed() >= Delta::from_secs(2) {
                dev_err!(inner.drm.as_ref(), "G16G: queue barrier timeout pipe={:?} queue={:?}\n", pipe, queue);
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_millis(10));
        }
    }

    fn check_compute(&mut self) -> Result {
        use crate::g16_compute::{SLOT, STAMP};
        let inner = &mut *self.inner;
        inner.device.check_pstate()?;
        inner.device.log_engine_state()?;
        dev_info!(inner.drm.as_ref(), "G16G: compute publication power-generation={}/{}\n", inner.config.consumed_generation()?, inner.generation);
        let mut roots = [(0, 0); 1];
        inner.uat.kernel_vm().context_roots(&mut roots)?;
        dev_info!(inner.drm.as_ref(), "G16G: pre-compute context0 roots={:x?}\n", roots);
        inner.config.log_firmware_state(&inner.drm)?;
        inner.compute.log_context(&inner.drm)?;
        inner.state.health.set_gpu_pending(true);
        inner.gpu_pending = true;
        let queue = inner.compute_queue.prepare_command(inner.compute.address())?;
        let next = inner.config.publish_queue(2, queue, 1, SLOT, true)?;
        Pin::new(&mut inner.transport).send_message(0x21, 0x0083000000000002)?;
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        let mut completed = None;
        let capture_clock = inner.device.firmware().board.chip_id == 0x8122;
        let mut clock_samples = [[0u32; 4]; 32];
        let mut clock_counts = [0u32; 32];
        let mut clock_used = 0usize;
        let mut clock_overflow = 0u32;
        let mut last_clock_state = u32::MAX;
        let mut transitions = [(0u64, [0u32; 4]); 64];
        let mut transition_used = 0usize;
        let mut transition_overflow = 0u32;
        loop {
            let inner = &mut *self.inner;
            if capture_clock {
                let sample = inner.device.clock_state()?;
                if sample[0] != last_clock_state {
                    if transition_used < transitions.len() {
                        transitions[transition_used] = (start.elapsed().as_nanos() as u64, sample);
                        transition_used += 1;
                    } else { transition_overflow += 1; }
                    last_clock_state = sample[0];
                }
                if let Some(index) = clock_samples[..clock_used].iter().position(|seen| *seen == sample) {
                    clock_counts[index] += 1;
                } else if clock_used < clock_samples.len() {
                    clock_samples[clock_used] = sample;
                    clock_counts[clock_used] = 1;
                    clock_used += 1;
                } else { clock_overflow += 1; }
            }
            let status = inner.compute.status()?;
            let pipe = inner.config.pipe_indices(2)?;
            let queue = inner.compute_queue.status()?;
            if let Err(error) = inner.config.drain(&inner.drm) {
                dev_err!(inner.drm.as_ref(), "G16G: compute firmware error pipe={:?} queue={:?} status={:x?}\n", pipe, queue, status);
                if capture_clock { inner.device.log_clock_census(&clock_samples[..clock_used], &clock_counts[..clock_used], clock_overflow); inner.device.log_clock_transitions(&transitions[..transition_used], transition_overflow); }
                return Err(error);
            }
            if !inner.state.healthy() || !inner.config.ready()? {
                if capture_clock { inner.device.log_clock_census(&clock_samples[..clock_used], &clock_counts[..clock_used], clock_overflow); inner.device.log_clock_transitions(&transitions[..transition_used], transition_overflow); }
                return Err(EIO);
            }
            inner.device.check_pstate()?;
            if pipe == [next; 3] && queue == [1, 1, 1, 1, 1, 0]
                && status[0] == STAMP as u64 && status[1] == STAMP as u64
                && status[2] != 0 && status[3] > status[2]
                && status[9] == crate::g16_dispatch::VALUE as u64 {
                let completion = completed.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                if completion.elapsed() >= Delta::from_millis(50) {
                    dev_info!(inner.drm.as_ref(), "G16G: bounded compute timing host_ns={} raw_gpu_ticks={} (timestamp timebase not assumed)\n", start.elapsed().as_nanos(), status[3] - status[2]);
                    inner.config.log_firmware_state(&inner.drm)?;
                    inner.gpu_pending = false;
                    inner.state.health.set_gpu_pending(false);
                    dev_info!(inner.drm.as_ref(), "G16G: owned compute store retired, stamps/timestamps={:x?} queue={:?}, firmware ready\n", status, queue);
                    if capture_clock { inner.device.log_clock_census(&clock_samples[..clock_used], &clock_counts[..clock_used], clock_overflow); inner.device.log_clock_transitions(&transitions[..transition_used], transition_overflow); }
                    return Ok(());
                }
            } else if completed.is_some() { return Err(EIO); }
            if start.elapsed() >= Delta::from_secs(2) {
                dev_err!(inner.drm.as_ref(), "G16G: compute timeout pipe={:?} queue={:?} status={:x?}\n", pipe, queue, status);
                if capture_clock { inner.device.log_clock_census(&clock_samples[..clock_used], &clock_counts[..clock_used], clock_overflow); inner.device.log_clock_transitions(&transitions[..transition_used], transition_overflow); }
                inner.config.log_firmware_state(&inner.drm)?;
                inner.compute.log_progress(&inner.drm)?;
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_micros(100));
        }
    }

    fn send_control(&mut self, opcode: u32) -> Result {
        let inner = &mut *self.inner;
        let next = inner.config.enqueue_control(opcode, 0, &mut inner.generation, &inner.device)?;
        Pin::new(&mut inner.transport).send_message(0x21, 0x0084000000000011)?;
        self.wait_control(next)
    }

    fn wait_control(&mut self, next: u32) -> Result {
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        let mut completed = None;
        let mut kicked = false;
        loop {
            let inner = &mut *self.inner;
            inner.config.drain(&inner.drm)?;
            inner.device.check_pstate()?;
            if !kicked && completed.is_none() && start.elapsed() >= Delta::from_millis(300) {
                kicked = true;
                let indices = inner.config.control_indices()?;
                dev_info!(inner.drm.as_ref(), "G16G: device-control {} not retired after 300 ms, indices={:?}; ringing 0x84 doorbell\n", next, indices);
                Pin::new(&mut inner.transport).send_message(0x21, 0x0084000000000011)?;
            }
            if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
            let indices = inner.config.control_indices()?;
            if indices == [next; 3] {
                let completion = completed.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                if completion.elapsed() >= Delta::from_millis(50) {
                    dev_info!(inner.drm.as_ref(), "G16G: device-control retired index={} power-generation={}/{}\n",
                        next, inner.config.consumed_generation()?, inner.generation);
                    return Ok(());
                }
            } else if completed.is_some() { return Err(EIO); }
            if start.elapsed() >= Delta::from_secs(4) {
                dev_err!(inner.drm.as_ref(), "G16G: device-control timeout indices={:?} expected={} power-generation={}/{}\n",
                    indices, next, inner.config.consumed_generation()?, inner.generation);
                inner.config.log_control_publication(&inner.drm)?;
                inner.device.log_power_handshake()?;
                inner.config.log_firmware_state(&inner.drm)?;
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_millis(10));
        }
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        if !self.inner.inflight.is_empty() || !self.inner.pending.is_empty() {
            self.fail_all(ENODEV);
        }
        self.inner.state.health.mark_failed();
        if self.inner.gpu_pending {
            // Stopping ASC alone does not stop GPU DMA. Keep the complete
            // transport, mappings, GEM objects and PMP vote until reboot.
            // The leaked callback graph must pin the module's code too.
            // SAFETY: We are executing within this live module; the reference
            // is intentionally retained with Inner until system reset.
            unsafe { kernel::bindings::__module_get(crate::THIS_MODULE.as_ptr()) };
            let _ = self.inner.device.log_engine_state();
            let _ = self.inner.device.stop_asc();
            dev_err!(self.inner.drm.as_ref(), "G16G: GPU retirement unproven; retaining runtime until reboot\n");
            return;
        }
        if let Err(error) = self.inner.device.stop_asc() {
            // SAFETY: same retained callback/code lifetime as the DMA case.
            unsafe { kernel::bindings::__module_get(crate::THIS_MODULE.as_ptr()) };
            dev_err!(self.inner.drm.as_ref(), "G16G: ASC failed to stop; retaining runtime backing ({:?})\n", error);
            return;
        }
        // SAFETY: no GPU work is pending, and stop_asc succeeded above.
        unsafe { self.inner.uat.disarm_g16_flushes_after_stop() };
        dev_info!(self.inner.drm.as_ref(), "G16G: ASC stopped before RTKit transport cleanup\n");
        // SAFETY: ASC is stopped and any submitted engine work retired.
        // Incomplete or failed jobs retain Inner above. Callbacks drain before UAT and power are released.
        unsafe { ManuallyDrop::drop(&mut self.inner) };
    }
}

fn physical_counter() -> u64 {
    let tick: u64;
    let frequency: u64;
    // SAFETY: Architectural counter/frequency reads at EL1 on admitted J613.
    unsafe { core::arch::asm!("mrs {f}, cntfrq_el0", "mrs {t}, cntpct_el0",
        f = out(reg) frequency, t = out(reg) tick,
        options(nomem, nostack, preserves_flags)) };
    let frequency = frequency & 0xffff_ffff;
    if frequency == 0 { return 0; }
    (tick / frequency) * 24_000_000 + (tick % frequency) * 24_000_000 / frequency
}
