#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Production growth abort/commit/vacancy at controlled allocation/MMIO boundaries."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'
def item(source, signature):
    start=source.index(signature);begin=source.index('{',start);depth=1;end=begin+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


def fixture():
    source = (ROOT / 'queue/compute.rs').read_text()
    text = r'''
#![allow(dead_code)]
use std::sync::{Arc,atomic::{AtomicU32,AtomicUsize,Ordering}};
type Result<T=()> = std::result::Result<T,i32>;
const EIO:i32=-5;const EBUSY:i32=-16;const EINVAL:i32=-22;
fn now_ns()->u64{123}
struct Context {live:Arc<AtomicUsize>}
impl Context {fn note_compute_binding(&self,_:bool){} fn new(live:&Arc<AtomicUsize>)->Arc<Self>{live.fetch_add(1,Ordering::SeqCst);Arc::new(Self{live:live.clone()})}}
impl Drop for Context {fn drop(&mut self){self.live.fetch_sub(1,Ordering::SeqCst);}}
struct Binding {context:Arc<Context>,pool_alias:Option<u64>,descriptors:Option<u64>}
struct Previous {context:Arc<Context>,binding:Option<Binding>,publication:Option<(u64,u64)>}
#[derive(Clone,Copy,PartialEq)] enum PoolPhase{Populated,ReleaseSent(u32),Vacant}
struct Fifo {len:usize}
struct Graph;
impl Graph {fn set_owner(&mut self,_:&Context)->Result{Ok(())}}
struct ReaderHint; impl ReaderHint {fn has_readers(&self,count:&AtomicU32)->bool{count.load(Ordering::Acquire)!=0}}
struct Queue {
 owner:Option<u64>,installed:bool,growing:bool,growth_blocks:usize,free_list_generation:u64,
 pool_phase:PoolPhase,release_sent_ns:Option<u64>,pool:Pool,binding:Option<Binding>,previous:Option<Previous>,graph:Graph,
 active:Fifo,replays:Fifo,quarantined:bool,released:bool,retirement_ready:bool,
 retired_by_teardown:bool,released_at:u64,exit:Option<()>,needs_fresh_backing:bool,
 source_hint:ReaderHint,source_readers:AtomicU32,
}
fn idle(live:&Arc<AtomicUsize>)->Queue{Queue{owner:Some(1),installed:true,growing:false,growth_blocks:0,
 free_list_generation:0,pool_phase:PoolPhase::Populated,release_sent_ns:None,pool:Pool{pages:Some(Pages(10)),fail:false},
 binding:Some(Binding{context:Context::new(live),pool_alias:Some(1),descriptors:Some(2)}),previous:None,graph:Graph,
 active:Fifo{len:0},replays:Fifo{len:0},quarantined:false,released:false,retirement_ready:true,
 retired_by_teardown:false,released_at:0,exit:None,needs_fresh_backing:false,source_hint:ReaderHint,source_readers:AtomicU32::new(0)}}
impl Queue {
''' + item(source,'pub(crate) fn release_owner(') + item(source,'pub(crate) fn reusable(') + '\n}\n'
    methods = '\n'.join(item(source, sig) for sig in ['pub(crate) fn begin_growth(', 'pub(crate) fn finish_growth(', 'pub(crate) fn growth_token(', 'pub(crate) fn abort_growth(', 'pub(crate) fn observe_vacancy('])
    replace = item((ROOT / 'freelist.rs').read_text(), 'pub(crate) fn replace_pages(')
    observe = item((ROOT / 'runtime.rs').read_text(), 'pub(super) fn observe_pool_releases(').replace('pub(super) ', '').replace('super::initdata::', 'initdata::')
    return text + r'''
const EAGAIN:i32=-11;const EFAULT:i32=-14;
static CONSUMER:AtomicU32=AtomicU32::new(0);
static PRODUCER:AtomicU32=AtomicU32::new(3);
struct Pages(u64);
impl Pages {fn page_list_va(&self)->u64{self.0}}
mod mmu {pub type KernelMapping=u64;}
mod g17 {
 pub fn now_ns()->u64{crate::now_ns()}
 pub mod freelist {pub(crate) use crate::Pages; pub fn control_consumed(c:u32,_:u32,t:u32)->crate::Result<bool>{Ok(c==t)}}
 pub mod initdata {
  pub struct InitData {pub fail:bool}
  impl InitData {
   pub fn control_consumer(&self)->crate::Result<&crate::AtomicU32>{Ok(&crate::CONSUMER)}
   pub fn control_producer(&self)->crate::Result<&crate::AtomicU32>{Ok(&crate::PRODUCER)}
   pub fn initialize_compute_pool_blocks(&self,_:u16,_:u64,_:usize)->crate::Result{if self.fail{Err(crate::EIO)}else{Ok(())}}
  }
 }
}
use std::sync::atomic::fence;
use g17::initdata;
struct Pool {pages:Option<Pages>,fail:bool}
impl Pool {
 fn id(&self)->u16{1}
 fn release_record(&self,_:u64)->u64{0}
 fn take_pages(&mut self)->Option<Pages>{self.pages.take()}
 fn write_pages(&mut self,_:&Pages)->Result{if self.fail{Err(EIO)}else{Ok(())}}
''' + replace + r'''
}
enum ControlRecord {FreeListRelease(u64)}
struct Rings;
impl Rings {fn publish_control(_:&initdata::InitData,_:&ControlRecord)->Result<u32>{Ok(3)}}
struct Vacated {_alias:Option<u64>,_pages:Option<Pages>,_descriptors:Option<u64>,_ring:Option<u64>}
impl Graph {fn vacate_ring(&mut self)->Option<u64>{Some(1)}}
''' + '#[derive(Clone, Copy)]\n' + item(source, 'pub(crate) struct Growth') + r'''
impl Queue {
 fn qid(&self)->u8{7}
''' + methods + r'''
}
struct Entry {queue:Option<Box<Queue>>}
struct Registry {compute:Vec<Option<Entry>>}
struct DeferredBatch {vacated:Vec<Vacated>,capacity:usize}
impl DeferredBatch {
 fn vacated_room(&self)->bool{self.vacated.len()<self.capacity}
 fn defer_vacated(&mut self,v:Vacated){assert!(self.vacated_room());self.vacated.push(v)}
}
impl Registry {
''' + observe + r'''
}
fn main(){
 let live=Arc::new(AtomicUsize::new(0));let init=initdata::InitData{fail:false};
 for close_first in [false,true] {for consumed_first in [false,true] {
  let mut q=idle(&live);q.retirement_ready=true;
  assert!(q.begin_growth(&init,40).unwrap());let token=q.growth_token().unwrap();
  assert!(q.growing);assert!(q.pool_phase==PoolPhase::ReleaseSent(3));
  assert_eq!(q.release_sent_ns,Some(now_ns()),"exact publication starts the release deadline");
  assert_eq!(q.abort_growth(Growth{qid:8,phase:token.phase}).unwrap(),None,"wrong physical queue");
  assert_eq!(q.abort_growth(Growth{qid:token.qid,phase:PoolPhase::ReleaseSent(4)}).unwrap(),None,"stale release transaction");
  assert!(q.growing);
  assert!(q.begin_growth(&init,40).is_err(),"another builder cannot steal growth");
  if close_first {q.release_owner().unwrap();}
  CONSUMER.store(if consumed_first{3}else{2},Ordering::SeqCst);
  assert!(q.observe_vacancy(CONSUMER.load(Ordering::SeqCst),3).unwrap().is_none(),"live builder owns growth");
  // Timeout, epoch invalidation, notify/MMIO error all run this same abort.
  assert_eq!(q.abort_growth(token).unwrap(),Some(false));
  assert!(!q.growing);assert!(q.owner.is_none());assert_eq!(q.growth_blocks,0);
  assert_eq!(q.abort_growth(token).unwrap(),None);
  assert!(q.pool.pages.is_some(),"abort must retain backing");
  assert_eq!(q.release_sent_ns,Some(now_ns()),"abort must not refresh or discard an owned deadline");
  assert!(!q.reusable(),"published release still owns backing");
  let mut registry=Registry{compute:vec![Some(Entry{queue:Some(Box::new(q))})]};
  let mut deferred=DeferredBatch{vacated:vec![],capacity:0};
  assert_eq!(registry.observe_pool_releases(&init,&mut deferred).unwrap(),0,"collector pressure retains mappings");
  deferred.capacity=1;
  let freed=registry.observe_pool_releases(&init,&mut deferred).unwrap();
  assert_eq!(freed,usize::from(consumed_first));
  CONSUMER.store(3,Ordering::SeqCst);
  if !consumed_first {assert_eq!(registry.observe_pool_releases(&init,&mut deferred).unwrap(),1);}
  let q=registry.compute[0].as_ref().unwrap().queue.as_ref().unwrap();
  assert!(q.pool_phase==PoolPhase::Vacant);assert!(q.reusable());
  assert_eq!(q.release_sent_ns,None,"only the consumed release clears its deadline");
  assert_eq!(deferred.vacated[0]._pages.as_ref().unwrap().0,10);
  drop(registry);drop(deferred);assert_eq!(live.load(Ordering::SeqCst),0);
 }}
 for failure in 0..2 {
  let mut q=idle(&live);q.retirement_ready=true;q.begin_growth(&init,40).unwrap();
  let token=q.growth_token().unwrap();CONSUMER.store(3,Ordering::SeqCst);
  let mut pages=Some(Pages(20));let mut alias=Some(30);
  q.pool.fail=failure==0;
  assert!(q.finish_growth(&initdata::InitData{fail:failure==1},&mut pages,&mut alias,40).is_err());
  assert_eq!(pages.as_ref().unwrap().0,20,"failed rearm preserves tentative pages");
  assert_eq!(q.pool.pages.as_ref().unwrap().0,10,"failed rearm preserves old pages");
  assert_eq!(alias,Some(30));assert_eq!(q.binding.as_ref().unwrap().pool_alias,Some(1));
  q.abort_growth(token).unwrap();assert!(q.observe_vacancy(3,3).unwrap().is_some());
 }
 let mut q=idle(&live);q.retirement_ready=true;q.begin_growth(&init,40).unwrap();
 let token=q.growth_token().unwrap();let mut pages=Some(Pages(20));let mut alias=Some(30);
 let old=q.finish_growth(&init,&mut pages,&mut alias,40).unwrap();
 assert_eq!(q.release_sent_ns,None,"committed replacement clears the witnessed release deadline");
 assert_eq!(old._pages.unwrap().0,10);assert_eq!(old._alias,Some(1));assert!(pages.is_none());assert!(alias.is_none());
 assert_eq!(q.pool.pages.as_ref().unwrap().0,20);assert_eq!(q.abort_growth(token).unwrap(),None,"committed growth cannot be aborted");
 println!("PASS production growth: abort before/after consumption and close; stale witness/collector preserve backing; failed row/header rearm preserves both owners; commit invalidates abort token");
}
'''



def mutate_once(source, old, new):
    """Match Rust tokens across rustfmt whitespace and reject a no-op mutant."""
    tokens = re.findall(r"[A-Za-z_]\w*|[0-9]+|[^\w\s]", old)
    pattern = r"(?<!\w)" + r"\s*".join(re.escape(token) for token in tokens)
    changed, count = re.subn(pattern, lambda _: new, source)
    assert count == 1, f"mutation matched {count} sites: {old}"
    return changed

def main():
    source = fixture()
    runtime = (ROOT / 'runtime.rs').read_text()
    assert 'growth = queue.growth_token();' in runtime and 'queue.abort_growth(token)' in runtime
    cases = [('fixed', source),
             ('missing-abort-transfer', mutate_once(source, 'self.growing = false;\n        self.growth_blocks = 0;\n        self.release_owner().map(Some)', 'self.release_owner().map(Some)')),
             ('ignore-consumption', mutate_once(source, 'if !crate::g17::freelist::control_consumed(consumer, producer, cursor)? {', 'if false {')),
             ('ignore-collector-pressure', mutate_once(source, 'if !deferred.vacated_room() { break; }', ''))]
    with tempfile.TemporaryDirectory(prefix='g17-growth-') as tmp:
        for name, code in cases:
            path=Path(tmp)/(name+'.rs');path.write_text(code);binary=path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC','rustc'),'--edition=2021','-Awarnings',str(path),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            if name=='fixed':
                assert result.returncode==0,result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode!=0,name
                print('PASS negative control:',name)

if __name__=='__main__':main()
