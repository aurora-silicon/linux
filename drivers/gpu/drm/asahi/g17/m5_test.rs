// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The M5 arithmetic qualification (linux-aurora t6050-gpu's g17/m5_test.rs): one retained
//! compute queue runs a fixed shader ([`super::m5_test_program`]) that computes `21 * i + 5`
//! from `7 * i`, and each dispatch must complete exactly: inner cursors, descriptor stamp,
//! work-state and pool counters, the shared completion record and every output.
//!
//! Three phases: one dispatch; enough more to wrap every ring the queue publishes into; then
//! work -> idle -> work cycles, each dispatch after the firmware powered the cores off by its own
//! policy. Work wakes the cores itself ([`super::port::Port::publish_m5_compute`]); no
//! idle-policy control is sent.
//!
//! It is not DRM submission, but it runs on the channel DRM compute submission uses
//! ([`super::queue::m5::Channel`]): the same construction, installation, publication and
//! retirement predicate, with the descriptor profile of [`super::Config::m5`]. The fixture
//! supplies the user pages (shader, pipeline, CDM stream, argument page and buffers) a client
//! binds. The firmware session retains the whole graph once [`Compute::run`] started publishing.

use super::{
    context::{Context, WorkStateLease},
    fw::{self, m5::compute, queue::*},
    initdata::COMPUTE_COMPLETION_SLOTS,
    job::Engine,
    m5_test_program as program, m5_vm,
    object::{Allocator, CpuMap, KernelObject, Placement},
    queue::compute::retirement::Batch,
    queue::m5::{Channel, Dispatch, Host, Published},
    Firmware, Role, Shared,
};
use crate::{driver::AsahiDrmDevice, mmu};
use core::sync::atomic::{fence, Ordering};
use kernel::{
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, Delta},
};

const PAGE: usize = mmu::UAT_PGSZ;
/// Preferred QID; a diagnostic choice, not GPU topology.
const QID: u8 = 11;
/// Dispatches of the repeat phase: one lap of the largest ring a dispatch advances, the shared
/// completion ring, plus one kick ring. Each dispatch takes one kick entry, one outer work slot,
/// one inner item and one completion record, so every ring wraps at least once.
const REPEAT_DISPATCHES: u32 = (COMPUTE_COMPLETION_SLOTS + fw::kick::KICK_SLOTS) as u32;
const _: () = assert!(
    REPEAT_DISPATCHES > ITEM_RING_ENTRIES
        && REPEAT_DISPATCHES > fw::channels::WORK_RING_SLOTS
        && REPEAT_DISPATCHES as usize > fw::kick::KICK_SLOTS
);
/// Work -> idle -> work cycles of the idle phase.
const IDLE_CYCLES: u32 = 8;
/// Remaps of the remap phase: each replaces the buffers page at the same VA with fresh backing
/// and runs one dispatch, so every pair of clusters (they rotate per dispatch) meets a remapped page
/// twice.
const REMAPS: u32 = 8;
/// What each remap adds to every input, so a stale translation's results show.
const REMAP_BIAS: u32 = 1000;
/// Polls of 1 ms for the firmware to go idle.
const IDLE_POLLS: u32 = 5000;
/// Status A word that each firmware role clears once it has powered the cores off: the
/// GPU is powered down only once both read zero.
const POWER_STATE: usize = 0x10;
/// Status A word that counts each firmware role's power-ups; it advances after a power request.
/// On J775D it stays 0 through back-to-back work and advances once per work -> idle -> work cycle.
const POWER_SEQUENCE: usize = 0x1c;
/// Label of the diagnostic VM; VM IDs only name VMs in diagnostics.
const VM_LABEL: u64 = 1;
/// The fixture's user pages are in Mesa's USC window: they are the fixture's own mappings, not
/// kernel reservations.
const _: () = {
    let pages = [
        program::SHADER_PAGE,
        program::PIPELINE_PAGE,
        program::STREAM_PAGE,
        program::ARGUMENT_PAGE,
        program::BUFFER_PAGE,
    ];
    let mut index = 0;
    while index < pages.len() {
        assert!(m5_vm::USC.start <= pages[index] && pages[index] + PAGE as u64 <= m5_vm::USC.end);
        index += 1;
    }
};
/// Polls of 1 ms for each wait.
const WAIT_POLLS: usize = 2000;
/// Bytes of the fixture's CDM stream.
const STREAM_BYTES: u64 = 48;

/// The retained graph of the qualification: its channel and the fixture pages mapped into the
/// channel's VM.
pub(super) struct Compute {
    current_work: Option<Arc<WorkStateLease>>,
    channel: KBox<Channel>,
    objects: KVec<KernelObject>,
    buffers: usize,
    /// Added to every input of the buffers page now mapped: `7 * i + bias`.
    bias: u32,
    observations: Batch,
    /// Completion-ring control words of the last scan.
    seen: KVVec<u64>,
    laps: Laps,
}

/// What one retired dispatch left in the output buffer.
#[derive(Clone, Copy)]
struct Outputs {
    /// Outputs other than `21 * i + 5`.
    wrong: usize,
    /// The 64-thread workgroups that left an output at the sentinel.
    untouched: u16,
    /// Both roles' power words when the work was published.
    published: [[u32; 2]; 2],
}

/// The dispatches of one phase whose outputs were not exact.
#[derive(Default)]
struct Tally {
    inexact: u32,
    /// The first inexact dispatch, and the power words at its publication.
    first: Option<(u32, [[u32; 2]; 2])>,
    /// Workgroups any of them left untouched.
    untouched: u16,
}

impl Tally {
    fn add(&mut self, generation: u32, outputs: Outputs) {
        if outputs.wrong != 0 {
            self.inexact += 1;
            self.first.get_or_insert((generation, outputs.published));
            self.untouched |= outputs.untouched;
        }
    }
}

/// One wait for the firmware's idle.
struct Idle {
    /// Milliseconds until both roles reported the cores off, and until the host's idle path
    /// cleared its effort.
    off: u32,
    effort: Option<u32>,
    /// Both roles' power words when the wait began.
    before: [[u32; 2]; 2],
}

/// Where the rings a dispatch advances stand, and how often each wrapped.
#[derive(Default)]
struct Laps {
    /// The outer work ring, the inner item ring and the shared completion ring.
    position: [u32; 3],
    laps: [u32; 3],
}

impl Laps {
    const OUTER: usize = 0;
    const ITEMS: usize = 1;
    const COMPLETIONS: usize = 2;

    fn advance(&mut self, ring: usize, position: u32) {
        if position < self.position[ring] {
            self.laps[ring] += 1;
        }
        self.position[ring] = position;
    }
}

impl Compute {
    /// Builds the channel and the fixture pages. Nothing is visible to the firmware yet, so a
    /// failure here frees everything.
    pub(super) fn new(
        dev: &AsahiDrmDevice,
        firmware: &mut Firmware,
        shared: &Arc<Shared>,
        gpu: compute::Gpu,
    ) -> Result<KBox<Self>> {
        let uat = firmware.uat.clone();
        let alloc = Allocator {
            dev,
            uat: &uat,
            layout: firmware.variant.vm_layout,
        };
        // An ordinary M5 client VM with the driver reservation Mesa requests, so the
        // qualification runs on the layout DRM clients get.
        let vm = m5_vm::client_vm(&uat, VM_LABEL, m5_vm::DRIVER_RESERVATION)?;
        let execution = uat.new_execution_context(&vm)?;
        let context = Arc::new(
            Context::new(
                &alloc,
                &firmware.pool,
                &firmware.queues.qos_ids,
                execution,
                0,
                fw::m5::queue::MEDIUM,
                shared.renders.clone(),
                shared.progress.clone(),
            )?,
            GFP_KERNEL,
        )?;
        let owner = firmware.queues.reserve_identity()?;
        let id = firmware
            .queues
            .qids
            .reserve(owner, DataMaster::Compute, Some(QID))?;
        let channel =
            match Channel::new(&alloc, context, id, owner, gpu, &firmware.variant.pointers) {
                Ok(channel) => channel,
                Err(error) => {
                    firmware.queues.qids.cancel_unpublished(id)?;
                    return Err(error);
                }
            };
        let vm = channel.context().vm();
        let cpu = CpuMap::WriteCombined;
        let mut objects = KVec::new();
        let mut buffers = None;
        for (address, kind) in [
            (program::SHADER_PAGE, program::PageKind::Shader),
            (program::PIPELINE_PAGE, program::PageKind::Pipeline),
            (program::STREAM_PAGE, program::PageKind::Stream),
            (program::ARGUMENT_PAGE, program::PageKind::Arguments),
            (program::BUFFER_PAGE, program::PageKind::Buffers),
        ] {
            let access = match kind {
                program::PageKind::Shader | program::PageKind::Stream => mmu::PROT_GPU_SHARED_RO,
                _ => mmu::PROT_GPU_SHARED_RW,
            };
            let mut object = KernelObject::new(dev, vm, Placement::At(address), PAGE, access, cpu)?;
            program::initialize_page(kind, object.bytes_mut()).map_err(|_| EINVAL)?;
            object.publish_to_contexts()?;
            if matches!(kind, program::PageKind::Buffers) {
                buffers = Some(objects.len());
            }
            objects.push(object, GFP_KERNEL)?;
        }
        dev_info!(
            dev.as_ref(),
            "M5 compute graph: context {}, queue {}\n",
            channel.context().id(),
            channel.qid()
        );
        Ok(KBox::new(
            Self {
                current_work: None,
                channel,
                objects,
                buffers: buffers.ok_or(EINVAL)?,
                bias: 0,
                observations: Batch::new()?,
                seen: KVVec::from_elem(0, COMPUTE_COMPLETION_SLOTS, GFP_KERNEL)?,
                laps: Laps::default(),
            },
            GFP_KERNEL,
        )?)
    }

    /// What the channel uses of the firmware session.
    fn host(firmware: &mut Firmware) -> Host<'_> {
        Host {
            init: &firmware.init,
            port: &firmware.port,
            qids: &mut firmware.queues.qids,
            crashed: &firmware.primary.state.shared.crashed,
        }
    }

    /// Both roles' power-state and power-sequence words in status A.
    fn power_words(firmware: &Firmware) -> Result<[[u32; 2]; 2]> {
        let mut words = [[0; 2]; 2];
        for (state, &role) in words.iter_mut().zip(&[Role::Primary, Role::Secondary]) {
            for (word, offset) in state.iter_mut().zip([POWER_STATE, POWER_SEQUENCE]) {
                *word = firmware
                    .init
                    .status_word(role, offset)?
                    .load(Ordering::Acquire);
            }
        }
        Ok(words)
    }

    /// Waits for the firmware to finish the inner items up to `target`.
    fn wait_inner(&self, firmware: &Firmware, target: u32) -> Result {
        for _ in 0..WAIT_POLLS {
            if firmware
                .primary
                .state
                .shared
                .crashed
                .load(Ordering::Acquire)
            {
                return Err(EIO);
            }
            if self.channel.cursors()?.consumer == target {
                return Ok(());
            }
            fsleep(Delta::from_millis(1));
        }
        Err(ETIMEDOUT)
    }

    /// Consumes both roles' events, trace and statistics records and returns how many events
    /// there were; a restart fails the run.
    fn drain(&self, dev: &AsahiDrmDevice, firmware: &mut Firmware, log: bool) -> Result<usize> {
        let device = dev.as_ref().as_ref();
        let mut restarted = false;
        let mut count = 0;
        for &role in firmware.roles() {
            let mut events = super::channel::Events::new(role, firmware.variant.graph);
            while let Some(event) = events.next(device, &firmware.init)? {
                restarted |= matches!(event, fw::channels::Event::Restart { .. });
                count += 1;
                if log {
                    dev_info!(dev.as_ref(), "M5 compute {:?} event {:?}\n", role, event);
                }
            }
            firmware.rings.drain_trace(device, &firmware.init, role);
            firmware
                .rings
                .drain_statistics(device, &firmware.init, role);
        }
        if restarted {
            return Err(EIO);
        }
        Ok(count)
    }

    /// Installs the queue and runs the three phases. The caller retains this graph whatever the
    /// result: from here on the firmware may read it.
    /// The qualified dispatch's resources, through the DRM path's plan: one storage-free
    /// 64-thread profile, which must give the words qualified on J714 (0x54b, PTC
    /// target 245).
    fn resources(&self) -> Result<compute::Resources> {
        let resources = self
            .channel
            .gpu()
            .plan(compute::Declared::threads(program::WORKGROUP_THREADS)?)?;
        if (resources.control, resources.policy) != (0x54b, 0xf5_0040_0020) {
            pr_err!(
                "M5 compute: planned {:#x}/{:#x}, not the qualified 0x54b/0xf500400020\n",
                resources.control,
                resources.policy
            );
            return Err(EINVAL);
        }
        Ok(resources)
    }

    pub(super) fn run(&mut self, dev: &AsahiDrmDevice, firmware: &mut Firmware) -> Result {
        if firmware.variant.submission != fw::variant::Submission::M5HostCommand {
            return Err(EINVAL);
        }
        self.channel.install(&mut Self::host(firmware))?;
        dev_info!(
            dev.as_ref(),
            "M5 compute queue installed: core power {:#x}, counters {:?}, power words {:x?}\n",
            firmware.bridge.regs.g17_core_power(),
            firmware.init.control_counters(),
            Self::power_words(firmware)
        );

        let first = self.dispatch(dev, firmware, 1)?;
        self.drain(dev, firmware, true)?;
        if first.wrong != 0 {
            return Err(EIO);
        }

        let (mut repeat, mut events) = (Tally::default(), 0);
        for generation in 2..=1 + REPEAT_DISPATCHES {
            let outputs = self.dispatch(dev, firmware, generation)?;
            repeat.add(generation, outputs);
            events += self.drain(dev, firmware, false)?;
        }
        let laps = &self.laps;
        dev_info!(
            dev.as_ref(),
            "M5 compute repeat: {} dispatches, {} inexact (first {:?}, power words {:x?} at its publication; untouched workgroups {:#06x}); {} kicks in all; outer ring at {} after {} laps, items at {} after {}, completions at {} after {}; {} events\n",
            REPEAT_DISPATCHES,
            repeat.inexact,
            repeat.first.map(|first| first.0),
            repeat.first.map(|first| first.1),
            repeat.untouched,
            self.channel.kicks(),
            laps.position[Laps::OUTER],
            laps.laps[Laps::OUTER],
            laps.position[Laps::ITEMS],
            laps.laps[Laps::ITEMS],
            laps.position[Laps::COMPLETIONS],
            laps.laps[Laps::COMPLETIONS],
            events
        );
        let wrapped = !laps.laps.contains(&0);

        // Work -> idle -> work: each dispatch waits for the firmware to report the cores off, and
        // each wakes them once.
        let sequence = |words: [[u32; 2]; 2]| [words[0][1], words[1][1]];
        let wakes = sequence(Self::power_words(firmware)?);
        let (mut woken, mut first_idle, mut off) = (Tally::default(), None, (u32::MAX, 0));
        let mut generation = 1 + REPEAT_DISPATCHES;
        for _ in 0..IDLE_CYCLES {
            let idle = self.wait_idle(firmware)?;
            off = (off.0.min(idle.off), off.1.max(idle.off));
            first_idle.get_or_insert(idle);
            generation += 1;
            let outputs = self.dispatch(dev, firmware, generation)?;
            woken.add(generation, outputs);
            self.drain(dev, firmware, false)?;
        }
        let after = sequence(Self::power_words(firmware)?);
        let cycled = after
            .iter()
            .zip(wakes)
            .all(|(after, before)| after.wrapping_sub(before) == IDLE_CYCLES);
        dev_info!(
            dev.as_ref(),
            "M5 compute wake: {} work -> idle -> work cycles, {} inexact (first {:?}, power words {:x?} at its publication; untouched workgroups {:#06x}); off {}..{} ms after work (first wait: power words {:x?} before it, host effort cleared at {:?} ms); power-ups {:?} -> {:?}; core power {:#x}, counters {:?}\n",
            IDLE_CYCLES,
            woken.inexact,
            woken.first.map(|first| first.0),
            woken.first.map(|first| first.1),
            woken.untouched,
            off.0,
            off.1,
            first_idle.as_ref().map(|idle| idle.before),
            first_idle.as_ref().and_then(|idle| idle.effort),
            wakes,
            after,
            firmware.bridge.regs.g17_core_power(),
            firmware.init.control_counters()
        );
        if !wrapped || !cycled || repeat.inexact != 0 || woken.inexact != 0 {
            return Err(EIO);
        }

        let (mut remapped, mut stale) = (Tally::default(), 0);
        for round in 1..=REMAPS {
            generation += 1;
            stale += self.remap(dev, round * REMAP_BIAS)?;
            let outputs = self.dispatch(dev, firmware, generation)?;
            remapped.add(generation, outputs);
            self.drain(dev, firmware, false)?;
        }
        stale += self.stale()?;
        dev_info!(
            dev.as_ref(),
            "M5 compute remap: {} times unmapped the buffers page at {:#x} and mapped fresh backing there, then dispatched: {} inexact (first {:?}; untouched workgroups {:#06x}), {} outputs written to unmapped backing\n",
            REMAPS,
            program::BUFFER_PAGE,
            remapped.inexact,
            remapped.first.map(|first| first.0),
            remapped.untouched,
            stale
        );
        if remapped.inexact != 0 || stale != 0 {
            return Err(EIO);
        }
        dev_info!(
            dev.as_ref(),
            "M5 compute PASS: {} dispatches of {} exact outputs (1, {} wrapping every ring, {} after a firmware idle, {} after a remap), with exact retirement, work-state and pool counters\n",
            generation,
            program::ELEMENTS,
            REPEAT_DISPATCHES,
            IDLE_CYCLES,
            REMAPS
        );
        Ok(())
    }

    /// Runs the host's idle path as the event worker would (it holds nothing at the firmware's
    /// own idle delay) and waits for both firmware roles to report the cores off. The idle
    /// policy is the firmware's own: no control may be sent meanwhile.
    fn wait_idle(&self, firmware: &mut Firmware) -> Result<Idle> {
        let before = Self::power_words(firmware)?;
        let counters = firmware.init.control_counters()?;
        let (mut effort, mut off) = (None, None);
        for ms in 0..IDLE_POLLS {
            if firmware
                .primary
                .state
                .shared
                .crashed
                .load(Ordering::Acquire)
            {
                return Err(EIO);
            }
            if effort.is_none() && firmware.idle_check(super::now_ns(), false)?.is_none() {
                effort = Some(ms);
            }
            let powered = Self::power_words(firmware)?.iter().any(|role| role[0] != 0);
            off = match (powered, off) {
                (true, _) => None,
                (false, None) => Some(ms),
                (false, since) => since,
            };
            if let (Some(_), Some(off)) = (effort, off) {
                if firmware.init.control_counters()? != counters {
                    return Err(EIO);
                }
                return Ok(Idle {
                    off,
                    effort,
                    before,
                });
            }
            fsleep(Delta::from_millis(1));
        }
        pr_warn!(
            "M5 compute idle: not off after {} ms: power words {:x?} -> {:x?}, host effort cleared at {:?} ms\n",
            IDLE_POLLS,
            before,
            Self::power_words(firmware),
            effort
        );
        Err(ETIMEDOUT)
    }

    /// Runs one dispatch and qualifies its retirement; returns what it left in the outputs,
    /// which the phase judges.
    fn dispatch(
        &mut self,
        dev: &AsahiDrmDevice,
        firmware: &mut Firmware,
        generation: u32,
    ) -> Result<Outputs> {
        let stamp_target = generation.checked_mul(0x100).ok_or(EOVERFLOW)?;
        let cursors = self.channel.cursors()?;
        if cursors.consumer != cursors.producer || self.channel.in_flight() {
            return Err(EBUSY);
        }
        let buffers = &self.objects[self.buffers];
        for index in 0..program::ELEMENTS {
            buffers
                .word(program::OUTPUT_OFFSET + index * 4)?
                .store(program::SENTINEL, Ordering::Relaxed);
        }
        if self.current_work.is_some() {
            return Err(EBUSY);
        }
        let work = Arc::new(
            WorkStateLease::deferred(self.channel.context().clone()),
            GFP_KERNEL,
        )?;
        work.acquire(Engine::Compute)?;
        self.current_work = Some(work.clone());
        let dispatch = Dispatch {
            cdm_va: program::STREAM_PAGE,
            cdm_end_va: program::STREAM_PAGE + STREAM_BYTES - 4,
            resources: self.resources()?,
        };
        let published_power = Self::power_words(firmware)?;
        let published = self.channel.publish(
            &mut Self::host(firmware),
            &dispatch,
            &work,
            u64::from(generation),
            0,
            None,
        )?;
        if published.pool_target != u64::from(generation) {
            return Err(EIO);
        }
        self.laps.advance(Laps::OUTER, published.outer);
        self.laps
            .advance(Laps::ITEMS, published.ticket.item_producer);
        firmware.note_submission()?;
        let mut waited = self.wait_inner(firmware, published.ticket.item_producer);
        if waited.is_ok() {
            waited = Err(ETIMEDOUT);
            for _ in 0..WAIT_POLLS {
                if firmware
                    .primary
                    .state
                    .shared
                    .crashed
                    .load(Ordering::Acquire)
                {
                    break;
                }
                if self.channel.stamp()? == stamp_target
                    && work.completed_kicks()? == 1
                    && self.channel.completed_submissions()? == published.pool_target
                {
                    waited = Ok(());
                    break;
                }
                fsleep(Delta::from_millis(1));
            }
        }
        let stamp = self.channel.stamp()?;
        let (wrong, untouched, changed, groups) = self.outputs()?;
        if generation == 1 || waited.is_err() || changed != 0 {
            let cursors = self.channel.cursors()?;
            dev_info!(
                dev.as_ref(),
                "M5 compute result {}: wait {:?}, stamp {:#x}, wrong {}, untouched {}, changed inputs {}, inner [{}, {}]\n",
                generation,
                waited,
                stamp,
                wrong,
                untouched,
                changed,
                cursors.consumer,
                cursors.producer
            );
        }
        if waited.is_err() {
            self.report_failure(dev, firmware, &work, &published)?;
        }
        waited?;
        if stamp != stamp_target || changed != 0 {
            return Err(EIO);
        }
        let outputs = Outputs {
            wrong,
            untouched: groups,
            published: published_power,
        };
        // Qualify the completion decoder and the channel's retirement predicate with the same
        // publication, before reusing any descriptor.
        self.observations.clear();
        if !self.channel.request(&mut self.observations)? || !self.observations.covered_pending() {
            return Err(EIO);
        }
        let observations = &mut self.observations;
        let next = firmware
            .init
            // The qualification's queue is class 2 (MEDIUM), which completes into ring 0.
            .resync_compute_completions(0, &mut self.seen, |record| {
                observations.fold_any(record)
            })?;
        self.laps.advance(Laps::COMPLETIONS, next.try_into()?);
        let mut retired = None;
        let port = &firmware.port;
        self.channel.retire(
            &mut self.observations,
            0,
            &mut |owner| port.qos_complete(&[owner]),
            &mut |kick| {
                retired = Some((kick.ticket, kick.result));
                Ok(())
            },
        )?;
        let Some((ticket, timestamps)) = retired else {
            return Err(EIO);
        };
        if ticket != published.ticket || self.channel.in_flight() {
            return Err(EIO);
        }
        let timestamps = timestamps?;
        if generation == 1 {
            dev_info!(
                dev.as_ref(),
                "M5 shared compute retirement: record {:?}; descriptor written to +{:#x}\n",
                timestamps,
                self.channel
                    .descriptor_extent(ticket.kick as usize % super::queue::m5::DEPTH)?
            );
        }
        self.current_work.take();
        Ok(outputs)
    }

    /// Replaces the buffers page with fresh backing at the same VA (a translation-retirement
    /// check): resets the current page's outputs to the sentinel, unmaps it through the normal
    /// path (unmap, then TLB invalidation of the VM's contexts), and maps a new page whose inputs
    /// are `7 * i + bias`. Returns how many outputs the GPU had written to the pages unmapped by
    /// earlier remaps, which it must not reach. Unmapped pages are kept until the graph is dropped.
    fn remap(&mut self, dev: &AsahiDrmDevice, bias: u32) -> Result<usize> {
        let stale = self.stale()?;
        let current = &mut self.objects[self.buffers];
        for i in 0..program::ELEMENTS {
            current
                .word(program::OUTPUT_OFFSET + i * 4)?
                .store(program::SENTINEL, Ordering::Relaxed);
        }
        drop(current.take_mapping()?);
        let mut fresh = KernelObject::new(
            dev,
            self.channel.context().vm(),
            Placement::At(program::BUFFER_PAGE),
            PAGE,
            mmu::PROT_GPU_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        program::initialize_page(program::PageKind::Buffers, fresh.bytes_mut())
            .map_err(|_| EINVAL)?;
        for i in 0..program::ELEMENTS {
            fresh
                .word(program::INPUT_OFFSET + i * 4)?
                .store(i as u32 * 7 + bias, Ordering::Relaxed);
        }
        fresh.publish_to_contexts()?;
        fence(Ordering::SeqCst);
        let unmapped = core::mem::replace(&mut self.objects[self.buffers], fresh);
        self.objects.push(unmapped, GFP_KERNEL)?;
        self.bias = bias;
        Ok(stale)
    }

    /// Outputs the GPU wrote to buffers pages after [`Self::remap`] unmapped them.
    fn stale(&self) -> Result<usize> {
        let mut stale = 0;
        for (index, object) in self.objects.iter().enumerate() {
            if index > self.buffers && object.gpu_va() == 0 {
                for i in 0..program::ELEMENTS {
                    let output = object
                        .word(program::OUTPUT_OFFSET + i * 4)?
                        .load(Ordering::Relaxed);
                    stale += usize::from(output != program::SENTINEL);
                }
            }
        }
        Ok(stale)
    }

    /// Counts the wrong, untouched (sentinel) outputs and changed inputs, with a mask of the
    /// 64-thread workgroups that left an output untouched.
    fn outputs(&self) -> Result<(usize, usize, usize, u16)> {
        let (mut wrong, mut untouched, mut changed, mut groups) = (0, 0, 0, 0u16);
        // Device memory reads follow all observed completion writes.
        // SAFETY: A load barrier has no memory operands.
        unsafe { core::arch::asm!("dmb oshld", options(nostack, preserves_flags)) };
        let buffers = &self.objects[self.buffers];
        for index in 0..program::ELEMENTS {
            let input = buffers
                .word(program::INPUT_OFFSET + index * 4)?
                .load(Ordering::Relaxed);
            let output = buffers
                .word(program::OUTPUT_OFFSET + index * 4)?
                .load(Ordering::Relaxed);
            changed += usize::from(input != index as u32 * 7 + self.bias);
            wrong += usize::from(output != (index as u32 * 7 + self.bias) * 3 + 5);
            if output == program::SENTINEL {
                untouched += 1;
                groups |= 1 << (index / 64);
            }
        }
        Ok((wrong, untouched, changed, groups))
    }

    /// Logs what a timed-out dispatch left: the raw fault window and queue configuration, both
    /// roles' events and reports, the firmware state, and this dispatch's counters and stamps.
    fn report_failure(
        &self,
        dev: &AsahiDrmDevice,
        firmware: &mut Firmware,
        work: &WorkStateLease,
        published: &Published,
    ) -> Result {
        let qid = self.channel.qid();
        dev_info!(
            dev.as_ref(),
            "M5 compute raw fault: {:x?}, queue configuration: {:x?}\n",
            firmware.bridge.regs.m5_fault_registers(),
            firmware.port.m5_queue_configurations([qid; 2])
        );
        if let Err(error) = self.drain(dev, firmware, true) {
            dev_info!(dev.as_ref(), "M5 compute event drain: {:?}\n", error);
        }
        firmware.init.dump_m5_failure(dev);
        dev_info!(
            dev.as_ref(),
            "M5 work state {:#x}, completed {:?}\n",
            work.word(),
            work.completed_kicks()
        );
        self.channel.report(dev, published);
        Ok(())
    }
}
