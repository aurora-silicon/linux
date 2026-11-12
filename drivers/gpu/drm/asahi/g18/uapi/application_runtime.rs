// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Pure application frame policy shared with the three-frame host replay.
//! Ring storage wraps; event stamps remain unique until their native u24 limit.
use crate::{g18::fw::render_params::G17pRenderParameters, g18::fw::render::{self as g18_render, G18RenderControls},
    g18::fw::render_command::{G18RenderCommandInputs, G18StageCompletion}, g18::uapi::render_policy as g18_render_policy};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Frame { pub generation: u32 }
impl Frame {
    pub(crate) fn new(generation: u32) -> Option<Self> {
        if generation==0 || generation>0xff_ffff {None} else {Some(Self {generation})}
    }
    pub(crate) fn stamp(self) -> u32 {self.generation<<8}
    pub(crate) fn previous_stamp(self) -> u32 {(self.generation-1)<<8}
    pub(crate) fn inner_before(self) -> u32 {(3+(self.generation-1)*3)%256}
    pub(crate) fn inner_after(self) -> u32 {(3+self.generation*3)%256}
    pub(crate) fn outer_before(self) -> u32 {(6+(self.generation-1)*2)%256}
    pub(crate) fn outer_after(self) -> u32 {(6+self.generation*2)%256}
    pub(crate) fn submitted(self) -> u32 {self.generation*2}
}

/// The native G18 tail is populated from application parameters, never a
/// capture template. Optional unsupported features are explicit zero policy.
pub(crate) fn inputs(p: &mut G17pRenderParameters, c: &mut G18RenderControls,
    context: u32, frame: Frame) -> G18RenderCommandInputs {
    let mut i=g18_render_policy::owned_inputs(G18RenderCommandInputs::default(),context,p,c);
    let g=u64::from(frame.generation);
    p.lifecycle=g<<32;p.fragment_lifecycle=p.lifecycle;
    i.ta.sequence=u64::from(frame.submitted()-1);i.fragment.sequence=u64::from(frame.submitted()-2);
    i.fragment.pass=frame.generation as u8;
    for (stage,completion) in [&mut i.ta.completion,&mut i.fragment.completion].into_iter().enumerate() {
        *completion=G18StageCompletion {stamp:frame.stamp(),completion:(1<<40)|g,
            key:u64::from(frame.submitted()-stage as u32),..*completion};
    }
    i.ta.linked_3d.stamp=frame.stamp();i.ta.linked_3d.completion=i.fragment.completion.completion;
    i.ta.d08a4=frame.generation;i.fragment.d08a4=frame.generation;
    i.fragment.d04e0=p.depth_bias_array;i.fragment.d04e8=p.scissor_array;i.fragment.d04f0=p.occlusion_query_base;
    i.fragment.d0518[18]=[p.load_pipeline_bind,p.load_pipeline];
    i.fragment.d0518[21]=[p.partial_load_pipeline_bind,p.partial_load_pipeline];
    i.fragment.d0518[22]=[p.depth_flags,0x0404_0404];
    i.fragment.d0690=p.depth_dimensions;
    i.fragment.d0698=p.depth_buffer;
    i.fragment.d06a0=[p.depth_buffer,p.depth_buffer];
    i.fragment.d0728=p.depth_aux_buffer;
    // Captured gears/colour and M5 shared-runtime flag policy. These are
    // independent of the application-owned depth/PPP/sample-size fields.
    // msaa163: +0x212c (flag 2) is 1 in native 4x (M5: 1 at 2x and 4x).
    let msaa4=g18_render::g18_msaa4(p);
    i.fragment.d08c5_flags=[1,1,u8::from(msaa4),0];
    i.fragment.d0770=[c.fragment_sample_control,p.aux_fb_flags];
    i.fragment.d0780=[u64::from(p.width)|(u64::from(p.height)<<32),p.aux_fb_page_count];
    i.fragment.d0790=[p.store_pipeline_bind<<32,p.store_pipeline];
    i.fragment.d07b0=[p.partial_store_pipeline_bind<<32,p.partial_store_pipeline];
    i.fragment.d07d8=u64::from(p.depth_clear_value_bits)|(0x300<<32);
    // +0x1fb0 is bytes per sample, not storage blocks or sample control.
    // Both native2x/4x geometries contain2048 raster samples;1x contains1024.
    // All admitted strides are multiples of8, so the2-KiB block count is exact.
    let raster_samples=u64::from(p.utile_width)*u64::from(p.utile_height)
        *u64::from(g18_render::g18_sample_count(p));
    i.fragment.d04d0=(p.tib_blocks*2048/raster_samples) as u32;
    // msaa163: +0x50 samples (native 4) and +0x54 macro size (0x40008).
    i.fragment.d0900=g18_render::g18_sample_count(p);
    let tiles_x=(p.width+31)/32;let tiles_y=(p.height+31)/32;
    i.fragment.d0908=g18_render::g18_macro_size(p);
    i.fragment.d0910=[0,u64::from(p.merge_upper_x_bits)|(u64::from(p.merge_upper_y_bits)<<32)];
    i.fragment.d0930=tiles_x*tiles_y;
    i
}
