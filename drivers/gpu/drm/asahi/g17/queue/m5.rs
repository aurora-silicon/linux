// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 compute channels: one hardware compute queue (QID) and its graph, built on the M5 queue,
//! descriptor and pool encodings ([`crate::g17::fw::m5`]) and published through
//! [`Port::publish_m5_compute`]. DRM compute submission and the arithmetic qualification
//! (`g17/m5_test.rs`) both run work through a [`Channel`]: one construction, one installation,
//! one publication transaction ([`Channel::publish`]) and one retirement predicate
//! ([`Channel::retire`]).
//!
//! An installed channel stays with its firmware session: no M5 queue release is established, and
//! the M5 profile keeps every stopped session (`Variant::retain_stopped`). Its QID, execution
//! context and backing are never reused, and a failed kick keeps its command pinned.
//!
//! Retirement is exact and strict: the completion record of this queue, kick and
//! descriptor, the queue's stamp covering the kick, the inner consumer past the kick's items, the
//! pool's completed count at the kick's target, the work-state node drained and the record's end
//! time equal to the descriptor's, in the recovery generation the kick was published in, with
//! recovery closed. There is no aged-record or recordless retirement. These witnesses prove that
//! the firmware retired the kick, not that every workgroup ran: dispatches that lost workgroups
//! retired with all of them.

use super::compute::{GRAPH_SIZE, ITEMS, POINTERS};
use crate::g17::{
    context::{Context, WorkStateLease},
    freelist::M5ComputePool,
    fw::{
        self,
        kick::{KickArgs, KickQos, KickTimestamp},
        m5::{
            compute::{self, Descriptor, DescriptorArgs, Profile, RegisterArgs, Resources},
            queue::{HardwareSubmission, Notification},
        },
        queue::*,
        variant::PointerLayout,
    },
    initdata::InitData,
    job::Packet,
    kick,
    object::{Allocator, CpuMap, KernelObject},
    port::Port,
    qos,
    queue::compute::retirement::{self, Batch, Cursors, Observation, Recordless, Ticket},
    vm_layout::Domain,
};
use crate::{driver::AsahiDrmDevice, mmu};
use core::sync::atomic::{fence, AtomicBool, Ordering};
use kernel::{
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, Delta},
};

const PAGE: usize = mmu::UAT_PGSZ;
/// Kicks a channel holds in flight: the retirement batch's bound per QID. A descriptor and a
/// notification slot are reused only after the kick that last used them retired.
pub(crate) const DEPTH: usize = retirement::IN_FLIGHT;
const _: () = assert!(DEPTH <= fw::kick::KICK_SLOTS);

/// Queue graph: the graph ([`super::compute::initialize_m5_graph`]), then the records its items
/// name.
const GRAPH_BYTES: usize = 0x10000;
/// Initial queue configuration.
const CONFIG: usize = GRAPH_SIZE;
/// Context and pool update of the first kick.
const UPDATE: usize = 0x8200;
/// Current-kick notifications, one per in-flight slot.
const ANNOUNCES: usize = 0x9000;
const ANNOUNCE_STRIDE: usize = 0x40;
const _: () = assert!(ANNOUNCES + DEPTH * ANNOUNCE_STRIDE <= GRAPH_BYTES);
/// Descriptor ring: one descriptor per in-flight slot.
/// The firmware reaches past the descriptor's described prefix: with a 0x1000-byte stride the
/// last slot's kick ran (its outputs exact) but never completed, at the ring's guard page,
/// though a retired descriptor holds nothing past +0xfec. The qualification's descriptor
/// had 0x8000 bytes to itself before its usage record.
const DESCRIPTOR_BYTES: usize = 0x8000;
const _: () = assert!(DESCRIPTOR_BYTES >= compute::DESCRIPTOR_PREFIX_BYTES);
/// Firmware start and end timestamps in a descriptor.
const DESCRIPTOR_TIMES: usize = 0xf2c;
/// Support page: the completion stamps and status words, then the firmware usage record.
const STAMPS: usize = 0;
const STATUS: usize = 0x80;
const USAGE: usize = 0x1000;
const SUPPORT_BYTES: usize = 0x4000;
/// Polls of 1 ms for the installation to be consumed.
const INSTALL_POLLS: u32 = 2000;

/// What a channel uses of its firmware session, borrowed under the device mutex.
pub(crate) struct Host<'a> {
    pub(crate) init: &'a InitData,
    pub(crate) port: &'a Port,
    pub(crate) qids: &'a mut kick::QueueIds,
    pub(crate) crashed: &'a AtomicBool,
}

/// What one kick runs: the user's CDM stream (its first span) and its planned resources.
#[derive(Clone, Copy)]
pub(crate) struct Dispatch {
    pub(crate) cdm_va: u64,
    /// The stream's final word: its exclusive end minus four.
    pub(crate) cdm_end_va: u64,
    pub(crate) resources: Resources,
}

impl Dispatch {
    /// The dispatch of a validated M5 compute command, as every DRM publication path takes it.
    pub(crate) fn of(command: &crate::g17::command::Validated) -> Result<Self> {
        match command {
            crate::g17::command::Validated::Compute {
                cdm_va,
                cdm_end_va,
                resources: Some(resources),
                ..
            } => Ok(Self {
                cdm_va: *cdm_va,
                cdm_end_va: *cdm_end_va,
                resources: *resources,
            }),
            _ => Err(EINVAL),
        }
    }
}

/// A kick in flight.
struct Active {
    ticket: Ticket,
    /// Its descriptor and notification slot.
    slot: usize,
    /// The pool's submission count that covers this kick.
    pool_target: u64,
    work: Arc<WorkStateLease>,
    /// The DRM command; `None` for the qualification's own kicks.
    packet: Option<Arc<Packet>>,
    observed: Option<Observation>,
}

/// A kick just published.
pub(crate) struct Published {
    pub(crate) ticket: Ticket,
    pub(crate) pool_target: u64,
    /// The outer work ring's producer after this kick.
    pub(crate) outer: u32,
}

/// One retirement of [`Channel::retire`]: the kick, its command and its result.
pub(crate) struct Retired {
    pub(crate) ticket: Ticket,
    pub(crate) packet: Option<Arc<Packet>>,
    pub(crate) result: Result<[u64; 2]>,
}

/// One M5 hardware compute queue. Field order drops every alias before its backing.
pub(crate) struct Channel {
    aliases: KVec<mmu::KernelMapping>,
    context: Arc<Context>,
    id: kick::Id,
    owner: u64,
    profile: &'static Profile,
    graph: KernelObject,
    descriptors: KernelObject,
    descriptors_low: u64,
    support: KernelObject,
    kick: kick::Queue,
    pool: M5ComputePool,
    operand: KernelObject,
    preempt: KernelObject,
    usage: KernelObject,
    /// The client views of the preemption buffer and the usage (robustness) page.
    preempt_va: u64,
    usage_va: u64,
    /// Offset of the item-ring producer in the pointer block of this firmware build.
    producer: usize,
    installed: bool,
    ordinal: u32,
    active: KVec<Active>,
    /// Set once a kick failed or the channel's state became ambiguous: nothing more is published,
    /// and every failed kick stays in `failed`, pinning its command.
    quarantine: Option<Error>,
    failed: KVec<Active>,
    /// The logical queue closed; the channel stays installed.
    closed: bool,
    /// The firmware reported a failed kick of this queue.
    faulted: bool,
}

impl Channel {
    /// Builds the channel of QID `id` for `context` and maps it into the context's VM. Nothing is
    /// visible to the firmware yet, so a failure here frees everything; the caller cancels the
    /// unpublished QID. Boxed: a channel holds a dozen objects, and the boot-time qualification
    /// builds it deep in probe's stack.
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        id: kick::Id,
        owner: u64,
        profile: &'static Profile,
        pointers: &PointerLayout,
    ) -> Result<KBox<Self>> {
        let vm = context.vm();
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let cpu = CpuMap::WriteCombined;
        let mut aliases = KVec::with_capacity(8, GFP_KERNEL)?;
        let mut kick = kick::Queue::compute(alloc, id)?;
        aliases.push(kick.map_client(vm)?, GFP_KERNEL)?;
        let mut graph = alloc.kernel(GRAPH_BYTES, PAGE as u64, prot, cpu)?;
        super::compute::initialize_m5_graph(&mut graph, &context, pointers)?;
        let descriptors = alloc.kernel(DEPTH * DESCRIPTOR_BYTES, PAGE as u64, prot, cpu)?;
        // The canonical low arena, as for every descriptor ring, and the same address in the VM.
        let low = descriptors.alias_in(
            alloc.uat.kernel_lower_vm(),
            alloc.domain(Domain::Canonical),
            PAGE as u64,
            mmu::PROT_GPU_SHARED_RW,
        )?;
        let descriptors_low = low.iova();
        aliases.push(low, GFP_KERNEL)?;
        aliases.push(
            descriptors.map_alias(vm, descriptors_low, mmu::PROT_GPU_SHARED_RW)?,
            GFP_KERNEL,
        )?;
        let support = alloc.kernel(SUPPORT_BYTES, PAGE as u64, prot, cpu)?;
        let pool = M5ComputePool::new(alloc, id.qid(), profile.no_storage.pool_blocks)?;
        aliases.push(pool.map_client(vm)?, GFP_KERNEL)?;
        let operand = alloc.lower(
            profile.operand.bytes,
            PAGE as u64,
            mmu::PROT_GPU_SHARED_RW,
            cpu,
        )?;
        aliases.push(
            operand.map_alias(vm, operand.gpu_va(), mmu::PROT_GPU_SHARED_RW)?,
            GFP_KERNEL,
        )?;
        // Kernel-owned buffers the GPU writes in the client's context: client-private views.
        let private = alloc.domain(Domain::Private);
        let preempt = alloc.kernel(compute::PREEMPT_BYTES, PAGE as u64, prot, cpu)?;
        let preempt_alias =
            preempt.alias_in(vm, private.clone(), PAGE as u64, mmu::PROT_GPU_SHARED_RW)?;
        let preempt_va = preempt_alias.iova();
        aliases.push(preempt_alias, GFP_KERNEL)?;
        let usage = alloc.kernel(PAGE, PAGE as u64, prot, cpu)?;
        let usage_alias = usage.alias_in(vm, private, PAGE as u64, mmu::PROT_GPU_SHARED_RW)?;
        let usage_va = usage_alias.iova();
        aliases.push(usage_alias, GFP_KERNEL)?;
        let channel = KBox::new(
            Self {
                aliases,
                context,
                id,
                owner,
                profile,
                graph,
                descriptors,
                descriptors_low,
                support,
                kick,
                pool,
                operand,
                preempt,
                usage,
                preempt_va,
                usage_va,
                producer: pointers.producer,
                installed: false,
                ordinal: 0,
                active: KVec::with_capacity(DEPTH, GFP_KERNEL)?,
                quarantine: None,
                failed: KVec::new(),
                closed: false,
                faulted: false,
            },
            GFP_KERNEL,
        )?;
        channel.publish_mappings()?;
        Ok(channel)
    }

    /// Publishes the complete mapping set to every context naming its roots, before any record
    /// names it. The kick ring's own aliases are published when its first entry is installed.
    fn publish_mappings(&self) -> Result {
        for alias in &self.aliases {
            alias.publish_to_contexts();
        }
        for object in [
            &self.graph,
            &self.descriptors,
            &self.support,
            &self.operand,
            &self.preempt,
            &self.usage,
        ] {
            object.publish_to_contexts()?;
        }
        self.pool.publish_to_contexts()
    }

    pub(crate) fn qid(&self) -> u8 {
        self.id.qid()
    }
    pub(crate) fn profile(&self) -> &'static Profile {
        self.profile
    }
    pub(crate) fn owner(&self) -> u64 {
        self.owner
    }
    pub(crate) fn context(&self) -> &Arc<Context> {
        &self.context
    }
    pub(crate) fn installed(&self) -> bool {
        self.installed
    }
    pub(crate) fn in_flight(&self) -> bool {
        !self.active.is_empty()
    }
    /// Room for another kick: installed, healthy, open and below [`DEPTH`].
    pub(crate) fn has_room(&self) -> bool {
        self.installed && self.quarantine.is_none() && !self.closed && self.active.len() < DEPTH
    }
    pub(crate) fn quarantined(&self) -> Option<Error> {
        self.quarantine
    }
    pub(crate) fn close(&mut self) {
        self.closed = true;
    }
    /// The firmware failed a kick of this queue: the next retirement pass fails the channel.
    pub(crate) fn note_fault(&mut self) {
        self.faulted = true;
    }
    /// Kicks published since installation.
    pub(crate) fn kicks(&self) -> u64 {
        self.kick.timestamp().get() - KickTimestamp::FIRST.get()
    }
    /// The DRM command `packet` is in flight on this channel, or failed on it.
    pub(crate) fn owns_packet(&self, packet: &Arc<Packet>) -> bool {
        Iterator::chain(self.active.iter(), self.failed.iter()).any(|active| {
            active
                .packet
                .as_ref()
                .is_some_and(|own| Arc::ptr_eq(own, packet))
        })
    }

    fn word(&self, offset: usize) -> Result<u32> {
        Ok(self.graph.word(offset)?.load(Ordering::Acquire))
    }

    /// The inner item ring's cursors.
    pub(crate) fn cursors(&self) -> Result<Cursors> {
        Ok(Cursors {
            producer: self.word(POINTERS + self.producer)?,
            consumer: self.word(POINTERS)?,
            count: ITEM_RING_ENTRIES,
        })
    }

    /// The queue's completion stamp.
    pub(crate) fn stamp(&self) -> Result<u32> {
        fence(Ordering::Acquire);
        Ok(self.support.word(STAMPS)?.load(Ordering::Relaxed))
    }

    pub(crate) fn completed_submissions(&self) -> Result<u64> {
        self.pool.completed_submissions()
    }

    fn qos_owner(&self) -> qos::Owner {
        qos::Owner {
            qid: self.qid(),
            qos: self.context.qos_id(),
            data_master: DataMaster::Compute as u8,
        }
    }

    fn config(&self, install: bool, completion_seed: Option<u64>) -> Result<QueueConfig> {
        QueueConfig::new(&QueueConfigArgs {
            target: QueueConfigTarget::Compute,
            kick_ring_va: self.kick.low_va(),
            kick_ring_fw_va: self.kick.firmware_va(),
            qid: self.qid(),
            install,
            completion_seed,
            context_id: self.context.id().try_into()?,
            // The pool is bound with the first kick's context update.
            free_list: match install {
                true => FreeListBinding {
                    va: 0,
                    generation: u64::MAX,
                    slot: u32::MAX,
                },
                false => FreeListBinding {
                    va: self.pool.control_va(),
                    generation: 1,
                    slot: u32::from(self.pool.id()),
                },
            },
            scheduler_va: self.context.scheduler_va(),
            policy: self.context.policy(),
            qos_slot: self.context.qos_id(),
            qos_update: install,
            owner_pid: self.context.owner_pid(),
            context_update: !install,
        })
    }

    /// Exposes the inner items up to `producer` through the claimed outer slot, then runs
    /// `submission` (a kick's) through the host command port. Returns the outer producer.
    fn expose(
        &self,
        port: &Port,
        claim: crate::g17::port::OuterClaim<'_>,
        producer: u32,
        first: bool,
        submission: Option<&HardwareSubmission>,
    ) -> Result<u32> {
        fence(Ordering::SeqCst);
        self.graph
            .word(POINTERS + self.producer)?
            .store(producer, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        let slot = fw::channels::WorkSlot::new(
            DataMaster::Compute,
            self.graph.gpu_va(),
            self.qid(),
            producer.try_into()?,
            first,
        )?;
        port.publish_m5_compute(claim, &slot, submission)
    }

    /// Installs the queue: its pool row, the initial configuration, the QID, and the first outer
    /// slot naming it, then waits for the firmware to consume the configuration. From the
    /// publication on the channel is retained whatever happens; a failed installation leaves it
    /// quarantined.
    pub(crate) fn install(&mut self, host: &mut Host<'_>) -> Result {
        if self.installed {
            return Err(EINVAL);
        }
        let claim = host
            .port
            .reserve_compute(self.context.policy().priority())?;
        host.init
            .install_m5_compute_pool(self.pool.id(), self.pool.descriptor()?)?;
        self.graph.write(CONFIG, self.config(true, None)?)?;
        self.graph
            .write(ITEMS, self.graph.gpu_va() + CONFIG as u64)?;
        host.qids.publish(self.id)?;
        self.installed = true;
        let result = (|| {
            self.expose(host.port, claim, 1, true, None)?;
            for _ in 0..INSTALL_POLLS {
                if host.crashed.load(Ordering::Acquire) {
                    return Err(EIO);
                }
                if self.cursors()?.consumer == 1 {
                    return Ok(());
                }
                fsleep(Delta::from_millis(1));
            }
            Err(ETIMEDOUT)
        })();
        if let Err(error) = result {
            self.quarantine = Some(error);
        }
        result
    }

    /// The one M5 publication transaction: descriptor, notification and kick entry, the inner
    /// items, then the outer slot and the host command. `work` holds the kick's
    /// work-state node; `epoch` is the recovery generation the caller checked with recovery
    /// closed. Everything fallible precedes the first firmware-visible store; a failure after it
    /// quarantines the channel. EAGAIN: no room yet, and nothing changed.
    pub(crate) fn publish(
        &mut self,
        host: &mut Host<'_>,
        dispatch: &Dispatch,
        work: &Arc<WorkStateLease>,
        submission: u64,
        epoch: u64,
        packet: Option<Arc<Packet>>,
    ) -> Result<Published> {
        if let Some(error) = self.quarantine {
            return Err(error);
        }
        if !self.installed || self.closed {
            return Err(EINVAL);
        }
        if self.active.len() >= DEPTH {
            return Err(EAGAIN);
        }
        let policy = self.context.policy();
        let cursors = self.cursors()?;
        let first = self.ordinal == 0;
        let items = if first { 2 } else { 1 };
        if (cursors.consumer + cursors.count - cursors.producer - 1) % cursors.count < items {
            return Err(EAGAIN);
        }
        let claim = host.port.reserve_compute(policy.priority())?;
        let timestamp = self.kick.timestamp();
        let slot = timestamp.slot() % DEPTH;
        let offset = (slot * DESCRIPTOR_BYTES) as u64;
        let high = self.descriptors.gpu_va() + offset;
        let low = self.descriptors_low + offset;
        let work_word = work.word();
        let work_node = work.node_va()?;
        let stamps = self.support.gpu_va() + STAMPS as u64;
        let status = self.support.gpu_va() + STATUS as u64;
        let args = DescriptorArgs {
            registers: RegisterArgs {
                auxiliary_allocation: 0,
                preempt_va: self.preempt_va,
                cdm_va: dispatch.cdm_va,
                operand_va: self.operand.gpu_va(),
                usage_va: self.usage_va,
                free_list_slot: u32::from(self.pool.id()),
                context_id: self.context.id().try_into()?,
                work_state: work_word,
                object_ids: compute::OBJECT_IDS,
                resource_policy: dispatch.resources.policy,
                preempt_state_offset: self.profile.preempt_state_offset,
                operand: self.profile.operand,
                resource_control: dispatch.resources.control,
                dispatch_control: self.profile.dispatch_control,
                auxiliary_control: self.profile.auxiliary_control,
            },
            descriptor_va: low,
            submit_sequence: 0,
            work_state_va: work_node,
            cdm_end_va: dispatch.cdm_end_va,
            stamp_va: [stamps, stamps + 0x40],
            stamp_value: fw::stamp_value(timestamp.get() as u32),
            qid: u32::from(self.qid()),
            kick_timestamp: timestamp.get(),
            completion_ordinal: 0,
            status_va: [status, status + 8],
            timestamp_va: [0; 2],
            shared_control_va: self.pool.control_va(),
            event_metadata: [0; 16],
            usage_fw_va: self.support.gpu_va() + USAGE as u64,
            completion_identity: compute::COMPLETION_IDENTITY,
        };
        // The slot's previous kick retired: firmware no longer reads its descriptor or record.
        self.descriptors
            .initialize::<Descriptor>(slot * DESCRIPTOR_BYTES, |descriptor| {
                descriptor.write(&args)
            })?;
        if first {
            self.graph.write(UPDATE, self.config(false, Some(0))?)?;
        }
        let announce = ANNOUNCES + slot * ANNOUNCE_STRIDE;
        self.graph
            .write(announce, Notification::new(self.qid(), timestamp)?)?;
        let command =
            HardwareSubmission::compute(self.qid(), self.kick.low_va(), policy.priority(), first)?;
        let mut target = cursors.producer;
        let mut records = [0u64; 2];
        let mut count = 0;
        if first {
            records[0] = self.graph.gpu_va() + UPDATE as u64;
            count = 1;
        }
        records[count] = self.graph.gpu_va() + announce as u64;
        count += 1;
        let kick_args = KickArgs {
            qid: self.qid(),
            timestamp,
            parent: self.kick.parent(),
            barriers: &[],
            dependency_types: Some(&[compute::PARENT_DEPENDENCY]),
            descriptor_va: high,
            queue_va: self.graph.gpu_va(),
            qos: KickQos {
                slot: self.context.qos_id(),
                class: compute::KICK_QOS_CLASS,
            },
            mcache: None,
            event_mask: compute::KICK_EVENT_MASK,
            register_arrays: compute::bindings(low)?,
            scs_enabled: false,
            priority: policy.priority(),
        };
        self.active.reserve(1, GFP_KERNEL)?;
        let prepared = self.kick.prepare_entry(&kick_args)?;
        if first {
            prepared.prepare_compute_install()?;
        }
        work.add_submitted_kicks(1)?;
        let pool_target = match self.pool.reserve_submission() {
            Ok(target) => target,
            Err(error) => {
                // The pool counter is unchanged: nothing is visible yet.
                work.cancel_unpublished_kicks(1)?;
                return Err(error);
            }
        };
        // From here on the firmware may see this kick: the pool counter names it, and the
        // queue may be running earlier kicks that read the cursors.
        prepared.commit();
        if let Some(packet) = &packet {
            packet.mark_published();
        }
        let result = (|| {
            for &record in &records[..count] {
                self.graph.write(ITEMS + target as usize * 8, record)?;
                target = (target + 1) % ITEM_RING_ENTRIES;
            }
            host.port
                .qos_publish(self.qos_owner(), self.context.scheduler_va(), policy)?;
            self.context.mark_published();
            fence(Ordering::SeqCst);
            // The work ring names the queue and the firmware hears of it before the host
            // command makes it runnable; whether the cores are powered is not a prerequisite.
            self.expose(host.port, claim, target, false, Some(&command))
        })();
        let ticket = Ticket {
            owner: self.owner,
            submission,
            context_id: self.context.id(),
            qid: self.qid(),
            descriptor: high,
            work_node,
            kick: timestamp.get(),
            ordinal: self.ordinal,
            item_producer: target,
            item_count: ITEM_RING_ENTRIES,
            epoch,
        };
        self.ordinal = self.ordinal.saturating_add(1);
        self.active
            .push_within_capacity(Active {
                ticket,
                slot,
                pool_target,
                work: work.clone(),
                packet,
                observed: None,
            })
            .map_err(|_| ENOMEM)?;
        match result {
            Ok(outer) => Ok(Published {
                ticket,
                pool_target,
                outer,
            }),
            Err(error) => {
                // The kick stays in flight and is failed with the channel.
                self.quarantine = Some(error);
                Err(error)
            }
        }
    }

    /// Stages the exact identity of every kick not yet observed for the shared completion-ring
    /// scan. Returns whether any was staged.
    pub(crate) fn request(&self, batch: &mut Batch) -> Result<bool> {
        if self.active.is_empty() || self.quarantine.is_some() {
            return Ok(false);
        }
        batch.stage_frontier(self.qid(), self.kick.timestamp().get())?;
        let stamp = self.stamp()?;
        let mut requested = false;
        for active in self
            .active
            .iter()
            .filter(|active| active.observed.is_none())
        {
            batch.request(
                &active.ticket,
                retirement::stamp_covers(stamp, active.ticket.kick),
            )?;
            requested = true;
        }
        Ok(requested)
    }

    /// Retires the kicks the scan of `batch` completed, oldest first, reporting each to
    /// `retired`; `generation` and `state` are the device's recovery generation and state. A kick
    /// whose witnesses are missing or disagree stays in flight, for the scheduler's bounded
    /// timeout to fail. A zero-duration record (the firmware failed the kick) or a kick that
    /// crossed a recovery quarantines the channel ([`Self::fail`]).
    pub(crate) fn retire(
        &mut self,
        batch: &mut Batch,
        generation: u64,
        state: u32,
        complete_qos: &mut impl FnMut(qos::Owner) -> Result,
        retired: &mut impl FnMut(Retired) -> Result,
    ) -> Result {
        if self.quarantine.is_some() {
            return Ok(());
        }
        if self.faulted {
            return self.fail(EIO, retired);
        }
        for active in self
            .active
            .iter_mut()
            .filter(|active| active.observed.is_none())
        {
            active.observed = batch.take(&active.ticket)?;
        }
        while let Some(front) = self.active.first() {
            match self.check(front, generation, state) {
                Ok(Some(timestamps)) => {
                    complete_qos(self.qos_owner())?;
                    let front = self.active.remove(0).map_err(|_| EINVAL)?;
                    retired(Retired {
                        ticket: front.ticket,
                        packet: front.packet,
                        result: Ok(timestamps),
                    })?;
                }
                Ok(None) => break,
                Err(error) => return self.fail(error, retired),
            }
        }
        Ok(())
    }

    /// The retirement predicate of one kick: `Some` with its record's timestamps once every
    /// witness agrees, `None` while any is missing or disagrees.
    fn check(&self, active: &Active, generation: u64, state: u32) -> Result<Option<[u64; 2]>> {
        if state != 0 {
            return Ok(None);
        }
        if generation != active.ticket.epoch {
            return Err(EIO);
        }
        let stamp = self.stamp()?;
        let Some(timestamps) = retirement::poll(
            &active.ticket,
            active.observed,
            self.cursors()?,
            stamp,
            Recordless::NotYet,
            0,
            None,
            false,
        )?
        else {
            return Ok(None);
        };
        if self.pool.completed_submissions()? < active.pool_target || !active.work.drained()? {
            return Ok(None);
        }
        let offset = active.slot * DESCRIPTOR_BYTES;
        let words = self
            .descriptors
            .read_words::<4>(offset + DESCRIPTOR_TIMES)?;
        let descriptor = [
            u64::from(words[0]) | (u64::from(words[1]) << 32),
            u64::from(words[2]) | (u64::from(words[3]) << 32),
        ];
        if timestamps[1] != descriptor[1] || timestamps[0] > descriptor[0] {
            return Ok(None);
        }
        Ok(Some(timestamps))
    }

    /// Quarantines the channel with `error` and fails every kick in flight. The kicks stay
    /// owned by the channel, pinning their commands: no release of a failed M5 kick is
    /// established, so its memory is never handed back while the session runs.
    pub(crate) fn fail(
        &mut self,
        error: Error,
        retired: &mut impl FnMut(Retired) -> Result,
    ) -> Result {
        self.quarantine.get_or_insert(error);
        let error = self.quarantine.unwrap_or(error);
        self.failed.reserve(self.active.len(), GFP_KERNEL)?;
        while !self.active.is_empty() {
            let active = self.active.remove(0).map_err(|_| EINVAL)?;
            let report = Retired {
                ticket: active.ticket,
                packet: active.packet.clone(),
                result: Err(error),
            };
            self.failed
                .push_within_capacity(active)
                .map_err(|_| ENOMEM)?;
            retired(report)?;
        }
        Ok(())
    }

    /// The end of the last nonzero word of descriptor slot `slot`: how far the firmware wrote.
    pub(crate) fn descriptor_extent(&self, slot: usize) -> Result<usize> {
        let base = slot * DESCRIPTOR_BYTES;
        let mut end = 0;
        for offset in (0..DESCRIPTOR_BYTES).step_by(4) {
            if self
                .descriptors
                .word(base + offset)?
                .load(Ordering::Relaxed)
                != 0
            {
                end = offset + 4;
            }
        }
        Ok(end)
    }

    /// Logs the counters and nonzero stamp and status words of a kick that did not retire.
    pub(crate) fn report(&self, dev: &AsahiDrmDevice, published: &Published) {
        dev_info!(
            dev.as_ref(),
            "M5 channel {}: kick {:#x}, pool completed {:?}/{}, cursors {:?}, quarantine {:?}\n",
            self.qid(),
            published.ticket.kick,
            self.pool.completed_submissions(),
            published.pool_target,
            self.cursors().map(|c| (c.producer, c.consumer)),
            self.quarantine
        );
        for offset in (0..STATUS + 0x10).step_by(4) {
            if let Ok(value) = self
                .support
                .word(STAMPS + offset)
                .map(|w| w.load(Ordering::Relaxed))
            {
                if value != 0 {
                    dev_info!(dev.as_ref(), "M5 completion +{:#x}={:#x}\n", offset, value);
                }
            }
        }
    }
}
