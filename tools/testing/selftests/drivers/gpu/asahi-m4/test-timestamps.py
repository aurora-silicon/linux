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
cache_policy = function((backend / 'gem.rs').read_text(), 'pub(crate) fn is_writeback(')
store = function(mmu, 'pub(crate) fn write(&self, value: u64)')
vmap_store = function(mmu, 'pub(crate) fn write(&self, offset: usize, value: u64)')
write = function(shared, 'fn write(&self, ticks: u64, frequency_hz: u64)')
checked_offset = function(shared, 'fn checked_offset(')
publish = function(shared, 'pub(crate) fn publish(&self, ticks: [u64; 4], frequency_hz: u64)')
retire = function(poll, 'fn publish_timestamps(&self, flight: &Flight, claims: &super::slots::Claims)')
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
    pub mod slots{pub struct Claims{pub slot:[u8;2],pub slot_count:u32,pub pass:[u8;2]}}
    pub mod command{
        use super::*;
        pub struct Compute{pub timestamp_outputs:Destinations}
        pub struct Render{pub timestamp_outputs:Destinations}
        pub enum Command{Compute(Compute),Render(Render)}
    }
    pub struct Cells(pub [u64;4]);impl Cells{pub fn r64(&self,offset:usize)->Result<u64>{
        match offset{0=>Ok(self.0[0]),8=>Ok(self.0[1]),0x600=>Ok(self.0[0]),0x608=>Ok(self.0[1]),0x620=>Ok(self.0[2]),0x628=>Ok(self.0[3]),_=>Err(EINVAL)}
    }}
    pub struct Compute{pub its:Cells}pub struct Pass{pub scene:Cells}
    pub struct Scrub{pub compute:Vec<Compute>,pub passes:Vec<Pass>}
    pub struct Slots{pub scrub:Scrub}pub struct Firmware{pub slots:Slots}
    pub struct Resources{pub compute:Vec<Compute>,pub passes:Vec<Pass>}
    impl Resources{pub fn compute(&self)->Result<&Self>{Ok(self)}pub fn render(&self)->Result<&Self>{Ok(self)}}
    pub struct Job{pub commands:Vec<command::Command>,pub resources:Resources}
    pub struct Flight{pub job:Job,pub first:usize,pub count:usize,pub renders:u32}
    pub mod poll{use super::*;impl Firmware{ __RETIRE__ }}
}
use backend::*;
fn firmware()->Firmware{Firmware{slots:Slots{scrub:Scrub{compute:vec![],passes:vec![]}}}}
fn resources()->Resources{Resources{
    compute:vec![Compute{its:Cells([10,20,0,0])},Compute{its:Cells([30,40,0,0])}],
    passes:vec![Pass{scene:Cells([100,200,300,400])},Pass{scene:Cells([500,600,700,800])}]
}}
#[test]fn retirement_uses_claimed_hardware_cells_and_endpoint_order(){
    let compute=backing();let render=backing();
    let flight=Flight{job:Job{commands:vec![
        command::Command::Compute(command::Compute{timestamp_outputs:outputs(&compute)}),
        command::Command::Render(command::Render{timestamp_outputs:outputs(&render)})
    ],resources:resources()},first:0,count:2,renders:1};
    let claims=slots::Claims{slot:[1,0],slot_count:1,pass:[1,0]};
    firmware().publish_timestamps(&flight,&claims).unwrap();
    assert_eq!(compute.words[0].load(Ordering::Relaxed),expected(30));
    assert_eq!(compute.words[1].load(Ordering::Relaxed),expected(40));
    for (index,ticks) in [500,600,700,800].into_iter().enumerate(){
        assert_eq!(render.words[index].load(Ordering::Relaxed),expected(ticks));
    }
}
#[test]fn invalid_command_span_and_missing_claim_are_rejected(){
    let memory=backing();let mut flight=Flight{job:Job{commands:vec![
        command::Command::Compute(command::Compute{timestamp_outputs:outputs(&memory)})
    ],resources:resources()},first:usize::MAX,count:2,renders:0};
    let mut claims=slots::Claims{slot:[0,0],slot_count:1,pass:[0,0]};
    assert_eq!(firmware().publish_timestamps(&flight,&claims),Err(EOVERFLOW));
    flight.first=1;assert_eq!(firmware().publish_timestamps(&flight,&claims),Err(EINVAL));
    flight.first=0;flight.count=1;claims.slot_count=0;
    assert_eq!(firmware().publish_timestamps(&flight,&claims),Err(EINVAL));
    assert_eq!(memory.words[0].load(Ordering::Relaxed),u64::MAX);
}
'''.replace('__CHECKED_OFFSET__', checked_offset).replace('__STORE__', store).replace('__VMAP_STORE__', vmap_store.replace('.timestamp_lock.lock()', '.timestamp_lock.lock().unwrap()')).replace('__CACHE_POLICY__', cache_policy).replace('__WRITE__', write).replace('__PUBLISH__', publish).replace('__RETIRE__', retire.replace('fn publish_timestamps', 'pub(crate) fn publish_timestamps'))
path = out / 'timestamps-host.rs'
path.write_text(source)
subprocess.run(rustc() + ['--edition=2021', '--test', str(path), '-o', str(out / 'timestamps-host')], check=True)
subprocess.run([str(out / 'timestamps-host')], check=True)
# The hardware retirement method is the authority; never publish an aborted/error result.
retirement = function(poll, 'pub(crate) fn retire_oldest(')
assert retirement.index('self.slots.retire(') < retirement.index('self.publish_timestamps(') < retirement.index('f.job.command_retired(')
assert 'if result.is_ok() {\n            if let Err(error) = self.publish_timestamps' in retirement
print('Actual shared stores/conversion and G16 publisher passed; retirement/fence integration still requires hardware tests')
