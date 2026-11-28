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
pub const drm_asahi_compute_flags_DRM_ASAHI_COMPUTE_NO_QUERY_ACCESS: drm_asahi_compute_flags = 131072;
'''
BINDINGS = OUT / 'm5-uapi.rs'
BINDINGS.write_text('include!(' + json.dumps(str(FLAT)) + ');\n' + M5_UAPI)
with tempfile.TemporaryDirectory(prefix='m4-m5-uapi-') as name:
    errors = differences(c_layout(ROOT / 'include/uapi/drm/asahi_drm.h', BINDINGS, pathlib.Path(name), 'm5-layout'),
                         rust_layout(BINDINGS, pathlib.Path(name)))
if errors:
    raise RuntimeError('Kernel UAPI ABI differs from the M5 bindings: ' + json.dumps(errors))
command = SOURCE.read_text()
queue_src = (SOURCE.parent / 'queue.rs').read_text()
poll_src = (SOURCE.parent / 'poll.rs').read_text()
assert 'command::parse_stream(bytes, params::early_tiling())?' in queue_src
assert 'parsed.prepare(self.accepted, objects)?' in queue_src
assert 'command::check_stream(' not in queue_src
parse_idx = command.index('pub(crate) fn parse_stream(')
prep_idx = command.index('impl ParsedStream {')
assert 'stream(bytes)' in command[parse_idx:prep_idx]
assert 'stream(bytes)' not in command[prep_idx:]
assert 'resnap_has_handle' not in poll_src
assert 'let mut handle = self.resnap_handle.lock();' in poll_src
assert 'if self.sleeping.load(Ordering::Acquire)' in poll_src
assert 'fn check_stream(' not in command
assert 'fn cover(' not in command
assert 'enum Resolved' not in command
hardware = (SOURCE.parent / 'hw.rs').read_text()
capacities = '\n'.join(re.findall(r'pub\(crate\) const (?:TPC_SIZE|TILEMAP_SIZE|SCS_POOL_WINDOW): u64 = .*?;', hardware))
names = ['drm_asahi_cmd_header', 'drm_asahi_attachment', 'drm_asahi_cmd_render',
         'drm_asahi_cmd_compute', 'drm_asahi_bg_eot', 'drm_asahi_zls_buffer',
         'drm_asahi_timestamp', 'drm_asahi_timestamps', 'drm_asahi_helper_program',
         'drm_asahi_render_ray_query_stage']
constants = command[command.index('/// Largest command buffer.'):command.index('/// A validated render command.')]
parse_types = command[command.index('/// A validated render command.'):command.index('/// Inline storage for the common 1-2 command ordinals')]
inline_containers = command[command.index('/// Inline storage for the common 1-2 command submission'):command.index('/// A validated SUBMIT.')]
parser = (
    command[command.index('/// One command of the stream.'):command.index('/// Checks that `[start, end)`')]
    + command[command.index('/// Parses the attachment list'):command.index('/// Checks that `[start, end)` (rounded out to pages) is user memory mapped')]
    + command[command.index('/// Per-command barrier and stage-prefix metadata'):command.index('impl ParsedStream {')]
)
render_admission = (
    command[command.index('/// Checks that `[start, end)`'):command.index('/// Parses the attachment list')]
    + command[command.index('/// Whether a GVA can be named by a 32-bit compact pointer.'):command.index('/// Per-command compute admission.')]
)
g17 = (ROOT / 'drivers/gpu/drm/asahi/g17/command.rs').read_text()
# The stage-prefix offset and the render payload reader precede the parser.
g17_parser = g17[g17.index('/// Where a render command\'s stage-prefix fields start'):g17.index('#[derive(Clone, Copy, PartialEq, Eq)]')]
g17_constants = '\n'.join(re.findall(r'const (?:RENDER_BASE_SIZE|COMPUTE_BASE_SIZE|ATTACHMENT_KINDS): usize = .*?;|const (?:RENDER_TIMESTAMP_COPIES|COMPUTE_TIMESTAMP_COPIES): Range<usize> = .*?;', g17, re.S))
reader = (ROOT / 'drivers/gpu/drm/asahi/util.rs').read_text()
reader = reader[reader.index('pub(crate) unsafe trait AnyBitPattern:'):]
harness = r'''
#![allow(dead_code, non_camel_case_types, non_upper_case_globals)]
use core::{mem::{offset_of,size_of}, ops::Range};
type Result<T = ()> = core::result::Result<T, i32>;
const EINVAL: i32 = -22;
const EOVERFLOW: i32 = -75;
const E2BIG: i32 = -7;
const GFP_KERNEL: u32 = 0;
struct Error; impl Error { fn from_errno(error:i32)->i32 { error } }
extern crate self as kernel;
// The parser logs why it refuses a command; check the format arguments, print nothing.
macro_rules! pr_warn { ($($arg:tt)*) => {{ let _ = format_args!($($arg)*); }}; }
pub mod bindings { pub const EOPNOTSUPP:i32=95; }
pub mod prelude { pub type Result<T=()> = core::result::Result<T,i32>; pub const EINVAL:i32=-22; pub const E2BIG:i32=-7; pub const EOVERFLOW:i32=-75; }
pub(crate) mod timestamp {
    pub(crate) struct Destinations;
    impl Destinations { pub(crate) const fn empty() -> Self { Self } }
}
pub(crate) struct KBox<T>(Box<T>);
impl<T> KBox<T> { fn new(v: T, _: u32) -> Result<Self> { Ok(Self(Box::new(v))) } }
impl<T> core::ops::Deref for KBox<T> { type Target = T; fn deref(&self) -> &T { &self.0 } }
pub(crate) struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn new() -> Self { Self(Vec::new()) }
    fn with_capacity(cap: usize, _: u32) -> Result<Self> { Ok(Self(Vec::with_capacity(cap))) }
    fn push(&mut self, v: T, _: u32) -> Result { self.0.push(v); Ok(()) }
    fn push_within_capacity(&mut self, v: T) -> std::result::Result<(), T> { self.0.push(v); Ok(()) }
}
impl<T> core::ops::Deref for KVec<T> { type Target = [T]; fn deref(&self) -> &[T] { &self.0 } }
impl<T> core::ops::DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut [T] { &mut self.0 } }
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
#[path="RT_MOD"] mod rt;
READER
IMPLS
PRODUCTION_CONSTANTS
PARSE_TYPES
PRODUCTION_PARSER
fn check_stream(bytes: &[u8], early_tiling: bool) -> Result {
    parse_stream(bytes, early_tiling).map(|_| ())
}
mod g17 {
    use super::{uapi, AnyBitPattern, Reader, Result, EINVAL, size_of, Range};
    G17_CONSTANTS
    const STAGE_PREFIXES: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES;
    const FRAGMENT_BARRIERS: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS;
    const GFP_KERNEL: u32 = 0;
    const GFP_KERNEL_ACCOUNT: u32 = 0;
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
    let mut r=render(); r.ray_query[0].scs_layout=0xffff_0000_ff00_ffff;
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
fn g16_stage_prefixes_v2_framing_and_tiling_narrowing() {
    let mut m5 = uapi::drm_asahi_cmd_render_m5 { legacy: render(), ..Default::default() };
    m5.legacy.flags = STAGE_PREFIXES;
    m5.legacy.prior_vdm = 5;
    m5.legacy.prior_cdm = 9;
    m5.stage_prefixes.ta_vdm_barrier = NONE;
    m5.stage_prefixes.ta_cdm_barrier = NONE;
    m5.stage_prefixes.ta_prior_vdm = 3;
    m5.stage_prefixes.ta_prior_cdm = 0;

    // Rejected when early_tiling is disabled; accepted when enabled.
    let wire = entry(CMD_RENDER, &data(&m5), [NONE; 2]);
    assert!(check_stream(&wire, false).is_err());
    assert!(check_stream(&wire, true).is_ok());

    // Exact 480-byte payload, zero reserved fields, zero ray-query bytes, mutually exclusive with FRAGMENT_BARRIERS.
    assert!(check_stream(&entry(CMD_RENDER, &data(&m5)[..448], [NONE; 2]), true).is_err());
    let mut over = data(&m5);
    over.extend([0u8; 8]);
    assert!(check_stream(&entry(CMD_RENDER, &over, [NONE; 2]), true).is_err());
    for field in 0..4 {
        let mut bad = m5;
        match field {
            0 => bad.stage_prefixes.reserved0 = 1,
            1 => bad.stage_prefixes.reserved1 = 1,
            2 => bad.stage_prefixes.reserved2 = 1,
            _ => bad.stage_prefixes.reserved3 = 1,
        }
        assert!(check_stream(&entry(CMD_RENDER, &data(&bad), [NONE; 2]), true).is_err());
    }
    let mut both = m5;
    both.legacy.flags |= FRAGMENT_BARRIERS;
    assert!(check_stream(&entry(CMD_RENDER, &data(&both), [NONE; 2]), true).is_err());
    let mut rq = m5;
    rq.legacy.ray_query[0].scs_layout = 0xffff_0000_ff00_ffff;
    assert!(check_stream(&entry(CMD_RENDER, &data(&rq), [NONE; 2]), true).is_err());

    // Without STAGE_PREFIXES, nonzero trailing bytes past 448 are rejected; zero tail is accepted.
    let mut plain = m5;
    plain.legacy.flags = 0;
    assert!(check_stream(&entry(CMD_RENDER, &data(&plain), [NONE; 2]), true).is_err());
    plain.stage_prefixes = Default::default();
    assert!(check_stream(&entry(CMD_RENDER, &data(&plain), [NONE; 2]), true).is_ok());

    // Out-of-bounds in-stream ta_vdm_barrier / ta_cdm_barrier are rejected by check_stream.
    let mut bad_bar = m5;
    bad_bar.stage_prefixes.ta_vdm_barrier = 1;
    assert!(check_stream(&entry(CMD_RENDER, &data(&bad_bar), [NONE; 2]), true).is_err());
    let mut bad_cdm = m5;
    bad_cdm.stage_prefixes.ta_cdm_barrier = 1;
    assert!(check_stream(&entry(CMD_RENDER, &data(&bad_cdm), [NONE; 2]), true).is_err());

    // In-stream barriers following prior compute + render succeed in check_stream.
    let mut seq = entry(CMD_COMPUTE, &data(&compute())[..64], [NONE; 2]);
    seq.extend(entry(CMD_RENDER, &data(&render())[..256], [NONE; 2]));
    let mut staged = m5;
    staged.stage_prefixes.ta_vdm_barrier = 1;
    staged.stage_prefixes.ta_cdm_barrier = 1;
    seq.extend(entry(CMD_RENDER, &data(&staged), [1, 1]));
    assert!(check_stream(&seq, true).is_ok());

    // Order::narrow_tiling allows narrower TA prefixes (including TA-no-compute while FS waits)
    // and rejects any TA prefix wider than the fragment prefix.
    let mut full = Order::new([10, 20], [2, 1], [2, 1], [10, 20], RENDER).unwrap();
    assert_eq!(full.wait_through, [Some(12), Some(21)]);
    assert_eq!(full.ta_wait_through, [Some(12), Some(21)]);
    let ta_narrow = Order::new([10, 20], [2, 1], [1, NONE], [0, 0], RENDER).unwrap();
    assert_eq!(full.narrow_tiling(&ta_narrow), Ok(()));
    assert_eq!(full.wait_through, [Some(12), Some(21)]);
    assert_eq!(full.ta_wait_through, [Some(11), None]);

    let ta_too_wide_vdm = Order::new([10, 20], [2, 1], [2, NONE], [0, 0], RENDER).unwrap();
    let mut full_vdm1 = Order::new([10, 20], [2, 1], [1, 1], [0, 0], RENDER).unwrap();
    assert_eq!(full_vdm1.narrow_tiling(&ta_too_wide_vdm), Err(EINVAL));
    let ta_too_wide_cdm = Order::new([10, 20], [2, 1], [NONE, 1], [0, 0], RENDER).unwrap();
    let mut full_nocdm = Order::new([10, 20], [2, 1], [2, NONE], [0, 0], RENDER).unwrap();
    assert_eq!(full_nocdm.narrow_tiling(&ta_too_wide_cdm), Err(EINVAL));
}
#[test]
fn unsupported_flags_and_compute_fields() {
    for flag in [1u32,4,1<<21,FRAGMENT_BARRIERS] {
        let mut r=render(); r.flags=flag;
        assert!(check_stream(&entry(CMD_RENDER,&data(&r)[..256],[NONE;2]),false).is_err());
    }
    let mut c=compute();
    c.flags=COMPUTE_NO_QUERY_ACCESS;
    assert!(check_stream(&entry(CMD_COMPUTE,&data(&c),[NONE;2]),false).is_ok());
    for bad in [1u32, 1<<16, 1<<18, COMPUTE_NO_QUERY_ACCESS | 1] {
        c.flags=bad;
        assert!(check_stream(&entry(CMD_COMPUTE,&data(&c),[NONE;2]),false).is_err());
    }
    let mut c=compute();
    c.scs_layout=0xffff_0000_ff00_ffff;
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
    assert!(!order.ready([6,6],[6,6]));
    assert!(order.ready([6,7],[6,7]));
    // Same-engine and same-VM cross-engine commands may follow published predecessors
    // (ordered on the hardware engine rings and cross-engine ring barriers).
    let compute=Order::new([0,2],[0,0],[NONE,NONE],[0,2],COMPUTE).unwrap();
    assert!(!compute.ready([0,1],[0,1]));
    assert!(compute.ready([0,1],[0,2]));
    let render=Order::new([0,2],[0,0],[NONE,NONE],[0,2],RENDER).unwrap();
    assert!(!render.ready([0,1],[0,1]));
    assert!(render.ready([0,1],[0,2]));
    assert!(render.ready([0,2],[0,2]));
    let compute_after_render=Order::new([2,0],[0,0],[NONE,NONE],[2,0],COMPUTE).unwrap();
    assert!(!compute_after_render.ready([1,0],[1,0]));
    assert!(compute_after_render.ready([1,0],[2,0]));
    let render_next=Order::new([2,0],[0,0],[NONE,NONE],[2,0],RENDER).unwrap();
    assert!(!render_next.ready([1,0],[1,0]));
    assert!(render_next.ready([1,0],[2,0]));
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
mod inline_submit_containers {
    use super::{Order, Result, COMPUTE, NONE, RENDER};
    use std::cell::Cell;

    const ENOMEM: i32 = -12;
    const GFP_KERNEL: u32 = 0;

    std::thread_local! {
        static ALLOCS: Cell<u32> = const { Cell::new(0) };
        static FAIL_NEXT: Cell<bool> = const { Cell::new(false) };
    }

    pub(crate) mod timestamp {
        #[derive(Debug, PartialEq, Eq)]
        pub(crate) struct Destinations(pub(crate) u32);
    }

    #[derive(Debug, PartialEq, Eq)]
    pub(crate) struct Command(pub(crate) u32);

    pub(crate) struct KVec<T>(Vec<T>);
    impl<T> KVec<T> {
        fn with_capacity(cap: usize, _flags: u32) -> Result<Self> {
            if FAIL_NEXT.with(|f| f.replace(false)) {
                return Err(ENOMEM);
            }
            ALLOCS.with(|a| a.set(a.get() + 1));
            Ok(Self(Vec::with_capacity(cap)))
        }
        fn push(&mut self, v: T, _flags: u32) -> Result {
            if FAIL_NEXT.with(|f| f.replace(false)) {
                return Err(ENOMEM);
            }
            self.0.push(v);
            Ok(())
        }
        fn push_within_capacity(&mut self, v: T) -> std::result::Result<(), T> {
            self.0.push(v);
            Ok(())
        }
        fn get_mut(&mut self, idx: usize) -> Option<&mut T> {
            self.0.get_mut(idx)
        }
    }
    impl<T> core::ops::Deref for KVec<T> {
        type Target = [T];
        fn deref(&self) -> &[T] { &self.0 }
    }
    impl<T> core::ops::DerefMut for KVec<T> {
        fn deref_mut(&mut self) -> &mut [T] { &mut self.0 }
    }

    INLINE_CONTAINERS

    #[test]
    fn inline_commands_and_orders_skip_heap_up_to_two_and_spill_safely() {
        ALLOCS.with(|a| a.set(0));
        let mut cmds = Commands::new();
        let mut orders = Orders::new();
        let o0 = Order::new([0, 0], [0, 0], [NONE; 2], [0; 2], COMPUTE).unwrap();
        let o1 = Order::new([0, 0], [0, 1], [NONE; 2], [0; 2], RENDER).unwrap();
        let o2 = Order::new([0, 0], [1, 1], [NONE; 2], [0; 2], COMPUTE).unwrap();
        cmds.push(Command(1)).unwrap();
        cmds.push(Command(2)).unwrap();
        orders.push(o0).unwrap();
        orders.push(o1).unwrap();
        assert_eq!(ALLOCS.with(|a| a.get()), 0);
        assert_eq!(&*cmds, &[Command(1), Command(2)]);
        assert_eq!(&*orders, &[o0, o1]);

        // OOM on 2 -> 3 spill preserves inline entries.
        FAIL_NEXT.with(|f| f.set(true));
        assert_eq!(cmds.push(Command(3)), Err(ENOMEM));
        assert_eq!(&*cmds, &[Command(1), Command(2)]);
        FAIL_NEXT.with(|f| f.set(true));
        assert_eq!(orders.push(o2), Err(ENOMEM));
        assert_eq!(&*orders, &[o0, o1]);
        assert_eq!(ALLOCS.with(|a| a.get()), 0);

        // Spill to heap preserves exact FIFO order across both containers.
        cmds.push(Command(3)).unwrap();
        orders.push(o2).unwrap();
        assert_eq!(ALLOCS.with(|a| a.get()), 2);
        assert_eq!(&*cmds, &[Command(1), Command(2), Command(3)]);
        assert_eq!(&*orders, &[o0, o1, o2]);
    }
}
mod render_admission_tests {
    use super::{
        check_render_helpers, hw, uapi, Result, DBIAS_IS_INT, EINVAL, FEW_PRIMITIVES,
        FRAGMENT_BARRIERS, MAX_DIMENSION, MAX_LAYERS, PROCESS_EMPTY_TILES,
        RSRC_SPEC_HI, STAGE_PREFIXES, ZLS_METADATA_STRIDE,
    };
    const ENOENT: i32 = -2;
    const EACCES: i32 = -13;

    pub(crate) mod vm {
        pub(crate) fn reserved_ranges() -> &'static [core::ops::Range<u64>] {
            &[]
        }
    }
    pub(crate) mod params {
        pub(crate) fn early_tiling() -> bool { true }
    }
    pub(crate) mod gate {
        use super::{mmu, Result};
        pub(crate) fn vdm(_vm: &mmu::Vm, _va: u64) -> Result { Ok(()) }
    }
    pub(crate) mod mmu {
        pub(crate) const UAT_PGMSK: usize = 0x3fff;
        pub(crate) struct VmReadGuard<'a>(&'a Vm);
        impl VmReadGuard<'_> {
            pub(crate) fn epoch(&self) -> Option<u64> {
                self.0.epoch.get()
            }
            pub(crate) fn covers_range(&mut self, start: u64, size: u64, read: bool, write: bool) -> bool {
                self.0.covers_range(start, size, read, write)
            }
        }
        pub(crate) struct Vm {
            pub(crate) maps: Vec<(u64, u64, bool, bool)>,
            pub(crate) epoch: std::cell::Cell<Option<u64>>,
            pub(crate) batch_calls: std::cell::Cell<usize>,
            pub(crate) cover_calls: std::cell::Cell<usize>,
            pub(crate) batch_ranges: std::cell::Cell<usize>,
        }
        impl Vm {
            pub(crate) fn read_lock(&self) -> super::Result<VmReadGuard<'_>> {
                Ok(VmReadGuard(self))
            }
            pub(crate) fn mapping_validation_epoch(&self) -> Option<u64> {
                self.epoch.get()
            }
            pub(crate) fn covers_range(&self, start: u64, size: u64, read: bool, write: bool) -> bool {
                self.cover_calls.set(self.cover_calls.get() + 1);
                let Some(end) = start.checked_add(size) else { return false };
                self.maps.iter().any(|&(s, e, r, w)| {
                    start >= s && end <= e && (!read || r) && (!write || w)
                })
            }
            pub(crate) fn covers_ranges_batch(&self, ranges: &[(u64, u64, u8)]) -> (bool, Option<u64>) {
                self.batch_calls.set(self.batch_calls.get() + 1);
                self.batch_ranges.set(self.batch_ranges.get() + ranges.len());
                for &(start, size, acc) in ranges {
                    if !self.covers_range(start, size, acc & 1 != 0, acc & 2 != 0) {
                        return (false, self.epoch.get());
                    }
                }
                (true, self.epoch.get())
            }
        }
    }
    pub(crate) struct Context<'a> {
        pub(crate) vm: &'a mmu::Vm,
        pub(crate) usc_base: u64,
        pub(crate) active_cores: u32,
        pub(crate) render_scs: [u16; 2],
        pub(crate) max_tile_buffer: u32,
    }
    use super::MAX_COMMANDS;
    RENDER_ADMISSION

    fn check_render_one(
        ctx: &Context<'_>,
        cmd: &uapi::drm_asahi_cmd_render,
        vertex: &[uapi::drm_asahi_attachment],
        fragment: &[uapi::drm_asahi_attachment],
    ) -> Result {
        let mut batch = SpanBatch::new();
        check_render(&mut batch, ctx, cmd, vertex, fragment)?;
        batch.flush(ctx.vm)
    }

    const BASE_GVA: u64 = 0x10_0000_0000;
    fn base_vm() -> mmu::Vm {
        mmu::Vm {
            maps: vec![
                (BASE_GVA, BASE_GVA + 0x10_0000, true, false),
                (BASE_GVA + 0x20_0000, BASE_GVA + 0x40_0000, true, true),
                (BASE_GVA + 0x50_0000, BASE_GVA + 0x51_0000, true, false),
            ],
            epoch: std::cell::Cell::new(Some(1)),
            batch_calls: std::cell::Cell::new(0),
            cover_calls: std::cell::Cell::new(0),
            batch_ranges: std::cell::Cell::new(0),
        }
    }
    fn base_cmd() -> uapi::drm_asahi_cmd_render {
        uapi::drm_asahi_cmd_render {
            width_px: 64,
            height_px: 64,
            layers: 1,
            samples: 1,
            utile_width_px: 32,
            utile_height_px: 32,
            sample_size_B: 8,
            vdm_ctrl_stream_base: BASE_GVA,
            isp_scissor_base: BASE_GVA + 0x100,
            ..Default::default()
        }
    }
    const Z_COMP: u64 = (1 << 1) | (1 << 2) | (1 << 5) | (1 << 6) | (1 << 15) | (1 << 19);
    const S_COMP: u64 = (1 << 3) | (1 << 4) | (1 << 7) | (1 << 8) | (1 << 14) | (1 << 18);

    #[test]
    fn compressed_zls_1x_layered_and_2x_accepted() {
        let vm = base_vm();
        let ctx = Context { vm: &vm, usc_base: 1 << 40, active_cores: 10, render_scs: [0, 0], max_tile_buffer: 32 * 1024 };
        // 1x compressed D32F depth + stencil.
        let mut cmd = base_cmd();
        cmd.zls_ctrl = Z_COMP | S_COMP;
        cmd.depth.base = BASE_GVA + 0x20_0000;
        cmd.depth.comp_base = BASE_GVA + 0x24_0000;
        cmd.stencil.base = BASE_GVA + 0x28_0000;
        cmd.stencil.comp_base = BASE_GVA + 0x2c_0000;
        assert_eq!(check_render_one(&ctx, &cmd, &[], &[]), Ok(()));
        // 1x compressed D16 depth (Z Format = 2 at bits 25..26).
        let mut d16 = base_cmd();
        d16.zls_ctrl = Z_COMP | (2 << 25);
        d16.depth.base = BASE_GVA + 0x20_0000;
        d16.depth.comp_base = BASE_GVA + 0x24_0000;
        assert_eq!(check_render_one(&ctx, &d16, &[], &[]), Ok(()));
        // 2x MSAA compressed depth.
        let mut msaa2 = d16;
        msaa2.samples = 2;
        assert_eq!(check_render_one(&ctx, &msaa2, &[], &[]), Ok(()));
        // Layered compressed depth/stencil with comp_stride == 0 (1 line = 0x80) and nonzero stride.
        let mut layered = cmd;
        layered.layers = 4;
        layered.depth.stride = (1 << 14) | 1;
        layered.stencil.stride = (1 << 14) | 1;
        assert_eq!(check_render_one(&ctx, &layered, &[], &[]), Ok(()));
        layered.depth.comp_stride = 3 << 14;
        layered.stencil.comp_stride = 2 << 14;
        assert_eq!(check_render_one(&ctx, &layered, &[], &[]), Ok(()));
    }

    #[test]
    fn tile_buffer_limit_follows_the_context() {
        let vm = base_vm();
        let k32 = Context { vm: &vm, usc_base: 1 << 40, active_cores: 10, render_scs: [0, 0], max_tile_buffer: 32 * 1024 };
        let k48 = Context { max_tile_buffer: 48 * 1024, ..k32 };
        // 8 B x 32x32 x 4 samples = 32 KiB: accepted by both.
        let mut k32_tile = base_cmd();
        k32_tile.samples = 4;
        assert_eq!(check_render_one(&k32, &k32_tile, &[], &[]), Ok(()));
        assert_eq!(check_render_one(&k48, &k32_tile, &[], &[]), Ok(()));
        // 24 B x 32x16 x 4 samples = 48 KiB, the MSAA4 tile macOS programs on the M4.
        let mut k48_tile = k32_tile;
        k48_tile.utile_height_px = 16;
        k48_tile.sample_size_B = 24;
        assert_eq!(check_render_one(&k32, &k48_tile, &[], &[]), Err(EINVAL));
        assert_eq!(check_render_one(&k48, &k48_tile, &[], &[]), Ok(()));
        // 28 B x 32x16 x 4 samples = 56 KiB: over both limits.
        let mut over = k48_tile;
        over.sample_size_B = 28;
        assert_eq!(check_render_one(&k48, &over, &[], &[]), Err(EINVAL));
    }

    #[test]
    fn compressed_zls_invalid_ctrl_alignment_and_vm_spans_rejected() {
        let vm = base_vm();
        let ctx = Context { vm: &vm, usc_base: 1 << 40, active_cores: 10, render_scs: [0, 0], max_tile_buffer: 32 * 1024 };
        let valid = || {
            let mut cmd = base_cmd();
            cmd.zls_ctrl = Z_COMP | S_COMP;
            cmd.depth.base = BASE_GVA + 0x20_0000;
            cmd.depth.comp_base = BASE_GVA + 0x24_0000;
            cmd.stencil.base = BASE_GVA + 0x28_0000;
            cmd.stencil.comp_base = BASE_GVA + 0x2c_0000;
            cmd
        };
        // Asymmetric compress bits per plane.
        for bit in [1u64 << 2, 1 << 6, 1 << 4, 1 << 8] {
            let mut bad = valid();
            bad.zls_ctrl ^= bit;
            assert_eq!(check_render_one(&ctx, &bad, &[], &[]), Err(EINVAL));
        }
        // Unaligned comp_stride or comp_base.
        for bad_stride in [1u32, 0x4001, 0x3fff] {
            let mut bad = valid();
            bad.depth.comp_stride = bad_stride;
            assert_eq!(check_render_one(&ctx, &bad, &[], &[]), Err(EINVAL));
        }
        let mut bad = valid();
        bad.depth.comp_base += 2;
        assert_eq!(check_render_one(&ctx, &bad, &[], &[]), Err(EINVAL));
        // comp_base without compress bits, compress bits without comp_base, stride without base.
        let mut bad = valid();
        bad.zls_ctrl &= !((1 << 2) | (1 << 6));
        assert_eq!(check_render_one(&ctx, &bad, &[], &[]), Err(EINVAL));
        let mut bad = valid();
        bad.depth.comp_base = 0;
        assert_eq!(check_render_one(&ctx, &bad, &[], &[]), Err(EINVAL));
        let mut bad = valid();
        bad.zls_ctrl = Z_COMP;
        bad.stencil = Default::default();
        bad.stencil.comp_stride = 1 << 14;
        assert_eq!(check_render_one(&ctx, &bad, &[], &[]), Err(EINVAL));
        // Unmapped comp_base -> ENOENT; read-only comp_base -> EACCES.
        let mut unmapped = valid();
        unmapped.depth.comp_base = BASE_GVA + 0x60_0000;
        assert_eq!(check_render_one(&ctx, &unmapped, &[], &[]), Err(ENOENT));
        let mut ro = valid();
        ro.depth.comp_base = BASE_GVA + 0x50_0000;
        assert_eq!(check_render_one(&ctx, &ro, &[], &[]), Err(EACCES));
        // Layered metadata span extending past mapped range -> ENOENT.
        let mut past_end = valid();
        past_end.layers = 4;
        past_end.depth.stride = 1;
        past_end.stencil.stride = 1;
        past_end.depth.comp_base = BASE_GVA + 0x3f_c000;
        past_end.depth.comp_stride = 0x100 << 14;
        assert_eq!(check_render_one(&ctx, &past_end, &[], &[]), Err(ENOENT));
    }

    #[test]
    fn submit_wide_span_batch_dedups_and_flushes_once_across_passes() {
        let vm = base_vm();
        let ctx = Context { vm: &vm, usc_base: 1 << 40, active_cores: 10, render_scs: [0, 0], max_tile_buffer: 32 * 1024 };
        let mut batch = SpanBatch::new();
        for pass in 0..4u64 {
            let mut cmd = base_cmd();
            cmd.vdm_ctrl_stream_base = BASE_GVA + pass * 0x20;
            cmd.zls_ctrl = Z_COMP;
            cmd.depth.base = BASE_GVA + 0x20_0000 + pass * 0x4000;
            cmd.depth.comp_base = BASE_GVA + 0x24_0000 + pass * 0x4000;
            assert_eq!(check_render(&mut batch, &ctx, &cmd, &[], &[]), Ok(()));
        }
        assert_eq!(vm.batch_calls.get(), 0);
        assert_eq!(batch.flush(&vm), Ok(()));
        assert_eq!(vm.batch_calls.get(), 1);
        let mut guard = vm.read_lock().unwrap();
        let mut proof = SpanProof::new();
        let epoch = guard.epoch();
        assert_eq!(check_render(&mut batch, &ctx, &base_cmd(), &[], &[]), Ok(()));
        assert_eq!(batch.flush_locked_with_proof(&mut guard, epoch, &mut proof), Ok(()));
        assert_eq!(vm.batch_calls.get(), 1);
        drop(guard);

        // Unmapped or read-only span on a later pass still reports exact ENOENT / EACCES.
        let mut bad_unmapped = base_cmd();
        bad_unmapped.zls_ctrl = Z_COMP;
        bad_unmapped.depth.base = BASE_GVA + 0x20_0000;
        bad_unmapped.depth.comp_base = BASE_GVA + 0x60_0000;
        assert_eq!(check_render(&mut batch, &ctx, &base_cmd(), &[], &[]), Ok(()));
        assert_eq!(check_render(&mut batch, &ctx, &bad_unmapped, &[], &[]), Ok(()));
        assert_eq!(batch.flush(&vm), Err(ENOENT));

        let mut bad_ro = bad_unmapped;
        bad_ro.depth.comp_base = BASE_GVA + 0x50_0000;
        assert_eq!(check_render(&mut batch, &ctx, &base_cmd(), &[], &[]), Ok(()));
        assert_eq!(check_render(&mut batch, &ctx, &bad_ro, &[], &[]), Ok(()));
        assert_eq!(batch.flush(&vm), Err(EACCES));
    }

    #[test]
    fn few_primitives_flag_admitted_in_parse_and_render_check() {
        let vm = base_vm();
        let ctx = Context { vm: &vm, usc_base: 1 << 40, active_cores: 10, render_scs: [0, 0], max_tile_buffer: 32 * 1024 };
        let mut r = super::render();
        r.flags = FEW_PRIMITIVES;
        assert!(super::check_stream(
            &super::entry(super::CMD_RENDER, &super::data(&r)[..256], [super::NONE; 2]),
            false,
        )
        .is_ok());
        assert!(super::check_stream(
            &super::entry(super::CMD_RENDER, &super::data(&r)[..256], [super::NONE; 2]),
            true,
        )
        .is_ok());
        r.flags = FEW_PRIMITIVES | DBIAS_IS_INT;
        assert!(super::check_stream(
            &super::entry(super::CMD_RENDER, &super::data(&r)[..256], [super::NONE; 2]),
            false,
        )
        .is_ok());
        let mut cmd = base_cmd();
        cmd.flags |= FEW_PRIMITIVES;
        assert_eq!(check_render_one(&ctx, &cmd, &[], &[]), Ok(()));
        cmd.isp_dbias_base = BASE_GVA + 0x200;
        cmd.flags |= DBIAS_IS_INT;
        assert_eq!(check_render_one(&ctx, &cmd, &[], &[]), Ok(()));
        cmd.flags |= 1 << 21;
        assert_eq!(check_render_one(&ctx, &cmd, &[], &[]), Err(EINVAL));
    }

    #[test]
    fn span_proof_reuses_epoch_partial_walks_and_invalidates_on_unmap() {
        let mut vm = base_vm();
        let mut proof = SpanProof::new();
        let cmd = base_cmd();

        // First flush populates the proof for epoch 1.
        let mut batch = SpanBatch::new();
        let ctx = Context { vm: &vm, usc_base: 1 << 40, active_cores: 10, render_scs: [0, 0], max_tile_buffer: 32 * 1024 };
        check_render(&mut batch, &ctx, &cmd, &[], &[]).unwrap();
        assert_eq!(batch.flush_with_proof(&vm, &mut proof), Ok(()));
        assert_eq!(vm.batch_calls.get(), 1);

        // Second flush at unchanged epoch hits the proof with zero VM walks.
        let mut batch = SpanBatch::new();
        check_render(&mut batch, &ctx, &cmd, &[], &[]).unwrap();
        assert_eq!(batch.flush_with_proof(&vm, &mut proof), Ok(()));
        assert_eq!(vm.batch_calls.get(), 1);

        // A new attachment triggers a partial walk of only the uncovered span.
        let att = uapi::drm_asahi_attachment {
            pointer: BASE_GVA + 0x20_0000,
            size: 0x4000,
            ..Default::default()
        };
        let ranges_before = vm.batch_ranges.get();
        let mut batch = SpanBatch::new();
        check_render(&mut batch, &ctx, &cmd, &[att], &[]).unwrap();
        assert_eq!(batch.flush_with_proof(&vm, &mut proof), Ok(()));
        assert_eq!(vm.batch_calls.get(), 2);
        assert_eq!(vm.batch_ranges.get() - ranges_before, 1);

        // Locked flush under a held VmReadGuard also skips already-proven spans.
        let covers_before = vm.cover_calls.get();
        let mut batch = SpanBatch::new();
        check_render(&mut batch, &ctx, &cmd, &[att], &[]).unwrap();
        let mut guard = vm.read_lock().unwrap();
        let epoch = guard.epoch();
        assert_eq!(batch.flush_locked_with_proof(&mut guard, epoch, &mut proof), Ok(()));
        drop(guard);
        assert_eq!(vm.cover_calls.get(), covers_before);

        // Adjacent read-only and read-write ranges never merge to grant write on RO memory.
        let mut rw_batch = SpanBatch::new();
        rw_batch.push(&vm, BASE_GVA, BASE_GVA + 0x4000, true, true).unwrap();
        assert_eq!(rw_batch.flush_with_proof(&vm, &mut proof), Err(EACCES));

        // Repopulate, then simulate an unmap that bumps the epoch: cached proof is invalidated.
        let mut batch = SpanBatch::new();
        check_render(&mut batch, &ctx, &cmd, &[att], &[]).unwrap();
        assert_eq!(batch.flush_with_proof(&vm, &mut proof), Ok(()));
        vm.maps.remove(1);
        vm.epoch.set(Some(2));
        let mut batch = SpanBatch::new();
        let ctx = Context { vm: &vm, usc_base: 1 << 40, active_cores: 10, render_scs: [0, 0], max_tile_buffer: 32 * 1024 };
        check_render(&mut batch, &ctx, &cmd, &[att], &[]).unwrap();
        assert_eq!(batch.flush_with_proof(&vm, &mut proof), Err(ENOENT));

        // Epoch None (concurrent mutation) never serves a stale cached proof.
        vm.epoch.set(None);
        let mut batch = SpanBatch::new();
        check_render(&mut batch, &ctx, &cmd, &[], &[]).unwrap();
        let calls_before = vm.batch_calls.get();
        assert_eq!(batch.flush_with_proof(&vm, &mut proof), Ok(()));
        assert_eq!(vm.batch_calls.get(), calls_before + 1);
    }
}
'''
replacements = {'FULL_UAPI': str(BINDINGS), 'READER': reader,
                'CAPACITIES': capacities, 'FW_GRAPH': str(SOURCE.parent/'fw/graph.rs'),
                'FW_WORK': str(SOURCE.parent/'fw/work.rs'), 'FULL_ENCODER': str(SOURCE.parent/'encode.rs'),
                'RT_MOD': str(SOURCE.parent/'rt.rs'),
                'IMPLS': '\n'.join('unsafe impl AnyBitPattern for uapi::' + name + ' {}' for name in names),
                'PRODUCTION_CONSTANTS': constants,
                'PARSE_TYPES': parse_types.replace('super::rt::', 'rt::'),
                'PRODUCTION_PARSER': parser.replace('super::encode::', 'encode::').replace('super::rt::', 'rt::'),
                'INLINE_CONTAINERS': inline_containers.replace('crate::timestamp::', 'timestamp::'),
                'RENDER_ADMISSION': render_admission.replace('super::vm::', 'vm::').replace('super::params::', 'params::'),
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
