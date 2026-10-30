#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Inject a nonfatal first-submit error while all ten logical owners stay open."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]/'drivers/gpu/drm/asahi/g17'
spec=importlib.util.spec_from_file_location('fault',Path(__file__).with_name('g17-capacity-fault-tests.py'))
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)


def fixture():
    text=adapter.fixture().split('fn main() {')[0]
    method=adapter.item((ROOT/'queue/compute.rs').read_text(),'pub(crate) fn rollback_unpublished(')
    finish=adapter.item((ROOT/'runtime/backend.rs').read_text(),'pub(super) fn finish_publication(').replace('pub(super) ','')
    return text+'\nimpl Queue {\n'+method+'\n}\n'+r'''
const ETIMEDOUT:i32=-110;const ENODEV:i32=-19;const ENOMEM:i32=-12;
type Fence=u8;
#[derive(PartialEq)] enum Engine{Compute}
struct Status;
impl Status {fn get(&self)->i32{0}}
impl Completion {fn status(&self)->Status{Status} fn fail_unpublished(&self,_:i32){} fn scheduler_fence(&self)->Fence{0}}
impl Packet {fn is_published(&self)->bool{false} fn engine(&self)->Engine{Engine::Compute}}
struct DeferredBatch;
impl DeferredBatch {fn finish(&mut self){}}
mod job {pub trait Backend{fn fail_vm(&self,error:i32);}}
struct Backend {q:std::cell::RefCell<Queue>,owner:u64,context:Arc<Context>}
impl job::Backend for Backend {fn fail_vm(&self,_:i32){panic!("nonfatal first submit must leave logical owner open")}}
impl Backend {
 fn stop_after_error(&self){panic!("rollback must preserve healthy device")}
 fn rollback_unpublished_compute(&self)->Result {
  let (cancel,binding)={self.q.borrow_mut().rollback_unpublished(self.owner,&self.context)?};
  assert!(!cancel);drop(binding);Ok(())
 }
''' + finish + r'''
}
fn main(){
 let live=Arc::new(AtomicUsize::new(0));
 for predecessor in 0..3 {
  let mut channels=Vec::new();
  for owner in 1..=10 {
   let mut q=idle(&live);q.owner=Some(owner);q.unpublished_binding=true;
   let context=q.binding.as_ref().unwrap().context.clone();
   if predecessor!=0 {
    let old=Context::new(100+owner,&live);
    q.previous=Some(Previous{binding:if predecessor==1{Some(Binding{context:old.clone()})}else{None},context:old,publication:None});
   }
   channels.push(Backend{q:std::cell::RefCell::new(q),owner,context});
  }
  let packet=Arc::new(Packet{completion:Arc::new(Completion)});
  for b in &channels {
   assert_eq!(b.finish_publication(&packet,Err(ENOMEM),&mut DeferredBatch),Err(ENOMEM));
   let q=b.q.borrow();assert!(q.reusable(),"failed open-owner first submit must return channel");
   assert!(q.owner.is_none());assert!(q.previous.is_none());assert_eq!(q.graph.resets,0);
  }
  drop(channels);assert_eq!(live.load(Ordering::SeqCst),0);
 }
 for guard in 0..13 {
  let mut q=idle(&live);q.unpublished_binding=true;
  let context=q.binding.as_ref().unwrap().context.clone();let mut owner=2;let mut ctx=context.clone();
  match guard {
   0=>q.unpublished_binding=false,
   1=>q.exit=Some(Exit{phase:Phase::Acknowledged}),
   2=>q.active.push(Active{packet:Arc::new(Packet{completion:Arc::new(Completion)}),ticket:None,qos_pending:false}).unwrap(),
   3=>q.replays.push(Arc::new(Packet{completion:Arc::new(Completion)})).unwrap(),
   4=>q.growing=true,
   5=>q.quarantined=true,
   6=>q.awaiting_witness=true,
   7=>q.previous=Some(Previous{context:context.clone(),binding:None,publication:Some((1,1))}),
   8=>q.pool_phase=PoolPhase::ReleaseSent(3),
   9=>owner=3,
   10=>ctx=Context::new(4,&live),
   11=>q.retire_pending=true,
   12=>q.failure_status_pending=true,
   _=>unreachable!(),
  }
  let (cancel,binding)=q.rollback_unpublished(owner,&ctx).unwrap();
  assert!(!cancel);assert!(binding.is_none());assert_eq!(q.owner,Some(2),"guard {} must retain physical owner",guard);
 }
 let mut q=idle(&live);q.installed=false;q.unpublished_binding=true;
 let context=q.binding.as_ref().unwrap().context.clone();
 assert!(q.rollback_unpublished(2,&context).unwrap().0,"never-published graph can cancel only its unpublished QID");
 println!("PASS production failure completion returns all ten open-owner channels for detached/same-VM/rebound predecessors; 13 published/kill/work/owner/epoch guards retain ownership; installed rings never reset");
}
'''


def main():
    source=fixture()
    compute=(ROOT/'queue/compute.rs').read_text()
    assert compute.count('visible = true;\n                self.unpublished_binding = false;')==2
    backend=(ROOT/'runtime/backend.rs').read_text()
    assert 'if !lease.is_current() { drop(state); drop(lease); continue; }' in backend
    no_kill, count = re.subn(r'\|\|\s*self\.exit\.is_some\(\)\s*(?=\|\|\s*self\.active\.len\s*!=\s*0)', '', source)
    assert count == 1, count
    cases=[('fixed',source),('omit-first-failure-rollback',source.replace('self.rollback_unpublished_compute().is_err()','false')),
           ('rollback-published-binding',source.replace('|| !self.unpublished_binding','')),
           ('rollback-issued-kill',no_kill)]
    with tempfile.TemporaryDirectory(prefix='g17-unpublished-') as tmp:
        for name,code in cases:
            path=Path(tmp)/(name+'.rs');path.write_text(code);binary=path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC','rustc'),'--edition=2021','-Awarnings',str(path),'-o',str(binary)],check=True)
            run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            if name=='fixed':
                assert run.returncode==0,run.stderr
                print(run.stdout.strip())
            else:
                assert run.returncode!=0,name
                print('PASS negative control:',name)
if __name__=='__main__':main()
