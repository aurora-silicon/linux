#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Controls for the M3 device-control send and acknowledgement boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(s,marker):
 a=s.index(marker);b=s.index('{',a);n=1;e=b+1
 while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
PREFIX=r'''
#![allow(dead_code,unused_variables)]
use std::pin::Pin;use std::sync::Mutex;use std::marker::PhantomData;
type Result<T=()> = std::result::Result<T,i32>;const ETIMEDOUT:i32=-110;const EIO:i32=-5;
static CLOCK:Mutex<i64>=Mutex::new(0);static LOG:Mutex<Vec<String>>=Mutex::new(Vec::new());
macro_rules! dev_err{($dev:expr,$($arg:tt)*)=>{LOG.lock().unwrap().push(format!($($arg)*));}}
#[derive(PartialEq,Eq,PartialOrd,Ord)]struct Delta{nanos:i64}
struct Monotonic;struct Instant<T>{nanos:i64,_p:PhantomData<T>}
impl<T> Instant<T>{fn now()->Self{Self{nanos:*CLOCK.lock().unwrap(),_p:PhantomData}}fn elapsed(&self)->Delta{Delta{nanos:*CLOCK.lock().unwrap()-self.nanos}}}
fn fsleep(d:Delta){*CLOCK.lock().unwrap()+=d.nanos;}
struct Drm;impl Drm{fn as_ref(&self)->&Self{self}}
struct Config{queue_error:i32,done_ns:Option<i64>,drain_error:i32}
impl Config{
 fn control(&mut self,_:u32)->Result<u32>{if self.queue_error!=0{Err(self.queue_error)}else{Ok(1)}}
 fn drain(&mut self,_:&Drm)->Result{if self.drain_error!=0{Err(self.drain_error)}else{Ok(())}}
 fn control_done(&mut self,n:u32)->Result<bool>{assert_eq!(n,1);Ok(self.done_ns.is_some_and(|d|*CLOCK.lock().unwrap()>=d))}
}
struct Transport{error:i32,latency:i64}
impl Transport{fn send_message(self:Pin<&mut Self>,ep:u8,msg:u64)->Result{assert_eq!((ep,msg),(0x21,0x0083000000000011));*CLOCK.lock().unwrap()+=self.latency;if self.error!=0{Err(self.error)}else{Ok(())}}}
struct State;impl State{fn healthy(&self)->bool{true}}
struct Inner{config:Config,transport:Transport,drm:Drm,state:State}
struct Runtime{inner:Box<Inner>}
fn runtime(queue:i32,send:i32,done:Option<i64>)->Runtime{
 *CLOCK.lock().unwrap()=0;LOG.lock().unwrap().clear();
 Runtime{inner:Box::new(Inner{config:Config{queue_error:queue,done_ns:done,drain_error:0},transport:Transport{error:send,latency:500_000_000},drm:Drm,state:State})}
}
'''
CONTROLS=r'''
fn main(){
 assert_eq!(Delta::from_secs(2).as_nanos(),2_000_000_000);assert_eq!(Delta::from_millis(1).as_nanos(),1_000_000);
 let mut r=runtime(0,0,Some(500_000_000));assert_eq!(r.send_control(0x13),Ok(()));assert!(LOG.lock().unwrap().is_empty());
 let mut r=runtime(EIO,0,None);assert_eq!(r.send_control(0x13),Err(EIO));assert_eq!(*CLOCK.lock().unwrap(),0);assert!(LOG.lock().unwrap()[0].contains("queue failed"));
 let mut r=runtime(0,ETIMEDOUT,None);assert_eq!(r.send_control(0x13),Err(ETIMEDOUT));assert_eq!(*CLOCK.lock().unwrap(),500_000_000);
 let log=LOG.lock().unwrap()[0].clone();assert!(log.contains("mailbox send failed")&&log.contains("elapsed_ns=500000000")&&!log.contains("acknowledgement timed out"));
 let mut r=runtime(0,0,None);assert_eq!(r.send_control(9),Err(ETIMEDOUT));assert_eq!(*CLOCK.lock().unwrap(),2_500_000_000);
 let log=LOG.lock().unwrap()[0].clone();assert!(log.contains("acknowledgement timed out")&&log.contains("elapsed_ns=2000000000")&&!log.contains("mailbox send failed"));
 let mut r=runtime(0,0,Some(2_499_000_000));assert_eq!(r.send_control(9),Ok(()));assert!(LOG.lock().unwrap().is_empty());
 puts();
}
fn puts(){println!("PASS actual send_control: queue refusal, mailbox500ms versus ACK2s boundaries, elapsed nanoseconds and late positive acknowledgement");}
'''
source=(ROOT/'drivers/gpu/drm/asahi/m3_runtime.rs').read_text();time=(ROOT/'rust/kernel/time.rs').read_text();constants=(ROOT/'include/vdso/time64.h').read_text()
ns={name:int(re.search(r'#define\s+'+name+r'\s+([0-9]+)',constants)[1]) for name in ['NSEC_PER_SEC','NSEC_PER_MSEC']}
code=PREFIX+''.join(f'const {name}:i64={value};\n' for name,value in ns.items())+'impl Delta{'+function(time,'    pub const fn from_secs(')+function(time,'    pub const fn from_millis(')+function(time,'    pub const fn as_nanos(')+'}\n'
code+='impl Runtime{'+function(source,'    fn send_control(')+'}\n'+CONTROLS
with tempfile.TemporaryDirectory(prefix='m3-control-') as td:
 p=Path(td)/'controls.rs';p.write_text(code);subprocess.run(['rustc','--edition=2021',str(p),'-o',str(p.with_suffix(''))],check=True);subprocess.run([str(p.with_suffix(''))],check=True)
 mutant=code.replace('nanos: secs.saturating_mul(NSEC_PER_SEC)','nanos: secs.saturating_mul(NSEC_PER_MSEC)');assert mutant!=code;p=Path(td)/'millisecond-seconds-mutant.rs';p.write_text(mutant);subprocess.run(['rustc','--edition=2021',str(p),'-o',str(p.with_suffix(''))],check=True);r=subprocess.run([str(p.with_suffix(''))],stdout=subprocess.PIPE,stderr=subprocess.STDOUT);assert r.returncode!=0;print('PASS millisecond-scaled seconds mutant fails actual Delta unit control')
print('Production send/control and Delta conversion bodies use a virtual monotonic clock and mocked transport/rings; physical timing not measured.')
