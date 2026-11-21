#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute the production M4 queue-behind rule: what may follow in-flight compute."""
from common import kernel_root, output_dir, rustc, sha
import json
import subprocess

root = kernel_root() / 'drivers/gpu/drm/asahi/g16'
engine = (root / 'engine.rs').read_text()
command = (root / 'command.rs').read_text()
params = (root / 'params.rs').read_text()
output = output_dir()


def item(source, declaration):
    start = source.index(declaration)
    at = source.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#![allow(dead_code)]
use std::sync::{Arc, atomic::{AtomicU32, Ordering}};
#[derive(Copy,Clone,Debug,PartialEq,Eq)] struct Error(i32);
const EAGAIN:Error=Error(11);
type Result<T=()> = std::result::Result<T,Error>;
struct Status(AtomicU32);impl Status{fn get(&self)->u32{self.0.load(Ordering::Relaxed)}}
struct Vm{status:Status}
struct Destinations(bool);impl Destinations{fn is_empty(&self)->bool{!self.0}}
struct ComputeCommand{timestamp_outputs:Destinations,query_access:bool}
struct RenderCommand{timestamp_outputs:Destinations}
enum Command{Compute(Box<ComputeCommand>),Render(Box<RenderCommand>)}
struct Progress{sent:u32}impl Progress{fn sent(&self)->u32{self.sent}}
struct Resources;impl Resources{fn compute_units(&self)->Result<usize>{Ok(2)}}
struct Job{vm:Arc<Vm>,progress:Progress,commands:Vec<Command>,resources:Resources,dependency:Result}
impl Job{fn check_command_dependencies(&self,_index:u32)->Result{self.dependency}}
fn command(job:&Job,k:u32)->Option<&Command>{job.commands.get(k as usize)}
struct Flight{job:Arc<Job>,first:usize,count:usize,renders:u32,doomed:bool,recovery_suspect:bool}
struct Engine{flights:Vec<Flight>,ordinal:u64}
struct Slots{exhausted:bool,available:bool}
impl Slots{fn compute_exhausted(&self)->bool{self.exhausted}fn compute_available(&self,_units:usize)->bool{self.available}}
struct Firmware{engine:Engine,slots:Slots}
mod params{pub fn compute_flights()->usize{4}}
mod recovery{#[path="__LOGIC__"]pub mod logic;}
impl Firmware{
    __CAN_QUEUE_BEHIND__
    __WRITES_TIMESTAMPS__
}

/// A compute command, with or without user timestamps, that may (copy, reset, old userspace)
/// or may not access queries.
fn compute(timestamps:bool,query_access:bool)->Command{
    Command::Compute(Box::new(ComputeCommand{timestamp_outputs:Destinations(timestamps),query_access}))
}
fn render(timestamps:bool)->Command{Command::Render(Box::new(RenderCommand{timestamp_outputs:Destinations(timestamps)}))}
fn vm()->Arc<Vm>{Arc::new(Vm{status:Status(AtomicU32::new(0))})}
fn job(vm:&Arc<Vm>,commands:Vec<Command>,sent:u32)->Arc<Job>{
    Arc::new(Job{vm:vm.clone(),progress:Progress{sent},commands,resources:Resources,dependency:Ok(())})
}
fn flight(job:&Arc<Job>,index:usize)->Flight{
    let renders=matches!(job.commands[index],Command::Render(_)) as u32;
    Flight{job:job.clone(),first:index,count:1,renders,doomed:false,recovery_suspect:false}
}
fn firmware(flights:Vec<Flight>)->Firmware{
    Firmware{engine:Engine{flights,ordinal:0},slots:Slots{exhausted:false,available:true}}
}

#[test]fn untimed_compute_queues_behind_untimed_compute(){
    let v=vm();let a=job(&v,vec![compute(false,true)],0);
    for query_access in [false,true]{
        let next=job(&v,vec![compute(false,query_access)],0);
        assert!(firmware(vec![flight(&a,0)]).can_queue_behind(&next));
    }
}
#[test]fn work_without_query_access_queues_behind_a_timestamp(){
    // The timed compute.light stream: each dispatch carries its own timestamps and touches
    // no query memory; it follows the stamped dispatch as untimed work does.
    let v=vm();let stamped=job(&v,vec![compute(true,false)],0);
    let next=job(&v,vec![compute(true,false)],0);
    assert!(firmware(vec![flight(&stamped,0)]).can_queue_behind(&next));
    let untimed=job(&v,vec![compute(false,false)],0);
    assert!(firmware(vec![flight(&stamped,0),flight(&next,0)]).can_queue_behind(&untimed));
}
#[test]fn query_copy_waits_until_the_timestamp_exists(){
    // A query copy or reset (or a compute command from userspace that does not declare
    // DRM_ASAHI_COMPUTE_NO_QUERY_ACCESS) never runs before the host wrote the timestamps of
    // a flight ahead of it: it waits for that flight's retirement.
    let v=vm();let stamped=job(&v,vec![compute(true,false)],0);
    let dispatch=job(&v,vec![compute(false,false)],0);
    let copy=job(&v,vec![compute(false,true)],0);
    let mut f=firmware(vec![flight(&stamped,0)]);
    assert!(!f.can_queue_behind(&copy));
    assert!(f.can_queue_behind(&dispatch));
    f.engine.flights.push(flight(&dispatch,0));
    assert!(!f.can_queue_behind(&copy));
    // The stamped flight retired (the host wrote its timestamps): the copy follows the
    // untimed dispatch still in flight.
    f.engine.flights.remove(0);
    assert!(f.can_queue_behind(&copy));
}
#[test]fn same_job_query_copy_waits_for_its_own_stamped_command(){
    // vkCmdWriteTimestamp then vkCmdCopyQueryPoolResults in one submission.
    let v=vm();
    let stamped=job(&v,vec![compute(true,false),compute(false,true)],1);
    assert!(!firmware(vec![flight(&stamped,0)]).can_queue_behind(&stamped));
    let untimed=job(&v,vec![compute(false,false),compute(false,true)],1);
    assert!(firmware(vec![flight(&untimed,0)]).can_queue_behind(&untimed));
}
#[test]fn renders_still_serialize(){
    let v=vm();
    for timestamps in [false,true]{
        let r=job(&v,vec![render(timestamps)],0);
        let next=job(&v,vec![compute(false,false)],0);
        assert!(!firmware(vec![flight(&r,0)]).can_queue_behind(&next));
    }
    let c=job(&v,vec![compute(false,false)],0);
    let r=job(&v,vec![render(false)],0);
    assert!(!firmware(vec![flight(&c,0)]).can_queue_behind(&r));
}
#[test]fn other_vms_failures_depth_and_dependencies_still_refuse(){
    let v=vm();let a=job(&v,vec![compute(true,false)],0);
    let other=job(&vm(),vec![compute(false,false)],0);
    assert!(!firmware(vec![flight(&a,0)]).can_queue_behind(&other));
    let next=job(&v,vec![compute(false,false)],0);
    let mut f=firmware(vec![flight(&a,0)]);f.engine.flights[0].doomed=true;
    assert!(!f.can_queue_behind(&next));
    let mut f=firmware(vec![flight(&a,0)]);f.engine.flights[0].recovery_suspect=true;
    assert!(!f.can_queue_behind(&next));
    let f=firmware((0..params::compute_flights()).map(|_|flight(&a,0)).collect());
    assert!(!f.can_queue_behind(&next));
    let mut f=firmware(vec![flight(&a,0)]);f.slots.available=false;
    assert!(!f.can_queue_behind(&next));
    let mut f=firmware(vec![flight(&a,0)]);f.engine.ordinal=u64::MAX;
    assert!(!f.can_queue_behind(&next));
    let waiting=Arc::new(Job{vm:v.clone(),progress:Progress{sent:0},commands:vec![compute(false,false)],resources:Resources,dependency:Err(EAGAIN)});
    assert!(!firmware(vec![flight(&a,0)]).can_queue_behind(&waiting));
    let failed=vm();failed.status.0.store(5,Ordering::Relaxed);
    let b=job(&failed,vec![compute(false,false)],0);let next=job(&failed,vec![compute(false,false)],0);
    assert!(!firmware(vec![flight(&b,0)]).can_queue_behind(&next));
}
#[test]fn a_flight_without_a_valid_command_span_counts_as_timestamped(){
    let v=vm();let a=job(&v,vec![compute(false,false)],0);
    let mut broken=flight(&a,0);broken.first=7;
    assert!(Firmware::writes_timestamps(&broken));
    let copy=job(&v,vec![compute(false,true)],0);
    assert!(!firmware(vec![broken]).can_queue_behind(&copy));
}
'''
harness = harness.replace('__CAN_QUEUE_BEHIND__', item(engine, 'fn can_queue_behind(&self, job: &Arc<Job>)'))
harness = harness.replace('__WRITES_TIMESTAMPS__', item(engine, 'fn writes_timestamps(f: &Flight)'))
harness = harness.replace('super::recovery::', 'recovery::')
harness = harness.replace('__LOGIC__', str(root / 'recovery/logic.rs'))

# The flag reaches the rule: parsed into `query_access` and advertised in GET_PARAMS.
assert 'query_access: cmd.flags & NO_QUERY_ACCESS == 0,' in command
assert command.count('cmd.flags & !NO_QUERY_ACCESS != 0') == 2
assert 'DRM_ASAHI_FEATURE_COMPUTE_NO_QUERY_ACCESS' in item(params, 'pub(crate) fn fill(')

unit = output / 'queue-behind.rs'
unit.write_text(harness)
log = ''
for step in [rustc() + ['--edition=2021', '--test', str(unit), '-o', str(output / 'queue-behind')],
             [str(output / 'queue-behind')]]:
    result = subprocess.run(step, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log += result.stdout
    print(result.stdout, end='')
    (output / 'queue-behind.log').write_text(log)
    if result.returncode:
        raise SystemExit(result.returncode)
(output / 'queue-behind.json').write_text(json.dumps({
    'passed': True, 'cpu_only': True, 'hardware_execution': False,
    'scope': 'Verbatim production can_queue_behind and writes_timestamps over a CPU-only engine.',
    'source_sha256': sha(root / 'engine.rs'),
}, indent=2) + '\n')
