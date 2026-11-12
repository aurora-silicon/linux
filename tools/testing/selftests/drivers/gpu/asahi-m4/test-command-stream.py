#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host-check production M4 Rust stream parsing with generated kernel UAPI types."""
from common import kernel_root, output_dir, rustc, flat_bindings
from abi import c_layout, differences, rust_layout

import hashlib
import json
import pathlib
import re
import subprocess
import tempfile

ROOT = kernel_root()
OUT = output_dir()
SOURCE = ROOT / 'drivers/gpu/drm/asahi/g16/command.rs'
FLAT = flat_bindings()
# The M5 render payload and resource commands are newer than the frozen M4
# bindings. Declare them beside the flat ones and check their layout and values
# against the kernel header, as the frozen ones are.
M5_UAPI = r'''
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct drm_asahi_render_stage_prefixes {
    pub ta_cdm_barrier: __u16,
    pub reserved0: __u16,
    pub reserved1: __u32,
    pub ta_prior_cdm: __u64,
    pub ta_vdm_barrier: __u16,
    pub reserved2: __u16,
    pub reserved3: __u32,
    pub ta_prior_vdm: __u64,
}
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct drm_asahi_cmd_render_m5 {
    pub legacy: drm_asahi_cmd_render,
    pub stage_prefixes: drm_asahi_render_stage_prefixes,
}
pub const drm_asahi_cmd_type_DRM_ASAHI_SET_RENDER_RESOURCES: drm_asahi_cmd_type = 5;
pub const drm_asahi_cmd_type_DRM_ASAHI_SET_COMPUTE_RESOURCES: drm_asahi_cmd_type = 6;
pub const drm_asahi_cmd_type_DRM_ASAHI_SET_RENDER_RESOURCES_V2: drm_asahi_cmd_type = 7;
pub const drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES: drm_asahi_render_flags = 8388608;
'''
BINDINGS = OUT / 'm5-uapi.rs'
BINDINGS.write_text('include!(' + json.dumps(str(FLAT)) + ');\n' + M5_UAPI)
with tempfile.TemporaryDirectory(prefix='m4-m5-uapi-') as name:
    errors = differences(c_layout(ROOT / 'include/uapi/drm/asahi_drm.h', BINDINGS, pathlib.Path(name), 'm5-layout'),
                         rust_layout(BINDINGS, pathlib.Path(name)))
if errors:
    raise RuntimeError('Kernel UAPI ABI differs from the M5 bindings: ' + json.dumps(errors))
command = SOURCE.read_text()
hardware = (SOURCE.parent / 'hw.rs').read_text()
capacities = '\n'.join(re.findall(r'pub\(crate\) const (?:TPC_SIZE|TILEMAP_SIZE): u64 = .*?;', hardware))
names = ['drm_asahi_cmd_header', 'drm_asahi_attachment', 'drm_asahi_cmd_render',
         'drm_asahi_cmd_compute', 'drm_asahi_bg_eot', 'drm_asahi_zls_buffer',
         'drm_asahi_timestamp', 'drm_asahi_timestamps', 'drm_asahi_helper_program',
         'drm_asahi_render_ray_query_stage']
constants = command[command.index('/// Largest command buffer.'):command.index('/// A validated render command.')]
parser = command[command.index('/// One command of the stream.'):command.index('/// Checks that `[start, end)`')]
g17 = (ROOT / 'drivers/gpu/drm/asahi/g17/command.rs').read_text()
# The stage-prefix offset and the render payload reader precede the parser.
g17_parser = g17[g17.index('/// Where a render command\'s stage-prefix fields start'):g17.index('#[derive(Clone, Copy, PartialEq, Eq)]')]
g17_constants = '\n'.join(re.findall(r'const (?:RENDER_BASE_SIZE|COMPUTE_BASE_SIZE): usize = .*?;|const (?:RENDER_TIMESTAMP_COPIES|COMPUTE_TIMESTAMP_COPIES): Range<usize> = .*?;', g17, re.S))
reader = (ROOT / 'drivers/gpu/drm/asahi/util.rs').read_text()
reader = reader[reader.index('pub(crate) unsafe trait AnyBitPattern:'):]
harness = r'''
#![allow(dead_code, non_camel_case_types, non_upper_case_globals)]
use core::{mem::{offset_of,size_of}, ops::Range};
type Result<T = ()> = core::result::Result<T, i32>;
const EINVAL: i32 = -22;
const EOVERFLOW: i32 = -75;
const E2BIG: i32 = -7;
struct Error; impl Error { fn from_errno(error:i32)->i32 { error } }
extern crate self as kernel;
// The parser logs why it refuses a command; check the format arguments, print nothing.
macro_rules! pr_warn { ($($arg:tt)*) => {{ let _ = format_args!($($arg)*); }}; }
pub mod bindings { pub const EOPNOTSUPP:i32=95; }
pub mod prelude { pub type Result<T=()> = core::result::Result<T,i32>; pub const E2BIG:i32=-7; }
mod uapi { include!("FULL_UAPI"); }
mod file { pub const MAX_COMMANDS_PER_SUBMISSION: u32 = 64; }
mod hw {
    pub const USER_RANGE: core::ops::Range<u64> = 0x4000..((1u64 << 42) - 0x8000);
    pub mod sizes { pub const MAX_ATTACHMENTS: usize = 16; }
    pub mod client { pub const PB_HEAP_SIZE:u64=0x10000000; CAPACITIES }
}
mod fw {
    #[path="FW_GRAPH"] pub mod graph;
    #[path="FW_WORK"] pub mod work;
}
#[path="FULL_ENCODER"] mod encode;
READER
IMPLS
PRODUCTION_CONSTANTS
PRODUCTION_PARSER
mod g17 {
    use super::{uapi, AnyBitPattern, Reader, Result, EINVAL, size_of, offset_of, Range};
    G17_CONSTANTS
    const STAGE_PREFIXES: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES;
    const FRAGMENT_BARRIERS: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS;
    const GFP_KERNEL: u32 = 0;
    pub struct Arc<T>(std::rc::Rc<T>);
    impl<T> Arc<T> { fn new(value: T, _: u32) -> Result<Self> { Ok(Self(std::rc::Rc::new(value))) } }
    impl<T> Clone for Arc<T> { fn clone(&self) -> Self { Self(self.0.clone()) } }
    impl<T> core::ops::Deref for Arc<T> { type Target = T; fn deref(&self) -> &T { &self.0 } }
    // The M5 program lists, likewise retained as their bytes.
    pub struct Programs(pub Vec<u8>, pub u8);
    impl Programs {
        fn parse(bytes: &[u8]) -> Result<Self> { Ok(Self(bytes.to_vec(), 1)) }
        fn parse_v2(bytes: &[u8]) -> Result<Self> { Ok(Self(bytes.to_vec(), 2)) }
    }
    pub struct ComputePrograms(pub Vec<u8>);
    impl ComputePrograms { fn parse(bytes: &[u8]) -> Result<Self> { Ok(Self(bytes.to_vec())) } }
    // These stand-ins only retain extension bytes. This test checks the actual
    // parser's framing and offsets; backend resource validation has other tests.
    #[derive(Clone,Copy)]
    pub struct Attachments;
    impl Attachments {
        const EMPTY:Self=Self;
        fn parse(_: &[u8])->Result<Self> { Ok(Self) }
    }
    #[derive(Clone,Copy)]
    pub struct RayQueryStage(pub [u8;56]);
    impl RayQueryStage {
        fn parse_pair(bytes:&[u8])->Result<[Self;2]> {
            if bytes.get(112..).is_some_and(|tail|tail.iter().any(|&v|v!=0)) { return Err(EINVAL); }
            let mut wire=[0u8;112]; let count=bytes.len().min(112);
            wire[..count].copy_from_slice(&bytes[..count]);
            Ok([Self(wire[..56].try_into().unwrap()),Self(wire[56..].try_into().unwrap())])
        }
    }
    pub struct IntersectionTables(pub [u8;48]);
    impl IntersectionTables {
        fn parse(bytes:&[u8])->Result<Self> {
            if bytes.get(48..).is_some_and(|tail|tail.iter().any(|&v|v!=0)) { return Err(EINVAL); }
            let mut wire=[0u8;48]; let count=bytes.len().min(48);
            wire[..count].copy_from_slice(&bytes[..count]); Ok(Self(wire))
        }
    }
    G17_PARSER
}
const SET_RENDER_RESOURCES: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_RENDER_RESOURCES;
const SET_COMPUTE_RESOURCES: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_COMPUTE_RESOURCES;
const SET_RENDER_RESOURCES_V2: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_RENDER_RESOURCES_V2;
fn data<T: Copy>(value: &T) -> Vec<u8> {
    unsafe { core::slice::from_raw_parts(value as *const T as *const u8, size_of::<T>()).to_vec() }
}
fn entry(kind: u32, payload: &[u8], barriers: [u16;2]) -> Vec<u8> {
    let header = uapi::drm_asahi_cmd_header {
        cmd_type: kind as u16, size: payload.len() as u16,
        vdm_barrier: barriers[0], cdm_barrier: barriers[1]
    };
    let mut out = data(&header); out.extend(payload); out
}
fn compute() -> uapi::drm_asahi_cmd_compute {
    uapi::drm_asahi_cmd_compute { cdm_ctrl_stream_base: 0x4000,
        cdm_ctrl_stream_end: 0x4040, ..Default::default() }
}
fn render() -> uapi::drm_asahi_cmd_render {
    uapi::drm_asahi_cmd_render { width_px: 32, height_px: 32, layers:1, samples:1,
        utile_width_px:32, utile_height_px:32, sample_size_B:8, ..Default::default() }
}
#[test]
fn mixed_batch_and_baseline_barriers() {
    let mut stream = entry(CMD_COMPUTE, &data(&compute())[..64], [NONE;2]);
    stream.extend(entry(CMD_RENDER, &data(&render())[..256], [0,1]));
    assert!(check_stream(&stream,false).is_ok());
    let len=stream.len(); stream[len-256-2..len-256].copy_from_slice(&2u16.to_le_bytes());
    assert!(check_stream(&stream,false).is_err());
}
#[test]
fn stream_size_alignment_and_command_counts() {
    assert!(check_stream(&[],false).is_err());
    assert!(check_stream(&[0;7],false).is_err());
    assert!(check_stream(&entry(99,&[],[NONE;2]),false).is_err());
    let one = entry(CMD_COMPUTE,&data(&compute())[..64],[NONE;2]);
    let mut stream = one.repeat(64); assert!(check_stream(&stream,false).is_ok());
    stream.extend(&one); assert!(check_stream(&stream,false).is_err());
    let mut short=one.clone(); short.pop(); assert!(check_stream(&short,false).is_err());
}
#[test]
fn render_high_resource_words_are_preserved() {
    let mut r=render(); r.flags=RSRC_SPEC_HI;
    r.bg_eot_rsrc_spec_hi=1; r.bg_eot_partial_rsrc_spec_hi=2;
    r.bg_rsrc_spec_hi=3; r.bg_partial_rsrc_spec_hi=4;
    assert!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false).is_ok());
    assert!(check_stream(&entry(CMD_RENDER,&data(&r)[..248],[NONE;2]),false).is_err());
    r.flags=0;
    assert!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false).is_err());
}
#[test]
fn ray_query_and_unknown_extension_bytes_are_refused() {
    let mut r=render(); r.ray_query[0].scs_layout=1;
    assert!(check_stream(&entry(CMD_RENDER,&data(&r),[NONE;2]),false).is_err());
    let mut raw=data(&compute()); raw.extend([0u8;8]);
    assert!(check_stream(&entry(CMD_COMPUTE,&raw,[NONE;2]),false).is_ok());
    *raw.last_mut().unwrap()=1;
    assert!(check_stream(&entry(CMD_COMPUTE,&raw,[NONE;2]),false).is_err());
}
#[test]
fn canonical_prior_offsets_and_reserved_timestamp_copies() {
    assert_eq!(size_of::<uapi::drm_asahi_cmd_render>(),448);
    assert_eq!(size_of::<uapi::drm_asahi_cmd_compute>(),200);
    assert_eq!(offset_of!(uapi::drm_asahi_cmd_render,prior_vdm),320);
    assert_eq!(offset_of!(uapi::drm_asahi_cmd_compute,scs_layout),128);
    assert_eq!(offset_of!(uapi::drm_asahi_cmd_compute,prior_vdm),136);
    let r=uapi::drm_asahi_cmd_render {prior_vdm:17,prior_cdm:29,..render()};
    let wire=data(&r);
    let parsed:uapi::drm_asahi_cmd_render=payload(&wire).unwrap();
    assert_eq!([parsed.prior_vdm,parsed.prior_cdm],[17,29]);
    assert!(check_stream(&entry(CMD_RENDER,&wire,[NONE;2]),false).is_ok());
    let c=uapi::drm_asahi_cmd_compute {prior_vdm:17,prior_cdm:29,..compute()};
    let wire=data(&c);
    let parsed:uapi::drm_asahi_cmd_compute=payload(&wire).unwrap();
    assert_eq!([parsed.prior_vdm,parsed.prior_cdm],[17,29]);
    assert!(check_stream(&entry(CMD_COMPUTE,&wire,[NONE;2]),false).is_ok());
    for index in 0..8 {
        for offset_only in [false,true] {
            let timestamp=uapi::drm_asahi_timestamp {handle:u32::from(!offset_only),offset:u32::from(offset_only)};
            let mut r=render(); r.ts_frag_end_copies[index]=timestamp;
            assert!(check_stream(&entry(CMD_RENDER,&data(&r),[NONE;2]),false).is_err());
            let mut c=compute(); c.ts_end_copies[index]=timestamp;
            assert!(check_stream(&entry(CMD_COMPUTE,&data(&c),[NONE;2]),false).is_err());
        }
    }
}
#[test]
fn g17_extensions_keep_canonical_offsets() {
    let mut r=render(); r.prior_vdm=17; r.prior_cdm=29;
    r.ray_query[0].scs_layout=0x1234;
    let wire=entry(CMD_RENDER,&data(&r),[NONE;2]);
    let parsed=g17::Parser::new(&wire).next().unwrap().unwrap();
    let g17::Payload::Render(cmd,prefixes,_,ray,resources)=parsed.payload else { panic!("wrong engine") };
    assert!(prefixes.is_none()&&resources.is_none());
    assert_eq!([cmd.prior_vdm,cmd.prior_cdm],[17,29]);
    assert_eq!(u64::from_le_bytes(ray[0].0[..8].try_into().unwrap()),0x1234);
    let c=uapi::drm_asahi_cmd_compute {scs_layout:0x1234,
        prior_vdm:17,prior_cdm:29,cdm_auxiliary_allocation:8,intersection_pool:0x4000,..Default::default()};
    let wire=entry(CMD_COMPUTE,&data(&c),[NONE;2]);
    let parsed=g17::Parser::new(&wire).next().unwrap().unwrap();
    let g17::Payload::Compute(cmd,_,intersection,resources)=parsed.payload else { panic!("wrong engine") };
    assert!(resources.is_none());
    assert_eq!([cmd.prior_vdm,cmd.prior_cdm],[17,29]); assert_eq!(cmd.scs_layout,0x1234);
    assert_eq!(u32::from_le_bytes(intersection.0[..4].try_into().unwrap()),8);
    assert_eq!(u64::from_le_bytes(intersection.0[8..16].try_into().unwrap()),0x4000);
    for index in 0..8 {
        for offset_only in [false,true] {
            let ts=uapi::drm_asahi_timestamp {handle:u32::from(!offset_only),offset:u32::from(offset_only)};
            let mut r=render(); r.ts_frag_end_copies[index]=ts;
            let wire=entry(CMD_RENDER,&data(&r),[NONE;2]);
            assert!(g17::Parser::new(&wire).next().is_err());
            let mut c=compute(); c.ts_end_copies[index]=ts;
            let wire=entry(CMD_COMPUTE,&data(&c),[NONE;2]);
            assert!(g17::Parser::new(&wire).next().is_err());
        }
    }
}
#[test]
fn g17_stage_prefixes_follow_the_complete_render() {
    let mut m5=uapi::drm_asahi_cmd_render_m5 {legacy:render(),..Default::default()};
    m5.legacy.flags=uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES; m5.legacy.prior_vdm=17;
    m5.stage_prefixes.ta_cdm_barrier=3; m5.stage_prefixes.ta_prior_vdm=0x1234;
    let wire=entry(CMD_RENDER,&data(&m5),[NONE;2]);
    let parsed=g17::Parser::new(&wire).next().unwrap().unwrap();
    let g17::Payload::Render(cmd,prefixes,..)=parsed.payload else { panic!("wrong engine") };
    let prefixes=prefixes.expect("stage prefixes");
    assert_eq!(cmd.prior_vdm,17); assert_eq!((prefixes.ta_cdm_barrier,prefixes.ta_prior_vdm),(3,0x1234));
    // The flag needs the complete struct, its reserved fields zero, and no fragment barriers.
    assert!(g17::Parser::new(&entry(CMD_RENDER,&data(&m5)[..448],[NONE;2])).next().is_err());
    let mut bad=m5; bad.stage_prefixes.reserved1=1;
    assert!(g17::Parser::new(&entry(CMD_RENDER,&data(&bad),[NONE;2])).next().is_err());
    let mut bad=m5; bad.legacy.flags|=FRAGMENT_BARRIERS;
    assert!(g17::Parser::new(&entry(CMD_RENDER,&data(&bad),[NONE;2])).next().is_err());
    // Without the flag the prefix fields are absent or zero.
    let mut plain=m5; plain.legacy.flags=0;
    assert!(g17::Parser::new(&entry(CMD_RENDER,&data(&plain),[NONE;2])).next().is_err());
    plain.stage_prefixes=Default::default();
    let parsed=g17::Parser::new(&entry(CMD_RENDER,&data(&plain),[NONE;2])).next().unwrap().unwrap();
    let g17::Payload::Render(_,prefixes,..)=parsed.payload else { panic!("wrong engine") };
    assert!(prefixes.is_none());
}
#[test]
fn g17_resource_lists_apply_to_later_commands_until_cleared() {
    let mut stream=entry(SET_RENDER_RESOURCES,&[1,2,3,4],[NONE;2]);
    stream.extend(entry(SET_COMPUTE_RESOURCES,&[5,6],[NONE;2]));
    stream.extend(entry(CMD_RENDER,&data(&render()),[NONE;2]));
    stream.extend(entry(CMD_COMPUTE,&data(&compute()),[NONE;2]));
    stream.extend(entry(SET_RENDER_RESOURCES_V2,&[7],[NONE;2]));
    stream.extend(entry(CMD_RENDER,&data(&render()),[NONE;2]));
    stream.extend(entry(SET_RENDER_RESOURCES,&[],[NONE;2]));
    stream.extend(entry(CMD_RENDER,&data(&render()),[NONE;2]));
    let mut parser=g17::Parser::new(&stream);
    let mut renders=Vec::new();
    while let Some(command)=parser.next().unwrap() {
        match command.payload {
            g17::Payload::Render(..,resources)=>renders.push(resources.map(|r|(r.0.clone(),r.1))),
            g17::Payload::Compute(..,resources)=>assert_eq!(resources.map(|r|r.0.clone()),Some(vec![5,6])),
        }
    }
    assert!(parser.finish().is_ok());
    assert_eq!(renders,[Some((vec![1,2,3,4],1)),Some((vec![7],2)),None]);
    // Like the attachment commands, they carry no barriers.
    for kind in [SET_RENDER_RESOURCES,SET_COMPUTE_RESOURCES,SET_RENDER_RESOURCES_V2] {
        assert!(g17::Parser::new(&entry(kind,&[1],[0,NONE])).next().is_err());
    }
}
#[test]
fn unsupported_flags_and_compute_fields() {
    for flag in [1u32,4,1<<19,FRAGMENT_BARRIERS] {
        let mut r=render(); r.flags=flag;
        assert!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false).is_err());
    }
    let mut c=compute();
    c.scs_layout=1;
    assert!(check_stream(&entry(CMD_COMPUTE,&data(&c),[NONE;2]),false).is_err());
    c.scs_layout=0; c.cdm_auxiliary_allocation=8;
    assert!(check_stream(&entry(CMD_COMPUTE,&data(&c),[NONE;2]),false).is_err());
    let mut c=compute(); c.cdm_ctrl_stream_end=c.cdm_ctrl_stream_base;
    assert!(check_stream(&entry(CMD_COMPUTE,&data(&c),[NONE;2]),false).is_err());
}
#[test]
fn scratch_and_helper_requests_report_unsupported() {
    let mut r=render(); r.flags=VERTEX_SCRATCH;
    assert_eq!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false),Err(-95));
    for fragment in [false,true] {
        for field in 0..3 {
            let mut r=render();
            let helper=if fragment { &mut r.fragment_helper } else { &mut r.vertex_helper };
            match field { 0=>helper.binary=1,1=>helper.cfg=1,_=>helper.data=1 }
            assert_eq!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false),Err(-95));
        }
    }
}
#[test]
fn attachments_are_bounded_and_do_not_count_as_work() {
    let a=uapi::drm_asahi_attachment { pointer:0x4000,size:4,..Default::default() };
    let mut stream=entry(SET_VERTEX_ATTACHMENTS,&data(&a),[NONE;2]);
    assert!(check_stream(&stream,false).is_err());
    stream.extend(entry(CMD_RENDER,&data(&render())[..256],[NONE;2]));
    assert!(check_stream(&stream,false).is_ok());
    let mut bad=a; bad.flags=1;
    let mut stream=entry(SET_COMPUTE_ATTACHMENTS,&data(&bad),[NONE;2]);
    stream.extend(entry(CMD_COMPUTE,&data(&compute())[..64],[NONE;2]));
    assert!(check_stream(&stream,false).is_err());
    let mut stream=entry(SET_FRAGMENT_ATTACHMENTS,&data(&a).repeat(17),[NONE;2]);
    stream.extend(entry(CMD_RENDER,&data(&render())[..256],[NONE;2]));
    assert!(check_stream(&stream,false).is_err());
}
#[test]
fn historical_priors_enforce_entry_bounds() {
    assert!(Order::new([10,10],[0,0],[NONE,NONE],[0,0],RENDER).is_ok());
    assert!(Order::new([10,10],[0,0],[NONE,NONE],[1,0],RENDER).is_ok());
    assert!(Order::new([10,10],[0,0],[NONE,NONE],[0,11],RENDER).is_err());
    assert!(Order::new([0,0],[0,0],[1,0],[0,0],RENDER).is_err());
    assert!(Order::new([u64::MAX,0],[0,0],[NONE,NONE],[0,0],COMPUTE).is_err());
    let order=Order::new([4,8],[2,1],[1,NONE],[3,7],RENDER).unwrap();
    assert_eq!(order.sequence,7);
    assert_eq!(order.wait_through,[Some(5),Some(7)]);
    assert!(!order.ready([6,6]));
    assert!(order.ready([6,7]));
}
#[test]
fn geometry_capacity_boundary_uses_the_real_encoder() {
    let mut r=render(); r.width_px=16384; r.height_px=16384;
    r.utile_width_px=16; r.utile_height_px=16;
    let g=encode::Geometry::new(&r,0);
    assert_eq!((g.tpc_bytes,g.tilemap_bytes),(8<<20,5<<20));
    assert!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false).is_ok());
    r.layers=2;
    let g=encode::Geometry::new(&r,0);
    assert_eq!((g.tpc_bytes,g.tilemap_bytes),(16<<20,10<<20));
    assert_eq!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false),Err(E2BIG));
    let mut g=encode::Geometry::default();
    g.tpc_bytes=hw::client::TPC_SIZE; g.tilemap_bytes=hw::client::TILEMAP_SIZE;
    assert!(g.validate_capacity(hw::client::TPC_SIZE,hw::client::TILEMAP_SIZE).is_ok());
    g.tpc_bytes+=1;
    assert_eq!(g.validate_capacity(hw::client::TPC_SIZE,hw::client::TILEMAP_SIZE),Err(E2BIG));
    g.tpc_bytes-=1; g.tilemap_bytes+=1;
    assert_eq!(g.validate_capacity(hw::client::TPC_SIZE,hw::client::TILEMAP_SIZE),Err(E2BIG));
}
#[test]
fn mixed_batch_rejects_before_any_command_can_publish() {
    let mut r=render(); r.width_px=16384; r.height_px=16384;
    r.utile_width_px=16; r.utile_height_px=16; r.layers=2;
    let mut mixed=entry(CMD_COMPUTE,&data(&compute())[..64],[NONE;2]);
    mixed.extend(entry(CMD_RENDER,&data(&r)[..256],[NONE;2]));
    let mut publications=0;
    let result=(|| {
        check_stream(&mixed,false)?;
        for command in stream(&mixed) { command?; publications+=1; }
        Ok(())
    })();
    assert_eq!(result,Err(E2BIG));
    assert_eq!(publications,0,"a rejected render must prevent the earlier compute publishing");
}
'''
replacements = {'FULL_UAPI': str(BINDINGS), 'READER': reader,
                'CAPACITIES': capacities, 'FW_GRAPH': str(SOURCE.parent/'fw/graph.rs'),
                'FW_WORK': str(SOURCE.parent/'fw/work.rs'), 'FULL_ENCODER': str(SOURCE.parent/'encode.rs'),
                'IMPLS': '\n'.join('unsafe impl AnyBitPattern for uapi::' + name + ' {}' for name in names),
                'PRODUCTION_CONSTANTS': constants, 'PRODUCTION_PARSER': parser.replace('super::encode::', 'encode::'),
                'G17_CONSTANTS': g17_constants, 'G17_PARSER': g17_parser}
for key in sorted(replacements,key=len,reverse=True):
    harness = harness.replace(key,replacements[key])
with tempfile.TemporaryDirectory(prefix='m4-rust-command-') as directory:
    directory=pathlib.Path(directory)
    test=directory/'test.rs'; test.write_text(harness)
    binary=directory/'test'
    subprocess.run(rustc() + ['--edition=2021','--test',str(test),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary),'--nocapture'],text=True,capture_output=True)
    print(result.stdout+result.stderr, end='')
    result.check_returncode()
    print(result.stdout,end='')
    (OUT/'command-stream-host.log').write_text(result.stdout+result.stderr)
    (OUT/'command-stream-host.json').write_text(json.dumps({
        'source':str(SOURCE),'sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'g17_source':str(ROOT/'drivers/gpu/drm/asahi/g17/command.rs'),
        'g17_sha256':hashlib.sha256(g17.encode()).hexdigest(),
        'uapi_generated':str(BINDINGS),'uapi_sha256':hashlib.sha256(BINDINGS.read_bytes()).hexdigest(),
        'result':'pass','test_groups':16,
        'limitations':'Links complete production encoder/firmware helper modules; compiles isolated production G16 structural parser/reader and G17 framing/prefix parser with generated UAPI structs. G17 extension validators are stand-ins; does not exercise kernel VM/scheduler or hardware.'
    },indent=2)+'\n')
