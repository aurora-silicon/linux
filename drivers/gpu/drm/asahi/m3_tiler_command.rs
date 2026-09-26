// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::{agx_render::Geometry, m3_compute_layout::{GpuVa,RegisterSet},
    m3_init_layout::{Error,Region}, m3_queue_layout::FirmwareVa,
    m3_render_sequence::ta};
pub(crate) const SIZE:usize=0x92b;
pub(crate) const REGISTERS:usize=ta::REGISTERS;
/// The registers G15G does not take; the others keep the G15S order.
pub(crate) const G15G_OMIT:[u32;14]=[0x1c041,0x1c9d0,0x1c948,0x1c888,0x1c890,0x1c918,0x1c089,0x1c9e0,0x16c41,0x1ca40,0x1c9a8,0x1c920,0xa5a1,0xd419];
pub(crate) const REGISTER_COUNT:usize=72;
pub(crate) const REGISTER_STRIDE:usize=12;
pub(crate) const STAMP_VALUE:usize=0x780;
pub(crate) const STRUCT3_STAMP:usize=ta::STRUCT3+0xf0;
pub(crate) const CONTEXT:usize=0xc;
fn word(out:&mut[u8],off:usize,value:u32) {out[off..off+4].copy_from_slice(&value.to_le_bytes());}
fn pointer(out:&mut[u8],off:usize,value:u64) {out[off..off+8].copy_from_slice(&value.to_le_bytes());}
fn compact(address:u64)->Result<u64,Error> {
    crate::agx_render_state::compact(address).map_err(|_|Error::Address)
}
pub(crate) struct Command {
    pub(crate) context:u32,
    /// The register list of the GPU: G15S's, or G15G's without [`G15G_OMIT`].
    pub(crate) register_set:RegisterSet,
    pub(crate) notifier:FirmwareVa,
    pub(crate) manager:FirmwareVa,
    pub(crate) scene:FirmwareVa,
    pub(crate) empty:FirmwareVa,
    pub(crate) gpu_alias:GpuVa,
    pub(crate) sequence:Region,
    pub(crate) pool:FirmwareVa,
    pub(crate) scratch:FirmwareVa,
    pub(crate) stamp:FirmwareVa,
    pub(crate) fw_stamp:FirmwareVa,
    pub(crate) stamp_value:u32,
    pub(crate) timestamps:[FirmwareVa;2],
    pub(crate) user_timestamps:[FirmwareVa;2],
    pub(crate) preemption:[GpuVa;3],
    pub(crate) tilemap:GpuVa,
    pub(crate) tpc:GpuVa,
    pub(crate) tpc_bytes:u64,
    pub(crate) heap:GpuVa,
    pub(crate) scene_user:GpuVa,
    /// Qualified register uses the initial scene-list entry for every draw,
    /// even while RenderScene advances. Preserve this separate binding.
    pub(crate) initial_scene_entry:FirmwareVa,
    pub(crate) geometry:Geometry,
    pub(crate) page_count:u32,
    /// Already encoded, validated compact client VDM pointer. Zero is valid
    /// only for host fixture construction before the application patch.
    pub(crate) vdm:u64,
    pub(crate) multisample:u64,
    pub(crate) ppp:u32,
    pub(crate) process_empty_tiles:bool,
}
impl Command {
    /// All address checks precede mutation. Zero all reserved fields, then
    /// emit semantic registers and explicit packed command/tail fields.
    pub(crate) fn encode(&self,out:&mut[u8])->Result<(),Error> {
        if out.len()<SIZE {return Err(Error::Size);}
        if self.context>=64 || self.page_count==0 || self.tpc_bytes==0 {return Err(Error::Address);}
        let registers=self.gpu_alias.get().checked_add(REGISTERS as u64).ok_or(Error::Address)?;
        GpuVa::new(self.gpu_alias.get().checked_add((SIZE-3) as u64).ok_or(Error::Address)?)
            .map_err(|_|Error::Address)?;
        let seq=self.sequence.at(0,ta::LENGTH)?;
        let tilemap=compact(self.tilemap.get())?;
        let tpc=compact(self.tpc.get())?;
        let heap=compact(self.heap.get())?;
        let preemption=[compact(self.preemption[0].get())?,compact(self.preemption[1].get())?,compact(self.preemption[2].get())?];
        let user=compact(self.scene_user.get().checked_add(0x40).ok_or(Error::Address)?)?;
        let g=&self.geometry;
        let layers=g.layers;
        let layer_metadata=compact(self.heap.get().checked_add(if layers>1 {0x200}else{0}).ok_or(Error::Address)?)?;
        let layer_mode=if layers>1 {0xc000 | if self.process_empty_tiles{0x2000}else{0x1000} | u64::from(layers-1)}else{0x8000};
        if self.vdm>=0x80_0000_0000 || self.vdm&3!=0 {return Err(Error::Address);}
        let values:[(u32,u64);REGISTER_COUNT]=[
            (0x1748,1),(0x10141,0x200),(0x1c039,tilemap),(0x1c9c8,tilemap),
            (0x1c041,0),(0x1c9d0,0),(0x1c0a1,tpc),
            (0x1c031,heap|1<<63),(0x1c9c0,heap|1<<63),
            (0x1c051,0x003a0012006b0003),(0x1c061,1),
            (0x10149,g.utile_config),(0x10139,self.multisample),
            (0x10111,preemption[0]),(0x1c9b0,preemption[0]),
            (0x10119,preemption[1]),(0x1c9b8,preemption[1]),
            (0x1c958,1),(0x1c950,preemption[2]|1<<50),(0x1c930,0),
            (0x1c880,self.vdm),(0x1c898,0),(0x1c948,0),(0x1c888,0),
            (0x1c890,1),(0x1c918,0),(0x1c079,layer_metadata),(0x1c9d8,layer_metadata),
            (0x1c089,0),(0x1c9e0,0),(0x16c41,0),(0x1ca40,0),
            (0x1c9a8,u64::from(self.page_count)),(0x1c920,0),(0x10151,0),
            (0x1c199,0),(0x1c1a1,0),(0x1c1a9,0),(0x1c1b1,0),(0x1c1b9,0),
            (0x1c8f8,0x8860),(0x1c0b1,u64::from(g.region_stride)),(0x1c850,u64::from(g.region_stride)),
            (0x10131,0x88),(0x10121,2|u64::from(self.ppp&0x201)),
            (0x10129,u64::from(g.pixels)),(0x101b9,u64::from(g.screen)),
            (0x1c069,u64::from(g.x_blocks)),(0x1c071,u64::from(g.y_blocks)),
            (0x1c081,u64::from(g.blocks)),(0x1c0a9,u64::from(g.tpc_stride)),
            (0x10171,0x100),(0x10169,layer_mode),(0x12099,0x1c),(0x101e1,0x1c),
            (0x1c9e8,0),(0x1a099,0),(0x1a0a1,0),(0x1a069,0),(0x1a071,0),
            (0x1a0c9,0),(0x1a0d1,0),(0x101c9,0),(0xd471,0),(0x1a0f1,8),
            (0x10799,0xff0000),(0xa5a1,0x7f00400020),(0xd419,0x200000001),
            (0x1c830,0),(0x1ca30,user),(0x16c39,user),
            (0x1c910,0xa0_00000000|(self.initial_scene_entry.get()&0xf_ffffffff)|1),
        ];
        let bytes=&mut out[..SIZE];bytes.fill(0); // TA opcode is zero
        pointer(bytes,4,1); // qualified work-command counter remains one
        word(bytes,CONTEXT,self.context);
        for (offset,value) in [(0x14,self.notifier.get()),(0x24,self.manager.get()),
            (0x2c,self.scene.get()),(0x34,self.empty.get()),(0x740,registers),
            (0x760,self.tpc.get()),(0x768,self.tpc_bytes),(0x770,seq)] {pointer(bytes,offset,value);}
        let g15g=self.register_set==RegisterSet::G15G;
        let mut count=0;
        for (register,value) in values.into_iter().filter(|(r,_)| !g15g || !G15G_OMIT.contains(r)) {
            let n=count;count+=1;
            word(bytes,REGISTERS+n*REGISTER_STRIDE,register);
            pointer(bytes,REGISTERS+n*REGISTER_STRIDE+4,value);
        }
        bytes[0x748..0x74a].copy_from_slice(&(count as u16).to_le_bytes());
        bytes[0x74a..0x74c].copy_from_slice(&((count*REGISTER_STRIDE) as u16).to_le_bytes());
        word(bytes,0x778,ta::LENGTH as u32);
        word(bytes,0x77c,1); // fragment event slot
        word(bytes,STAMP_VALUE,self.stamp_value);
        pointer(bytes,ta::STRUCT3+0x24,preemption[0]);
        word(bytes,ta::STRUCT3+0xbc,0x67); // retained producer encoder_id
        word(bytes,ta::STRUCT3+0xc4,u32::MAX); // unk_53c
        word(bytes,ta::STRUCT3+0xd0,1); // sampler_max
        pointer(bytes,ta::STRUCT3+0xe0,self.stamp.get());
        pointer(bytes,ta::STRUCT3+0xe8,self.fw_stamp.get());
        word(bytes,STRUCT3_STAMP,self.stamp_value);
        word(bytes,ta::STRUCT3+0xfc,1); // flush completion stamps
        word(bytes,ta::STRUCT3+0x100,ta::UUID);
        for (offset,value) in [(8,self.timestamps[0].get()),(16,self.timestamps[1].get()),
            (0x18,self.user_timestamps[0].get()),(0x20,self.user_timestamps[1].get()),
            (0x2e,self.pool.get()),(0x37,0xad0000),(0x3f,0x1600000),
            (0x47,self.scratch.get())] {pointer(bytes,ta::COMMAND_TAIL+offset,value);}
        bytes[ta::COMMAND_TAIL+0x36]=1; // unk_846
        bytes[ta::COMMAND_TAIL+0x4f]=1; // unk_85f
        Ok(())
    }
}
