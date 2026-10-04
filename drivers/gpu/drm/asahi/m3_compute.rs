// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Owned RTKit 2419 M3 compute graph, populated with a validated user CDM.
use kernel::prelude::*;
use crate::m3_queue_layout as queue;
use crate::m3_state_layout as state;
use crate::{m3_compute_layout as command, m3_compute_sequence as microsequence,
    m3_pool_layout::GpuRegion, m3_init_layout::Region};
use crate::{driver,mmu,agx_memory::{self},m3_memory::Buffer,m3_compute_storage as storage};
use crate::m3_timeline as timeline;
use storage::{NOTIFIER,PREEMPTION,GPU_CONTEXT,JOB_LIST,QUEUE_SCRATCH,TIMESTAMPS,
    TAIL_SCRATCH,MICROSEQUENCE_SCRATCH,Mapping};
const UMA_POOL:crate::m3_pool::Owners=crate::m3_pool::Owners {
    backing:storage::POOL_BACKING,pages:storage::POOL_PAGES,table:storage::POOL_TABLE,
    manager:storage::POOL,counter:storage::COUNTER,table_index:2,
};
// Retain at most four VM views, including the active one, as the render
// graph does. Game and compositor compute jobs otherwise remap the whole
// private arena every time they alternate. Each entry pins its VM binding.
struct CachedViews {
    command: mmu::KernelMapping,
    views: KVec<(usize, mmu::KernelMapping)>,
    // Release cached mappings before returning their ASID lease.
    binding: mmu::VmBind,
}
pub(crate) struct Compute {
    _client_command: mmu::KernelMapping,
    objects: KVec<Buffer>,
    command_bytes: KVec<u8>,
    sequence_bytes: KVec<u8>,
    _binding: mmu::VmBind,
    stats: Region,
    sequence: u64,
    last_progress_ns: i64,
    head: u16,
    cached_views: KVec<CachedViews>,
    slot: usize,
    batch_count: usize,
    checkpoint: (u64,u16,usize),
}
impl Compute {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, vm: &mmu::Vm,
        stats: Region, control: crate::agx_compute::Control) -> Result<Self> {
        let binding=uat.bind(vm)?;
        let mut objects=KVec::new();
        let mut client_command=None;
        let mut command_bytes = KVec::new();
        command_bytes.resize(command::SIZE, 0u8, GFP_KERNEL)?;
        let mut sequence_bytes = KVec::new();
        sequence_bytes.resize(microsequence::STORAGE, 0u8, GFP_KERNEL)?;
        // A private compute aperture, disjoint from the retained render graph.
        // Address-bearing graph fields are constructed from their live owners.
        for index in 0..storage::COUNT {
            let a=storage::batch_allocation(index).map_err(|_|EINVAL)?;
            let mut b=match a.mapping {
                Mapping::CommandAlias(va)=>Buffer::at(dev,uat.kernel_vm(),Some((uat.kernel_lower_vm(),va)),None,a.size,true)?,
                Mapping::Client(va)=>Buffer::at(dev,uat.kernel_vm(),Some((vm,va)),None,a.size,true)?,
                Mapping::FirmwareGpuShared=>Buffer::at(dev,uat.kernel_vm(),None,None,a.size,true)?,
                Mapping::FirmwareOnly=>Buffer::at(dev,uat.kernel_vm(),None,None,a.size,false)?,
            };
            if matches!(a.mapping,Mapping::CommandAlias(_)) {client_command=Some(b.map_gpu_view(vm)?);}
            objects.push(b,GFP_KERNEL)?;
        }
        Self::encode_command(&mut objects, &mut command_bytes, binding.slot(), control, 1, 0)?;
        Self::encode_sequence(&mut objects, &mut sequence_bytes, stats, binding.slot(), timeline::stamp(0xc1000000,1), 0)?;
        UMA_POOL.initialize(&mut objects)?;
        Self::initialize_state(&mut objects, binding.slot())?;
        // Queue references are firmware VAs from live allocations. In contrast
        // to render's unpublished seed entries, compute dispatches entry zero
        // immediately, so its initial producer cursor remains one.
        let address = |index: usize| queue::FirmwareVa::new(objects[index].va()).map_err(|_| EINVAL);
        let info = queue::QueueInfo { state: address(storage::QUEUE_STATE)?, ring: address(storage::RING)?,
            job_list: address(JOB_LIST)?, gpu_scratch: address(QUEUE_SCRATCH)?,
            context: address(GPU_CONTEXT)?, uuid: 0xc10000 };
        let command = address(storage::COMMAND)?;
        let mut bytes = [0; queue::INFO_SIZE];
        info.encode(&mut bytes).map_err(|_| EINVAL)?;
        objects[storage::QUEUE].write(0, &bytes)?;
        let mut state = [0; queue::STATE_SIZE];
        queue::RingState { capacity: queue::QUALIFIED_CAPACITY, done: 0, read: 0, write: 1 }
            .encode(&mut state).map_err(|_| EINVAL)?;
        objects[storage::QUEUE_STATE].write(0, &state)?;
        for index in 0..queue::QUALIFIED_CAPACITY {
            let entry = queue::ring_entry(index, queue::QUALIFIED_CAPACITY, &[command]).map_err(|_| EINVAL)?;
            objects[storage::RING].write(index as usize * 8, &entry)?;
        }
        dev_info!(dev.as_ref(),"M3 compute graph: context={} queue={:#x} command={:#x} low={:#x} cdm={:#x}..{:#x} pool={:#x}\n",
            binding.slot(),objects[storage::QUEUE].va(),objects[storage::COMMAND].va(),
            objects[storage::COMMAND].gpu_va()?,control.base,control.end,objects[storage::POOL].va());
        // RTKit 2419's qualified M3 register producer uses absolute IOTO
        // addresses and does not emit M4's USC_EXEC_BASE_CP register.
        let mut stream=[0u8;48];
        vm.read_bytes(control.base,&mut stream)?;
        dev_info!(dev.as_ref(),"M3 client CDM bytes={:02x?}\n",stream);
        agx_memory::publish();
        // Match the qualified lab's complete publication sequence, including
        // completion of WC TTBAT stores before broadcasting invalidation.
        crate::mem::sync();crate::mem::tlbi_all();crate::mem::sync();
        unsafe {core::arch::asm!("isb",options(nostack,preserves_flags))};
        Ok(Self {_client_command:client_command.ok_or(EINVAL)?,objects,command_bytes,sequence_bytes,_binding:binding,stats,last_progress_ns:0,sequence:1,head:1,cached_views:KVec::new(),slot:0,batch_count:1,checkpoint:(0,0,0)})
    }
    fn encode_command(objects: &mut [Buffer], bytes: &mut [u8], context: u32,
        control: crate::agx_compute::Control, sequence: u64, slot: usize) -> Result {
        let offset = |owner:usize| storage::slot_offset(owner,slot).map_err(|_|EINVAL);
        let fw = |owner: usize| queue::FirmwareVa::new(objects[owner].va()+offset(owner)? as u64).map_err(|_| EINVAL);
        let preemption = &objects[PREEMPTION];
        let value = command::Command {
            context, counter: timeline::previous(sequence), notifier: fw(NOTIFIER)?,
            preemption: GpuRegion::new(preemption.gpu_va()?+offset(PREEMPTION)? as u64, storage::allocation(PREEMPTION).map_err(|_|EINVAL)?.size).map_err(|_| EINVAL)?,
            cdm: command::GpuVa::new(control.base).map_err(|_| EINVAL)?,
            cdm_last: command::GpuVa::new(control.end.checked_sub(4).ok_or(EINVAL)?).map_err(|_| EINVAL)?,
            gpu_alias: command::GpuVa::new(objects[storage::COMMAND].gpu_va()?+offset(storage::COMMAND)? as u64).map_err(|_| EINVAL)?,
            microsequence: fw(storage::MICROSEQUENCE)?, microsequence_size: microsequence::LENGTH as u32,
            stamp: fw(storage::STAMP)?, fw_stamp: fw(storage::FW_STAMP)?,
            stamp_value: timeline::stamp(0xc1000000,sequence),
            timestamps: [fw(TIMESTAMPS[0])?, fw(TIMESTAMPS[1])?],
            pool: fw(storage::POOL)?, tail_scratch: fw(TAIL_SCRATCH)?, flush_stamps: false,
        };
        value.encode(bytes).map_err(|_| EINVAL)?;
        objects[storage::COMMAND].write(offset(storage::COMMAND)?, bytes)
    }
    fn encode_sequence(objects: &mut [Buffer], bytes: &mut [u8], stats: Region,
        context: u32, stamp_value: u32, slot: usize) -> Result {
        let offset = |owner:usize| storage::slot_offset(owner,slot).map_err(|_|EINVAL);
        let fw = |owner: usize| queue::FirmwareVa::new(objects[owner].va()+offset(owner)? as u64).map_err(|_| EINVAL);
        let region = |owner: usize| Region::new(objects[owner].va()+offset(owner)? as u64, storage::allocation(owner).map_err(|_|EINVAL)?.size).map_err(|_| EINVAL);
        let value = microsequence::Sequence {
            command: region(storage::COMMAND)?, stats, notifier: region(NOTIFIER)?,
            queue: fw(storage::QUEUE)?, context, generation: 1, pool: fw(storage::POOL)?,
            scratch: [fw(MICROSEQUENCE_SCRATCH[0])?, fw(MICROSEQUENCE_SCRATCH[1])?,
                fw(MICROSEQUENCE_SCRATCH[2])?, fw(MICROSEQUENCE_SCRATCH[3])?, fw(MICROSEQUENCE_SCRATCH[4])?],
            fw_stamp: fw(storage::FW_STAMP)?, stamp_value,
        };
        value.encode(bytes).map_err(|_| EINVAL)?;
        objects[storage::MICROSEQUENCE].write(offset(storage::MICROSEQUENCE)?, bytes)
    }
    fn initialize_state(objects: &mut [Buffer], context_slot: u32) -> Result {
        let address = |owner: usize| queue::FirmwareVa::new(objects[owner].va()).map_err(|_| EINVAL);
        let notification = state::Notifier { threshold: address(storage::EVENT)?, context: context_slot };
        let list_owner = address(JOB_LIST)?;
        let mut context = [0; state::CONTEXT_SIZE];
        state::context(&mut context).map_err(|_| EINVAL)?;
        objects[GPU_CONTEXT].write(0, &context)?;
        let mut notifier = [0; state::NOTIFIER_SIZE];
        notification.encode(&mut notifier).map_err(|_| EINVAL)?;
        objects[NOTIFIER].write(0, &notifier)?;
        let mut list = [0; state::JOB_LIST_SIZE];
        state::job_list(&mut list, list_owner).map_err(|_| EINVAL)?;
        objects[JOB_LIST].write(0, &list)?;
        for (owner, value) in [(storage::COUNTER, 1), (storage::EVENT, 1), (storage::STAMP, timeline::stamp(0xc1000000,0))] {
            let mut bytes = [0; state::MANAGER_STATE_SIZE];
            let bytes = bytes.get_mut(..objects[owner].size()).ok_or(EINVAL)?;
            state::counter(bytes, value).map_err(|_| EINVAL)?;
            objects[owner].write(0, bytes)?;
        }
        // Preserve the qualified producer's poison, including on replay reset.
        objects[PREEMPTION].fill(0xcc)
    }
    pub(crate) fn replay(&mut self, uat: &mmu::Uat, vm: &mmu::Vm,
        control: crate::agx_compute::Control) -> Result {
        if !self.complete()? { return Err(EBUSY); }
        // All prior flushed stamps/events and engine idle were checked by
        // Runtime before entry. Keep pool/queue ownership, remap only client
        // views and patch the context when switching Vulkan processes.
        if !self._binding.matches(vm) {
            let next = if let Some(index) = self.cached_views.iter().position(|v| v.binding.matches(vm)) {
                self.cached_views.remove(index).map_err(|_| EIO)?
            } else {
                let binding = uat.bind(vm)?;
                let mut views = KVec::new();
                for i in 0..storage::COUNT {
                    if matches!(storage::allocation(i).map_err(|_|EINVAL)?.mapping,Mapping::Client(_)) {
                        views.push((i,self.objects[i].map_gpu_view(vm)?),GFP_KERNEL)?;
                    }
                }
                let command = self.objects[storage::COMMAND].map_gpu_view(vm)?;
                CachedViews {binding, command, views}
            };
            // Complete allocations before moving the active views. Eviction
            // drops only an idle cached view; no arena storage is duplicated.
            let old_views = KVec::with_capacity(next.views.len(), GFP_KERNEL)?;
            self.cached_views.reserve(1, GFP_KERNEL)?;
            if self.cached_views.len() == 3 {
                drop(self.cached_views.remove(0).map_err(|_| EIO)?);
            }
            let mut old = CachedViews {
                binding: core::mem::replace(&mut self._binding, next.binding),
                command: core::mem::replace(&mut self._client_command, next.command),
                views: old_views,
            };
            for (i, view) in next.views {
                old.views.push((i, self.objects[i].swap_gpu_view(view)?), GFP_KERNEL)?;
            }
            self.cached_views.push(old, GFP_KERNEL)?;
        }
        self.checkpoint=(self.sequence,self.head,self.batch_count);
        self.batch_count=0;
        if let Err(error)=self.append(control) {
            self.abort_batch()?;
            return Err(error);
        }
        Ok(())
    }
    /// Append only while the previous batch is fully retired and this one
    /// remains unpublished. Firmware serializes each command, including its
    /// original completion flush, in this one compute queue.
    pub(crate) fn append(&mut self,control:crate::agx_compute::Control)->Result {
        if self.batch_count>=storage::SLOTS {return Err(E2BIG);}
        self.slot=self.batch_count;
        for i in storage::RESET {
            let offset=storage::slot_offset(i,self.slot).map_err(|_|EINVAL)?;
            let size=storage::allocation(i).map_err(|_|EINVAL)?.size;
            self.objects[i].fill_range(offset,size,if i==PREEMPTION {0xcc} else {0})?;
        }
        self.objects[NOTIFIER].u32(state::NOTIFIER_CONTEXT,self._binding.slot())?;
        self.sequence=self.sequence.checked_add(1).ok_or(EOVERFLOW)?;
        Self::encode_command(&mut self.objects,&mut self.command_bytes,self._binding.slot(),control,self.sequence,self.slot)?;
        let stamp=self.stamp();
        Self::encode_sequence(&mut self.objects,&mut self.sequence_bytes,self.stats,self._binding.slot(),stamp,self.slot)?;
        self.objects[storage::EVENT].u32(0,self.sequence as u32)?;
        self.objects[storage::COUNTER].u32(0,self.sequence as u32)?;
        let address=self.objects[storage::COMMAND].va()+storage::slot_offset(storage::COMMAND,self.slot).map_err(|_|EINVAL)? as u64;
        self.objects[storage::RING].u64(usize::from(self.head)*8,address)?;
        self.head=(self.head+1)%queue::QUALIFIED_CAPACITY as u16;
        self.batch_count+=1;
        self.objects[storage::QUEUE_STATE].u32(queue::WRITE,u32::from(self.head))?;
        agx_memory::publish();crate::mem::sync();crate::mem::tlbi_all();crate::mem::sync();
        Ok(())
    }
    /// Roll back producer state before any SubmitQueue publication. Old ring
    /// entries beyond the restored cursor are unreachable and get overwritten.
    pub(crate) fn abort_batch(&mut self)->Result {
        (self.sequence,self.head,self.batch_count)=self.checkpoint;
        self.slot=self.batch_count.saturating_sub(1);
        self.objects[storage::EVENT].u32(0,self.sequence as u32)?;
        self.objects[storage::COUNTER].u32(0,self.sequence as u32)?;
        self.objects[storage::QUEUE_STATE].u32(queue::WRITE,u32::from(self.head))?;
        agx_memory::publish();Ok(())
    }
    fn offset(&self,owner:usize)->Result<usize> {
        storage::slot_offset(owner,self.slot).map_err(|_|EINVAL)
    }
    /// Same cache-generation contract as M4: StartCompute and the persistent
    /// notifier must use the same tag before the firmware flushes user stamps.
    /// RTKit 2419: StartCompute.counter1 +0x34; JobMeta.flush_stamps +0x804.
    pub(crate) fn prepare_completion(&mut self,coalesce:bool)->Result {
        let sequence=self.offset(storage::MICROSEQUENCE)?;
        let command=self.offset(storage::COMMAND)?;
        self.objects[storage::MICROSEQUENCE].u32(sequence+microsequence::GENERATION,0)?;
        self.objects[storage::COMMAND].u32(command+command::FLUSH_STAMPS,u32::from(!coalesce))?;
        agx_memory::publish();Ok(())
    }
    pub(crate) fn set_user_timestamps(&mut self,addresses:[u64;2])->Result {
        let offset=self.offset(storage::COMMAND)?+command::USER_TIMESTAMPS;
        self.objects[storage::COMMAND].u64(offset,addresses[0])?;
        self.objects[storage::COMMAND].u64(offset+8,addresses[1])?;
        let pointer=if addresses==[0,0] {0} else {self.objects[storage::COMMAND].va()+offset as u64};
        let sequence=self.offset(storage::MICROSEQUENCE)?;
        for start in [microsequence::TIMESTAMP_START,microsequence::TIMESTAMP_END] {
            self.objects[storage::MICROSEQUENCE].u64(sequence+start+microsequence::USER_TIMESTAMP_POINTER,pointer)?;
        }
        agx_memory::publish();Ok(())
    }
    pub(crate) fn set_attachments(&mut self,attachments:&crate::agx_attachments::Attachments)->Result {
        let sequence=self.offset(storage::MICROSEQUENCE)?;
        self.objects[storage::MICROSEQUENCE].write(sequence+microsequence::ATTACHMENTS,&attachments.0)?;
        self.objects[storage::MICROSEQUENCE].write(sequence+microsequence::HAS_ATTACHMENTS,&[u8::from(attachments.count()!=0)])?;
        agx_memory::publish();Ok(())
    }
    fn stamp(&self) -> u32 { timeline::stamp(0xc1000000,self.sequence) }
    pub(crate) fn gpu_ns(&mut self)->Result<u64> {
        let mut total=0;
        for slot in 0..self.batch_count {total+=self.slot_gpu_ns(slot)?;}
        Ok(total)
    }
    pub(crate) fn slot_gpu_ns(&mut self,slot:usize)->Result<u64> {
        if slot>=self.batch_count {return Err(EINVAL);}
        let start=self.objects[TIMESTAMPS[0]].read_u64(storage::slot_offset(TIMESTAMPS[0],slot).map_err(|_|EINVAL)?)?;
        let end=self.objects[TIMESTAMPS[1]].read_u64(storage::slot_offset(TIMESTAMPS[1],slot).map_err(|_|EINVAL)?)?;
        Ok(if start!=0 && end>=start {(end-start)*1000/24} else {0})
    }
    /// A batch span includes gaps between its compute commands, unlike gpu_ns.
    /// Read only after stamps, queues, events and idle state prove retirement.
    pub(crate) fn batch_gpu_span(&mut self)->Result<[u64;2]> {
        if self.batch_count == 0 {return Err(EINVAL);}
        Ok([self.objects[TIMESTAMPS[0]].read_u64(storage::slot_offset(TIMESTAMPS[0],0).map_err(|_|EINVAL)?)?,
            self.objects[TIMESTAMPS[1]].read_u64(storage::slot_offset(TIMESTAMPS[1],self.batch_count-1).map_err(|_|EINVAL)?)?])
    }
    pub(crate) fn head(&self) -> u16 { self.head }
    pub(crate) fn ordinal(&self)->u64 {self.sequence}
    pub(crate) fn first(&self) -> bool { self.sequence == self.batch_count as u64 }
    pub(crate) fn progress(&mut self, dev: &driver::AsahiDevice) -> Result {
        // Report real completions by elapsed time as well as sequence count;
        // batches need not land on a multiple of 128.
        if !crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
            return Ok(());
        }
        let now = <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get();
        if self.last_progress_ns != 0 && now - self.last_progress_ns < 1_000_000_000
            && self.sequence != 1 && self.sequence % 128 != 0
            && !crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
            return Ok(());
        }
        self.last_progress_ns = now;
        dev_info!(dev.as_ref(), "M3_COMPUTE_RETIRED sequence={} context={} stamp={:#x}/{:#x} queue={}/{} head={} idle=1 cache_flush=1\n",
            self.sequence, self._binding.slot(), self.objects[storage::STAMP].read_u32(0)?,
            self.objects[storage::FW_STAMP].read_u32(0)?, self.objects[storage::QUEUE_STATE].read_u32(queue::DONE)?,
            self.objects[storage::QUEUE_STATE].read_u32(queue::READ)?, self.head);
        Ok(())
    }
    pub(crate) fn registers(&self)->Result<u64> {Ok(self.objects[storage::COMMAND].gpu_va()?+command::REGISTERS as u64)}
    pub(crate) fn queue(&self)->u64 {self.objects[storage::QUEUE].va()}
    pub(crate) fn complete(&mut self)->Result<bool> {
        Ok(self.objects[storage::STAMP].read_u32(0)?==self.stamp()
            && self.objects[storage::FW_STAMP].read_u32(0)?==self.stamp()
            && self.objects[storage::QUEUE_STATE].read_u32(queue::DONE)?==u32::from(self.head)
            && self.objects[storage::QUEUE_STATE].read_u32(queue::READ)?==u32::from(self.head))
    }
    pub(crate) fn log(&mut self,dev:&driver::AsahiDevice)->Result {
        for off in (0..512).step_by(128) {
            let mut raw=[0u8;128]; self.objects[storage::COMMAND].read(off,&mut raw)?;
            dev_info!(dev.as_ref(),"M3 post-command {:#x} {:02x?}\n",off,raw);
        }
        let stamp=self.objects[storage::STAMP].read_u32(0)?;
        let fw=self.objects[storage::FW_STAMP].read_u32(0)?;
        let done=self.objects[storage::QUEUE_STATE].read_u32(queue::DONE)?;
        let read=self.objects[storage::QUEUE_STATE].read_u32(queue::READ)?;
        dev_info!(dev.as_ref(),"M3 compute: stamps={:#x}/{:#x} done={} read={}\n",stamp,fw,done,read);
        let mut record=[0u8;0x84];self.objects[storage::POOL].read(0,&mut record)?;
        dev_info!(dev.as_ref(),"M3 compute pool={:02x?}\n",record);
        let mut roots=[(0,0);64];self._binding.vm().context_roots(&mut roots)?;
        dev_info!(dev.as_ref(),"M3 context {} roots={:x?}, bootstrap={:x?}\n",self._binding.slot(),roots[self._binding.slot() as usize],roots[0]);
        Ok(())
    }
}
