#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""The production cleanup boundary must unmap before returning an identity."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]/'drivers/gpu/drm/asahi/g17'


def item(source,sig):
    start=source.index(sig);begin=source.index('{',start);depth=1;end=begin+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


def fixture():
    runtime=(ROOT/'runtime.rs').read_text()
    helper=item(runtime,'fn finish_uninstalled_compute(')
    # Growth must retain the empty constructor entry until the helper consumes it.
    abort=item(runtime,'fn grow_compute(')
    assert 'retired = entry.queue.take().map(|queue| (entry.reservation, queue));' in abort
    assert 'self.finish_uninstalled_compute(retired)?;' in abort
    assert 'cancel_unpublished' not in abort
    start=abort.index('                drop(state);\n                // Tentative replacement mappings')
    end=abort.index('\n            }',start)
    cleanup=abort[start:end]
    return r'''
use std::sync::{Arc,Mutex,atomic::{AtomicBool,Ordering}};
use std::ops::{Deref,DerefMut};
type Result<T=()> = std::result::Result<T,i32>;
type KBox<T> = Box<T>;
const ENODEV:i32=-19;
static MAPPED:AtomicBool=AtomicBool::new(false);
static ALIAS:AtomicBool=AtomicBool::new(false);
static PAGES:AtomicBool=AtomicBool::new(false);
static RESERVED:AtomicBool=AtomicBool::new(false);
static LOCKED:AtomicBool=AtomicBool::new(false);
#[derive(Clone,Copy)] struct ComputeReservation(u64);
struct Queue;
impl Drop for Queue {fn drop(&mut self){
 assert!(!LOCKED.load(Ordering::SeqCst),"mapping destruction under device mutex");
 assert!(RESERVED.load(Ordering::SeqCst),"identity returned before aliases disappeared");
 assert!(!ALIAS.load(Ordering::SeqCst)&&!PAGES.load(Ordering::SeqCst),"tentative aliases still live at identity return");
 MAPPED.store(false,Ordering::SeqCst);
}}
struct Tentative(&'static AtomicBool);
impl Drop for Tentative{fn drop(&mut self){assert!(!LOCKED.load(Ordering::SeqCst));self.0.store(false,Ordering::SeqCst);}}
mod compute {pub(crate) use crate::Queue;}
struct Registry;
impl Registry {fn cancel_compute(&mut self,res:ComputeReservation)->Result{
 assert_eq!(res.0,7);
 assert!(!MAPPED.load(Ordering::SeqCst),"identity returned before aliases disappeared");
 assert!(RESERVED.swap(false,Ordering::SeqCst));Ok(())
}}
struct Firmware{queues:Registry}
struct Guard<'a>(std::sync::MutexGuard<'a,Option<Box<Firmware>>>);
impl Deref for Guard<'_>{type Target=Option<Box<Firmware>>;fn deref(&self)->&Self::Target{&self.0}}
impl DerefMut for Guard<'_>{fn deref_mut(&mut self)->&mut Self::Target{&mut self.0}}
impl Drop for Guard<'_>{fn drop(&mut self){LOCKED.store(false,Ordering::SeqCst);}}
struct State(Mutex<Option<Box<Firmware>>>);
impl State{fn lock(&self)->Guard<'_>{let g=self.0.lock().unwrap();assert!(!LOCKED.swap(true,Ordering::SeqCst));Guard(g)}}
struct Available; impl Available {fn notify(&self){}}
struct Shared{state:State,channels:Available}
impl Shared{fn queue_events(&self){}}
struct Backend{shared:Arc<Shared>}
impl Backend {
''' +helper+r'''
 fn growth_cleanup(&self)->Result{
  let mut alias=Some(Tentative(&ALIAS));let mut pages=Some(Tentative(&PAGES));
  ALIAS.store(true,Ordering::SeqCst);PAGES.store(true,Ordering::SeqCst);
  let retired=Some((ComputeReservation(7),Box::new(Queue)));
  let state=self.shared.state.lock();
''' +cleanup+r'''
  Ok(())
 }
}
fn main(){
 let b=Backend{shared:Arc::new(Shared{state:State(Mutex::new(Some(Box::new(Firmware{queues:Registry})))),channels:Available})};
 b.finish_uninstalled_compute(None).unwrap();
 for _ in 0..512 {
  MAPPED.store(true,Ordering::SeqCst);RESERVED.store(true,Ordering::SeqCst);
  b.finish_uninstalled_compute(Some((ComputeReservation(7),Box::new(Queue)))).unwrap();
  assert!(!MAPPED.load(Ordering::SeqCst));assert!(!RESERVED.load(Ordering::SeqCst));
  MAPPED.store(true,Ordering::SeqCst);RESERVED.store(true,Ordering::SeqCst);
  b.growth_cleanup().unwrap();
  assert!(!MAPPED.load(Ordering::SeqCst));assert!(!RESERVED.load(Ordering::SeqCst));
 }
 println!("PASS 512 growth-frame and 512 production uninstalled cleanups: graph aliases drop off-lock while the identity remains reserved; only then can QID cancellation make it reusable");
}
'''


def main():
    source=fixture()
    cases=[('fixed',source),('cancel-before-alias-drop',source.replace('drop(queue);','core::mem::forget(queue);')),
           ('keep-tentative-growth-aliases',source.replace('drop(alias.take());','').replace('drop(pages.take());','')),
           ('drop-under-device-mutex',source.replace('        drop(queue);\n','').replace('        let mut state = self.shared.state.lock();','        let mut state = self.shared.state.lock();\n        drop(queue);'))]
    with tempfile.TemporaryDirectory(prefix='g17-uninstalled-') as tmp:
        for name,code in cases:
            path=Path(tmp)/(name+'.rs');path.write_text(code);binary=path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC','rustc'),'--edition=2021','-Awarnings',str(path),'-o',str(binary)],check=True)
            run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            if name=='fixed':
                assert run.returncode==0,run.stderr
                print(run.stdout.strip())
            else:
                assert run.returncode!=0,name
                expected={'cancel-before-alias-drop':'identity returned before aliases disappeared','keep-tentative-growth-aliases':'tentative aliases still live at identity return','drop-under-device-mutex':'mapping destruction under device mutex'}
                assert expected[name] in run.stderr,run.stderr
                print('PASS negative control:',name)
if __name__=='__main__':main()
