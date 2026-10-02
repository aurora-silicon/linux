// SPDX-License-Identifier: GPL-2.0-only OR MIT
use core::{mem::ManuallyDrop, sync::atomic::Ordering};
use kernel::{device::Core, platform, prelude::*, soc::apple::rtkit, sync::Arc,
    time::{Instant, Monotonic, Delta, delay::fsleep}};
use crate::{driver, mmu, m3_device::Device, m3_config::Config, m3_rtkit};

enum NativeJob {Compute(crate::m3_compute::Compute),Render(crate::m3_render::Render)}
#[derive(Clone, Copy, Default)]
struct GeometryTiming {
    width: u16, height: u16, samples: u8, count: u64,
    prepare_ns: i64, active_ns: i64, gpu_ns: u64,
    tiling_ns: u64, fragment_ns: u64, max_fragment_ns: u64,
    polling_ns: i64, retirement_ns: i64, sleeping_ns: i64, polls: u64,
}
impl NativeJob {
    fn complete(&mut self)->Result<bool> {match self {Self::Compute(j)=>j.complete(),Self::Render(j)=>j.complete()}}
    fn log(&mut self,dev:&driver::AsahiDevice)->Result {match self {Self::Compute(j)=>j.log(dev),Self::Render(j)=>j.log(dev)}}
}
struct Inner {
    transport: rtkit::RtKit<m3_rtkit::Operations>,
    state: Arc<m3_rtkit::State>,
    jobs: KVec<NativeJob>,
    packets: KVec<Arc<crate::m3_submit::Packet>>,
    gpu_pending: bool,
    fault_captured: bool,
    timing: [[i64;7];2],
    render_batches: [u64;crate::m3_pass_layout::SLOTS],
    geometry: [GeometryTiming; 32],
    geometry_overflow: u64,
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
                dev_err!(self.drm.as_ref(),"M3 resume experiment: stopped by {:?}\n",e);
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
            dev_info!(self.drm.as_ref(),"M3 resume experiment: the firmware did not halt within 100 ms (halt_count={} resume={}); not resuming\n",halt.0,halt.2);
            return Ok(());
        }
        dev_info!(self.drm.as_ref(),"M3 resume experiment: the firmware halted (halt_count={} halted={}); clearing halted and setting resume\n",halt.0,halt.1);
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
                dev_info!(self.drm.as_ref(),"M3 resume experiment +{}us: halt_count={} halted={} resume={} engines={} job_complete={} event_messages={} crashed={}\n",
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
// Keep the large graph on the heap: returning and moving it through probe,
// Registration and Mutex initialization otherwise duplicates it on the bounded
// kernel stack. ManuallyDrop retains the whole allocation on failed ASC stop.
pub(crate) struct Runtime { inner: ManuallyDrop<KBox<Inner>> }
impl Runtime {
    #[inline(never)]
    pub(crate) fn new(pdev: &platform::Device<Core>, device: Device, contents: crate::m3_adt_config::Contents) -> Result<Self> {
        device.require_stopped(pdev)?;
        // Reserve before starting ASC. ENOMEM must not drop firmware owners
        // while the coprocessor may still access them. Writing the completed
        // graph into this allocation is infallible.
        let owner = KBox::<Inner>::new_uninit(GFP_KERNEL)?;
        let drm: driver::AsahiDevRef = kernel::drm::Device::new(pdev.as_ref(), driver::AsahiData::new(pdev, None, true))?;
        let state = m3_rtkit::State::new(pdev, drm.clone(), device.firmware().resources.regions[5])?;
        let mut transport = rtkit::RtKit::new(pdev.as_ref(), None, 0, state.clone())?;
        if crate::m3_adt_config::stop_before_asc(pdev.as_ref()) { return Err(ENODEV); }
        let prepared=(|| -> Result<_> {
            device.start_asc(pdev)?;
            Pin::new(&mut transport).wake()?;
            if !state.healthy() || !Pin::new(&transport).is_running() {return Err(EIO);}
            for ep in [0x20,0x21] {
                if !Pin::new(&mut transport).has_endpoint(ep) {return Err(ENODEV);}
                Pin::new(&mut transport).start_endpoint(ep)?;
            }
            // SAFETY: admitted J514S RTKit is awake; no initdata or GPU job
            // has been published. Its running PPL handoff owns the peer lock.
            let uat=unsafe {mmu::Uat::new_t6030_running(&drm)}?;
            let config=Config::new(&drm,&uat,device.firmware(),&contents)?;
            Ok((uat,config))
        })();
        let (uat,config)=match prepared {
            Ok(v)=>v,
            Err(e)=>{
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
            jobs:KVec::new(),packets:KVec::new(),gpu_pending:false,fault_captured:false,timing:[[0;7];2],render_batches:[0;crate::m3_pass_layout::SLOTS],
            geometry:[GeometryTiming::default();32],geometry_overflow:0 })) })
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
    /// The DRM scheduler's single run-job worker owns execution. Release only
    /// the runtime mutex during bounded waits so independent CPU VM/timestamp
    /// preparation can overlap GPU work; no second packet or pass is published.
    pub(crate) fn execute(shared: &crate::m3_drm::Shared,
                          packet: Arc<crate::m3_submit::Packet>) -> Result {
        let result = Self::execute_inner(shared, packet);
        if let Err(error) = result {
            if let Some(runtime) = Option::as_mut(&mut *shared.lock()) {
                if runtime.inner.gpu_pending { runtime.inner.capture_fault(error); }
            }
        }
        result
    }
    fn execute_inner(shared: &crate::m3_drm::Shared,
                     packet: Arc<crate::m3_submit::Packet>) -> Result {
        crate::debug::update_debug_flags();
        let render_batch_size=crate::m3_params::render_batch_size();
        let compute_batch_size=crate::m3_params::compute_batch_size();
        let mut guard = shared.lock();
        let events = {
            let inner: &mut Inner = &mut *Option::as_mut(&mut *guard).ok_or(ENODEV)?.inner;
            if !inner.state.healthy() { return Err(EIO); }
            // A future second executor must fail closed, never reset storage
            // or clear retained owners of the scheduler's in-flight packet.
            if inner.gpu_pending { return Err(EBUSY); }
            if packet.commands.is_empty() || packet.commands.len() > 256 {
                pr_err!("M3 submit limit: commands={} maximum=256\n", packet.commands.len());
                return Err(E2BIG);
            }
            inner.packets.clear();
            inner.packets.push(packet.clone(), GFP_KERNEL)?;
            inner.state.events.clone()
        };
        // Snapshot once per packet. A root policy write cannot change ownership
        // halfway through it. The initial value preserves m3_unlocked_wait.
        let overlap_cpu = events.cpu_overlap();
        let mut command_index=0;
        while command_index<packet.commands.len() {
            let control=packet.commands[command_index];
            let batch_count=if matches!(control,crate::m3_submit::Command::Render{..}) {
                packet.commands[command_index..].iter().take(render_batch_size)
                    .take_while(|c|matches!(c,crate::m3_submit::Command::Render{..})).count()
            } else {
                packet.commands[command_index..].iter().take(compute_batch_size)
                    .take_while(|c|matches!(c,crate::m3_submit::Command::Compute(_))).count()
            };
            let prepare=Instant::<Monotonic>::now();
            let inner: &mut Inner=&mut *Option::as_mut(&mut *guard).ok_or(ENODEV)?.inner;
            let found=inner.jobs.iter().position(|job| matches!((job,control),
                (NativeJob::Compute(_),crate::m3_submit::Command::Compute(_)) |
                (NativeJob::Render(_),crate::m3_submit::Command::Render{..})));
            let index=found.unwrap_or(inner.jobs.len());
            if found.is_none() {
                let job=match control {
                    crate::m3_submit::Command::Compute(c)=>NativeJob::Compute(crate::m3_compute::Compute::new(&inner.drm,&inner.uat,&packet.vm,inner.config.stats_region()?,c)?),
                    crate::m3_submit::Command::Render{..}=>NativeJob::Render(crate::m3_render::Render::new(&inner.drm,&inner.uat,&packet.vm,inner.config.stats_region()?)?),
                };
                inner.jobs.push(job,GFP_KERNEL)?;
            } else {
                match (&mut inner.jobs[index],control) {
                    (NativeJob::Render(_),crate::m3_submit::Command::Render{..})=>{},
                    (NativeJob::Compute(j),crate::m3_submit::Command::Compute(c))=>j.replay(&inner.uat,&packet.vm,c)?,
                    _=>return Err(ENOTSUPP),
                }
            }
            if let NativeJob::Compute(j)=&mut inner.jobs[index] {
                let prepared=(||->Result {
                    for next in command_index..command_index+batch_count {
                        if next!=command_index {
                            let crate::m3_submit::Command::Compute(c)=packet.commands[next] else {return Err(EINVAL);};
                            j.append(c)?;
                        }
                        let timestamps=packet.timestamp_addresses(next);
                        let coalesce=crate::m3_compute_storage::coalesce_stamp_flush(
                            packet.wide_visibility[next],timestamps,next+1==command_index+batch_count);
                        j.prepare_completion(coalesce)?;
                        j.set_user_timestamps(timestamps)?;
                        j.set_attachments(&packet.attachments[next])?;
                    }
                    Ok(())
                })();
                if let Err(error)=prepared {
                    j.abort_batch()?;
                    if found.is_none() {drop(inner.jobs.remove(index).map_err(|_|EIO)?);}
                    return Err(error);
                }
            }
            if let NativeJob::Render(j)=&mut inner.jobs[index] {
                j.begin_batch(&inner.drm,&inner.uat,&packet.vm,
                    &packet.commands[command_index..command_index+batch_count])?;
                let prepared=(||->Result {
                    for next in command_index..command_index+batch_count {
                        let crate::m3_submit::Command::Render{command,usc}=packet.commands[next] else {return Err(EINVAL);};
                        j.append(command,usc)?;
                        j.set_user_timestamps(packet.render_timestamp_addresses(next))?;
                    }
                    Ok(())
                })();
                if let Err(error)=prepared {j.abort_batch()?;return Err(error);}
            }
            let preparation_ns=prepare.elapsed().as_nanos();
            let previous_events=inner.config.completed_events;
            inner.state.health.set_gpu_pending(true);
            inner.gpu_pending = true;
            // Diagnostic only: publication precedes queue writes/doorbells.
            // Never compare an unscaled AP counter to firmware timestamps.
            let boundary_profile=crate::debug::debug_enabled(crate::debug::DebugFlags::M3PassTiming);
            let publication_tick=if boundary_profile {physical_counter()} else {0};
            let expected_events=match &inner.jobs[index] {
                NativeJob::Compute(j)=>{
                    inner.config.submit_queue(2,j.queue(),j.head(),2,j.first())?;
                    Pin::new(&mut inner.transport).send_message(0x21,0x0083000000000002)?;1
                },
                NativeJob::Render(j)=>{
                    if j.first() {inner.config.render_pb()?;}
                    let queues=j.queues();
                    inner.config.submit_queue(1,queues[1],j.heads()[1],1,j.first())?;
                    Pin::new(&mut inner.transport).send_message(0x21,0x0083000000000001)?;
                    inner.config.submit_queue(0,queues[0],j.heads()[0],0,j.first())?;
                    Pin::new(&mut inner.transport).send_message(0x21,0x0083000000000000)?;2
                },
            };
            let start=Instant::<Monotonic>::now();
            let mut trailing=0;
            let mut waited_at=u64::MAX;
            let measure=crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary);
            let (mut polling_ns,mut retirement_ns,mut sleeping_ns,mut polls)=(0,0,0,0);
            loop {
                // Unbind may remove the runtime while unlocked. Drop either
                // proves ASC stopped or quarantines all pending DMA owners.
                let inner: &mut Inner=&mut *Option::as_mut(&mut *guard).ok_or(ENODEV)?.inner;
                let poll_start=measure.then(Instant::<Monotonic>::now);
                let messages=inner.state.event_messages.load(Ordering::Acquire);
                if !inner.state.healthy() || inner.config.drain(&inner.drm).is_err() {
                    inner.fail(index, &packet.vm, EIO);return Err(EIO);
                }
                let complete=inner.jobs[index].complete()? && inner.config.completed_events>=previous_events+expected_events;
                if let Some(t)=poll_start {polling_ns+=t.elapsed().as_nanos();polls+=1;}
                if complete {
                    let retire_start=measure.then(Instant::<Monotonic>::now);
                    match inner.device.check_idle() {
                        Ok(()) if inner.config.pipes_idle()?=>{
                            if let Err(e)=inner.config.check_pstate(&inner.drm,&inner.device,"after a job") {inner.state.health.mark_failed();return Err(e);}
                            let observed_tick=if boundary_profile {physical_counter()} else {0};
                            // Stamps, both queue indices, required events,
                            // firmware health, engines and pipes are verified.
                            inner.state.health.record_completion();
                            match &mut inner.jobs[index] {
                                NativeJob::Compute(j)=>j.progress(&inner.drm)?,
                                NativeJob::Render(j)=>j.progress(&inner.drm)?,
                            }
                            let stages=match &mut inner.jobs[index] {
                                NativeJob::Render(j)=>j.stage_ns()?,
                                _=>[0;3],
                            };
                            let (kind,gpu_ns)=match &mut inner.jobs[index] {
                                NativeJob::Compute(j)=>(1,j.gpu_ns()?),
                                NativeJob::Render(j)=>(0,j.gpu_ns()?),
                            };
                            if kind==0 {inner.render_batches[batch_count-1]+=1;}
                            if crate::debug::debug_enabled(crate::debug::DebugFlags::M3PassTiming) {
                                let (cs,span)=match (&mut inner.jobs[index],control) {
                                    (NativeJob::Compute(j),crate::m3_submit::Command::Compute(c))=>(c.base,j.batch_gpu_span()?),
                                    (NativeJob::Render(j),crate::m3_submit::Command::Render{command:r,..})=>(r.vdm_base,j.batch_gpu_span()?),
                                    _=>return Err(EINVAL),
                                };
                                let valid=publication_tick!=0 && span[0]>=publication_tick &&
                                    span[1]>=span[0] && observed_tick>=span[1];
                                dev_info!(inner.drm.as_ref(),"M3_BATCH_BOUNDARY cs={:#x} batch={} publication_tick={} gpu_start_tick={} gpu_end_tick={} observed_tick={} counter_hz=24000000 valid={}\n",
                                    cs,batch_count,publication_tick,span[0],span[1],observed_tick,u32::from(valid));
                                if let NativeJob::Compute(j)=&mut inner.jobs[index] {
                                    for slot in 0..batch_count {
                                        let crate::m3_submit::Command::Compute(c)=packet.commands[command_index+slot] else {return Err(EINVAL);};
                                        let ns=j.slot_gpu_ns(slot)?;
                                        dev_info!(inner.drm.as_ref(),"M3_COMPUTE_SLOT slot={} batch={} cdm={:#x} end={:#x} usc={:#x} gpu_ns={}\n",slot,batch_count,c.base,c.end,c.usc_base,ns);
                                    }
                                }
                                if let NativeJob::Render(j)=&mut inner.jobs[index] {
                                    for slot in 0..batch_count {
                                        let crate::m3_submit::Command::Render{command:r,..}=packet.commands[command_index+slot] else {return Err(EINVAL);};
                                        let s=j.slot_stage_ns(slot)?;
                                        dev_info!(inner.drm.as_ref(),"M3_PASS_SLOT slot={} batch={} vdm={:#x} width={} height={} samples={} ta_ns={} fragment_ns={} gap_ns={}\n",slot,batch_count,r.vdm_base,r.width,r.height,r.samples,s[0],s[1],s[2]);
                                    }
                                }
                            }
                            let t=&mut inner.timing[kind];
                            let active_ns=start.elapsed().as_nanos();
                            inner.state.health.timing.record(kind==0, batch_count as u64,
                                preparation_ns, active_ns, gpu_ns, stages);
                            if let Some(t)=retire_start {retirement_ns+=t.elapsed().as_nanos();}
                            if crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
                                if let crate::m3_submit::Command::Render {command:r,..}=control {
                                    if batch_count>1 {inner.geometry_overflow+=1;} else {
                                    if let Some(g)=inner.geometry.iter_mut().find(|g|
                                        g.count==0 || (g.width==r.width && g.height==r.height && g.samples==r.samples)) {
                                        g.width=r.width;g.height=r.height;g.samples=r.samples;
                                        g.count+=1;g.prepare_ns+=preparation_ns;g.active_ns+=active_ns;
                                        g.gpu_ns+=gpu_ns;g.tiling_ns+=stages[0];g.fragment_ns+=stages[1];
                                        g.max_fragment_ns=g.max_fragment_ns.max(stages[1]);
                                        g.polling_ns+=polling_ns;g.retirement_ns+=retirement_ns;
                                        g.sleeping_ns+=sleeping_ns;g.polls+=polls;
                                    } else {
                                        inner.geometry_overflow+=1;
                                    }
                                    }
                                }
                            }
                            if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
                                match control {
                                    crate::m3_submit::Command::Render {command:r,..}=>
                                        dev_info!(inner.drm.as_ref(),"M3_PASS kind=render vdm={:#x} width={} height={} samples={} prepare_ns={} active_ns={} gpu_ns={} ta_ns={} fragment_ns={} gap_ns={}\n",r.vdm_base,r.width,r.height,r.samples,preparation_ns,start.elapsed().as_nanos(),gpu_ns,stages[0],stages[1],stages[2]),
                                    crate::m3_submit::Command::Compute(_)=>
                                        dev_info!(inner.drm.as_ref(),"M3_PASS kind=compute prepare_ns={} active_ns={} gpu_ns={}\n",preparation_ns,start.elapsed().as_nanos(),gpu_ns),
                                }
                            }
                            t[0]+=1;t[1]+=preparation_ns;t[2]+=active_ns;t[3]+=gpu_ns as i64;
                            for i in 0..3 {t[4+i]+=stages[i] as i64;}
                            if t[0]%128==0 {
                                dev_info!(inner.drm.as_ref(),"M3_TIMING kind={} count={} prepare_ns={} active_ns={} gpu_ns={} ta_ns={} fragment_ns={} gap_ns={} ordinal={}\n",kind,t[0],t[1],t[2],t[3],t[4],t[5],t[6],
                                    match &inner.jobs[index] {NativeJob::Render(j)=>j.ordinal(),NativeJob::Compute(j)=>j.ordinal()});
                            }
                            if kind==0 && t[0]%512==0 &&
                                crate::debug::debug_enabled(crate::debug::DebugFlags::M3SubmitSummary) {
                                let early=match &inner.jobs[index] {NativeJob::Render(j)=>j.early_count(),_=>0};
                                dev_info!(inner.drm.as_ref(),"M3_ORDERED_BATCHES depths={:?} early={}\n", inner.render_batches,early);
                                let now=<Monotonic as kernel::time::ClockSource>::ktime_get();
                                dev_info!(inner.drm.as_ref(),"M3_GEOMETRY_BATCH ns={} render_count={} overflow={}\n",
                                    now,t[0],inner.geometry_overflow);
                                inner.geometry_overflow=0;
                                for g in &mut inner.geometry {
                                    if g.count==0 {continue;}
                                    dev_info!(inner.drm.as_ref(),"M3_GEOMETRY ns={} width={} height={} samples={} count={} prepare_ns={} active_ns={} gpu_ns={} tiling_ns={} fragment_ns={} max_fragment_ns={} polling_ns={} retirement_ns={} sleeping_ns={} polls={}\n",
                                        now,g.width,g.height,g.samples,g.count,g.prepare_ns,g.active_ns,g.gpu_ns,g.tiling_ns,g.fragment_ns,g.max_fragment_ns,g.polling_ns,g.retirement_ns,g.sleeping_ns,g.polls);
                                    *g=GeometryTiming::default();
                                }
                            }
                            inner.gpu_pending = false;
                            inner.state.health.set_gpu_pending(false);break;
                        },
                        Ok(())=>{}, // Firmware must consume every submitted queue message.
                        Err(e) if e==EBUSY=>{}, // Retirement can trail the event.
                        Err(e)=>{inner.fail(index, &packet.vm, e);return Err(e);}
                    }
                }
                if start.elapsed()>=Delta::from_secs(2) {
                    dev_err!(inner.drm.as_ref(),"M3 completion events={} previous={}\n",inner.config.completed_events,previous_events);
                    inner.fail(index, &packet.vm, ETIMEDOUT);return Err(ETIMEDOUT);
                }
                // Four bounded 5us polls cover stamps written just after an
                // event without paying a scheduler round-trip for each poll.
                // fsleep uses udelay at <=10us. Longer waits still sleep until
                // an IRQ or the bounded resnapshot timer; retirement checks remain above.
                // Wait without the runtime lock, so that VM creation, timestamp
                // mapping and the completion worker's event drain are not held
                // up for the duration of a job.
                let sleep_start=measure.then(Instant::<Monotonic>::now);
                if messages!=waited_at {waited_at=messages;trailing=0;}
                let mut wait=|| {
                    if trailing<4 {trailing+=1;fsleep(Delta::from_micros(5));}
                    else {events.wait_past(messages);trailing=0;}
                };
                if overlap_cpu { drop(guard); wait(); guard=shared.lock(); }
                else { wait(); }
                if let Some(t)=sleep_start {sleeping_ns+=t.elapsed().as_nanos();}
            }
            command_index+=batch_count;
        }
        Ok(())
    }
    pub(crate) fn completion_wait(&self) -> Arc<m3_rtkit::EventWait> {
        self.inner.state.events.clone()
    }
    pub(crate) fn service_events(&mut self) {
        let inner: &mut Inner=&mut *self.inner;
        if inner.fault_captured { return; }
        let result = if inner.state.healthy() { inner.config.drain(&inner.drm) }
            else { Err(EIO) };
        if let Err(error) = result { inner.capture_fault(error); }
    }

    pub(crate) fn boot(&mut self, pdev: &platform::Device<Core>) -> Result {
        let result = self.boot_inner(pdev);
        if let Err(error) = result { self.inner.capture_fault(error); }
        result
    }
    fn boot_inner(&mut self, pdev: &platform::Device<Core>) -> Result {
        let root = self.inner.config.root();
        dev_info!(pdev.as_ref(), "M3: publishing owned initdata {:#x}\n", root);
        Pin::new(&mut self.inner.transport).send_message(0x20, 0x0081000000000000 | (root & ((1u64<<44)-1)))?;
        self.send_control(0x13)?;
        self.send_control(9)?;
        let start = Instant::<Monotonic>::now();
        while !self.inner.config.ready()? {
            if start.elapsed() >= Delta::from_secs(2) || !self.inner.state.healthy() {
                let inner: &mut Inner=&mut *self.inner;let _=inner.config.log_ready(&inner.drm);
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_millis(1));
        }
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
        if self.inner.device.stop_asc().is_err() || self.inner.gpu_pending {
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
