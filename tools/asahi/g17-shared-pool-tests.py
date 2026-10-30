#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile production pressure reclaim with mock firmware/mapping boundaries.

The production eligibility loop must retain backing until release consumption,
exclude every borrower and defer mapping destruction until outside the lock.
"""
from pathlib import Path
import os
import re
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


def fixture():
    method = item((ROOT / 'runtime/render.rs').read_text(), 'pub(super) fn reclaim_idle_render_pools(').replace('pub(super) ', '')
    return r'''
#![allow(dead_code)]
use std::sync::{Arc, Mutex, atomic::{AtomicUsize, Ordering}};
type Result<T=()> = std::result::Result<T,i32>;
const EIO:i32=-5;
mod g17 {pub mod initdata {pub struct InitData;}}
struct Backing(Arc<AtomicUsize>);
impl Backing {fn new(used:&Arc<AtomicUsize>)->Self {used.fetch_add(1,Ordering::SeqCst);Self(used.clone())}}
impl Drop for Backing {fn drop(&mut self) {self.0.fetch_sub(1,Ordering::SeqCst);}}
struct Pool {id:u16, backing:Mutex<Option<Backing>>}
impl Pool {
 fn id(&self)->u16 {self.id}
 fn control_va(&self)->u64 {0}
 fn vacant(&self)->bool {self.backing.lock().unwrap().is_none()}
 fn vacate(&self)->Option<Backing> {self.backing.lock().unwrap().take()}
}
struct Context;
struct Pair {idle:Result<bool>}
impl Pair {fn context(&self)->&Context {&Context} fn idle_for_context(&self,_:&Context)->Result<bool>{self.idle}}
struct Reservation {owner:u64}
struct Entry {reservation:Reservation,pool:Option<Arc<Pool>>,pair:Option<Pair>,constructing:bool,growing:bool,preparing:Option<()>,pending_error:Option<()>,pending_timeout:bool}
struct Pools {consumed:bool,registered:bool}
impl Pools {fn observe(&mut self,_:&g17::initdata::InitData)->Result{Ok(())} fn released(&self,_:u32,_:u64)->bool{!self.registered||self.consumed}}
struct Render {entries:Vec<Option<Entry>>,pools:Pools}
struct Admission {active:bool}
impl Admission {fn owner_active(&self,_:u64)->bool {self.active}}
struct Registry {render:Render,admission:Admission}
struct DeferredBatch {backing:Vec<Backing>,cap:usize}
impl DeferredBatch {
 fn render_backing_room(&self)->bool{self.backing.len()<self.cap}
 fn defer_render_backing(&mut self,b:Backing){self.backing.push(b)}
 fn finish(&mut self){self.backing.clear()}
}
impl Registry {
''' + method + r'''
}
fn state(used:&Arc<AtomicUsize>,count:usize)->Registry {
 let entries=(0..count).map(|id|Some(Entry {
  reservation:Reservation{owner:id as u64+1},
  pool:Some(Arc::new(Pool{id:id as u16,backing:Mutex::new(Some(Backing::new(used)))})),
  pair:Some(Pair{idle:Ok(true)}),constructing:false,growing:false,preparing:None,pending_error:None,pending_timeout:false,
 })).collect();
 Registry {render:Render{entries,pools:Pools{consumed:true,registered:true}},admission:Admission{active:false}}
}
fn main(){
 let used=Arc::new(AtomicUsize::new(0));
 for guard in 0..10 {
  let mut registry=state(&used,1);
  let mut deferred=DeferredBatch{backing:vec![],cap:4};
  match guard {
   0=>registry.render.pools.consumed=false,
   1=>registry.admission.active=true,
   2=>registry.render.entries[0].as_mut().unwrap().constructing=true,
   3=>registry.render.entries[0].as_mut().unwrap().growing=true,
   4=>registry.render.entries[0].as_mut().unwrap().preparing=Some(()),
   5=>registry.render.entries[0].as_mut().unwrap().pending_error=Some(()),
   6=>registry.render.entries[0].as_mut().unwrap().pending_timeout=true,
   7=>registry.render.entries[0].as_mut().unwrap().pair.as_mut().unwrap().idle=Ok(false),
   8=>deferred.cap=0,
   9=>registry.render.entries.push(Some(Entry{reservation:Reservation{owner:1},pool:None,pair:None,constructing:true,growing:false,preparing:None,pending_error:None,pending_timeout:false})),
   _=>unreachable!(),
  }
  registry.reclaim_idle_render_pools(&g17::initdata::InitData,&mut deferred).unwrap();
  assert!(deferred.backing.is_empty(),"guard {} must retain pool backing",guard);
  assert!(!registry.render.entries[0].as_ref().unwrap().pool.as_ref().unwrap().vacant());
  assert_eq!(used.load(Ordering::SeqCst),1);
  drop(registry);assert_eq!(used.load(Ordering::SeqCst),0);
 }
 for _ in 0..512 {
  let mut registry=state(&used,4);
  let mut deferred=DeferredBatch{backing:vec![],cap:4};
  registry.reclaim_idle_render_pools(&g17::initdata::InitData,&mut deferred).unwrap();
  assert_eq!(deferred.backing.len(),4);
  assert_eq!(used.load(Ordering::SeqCst),4,"mapping destruction must remain off-lock");
  registry.reclaim_idle_render_pools(&g17::initdata::InitData,&mut deferred).unwrap();
  assert_eq!(deferred.backing.len(),4,"vacant pools cannot double-return");
  deferred.finish();assert_eq!(used.load(Ordering::SeqCst),0);
 }
 let mut unregistered=state(&used,1);
 unregistered.render.pools.registered=false;unregistered.render.pools.consumed=false;
 let mut deferred=DeferredBatch{backing:vec![],cap:4};
 unregistered.reclaim_idle_render_pools(&g17::initdata::InitData,&mut deferred).unwrap();
 assert_eq!(deferred.backing.len(),1,"never-registered backing has no firmware reference");
 deferred.finish();assert_eq!(used.load(Ordering::SeqCst),0);
 println!("PASS 512 four-client pool-sharing cycles and ten witness/borrower/collector guards; backing returns to baseline off-lock");
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
    mutations = [('fixed', source),
                 ('ignore-release-consumption', mutate_once(source, '!render.pools.released(u32::from(pool.id()), pool.control_va())', 'false')),
                 ('ignore-admission-borrower', mutate_once(source, 'self.admission.owner_active(entry.reservation.owner)', 'false')),
                 ('ignore-constructor', mutate_once(source, 'other.constructing', 'false'))]
    with tempfile.TemporaryDirectory(prefix='g17-shared-pools-') as tmp:
        for name, code in mutations:
            path = Path(tmp) / (name + '.rs')
            binary = path.with_suffix('')
            path.write_text(code)
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'fixed':
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and 'must retain pool backing' in result.stderr, result.stderr
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
