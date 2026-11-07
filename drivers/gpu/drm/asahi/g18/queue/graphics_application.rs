// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Retired-frame reuse. Ring slots wrap, but native event identities do not.
use super::*;
use crate::g18::uapi::application_runtime::Frame;

impl G18GraphicsQueues {
    pub(crate) fn prepare_application_pass(&mut self, dev: &AsahiDrmDevice,
        pass: &G18QueuedPass, frame: Frame) -> Result {
        let before = frame.inner_before();
        if self.counters()? != [[before;3];2]
            || self.step != if frame.generation == 1 {3} else {5}
            || self.completion_list()? != [0,self.job_list()] {return Err(EBUSY);}
        let status=self.firmware_status()?;
        if !Self::installed(&status,&self.addresses) || !self.scheduler_bound(&status) {return Err(EIO);}
        let pb=pass.parameter_buffer.ok_or(EINVAL)?;
        let (pool,slot)=self.pool_and_pb_status(pass.uma.slot,pb.slot)?;
        if pool != [pass.uma.state,pass.uma.generation.ok_or(EINVAL)?,(2u64<<32)|3,0x600,0]
            || slot != [pb.state,pb.generation,u64::from(pass.context_id),0x200,0,0x400] {return Err(EIO);}
        for stage in [1,0] {
            let state=G18KickQueueState {current_stamp:u64::from(frame.generation),
                previous_stamp:if frame.generation==1 {None} else {Some(u64::from(frame.generation-1))},
                base:1,submitted_kicks:(frame.generation-1) as u16};
            let (entry,kick)=g18_stage_kick(stage,pass,state,self.queue_address(stage)).map_err(|_|EINVAL)?;
            let config=g18_stage_config(stage,3,(self.addresses.entry_low[stage],self.addresses.entry_high[stage]),pass);
            self.entries[stage].with_bytes_mut(|out| {
                let slot=out.get_mut(entry.offset as usize..entry.offset as usize+entry.bytes.len()).ok_or(EINVAL)?;
                slot.copy_from_slice(&entry.bytes);
                if slot!=entry.bytes {return Err(EIO);} Ok(())
            })?;
            let a=self.addresses.graph;let at=CONFIGS[stage]+0x300;
            self.graph.with_bytes_mut(|out| {
                out[at..at+G18_CHANNEL_CONFIG_BYTES].copy_from_slice(&config);
                out[KICKS[stage]..KICKS[stage]+G18_KICK_BYTES].copy_from_slice(&kick);
                for (i,address) in [pass.command_high[stage],a+at as u64,a+KICKS[stage] as u64].into_iter().enumerate() {
                    put64(out,ITEMS[stage]+((before as usize+i)%CAPACITY as usize)*8,address);
                }
                if out[at..at+G18_CHANNEL_CONFIG_BYTES]!=config
                    || out[KICKS[stage]..KICKS[stage]+G18_KICK_BYTES]!=kick {return Err(EIO);} Ok(())
            })?;
            if frame.generation<=3 {
                for (name,bytes) in [("sksm",entry.bytes.as_slice()),("kick",kick.as_slice()),("tag15",config.as_slice())] {
                    dev_info!(dev.as_ref(),"G18_APP_DIGEST frame={} stage={} name={} fnv={:#018x}\n",
                        frame.generation,stage,name,crate::g18::fw::graphics_payload::fnv64(bytes));
                }
            }
        }
        fence(Ordering::SeqCst);self.step=4;Ok(())
    }

    /// The first firmware-owned precondition of the next frame's
    /// [`Self::prepare_application_pass`] that does not hold after `frame`
    /// retired, if any. A firmware recovery can reset queue state.
    pub(crate) fn render_unready(&mut self, pass: &G18QueuedPass, frame: Frame) -> Result<Option<&'static str>> {
        let status=self.firmware_status()?;
        let pb=pass.parameter_buffer.ok_or(EINVAL)?;
        let (pool,slot)=self.pool_and_pb_status(pass.uma.slot,pb.slot)?;
        Ok(if self.counters()? != [[frame.inner_after();3];2] {Some("counters")}
            else if self.completion_list()? != [0,self.job_list()] {Some("completion-list")}
            else if !Self::installed(&status,&self.addresses) {Some("installed")}
            else if !self.scheduler_bound(&status) {Some("scheduler")}
            else if pool != [pass.uma.state,pass.uma.generation.ok_or(EINVAL)?,(2u64<<32)|3,0x600,0] {Some("pool")}
            else if slot != [pb.state,pb.generation,u64::from(pass.context_id),0x200,0,0x400] {Some("parameter-buffer")}
            else {None})
    }

    /// Every firmware word [`Self::render_unready`] reads, for the kernel log.
    pub(crate) fn render_state(&mut self, pass: &G18QueuedPass) -> Result<(G18GraphicsFirmwareStatus, [u64;5], [u64;6])> {
        let pb=pass.parameter_buffer.ok_or(EINVAL)?;
        let (pool,slot)=self.pool_and_pb_status(pass.uma.slot,pb.slot)?;
        Ok((self.firmware_status()?,pool,slot))
    }

    pub(crate) fn publish_application_pass(&mut self, frame: Frame) -> Result<[[u8;24];2]> {
        if self.step!=4 || self.counters()?!=[[frame.inner_before();3];2] {return Err(EBUSY);}
        let mut records=[[0;24];2];
        fence(Ordering::SeqCst);
        for stage in [1,0] {
            self.graph.write_u32(self.addresses.graph+POINTERS[stage] as u64+0x40,frame.inner_after())?;
            records[stage]=encode_g18_outer_record(self.queue_address(stage),G18_GRAPHICS_QUEUE_IDS[stage],frame.inner_after() as u16);
        }
        fence(Ordering::SeqCst);self.step=5;Ok([records[1],records[0]])
    }
}

impl G18ParameterBuffer {
    /// Keep the PB resident in its hardware slot across VM handoffs. Every
    /// alias uses the addresses selected by the PB owner, including row VA.
    pub(crate) fn application_aliases(&mut self, vm: &mmu::Vm) -> Result<KVec<mmu::KernelMapping>> {
        let a=self.addresses;let bias=crate::g18::address_map::COMPACT_BIAS;
        let mut aliases=KVec::new();
        for (object,low) in [(&mut self.page_list,a.page_list_low),(&mut self.metrics,a.metrics_low),
            (&mut self.scene_scratch,a.scene_scratch_low),(&mut self.discard,a.discard_low)] {
            for address in [low,low+bias] {aliases.push(object.map_alias_at(vm,address,mmu::PROT_GPU_SHARED_RW)?,GFP_KERNEL)?;}
        }
        if a.page_list_row!=a.page_list_low && a.page_list_row!=a.page_list_low+bias {
            aliases.push(self.page_list.map_alias_at(vm,a.page_list_row,mmu::PROT_GPU_SHARED_RW)?,GFP_KERNEL)?;
        }
        for (index,block) in self.blocks.iter_mut().enumerate() {
            let low=a.blocks_low+index as u64*G18_PB_BLOCK_BYTES;
            for address in [low,low+bias] {aliases.push(block.map_alias_at(vm,address,mmu::PROT_GPU_SHARED_RW)?,GFP_KERNEL)?;}
        }
        Ok(aliases)
    }

    /// The PB words a recovery can leave behind: the firmware-visible state
    /// (ring cursors, free list, the +0x1c flag firmware sets when a recovery
    /// flushes a render using this PB, b000 0xfd58-0xfd9c), the active scene
    /// record, the submitted-render counter and the scene index.
    pub(crate) fn recovery_words(&mut self) -> Result<([u32;G18_PB_STATE_FIRMWARE_BYTES/4],[u32;SCENE_BYTES/4],u32,u32)> {
        let state: [u8;G18_PB_STATE_FIRMWARE_BYTES]=self.state.with_bytes_mut(|out|out[..G18_PB_STATE_FIRMWARE_BYTES].try_into().map_err(|_|EINVAL))?;
        let start=SCENES+self.policy.scene as usize*SCENE_BYTES;
        let scene: [u8;SCENE_BYTES]=self.graph.with_bytes_mut(|out|out[start..start+SCENE_BYTES].try_into().map_err(|_|EINVAL))?;
        let word=|b:&[u8],i:usize|u32::from_le_bytes(b[i*4..i*4+4].try_into().unwrap_or([0;4]));
        let counter=self.graph.read_u32(self.addresses.graph_high+COUNTER as u64)?;
        Ok((core::array::from_fn(|i|word(&state,i)),core::array::from_fn(|i|word(&scene,i)),counter,self.policy.scene))
    }


    pub(crate) fn application_scene(&mut self, template: Option<&[u8;SCENE_BYTES]>) -> Result<[u8;SCENE_BYTES]> {
        let start=SCENES+self.policy.scene as usize*SCENE_BYTES;
        self.graph.with_bytes_mut(|out| {
            if let Some(bytes)=template {out[start..start+SCENE_BYTES].copy_from_slice(bytes);}
            out[start..start+SCENE_BYTES].try_into().map_err(|_|EINVAL)
        })
    }

    /// Hardware owns the ring cursors and free-list ordering after a render.
    /// Do not re-encode them. Check the stable address/capacity and state owner.
    pub(crate) fn verify_resident(&mut self, table: &mut MappedObject, context: u32,
        generation: u32) -> Result<G18PbReadback> {
        if generation==1 {return self.verify_before_kick(table);}
        let offset=self.policy.slot as usize*G18_PB_ROW_BYTES;
        let row: [u8;G18_PB_ROW_BYTES]=table.with_bytes_mut(|out|out[offset..offset+G18_PB_ROW_BYTES].try_into().map_err(|_|EINVAL))?;
        let word=|i:usize|u32::from_le_bytes(row[i..i+4].try_into().unwrap());
        if g18_pb_row_decoded_address(&row)!=Some(self.inputs.page_list_gpu)
            || (word(4)&0x3f_ffff)!=(self.row.ok_or(EINVAL)?[4..8].try_into().map(u32::from_le_bytes).map_err(|_|EINVAL)?&0x3f_ffff)
            || word(8)&0x3f_ffff>=self.policy.capacity*G18_PB_PAGES_PER_BLOCK
            || word(12)&0x3f_ffff>=self.policy.capacity*G18_PB_PAGES_PER_BLOCK {return Err(EIO);}
        let state: [u8;G18_PB_STATE_FIRMWARE_BYTES]=self.state.with_bytes_mut(|out|out[..G18_PB_STATE_FIRMWARE_BYTES].try_into().map_err(|_|EINVAL))?;
        if u32::from_le_bytes(state[8..12].try_into().map_err(|_|EINVAL)?)!=context {return Err(EIO);}
        let counter=self.graph.read_u32(self.addresses.graph_high+COUNTER as u64)?;
        if counter!=generation {return Err(EIO);}
        Ok(G18PbReadback {row,state,counter})
    }
}
