// SPDX-License-Identifier: GPL-2.0-only
use crate::g16_render::{BuildError, Geometry, Register, Stage};

#[derive(Clone, Copy, Default)]
pub(crate) struct Program { pub address: u64, pub resources: u64 }
#[derive(Clone, Copy, Default)]
pub(crate) struct DepthStencil { pub base: u64, pub stride: u64 }

pub(crate) struct State {
    pub geometry: Geometry,
    pub vdm: u64, pub tilemap: u64, pub tpc: u64, pub unknown: u64,
    pub preemption: [u64; 3], pub scratch: u64, pub auxiliary: u64, pub scene_list: u64,
    pub multisample: u64, pub ppp: u32,
    pub merge_upper: [u32; 2], pub tilebuffer_blocks: u32,
    pub background: Program, pub eot: Program,
    pub partial_background: Program, pub partial_eot: Program,
    pub depth_bias: u64, pub scissor: u64, pub query: u64,
    pub depth: DepthStencil, pub stencil: DepthStencil,
    pub depth_dimensions: u64, pub zls_control: u64,
    pub depth_clear: u32, pub stencil_clear: u32,
    pub process_empty_tiles: bool,
    pub integer_depth_bias: bool,
}

impl State {
    fn layer_metadata(&self) -> Result<u64, BuildError> {
        self.unknown.checked_add(if self.geometry.layers > 1 { 0x200 } else { 0 })
            .ok_or(BuildError::ArithmeticOverflow)
    }

    fn layer_mode(&self) -> u64 {
        let layers = self.geometry.layers;
        if layers > 1 {
            0xc000 | if self.process_empty_tiles { 0x2000 } else { 0x1000 }
                | u64::from(layers - 1)
        } else { 0x8000 }
    }

    fn isp_control(&self) -> u64 {
        // Common Asahi UAPI flag deliberately matches the hardware bit.
        0xc000 | if self.integer_depth_bias { 1 << 18 } else { 0 }
    }

    fn tilebuffer_control(&self) -> u64 {
        u64::from(self.tilebuffer_blocks) | ((self.geometry.utile_config & 3) << 17)
    }

    fn framebuffer_dimensions(&self) -> u64 {
        let pixels = self.geometry.pixels;
        ((u64::from(pixels >> 16) + 1) << 32) | (u64::from(pixels & 0xffff) + 1)
    }
}

/// TA range-1 encoding. This is an engine address transform, not a mapping:
/// the normal GPU address must already be mapped in the client's GPUVM.
pub(crate) fn compact(address: u64) -> Result<u64, BuildError> {
    if !(0x10_0000_0000..0x90_0000_0000).contains(&address) {
        return Err(BuildError::Address);
    }
    Ok(address - 0x10_0000_0000)
}

/// The caller supplies storage sized for the hardware register aperture.
/// Unsupported sample counts and helper state are rejected by the UAPI adapter.
pub(crate) fn registers(out: &mut [Register], stage: Stage, s: &State, uuid: u32)
    -> Result<(usize, u64), BuildError> {
    let g = s.geometry;
    let mut n = 0;
    macro_rules! r { ($o:expr, $f:expr, $v:expr) => {{
        let dst = out.get_mut(n).ok_or(BuildError::BufferTooSmall)?;
        *dst = Register { offset: $o, flagged: $f != 0, value: $v as u64 };
        n += 1;
    }}; }
    if stage == Stage::Tiling {
        let tilemap = compact(s.tilemap)?;
        let unknown = compact(s.unknown)?;
        let layer_metadata = compact(s.layer_metadata()?)?;
        let preempt0 = compact(s.preemption[0])?;
        let preempt1 = compact(s.preemption[1])?;
        r!(0x1748,0,1); r!(0x10140,1,0x200);
        r!(0x1c038,1,tilemap); r!(0x1c9c8,0,tilemap);
        r!(0x1c0a0,1,compact(s.tpc)?);
        r!(0x1c030,1,unknown | 1u64<<63); r!(0x1c9c0,0,unknown | 1u64<<63);
        r!(0x1c050,1,0x003a0012006b0003u64); r!(0x1c060,1,1);
        r!(0x10148,1,g.utile_config); r!(0x10138,1,s.multisample);
        r!(0x10110,1,preempt0); r!(0x1c9b0,0,preempt0);
        r!(0x10118,1,preempt1); r!(0x1c9b8,0,preempt1);
        r!(0x1c958,0,1); r!(0x1c950,0,compact(s.preemption[2])? | (1u64 << 50)); r!(0x1c930,0,0);
        r!(0x1c880,0,compact(s.vdm)? & !3);
        r!(0x1c078,1,layer_metadata); r!(0x1c9d8,0,layer_metadata);
        r!(0x10150,1,0);
        for i in 0..5 { r!(0x1c198+i*8,1,0); }
        r!(0x1c8f8,0,0x8860); r!(0x1c0b0,1,g.region_stride); r!(0x1c850,0,g.region_stride);
        r!(0x10130,1,0x88); r!(0x10120,1,2 | (s.ppp & 0x201));
        r!(0x10128,1,g.pixels); r!(0x101b8,1,g.screen);
        r!(0x1c068,1,g.x_blocks); r!(0x1c070,1,g.y_blocks); r!(0x1c080,1,g.blocks);
        r!(0x1c0a8,1,g.tpc_stride); r!(0x10170,1,0x100); r!(0x10168,1,s.layer_mode());
        r!(0xa308,1,0); r!(0x1c8e0,0,u64::MAX); r!(0x1c8e8,0,u64::MAX);
        r!(0x1c898,0,0); r!(0x101e0,1,0x1c); r!(0x1c9e8,0,0);
        for o in [0x1a098,0x1a0a0,0x1a068,0x1a070,0x1a0c8,0x1a0d0,0x101c8,0xd470] { r!(o,1,0); }
        r!(0x1a0f0,1,8);
        r!(0x10798,1,0xff0000); r!(0x1c830,0,0);
        r!(0x1ca30,0,compact(s.scene_list)?); r!(0x16c38,1,compact(s.scene_list)?);
        let v = s.scratch;
        r!(0x1c910,0,((v>>3)&0x8000000000) | (((if v & 0x40000000000 != 0 {0} else {0x7000000000}) + v) & 0x7ffffffffe) | 1);
        r!(0xa5a0,1,0x6000400020u64); r!(0xd418,1,0x200000001u64);
        r!(0x1ca10,0,uuid); r!(0x14a0,1,uuid); r!(0xa348,1,uuid);
        Ok((n,0))
    } else {
        r!(0x1738,1,1); r!(0x10008,1,g.utile_config);
        r!(0x15378,1,s.eot.resources); r!(0x15380,1,s.eot.address);
        r!(0x15368,1,s.background.resources); r!(0x15370,1,s.background.address);
        r!(0x15130,1,s.merge_upper[0]); r!(0x15138,1,s.merge_upper[1]);
        r!(0x100a0,1,0); r!(0x15068,1,0); r!(0x15070,1,0); r!(0x16058,0,0);
        r!(0x10018,1,s.multisample); r!(0x100b0,1,g.isp_blocks); r!(0x16030,0,g.isp_blocks);
        r!(0x100d8,1,g.screen); r!(0xa300,1,0); r!(0x10790,1,0xff0200);
        r!(0x16098,0,s.unknown & !63); r!(0x15108,1,s.scissor & !3); r!(0x15100,1,s.depth_bias & !3);
        // Keep integer depth-bias selection in both the register and mirror.
        r!(0x15020,1,s.isp_control());
        r!(0x15210,1,s.framebuffer_dimensions());
        r!(0x15048,1,0x100000);
        r!(0x10050,1,s.tilebuffer_control());
        r!(0x15320,1,s.depth_dimensions); r!(0x15300,1,s.depth_clear); r!(0x15308,1,s.stencil_clear);
        r!(0x15310,1,s.query & !15); r!(0x15318,1,s.zls_control); r!(0x15348,1,0x04040404); r!(0x15350,1,0);
        r!(0x15328,1,s.depth.base); r!(0x15330,1,s.depth.base);
        r!(0x15338,1,s.stencil.base); r!(0x15340,1,s.stencil.base);
        for o in [0x15230,0x15220,0x15238,0x15228] { r!(o,1,0); }
        r!(0x15400,1,s.depth.stride); r!(0x15420,1,s.depth.stride);
        r!(0x15408,1,s.stencil.stride); r!(0x15428,1,s.stencil.stride);
        for o in [0x153c0,0x15410,0x153c8,0x15430,0x153d0,0x15418,0x153d8,0x15438] { r!(o,1,0); }
        r!(0x16428,1,s.tilemap); r!(0x16060,0,s.layer_metadata()?); r!(0x16430,1,(g.region_stride as u64)<<26);
        let mode = 0x280 | u64::from(g.layers > 1) | (u64::from(s.process_empty_tiles)<<16);
        r!(0x10038,1,mode); r!(0x16020,0,0); r!(0x16450,1,0); r!(0x15358,1,0);
        r!(0x100b8,0,0x8860); r!(0x16460,1,s.auxiliary); r!(0x16090,0,s.auxiliary);
        r!(0x101e8,1,0x1c); r!(0x160a8,0,0);
        r!(0x16068,0,0x3717fu64 | (u64::from(g.layers > 1)<<32) | ((g.utile_config>>12)<<40) | (0x200u64<<28) |
            ((g.screen as u64 & 0x1ff)<<44) | (((g.screen as u64>>12)&0x1ff)<<53));
        for o in [0x1a0a8,0x1a0b0,0x1a078,0x1a080,0x1a0d8,0x1a0e0,0x101c0,0xd468] { r!(o,1,0); }
        r!(0x1a0f8,1,8);
        r!(0xa5a8,1,0x6000400020u64); r!(0xd428,1,0x200000001u64);
        r!(0x160e0,0,uuid); r!(0x1498,1,uuid); r!(0xa340,1,uuid);
        r!(0x1c838,0,0); r!(0x1ca28,0,compact(s.scene_list)?);
        Ok((n,mode))
    }
}

pub(crate) fn mirrors(out: &mut [u8], stage: Stage, s: &State,
    tpc_fw: u64, uma_fw: u64, uma_aux_fw: u64, mode: u64) -> Result<(), BuildError> {
    if out.len() < stage.command_size() { return Err(BuildError::BufferTooSmall); }
    macro_rules! p { ($o:expr,$v:expr,$n:expr) => {
        out[$o..$o+$n].copy_from_slice(&($v as u64).to_le_bytes()[..$n]);
    }; }
    if stage == Stage::Tiling {
        let preempt = compact(s.preemption[0])?;
        p!(0x906,1,1);
        p!(0x760,tpc_fw,8); p!(0x768,s.geometry.tpc_bytes,8);
        p!(0x7a8,preempt,8); p!(0x8fe,uma_fw,8); p!(0x90f,uma_aux_fw,8); p!(0x830,mode,8);
    } else {
        let g = s.geometry;
        p!(0xc4e,1,1);
        p!(0x40,s.tilemap,8); p!(0x50,1,4); p!(0x54,g.isp_blocks,4);
        p!(0x60,g.x_blocks,4); p!(0x64,g.y_blocks,4); p!(0x68,g.blocks,4); p!(0x78,g.tiles,4);
        for (o,v) in [
            (0x7a0,s.depth_bias),(0x7b0,s.scissor),(0x7c0,s.query),
            (0x8f8,s.background.resources),(0x900,s.background.address),
            (0x928,s.partial_background.resources),(0x930,s.partial_background.address),
            (0x938,s.zls_control),(0x940,0x04040404),
            (0x948,s.depth.base),(0x950,s.depth.stride),(0x960,s.depth.base),(0x968,s.depth.base),
            (0x978,s.stencil.base),(0x980,s.stencil.stride),(0x990,s.stencil.base),(0x998,s.stencil.base),
            (0x9b8,s.tilebuffer_control()),(0x9c0,s.isp_control()),
            (0x9c8,s.framebuffer_dimensions()),(0x9d0,0x100000),
            (0x9f4,s.eot.resources),(0x9fc,s.eot.address),
            (0xa14,s.partial_eot.resources),(0xa1c,s.partial_eot.address),
            (0xa28,s.depth_clear as u64 | ((s.stencil_clear as u64)<<32)),(0xa50,s.depth_dimensions),
            (0xc46,uma_fw),(0xc57,uma_aux_fw),(0x9d8,mode)] { p!(o,v,8); }
    }
    Ok(())
}
