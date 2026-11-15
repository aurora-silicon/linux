#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Close recovery exactly between the production growth handoff and re-entry."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]/'drivers/gpu/drm/asahi/g17'


def item(text,sig):
    start=text.index(sig);begin=text.index('{',start);depth=1;end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]


def fixture():
    gate=(ROOT/'preparation.rs').read_text()
    methods='\n'.join(item(gate,sig) for sig in ['pub(super) fn try_enter(', 'pub(super) fn enter(', 'fn invalidate(', 'pub(super) fn close(', 'pub(super) fn wait_drained(', 'pub(super) fn reopen(']).replace('pub(super) ','')
    handoff=re.search(r'Rebind::Grow => \{([^}]+)\}',(ROOT/'runtime.rs').read_text()).group(1)
    return r'''
use std::sync::{Arc,Mutex as StdMutex,Condvar as StdCondvar,atomic::{AtomicU64,Ordering},mpsc};
type Result<T=()>=std::result::Result<T,i32>;
const ENODEV:i32=-19;const EAGAIN:i32=-11;const ENOSPC:i32=-28;const ERESTARTSYS:i32=-512;
const COUNT_BITS:u32=20;const COUNT_MASK:u64=(1<<COUNT_BITS)-1;
const CLOSED:u64=1<<COUNT_BITS;const REMOVED:u64=CLOSED<<1;const FAILED:u64=CLOSED<<2;const EPOCH:u64=CLOSED<<3;
struct Guard<'a>(Option<std::sync::MutexGuard<'a,()>>);
struct Mutex(StdMutex<()>);
impl Mutex{fn lock(&self)->Guard<'_>{Guard(Some(self.0.lock().unwrap()))}}
struct CondVar(StdCondvar);
impl CondVar {
 fn wait(&self,g:&mut Guard<'_>){g.0=Some(self.0.wait(g.0.take().unwrap()).unwrap());}
 fn wait_interruptible(&self,g:&mut Guard<'_>)->bool{self.wait(g);false}
 fn notify_all(&self){self.0.notify_all();}
}
struct Gate{state:AtomicU64,wait:Mutex,changed:CondVar}
struct Lease{gate:Arc<Gate>,epoch:u64}
impl Gate {
 fn wake_waiters(&self) {}
''' + methods + '\n}\n' + item(gate,'impl Drop for Lease') + r'''
struct Backend {gate:Arc<Gate>,ready:mpsc::Sender<()>,proceed:StdMutex<mpsc::Receiver<()>>}
impl Backend {
 fn handoff(&self,required:usize)->Result{
  let lease=self.gate.try_enter()?;let state=();let admission=&();
''' + handoff + r'''
 }
 fn grow_compute(&self,_:usize,_:&())->Result {
  self.ready.send(()).unwrap();self.proceed.lock().unwrap().recv().unwrap();
  let _growth=self.gate.enter(false)?;
  Ok(())
 }
}
fn main(){for _ in 0..128 {
 let gate=Arc::new(Gate{state:AtomicU64::new(EPOCH),wait:Mutex(StdMutex::new(())),changed:CondVar(StdCondvar::new())});
 let (ready_tx,ready_rx)=mpsc::channel();let (go_tx,go_rx)=mpsc::channel();
 let b=Backend{gate:gate.clone(),ready:ready_tx,proceed:StdMutex::new(go_rx)};
 let builder=std::thread::spawn(move||b.handoff(40));
 ready_rx.recv().unwrap();gate.close();
 assert_eq!(gate.state.load(Ordering::Acquire)&COUNT_MASK,0,"outer growth lease blocks recovery join");
 assert!(matches!(gate.try_enter(),Err(EAGAIN)));
 go_tx.send(()).unwrap();
 let recovery=std::thread::spawn(move||{gate.wait_drained();assert!(gate.reopen());});
 recovery.join().unwrap();builder.join().unwrap().unwrap();
}
 println!("PASS 128 concurrent production growth handoffs: recovery closes between entries, joins builder, reopens and growth completes");}
'''


def main():
    source=fixture();assert 'drop(lease);' in source
    with tempfile.TemporaryDirectory(prefix='g17-growth-gate-') as tmp:
        for name,code in [('fixed',source),('retain-outer-lease',source.replace('drop(lease);',''))]:
            path=Path(tmp)/(name+'.rs');path.write_text(code);binary=path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC','rustc'),'--edition=2021','-Awarnings',str(path),'-o',str(binary)],check=True)
            run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            if name=='fixed':
                assert run.returncode==0,run.stderr
                print(run.stdout.strip())
            else:
                assert run.returncode!=0 and 'outer growth lease' in run.stderr,run.stderr
                print('PASS negative control:',name)
if __name__=='__main__':main()
