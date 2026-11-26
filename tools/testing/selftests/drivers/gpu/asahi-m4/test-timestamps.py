#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise actual shared timestamp stores and G16 retirement publication with host memory."""
from common import kernel_root, output_dir, rustc
import subprocess

root = kernel_root()
out = output_dir()
backend = root / 'drivers/gpu/drm/asahi'


def function(source, marker):
    start = source.index(marker)
    first = source.index('{', start)
    depth, end = 1, first + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


mmu = (backend / 'mmu.rs').read_text()
shared = (backend / 'timestamp.rs').read_text()
poll = (backend / 'g16/poll.rs').read_text()
obj = (backend / 'g16/object.rs').read_text()
cache_policy = function((backend / 'gem.rs').read_text(), 'pub(crate) fn is_writeback(')
store = function(mmu, 'pub(crate) fn write(&self, value: u64)')
vmap_store = function(mmu, 'pub(crate) fn write(&self, offset: usize, value: u64)')
write = function(shared, 'fn write(&self, ticks: u64, frequency_hz: u64)')
checked_offset = function(shared, 'fn checked_offset(')
publish = function(shared, 'pub(crate) fn publish(&self, ticks: [u64; 4], frequency_hz: u64)')
retire = function(poll, 'fn publish_timestamps(&self, flight: &Flight, claims: &super::slots::Claims)')
compute_ticks = function(poll, 'fn compute_ticks(&self, unit: &super::slots::ComputeSlot)')
compute_ticks_with = function(poll, 'fn compute_ticks_with(')
fw_r64 = function(obj, 'pub(crate) fn r64(&self, offset: usize)')
fw_read = function(obj, 'pub(crate) fn read(&self, offset: usize, out: &mut [u8])')
source = r'''
#![allow(dead_code)]
use core::sync::atomic::{AtomicU8, AtomicU64, Ordering, fence as barrier};
use core::mem::size_of;
use std::sync::Arc;
mod uapi { pub const drm_asahi_gem_flags_DRM_ASAHI_GEM_WRITEBACK:u32=4; }
thread_local! { static TRACE:std::cell::RefCell<Option<Vec<&'static str>>> = const { std::cell::RefCell::new(None) }; }
mod pgtable {
    pub fn clean_poc(pointer:*const u8,size:usize) {
        assert!(!pointer.is_null()); assert_eq!(size,8);
        super::TRACE.with(|t| { if let Some(trace)=t.borrow_mut().as_mut() {trace.push("clean");} });
    }
    pub fn poc_barrier() { super::TRACE.with(|t| { if let Some(trace)=t.borrow_mut().as_mut() {trace.push("complete");} }); }
}
type Result<T=()> = std::result::Result<T,i32>;
const EINVAL:i32=-22; const EOVERFLOW:i32=-75;const ERANGE:i32=-34;
#[repr(C,align(8))]struct Backing{words:[AtomicU64;4],flags:u32,timestamp_lock:std::sync::Mutex<()>}
impl Backing { __CACHE_POLICY__ }
struct Map{backing:Arc<Backing>,addr:*mut core::ffi::c_void}
unsafe impl Send for Map {} unsafe impl Sync for Map {}
struct View(*mut u8);impl View{fn as_mut_ptr(&self)->*mut u8{self.0}}
impl Map{fn get(&self)->View{View(self.backing.words.as_ptr().cast_mut().cast())}fn owner(&self)->&Backing{&self.backing}}
struct TimestampVmap(Map);
impl TimestampVmap {
    fn size(&self)->usize{core::mem::size_of_val(&self.0.backing.words)}
    __VMAP_STORE__
}
struct TimestampMapping{mapping:TimestampVmap,offset:usize}
impl TimestampMapping { __STORE__ }
__CHECKED_OFFSET__
struct Destination{cpu:TimestampMapping}
impl Destination { __WRITE__ }
struct Destinations([Option<Destination>;4]);
impl Destinations {
    fn is_empty(&self)->bool{self.0.iter().all(Option::is_none)}
    fn iter(&self)->core::slice::Iter<'_,Option<Destination>>{self.0.iter()}
    __PUBLISH__
}
fn backing()->Arc<Backing>{backing_with_flags(0)}
fn backing_with_flags(flags:u32)->Arc<Backing>{Arc::new(Backing{flags,words:[const{AtomicU64::new(u64::MAX)};4],timestamp_lock:std::sync::Mutex::new(())})}
fn destination(memory:&Arc<Backing>,offset:usize)->Destination{
    Destination{cpu:TimestampMapping{mapping:TimestampVmap(Map{backing:memory.clone(),addr:memory.words.as_ptr().cast_mut().cast()}),offset}}
}
fn outputs(memory:&Arc<Backing>)->Destinations{
    Destinations(core::array::from_fn(|i|Some(destination(memory,i*8))))
}
fn expected(ticks:u64)->u64{((ticks as u128)*125/3)as u64}
#[test]fn cacheable_queries_are_cleaned_and_every_store_completes_before_publication(){
    for flags in [0,uapi::drm_asahi_gem_flags_DRM_ASAHI_GEM_WRITEBACK] {
        for offset in [0,3] {
            let memory=backing_with_flags(flags);
            TRACE.with(|t| *t.borrow_mut()=Some(Vec::new()));
            destination(&memory,offset).cpu.write(0x0102030405060708);
            let trace=TRACE.with(|t| t.borrow_mut().take().unwrap());
            if flags==0 { assert_eq!(trace,vec!["complete"]); }
            else { assert_eq!(trace,vec!["clean","complete"]); }
        }
    }
}
#[test]fn shared_resolver_preserves_g16_and_g17_offset_contracts(){
    for size in [0,8,16,16384]{for offset in [0,1,7,8,15,16,16376,16377,16384,u64::MAX-7,u64::MAX]{
        for aligned in [false,true]{
            let expected=if offset.checked_add(8).is_none()||(aligned&&offset%8!=0){Err(EINVAL)}
                else if offset+8>size as u64{Err(if aligned{EINVAL}else{ERANGE})}else{Ok(offset as usize)};
            assert_eq!(checked_offset(offset,size,aligned),expected);
        }
    }}
}
#[test]fn native_24mhz_conversion_includes_long_uptimes_and_wrap(){
    let memory=backing();let destination=destination(&memory,0);
    for ticks in [0,1,2,3,24_000_000,u32::MAX as u64,u64::MAX/125,u64::MAX/2,u64::MAX]{
        destination.write(ticks,24_000_000);
        assert_eq!(memory.words[0].load(Ordering::Relaxed),expected(ticks));
    }
}
#[test]fn aligned_query_is_published_without_a_torn_available_value(){
    const VALUE:u64=0x1020304050607080;
    let memory=backing();let cpu=destination(&memory,0).cpu;
    std::thread::scope(|scope|{
        scope.spawn(||{for _ in 0..100_000{cpu.write(VALUE);cpu.write(u64::MAX);}});
        for _ in 0..100_000{
            let value=memory.words[0].load(Ordering::Relaxed);
            assert!(value==VALUE||value==u64::MAX,"torn timestamp {value:x}");
        }
    });
}
#[test]fn unaligned_legacy_destination_preserves_neighbouring_bytes(){
    let memory=backing();let cpu=destination(&memory,3).cpu;
    cpu.write(0x0102030405060708);
    let mut bytes=Vec::new();for word in &memory.words{bytes.extend(word.load(Ordering::Relaxed).to_ne_bytes());}
    assert_eq!(&bytes[3..11],&0x0102030405060708u64.to_le_bytes());
    assert!(bytes[..3].iter().chain(&bytes[11..]).all(|&byte|byte==255));
}
#[test]fn overlapping_bindings_share_one_store_lock(){
    const A:u64=0x1020304050607080;const B:u64=0x1122334455667788;
    let memory=backing();let aligned=destination(&memory,0).cpu;let unaligned=destination(&memory,1).cpu;
    std::thread::scope(|scope|{
        scope.spawn(||{for _ in 0..100_000{aligned.write(A);}});
        scope.spawn(||{for _ in 0..100_000{unaligned.write(B);}});
        for _ in 0..100_000{
            let _guard=memory.timestamp_lock.lock().unwrap();
            let pointer=memory.words.as_ptr().cast::<AtomicU8>();
            let bytes:[u8;16]=core::array::from_fn(|index|unsafe{(*pointer.add(index)).load(Ordering::Relaxed)});
            assert!(bytes[..8]==A.to_le_bytes()||bytes[1..9]==B.to_le_bytes()||bytes.iter().all(|&b|b==255));
            assert!(bytes[9..].iter().all(|&b|b==255));
        }
    });
}
#[test]fn absent_endpoints_remain_untouched(){
    let memory=backing();let mut destinations=Destinations(core::array::from_fn(|_|None));
    destinations.0[1]=Some(destination(&memory,8));
    destinations.publish([11,22,33,44],24_000_000);
    assert_eq!(memory.words[0].load(Ordering::Relaxed),u64::MAX);
    assert_eq!(memory.words[1].load(Ordering::Relaxed),expected(22));
    assert_eq!(memory.words[2].load(Ordering::Relaxed),u64::MAX);
}
mod backend {
    use super::*;
    pub mod hw{pub const COMMAND_TIMESTAMP_HZ:u64=24_000_000;}
    pub mod slots{
        use super::*;
        pub struct Claims{pub slot:[u8;16],pub slot_count:u32,pub pass:[u8;4]}
        pub type ComputeSlot = Compute;
    }
    pub mod command{
        use super::*;
        pub struct Compute{pub timestamp_outputs:Destinations}
        pub struct Render{pub timestamp_outputs:Destinations}
        pub enum Command{Compute(Compute),Render(Render)}
    }
    pub struct Cells {
        pub vals: [u64; 4],
        pub r64_calls: std::cell::Cell<u32>,
        pub r32_calls: std::cell::Cell<u32>,
        pub sync_calls: std::cell::Cell<u32>,
    }
    impl Cells {
        pub fn new(vals: [u64; 4]) -> Self {
            Self {
                vals,
                r64_calls: std::cell::Cell::new(0),
                r32_calls: std::cell::Cell::new(0),
                sync_calls: std::cell::Cell::new(0),
            }
        }
        pub fn sync_for_cpu(&self, _offset: usize, _len: usize) -> Result {
            self.sync_calls.set(self.sync_calls.get() + 1);
            Ok(())
        }
        pub fn r64(&self, offset: usize) -> Result<u64> {
            self.r64_calls.set(self.r64_calls.get() + 1);
            match offset {
                0 => Ok(self.vals[0]),
                8 => Ok(self.vals[1]),
                0x600 => Ok(self.vals[0]),
                0x608 => Ok(self.vals[1]),
                0x620 => Ok(self.vals[2]),
                0x628 => Ok(self.vals[3]),
                _ => Err(EINVAL),
            }
        }
        pub fn r32(&self, offset: usize) -> Result<u32> {
            self.r32_calls.set(self.r32_calls.get() + 1);
            let v = match offset {
                0x100 => self.vals[0],
                0x104 => self.vals[0] >> 32,
                0x108 => self.vals[1],
                0x10c => self.vals[1] >> 32,
                _ => return Err(EINVAL),
            };
            Ok(v as u32)
        }
    }
    pub struct Compute{pub its:Cells,pub work:Cells}pub struct Pass{pub scene:Cells}
    pub struct ComputeAbi{pub work_ticks:Option<(usize,usize)>}
    pub struct Abi{pub compute:ComputeAbi}
    impl Abi{pub fn compute(&self)->&ComputeAbi{&self.compute}}
    pub struct Init{pub abi:Abi}
    pub struct Scrub{pub compute:Vec<Compute>,pub passes:Vec<Pass>}
    pub struct Slots{pub scrub:Scrub}pub struct Firmware{pub slots:Slots,pub init:Init}
    pub struct Resources{pub compute:Vec<Compute>,pub passes:Vec<Pass>}
    impl Resources{pub fn compute(&self)->Result<&Self>{Ok(self)}pub fn render(&self)->Result<&Self>{Ok(self)}pub fn compute_unit(&self,k:usize)->Result<&Compute>{self.compute.get(k).ok_or(super::EINVAL)}pub fn render_pass(&self,k:usize)->Result<&Pass>{self.passes.get(k).ok_or(super::EINVAL)}}
    pub struct Job{pub commands:Vec<command::Command>,pub resources:Resources}
    pub struct Flight{pub job:Job,pub group:Vec<Job>,pub first:usize,pub count:usize,pub renders:u32}
    pub mod poll{
        use super::*;
        const EIO:i32=-5;
        impl Firmware{
            __COMPUTE_TICKS__
            __COMPUTE_TICKS_WITH__
            __RETIRE__
        }
    }
}
use backend::*;
fn firmware()->Firmware{Firmware{slots:Slots{scrub:Scrub{compute:vec![],passes:vec![]}},init:Init{abi:Abi{compute:ComputeAbi{work_ticks:None}}}}}
fn resources()->Resources{Resources{
    compute:vec![
        Compute{its:Cells::new([10,20,0,0]),work:Cells::new([0,0,0,0])},
        Compute{its:Cells::new([30,40,0,0]),work:Cells::new([0,0,0,0])},
        Compute{its:Cells::new([50,60,0,0]),work:Cells::new([0,0,0,0])},
    ],
    passes:vec![Pass{scene:Cells::new([100,200,300,400])},Pass{scene:Cells::new([500,600,700,800])},Pass{scene:Cells::new([900,1000,1100,1200])}]
}}
#[test]fn retirement_uses_claimed_hardware_cells_and_endpoint_order(){
    let compute=backing();let render=backing();
    let flight=Flight{job:Job{commands:vec![
        command::Command::Compute(command::Compute{timestamp_outputs:outputs(&compute)}),
        command::Command::Render(command::Render{timestamp_outputs:outputs(&render)})
    ],resources:resources()},group:vec![],first:0,count:2,renders:1};
    let mut slot=[0u8;16];slot[0]=1;
    let claims=slots::Claims{slot,slot_count:1,pass:[1,0,0,0]};
    firmware().publish_timestamps(&flight,&claims).unwrap();
    assert_eq!(compute.words[0].load(Ordering::Relaxed),expected(30));
    assert_eq!(compute.words[1].load(Ordering::Relaxed),expected(40));
    for (index,ticks) in [500,600,700,800].into_iter().enumerate(){
        assert_eq!(render.words[index].load(Ordering::Relaxed),expected(ticks));
    }
}
#[test]fn batched_compute_horizon_group_timestamps_publish_for_all_members(){
    let c0=backing();let c1=backing();let c2=backing();
    let flight=Flight{
        job:Job{commands:vec![command::Command::Compute(command::Compute{timestamp_outputs:outputs(&c0)})],resources:resources()},
        group:vec![
            Job{commands:vec![command::Command::Compute(command::Compute{timestamp_outputs:outputs(&c1)})],resources:Resources{compute:vec![],passes:vec![]}},
            Job{commands:vec![command::Command::Compute(command::Compute{timestamp_outputs:outputs(&c2)})],resources:Resources{compute:vec![],passes:vec![]}},
        ],
        first:0,count:1,renders:0,
    };
    let mut slot=[0u8;16];slot[0]=0;slot[1]=1;slot[2]=2;
    let claims=slots::Claims{slot,slot_count:3,pass:[0,0,0,0]};
    firmware().publish_timestamps(&flight,&claims).unwrap();
    assert_eq!((c0.words[0].load(Ordering::Relaxed),c0.words[1].load(Ordering::Relaxed)),(expected(10),expected(20)));
    assert_eq!((c1.words[0].load(Ordering::Relaxed),c1.words[1].load(Ordering::Relaxed)),(expected(30),expected(40)));
    assert_eq!((c2.words[0].load(Ordering::Relaxed),c2.words[1].load(Ordering::Relaxed)),(expected(50),expected(60)));
}
#[test]fn batched_render_horizon_group_timestamps_publish_for_all_members(){
    let r0=backing();let r1=backing();let r2=backing();
    let flight=Flight{
        job:Job{commands:vec![command::Command::Render(command::Render{timestamp_outputs:outputs(&r0)})],resources:resources()},
        group:vec![
            Job{commands:vec![command::Command::Render(command::Render{timestamp_outputs:outputs(&r1)})],resources:Resources{compute:vec![],passes:vec![]}},
            Job{commands:vec![command::Command::Render(command::Render{timestamp_outputs:outputs(&r2)})],resources:Resources{compute:vec![],passes:vec![]}},
        ],
        first:0,count:1,renders:3,
    };
    let claims=slots::Claims{slot:[0;16],slot_count:0,pass:[0,1,2,0]};
    firmware().publish_timestamps(&flight,&claims).unwrap();
    for (buf, ticks) in [(&r0,[100,200,300,400]),(&r1,[500,600,700,800]),(&r2,[900,1000,1100,1200])] {
        for (index, tick) in ticks.into_iter().enumerate() {
            assert_eq!(buf.words[index].load(Ordering::Relaxed), expected(tick));
        }
    }
}
#[test]fn invalid_command_span_and_missing_claim_are_rejected(){
    let memory=backing();let mut flight=Flight{job:Job{commands:vec![
        command::Command::Compute(command::Compute{timestamp_outputs:outputs(&memory)})
    ],resources:resources()},group:vec![],first:usize::MAX,count:2,renders:0};
    let mut claims=slots::Claims{slot:[0;16],slot_count:1,pass:[0,0,0,0]};
    assert_eq!(firmware().publish_timestamps(&flight,&claims),Err(EOVERFLOW));
    flight.first=1;assert_eq!(firmware().publish_timestamps(&flight,&claims),Err(EINVAL));
    flight.first=0;flight.count=1;claims.slot_count=0;
    assert_eq!(firmware().publish_timestamps(&flight,&claims),Err(EINVAL));
    assert_eq!(memory.words[0].load(Ordering::Relaxed),u64::MAX);
}
#[test]fn empty_timestamp_outputs_skip_hardware_cell_reads(){
    let flight=Flight{job:Job{commands:vec![
        command::Command::Compute(command::Compute{timestamp_outputs:Destinations(core::array::from_fn(|_|None))}),
        command::Command::Render(command::Render{timestamp_outputs:Destinations(core::array::from_fn(|_|None))}),
    ],resources:Resources{compute:vec![],passes:vec![]}},group:vec![],first:0,count:2,renders:1};
    let claims=slots::Claims{slot:[0;16],slot_count:1,pass:[0,0,0,0]};
    firmware().publish_timestamps(&flight,&claims).unwrap();
}
#[test]fn work_ticks_fallback_publishes_compute_timestamps_for_26b123(){
    let c0=backing();let c1=backing();
    let mut fw=firmware();
    fw.init.abi.compute.work_ticks=Some((0x100,0x108));
    let res=Resources{
        compute:vec![
            Compute{its:Cells::new([0,0,0,0]),work:Cells::new([0x1_0000_0070,0x1_0000_0090,0,0])},
            Compute{its:Cells::new([0,0,0,0]),work:Cells::new([0x2_0000_0010,0x2_0000_0040,0,0])},
        ],
        passes:vec![],
    };
    let flight=Flight{
        job:Job{commands:vec![command::Command::Compute(command::Compute{timestamp_outputs:outputs(&c0)})],resources:res},
        group:vec![Job{commands:vec![command::Command::Compute(command::Compute{timestamp_outputs:outputs(&c1)})],resources:Resources{compute:vec![],passes:vec![]}}],
        first:0,count:1,renders:0,
    };
    let mut slot=[0u8;16];slot[0]=0;slot[1]=1;
    let claims=slots::Claims{slot,slot_count:2,pass:[0,0,0,0]};
    fw.publish_timestamps(&flight,&claims).unwrap();
    assert_eq!((c0.words[0].load(Ordering::Relaxed),c0.words[1].load(Ordering::Relaxed)),(expected(0x1_0000_0070),expected(0x1_0000_0090)));
    assert_eq!((c1.words[0].load(Ordering::Relaxed),c1.words[1].load(Ordering::Relaxed)),(expected(0x2_0000_0010),expected(0x2_0000_0040)));
    // 26B123 work_ticks-first path invalidates cacheable FwCoherent work lines and skips ITS WC reads.
    assert_eq!(flight.job.resources.compute[0].work.sync_calls.get(), 1);
    assert_eq!(flight.job.resources.compute[1].work.sync_calls.get(), 1);
    assert_eq!(flight.job.resources.compute[0].its.r64_calls.get(), 0);
    assert_eq!(flight.job.resources.compute[1].its.r64_calls.get(), 0);
    // When work_ticks holds no interval (0/0), fall back to ITS cleanly.
    let fallback_unit = Compute {
        its: Cells::new([0x300, 0x400, 0, 0]),
        work: Cells::new([0, 0, 0, 0]),
    };
    assert_eq!(fw.compute_ticks(&fallback_unit), Ok((0x300, 0x400)));
    assert_eq!(fallback_unit.work.sync_calls.get(), 1);
    assert_eq!(fallback_unit.its.r64_calls.get(), 2);
    // 25G83 (work_ticks = None) reads ITS directly with zero work reads or syncs.
    let fw25 = firmware();
    let unit25 = Compute {
        its: Cells::new([0x500, 0x600, 0, 0]),
        work: Cells::new([0x700, 0x800, 0, 0]),
    };
    assert_eq!(fw25.compute_ticks(&unit25), Ok((0x500, 0x600)));
    assert_eq!(unit25.its.r64_calls.get(), 2);
    assert_eq!(unit25.work.r32_calls.get(), 0);
    assert_eq!(unit25.work.sync_calls.get(), 0);
}
mod r64_scope {
    use super::{Result, EINVAL};
    use std::cell::Cell;
    use std::sync::atomic::{AtomicU32, Ordering};
    const EAGAIN: i32 = -11;
    thread_local! {
        static RMB_COUNT: Cell<u32> = const { Cell::new(0) };
        static MUTATE_HOOK: Cell<Option<fn()>> = const { Cell::new(None) };
    }
    fn rmb() {
        RMB_COUNT.with(|c| c.set(c.get() + 1));
        std::sync::atomic::fence(Ordering::SeqCst);
        if let Some(hook) = MUTATE_HOOK.with(|h| h.get()) {
            hook();
        }
    }
    #[repr(C, align(8))]
    struct Buf([u8; 32]);
    struct FwObject {
        ptr: *mut u8,
        len: usize,
        read_calls: Cell<u32>,
    }
    impl FwObject {
        fn ptr(&self, offset: usize, len: usize) -> Result<*mut u8> {
            if offset.checked_add(len).is_none_or(|e| e > self.len) {
                return Err(EINVAL);
            }
            Ok(unsafe { self.ptr.add(offset) })
        }
        fn read_impl(&self, offset: usize, out: &mut [u8]) -> Result {
            self.read_calls.set(self.read_calls.get() + 1);
            self.read_prod(offset, out)
        }
        __FW_READ__
        __FW_R64__
    }
    #[test]
    fn r64_4byte_aligned_uses_word_pairs_handles_carry_and_rejects_churn() {
        let mut buf = Buf([0u8; 32]);
        let val_a: u64 = 0x1122_3344_5566_7788;
        let val_b: u64 = 0x0123_4567_89ab_cdef;
        let val_c: u64 = 0xfedc_ba98_7654_3210;
        buf.0[0..8].copy_from_slice(&val_a.to_le_bytes());
        buf.0[4 + 8..4 + 16].copy_from_slice(&val_b.to_le_bytes());
        buf.0[21..29].copy_from_slice(&val_c.to_le_bytes());
        let obj = FwObject {
            ptr: buf.0.as_mut_ptr(),
            len: buf.0.len(),
            read_calls: Cell::new(0),
        };
        // 8-byte aligned: single volatile u64 + 1 rmb, never calls byte read().
        RMB_COUNT.with(|c| c.set(0));
        assert_eq!(obj.r64(0), Ok(val_a));
        assert_eq!(obj.read_calls.get(), 0);
        assert_eq!(RMB_COUNT.with(|c| c.get()), 1);
        // 4-byte aligned (matching compute::DESCRIPTOR_COUNTER 0xe4c / COMPLETED 0xe54):
        // uses 32-bit word pairs (3 rmbs), never falls back to byte read().
        RMB_COUNT.with(|c| c.set(0));
        assert_eq!(obj.r64(12), Ok(val_b));
        assert_eq!(obj.read_calls.get(), 0);
        assert_eq!(RMB_COUNT.with(|c| c.get()), 3);
        // Unaligned (offset % 4 != 0): still uses read() double-snapshot fallback.
        assert_eq!(obj.r64(21), Ok(val_c));
        assert_eq!(obj.read_calls.get(), 2);
        // Actively mutating 4-byte-aligned receipt returns EAGAIN instead of a torn word.
        static TARGET: std::sync::atomic::AtomicPtr<u32> =
            std::sync::atomic::AtomicPtr::new(core::ptr::null_mut());
        static TICK: AtomicU32 = AtomicU32::new(0);
        fn bump() {
            let p = TARGET.load(Ordering::Relaxed);
            if !p.is_null() {
                let n = TICK.fetch_add(1, Ordering::Relaxed);
                unsafe { p.write_volatile(n) };
            }
        }
        TARGET.store(unsafe { buf.0.as_mut_ptr().add(12).cast::<u32>() }, Ordering::Relaxed);
        MUTATE_HOOK.with(|h| h.set(Some(bump)));
        assert_eq!(obj.r64(12), Err(EAGAIN));
        MUTATE_HOOK.with(|h| h.set(None));
        TARGET.store(core::ptr::null_mut(), Ordering::Relaxed);
        // Torn carry-wrap snapshot recovery: snapshot 1 sees LOW_WRAP, snapshot 2 sees torn
        // (0xffff_ffff, 1), snapshots 3 & 4 see settled HIGH_WRAP -> returns HIGH_WRAP.
        const LOW_WRAP: u64 = 0x0000_0000_ffff_ffff;
        const HIGH_WRAP: u64 = 0x0000_0001_0000_0000;
        static WRAP_PTR: std::sync::atomic::AtomicPtr<u32> =
            std::sync::atomic::AtomicPtr::new(core::ptr::null_mut());
        static WRAP_STEP: AtomicU32 = AtomicU32::new(0);
        fn step_wrap() {
            let p = WRAP_PTR.load(Ordering::Relaxed);
            if p.is_null() {
                return;
            }
            match WRAP_STEP.fetch_add(1, Ordering::Relaxed) {
                0 => {
                    unsafe {
                        p.write_volatile(LOW_WRAP as u32);
                        p.add(1).write_volatile((LOW_WRAP >> 32) as u32);
                    }
                }
                1 => {
                    // Torn intermediate across the 32-bit carry boundary.
                    unsafe {
                        p.write_volatile(0xffff_ffff);
                        p.add(1).write_volatile(1);
                    }
                }
                _ => {
                    unsafe {
                        p.write_volatile(HIGH_WRAP as u32);
                        p.add(1).write_volatile((HIGH_WRAP >> 32) as u32);
                    }
                }
            }
        }
        WRAP_STEP.store(0, Ordering::Relaxed);
        WRAP_PTR.store(unsafe { buf.0.as_mut_ptr().add(12).cast::<u32>() }, Ordering::Relaxed);
        MUTATE_HOOK.with(|h| h.set(Some(step_wrap)));
        assert_eq!(obj.r64(12), Ok(HIGH_WRAP));
        MUTATE_HOOK.with(|h| h.set(None));
        WRAP_PTR.store(core::ptr::null_mut(), Ordering::Relaxed);
        assert_eq!(obj.read_calls.get(), 2);
    }
}
'''.replace('__CHECKED_OFFSET__', checked_offset).replace('__STORE__', store).replace('__VMAP_STORE__', vmap_store.replace('.timestamp_lock.lock()', '.timestamp_lock.lock().unwrap()')).replace('__CACHE_POLICY__', cache_policy).replace('__WRITE__', write).replace('__PUBLISH__', publish).replace('__COMPUTE_TICKS__', compute_ticks.replace('fn compute_ticks', 'pub(crate) fn compute_ticks')).replace('__COMPUTE_TICKS_WITH__', compute_ticks_with).replace('__RETIRE__', retire.replace('fn publish_timestamps', 'pub(crate) fn publish_timestamps')).replace('__FW_READ__', fw_read.replace('pub(crate) fn read(', 'fn read_prod(')).replace('__FW_R64__', fw_r64.replace('self.read(', 'self.read_impl('))
path = out / 'timestamps-host.rs'
path.write_text(source)
subprocess.run(rustc() + ['--edition=2021', '--test', str(path), '-o', str(out / 'timestamps-host')], check=True)
subprocess.run([str(out / 'timestamps-host')], check=True)
# The hardware retirement method is the authority; never publish an aborted/error result.
retirement = function(poll, 'pub(crate) fn retire_oldest(')
assert retirement.index('self.slots.retire(') < retirement.index('self.publish_timestamps(') < retirement.index('f.job.command_retired(')
assert 'if result.is_ok() {\n            if let Err(error) = self.publish_timestamps' in retirement
print('Actual shared stores/conversion and G16 publisher passed; retirement/fence integration still requires hardware tests')
