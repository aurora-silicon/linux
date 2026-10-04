// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::{agx_render::Geometry, agx_render_state::{Program,DepthStencil},
    m3_compute_layout::GpuVa, m3_init_layout::{Error,Region},
    m3_queue_layout::FirmwareVa, m3_render_sequence::fragment};
pub(crate) const SIZE:usize=0xc73;
pub(crate) const REGISTERS:usize=fragment::REGISTERS;
pub(crate) const REGISTER_COUNT:usize=79;
pub(crate) const REGISTER_STRIDE:usize=12;
pub(crate) const PARAMETERS:usize=0x790;
pub(crate) const META:usize=0xbac;
pub(crate) const STAMP_VALUE:usize=META+0x10;
fn word(out:&mut[u8],off:usize,value:u32) {out[off..off+4].copy_from_slice(&value.to_le_bytes());}
fn pointer(out:&mut[u8],off:usize,value:u64) {out[off..off+8].copy_from_slice(&value.to_le_bytes());}
/// The initial producer had different register/mirror values for tilebuffer
/// and tile mode. Keep those fields independent; the live adapter supplies
/// the same current value to both, exactly as the previous patch path did.
pub(crate) struct State {
    pub(crate) geometry:Geometry,
    pub(crate) multisample:u64,
    pub(crate) merge_upper:[u32;2],
    pub(crate) eot:Program,
    pub(crate) background:Program,
    pub(crate) partial_eot:Program,
    pub(crate) partial_background:Program,
    pub(crate) scissor:u64,
    pub(crate) depth_bias:u64,
    pub(crate) query:u64,
    pub(crate) isp_control:u32,
    pub(crate) tilebuffer_control:u32,
    pub(crate) mirror_tilebuffer_control:u32,
    pub(crate) tile_mode:u64,
    pub(crate) mirror_tile_mode:u32,
    pub(crate) depth:DepthStencil,
    pub(crate) stencil:DepthStencil,
    // Paired ZLS metadata address/stride. Native 26A428 compressed depth
    // witness: 153c1/153c9 and partial mirror +1e0; independent of image data.
    pub(crate) depth_compression:DepthStencil,
    pub(crate) stencil_compression:DepthStencil,
    pub(crate) zls_control:u64,
    pub(crate) depth_dimensions:u32,
    pub(crate) depth_clear:u32,
    pub(crate) stencil_clear:u32,
    pub(crate) sample_size:u32,
    pub(crate) process_empty_tiles:bool,
}
pub(crate) struct Command {
    pub(crate) context:u32,
    pub(crate) sequence:Region,
    pub(crate) notifier:FirmwareVa,
    pub(crate) manager:FirmwareVa,
    pub(crate) scene:FirmwareVa,
    pub(crate) empty:FirmwareVa,
    pub(crate) gpu_alias:GpuVa,
    pub(crate) tilemap:GpuVa,
    pub(crate) heap:GpuVa,
    pub(crate) auxiliary:GpuVa,
    pub(crate) scene_user:GpuVa,
    pub(crate) scratch:GpuVa,
    pub(crate) pool:FirmwareVa,
    pub(crate) stamp:FirmwareVa,
    pub(crate) fw_stamp:FirmwareVa,
    pub(crate) stamp_value:u32,
    pub(crate) timestamps:[FirmwareVa;2],
    pub(crate) user_timestamps:[FirmwareVa;2],
    pub(crate) state:State,
}
impl Command {
    /// Check references before touching output. The caller owns validation of
    /// client resource mappings; this layer serializes their semantic state.
    pub(crate) fn encode(&self,out:&mut[u8])->Result<(),Error> {
        if out.len()<SIZE {return Err(Error::Size);}
        if self.context>=64 {return Err(Error::Address);}
        let seq=self.sequence.at(0,fragment::LENGTH)?;
        let alias=self.gpu_alias.get();
        GpuVa::new(alias.checked_add((SIZE-3) as u64).ok_or(Error::Address)?).map_err(|_|Error::Address)?;
        let registers=alias+REGISTERS as u64;
        let s=&self.state;let g=&s.geometry;
        let layer_metadata=self.heap.get().checked_add(if g.layers>1 {0x200}else{0}).ok_or(Error::Address)?;
        GpuVa::new(layer_metadata).map_err(|_|Error::Address)?;
        let user=crate::agx_render_state::compact(self.scene_user.get().checked_add(0x40).ok_or(Error::Address)?)
            .map_err(|_|Error::Address)? & !15;
        let width=(g.pixels&0xffff)+1;let height=(g.pixels>>16)+1;
        let tx=(width+31)/32;let ty=(height+31)/32;
        let native_tiles=221201|(u64::from(g.layers>1)<<32)|(u64::from(tx-1)<<44)
            |(u64::from(ty-1)<<53)|137438953472|((g.utile_config&61440)<<28);
        let values:[(u32,u64);REGISTER_COUNT]=[
            (0x1739,1),(0x10009,g.utile_config),(0x15379,s.eot.resources),(0x15381,s.eot.address),
            (0x15369,s.background.resources),(0x15371,s.background.address),
            (0x15131,u64::from(s.merge_upper[0])),(0x15139,u64::from(s.merge_upper[1])),
            (0x100a1,0),(0x15069,0),(0x15071,0),(0x16058,0),(0x10019,s.multisample),
            (0x100b1,u64::from(g.isp_blocks)),(0x16030,u64::from(g.isp_blocks)),
            (0x100d9,u64::from(g.screen)),(0x10791,0xff0200),(0x16098,self.heap.get()),
            (0x15109,s.scissor),(0x15101,s.depth_bias),(0x15021,u64::from(s.isp_control)),
            (0x15211,(u64::from(height)<<32)|u64::from(width)),(0x15049,0x100000),
            (0x10051,u64::from(s.tilebuffer_control)),(0x15321,u64::from(s.depth_dimensions)),
            (0x15301,u64::from(s.depth_clear)),(0x15309,u64::from(s.stencil_clear)),
            (0x15311,s.query&!15),(0x15319,s.zls_control),(0x15349,0x04040404),(0x15351,0),
            (0x15329,s.depth.base),(0x15331,s.depth.base),(0x15339,s.stencil.base),(0x15341,s.stencil.base),
            (0x15231,0),(0x15221,0),(0x15239,0),(0x15229,0),
            (0x15401,s.depth.stride),(0x15421,s.depth.stride),(0x15409,s.stencil.stride),(0x15429,s.stencil.stride),
            (0x153c1,s.depth_compression.base),(0x15411,s.depth_compression.stride),
            (0x153c9,s.depth_compression.base),(0x15431,s.depth_compression.stride),
            (0x153d1,s.stencil_compression.base),(0x15419,s.stencil_compression.stride),
            (0x153d9,s.stencil_compression.base),(0x15439,s.stencil_compression.stride),
            (0x16429,self.tilemap.get()),(0x16060,layer_metadata),(0x16431,u64::from(4*g.region_stride)<<24),
            (0x10039,s.tile_mode),(0x16020,0),(0x16451,0),(0x15359,0),(0x100b8,0x8860),
            (0x16461,self.auxiliary.get()),(0x16090,self.auxiliary.get()),(0x120a1,0x1c),(0x101e9,0x1c),
            (0x160a8,0),(0x16068,native_tiles),(0x160b8,0),
            (0x1a0a9,0),(0x1a0b1,0),(0x1a079,0),(0x1a081,0),(0x1a0d9,0),(0x1a0e1,0),
            (0x101c1,0),(0xd469,0),(0x1a0f9,8),(0x1c838,0),(0x1ca28,user),
            (0xa5a9,0x6000400020),(0xd429,0x200000001),
        ];
        let bytes=&mut out[..SIZE];bytes.fill(0);
        word(bytes,0,1); // fragment opcode; work counter remains zero
        word(bytes,0xc,self.context);
        for (offset,value) in [(0x14,seq),(0x20,self.notifier.get()),(0x28,self.manager.get()),
            (0x30,self.scene.get()),(0x38,self.empty.get()),(0x40,self.tilemap.get()),(0x780,registers)] {pointer(bytes,offset,value);}
        word(bytes,0x1c,fragment::LENGTH as u32);
        // The qualified patch path updates register MSAA but retains these
        // producer header values. Representation cleanup must not change that.
        pointer(bytes,0x48,0x88);word(bytes,0x50,1);
        bytes[0x54..0x56].copy_from_slice(&((g.y_blocks>>18) as u16).to_le_bytes());
        bytes[0x56..0x58].copy_from_slice(&((g.x_blocks>>18) as u16).to_le_bytes());
        word(bytes,0x68,s.merge_upper[0]);word(bytes,0x6c,s.merge_upper[1]);
        pointer(bytes,0x78,u64::from(g.tiles));
        for (n,(register,value)) in values.into_iter().enumerate() {
            word(bytes,REGISTERS+n*REGISTER_STRIDE,register);pointer(bytes,REGISTERS+n*REGISTER_STRIDE+4,value);
        }
        bytes[0x788..0x78a].copy_from_slice(&(REGISTER_COUNT as u16).to_le_bytes());
        bytes[0x78a..0x78c].copy_from_slice(&((REGISTER_COUNT*REGISTER_STRIDE) as u16).to_le_bytes());
        // Start3DJobParameters3: mirror records use their actual packed widths.
        for (offset,value) in [(0,s.depth_bias),(0x10,s.scissor),(0x20,s.query),
            (0x168,s.partial_background.resources),(0x170,s.partial_background.address),
            (0x198,s.partial_background.resources),(0x1a0,s.partial_background.address),
            (0x1a8,s.zls_control),(0x1b8,s.depth.base),(0x1c0,s.depth.stride),
            (0x1c8,s.depth_compression.stride),(0x1e0,s.depth_compression.base),
            (0x1d0,s.depth.base),(0x1d8,s.depth.base),
            (0x1e8,s.stencil.base),(0x1f0,s.stencil.stride),(0x200,s.stencil.base),(0x208,s.stencil.base),
            (0x1f8,s.stencil_compression.stride),(0x210,s.stencil_compression.base),
            (0x26c,s.partial_eot.address),(0x28c,s.partial_eot.address),(0x2c0,u64::from(s.depth_dimensions))] {
            pointer(bytes,PARAMETERS+offset,value);
        }
        for (offset,value) in [(0x1b0,0x04040404),(0x228,s.mirror_tilebuffer_control),
            (0x230,s.isp_control),(0x238,width),(0x23c,height),(0x240,0x100000),
            (0x248,s.mirror_tile_mode),(0x264,s.partial_eot.resources as u32),(0x284,s.partial_eot.resources as u32),
            (0x298,s.depth_clear),(0x29c,s.stencil_clear),(0x2a0,s.sample_size)] {word(bytes,PARAMETERS+offset,value);}
        word(bytes,0xb88,0x67); // EncoderParams.encoder_id
        word(bytes,0xb90,u32::MAX); // EncoderParams.unk_mask
        word(bytes,0xb94,u32::from(s.process_empty_tiles));
        word(bytes,0xb98,1); // retained no_clear_pipeline_textures
        pointer(bytes,META,self.stamp.get());pointer(bytes,META+8,self.fw_stamp.get());
        word(bytes,STAMP_VALUE,self.stamp_value);word(bytes,META+0x14,1); // event slot
        word(bytes,META+0x1c,1); // flush completion stamps
        word(bytes,META+0x20,fragment::UUID);word(bytes,0xbe8,1); // unk_buf2.unk_10
        for (offset,value) in [(8,self.timestamps[0].get()),(16,self.timestamps[1].get()),
            (0x18,self.user_timestamps[0].get()),(0x20,self.user_timestamps[1].get()),
            (0x2e,self.pool.get()),(0x37,0xad0000),(0x3f,0x1600000),(0x47,self.scratch.get())] {
            pointer(bytes,fragment::COMMAND_TAIL+offset,value);
        }
        bytes[fragment::COMMAND_TAIL+0x36]=1;bytes[fragment::COMMAND_TAIL+0x4f]=1;
        Ok(())
    }
}
