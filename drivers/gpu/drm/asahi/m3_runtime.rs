// SPDX-License-Identifier: GPL-2.0-only OR MIT
use core::{mem::ManuallyDrop, sync::atomic::Ordering};
use kernel::{device::Core, platform, prelude::*, soc::apple::rtkit, sync::Arc,
    time::{Instant, Monotonic, Delta, delay::fsleep}};
use crate::{driver, mmu, m3_device::Device, m3_config::Config, m3_rtkit};

#[path = "m3_batch_policy.rs"]
pub(crate) mod policy;
const _: () = assert!(policy::SLOTS == crate::m3_pass_layout::SLOTS);

enum NativeJob {Compute(crate::m3_compute::Compute),Render(crate::m3_render::Render)}
/// Completion-worker outcome.
pub(crate) enum Service {Idle,Progress,Waiting(u64)}
/// The published batch: its commands in order, as (packet, command index).
struct Batch {
    entries: KVec<(Arc<crate::m3_submit::Packet>,usize)>,
    index: usize, kind: usize, first: crate::m3_submit::Command,
    preparation_ns: i64, previous_events: u64, expected_events: u64,
    start: Instant<Monotonic>, measure: bool, doorbell_counter_ns: i64,
    /// Render: first pass slot and the batch's last draw number.
    base: usize, last_draw: u64, publication_tick:u64,
    polling_ns: i64, polls: u64, stamp_ns: i64, first_event_ns: i64, events_ns: i64,
}
#[derive(Clone, Copy, Default)]
struct GeometryTiming {
    width: u16, height: u16, samples: u8, count: u64,
    prepare_ns: i64, active_ns: i64, gpu_ns: u64,
    tiling_ns: u64, fragment_ns: u64, max_fragment_ns: u64,
    polling_ns: i64, retirement_ns: i64, sleeping_ns: i64, polls: u64,
    stamp_ns:i64, first_event_ns:i64, events_ns:i64, pickup_ns:i64, tail_ns:i64,
}
impl NativeJob {
    fn complete(&mut self)->Result<bool> {match self {Self::Compute(j)=>j.complete(),Self::Render(j)=>j.complete()}}
    fn log(&mut self,dev:&driver::AsahiDevice)->Result {match self {Self::Compute(j)=>j.log(dev),Self::Render(j)=>j.log(dev)}}
    fn batch_gpu_span(&mut self)->Result<[u64;2]> {match self {Self::Compute(j)=>j.batch_gpu_span(),Self::Render(j)=>j.batch_gpu_span()}}
}
struct Inner {
    transport: rtkit::RtKit<m3_rtkit::Operations>,
    state: Arc<m3_rtkit::State>,
    jobs: KVec<NativeJob>,
    packets: KVec<Arc<crate::m3_submit::Packet>>,
    /// Packets not yet (fully) published: (packet, next command index), most urgent first.
    pending: KVec<(Arc<crate::m3_submit::Packet>,usize)>,
    /// Published batches, oldest first. More than one only with asahi.m3_pipeline_depth > 1.
    active: KVec<Batch>,
    gpu_pending: bool,
    fault_captured: bool,
    timing: [[i64;7];2],
    render_batches: [u64;crate::m3_pass_layout::SLOTS],
    geometry: [GeometryTiming; 32],
    geometry_overflow: u64,
    /// Whether a T8122 job verdict line was already emitted for the current job, so the
    /// execute-path fallback does not add a second one. T8122 only.
    t8122_verdict: bool,
    /// The step the firmware boot sequence has reached, for the T8122 boot verdict. T8122 only.
    boot_step: crate::t8122_start::BootStep,
    /// Packets that joined a batch started by an earlier packet (debug summary).
    coalesced_packets: u64,
    /// Recent measured render GPU time per pass by VM. Smoothed over batches;
    /// replacing the least recently measured VM does not retain its mappings.
    pass_cost: policy::PassCosts,
    /// Latched only after the isolated InitBM draw fully retired.
    render_initialized: bool,
    config: Config,
    uat: mmu::Uat,
    drm: driver::AsahiDevRef,
    device: Device,
}
impl Inner {
    /// Latch before allocating diagnostics, once per runtime. Called under
    /// its mutex even for errors outside the active-job polling loop.
    fn capture_fault(&mut self, primary: Error) {
        self.state.health.mark_failed();
        if self.fault_captured { return; }
        self.fault_captured = true;
        #[cfg(CONFIG_DEV_COREDUMP)]
        if let Err(error) = self.config.fault_snapshot(&self.drm, primary, self.gpu_pending) {
            dev_err!(self.drm.as_ref(), "M3 fault snapshot failed: {:?}\n", error);
        }
    }

    /// Record a batch that failed or never retired. Mark the GPU failed first,
    /// so that a failing diagnostic read cannot leave a job that never retired
    /// behind a device that still reports itself healthy.
    fn fail(&mut self, index: usize, vm: &mmu::Vm, primary: Error) {
        self.capture_fault(primary);
        if crate::t8122_start::is_t8122(self.device.soc()) {
            let pstate=self.device.pstate_register().ok();
            // The job's first GPU start timestamp: nonzero means the firmware dispatched it,
            // whatever the power state reads at the timeout.
            let start_stamp=self.jobs[index].batch_gpu_span().ok().map(|span| span[0]);
            crate::t8122_start::job_failed_verdict(self.drm.as_ref(),primary,start_stamp,pstate,self.state.health.crashed());
            self.t8122_verdict=true;
        }
        let events=self.state.event_messages.load(Ordering::Acquire);
        if let Err(e) = self.jobs[index].log(&self.drm) {
            dev_err!(self.drm.as_ref(), "M3 job diagnostics failed: {:?}\n", e);
        }
        if let Err(e) = self.device.log_engine_state(vm) {
            dev_err!(self.drm.as_ref(), "M3 engine diagnostics failed: {:?}\n", e);
        }
        let _=self.config.log_recovery_state(&self.drm,events);
        if crate::m3_params::g15_debug(crate::m3_params::G15Debug::M3ResumeAfterFault) {
            if let Err(e)=self.resume_experiment(index, vm) {
                dev_err!(self.drm.as_ref(),"M3: resume-after-fault: stopped by {:?}\n",e);
            }
        }
    }
    /// asahi.g15_debug bit 56. Wait up to 100 ms for the firmware to report
    /// that it halted, resume it, and log every change of the halt words,
    /// the engine state, the failed job's retirement and the event count
    /// for about half a second. The GPU stays marked failed.
    fn resume_experiment(&mut self, index: usize, vm: &mmu::Vm) -> Result {
        let start=Instant::<Monotonic>::now();
        let mut halt=self.config.halt_state()?;
        while halt.1==0 && start.elapsed()<Delta::from_millis(100) {
            fsleep(Delta::from_millis(1));
            halt=self.config.halt_state()?;
        }
        if halt.1==0 {
            dev_info!(self.drm.as_ref(),"M3: resume-after-fault: the firmware did not halt within 100 ms (halt_count={} resume={}); not resuming\n",halt.0,halt.2);
            return Ok(());
        }
        dev_info!(self.drm.as_ref(),"M3: resume-after-fault: the firmware halted (halt_count={} halted={}); clearing halted and setting resume\n",halt.0,halt.1);
        self.config.resume_halted()?;
        let resumed=Instant::<Monotonic>::now();
        let mut last=None;
        for _ in 0..500 {
            let halt=self.config.halt_state()?;
            // 0: idle and no fault bits, 1: an engine is busy, 2: fault bits set.
            let engines=match self.device.check_idle() {Ok(())=>0u8,Err(e) if e==EBUSY=>1,Err(_)=>2};
            let complete=self.jobs[index].complete().unwrap_or(false);
            let events=self.state.event_messages.load(Ordering::Acquire);
            let crashed=self.state.health.crashed();
            let now=(halt,engines,complete,events,crashed);
            if last!=Some(now) {
                dev_info!(self.drm.as_ref(),"M3: resume-after-fault: +{}us: halt_count={} halted={} resume={} engines={} job_complete={} event_messages={} crashed={}\n",
                    resumed.elapsed().as_nanos()/1000,halt.0,halt.1,halt.2,engines,complete,events,crashed);
                last=Some(now);
            }
            if crashed {break;}
            fsleep(Delta::from_millis(1));
        }
        self.jobs[index].log(&self.drm)?;
        self.device.log_engine_state(vm)?;
        let events=self.state.event_messages.load(Ordering::Acquire);
        self.config.log_recovery_state(&self.drm,events)
    }
}
impl Inner {
    /// Fail every packet that was never published. Their fences signal with the error.
    fn fail_pending(&mut self, error: Error) {
        for (packet,_) in self.pending.drain_all() {packet.finish(Err(error));}
    }
    /// A published batch failed or never retired: latch the fault, log it, and fail its
    /// packets. The packets stay retained (with their VM job guards) until teardown, because
    /// the firmware has not provably stopped using their memory.
    fn fail_active(&mut self, error: Error) {
        if let Some((index,vm))=self.active.first().map(|b|(b.index,b.entries[0].0.vm.clone())) {
            self.fail(index,&vm,error);
        } else {self.capture_fault(error);}
        for batch in self.active.drain_all() {Self::finish_all(&batch.entries,Err(error));}
        self.fail_pending(error);
    }
    /// Hardware completion is separate from fence signaling: cancellation can win the
    /// signal race while the DMA guard must survive until its final published batch retires.
    fn packet_active(&self, packet:&Arc<crate::m3_submit::Packet>)->bool {
        self.active.iter().any(|b|b.entries.iter().any(|(p,_)|Arc::ptr_eq(p,packet)))
    }
    fn retire_packets(&mut self,entries:&[(Arc<crate::m3_submit::Packet>,usize)]) {
        for (packet,_) in entries {
            let pending=self.pending.iter().any(|(p,_)|Arc::ptr_eq(p,packet));
            if policy::packet_retired(pending,self.packet_active(packet)) {packet.retired();}
        }
        // Keep exactly active DMA owners. Successful retirement may precede a VM switch.
        self.packets.retain(|p| self.active.iter().any(|b|b.entries.iter().any(|(q,_)|Arc::ptr_eq(p,q))));
    }
    fn fail_front(&mut self,error:Error) {
        if let Ok((packet,_))=self.pending.remove(0) {
            if !self.packet_active(&packet) && !self.fault_captured {packet.release_guard();}
            packet.finish(Err(error));
        }
    }
    /// Signal every packet of `entries` (packets that continue past the batch included).
    fn finish_all(entries:&[(Arc<crate::m3_submit::Packet>,usize)],result:Result) {
        for (packet,_) in entries {packet.finish(result);}
    }
    /// Choose the next batch from the front of `pending`: the front packet's next commands of
    /// one engine, and for render also the leading render commands of following packets of the
    /// same VM, up to the ordered-batch limit.
    fn next_batch(&self,max:usize)->Result<KVec<(Arc<crate::m3_submit::Packet>,usize)>> {
        let mut entries=KVec::new();
        let Some((packet,next))=self.pending.first() else {return Ok(entries);};
        let render=matches!(packet.commands[*next],crate::m3_submit::Command::Render{..});
        let limit=if render {
            // The first render draw initializes the shared buffer manager (InitBM) and opens
            // both queues. Publish it alone: appending later draws first advances the shared
            // manager and event counters it starts from, and on J613 the second pass's
            // fragment then fails with firmware error event 4 (draw=2 batch=2).
            let started=self.jobs.iter().any(|job| matches!(job,NativeJob::Render(j) if j.started()));
            if started {crate::m3_params::render_batch_size().min(max).max(1)} else {1}
        } else {
            crate::m3_params::compute_batch_size()
                .clamp(1,crate::m3_compute_storage::SLOTS)
        };
        let same_engine=|c:&crate::m3_submit::Command| render==matches!(c,crate::m3_submit::Command::Render{..});
        let per_pass=self.pass_cost.get(packet.vm.id());
        let budget=crate::m3_params::render_batch_budget_ns(self.device.soc());
        let in_flight:usize=self.active.iter().map(|b|b.entries.len()).sum();
        for (position,(candidate,start)) in self.pending.iter().enumerate() {
            // Later packets join only render batches, only from their first command, and only
            // for the VM the batch is bound to, while the batch's expected GPU time is within
            // the budget.
            if position>0 && !policy::may_join(render,*start,candidate.vm.same(&packet.vm),candidate.urgency==packet.urgency) {break;}
            let mut index=*start;
            while index<candidate.commands.len() && entries.len()<limit && same_engine(&candidate.commands[index]) {
                if render && !policy::within_budget(per_pass,budget,in_flight+entries.len()) {return Ok(entries);}
                entries.push((candidate.clone(),index),GFP_KERNEL)?;
                index+=1;
            }
            // Stop at the batch limit or where a packet continues with other commands.
            if entries.len()>=limit || index<candidate.commands.len() {break;}
        }
        Ok(entries)
    }
    /// Whether the front pending work may be published behind the in-flight batches, and if so
    /// in which pass slot it starts and how many passes it may use.
    fn pipeline_slot(&mut self)->Result<Option<(usize,usize)>> {
        let Some(last)=self.active.last() else {return Ok(Some((0,crate::m3_pass_layout::SLOTS)));};
        if !self.render_initialized {return Ok(None);}
        let Some((packet,next))=self.pending.first() else {return Ok(None);};
        if !matches!(packet.commands[*next],crate::m3_submit::Command::Render{..})
            || self.active.iter().any(|b| b.kind!=0) || !packet.vm.same(&last.entries[0].0.vm) {return Ok(None);}
        let in_flight:usize=self.active.iter().map(|b|b.entries.len()).sum();
        let Some(room)=policy::pipeline_room(crate::m3_params::pipeline_depth(self.device.soc()),
            self.active.len(),in_flight,last.base,last.entries.len()) else {return Ok(None);};
        // Yield at the next retirement when more urgent work is already waiting.
        if self.pending.iter().any(|(p,_)|p.urgency>packet.urgency) {return Ok(None);}
        if !policy::within_budget(self.pass_cost.get(packet.vm.id()),
            crate::m3_params::render_batch_budget_ns(self.device.soc()),in_flight) {return Ok(None);}
        if !policy::overlap_ready(self.render_initialized,true,true,true,
            self.config.pipe_free(0)? && self.config.pipe_free(1)?) {return Ok(None);}
        Ok(Some(room))
    }
    /// Publish pending batches while allowed: one when none is active, more behind it with
    /// asahi.m3_pipeline_depth > 1. Returns whether anything was published or failed.
    fn publish_next(&mut self)->bool {
        let mut progress=false;
        while !self.pending.is_empty() && self.state.healthy() {
            let overlap=!self.active.is_empty();
            let (base,max)=match self.pipeline_slot() {
                Ok(Some(slot))=>slot,
                Ok(None)=>break,
                Err(error)=>{self.fail_active(error);return true;}
            };
            let entries=match self.next_batch(max) {
                Ok(entries) if !entries.is_empty()=>entries,
                _=>{self.fail_front(ENOMEM);progress=true;continue;},
            };
            match self.publish(&entries,base,overlap) {
                Ok(batch)=>{
                    // Consume the published commands from `pending`.
                    let Some((last,last_index))=entries.last().map(|(p,i)|(p.clone(),*i)) else {break;};
                    while let Some((packet,_))=self.pending.first() {
                        if Arc::ptr_eq(packet,&last) {
                            if last_index+1==last.commands.len() {let _=self.pending.remove(0);}
                            else {self.pending[0].1=last_index+1;}
                            break;
                        }
                        let _=self.pending.remove(0);
                    }
                    if self.active.push(batch,GFP_KERNEL).is_err() {
                        // Published but untracked: it can never be retired.
                        self.capture_fault(ENOMEM);Self::finish_all(&entries,Err(ENOMEM));
                        self.fail_active(ENOMEM);return true;
                    }
                    progress=true;
                }
                Err((error,published))=>{
                    if published {
                        if crate::t8122_start::is_t8122(self.device.soc()) && !self.t8122_verdict {
                            crate::t8122_start::job_setup_failed_verdict(self.drm.as_ref(),error);
                            self.t8122_verdict=true;
                        }
                        // A doorbell may have been rung: latch the fault, keep the packets
                        // retained (self.packets) and fail them and everything in flight.
                        self.capture_fault(error);
                        Self::finish_all(&entries,Err(error));
                        self.fail_active(error);
                        self.fail_pending(error);
                        return true;
                    } else if overlap && error==EBUSY {
                        // Not publishable behind the running batch (new VM binding or storage
                        // growth): wait for it to retire.
                        break;
                    } else {
                        // Nothing reached the firmware: fail only the front packet, as a failed
                        // execute() did before publication.
                        self.fail_front(error);
                        progress=true;
                    }
                }
            }
        }
        progress
    }
    /// Prepare and publish one batch. On error, reports whether a doorbell may have been rung.
    fn publish(&mut self,entries:&[(Arc<crate::m3_submit::Packet>,usize)],base:usize,overlap:bool)->core::result::Result<Batch,(Error,bool)> {
        let early=|e:Error|(e,false);
        let (packet,first_index)=(&entries[0].0,entries[0].1);
        let control=packet.commands[first_index];
        let batch_count=entries.len();
        let prepare=Instant::<Monotonic>::now();
        // All host bookkeeping is reserved before mutating shared producer state.
        // No allocation may fail after publication and lose a DMA owner.
        self.active.reserve(1,GFP_KERNEL).map_err(|_|early(ENOMEM))?;
        self.packets.reserve(entries.len(),GFP_KERNEL).map_err(|_|early(ENOMEM))?;
        let mut owned=KVec::with_capacity(batch_count,GFP_KERNEL).map_err(|_|early(ENOMEM))?;
        for (p,i) in entries {owned.push((p.clone(),*i),GFP_KERNEL).map_err(|_|early(ENOMEM))?;}

        // Retain the batch's packets (and their VM job guards) until it retires; with batches
        // in flight, also those of the earlier ones.
        if !overlap {self.packets.clear();}
        for (p,_) in entries {
            if !self.packets.iter().any(|q| Arc::ptr_eq(q,p)) {self.packets.push(p.clone(),GFP_KERNEL).map_err(|_|early(ENOMEM))?;}
        }
        let found=self.jobs.iter().position(|job| matches!((job,control),
            (NativeJob::Compute(_),crate::m3_submit::Command::Compute(_)) |
            (NativeJob::Render(_),crate::m3_submit::Command::Render{..})));
        let index=found.unwrap_or(self.jobs.len());
        if found.is_none() {
            let job=match control {
                crate::m3_submit::Command::Compute(c)=>NativeJob::Compute(crate::m3_compute::Compute::new(&self.drm,&self.uat,&packet.vm,self.config.stats_region().map_err(early)?,c,self.device.soc().registers).map_err(early)?),
                crate::m3_submit::Command::Render{..}=>NativeJob::Render(crate::m3_render::Render::new(&self.drm,&self.uat,&packet.vm,self.config.stats_region().map_err(early)?,self.device.soc().clusters,self.device.soc().registers).map_err(early)?),
            };
            self.jobs.push(job,GFP_KERNEL).map_err(|_|early(ENOMEM))?;
        } else {
            match (&mut self.jobs[index],control) {
                (NativeJob::Render(_),crate::m3_submit::Command::Render{..})=>{},
                (NativeJob::Compute(j),crate::m3_submit::Command::Compute(c))=>j.replay(&self.uat,&packet.vm,c).map_err(early)?,
                _=>return Err(early(ENOTSUPP)),
            }
        }
        if let NativeJob::Compute(j)=&mut self.jobs[index] {
            let prepared=(||->Result {
                for (slot,(p,next)) in entries.iter().enumerate() {
                    if slot!=0 {
                        let crate::m3_submit::Command::Compute(c)=p.commands[*next] else {return Err(EINVAL);};
                        j.append(c)?;
                    }
                    let timestamps=p.timestamp_addresses(*next);
                    let coalesce=crate::m3_compute_storage::coalesce_stamp_flush(
                        p.wide_visibility[*next],timestamps,slot+1==batch_count);
                    j.prepare_completion(coalesce)?;
                    j.set_user_timestamps(timestamps)?;
                    j.set_attachments(&p.attachments[*next])?;
                }
                Ok(())
            })();
            if let Err(error)=prepared {
                j.abort_batch().map_err(|e|(e,true))?;
                if found.is_none() {drop(self.jobs.remove(index).map_err(|_|early(EIO))?);}
                return Err(early(error));
            }
        }
        if let NativeJob::Render(j)=&mut self.jobs[index] {
            let mut commands=KVec::with_capacity(batch_count,GFP_KERNEL).map_err(|_|early(ENOMEM))?;
            for (p,next) in entries {commands.push(p.commands[*next],GFP_KERNEL).map_err(|_|early(ENOMEM))?;}
            j.begin_batch(&self.drm,&self.uat,&packet.vm,&commands,base,overlap).map_err(early)?;
            let prepared=(||->Result {
                for (p,next) in entries {
                    let crate::m3_submit::Command::Render{command,usc}=p.commands[*next] else {return Err(EINVAL);};
                    j.append(command,usc)?;
                    j.set_user_timestamps(p.render_timestamp_addresses(*next))?;
                }
                Ok(())
            })();
            if let Err(error)=prepared {j.abort_batch().map_err(|e|(e,true))?;return Err(early(error));}
        }
        let preparation_ns=prepare.elapsed().as_nanos();
        self.t8122_verdict=false;
        let previous_events=self.config.completed_events;
        let publication_tick=if crate::debug::debug_enabled(crate::debug::DebugFlags::M3PassTiming) {physical_counter()} else {0};
        self.state.health.set_gpu_pending(true);
        self.gpu_pending = true;
        let late=|e:Error|(e,true);
        let expected_events=match &self.jobs[index] {
            NativeJob::Compute(j)=>{
                self.config.submit_queue(2,j.queue(),j.head(),2,j.first()).map_err(late)?;
                Pin::new(&mut self.transport).send_message(0x21,0x0083000000000002).map_err(late)?;1
            },
            NativeJob::Render(j)=>{
                if j.first() {self.config.render_pb().map_err(late)?;}
                let queues=j.queues();
                self.config.submit_queue(1,queues[1],j.heads()[1],1,j.first()).map_err(late)?;
                Pin::new(&mut self.transport).send_message(0x21,0x0083000000000001).map_err(late)?;
                self.config.submit_queue(0,queues[0],j.heads()[0],0,j.first()).map_err(late)?;
                Pin::new(&mut self.transport).send_message(0x21,0x0083000000000000).map_err(late)?;2
            },
        };
        let measure=crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary);
        let (base,last_draw)=match &self.jobs[index] {NativeJob::Render(j)=>{let (b,_,d)=j.batch();(b,d)},_=>(0,0)};
        Ok(Batch { entries:owned, index, kind:usize::from(matches!(control,crate::m3_submit::Command::Compute(_))),
            first:control, preparation_ns, previous_events, expected_events,
            start:Instant::<Monotonic>::now(), measure, doorbell_counter_ns:if measure {physical_counter() as i64 * 1000 / 24} else {0},
            polling_ns:0, polls:0, stamp_ns:-1, first_event_ns:-1, events_ns:-1, base, last_draw, publication_tick })
    }
    /// Check the active batch once. Ok(true): retired (its packets are signalled).
    fn poll_active(&mut self)->Result<bool> {
        let Some((index,measure,last_draw))=self.active.first().map(|b|(b.index,b.measure,b.last_draw)) else {return Ok(false);};
        let poll_start=measure.then(Instant::<Monotonic>::now);
        if !self.state.healthy() || self.config.drain(&self.drm).is_err() {
            self.fail_active(EIO);return Err(EIO);
        }
        // With later batches in flight, the oldest retires once its own last draw's stamps
        // have arrived; the last one is also checked for exact queue, event and engine idle.
        if self.active.len()>1 {
            let done=match &mut self.jobs[index] {NativeJob::Render(j)=>j.complete_through(last_draw),_=>Err(EINVAL)};
            match done {
                Ok(true)=>{
                    let batch=self.active.remove(0).map_err(|_|EIO)?;
                    if let Err(e)=self.retire(&batch) {
                        self.capture_fault(e);Self::finish_all(&batch.entries,Err(e));
                        self.fail_active(e);return Err(e);
                    }
                    self.retire_packets(&batch.entries);
                    return Ok(true);
                }
                Ok(false)=>{
                    let (expired,previous)=self.active.first().map(|b|(b.start.elapsed()>=Delta::from_secs(2),b.previous_events)).ok_or(EIO)?;
                    if expired {
                        dev_err!(self.drm.as_ref(),"M3 pipelined completion events={} previous={}\n",self.config.completed_events,previous);
                        self.fail_active(ETIMEDOUT);return Err(ETIMEDOUT);
                    }
                    return Ok(false);
                }
                Err(e)=>{self.fail_active(e);return Err(e);}
            }
        }
        let stamped=match self.jobs[index].complete() {Ok(v)=>v,Err(e)=>{self.fail_active(e);return Err(e);}};
        let completed=self.config.completed_events;
        let (evented,expired,previous)={
            let batch=self.active.first_mut().ok_or(EIO)?;
            let evented=completed>=batch.previous_events+batch.expected_events;
            if batch.measure {
                let at=batch.start.elapsed().as_nanos();
                if stamped && batch.stamp_ns<0 {batch.stamp_ns=at;}
                if completed>batch.previous_events && batch.first_event_ns<0 {batch.first_event_ns=at;}
                if evented && batch.events_ns<0 {batch.events_ns=at;}
            }
            if let Some(t)=poll_start {batch.polling_ns+=t.elapsed().as_nanos();batch.polls+=1;}
            (evented,batch.start.elapsed()>=Delta::from_secs(2),batch.previous_events)
        };
        if stamped && evented {
            let retire_mmio=crate::m3_params::retire_mmio(self.device.soc());
            match self.device.check_idle_parts(
                retire_mmio&crate::m3_params::RETIRE_MMIO_BUSY!=0,
                retire_mmio&crate::m3_params::RETIRE_MMIO_FAULTS!=0) {
                Ok(())=>match self.config.pipes_idle() {
                    Ok(true)=>{
                        if retire_mmio&crate::m3_params::RETIRE_MMIO_PSTATE!=0 {
                            if let Err(e)=self.config.check_pstate(&self.drm,&self.device,"after a job") {
                                if crate::t8122_start::is_t8122(self.device.soc()) {
                                    crate::t8122_start::cap_violated_verdict(self.drm.as_ref(),e);
                                    self.t8122_verdict=true;
                                }
                                self.fail_active(e);return Err(e);
                            }
                        }
                        // Stamps, both queue indices, required events,
                        // firmware health, engines and pipes are verified.
                        let batch=self.active.remove(0).map_err(|_|EIO)?;
                        if let Err(e)=self.retire(&batch) {
                            self.capture_fault(e);
                            Self::finish_all(&batch.entries,Err(e));
                            self.fail_pending(e);
                            return Err(e);
                        }
                        // Signal every packet whose last command was in this batch. A packet
                        // that continues with other commands is still at the front of `pending`.
                        self.retire_packets(&batch.entries);
                        return Ok(true);
                    },
                    Ok(false)=>{}, // Firmware must consume every submitted queue message.
                    Err(e)=>{self.fail_active(e);return Err(e);}
                },
                Err(e) if e==EBUSY=>{}, // Retirement can trail the event.
                Err(e)=>{self.fail_active(e);return Err(e);}
            }
        }
        if expired {
            dev_err!(self.drm.as_ref(),"M3 completion events={} previous={}\n",completed,previous);
            self.fail_active(ETIMEDOUT);return Err(ETIMEDOUT);
        }
        Ok(false)
    }
    /// Bookkeeping for a retired batch, then signal its completed packets.
    fn retire(&mut self,batch:&Batch)->Result {
        let retire_start=batch.measure.then(Instant::<Monotonic>::now);
        let index=batch.index;let batch_count=batch.entries.len();let control=batch.first;
        self.state.health.record_completion();
        if self.active.is_empty() {
            match &mut self.jobs[index] {
                NativeJob::Compute(j)=>j.progress(&self.drm)?,
                NativeJob::Render(j)=>j.progress(&self.drm)?,
            }
        }
        let retired_counter_ns=if batch.measure {physical_counter() as i64 * 1000 / 24} else {0};
        let ticks=match &mut self.jobs[index] {
            NativeJob::Render(j) if batch.measure=>Some(j.gpu_ticks(batch.base,batch_count)?),
            _=>None,
        };
        let stages=match &mut self.jobs[index] {
            NativeJob::Render(j)=>j.stage_ns(batch.base,batch_count)?,
            _=>[0;3],
        };
        let (kind,gpu_ns)=match &mut self.jobs[index] {
            NativeJob::Compute(j)=>(1,j.gpu_ns()?),
            NativeJob::Render(j)=>(0,j.gpu_ns(batch.base,batch_count)?),
        };
        if kind==0 {
            self.render_batches[batch_count-1]+=1;
            let vm=batch.entries[0].0.vm.id();
            self.pass_cost.record(vm,gpu_ns/batch_count as u64);
            self.render_initialized=true;
        }
        if crate::debug::debug_enabled(crate::debug::DebugFlags::M3PassTiming) {
            let observed_tick=physical_counter();
            let (cs,span)=match (&mut self.jobs[index],control) {
                (NativeJob::Compute(j),crate::m3_submit::Command::Compute(c))=>(c.base,j.batch_gpu_span()?),
                (NativeJob::Render(j),crate::m3_submit::Command::Render{command:r,..})=>{let (a,b)=j.gpu_ticks(batch.base,batch_count)?;(r.vdm_base,[a,b])},
                _=>return Err(EINVAL),
            };
            let valid=batch.publication_tick!=0 && span[0]>=batch.publication_tick && span[1]>=span[0] && observed_tick>=span[1];
            dev_info!(self.drm.as_ref(),"M3_BATCH_BOUNDARY cs={:#x} batch={} publication_tick={} gpu_start_tick={} gpu_end_tick={} observed_tick={} counter_hz=24000000 valid={}\n",
                cs,batch_count,batch.publication_tick,span[0],span[1],observed_tick,u32::from(valid));
            if let NativeJob::Compute(j)=&mut self.jobs[index] {
                for (slot,(p,next)) in batch.entries.iter().enumerate() {
                    let crate::m3_submit::Command::Compute(c)=p.commands[*next] else {return Err(EINVAL);};
                    let ns=j.slot_gpu_ns(slot)?;
                    dev_info!(self.drm.as_ref(),"M3_COMPUTE_SLOT slot={} batch={} cdm={:#x} end={:#x} usc={:#x} gpu_ns={}\n",slot,batch_count,c.base,c.end,c.usc_base,ns);
                }
            }
            if let NativeJob::Render(j)=&mut self.jobs[index] {
                for (slot,(p,next)) in batch.entries.iter().enumerate() {
                    let crate::m3_submit::Command::Render{command:r,..}=p.commands[*next] else {return Err(EINVAL);};
                    let s=j.slot_stage_ns(batch.base,batch_count,slot)?;
                    dev_info!(self.drm.as_ref(),"M3_PASS_SLOT slot={} batch={} vdm={:#x} width={} height={} samples={} ta_ns={} fragment_ns={} gap_ns={}\n",slot,batch_count,r.vdm_base,r.width,r.height,r.samples,s[0],s[1],s[2]);
                }
            }
        }
        let preparation_ns=batch.preparation_ns;
        let active_ns=batch.start.elapsed().as_nanos();
        self.state.health.timing.record(kind==0, batch_count as u64,
            preparation_ns, active_ns, gpu_ns, stages);
        let retirement_ns=retire_start.map_or(0,|t|t.elapsed().as_nanos());
        if batch.measure {
            if let crate::m3_submit::Command::Render {command:r,..}=control {
                if batch_count>1 {self.geometry_overflow+=1;} else {
                if let Some(g)=self.geometry.iter_mut().find(|g|
                    g.count==0 || (g.width==r.width && g.height==r.height && g.samples==r.samples)) {
                    g.width=r.width;g.height=r.height;g.samples=r.samples;
                    g.count+=1;g.prepare_ns+=preparation_ns;g.active_ns+=active_ns;
                    g.gpu_ns+=gpu_ns;g.tiling_ns+=stages[0];g.fragment_ns+=stages[1];
                    g.max_fragment_ns=g.max_fragment_ns.max(stages[1]);
                    g.polling_ns+=batch.polling_ns;g.retirement_ns+=retirement_ns;
                    g.polls+=batch.polls;
                    g.stamp_ns+=batch.stamp_ns;g.first_event_ns+=batch.first_event_ns;g.events_ns+=batch.events_ns;
                    if let Some((ta,fe))=ticks {
                        g.pickup_ns+=(ta as i64)*1000/24-batch.doorbell_counter_ns;
                        g.tail_ns+=retired_counter_ns-(fe as i64)*1000/24;
                    }
                } else {
                    self.geometry_overflow+=1;
                }
                }
            }
        }
        if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
            match control {
                crate::m3_submit::Command::Render {command:r,..}=>
                    dev_info!(self.drm.as_ref(),"M3_PASS kind=render batch={} vdm={:#x} width={} height={} samples={} prepare_ns={} active_ns={} gpu_ns={} ta_ns={} fragment_ns={} gap_ns={}\n",batch_count,r.vdm_base,r.width,r.height,r.samples,preparation_ns,active_ns,gpu_ns,stages[0],stages[1],stages[2]),
                crate::m3_submit::Command::Compute(_)=>
                    dev_info!(self.drm.as_ref(),"M3_PASS kind=compute prepare_ns={} active_ns={} gpu_ns={}\n",preparation_ns,active_ns,gpu_ns),
            }
        }
        let t=&mut self.timing[kind];
        t[0]+=1;t[1]+=preparation_ns;t[2]+=active_ns;t[3]+=gpu_ns as i64;
        for i in 0..3 {t[4+i]+=stages[i] as i64;}
        // Running totals are in debugfs `timing`; the periodic line is a SubmitTiming diagnostic.
        if t[0]==1 && crate::t8122_start::is_t8122(self.device.soc()) {
            let span=match &mut self.jobs[index] {
                NativeJob::Compute(j)=>j.batch_gpu_span().ok(),
                NativeJob::Render(j)=>j.gpu_ticks(batch.base,batch_count).ok().map(|(a,b)|[a,b]),
            };
            crate::t8122_start::job_completed_verdict(self.drm.as_ref(),kind,gpu_ns,span);
        }
        if t[0]%128==0 && crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
            dev_info!(self.drm.as_ref(),"M3_TIMING kind={} count={} prepare_ns={} active_ns={} gpu_ns={} ta_ns={} fragment_ns={} gap_ns={} ordinal={}\n",kind,t[0],t[1],t[2],t[3],t[4],t[5],t[6],
                match &self.jobs[index] {NativeJob::Render(j)=>j.ordinal(),NativeJob::Compute(j)=>j.ordinal()});
        }
        if kind==0 && t[0]%512==0 &&
            crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
            let early=match &self.jobs[index] {NativeJob::Render(j)=>j.early_count(),_=>0};
            dev_info!(self.drm.as_ref(),"M3_ORDERED_BATCHES depths={:?} early={} packets={}\n", self.render_batches,early,self.coalesced_packets);
            let now=<Monotonic as kernel::time::ClockSource>::ktime_get();
            dev_info!(self.drm.as_ref(),"M3_GEOMETRY_BATCH ns={} render_count={} overflow={}\n",
                now,t[0],self.geometry_overflow);
            self.geometry_overflow=0;
            for g in &mut self.geometry {
                if g.count==0 {continue;}
                dev_info!(self.drm.as_ref(),"M3_GEOMETRY ns={} width={} height={} samples={} count={} prepare_ns={} active_ns={} gpu_ns={} tiling_ns={} fragment_ns={} max_fragment_ns={} polling_ns={} retirement_ns={} sleeping_ns={} polls={} stamp_ns={} first_event_ns={} events_ns={} pickup_ns={} tail_ns={}\n",
                    now,g.width,g.height,g.samples,g.count,g.prepare_ns,g.active_ns,g.gpu_ns,g.tiling_ns,g.fragment_ns,g.max_fragment_ns,g.polling_ns,g.retirement_ns,g.sleeping_ns,g.polls,g.stamp_ns,g.first_event_ns,g.events_ns,g.pickup_ns,g.tail_ns);
                *g=GeometryTiming::default();
            }
        }
        if self.active.is_empty() {
            self.gpu_pending = false;
            self.state.health.set_gpu_pending(false);
        }
        for pair in batch.entries.windows(2) {
            if !Arc::ptr_eq(&pair[0].0,&pair[1].0) {self.coalesced_packets+=1;}
        }
        Ok(())
    }
}
// Keep the large graph on the heap: returning and moving it through probe,
// Registration and Mutex initialization otherwise duplicates it on the bounded
// kernel stack. ManuallyDrop retains the whole allocation on failed ASC stop.
pub(crate) struct Runtime { inner: ManuallyDrop<KBox<Inner>> }
impl Runtime {
    #[inline(never)]
    pub(crate) fn new(pdev: &platform::Device<Core>, device: Device, contents: crate::m3_adt_config::Contents) -> Result<Self> {
        // On an armed T8122, name a setup failure before the GPU coprocessor is started (RTKit
        // state, mailbox transport, the DRM device). Captured as a Copy bool so the closure does
        // not borrow `device`, which is moved into Inner below.
        let t8122 = crate::t8122_start::is_t8122(device.soc());
        let coproc_refused = |e: &Error| if t8122 { crate::t8122_start::coproc_setup_refused(pdev.as_ref(), *e); };
        device.require_stopped(pdev).inspect_err(coproc_refused)?;
        // Reserve before starting ASC. ENOMEM must not drop firmware owners
        // while the coprocessor may still access them. Writing the completed
        // graph into this allocation is infallible.
        let owner = KBox::<Inner>::new_uninit(GFP_KERNEL)?;
        let drm: driver::AsahiDevRef = kernel::drm::Device::new(pdev.as_ref(), driver::AsahiData::new(pdev, None, true)).inspect_err(coproc_refused)?;
        let state = m3_rtkit::State::new(pdev, drm.clone(), device.firmware().resources.regions[5]).inspect_err(coproc_refused)?;
        let mut transport = rtkit::RtKit::new(pdev.as_ref(), None, 0, state.clone()).inspect_err(coproc_refused)?;
        if crate::m3_adt_config::stop_before_asc(pdev.as_ref()) { return Err(ENODEV); }
        // Whether the coprocessor runs and offers its endpoints, for the T8122 verdict.
        let mut started=false;
        let prepared=(|| -> Result<_> {
            device.start_asc(pdev)?;
            Pin::new(&mut transport).wake()?;
            if !state.healthy() || !Pin::new(&transport).is_running() {return Err(EIO);}
            for ep in [0x20,0x21] {
                if !Pin::new(&mut transport).has_endpoint(ep) {return Err(ENODEV);}
                Pin::new(&mut transport).start_endpoint(ep)?;
            }
            started=true;
            // The UAT geometry of the admitted SoC (complete: `Soc::require_complete`).
            let hwcfg=device.soc().hwcfg.ok_or(ENODEV)?;
            // SAFETY: the admitted RTKit (J514S on T6030) is awake; no initdata or GPU
            // job has been published. Its running PPL handoff owns the peer lock.
            let uat=unsafe {mmu::Uat::new_m3_running(&drm,hwcfg)}?;
            let config=Config::new(&drm,&uat,device.firmware(),&contents)?;
            Ok((uat,config))
        })();
        let (uat,config)=match prepared {
            Ok(v)=>v,
            Err(e)=>{
                crate::t8122_start::prepare_verdict(pdev.as_ref(),device.soc(),started,e);
                state.health.mark_failed();
                if device.stop_asc().is_err() {
                    unsafe {kernel::bindings::__module_get(crate::THIS_MODULE.as_ptr())};
                    core::mem::forget(transport);core::mem::forget(state);
                    core::mem::forget(drm);core::mem::forget(device);
                }
                return Err(e);
            }
        };
        Ok(Self { inner: ManuallyDrop::new(owner.write(Inner { transport, state, config, uat, drm, device,
            jobs:KVec::new(),packets:KVec::new(),pending:KVec::new(),active:KVec::new(),gpu_pending:false,fault_captured:false,timing:[[0;7];2],render_batches:[0;crate::m3_pass_layout::SLOTS],
            geometry:[GeometryTiming::default();32],geometry_overflow:0,coalesced_packets:0,pass_cost:policy::PassCosts::default(),render_initialized:false,t8122_verdict:false,
            boot_step:crate::t8122_start::BootStep::Publish })) })
    }
    pub(crate) fn drm(&self) -> driver::AsahiDevRef { self.inner.drm.clone() }
    pub(crate) fn health(&self) -> Arc<m3_rtkit::Health> { self.inner.state.health.clone() }
    pub(crate) fn new_vm(&mut self, id: u64, range: core::ops::Range<u64>) -> Result<mmu::Vm> {
        use crate::util::RangeExt;
        let reserved_range=0x70_0000_0000..0x80_0000_0000;
        if range.overlaps(reserved_range.clone()) {return Err(EINVAL);}
        if self.inner.gpu_pending { self.inner.state.events.record_cpu_preparation(); }
        let vm=self.inner.uat.new_vm(id, range)?;
        let mut reserved=KVec::new();reserved.push(reserved_range,GFP_KERNEL)?;reserved.push(0x100_8000_0000..0x101_0000_0000,GFP_KERNEL)?;reserved.push(0x10_0000_0000..0x10_0800_0000,GFP_KERNEL)?;reserved.push(0x10_7400_0000..0x10_7402_8000,GFP_KERNEL)?;
        vm.install_driver_mappings(KVec::new(),reserved)?;
        Ok(vm)
    }
    pub(crate) fn map_timestamp(&self,mut bo:crate::gem::ObjectRef,range:core::ops::Range<usize>)->Result<mmu::KernelMapping> {
        if self.inner.gpu_pending { self.inner.state.events.record_cpu_preparation(); }
        bo.map_range_into_range(self.inner.uat.kernel_vm(),range,crate::agx_memory::TIMESTAMP_RANGE,
            mmu::UAT_PGSZ as u64,mmu::PROT_FW_SHARED_RW,false)
    }
    /// Queue a packet and return its fence at once. The scheduler can hand over further
    /// ready jobs up to its credit limit while the GPU runs. Only the completion worker
    /// publishes: it coalesces render commands of one VM and urgency, and may keep bounded
    /// same-VM batches in flight after InitBM retirement. VM/engine changes wait for full
    /// retirement. A burst can therefore share a firmware round trip across packets.
    pub(crate) fn submit(shared:&crate::m3_drm::Shared,packet:Arc<crate::m3_submit::Packet>)->Result {
        crate::debug::update_debug_flags();
        let mut guard=shared.lock();
        let inner=&mut *Option::as_mut(&mut *guard).ok_or(ENODEV)?.inner;
        if !inner.state.healthy() {return Err(EIO);}
        if packet.commands.is_empty() || packet.commands.len()>256 {
            pr_err!("M3 submit limit: commands={} maximum=256\n",packet.commands.len());
            return Err(E2BIG);
        }
        // Higher urgency first, FIFO within one urgency, including unfinished packets.
        // More urgent work can overtake a continuation between retired batches.
        // Packets of one queue share an urgency, so their order is kept.
        let urgency=packet.urgency;
        let at=policy::pending_position(inner.pending.iter().map(|(p,_)|p.urgency),urgency);
        if inner.pending.len()>=policy::MAX_PACKETS {return Err(EBUSY);}
        inner.pending.reserve(1,GFP_KERNEL)?;
        inner.pending.insert_within_capacity(at,(packet,0)).map_err(|_|ENOMEM)?;
        let drm=inner.drm.clone();
        drop(guard);
        crate::m3_completion::queue(&drm);
        Ok(())
    }
    /// Withdraw a packet's unpublished remainder on cancellation or timeout. Its active
    /// commands remain owned until retirement; error signaling cannot release a DMA guard.
    pub(crate) fn withdraw(shared:&crate::m3_drm::Shared,packet:&Arc<crate::m3_submit::Packet>)->bool {
        let mut guard=shared.lock();
        let Some(runtime)=Option::as_mut(&mut *guard) else {return false;};
        let inner: &mut Inner=&mut runtime.inner;
        let removed=inner.pending.iter().position(|(p,_)|Arc::ptr_eq(p,packet))
            .is_some_and(|i|inner.pending.remove(i).is_ok());
        if !inner.packet_active(packet) && !inner.fault_captured {packet.release_guard();}
        removed
    }
    pub(crate) fn completion_wait(&self) -> Arc<m3_rtkit::EventWait> { self.inner.state.events.clone() }
    pub(crate) fn events(&self)->Arc<m3_rtkit::EventWait> {self.inner.state.events.clone()}
    /// One completion-worker step: retire the active batch if it has completed, then publish
    /// the next one. The worker drops the runtime lock between steps.
    pub(crate) fn service_job(&mut self)->Service {
        let inner: &mut Inner=&mut *self.inner;
        if inner.active.is_empty() {
            if inner.fault_captured || !inner.state.healthy() {inner.fail_pending(EIO);return Service::Idle;}
            if let Err(error)=inner.config.drain(&inner.drm) {
                inner.capture_fault(error);inner.fail_pending(EIO);return Service::Idle;
            }
            return if inner.publish_next() {Service::Progress} else {Service::Idle};
        }
        let messages=inner.state.event_messages.load(Ordering::Acquire);
        match inner.poll_active() {
            Ok(true)=>{inner.publish_next();Service::Progress},
            Ok(false)=>if inner.publish_next() {Service::Progress} else {Service::Waiting(messages)},
            Err(_)=>{inner.fail_pending(EIO);Service::Idle},
        }
    }
    pub(crate) fn boot(&mut self, pdev: &platform::Device<Core>) -> Result {
        let is_t8122 = crate::t8122_start::is_t8122(self.inner.device.soc());
        self.inner.boot_step = crate::t8122_start::BootStep::Publish;
        let result = self.boot_inner(pdev);
        if let Err(error) = result {
            // On T8122, log the firmware readiness words on every boot failure, so the verdict's
            // "see M3 firmware readiness" always has a line to point at. The ready-wait timeout in
            // boot_inner already logs them (on every SoC, as in 12.0), so skip that case here.
            let logged = self.inner.boot_step == crate::t8122_start::BootStep::AwaitReady
                && error == ETIMEDOUT;
            if is_t8122 && !logged {
                let inner: &mut Inner = &mut *self.inner;
                let _ = inner.config.log_ready(&inner.drm);
            }
            self.inner.capture_fault(error);
        }
        if is_t8122 {
            let accepted = result.is_ok() || self.inner.config.ready().unwrap_or(false);
            crate::t8122_start::boot_verdict(pdev.as_ref(), self.inner.device.firmware().initdata_magic,
                self.inner.boot_step, accepted, self.inner.state.health.crashed(), result);
        }
        result
    }
    fn boot_inner(&mut self, pdev: &platform::Device<Core>) -> Result {
        use crate::t8122_start::BootStep;
        let root = self.inner.config.root();
        dev_info!(pdev.as_ref(), "M3: publishing owned initdata {:#x}\n", root);
        self.inner.boot_step = BootStep::Publish;
        Pin::new(&mut self.inner.transport).send_message(0x20, 0x0081000000000000 | (root & ((1u64<<44)-1)))?;
        self.inner.boot_step = BootStep::DeviceControl;
        self.send_control(0x13)?;
        self.send_control(9)?;
        self.inner.boot_step = BootStep::AwaitReady;
        let start = Instant::<Monotonic>::now();
        while !self.inner.config.ready()? {
            if start.elapsed() >= Delta::from_secs(2) || !self.inner.state.healthy() {
                let inner: &mut Inner=&mut *self.inner;let _=inner.config.log_ready(&inner.drm);
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_millis(1));
        }
        self.inner.boot_step = BootStep::PostReady;
        // Import the configured idle policy again after initial power-up.
        self.send_control(0x13)?;
        let inner: &mut Inner = &mut *self.inner;
        inner.config.drain(&inner.drm)?;
        inner.config.log_ready(&inner.drm)?;
        inner.config.check_pstate(&inner.drm, &inner.device, "after boot")?;
        inner.state.health.set_gpu_pending(false);
        dev_info!(pdev.as_ref(), "M3: firmware accepted owned initdata and device controls\n");
        Ok(())
    }
    fn send_control(&mut self, opcode: u32) -> Result {
        let next = self.inner.config.control(opcode)?;
        Pin::new(&mut self.inner.transport).send_message(0x21, 0x0083000000000011)?;
        let start = Instant::<Monotonic>::now();
        loop {
            let inner: &mut Inner = &mut *self.inner;
            inner.config.drain(&inner.drm)?;
            if !inner.state.healthy() { return Err(EIO); }
            if inner.config.control_done(next)? { return Ok(()); }
            if start.elapsed() >= Delta::from_secs(2) { return Err(ETIMEDOUT); }
            fsleep(Delta::from_millis(1));
        }
    }
}
impl Drop for Runtime {
    fn drop(&mut self) {
        self.inner.state.health.mark_failed();
        // Unpublished packets never reach the firmware; a published batch's packets stay
        // retained below if the GPU may still use them, but their fences must not hang.
        self.inner.fail_pending(ENODEV);
        for batch in self.inner.active.iter() {Inner::finish_all(&batch.entries,Err(ENODEV));}
        if policy::retain_runtime(self.inner.device.stop_asc().is_err(),self.inner.gpu_pending) {
            // SAFETY: a live module reference pins callbacks with retained DMA.
            unsafe { kernel::bindings::__module_get(crate::THIS_MODULE.as_ptr()) };
            return;
        }
        // SAFETY: no GPU command is pending and ASC is stopped. Field order
        // drains callbacks before dropping retained commands, UAT and power.
        unsafe { ManuallyDrop::drop(&mut self.inner) };
    }
}

/// Same scaling contract as G16's qualified publication profiler. AP counter
/// rates may differ between SoCs; firmware command timestamps use 24 MHz.
fn physical_counter() -> u64 {
    let tick: u64;
    let frequency: u64;
    // SAFETY: Read architectural counters only; no device state is modified.
    unsafe { core::arch::asm!("mrs {f}, cntfrq_el0", "mrs {t}, cntpct_el0",
        f = out(reg) frequency, t = out(reg) tick,
        options(nomem, nostack, preserves_flags)) };
    let frequency = frequency & 0xffff_ffff;
    if frequency == 0 { return 0; }
    (tick / frequency) * 24_000_000 + (tick % frequency) * 24_000_000 / frequency
}
