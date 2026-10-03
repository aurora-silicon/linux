// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Physical paired tiling and fragment queues and their retained command storage.

mod observation;
pub(crate) use observation::Observed;
mod dependencies;
mod memory;
pub(crate) mod parked;
mod publication;
pub(crate) mod retirement;
mod warm;

use crate::g17::{
    buffer::{self, target},
    command::Validated,
    completion::Deferred,
    context::Context,
    freelist::RenderPool,
    fw::{
        channels::WorkSlot,
        kick::{KickDependency, KickTimestamp, McacheTable, RenderRegistration},
        queue::DataMaster,
        render as abi,
    },
    job::Packet,
    kick,
    object::{Allocator, Pool},
    qos, recovery,
    status::VmStatus,
};
use crate::{hw::t8140::queues, mmu};
pub(crate) use dependencies::Dependencies;
use kernel::{prelude::*, sync::Arc};
use retirement::{Ticket, Tracker};

#[derive(Copy, Clone)]
pub(crate) struct RenderAddresses {
    /// Tiling then fragment; each pair contains the primary and auxiliary stamp.
    pub(crate) stamps: [[u64; 2]; 2],
}

#[derive(Copy, Clone)]
pub(crate) struct PublicationLease {
    pub(crate) free_list_generation: u64,
    pub(crate) buffer_token: u64,
}

/// Host services borrow the device-locked firmware state. Resource construction
/// and command geometry preparation do not borrow this interface.
pub(crate) trait Host {
    fn epoch(&self) -> Result<(u64, u32)>;
    fn stamps(&self, qids: [u8; 2]) -> Result<[u32; 2]>;
    fn qos_publish(&mut self, owner: qos::Owner, scheduler: u64) -> Result<qos::Publication>;
    fn qos_cancel(&mut self, publication: qos::Publication) -> Result;
    fn install_pair(&mut self, registration: &RenderRegistration) -> Result;
    fn publish_qid(&mut self, id: kick::Id) -> Result;
    fn publish_outer_pair(
        &mut self,
        fragment: &crate::g17::fw::channels::WorkSlot,
        tiling: &crate::g17::fw::channels::WorkSlot,
        commit: impl FnOnce() -> Result,
        late_tiling: impl FnOnce(),
    ) -> Result<[u32; 2]>;
    fn activate_render_pool(&mut self, id: u16, generation: u64) -> Result;
    fn publish_retained_pair(
        &mut self,
        fragment: &WorkSlot,
        tiling: &WorkSlot,
        next: [u32; 2],
    ) -> Result;
    fn notify_render(&mut self) -> Result;
    fn note_submission(&mut self) -> Result;
    fn complete_render(
        &mut self,
        owners: Option<[qos::Owner; 2]>,
        pool_id: u16,
        generation: u64,
        buffer_id: u8,
    ) -> Result;
}

struct Prepared {
    packet: Arc<Packet>,
    ordinal: u64,
    mcache: Option<McacheTable>,
}
struct Active {
    qos_completed: bool,
    flist_completed: bool,
    outer_published: bool,
    generation: u64,
    packet: Arc<Packet>,
    ticket: Ticket,
    kicks: [KickTimestamp; 2],
    context_update: bool,
    failed: bool,
    drained_result: Option<Result<[u64; 4]>>,
}

/// Installed kick backing remains owned even when this pair has no logical
/// owner. Commands retain their exact descriptor and allocation generations.
pub(crate) struct Pair {
    kick_aliases: Option<[Option<mmu::KernelMapping>; 2]>,
    memory: KBox<memory::Memory>,
    kicks: [kick::Queue; 2],
    free_list: Arc<RenderPool>,
    pending_aliases: Option<KVec<mmu::KernelMapping>>,
    prepared: Option<Prepared>,
    active: Tracker<Active>,
    slot: u8,
    owner: Option<u64>,
    ordinal: u64,
    initial_packet: u64,
    clusters: u32,
    descriptor_flags: [u32; 2],
    installed: [bool; 2],
    configs: [Option<crate::g17::fw::queue::QueueConfig>; 2],
    cursors: [u32; 2],
    recovery_generation: u64,
    registered: bool,
    outer_started: bool,
    quarantined: bool,
    terminal: Option<(Arc<VmStatus>, Error)>,
    previous: Option<RetiredBinding>,
    displaced: Option<RetiredBinding>,
    context_update: bool,
}
impl Pair {
    #[inline(never)]
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        pool: &Arc<Pool>,
        ids: [kick::Id; 2],
        slot: u8,
        owner: u64,
        context: Arc<Context>,
        free_list: Arc<RenderPool>,
        metrics: buffer::MetricsLease,
        buffer_id: (u8, u64),
        pm_generation: u64,
        first: &Arc<Packet>,
        clusters: u32,
        descriptor_flags: [u32; 2],
    ) -> Result<KBox<Self>> {
        if slot == 0
            || usize::from(slot) >= queues::RENDER_SLOTS
            || owner == 0
            || ids[0].engine() != DataMaster::Tiling
            || ids[1].engine() != DataMaster::Fragment
            || !Arc::ptr_eq(&context, &first.context)
            || !context.is_current()
        {
            return Err(EINVAL);
        }
        if let Err(error) = alloc.uat.vm_context_mask(context.vm()) {
            if error == ENOENT {
                context.status().record(error);
            }
            return Err(error);
        }
        let Validated::Render { pass, .. } = &first.command else {
            return Err(EINVAL);
        };
        let word = first.completion.work_state().word();
        if word == u32::MAX {
            return Err(EINVAL);
        }
        let layout = target::Layout::new(pass, clusters)?;
        let storage = KBox::<Self>::new_uninit(GFP_KERNEL)?;
        let (tiling, ta_alias) = kick::Queue::render(alloc, ids[0], context.vm())?;
        let (fragment, fragment_alias) = kick::Queue::render(alloc, ids[1], context.vm())?;
        let (memory, aliases) = memory::Memory::new(
            alloc,
            pool,
            context,
            [ids[0].qid(), ids[1].qid()],
            &layout,
            metrics,
            buffer_id,
            pm_generation,
            0,
            word,
        )?;
        storage.write_init(kernel::try_init!(Self {
            kick_aliases: Some([Some(ta_alias), Some(fragment_alias)]),
            memory,
            kicks: [tiling, fragment],
            free_list,
            pending_aliases: Some(aliases),
            prepared: None,
            active: Tracker::new(),
            slot,
            owner: Some(owner),
            ordinal: 0,
            initial_packet: first.id,
            clusters,
            descriptor_flags,
            installed: [false; 2],
            configs: [None; 2],
            cursors: [0; 2],
            recovery_generation: 0,
            registered: false,
            outer_started: false,
            quarantined: false,
            terminal: None,
            previous: None,
            displaced: None,
            context_update: true,
        }))
    }
    pub(crate) fn qids(&self) -> [u8; 2] {
        self.kicks.each_ref().map(|kick| kick.id().qid())
    }
    pub(crate) fn slot(&self) -> u8 {
        self.slot
    }
    pub(crate) fn owner(&self) -> Option<u64> {
        self.owner
    }
    pub(crate) fn context(&self) -> &Arc<Context> {
        &self.memory.context
    }
    pub(crate) fn pool(&self) -> &Arc<RenderPool> {
        &self.free_list
    }
    pub(crate) fn pool_id(&self) -> u64 {
        u64::from(self.memory.lifecycle[0].predecessor)
    }
    pub(crate) fn buffer_id(&self) -> u8 {
        self.memory.manager.buffer_id()
    }
    pub(crate) fn manager(&mut self) -> &mut buffer::Manager {
        &mut self.memory.manager
    }
    pub(crate) fn in_flight(&self) -> bool {
        self.active.len() != 0
    }
    pub(crate) fn published_in_flight(&self) -> bool {
        !self.quarantined && self.active.iter().any(|active| active.outer_published)
    }
    pub(crate) fn room(&self) -> bool {
        !self.quarantined && self.owner.is_some() && self.prepared.is_none() && self.active.room()
    }
    pub(crate) fn take_terminal(&mut self) -> Option<(Arc<VmStatus>, Error)> {
        self.terminal.take()
    }

    /// The registry owns this pair exclusively during potentially allocating
    /// preparation. No queue producer or shared submission counter changes.
    pub(crate) fn prepare(
        &mut self,
        alloc: &Allocator<'_>,
        packet: &Arc<Packet>,
        addresses: RenderAddresses,
    ) -> Result {
        if !self.room()
            || !Arc::ptr_eq(&self.memory.context, &packet.context)
            || !packet.context.is_current()
        {
            return Err(EBUSY);
        }
        let Validated::Render {
            pass, attachments, ..
        } = &packet.command
        else {
            return Err(EINVAL);
        };
        if packet.completion.status().get() != 0 {
            return Err(EIO);
        }
        let ordinal = self.ordinal;
        ordinal.checked_add(1).ok_or(EOVERFLOW)?;
        let work = packet.completion.work_state();
        let node = work.node_va()?;
        let word = work.word();
        let layout = target::Layout::new(pass, self.clusters)?;
        let owners = self.memory.owners();
        self.memory
            .target
            .ensure_capacity(alloc.dev, packet.context.vm(), &layout, &owners)?;
        let target = self.memory.target.prepare(ordinal, &layout)?;
        let scene = self.memory.manager.prepare_scene(ordinal)?;
        self.memory.generation(ordinal)?.prepare(ordinal, pass)?;
        let [deflake, ta_status, fragment_status, fragment_fw, auxiliary] =
            self.memory.generation(ordinal)?.addresses();
        let lifecycle = if ordinal != 0 || packet.id != self.initial_packet {
            let (fragment, tiling) =
                crate::g17::ids::OBJECTS.render_pair(self.memory.lifecycle[0].predecessor)?;
            [tiling, fragment]
        } else {
            self.memory.lifecycle
        };
        let (scratch, metric) = self.memory.manager.scene_registers(scene)?;
        let timestamp_offset = ordinal as usize % memory::RECORDS * 16;
        let timestamp = self.memory.timestamps.gpu_va() + timestamp_offset as u64;
        let resources = abi::RenderResources {
            context_id: packet.context.id().try_into()?,
            context_generation: packet.context.generation(),
            pb_slot: self.buffer_id().into(),
            free_list_slot: self.free_list.id().into(),
            free_list_control_va: self.free_list.control_va(),
            kick_record_va: node,
            pb_va: self.memory.manager.state_va(),
            scene_va: self.memory.manager.scene_va(scene)?,
            scene_end_va: self.memory.manager.trailer_va(),
            pm_scratch: scratch,
            pm_metric: metric,
            tilemap_va: target.tilemap,
            layermeta_va: target.layer,
            heapmeta_va: target.heap,
            tpc_va: self.memory.target.tpc_va(),
            tpc_size: layout.tpc_size as u64,
            deflake_va: deflake,
            ta_status_va: ta_status,
            fragment_status_va: fragment_status,
            fragment_status_fw_va: fragment_fw,
            aux_fb_va: auxiliary,
            timestamp_va: timestamp,
            ta_ids: lifecycle[0],
            fragment_ids: lifecycle[1],
            unk_2104: self.descriptor_flags[0],
            unk_210c: self.descriptor_flags[1],
        };
        let qids = self.qids();
        let args = abi::RenderArgs {
            pass,
            resources: &resources,
            ordinal,
            ta: abi::RenderStage {
                qid: qids[0],
                descriptor_va: self.memory.graph.descriptor_client(0, ordinal),
                state_word: word,
                stamp_va: addresses.stamps[0][0],
                aux_stamp_va: addresses.stamps[0][1],
            },
            fragment: abi::RenderStage {
                qid: qids[1],
                descriptor_va: self.memory.graph.descriptor_client(1, ordinal),
                state_word: word,
                stamp_va: addresses.stamps[1][0],
                aux_stamp_va: addresses.stamps[1][1],
            },
        };
        self.memory.graph.claim(ordinal)?;
        let result = self.memory.write_descriptors(
            &args,
            attachments[1].as_slice(),
            packet.context.id().try_into()?,
        );
        let mcache = match result {
            Ok(mcache) => mcache,
            Err(error) => {
                self.memory.graph.release(ordinal)?;
                self.memory.generation(ordinal)?.ordinal = None;
                self.memory.target.cancel(ordinal);
                return Err(error);
            }
        };
        self.memory.timestamps.write(timestamp_offset, [0u64; 2])?;
        self.memory.manager.claim_scene(ordinal, scene)?;
        self.prepared = Some(Prepared {
            packet: packet.clone(),
            ordinal,
            mcache,
        });
        Ok(())
    }
}

impl Pair {
    /// All descriptor bytes and retained allocations are ready before entry.
    /// The first inner producer establishes firmware ownership, including on
    /// later errors. Such failures retain the active packet for recovery.
    pub(crate) fn publish(
        &mut self,
        packet: Arc<Packet>,
        dependencies: &[KickDependency],
        lease: PublicationLease,
        host: &mut impl Host,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        use crate::g17::fw::{
            channels::WorkSlot,
            kick::{KickArgs, KickQos},
            queue::{
                self, FreeListBinding, QueueConfig, QueueConfigArgs, QueueConfigTarget,
                TilingBinding,
            },
        };
        use core::sync::atomic::{fence, Ordering};
        if self.pending_aliases.is_some() || self.kick_aliases.is_some() {
            return Err(EBUSY);
        }
        let (epoch, recovery_state) = host.epoch()?;
        if recovery_state != 0
            || self.quarantined
            || !self.active.room()
            || packet.completion.render_slot() != Some(self.slot)
        {
            return Err(EBUSY);
        }
        let prepared = self.prepared.as_ref().ok_or(EINVAL)?;
        if !Arc::ptr_eq(&prepared.packet, &packet)
            || prepared.ordinal != self.ordinal
            || !self.memory.owns(self.ordinal)
            || !packet.context.is_current()
        {
            return Err(EIO);
        }
        if packet.completion.status().get() != 0 {
            return Err(EIO);
        }
        let ordinal = self.ordinal;
        let next_ordinal = ordinal.checked_add(1).ok_or(EOVERFLOW)?;
        let mcache = prepared.mcache;
        let qids = self.qids();
        let kicks = self.kicks.each_ref().map(kick::Queue::timestamp);
        let parents = self.kicks.each_ref().map(kick::Queue::parent);
        let work = packet.completion.work_state();
        let work_slot = work.render_slot(self.slot)?;
        self.memory.bind_work_slot(ordinal, qids, work_slot)?;
        let payload = work.channel_payload(self.slot, ordinal)?;
        let stamps = host.stamps(qids)?;
        fence(Ordering::Acquire);
        let covered = stamps.map(|stamp| retirement::stamp_covers(stamp, ordinal));
        self.memory
            .completion_links(ordinal, kicks, payload, !covered[1])?;
        let context = &packet.context;
        let qos = context.qos_id();
        let qos_owners = [
            qos::Owner {
                qid: qids[0],
                qos,
                data_master: DataMaster::Tiling as u8,
            },
            qos::Owner {
                qid: qids[1],
                qos,
                data_master: DataMaster::Fragment as u8,
            },
        ];
        let mut configs = [None; 2];
        for stage in 0..2 {
            configs[stage] = Some(QueueConfig::new(&QueueConfigArgs {
                target: if stage == 0 {
                    QueueConfigTarget::Tiling(TilingBinding {
                        pb_va: self.memory.manager.state_va(),
                        pb_token: lease.buffer_token,
                        pb_slot: self.buffer_id().into(),
                        fragment_qid: qids[1],
                    })
                } else {
                    QueueConfigTarget::Fragment
                },
                kick_ring_va: self.kicks[stage].low_va(),
                kick_ring_fw_va: self.kicks[stage].firmware_va(),
                qid: qids[stage],
                install: !self.installed[stage],
                completion_seed: covered[stage].then(|| {
                    kicks[stage]
                        .prev()
                        .completion_value(payload.wrapping_sub(1))
                }),
                context_id: context.id().try_into()?,
                free_list: FreeListBinding {
                    va: self.free_list.control_va(),
                    generation: lease.free_list_generation,
                    slot: self.free_list.id().into(),
                },
                scheduler_va: context.scheduler_va(),
                qos_slot: qos,
                qos_update: self.context_update,
                owner_pid: context.owner_pid(),
                context_update: self.context_update,
            })?);
        }
        let configs = [configs[0].ok_or(EIO)?, configs[1].ok_or(EIO)?];
        let cache = if self.recovery_generation == epoch {
            self.configs
        } else {
            [None; 2]
        };
        let present = [
            publication::required(cache[0], configs[0]),
            publication::required(cache[1], configs[1]),
        ];
        let cursors = self.memory.graph.cursors()?;
        if cursors[0].write != self.cursors[0] || cursors[1].write != self.cursors[1] {
            return Err(EBUSY);
        }
        let queues = qids.map(|qid| {
            self.memory.graph.queues.gpu_va()
                + u64::from(qid) * size_of::<queue::QueueRecord>() as u64
        });
        let fragment_barriers = [KickDependency::new(qids[0], kicks[0]).ok_or(EINVAL)?];
        let args = |stage: usize, barriers, arrays, mcache| KickArgs {
            qid: qids[stage],
            timestamp: kicks[stage],
            parent: parents[stage],
            barriers,
            descriptor_va: self.memory.graph.descriptor_va(stage, ordinal),
            queue_va: queues[stage],
            qos: KickQos {
                slot: qos,
                class: if stage == 0 {
                    abi::TA_QOS_CLASS
                } else {
                    abi::FRAGMENT_QOS_CLASS
                },
            },
            mcache,
            event_mask: abi::RENDER_KICK_EVENT_MASK,
            register_arrays: arrays,
        };
        let ta_args = args(
            0,
            dependencies,
            abi::TaDescriptor::register_bindings(self.memory.graph.descriptor_client(0, ordinal))?,
            None,
        );
        let fragment_args = args(
            1,
            &fragment_barriers,
            abi::FragmentDescriptor::register_bindings(
                self.memory.graph.descriptor_client(1, ordinal),
            )?,
            mcache,
        );
        let registration = RenderRegistration::new(
            (qids[1], self.kicks[1].low_va()),
            (qids[0], self.kicks[0].low_va()),
        )?;
        let [tiling_kick, fragment_kick] = &mut self.kicks;
        let tiling_kick = tiling_kick.prepare_entry(&ta_args)?;
        let fragment_kick = fragment_kick.prepare_entry(&fragment_args)?;
        let memory = &mut *self.memory;
        let lanes = [
            publication::Lane::new(
                &memory.graph,
                0,
                ordinal,
                self.cursors[0],
                configs[0],
                present[0],
                kicks[0],
            )?,
            publication::Lane::new(
                &memory.graph,
                1,
                ordinal,
                self.cursors[1],
                configs[1],
                present[1],
                kicks[1],
            )?,
        ];
        let windows = [lanes[0].window, lanes[1].window];
        let previous = self.active.front().map_or(cursors, |active| {
            active.ticket.windows.map(|window| {
                let target = window.target();
                retirement::Cursors {
                    done: target,
                    read: target,
                    write: target,
                }
            })
        });
        let fragment_slot = WorkSlot::new(
            DataMaster::Fragment,
            queues[1],
            qids[1],
            windows[1].target().try_into()?,
            !self.outer_started,
        )?;
        let tiling_slot = WorkSlot::new(
            DataMaster::Tiling,
            queues[0],
            qids[0],
            windows[0].target().try_into()?,
            !self.outer_started,
        )?;
        let event_slot = memory
            .graph
            .support
            .word(abi::storage::EVENT_SLOTS + 4)?;
        let next_event = event_slot.load(Ordering::Relaxed).wrapping_add(1);
        let inner = memory.graph.support.word(0x28000)?;
        let next_inner = (next_ordinal as u32).wrapping_mul(2);
        let flist_submission = self.free_list.prepare_submission(2)?;
        let pm_submission = memory.manager.prepare_submission()?;
        self.active
            .push(Active {
                qos_completed: false,
                flist_completed: false,
                outer_published: false,
                generation: lease.free_list_generation,
                packet: packet.clone(),
                ticket: Ticket {
                    ordinal,
                    windows,
                    previous,
                },
                kicks,
                context_update: self.context_update,
                failed: false,
                drained_result: None,
            })
            .map_err(|_| EBUSY)?;
        self.prepared = None;
        packet.mark_published();
        lanes[1].publish_prefix();
        fragment_kick.commit();
        lanes[1].publish_kick();
        tiling_kick.commit();
        let mut qos_publications = [None; 2];
        let result = (|| {
            qos_publications[1] = Some(host.qos_publish(qos_owners[1], context.scheduler_va())?);
            qos_publications[0] = Some(host.qos_publish(qos_owners[0], context.scheduler_va())?);
            if !self.registered {
                host.install_pair(&registration)?;
                self.registered = true;
            }
            for kick in &self.kicks {
                host.publish_qid(kick.id())?;
            }
            host.publish_outer_pair(
                &fragment_slot,
                &tiling_slot,
                || {
                    work.add_submitted_kicks(2)?;
                    flist_submission.commit();
                    pm_submission.commit();
                    Ok(())
                },
                || lanes[0].publish_tiling(event_slot, next_event, inner, next_inner),
            )
        })();
        let outer_next = match result {
            Ok(next) => next,
            Err(error) => {
                self.quarantined = true;
                if !packet.completion.spared() && self.terminal.is_none() {
                    self.terminal = Some((packet.completion.status().clone(), error));
                }
                self.active
                    .back()
                    .ok_or(EIO)?
                    .packet
                    .completion
                    .quarantine_render();
                for publication in qos_publications.into_iter().flatten() {
                    host.qos_cancel(publication)?;
                }
                self.active.back_mut().ok_or(EIO)?.qos_completed = true;
                self.quarantined = true;
                if !packet.completion.spared() && self.terminal.is_none() {
                    self.terminal = Some((packet.completion.status().clone(), error));
                }
                defer(Deferred::FailedOwned(packet.completion.clone(), error))?;
                self.active.back_mut().ok_or(EIO)?.failed = true;
                return Err(error);
            }
        };
        self.ordinal = next_ordinal;
        self.cursors = windows.map(|window| window.target());
        for stage in 0..2 {
            if present[stage] {
                self.configs[stage] = Some(configs[stage]);
            }
        }
        self.recovery_generation = epoch;
        self.installed = [true; 2];
        self.outer_started = true;
        if let Some(previous) = self.previous.as_mut().filter(|p| p.publication.is_none()) {
            previous.publication = Some(ordinal);
        }
        packet.context.mark_published();
        let publish = (|| {
            host.activate_render_pool(self.free_list.id(), lease.free_list_generation)?;
            host.publish_retained_pair(&fragment_slot, &tiling_slot, outer_next)?;
            self.active.back_mut().ok_or(EIO)?.outer_published = true;
            host.notify_render()?;
            host.note_submission()
        })();
        if let Err(error) = publish {
            self.quarantined = true;
            self.active
                .back()
                .ok_or(EIO)?
                .packet
                .completion
                .quarantine_render();
            if !packet.completion.spared() && self.terminal.is_none() {
                self.terminal = Some((packet.completion.status().clone(), error));
            }
            defer(Deferred::FailedOwned(packet.completion.clone(), error))?;
            self.active.back_mut().ok_or(EIO)?.failed = true;
            return Err(error);
        }
        Ok(())
    }
}

struct Observer<'a, H> {
    memory: &'a memory::Memory,
    qids: [u8; 2],
    host: &'a mut H,
}
impl<H: Host> retirement::Memory for Observer<'_, H> {
    fn snapshot(&mut self, ordinal: u64) -> Result<retirement::Snapshot> {
        observation::snapshot(self.memory, ordinal)
    }

    fn stamps(&mut self) -> Result<[u32; 2]> {
        self.host.stamps(self.qids)
    }
    fn pass(&mut self, ordinal: u64) -> Result<[u64; 2]> {
        self.memory.pass(ordinal)
    }
}

impl Pair {
    fn qos_owners(&self) -> [qos::Owner; 2] {
        let qids = self.qids();
        let qos = self.context().qos_id();
        [
            qos::Owner {
                qid: qids[0],
                qos,
                data_master: DataMaster::Tiling as u8,
            },
            qos::Owner {
                qid: qids[1],
                qos,
                data_master: DataMaster::Fragment as u8,
            },
        ]
    }
    fn selected(&self, masks: Option<[u64; 2]>) -> bool {
        masks.is_none_or(|masks| {
            self.qids()
                .into_iter()
                .all(|qid| masks[usize::from(qid) / 64] & (1u64 << (qid % 64)) != 0)
        })
    }
    pub(crate) fn contains_packet(&self, packet: &Arc<Packet>) -> bool {
        self.active
            .iter()
            .any(|active| Arc::ptr_eq(&active.packet, packet))
            || self
                .prepared
                .as_ref()
                .is_some_and(|prepared| Arc::ptr_eq(&prepared.packet, packet))
    }
    pub(crate) fn take_deferred_timeout(&self) -> Option<(Arc<Packet>, Error)> {
        self.active.iter().find_map(|active| {
            active
                .packet
                .completion
                .take_deferred_timeout()
                .map(|error| (active.packet.clone(), error))
        })
    }

    pub(crate) fn oldest_packet(&self) -> Option<Arc<Packet>> {
        self.active.front().map(|active| active.packet.clone())
    }
    /// Sample PM feedback only after this pair's exact retirement. Busy pairs
    /// keep their current capacity and let firmware request growth as needed.
    pub(crate) fn host_growth_target(&mut self, packet: &Packet) -> Result<Option<usize>> {
        let Validated::Render { pass, .. } = &packet.command else {
            return Err(EINVAL);
        };
        let required = target::Layout::new(pass, self.clusters)?.tvb_blocks;
        if self.in_flight() {
            return Ok(None);
        }
        if !self.idle_for_context(self.context())? {
            return Err(EBUSY);
        }
        let feedback = if self.ordinal != 0 {
            self.memory.manager.automatic_target()?.unwrap_or(0)
        } else {
            0
        };
        let required = required.max(feedback);
        Ok((required > self.memory.manager.blocks()).then_some(required))
    }

    pub(crate) fn needs_drain(&self) -> bool {
        self.quarantined && self.in_flight()
    }

    pub(crate) fn oldest_spared(&self) -> Option<bool> {
        self.active
            .front()
            .map(|active| active.packet.completion.spared())
    }
    pub(crate) fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
        for active in self.active.iter() {
            visit(active.packet.completion.status());
        }
    }
    pub(crate) fn classify(&self, classify: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) {
        for active in self.active.iter() {
            active
                .packet
                .completion
                .classify(classify(active.packet.completion.status()));
        }
    }
    /// A slot stamp must name retained work before its execution markers are meaningful.
    pub(crate) fn pass_started(&self, stamp: u64) -> Option<bool> {
        const MASK: u64 = (1 << 40) - 1;
        let ordinal = self.active.iter().map(|active| active.ticket.ordinal)
            .find(|ordinal| ordinal.wrapping_add(1) & MASK == stamp & MASK)?;
        Some(observation::snapshot(&self.memory, ordinal).ok()?.pass[0] != 0)
    }

    /// Firmware is halted and all graph borrowers have joined before this copy.
    pub(crate) fn publish_recovery_targets(&self) -> Result {
        self.memory.graph.publish_recovery_targets()
    }

    pub(crate) fn resync(&mut self) {
        self.configs = [None; 2];
        for kick in &mut self.kicks {
            kick.clear_parent_after_recovery();
        }
    }
    pub(crate) fn matches(&self, owner: u64, context: &Arc<Context>) -> Result<bool> {
        Ok(self.owner == Some(owner)
            && Arc::ptr_eq(self.context(), context)
            && context.is_current())
    }
    /// Classification controls VM poisoning. Every failed render fence settles,
    /// while command memory remains owned until paired retirement or stop.
    pub(crate) fn quarantine(
        &mut self,
        error: Error,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        self.quarantined = true;
        // Successors published after recovery can still fail behind its spared
        // head. Classify the whole failing group before any fence can settle.
        if self.active.iter().any(|active| active.packet.completion.spared()) {
            for active in self.active.iter() {
                active.packet.completion.classify(recovery::Class::Spared);
            }
        }
        for active in self.active.iter_mut() {
            active.packet.completion.quarantine_render();
            if !active.failed {
                defer(Deferred::FailedOwned(
                    active.packet.completion.clone(),
                    error,
                ))?;
                active.failed = true;
                if !active.packet.completion.spared() && self.terminal.is_none() {
                    self.terminal = Some((active.packet.completion.status().clone(), error));
                }
            }
        }
        Ok(())
    }
    pub(crate) fn retire(
        &mut self,
        masks: Option<[u64; 2]>,
        _epoch: u64,
        state: u32,
        host: &mut impl Host,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<bool> {
        self.retire_front(masks, state, None, host, defer)
    }

    pub(crate) fn retire_observed(
        &mut self,
        observed: Observed,
        state: u32,
        host: &mut impl Host,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<bool> {
        self.retire_front(None, state, Some(observed), host, defer)
    }

    fn retire_front(
        &mut self,
        masks: Option<[u64; 2]>,
        state: u32,
        observed: Option<Observed>,
        host: &mut impl Host,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<bool> {
        if self.quarantined || state != 0 || !self.selected(masks) {
            return Ok(false);
        }
        let qids = self.qids();
        let qos = self.qos_owners();
        let pool = self.free_list.id();
        let buffer = self.buffer_id();
        loop {
            let Some(front) = self.active.front_mut() else {
                return Ok(false);
            };
            if !front.outer_published {
                return Ok(false);
            }
            if !Arc::ptr_eq(&front.packet.context, &self.memory.context)
                || !self.memory.owns(front.ticket.ordinal)
            {
                return Err(EIO);
            }
            let mut observer = Observer {
                memory: &self.memory,
                qids,
                host,
            };
            let result = if let Some(observed) = observed {
                if observed.ticket != front.ticket {
                    return Err(EIO);
                }
                retirement::poll_observed(&mut front.ticket, &mut observer, observed.snapshot)
            } else {
                retirement::poll(&mut front.ticket, &mut observer)
            };
            let timestamps = match result {
                Ok(retirement::Poll::Complete(timestamps)) => timestamps,
                Ok(retirement::Poll::Retry | retirement::Poll::Pending) => return Ok(false),
                Err(error) => {
                    self.quarantine(error, defer)?;
                    return Ok(false);
                }
            };
            let ordinal = front.ticket.ordinal;
            let context_update = front.context_update;
            self.note_retirement(ordinal, context_update);
            let front = self.active.front_mut().ok_or(EIO)?;
            if !front.flist_completed {
                host.complete_render(
                    (!front.qos_completed).then_some(qos),
                    pool,
                    front.generation,
                    buffer,
                )?;
                front.qos_completed = true;
                front.flist_completed = true;
            }
            defer(Deferred::Retired(
                front.packet.completion.clone(),
                Ok(timestamps),
            ))?;
            self.memory.retire(ordinal)?;
            self.active.pop().ok_or(EIO)?;
            return Ok(true);
        }
    }

    /// A closed preparation epoch invalidates only this unpublished descriptor.
    /// Its completion remains available for a retry under the next epoch.
    pub(crate) fn discard_prepared(&mut self, packet: &Arc<Packet>) -> Result {
        let Some(prepared) = self.prepared.as_ref() else {
            return Ok(());
        };
        if !Arc::ptr_eq(&prepared.packet, packet) {
            return Err(EBUSY);
        }
        self.memory.retire(prepared.ordinal)?;
        self.prepared = None;
        Ok(())
    }
}

impl Pair {
    /// Call before and again after servicing firmware events. FList completion
    /// counters are full-width, including beyond the low-word wrap boundary.
    pub(crate) fn firmware_drained(&self, host: &impl Host) -> Result<bool> {
        if self.prepared.is_some() || self.active.iter().any(|a| !a.outer_published) {
            return Ok(false);
        }
        let counters = self.free_list.counters()?;
        let stamps = host.stamps(self.qids())?;
        let (_, state) = host.epoch()?;
        Ok(retirement::drained(self.ordinal, stamps, counters, state))
    }

    /// Account one witnessed tracker, preserving all completion ownership.
    /// The caller services its pending FList release before accounting another
    /// tracker and invokes release_witnessed only after that control stage.
    pub(crate) fn account_drained(&mut self, host: &mut impl Host) -> Result<bool> {
        use retirement::Memory;
        if !self.firmware_drained(host)? {
            return Ok(false);
        }
        let qids = self.qids();
        let qos = self.qos_owners();
        let buffer = self.buffer_id();
        let Some(active) = self.active.iter_mut().find(|a| !a.flist_completed) else {
            return Ok(false);
        };
        let mut observer = Observer {
            memory: &self.memory,
            qids,
            host,
        };
        let result = match retirement::poll(&mut active.ticket, &mut observer)? {
            retirement::Poll::Complete(ticks) => Ok(ticks),
            retirement::Poll::Pending | retirement::Poll::Retry => {
                core::sync::atomic::fence(core::sync::atomic::Ordering::Acquire);
                let snapshot = observer.snapshot(active.ticket.ordinal)?;
                let stamps = observer.stamps()?;
                let (_, state) = observer.host.epoch()?;
                if !retirement::abandoned(&active.ticket, snapshot, stamps, state, true)? {
                    return Ok(false);
                }
                Err(EIO)
            }
        };
        let ordinal = active.ticket.ordinal;
        let context_update = result.is_ok() && active.context_update;
        self.note_retirement(ordinal, context_update);
        let active = self
            .active
            .iter_mut()
            .find(|active| active.ticket.ordinal == ordinal)
            .ok_or(EIO)?;
        host.complete_render(
            (!active.qos_completed).then_some(qos),
            self.free_list.id(),
            active.generation,
            buffer,
        )?;
        active.qos_completed = true;
        active.flist_completed = true;
        active.drained_result = Some(result);
        Ok(true)
    }

    /// Fresh proof after asynchronous control publication is the ownership
    /// boundary. A refused deferred item leaves the precise owner available.
    pub(crate) fn release_witnessed(
        &mut self,
        host: &impl Host,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        if self.active.iter().any(|a| !a.flist_completed) || !self.firmware_drained(host)? {
            return Ok(());
        }
        while let Some(active) = self.active.front() {
            let result = active.drained_result.ok_or(EIO)?;
            defer(Deferred::Drained(active.packet.completion.clone(), result))?;
            let ordinal = active.ticket.ordinal;
            self.memory.retire(ordinal)?;
            self.active.pop().ok_or(EIO)?;
        }
        self.quarantined = false;
        self.resync();
        Ok(())
    }

    /// The registry first polls the completed prefix through this exact Arc.
    /// With successors, source policy quarantines every remaining group. A
    /// single unexecuted group may use the strict ownership witness instead.
    pub(crate) fn fail_after_observation(
        &mut self,
        packet: &Arc<Packet>,
        error: Error,
        host: &mut impl Host,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<bool> {
        use retirement::Memory;
        if !self.active.iter().any(|a| Arc::ptr_eq(&a.packet, packet)) {
            return Ok(false);
        }
        if !self.quarantined && self.active.len() == 1 {
            let active = self.active.front().ok_or(EIO)?;
            let mut observer = Observer {
                memory: &self.memory,
                qids: self.qids(),
                host,
            };
            core::sync::atomic::fence(core::sync::atomic::Ordering::Acquire);
            let snapshot = observer.snapshot(active.ticket.ordinal)?;
            let stamps = observer.stamps()?;
            let (_, state) = observer.host.epoch()?;
            if active.outer_published
                && retirement::abandoned(&active.ticket, snapshot, stamps, state, false)?
            {
                let qos = self.qos_owners();
                let buffer = self.buffer_id();
                let active = self.active.front_mut().ok_or(EIO)?;
                if !active.flist_completed {
                    host.complete_render(
                        (!active.qos_completed).then_some(qos),
                        self.free_list.id(),
                        active.generation,
                        buffer,
                    )?;
                    active.flist_completed = true;
                    active.qos_completed = true;
                }
                if !active.failed && !active.packet.completion.spared() {
                    active.packet.completion.status().record(EIO);
                    if self.terminal.is_none() {
                        self.terminal = Some((active.packet.completion.status().clone(), error));
                    }
                }
                defer(Deferred::Retired(
                    active.packet.completion.clone(),
                    Err(EIO),
                ))?;
                let ordinal = active.ticket.ordinal;
                self.memory.retire(ordinal)?;
                self.active.pop().ok_or(EIO)?;
                return Ok(true);
            }
        }
        self.quarantine(error, defer)?;
        Ok(true)
    }

    /// Both processors have stopped before the caller reaches this method.
    /// VM ownership settles before quarantined admission is rehabilitated.
    pub(crate) fn stopped(&mut self, defer: &mut impl FnMut(Deferred) -> Result) -> Result {
        if let Some(prepared) = self.prepared.as_ref() {
            defer(Deferred::Unpublished(
                prepared.packet.completion.clone(),
                EIO,
            ))?;
            self.memory.retire(prepared.ordinal)?;
            self.prepared = None;
        }
        while let Some(active) = self.active.front() {
            defer(Deferred::Drained(
                active.packet.completion.clone(),
                Err(EIO),
            ))?;
            let ordinal = active.ticket.ordinal;
            self.memory.retire(ordinal)?;
            self.active.pop().ok_or(EIO)?;
        }
        if let Some(previous) = self.previous.as_mut() {
            previous.ready = true;
        }
        Ok(())
    }
}

impl Pair {
    /// Commit a cold graph only after the registry revalidates its preparation
    /// lease. The original PB row is restored on every later failure.
    pub(crate) fn finish_install(
        &mut self,
        alloc: &Allocator<'_>,
        descriptors: &crate::g17::object::KernelObject,
    ) -> Result {
        install_memory(
            alloc,
            descriptors,
            self.slot,
            &mut self.memory,
            &mut self.pending_aliases,
            &mut self.kick_aliases,
        )
    }
}

fn install_memory(
    alloc: &Allocator<'_>,
    descriptors: &crate::g17::object::KernelObject,
    slot: u8,
    memory: &mut memory::Memory,
    pending_aliases: &mut Option<KVec<mmu::KernelMapping>>,
    kick_aliases: &mut Option<[Option<mmu::KernelMapping>; 2]>,
) -> Result {
    use core::sync::atomic::{fence, Ordering};
    if pending_aliases.is_none() && kick_aliases.is_none() {
        return Ok(());
    }
    if !memory.context.is_current() || kick_aliases.is_none() {
        return Err(EIO);
    }
    let mut driver = KVec::new();
    driver.reserve(2, GFP_KERNEL)?;
    let aliases = memory.take_persistent(pending_aliases.as_mut().ok_or(EIO)?)?;
    let context = memory.context.clone();
    let vm = context.vm();
    let pool_id = u64::from(memory.lifecycle[0].predecessor);
    let mut staged: Option<[u32; 4]> = None;
    let offset = usize::from(memory.manager.buffer_id()) * 16;
    let result: Result = (|| {
        vm.install_render_pool_mappings(slot, pool_id, memory.manager.blocks(), aliases)?;
        memory.installed_pool = Some(slot);
        alloc.uat.flush_vm_contexts(vm)?;
        let words = [
            descriptors.word(offset)?,
            descriptors.word(offset + 4)?,
            descriptors.word(offset + 8)?,
            descriptors.word(offset + 12)?,
        ];
        let previous = words.map(|word| word.load(Ordering::Relaxed));
        let pages: u32 = (memory.manager.blocks()
            * crate::hw::t8140::resources::TVB_PAGES_PER_BLOCK)
            .try_into()?;
        let next = crate::g17::fw::buffer::reload_descriptor(
            previous,
            memory.manager.page_list_va(),
            pages,
        );
        for (word, value) in words.into_iter().zip(next) {
            word.store(value, Ordering::Relaxed);
        }
        fence(Ordering::SeqCst);
        staged = Some(previous);
        for mapping in kick_aliases.take().ok_or(EIO)?.into_iter().flatten() {
            driver.push(mapping, GFP_KERNEL)?;
        }
        vm.append_driver_mappings(driver)?;
        Ok(())
    })();
    if let Err(error) = result {
        if error == ENOENT {
            context.status().record(error);
        }
        if let Some(previous) = staged {
            for (index, value) in previous.into_iter().enumerate() {
                descriptors
                    .word(offset + index * 4)?
                    .store(value, Ordering::Relaxed);
            }
            fence(Ordering::SeqCst);
        }
        vm.clear_render_pool_mappings_if_owner(slot, pool_id);
        memory.installed_pool = None;
        return Err(error);
    }
    *pending_aliases = None;
    Ok(())
}

/// A binding prepared while the registry exclusively lends the idle pair.
/// Its frontier prevents a stale off-lock result from replacing another owner.
pub(crate) struct PreparedBinding {
    previous_context: Arc<Context>,
    previous_owner: Option<u64>,
    ordinal: u64,
    owner: u64,
    context: Arc<Context>,
    pool: Arc<RenderPool>,
    memory: Option<KBox<memory::Memory>>,
    aliases: Option<KVec<mmu::KernelMapping>>,
    kicks: Option<[Option<mmu::KernelMapping>; 2]>,
    initial_packet: u64,
}

/// The registry retains displaced backing until this exact context's release
/// is acknowledged. A warm binding retains the old context without its graph.
pub(crate) struct RetiredBinding {
    memory: Option<KBox<memory::Memory>>,
    context: Arc<Context>,
    pool: Arc<RenderPool>,
    buffer_id: u8,
    publication: Option<u64>,
    ready: bool,
}
impl RetiredBinding {
    pub(crate) fn context(&self) -> &Arc<Context> {
        &self.context
    }
    pub(crate) fn pool(&self) -> &Arc<RenderPool> {
        &self.pool
    }
    pub(crate) fn buffer_id(&self) -> Option<u8> {
        self.memory.as_ref().map(|_| self.buffer_id)
    }
}

impl Pair {
    fn note_retirement(&mut self, ordinal: u64, context_update: bool) {
        if !context_update {
            return;
        }
        self.context_update = false;
        if let Some(previous) = self.previous.as_mut() {
            if previous
                .publication
                .is_some_and(|published| published <= ordinal)
            {
                previous.ready = true;
            }
        }
    }
    pub(crate) fn take_retired_binding(&mut self) -> Option<RetiredBinding> {
        if self.displaced.is_some() {
            return self.displaced.take();
        }
        if self.previous.as_ref().is_some_and(|p| p.ready) {
            self.previous.take()
        } else {
            None
        }
    }
    pub(crate) fn idle_for_context(&self, context: &Arc<Context>) -> Result<bool> {
        if !Arc::ptr_eq(context, self.context()) {
            return Ok(true);
        }
        if self.prepared.is_some() || self.in_flight() || self.quarantined {
            return Ok(false);
        }
        if self.ordinal == 0 {
            return Ok(true);
        }
        core::sync::atomic::fence(core::sync::atomic::Ordering::Acquire);
        Ok(self
            .memory
            .graph
            .cursors()?
            .into_iter()
            .all(|c| c.done == c.read && c.read == c.write))
    }
    pub(crate) fn reusable(&self) -> Result<bool> {
        let previous_ready = self
            .previous
            .as_ref()
            .is_none_or(|p| p.memory.is_none() && self.context_update && !p.ready);
        Ok(self.owner.is_none()
            && self.displaced.is_none()
            && previous_ready
            && self.idle_for_context(self.context())?)
    }
    /// Logical close leaves the physical channel identity and backing retained.
    pub(crate) fn release_owner(&mut self) {
        self.owner = None;
    }
    #[inline(never)]
    pub(crate) fn prepare_bind(
        &mut self,
        alloc: &Allocator<'_>,
        pool: &Arc<Pool>,
        owner: u64,
        context: Arc<Context>,
        free_list: Arc<RenderPool>,
        metrics: Option<buffer::MetricsLease>,
        buffer_id: Option<(u8, u64)>,
        pm_generation: Option<u64>,
        first: &Arc<Packet>,
    ) -> Result<PreparedBinding> {
        if !self.reusable()?
            || owner == 0
            || !Arc::ptr_eq(&context, &first.context)
            || !context.is_current()
        {
            return Err(EBUSY);
        }
        alloc.uat.vm_context_mask(context.vm())?;
        let same_vm = Arc::ptr_eq(self.context().status(), context.status());
        let (memory, aliases, kicks) = if same_vm {
            if metrics.is_some() || buffer_id.is_some() || pm_generation.is_some() {
                return Err(EINVAL);
            }
            (None, None, None)
        } else {
            let metrics = metrics.ok_or(EINVAL)?;
            let buffer_id = buffer_id.ok_or(EINVAL)?;
            let pm_generation = pm_generation.ok_or(EINVAL)?;
            let Validated::Render { pass, .. } = &first.command else {
                return Err(EINVAL);
            };
            let layout = target::Layout::new(pass, self.clusters)?;
            let aliases = [
                self.kicks[0].prepare_client_alias(context.vm())?,
                self.kicks[1].prepare_client_alias(context.vm())?,
            ];
            let (memory, persistent) = memory::Memory::new(
                alloc,
                pool,
                context.clone(),
                self.qids(),
                &layout,
                metrics,
                buffer_id,
                pm_generation,
                self.ordinal,
                first.completion.work_state().word(),
            )?;
            (Some(memory), Some(persistent), Some(aliases))
        };
        Ok(PreparedBinding {
            previous_context: self.context().clone(),
            previous_owner: self.owner,
            ordinal: self.ordinal,
            owner,
            context,
            pool: free_list,
            memory,
            aliases,
            kicks,
            initial_packet: first.id,
        })
    }
    pub(crate) fn bind_owner(
        &mut self,
        binding: &mut PreparedBinding,
        alloc: &Allocator<'_>,
        descriptors: &crate::g17::object::KernelObject,
    ) -> Result {
        if !self.reusable()?
            || binding.ordinal != self.ordinal
            || binding.previous_owner != self.owner
            || !Arc::ptr_eq(&binding.previous_context, self.context())
            || !binding.context.is_current()
        {
            return Err(EBUSY);
        }
        let foreign = binding.memory.is_some();
        if let Some(memory) = binding.memory.as_mut() {
            install_memory(
                alloc,
                descriptors,
                self.slot,
                memory,
                &mut binding.aliases,
                &mut binding.kicks,
            )?;
        } else {
            self.memory.set_owner(self.qids(), &binding.context)?;
        }
        let old_context = self.context().clone();
        let old_buffer = self.buffer_id();
        let old_pool = core::mem::replace(&mut self.free_list, binding.pool.clone());
        let old_memory = binding
            .memory
            .take()
            .map(|memory| core::mem::replace(&mut self.memory, memory));
        let retired = RetiredBinding {
            memory: old_memory,
            context: old_context,
            pool: old_pool,
            buffer_id: old_buffer,
            publication: None,
            ready: foreign,
        };
        if foreign {
            self.displaced = Some(retired);
        } else if self.previous.is_none() {
            self.previous = Some(retired);
        }
        self.memory.context = binding.context.clone();
        self.owner = Some(binding.owner);
        self.initial_packet = binding.initial_packet;
        self.context_update = true;
        if foreign {
            self.cursors = [0; 2];
            self.configs = [None; 2];
        }
        Ok(())
    }
}
