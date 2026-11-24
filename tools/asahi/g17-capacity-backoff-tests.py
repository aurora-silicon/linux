#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production render capacity wait against a deterministic clock.

Storm: every reclaim reports a return and notifies the render domain, and the
refusal never clears (the starvation case). Quiet: nothing is returned and
nothing notifies. Release: capacity appears after 5 ms. Each attempt costs
200 ns of fake CPU time. This tests the retry schedule, not firmware.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def const(source, name):
    start = source.index('const ' + name)
    return source[start:source.index(';', start) + 1]


def fixture(source):
    parts = [const(source, 'CAPACITY_PAUSE_MIN_NS'), const(source, 'CAPACITY_PAUSE_MAX_NS'),
             '#[derive(Default)]\n' + item(source, 'struct WaitClock'), item(source, 'impl WaitClock'),
             'impl Backend {\n' + item(source, 'fn wait_render_capacity(') + '\n}']
    code = '\n'.join(parts).replace('super::super::', 'rt::').replace('crate::g17::preparation::', 'rt::')
    return r'''
#![allow(dead_code)]
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
type Result<T=()> = std::result::Result<T,i32>;
const EBUSY:i32=-16; const FALLBACK:i32=-115;
static NOW:AtomicU64=AtomicU64::new(0);
static GEN:AtomicU64=AtomicU64::new(0);
static ATTEMPTS:AtomicUsize=AtomicUsize::new(0);
static MODE:AtomicUsize=AtomicUsize::new(0);
const STORM:usize=0; const QUIET:usize=1; const RELEASE:usize=2;
const RELEASE_AT:u64=5_000_000;
fn recovery_wait_ns()->u64 {10_000_000_000}
mod rt {pub fn now_ns()->u64 {crate::NOW.load(crate::Ordering::SeqCst)} pub const WORK_RING_WAIT_NS:u64=2_000_000_000; pub const QUEUE_WAIT_NS:u64=30_000_000_000;}
struct Delta(i64);
impl Delta {fn from_nanos(n:i64)->Self {Delta(n)}}
fn fsleep(d:Delta) {assert!(d.0>0); NOW.fetch_add(d.0 as u64,Ordering::SeqCst);}
struct Renders;
impl Renders {
 fn observe(&self)->u64 {GEN.load(Ordering::SeqCst)}
 fn block(&self,sample:u64,ms:u32,_:bool)->Result {
  assert!(ms>=1 && ms<=10);
  if GEN.load(Ordering::SeqCst)!=sample {return Ok(());}
  let now=NOW.load(Ordering::SeqCst); let mut until=now+u64::from(ms)*1_000_000;
  if MODE.load(Ordering::SeqCst)==RELEASE && now<RELEASE_AT {until=until.min(RELEASE_AT); GEN.fetch_add(1,Ordering::SeqCst);}
  NOW.store(until,Ordering::SeqCst); Ok(())
 }
}
struct Shared {renders:Renders}
impl Shared {fn queue_events(&self) {}}
struct Backend {shared:Shared}
impl Backend {
 fn reclaim_render_capacity(&self)->Result<bool> {
  if MODE.load(Ordering::SeqCst)==STORM {GEN.fetch_add(1,Ordering::SeqCst); return Ok(true);}
  Ok(false)
 }
 fn admission_open(&self)->Result {Ok(())}
 fn wait_deadline(&self,clock:&mut WaitClock,bound:u64)->Result<u64> {Ok(clock.deadline(rt::now_ns(),false,bound))}
}
''' + code + r'''
fn run(mode:usize)->(Result,u64,usize) {
 NOW.store(0,Ordering::SeqCst); GEN.store(0,Ordering::SeqCst); ATTEMPTS.store(0,Ordering::SeqCst); MODE.store(mode,Ordering::SeqCst);
 let b=Backend{shared:Shared{renders:Renders}};
 let (mut observed,mut clock)=(None,WaitClock::default());
 // The wait's own bound: QUEUE_WAIT_NS from its start, at most the recovery wait.
 let deadline=rt::QUEUE_WAIT_NS.min(recovery_wait_ns());
 let result=loop {
  // One refused placement attempt of the caller's loop.
  NOW.fetch_add(200,Ordering::SeqCst);
  assert!(ATTEMPTS.fetch_add(1,Ordering::SeqCst)<200_000,"capacity wait spins");
  if rt::now_ns()>=deadline {break Err(EBUSY);}
  if mode==RELEASE && rt::now_ns()>=RELEASE_AT {break Ok(());}
  if let Err(e)=b.wait_render_capacity(&mut observed,&mut clock,true) {break Err(e);}
 };
 (result,rt::now_ns(),ATTEMPTS.load(Ordering::SeqCst))
}
fn main() {
 let bound=rt::QUEUE_WAIT_NS.min(recovery_wait_ns());
 let (r,t,n)=run(STORM);
 assert_eq!(r,Err(EBUSY)); assert!(t>=bound && t<bound+3_000_000,"storm ends at {t}"); assert!(n<=16_000,"storm attempts {n}");
 println!("storm: EBUSY at {:.3} s after {} attempts",t as f64/1e9,n);
 let (r,t,n)=run(QUIET);
 assert_eq!(r,Err(EBUSY)); assert!(t>=bound && t<bound+11_000_000); assert!(n<=2_100,"quiet attempts {n}");
 println!("quiet: EBUSY at {:.3} s after {} attempts",t as f64/1e9,n);
 let (r,t,n)=run(RELEASE);
 assert_eq!(r,Ok(())); assert!(t<RELEASE_AT+CAPACITY_PAUSE_MAX_NS+10_000,"release seen at {t}");
 println!("release: placed {:.3} ms after a 5 ms release wait, {} attempts",t as f64/1e6,n);
 let mut c=WaitClock::default(); assert!(c.fast_retry()); assert!(!c.fast_retry());
 assert!(matches!(b_fallback(),Err(FALLBACK)));
 println!("PASS production render capacity wait: storm/quiet keep the wait's bound (QUEUE_WAIT_NS, at most the recovery wait) with bounded attempts; a release is seen within one pause");
}
fn b_fallback()->Result {let b=Backend{shared:Shared{renders:Renders}}; b.wait_render_capacity(&mut None,&mut WaitClock::default(),false)}
'''


def main():
    source = (ROOT / 'runtime/render/submit.rs').read_text()
    fixed = fixture(source)
    cases = [('fixed', fixed),
             ('retry-after-every-reclaim', fixed.replace('&& clock.fast_retry()', '')),
             ('no-pause-after-wake', fixed.replace('if left != 0 {', 'if false {'))]
    with tempfile.TemporaryDirectory(prefix='g17-capacity-backoff-') as tmp:
        for name, code in cases:
            assert name == 'fixed' or code != fixed, name
            path = Path(tmp) / (name.replace('-', '_') + '.rs')
            path.write_text(code)
            binary = path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '-O', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            if name == 'fixed':
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0, name
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
