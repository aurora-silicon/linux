#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Controls for current14 T8122 GPU power acquisition and release."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]

def function(source, marker):
    begin = source.index(marker)
    brace = source.index('{', begin)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

PREFIX = r'''
#![allow(dead_code,unused_variables,static_mut_refs)]
use std::sync::Mutex;
type Result<T=()> = std::result::Result<T,i32>;
const ENODEV:i32=-19; const ENOTSUPP:i32=-524;const EINVAL:i32=-22; const EBUSY:i32=-16;
const EPROBE_DEFER:i32=-517;const EIO:i32=-5;
const ASC_CPU_CONTROL:usize=0x44;const ASC_CPU_RUN:u32=16;const GFP_KERNEL:u32=0;
macro_rules! c_str { ($s:literal) => {$s}; }
macro_rules! dev_info { ($($args:tt)*) => {()}; }
macro_rules! dev_warn { ($($args:tt)*) => {()}; }
macro_rules! dev_err { ($($args:tt)*) => {()}; }
static TRACE:Mutex<Vec<&'static str>>=Mutex::new(Vec::new());
static mut LINKED:bool=true;static mut LINK_ERROR:i32=0;static mut READY_ERROR:i32=0;
static mut COMMAND_ERROR:i32=0;static mut STOPPED:bool=true;static mut MAP_ACCESS:bool=true;
static mut VALID_ID:bool=true;static mut LIVE_ON_ERROR:bool=false;
fn record(s:&'static str){TRACE.lock().unwrap().push(s);}
fn to_result(v:i32)->Result {if v==0 {Ok(())}else{Err(v)}}
mod bindings {
    pub unsafe fn apple_pmp_link_device(_: *mut ())->i32 {super::record("link");super::LINK_ERROR}
    pub unsafe fn apple_pmp_set_device_power(op:u8,id:u16,on:u32)->i32 {
        assert_eq!((op,id),(0xf,5));super::record("ready");
        if super::READY_ERROR!=0{return super::READY_ERROR;}
        super::record(if on==1{"on"}else{"off"});if on==1&&super::COMMAND_ERROR!=0&&super::LIVE_ON_ERROR{super::STOPPED=false;}super::COMMAND_ERROR
    }
}
struct Core;
mod platform {
    pub struct Device<T>(pub std::marker::PhantomData<T>);
    impl<T> Device<T> {
        pub fn as_ref(&self)->&Self{self}
        pub fn as_raw(&self)->*mut (){std::ptr::null_mut()}
        pub fn io_request_by_name(&self,name:&str)->Option<super::Mapping>{Some(super::Mapping{asc:name=="asc"})}
    }
}
#[derive(Clone,Copy)] struct Mapping{asc:bool}
impl Mapping {
    fn iomap(self)->Self{self}
    fn iomap_sized<const N:usize>(self)->Self{self}
    fn access<T>(&self,_:&T)->Result<&Self>{if unsafe{MAP_ACCESS}{Ok(self)}else{Err(ENODEV)}}
    fn try_access(&self)->Option<&Self>{if unsafe{MAP_ACCESS}{Some(self)}else{None}}
    fn read32(&self,_:usize)->u32{if unsafe{STOPPED}{0}else{ASC_CPU_RUN}}
    fn try_read32(&self,offset:usize)->Result<u32>{
        record("id");if !unsafe{VALID_ID}{return Err(EIO);}
        Ok(match offset{0xd04000=>0x07022000,0xd04010=>0x0011010a,0xe01500=>0x3ff,_=>panic!()})
    }
    fn try_write32(&self,_:u32,_:usize)->Result{record("setup");Ok(())}
}
struct KBox;
impl KBox {fn pin_init(m:Mapping,_:u32)->Result<Mapping>{Ok(m)}}
struct Firmware;
impl Firmware{fn version(&self)->&str{"RTKit-2419.140.12.release"}}
#[derive(Clone,Copy)] struct Id{version:u32,version_mask:u32,counts:u32,counts_mask:u32}
struct Soc{chip_id:u32,gpu_name:&'static str,id:Id,sgx_setup:Option<(usize,u32)>}
static T8122:Soc=Soc{chip_id:0x8122,gpu_name:"G15G",id:Id{version:0x07022000,version_mask:!0,counts:0x0011010a,counts_mask:!0},sgx_setup:Some((0xd14000,0x70001))};
static T6030:Soc=Soc{chip_id:0x6030,..T8122};
mod t8122_start {pub struct Experiment; impl Experiment{pub fn sgx_setup(&self)->Option<(usize,u32)>{Some((0xd14000,0x70001))}}pub fn is_t8122(s:&super::Soc)->bool{s.chip_id==0x8122}}
mod m3_board {pub fn has_pmp_link<T>(_:&super::platform::Device<T>)->bool{unsafe{super::LINKED}}pub fn core_mask_valid(_:&super::Soc,m:u32)->bool{m==0x3ff}}
struct DevRef;
impl<T> From<&platform::Device<T>> for DevRef{fn from(_:&platform::Device<T>)->Self{Self}}
impl DevRef{fn as_ref(&self)->&Self{self}}
struct Device{dev:DevRef,asc:Mapping,sgx:Mapping,firmware:Firmware,core_mask:u32,soc:&'static Soc,power_vote:bool}
'''
CONTROLS = r'''
fn reset(){TRACE.lock().unwrap().clear();unsafe{LINKED=true;LINK_ERROR=0;READY_ERROR=0;COMMAND_ERROR=0;STOPPED=true;MAP_ACCESS=true;VALID_ID=true;LIVE_ON_ERROR=false;}}
fn trace()->Vec<&'static str>{TRACE.lock().unwrap().clone()}
fn main(){
    let pdev=platform::Device::<Core>(std::marker::PhantomData);
    reset();let d=Device::new(&pdev,Firmware,&T8122,None).unwrap();
    assert_eq!(trace(),vec!["link","ready","on","id","id","id","setup"]);drop(d);
    assert_eq!(&trace()[7..],&["ready","off"]);
    reset();unsafe{LINKED=false;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(ENODEV)));assert!(trace().is_empty());
    reset();unsafe{LINK_ERROR=EPROBE_DEFER;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(EPROBE_DEFER)));assert_eq!(trace(),vec!["link"]);
    reset();unsafe{READY_ERROR=EPROBE_DEFER;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(EPROBE_DEFER)));assert_eq!(trace(),vec!["link","ready","ready"]);
    reset();unsafe{READY_ERROR=EIO;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(EIO)));assert_eq!(trace(),vec!["link","ready","ready"]);
    reset();unsafe{COMMAND_ERROR=EIO;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(EIO)));assert_eq!(trace(),vec!["link","ready","on","ready","off"]);
    reset();unsafe{COMMAND_ERROR=EIO;LIVE_ON_ERROR=true;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(EIO)));assert_eq!(trace(),vec!["link","ready","on"]);
    reset();unsafe{STOPPED=false;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(EBUSY)));assert_eq!(trace(),vec!["link"]);
    reset();unsafe{VALID_ID=false;}assert!(matches!(Device::new(&pdev,Firmware,&T8122,None),Err(EIO)));assert_eq!(trace(),vec!["link","ready","on","id","ready","off"]);
    reset();let d=Device::new(&pdev,Firmware,&T8122,None).unwrap();unsafe{STOPPED=false;}drop(d);assert!(!trace().contains(&"off"));
    reset();let d=Device::new(&pdev,Firmware,&T8122,None).unwrap();unsafe{MAP_ACCESS=false;}drop(d);assert!(!trace().contains(&"off"));
    reset();assert!(matches!(Device::new(&pdev,Firmware,&T6030,None),Err(ENOTSUPP)));assert!(trace().is_empty());
    reset();unsafe{LINKED=false;}let d=Device::new(&pdev,Firmware,&T6030,None).unwrap();drop(d);assert_eq!(trace(),vec!["id","id","id","setup"]);
    println!("PASS actual Device::new/Drop: link-ready-vote-ID-setup ordering, defer/refusal, ASC ownership and release controls; T6030 unchanged");
}
'''

def build(source, path):
    body = function(source, '    pub(crate) fn new(')
    drop = function(source, 'impl Drop for Device') if 'impl Drop for Device' in source else ''
    prefix = PREFIX if 'power_vote:' in body else PREFIX.replace(',power_vote:bool', '')
    path.write_text(prefix + 'impl Device {\n' + body + '\n}\n' + drop + CONTROLS)
    subprocess.run(['rustc','--edition=2021',str(path),'-o',str(path.with_suffix(''))],check=True)
    return subprocess.run([str(path.with_suffix(''))],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-source',type=Path)
    args=parser.parse_args()
    source=(ROOT/'drivers/gpu/drm/asahi/m3_device.rs').read_text()
    with tempfile.TemporaryDirectory(prefix='t8122-pmp-') as td:
        td=Path(td);r=build(source,td/'current.rs');print(r.stdout,end='');assert r.returncode==0
        if args.old_source:
            r=build(args.old_source.read_text(),td/'old.rs');assert r.returncode!=0;print('PASS original Device::new fails linked-T8122 positive control')
        mutant=source.replace('device.power_vote = true;','device.power_vote = false;')
        assert mutant!=source;r=build(mutant,td/'no-release.rs');assert r.returncode!=0;print('PASS lost-vote-ownership mutant fails release control')
    print('Compiled production new/drop bodies use mocked kernel, MMIO and PMP services; no physical boot claim.')
if __name__=='__main__':main()
