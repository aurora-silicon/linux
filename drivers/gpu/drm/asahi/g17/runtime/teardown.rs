// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Scheduler release and off-lock destruction of acknowledged logical queue resources.

use super::{ComputeEntry, Registry};
use crate::g17::{
    buffer::BufferIds,
    channel::Rings,
    context::Context,
    fw::channels::{ControlRecord, SchedulerStateRelease},
    initdata::InitData,
    queue::render::RetiredBinding,
    recovery::Memory as _,
    teardown::{Failure, Host},
    Coprocessor, Firmware, MSG_CONTROL_NOTIFY,
};
use crate::hw::t8140::queues::RENDER_SLOTS;
use core::sync::atomic::Ordering;
use kernel::{prelude::*, sync::Arc};

const CONTEXTS: usize = 128;

/// Pop under the reclaim mutex, then finish after dropping every device lock.
pub(in crate::g17) enum Reclaim {
    Binding(RetiredBinding),
    /// A parked M5 lane's last owner binding: its aliases in that owner's VM and its context.
    M5(crate::g17::queue::m5::Binding),
    Context(Arc<Context>),
}

impl Reclaim {
    pub(in crate::g17) fn finish(self) {
        match self {
            Self::Binding(binding) => drop(binding),
            Self::M5(binding) => drop(binding),
            Self::Context(context) => {
                context.release_execution();
                drop(context.take_render_pool());
                drop(context);
            }
        }
    }
}

/// All slots are allocated at probe. A full collector postpones detach. Empty
/// slots let a validated transfer move ownership without a fallible vector push.
pub(in crate::g17) struct ReclaimBatch {
    bindings: KVVec<Option<RetiredBinding>>,
    m5: KVec<Option<crate::g17::queue::m5::Binding>>,
    contexts: KVec<Option<Arc<Context>>>,
}

impl ReclaimBatch {
    pub(in crate::g17) fn new() -> Result<Self> {
        let mut bindings = KVVec::with_capacity(CONTEXTS + RENDER_SLOTS, GFP_KERNEL)?;
        for _ in 0..CONTEXTS + RENDER_SLOTS {
            bindings.push(None, GFP_KERNEL)?;
        }
        let mut contexts = KVec::with_capacity(CONTEXTS, GFP_KERNEL)?;
        let mut m5 = KVec::with_capacity(CONTEXTS, GFP_KERNEL)?;
        for _ in 0..CONTEXTS {
            contexts.push(None, GFP_KERNEL)?;
            m5.push(None, GFP_KERNEL)?;
        }
        Ok(Self {
            bindings,
            m5,
            contexts,
        })
    }

    pub(in crate::g17) fn is_empty(&self) -> bool {
        self.bindings.iter().all(Option::is_none)
            && self.m5.iter().all(Option::is_none)
            && self.contexts.iter().all(Option::is_none)
    }

    /// Graphs go before the Context owners that armed their scheduler releases.
    pub(in crate::g17) fn take_one(&mut self) -> Option<Reclaim> {
        if let Some(slot) = self.bindings.iter_mut().find(|slot| slot.is_some()) {
            return slot.take().map(Reclaim::Binding);
        }
        if let Some(slot) = self.m5.iter_mut().find(|slot| slot.is_some()) {
            return slot.take().map(Reclaim::M5);
        }
        self.contexts
            .iter_mut()
            .find(|slot| slot.is_some())
            .and_then(Option::take)
            .map(Reclaim::Context)
    }

    pub(in crate::g17::runtime) fn has_room(&self, bindings: usize, contexts: usize) -> bool {
        self.bindings.iter().filter(|slot| slot.is_none()).count() >= bindings
            && self.contexts.iter().filter(|slot| slot.is_none()).count() >= contexts
    }

    pub(in crate::g17::runtime) fn binding_slot(&mut self) -> Result<&mut Option<RetiredBinding>> {
        self.bindings
            .iter_mut()
            .find(|slot| slot.is_none())
            .ok_or(EBUSY)
    }

    /// Room for `count` M5 bindings.
    fn m5_room(&self, count: usize) -> bool {
        self.m5.iter().filter(|slot| slot.is_none()).count() >= count
    }

    fn m5_binding(&mut self, binding: crate::g17::queue::m5::Binding) -> Result {
        let slot = self
            .m5
            .iter_mut()
            .find(|slot| slot.is_none())
            .ok_or(EBUSY)?;
        *slot = Some(binding);
        Ok(())
    }

    fn context(&mut self, context: &Arc<Context>) -> Result {
        let slot = self
            .contexts
            .iter_mut()
            .find(|slot| slot.is_none())
            .ok_or(EBUSY)?;
        *slot = Some(context.clone());
        Ok(())
    }
}

struct Service<'a> {
    render: &'a mut super::render::RenderState,
    admission: &'a crate::g17::queue::admission::Pool,
    objects: &'a Arc<crate::g17::object::Pool>,
    /// The compute entries: Neo's compute queues and the M5 Ultra's bound compute channels.
    compute: &'a mut [Option<ComputeEntry>],
    buffers: &'a mut BufferIds,
    init: &'a InitData,
    primary: &'a mut Coprocessor,
    reclaim: &'a mut ReclaimBatch,
    /// The M5 Ultra's lanes, in the compute entries and render slots above (`queue::m5`); None
    /// on every other GPU.
    m5: Option<super::m5::Lanes>,
}

impl Host for Service<'_> {
    fn render_idle(&mut self, context: &Arc<Context>) -> Result<bool> {
        if let Some(m5) = &self.m5 {
            if !m5.render_idle(&self.render.m5, context) {
                return Ok(false);
            }
        }
        self.render.teardown_idle(context)
    }
    fn compute_idle(&self, context: &Arc<Context>) -> bool {
        // The M5 channels among the entries first; there are none off the M5 Ultra.
        super::m5::compute_idle(self.compute, context)
            && self
                .compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .all(|queue| queue.idle_for_context(context) && !queue.switching_from(context))
    }
    fn flists_released(&self, context: &Arc<Context>) -> bool {
        self.render.teardown_pools_released(context)
    }
    fn control_backpressured(&self) -> bool {
        self.render.control_backpressured()
    }
    fn publish(&mut self, record: &SchedulerStateRelease) -> Result<u32> {
        let (page_va, id) = (record.page_va, record.id);
        let release = match self.m5 {
            // M5 releases under its own opcode.
            Some(_) => SchedulerStateRelease::m5(page_va, id),
            None => SchedulerStateRelease::new(page_va, id),
        };
        Rings::publish_control(self.init, &ControlRecord::SchedulerStateRelease(release))
    }
    fn notify(&mut self) -> Result {
        self.primary.notify(MSG_CONTROL_NOTIFY)
    }
    fn detach(&mut self, context: &Arc<Context>) -> Result {
        let mut retained = false;
        if let Some(m5) = &mut self.m5 {
            // Every compute lane of the context parks; its bindings drop off the device mutex.
            let count = m5.parkable(self.compute, &self.render.m5, context)?;
            if !self.reclaim.m5_room(count) || !self.reclaim.has_room(0, 1) {
                return Err(EBUSY);
            }
            let reclaim = &mut *self.reclaim;
            m5.park(
                self.compute,
                &mut self.render.m5,
                self.admission,
                context,
                self.buffers,
                &mut |binding| reclaim.m5_binding(binding),
            )?;
            retained = m5.retains(self.compute, &self.render.m5, context);
        }
        let linked = context.retain_linked_work(self.objects);
        let (_, _, slots) = self.render.exit_scope(context);
        crate::g17::trace::teardown(5, u64::from(context.id()), slots, u64::from(linked));
        self.render
            .detach_context(context, self.buffers, self.reclaim)?;
        // The firmware consumed this context's scheduler release after its kill (if any)
        // was acknowledged and its pairs went idle: it no longer names these pairs, so
        // their kill hold ends here rather than at the next processor stop.
        self.admission.release_killed_slots(slots);
        if retained {
            // A quarantined M5 lane keeps the context and its execution context: the firmware
            // may still name them.
            return Ok(());
        }
        // detach_context preflights this slot together with every graph slot.
        // Retain it before Pending removes its reference under the device lock.
        self.reclaim.context(context)
    }
    fn report(&mut self, context: &Arc<Context>, failure: Failure) {
        let dev = self.primary.state.shared.dev.as_ref();
        match failure {
            Failure::Publication(error) => dev_err!(
                dev,
                "Scheduler {} release failed: {:?}\n",
                context.id(),
                error
            ),
            Failure::Announcement(error) => dev_warn!(
                dev,
                "Scheduler {} release announcement failed: {:?}\n",
                context.id(),
                error
            ),
        }
    }
}

impl Firmware {
    /// The event worker has already reclaimed drained pairs and reannounced
    /// pending controls. Recovery blocks new releases, never an observed ack.
    pub(in crate::g17) fn service_teardown(&mut self, reclaim: &mut ReclaimBatch) -> Result {
        self.queues.render.collect_reclaimable(reclaim)?;
        if self.queues.teardown.is_empty() {
            return Ok(());
        }
        self.queues.render.observe_teardown_releases(&self.init)?;
        let recovery = self.recovery.pending() || self.init.recovery_state()? != 0;
        let m5 = self.primary.state.shared.m5.is_some();
        let Registry {
            admission,
            render,
            compute,
            buffers,
            teardown,
            ..
        } = &mut self.queues;
        let mut service = Service {
            render,
            admission,
            objects: &self.pool,
            compute,
            buffers,
            init: &self.init,
            primary: &mut self.primary,
            reclaim,
            m5: m5.then_some(super::m5::Lanes),
        };
        teardown.publish_ready(&mut service, recovery, super::now_ns())?;
        let consumer = self.init.control_consumer()?.load(Ordering::Relaxed);
        let producer = self.init.control_producer()?.load(Ordering::Relaxed);
        let mut consumed = false;
        while teardown
            .take_consumed(consumer, producer, &mut service)?
            .is_some()
        {
            consumed = true;
        }
        // An M5 context still closing 2 s after it was armed says once what holds it: every
        // later queue's lanes depend on its release.
        if m5 {
            const STALL_NS: u64 = 2_000_000_000;
            while let Some((context, target)) = teardown.take_stalled(super::now_ns(), STALL_NS) {
                pr_warn!(
                    "G17 M5: context {} still closing: release {:?} (control {}/{}), recovery {}, render idle {:?}, pools released {}, compute idle {}, backpressured {}\n",
                    context.id(),
                    target,
                    consumer,
                    producer,
                    recovery,
                    service.render_idle(&context),
                    service.flists_released(&context),
                    service.compute_idle(&context),
                    service.control_backpressured(),
                );
                if let Some(m5) = &service.m5 {
                    m5.report(service.compute, &service.render.m5, &context);
                }
            }
        }
        if consumed {
            // A channel whose last scheduler release is consumed can change owner.
            self.primary.state.shared.channels.notify();
        }
        Ok(())
    }
}
