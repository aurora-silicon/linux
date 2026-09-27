#!/usr/bin/env python3
"""Run actual shared retirement counters against a deterministic host clock."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/agx_host_progress.rs').read_text()
body=s[s.index('pub(crate) struct Progress'):]
harness=r'''
use std::sync::{Arc,atomic::{AtomicU64,AtomicBool,Ordering}};
struct Monotonic;
impl Monotonic {fn ktime_get()->u64{static T:AtomicU64=AtomicU64::new(1);T.fetch_add(1,Ordering::SeqCst)}}
/*BODY*/
fn main(){
 let p=Arc::new(Progress::new());let generation=p.snapshot().0;
 assert!(generation>0);assert_eq!(p.snapshot(),(generation,0,0));
 let q=Progress::new();assert_ne!(q.snapshot().0,generation);
 let done=Arc::new(AtomicBool::new(false));let writer=p.clone();let finished=done.clone();
 let thread=std::thread::spawn(move||{for _ in 0..100000 {writer.record_completion();}finished.store(true,Ordering::Release);});
 let(mut count,mut timestamp)=(0,0);
 while !done.load(Ordering::Acquire){
  let(g,c,t)=p.snapshot();assert_eq!(g,generation);assert!(c>=count);assert!(t>=timestamp);
  if c>0{assert!(t>=generation+c,"new count exposed before its timestamp");}
  count=c;timestamp=t;
 }
 thread.join().unwrap();assert_eq!(p.snapshot().1,100000);
 println!("PASS generation, idle baseline and concurrent monotonic retirement snapshots");
}
'''.replace('/*BODY*/',body)
with tempfile.TemporaryDirectory(prefix='host-progress-') as tmp:
 p=Path(tmp)/'test.rs';p.write_text(harness);exe=p.with_suffix('')
 subprocess.run(['rustc','--edition=2021',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
