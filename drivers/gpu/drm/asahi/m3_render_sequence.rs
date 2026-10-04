// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::{m3_init_layout::{Error, Region}, m3_queue_layout::FirmwareVa,
    m3_pool_layout::GpuRegion};
pub(crate) const STORAGE: usize = 0x1000;
pub(crate) const USER_TIMESTAMP_POINTER: usize = 0x24;
pub(crate) mod ta {
    pub const TIMESTAMP_START: usize = 0x1cc;
    pub const TIMESTAMP_END: usize = 0x20c;
    pub const FINALIZE: usize = 0x248;
    pub const LENGTH: usize = 0x2d8;
    pub const POOL: usize = 0x18c;
    pub const COMMAND_TAIL: usize = 0x8a8;
    pub const USER_PAIR: usize = COMMAND_TAIL + 0x18;
    pub const REGISTERS: usize = 0x40;
    pub const STRUCT3: usize = 0x784;
    pub const SAMPLER_ARRAY: usize = 0x84c;
    pub const SCRATCH_STATE: usize = 0x890;
    pub const STATS: usize = 0x1884;
    pub const UUID: u32 = 0x7a0000;
}
pub(crate) mod fragment {
    pub const TIMESTAMP_START: usize = 0x1dc;
    pub const TIMESTAMP_END: usize = 0x21c;
    pub const FINALIZE: usize = 0x258;
    pub const LENGTH: usize = 0x310;
    pub const POOL: usize = 0x1a4;
    pub const COMMAND_TAIL: usize = 0xbf0;
    pub const USER_PAIR: usize = COMMAND_TAIL + 0x18;
    pub const REGISTERS: usize = 0x80;
    pub const BUSY: usize = 0xb70;
    pub const STRUCT6: usize = 0xb74;
    // Start.struct7 and Finalize.job_meta refer to this prefix, not +0xbac.
    pub const STRUCT7: usize = 0xba0;
    pub const SCRATCH_FLAG: usize = 0xa58;
    pub const SCRATCH_STATE: usize = 0xbd8;
    pub const STATS: usize = 0x24c8;
    pub const UUID: u32 = 0x3d0000;
}
fn word(out: &mut [u8], off: usize, value: u32) { out[off..off+4].copy_from_slice(&value.to_le_bytes()); }
fn pointer(out: &mut [u8], off: usize, value: u64) { out[off..off+8].copy_from_slice(&value.to_le_bytes()); }
/// Resources common to both stages; command, sequence and scene are per-pass.
/// Stats and notifier belong to Config / Render for the entire submission.
#[derive(Clone, Copy)]
pub(crate) struct Owners {
    pub(crate) command: Region,
    pub(crate) sequence: Region,
    pub(crate) stats: Region,
    pub(crate) notifier: Region,
    pub(crate) queue: FirmwareVa,
    pub(crate) scene: FirmwareVa,
    pub(crate) manager: FirmwareVa,
    pub(crate) pool: FirmwareVa,
    pub(crate) fw_stamp: FirmwareVa,
    pub(crate) context: u32,
    pub(crate) stamp: u32,
}
struct Timestamps { time:u64, pair:u64, end:u64, complete:u64 }
impl Timestamps {
    fn new(command:Region, tail:usize)->Result<Self,Error> {
        Ok(Self {time:command.at(tail,8)?,pair:command.at(tail+8,16)?,
            end:command.at(tail+16,8)?,complete:command.at(tail+0x58,8)?})
    }
    fn encode(&self,out:&mut[u8],start:usize,end:usize,queue:FirmwareVa,uuid:u32) {
        for (offset,opcode,selected) in [(start,0x80000003,self.pair),(end,3,self.end)] {
            word(out,offset,opcode);
            for (field,value) in [(4,self.time),(0xc,self.pair),(0x14,selected),
                (0x1c,queue.get()),(0x2c,self.complete)] {pointer(out,offset+field,value);}
            word(out,offset+0x34,uuid);
        }
        word(out,end-4,1); // G15 WaitForIdle2, between the timestamp packets
    }
}
pub(crate) struct Tiler {pub(crate) owners:Owners,pub(crate) scratch:FirmwareVa}
impl Tiler {
    /// Validate every referenced subobject before changing output bytes.
    pub(crate) fn encode(&self,out:&mut[u8])->Result<(),Error> {
        use ta::*;
        let o=self.owners;
        if out.len()<STORAGE {return Err(Error::Size);}
        if o.context>=64 {return Err(Error::Address);}
        let registers=o.command.at(REGISTERS,128*12)?;
        let state=o.command.at(STRUCT3,0x10c)?;
        let sampler=o.command.at(SAMPLER_ARRAY,8)?;
        let scratch_state=o.command.at(SCRATCH_STATE,0x18)?;
        let timestamps=Timestamps::new(o.command,COMMAND_TAIL)?;
        let request=o.command.at(COMMAND_TAIL+0x50,8)?;
        let stats=o.stats.at(STATS,8)?;
        let notifier=o.notifier.at(0xa8,8)?;
        o.sequence.at(0,STORAGE)?;
        let pool_field=o.sequence.at(POOL,8)?;
        let bytes=&mut out[..STORAGE];bytes.fill(0);
        word(bytes,0,5); // StartTA
        for (field,value) in [(0x14,registers),(0x1c,o.manager.get()),(0x24,o.scene.get()),
            (0x2c,stats),(0x34,o.queue.get()),(0x3c,sampler),(0x64,state),
            (0x6c,scratch_state),(POOL,o.pool.get()),(0x1a4,self.scratch.get()),
            (0x1bc,notifier)] {pointer(bytes,field,value);}
        word(bytes,0x44,o.context);
        pointer(bytes,0x78,u64::from(UUID)<<32); // retained unk_178_0; StartTA.uuid stays zero
        pointer(bytes,0x1b4,1); // g15_unk_28
        timestamps.encode(bytes,TIMESTAMP_START,TIMESTAMP_END,o.queue,UUID);
        word(bytes,FINALIZE,6); // FinalizeTA
        for (field,value) in [(4,o.scene.get()),(0xc,o.manager.get()),(0x14,stats),
            (0x1c,o.queue.get()),(0x24,sampler),(0x34,state),(0x44,o.fw_stamp.get()),
            (0x74,pool_field),(0x84,request)] {pointer(bytes,FINALIZE+field,value);}
        word(bytes,FINALIZE+0x2c,o.context);
        word(bytes,FINALIZE+0x40,UUID);
        word(bytes,FINALIZE+0x4c,o.stamp);
        word(bytes,FINALIZE+0x7c,(-(FINALIZE as i32)) as u32);
        word(bytes,LENGTH-4,0x40000002); // End, qualified completion flag
        Ok(())
    }
}
pub(crate) struct Fragment {
    pub(crate) owners:Owners,
    /// Unlike the tiler scratch reference, this is in the client GPU space.
    pub(crate) scratch:GpuRegion,
    pub(crate) queue_command_count:u32,
}
impl Fragment {
    pub(crate) fn encode(&self,out:&mut[u8])->Result<(),Error> {
        use fragment::*;
        let o=self.owners;
        if out.len()<STORAGE {return Err(Error::Size);}
        if o.context>=64 || self.scratch.size()<0x10000 {return Err(Error::Address);}
        let command=o.command.at(0,0xc73)?;
        let registers=o.command.at(REGISTERS,128*12)?;
        let busy=o.command.at(BUSY,4)?;
        let struct6=o.command.at(STRUCT6,0x2c)?;
        let struct7=o.command.at(STRUCT7,0x34)?;
        let flag=o.command.at(SCRATCH_FLAG,4)?;
        let state=o.command.at(SCRATCH_STATE,0x18)?;
        let timestamps=Timestamps::new(o.command,COMMAND_TAIL)?;
        let request=o.command.at(COMMAND_TAIL+0x50,8)?;
        let stats=o.stats.at(STATS,8)?;
        let notifier=o.notifier.at(0xa8,8)?;
        o.sequence.at(0,STORAGE)?;
        let pool_field=o.sequence.at(POOL,8)?;
        let bytes=&mut out[..STORAGE];bytes.fill(0);
        word(bytes,0,7); // Start3D
        for (field,value) in [(0x14,registers),(0x1c,o.scene.get()),(0x24,stats),
            (0x2c,busy),(0x34,struct6),(0x3c,struct7),(0x44,o.queue.get()),
            (0x4c,command),(0x74,flag),(0x7c,state),(POOL,o.pool.get()),
            (0x1bc,self.scratch.base()),(0x1d4,notifier)] {pointer(bytes,field,value);}
        word(bytes,0x54,o.context);
        word(bytes,0x58,1); // unk_50
        pointer(bytes,0x68,u64::from(self.queue_command_count));
        word(bytes,0x98,UUID);
        timestamps.encode(bytes,TIMESTAMP_START,TIMESTAMP_END,o.queue,UUID);
        word(bytes,FINALIZE,8); // Finalize3D
        word(bytes,FINALIZE+4,UUID);
        for (field,value) in [(0xc,o.fw_stamp.get()),(0x1c,o.scene.get()),
            (0x24,o.manager.get()),(0x34,stats),(0x3c,struct7),(0x44,busy),
            (0x4c,o.queue.get()),(0x54,command),(0x64,flag),(0x9c,pool_field),
            (0xac,request)] {pointer(bytes,FINALIZE+field,value);}
        word(bytes,FINALIZE+0x14,o.stamp);
        pointer(bytes,FINALIZE+0x2c,1); // unk_2c
        pointer(bytes,FINALIZE+0x5c,u64::from(o.context)); // firmware u64, unlike Start3D
        word(bytes,FINALIZE+0xa4,(-(FINALIZE as i32)) as u32);
        word(bytes,FINALIZE+0xa8,1); // unk_98
        word(bytes,LENGTH-4,0x40000002);
        Ok(())
    }
}
