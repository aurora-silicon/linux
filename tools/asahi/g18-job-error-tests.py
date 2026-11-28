#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the G18 session's job-error classification, actual bodies, against a model.

The harness extracts from drivers/gpu/drm/asahi/g18 JobFailure, Failure and From<Error> for
Failure (boot/recovery.rs); ApplicationOwner::outcome(), decode(), finish(), JobOutcome and the
prepare step of G18Session::submit_application() (boot/session.rs); ApplicationJob, prepare()
and unpublished() (runtime/graphics_job_application.rs, with the PB and pool bodies the hand-over
test uses, through its model); and G18FirmwareMemory::application_compute_unpublished()
(initdata/application_compute.rs). They run against a model whose client VMs can have an address
unmapped, be closed, or ask for a target larger than an allocation admits.

What it shows:
- a render refused from the client's own state (an unmapped address it names, its VM closed, a
  target allocation that fails, also on the first frame) fails that job only: its errno reaches
  the submitter, the session stays live, no path is quarantined, the client's VM is not
  poisoned, the job stays retired with its generation unchanged, and the next render of any
  client runs;
- a prepare on a job that did not retire, and any job error on a crashed firmware, still end
  the session;
- the compute owner counts as unpublished only while no process, command, context, previous
  pool or held VM exists, and before it exists only while its scheduler record is unarmed;
- source checks: the compute submission classifies a failed preparation by that state, and
  nothing in the render submission reaches the firmware before prepare().

--mutations runs every mutant, including the code at c5e25e0925351, and requires each to fail.
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('g18_handoff', HERE / 'g18-client-handoff-tests.py')
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)
item, ROOT, BASE, G18 = handoff.item, handoff.ROOT, handoff.BASE, handoff.G18
RECOVERY = G18 + 'boot/recovery.rs'
SESSION = G18 + 'boot/session.rs'
COMPUTE_SESSION = G18 + 'boot/compute_session.rs'
COMPUTE = G18 + 'initdata/application_compute.rs'
PREPARE = handoff.PREPARE

EXTRA = r'''
thread_local! {
    static UNMAPPED: RefCell<Vec<(u64, u64)>> = RefCell::new(Vec::new());
    static CLOSED: RefCell<Vec<u64>> = RefCell::new(Vec::new());
    static POISONED: RefCell<Vec<u64>> = RefCell::new(Vec::new());
    static STOPPED: RefCell<bool> = RefCell::new(false);
}
const ENOMEM: i32 = 12; const EIO: i32 = 5;
const ALLOC_LIMIT: usize = 0x10000;
type Error = i32;
trait Errno { fn to_errno(&self) -> i32; fn from_errno(errno: i32) -> Self; }
impl Errno for i32 { fn to_errno(&self) -> i32 { -*self } fn from_errno(errno: i32) -> Self { -errno } }
pub struct Status(u64);
impl Status { pub fn record(&self, _: Error) { POISONED.with(|p| p.borrow_mut().push(self.0)); } }
macro_rules! dev_err { ($($t:tt)*) => {}; }
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32};
#[derive(Debug)] struct G18Recovery;
#[derive(Debug, Clone, Copy)] enum JobPath { Render, Compute }
impl JobPath { fn bit(&self) -> u32 { 1 << (*self as u32) } }
@@RECOVERY@@
struct Mutex<T>(std::sync::Mutex<T>);
impl<T> Mutex<T> { fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() } }
struct Retained(Option<G18Session>);
impl Retained { fn stop(&mut self, _: &str) -> Result { STOPPED.with(|s| *s.borrow_mut() = true); Ok(()) } }
struct Memory;
impl Memory { fn quarantine_application_compute(&mut self) {} fn log_trace_journal(&self, _: &AsahiDrmDevice) {} }
struct G18Session { dev: AsahiDrmDevice, uat: mmu::Uat, application_job: Option<Box<ApplicationJob>>,
    memory: Option<Memory>, crashed: bool }
impl G18Session {
    fn any_crashed(&self) -> bool { self.crashed }
    fn device(&self) -> &AsahiDrmDevice { &self.dev }
    fn dump_fault_evidence(&self, _: &str) {}
    fn submit_prepare(&mut self, vm: &mmu::Vm, render: &G18ApplicationRender) -> core::result::Result<(), Failure> {
@@SUBMIT@@
        Ok(())
    }
}
fn log_job_failure(_: &G18Session, _: &JobFailure) {}
const TICKET_RUNNING: i32 = 1;
@@OUTCOME_ENUM@@
struct ApplicationOwner;
impl ApplicationOwner {
@@OWNER@@
}

struct ComputeMemory { process: bool }
impl ComputeMemory { fn unpublished_idle(&self) -> bool { !self.process } }
struct ApplicationCompute { retired: bool, guard: Option<mmu::T8140VmJobGuard>, aliases: KVec<mmu::KernelMapping>,
    previous_pool: Option<()>, previous_binding: Option<()> }
struct Scheduler { armed: bool }
impl Scheduler { fn binding(&self, _: usize) -> Result<()> { if self.armed { Ok(()) } else { Err(EINVAL) } } }
struct G18FirmwareMemory { compute: ComputeMemory, application_compute: Option<Box<ApplicationCompute>>, scheduler: Scheduler }
impl G18FirmwareMemory {
@@COMPUTE@@
}

struct Device { session: Mutex<Retained>, failed: AtomicBool, quarantined: AtomicU32 }
impl Device {
    fn job<R>(&self, f: impl FnOnce(&mut ApplicationJob) -> R) -> R {
        f(self.session.lock().0.as_mut().unwrap().application_job.as_mut().unwrap())
    }
    /// One render through the ticket, as submit_render: prepare, then retire on success.
    fn render(&self, vm: u64, scissor: u64, target: usize, retire: bool) -> Result {
        let v = mmu::Vm { id: vm };
        let mut r = render(target);
        r.parameters.scissor_array = scissor;
        let code = ApplicationOwner::outcome(&self.session, &self.failed, &self.quarantined, |s| s.submit_prepare(&v, &r));
        let result = ApplicationOwner::finish(&v, ApplicationOwner::decode(code));
        if result.is_ok() && retire { self.job(|j| j.retired()); }
        result
    }
    fn live(&self) -> bool { !self.failed.load(Ordering::Acquire) && !STOPPED.with(|s| *s.borrow()) }
}
fn device(crashed: bool) -> Device {
    let session = G18Session { dev: AsahiDrmDevice, uat: mmu::Uat { lower: mmu::Vm { id: 0 } },
        application_job: Some(new_job()), memory: Some(Memory), crashed };
    STOPPED.with(|s| *s.borrow_mut() = false);
    Device { session: Mutex(std::sync::Mutex::new(Retained(Some(session)))), failed: AtomicBool::new(false),
        quarantined: AtomicU32::new(0) }
}
fn refused(d: &Device, result: Result, errno: i32, what: &str, generation: u32) {
    assert_eq!(result, Err(errno), "{}: the submitter must get the job's own errno", what);
    assert!(d.live(), "{}: a client's own error must not end the session", what);
    assert_eq!(d.quarantined.load(Ordering::Acquire), 0, "{}: no path may be quarantined", what);
    assert!(POISONED.with(|p| p.borrow().is_empty()), "{}: nothing ran, the client's VM stays usable", what);
    assert!(d.job(|job| job.unpublished() && job.generation == generation),
        "{}: the refused job must stay retired at generation {}", what, generation);
}
const SCISSOR: u64 = 0x5000;

fn main() {
    let d = device(false);
    d.render(1, SCISSOR, 64, true).unwrap();
    d.render(1, SCISSOR, 64, true).unwrap();
    UNMAPPED.with(|u| u.borrow_mut().push((2, SCISSOR)));
    let r = d.render(2, SCISSOR, 64, true); refused(&d, r, EFAULT, "unmapped scissor", 2);
    UNMAPPED.with(|u| u.borrow_mut().clear());
    d.render(2, SCISSOR, 64, true).unwrap();
    CLOSED.with(|c| c.borrow_mut().push(3));
    let r = d.render(3, SCISSOR, 64, true); refused(&d, r, ENOENT, "closed VM", 3);
    let r = d.render(1, SCISSOR, ALLOC_LIMIT * 2, true); refused(&d, r, ENOMEM, "target allocation", 3);
    let r = d.render(2, SCISSOR, ALLOC_LIMIT * 2, true); refused(&d, r, ENOMEM, "target allocation, another VM", 3);
    d.render(1, SCISSOR, 64, true).unwrap();
    assert_eq!(d.job(|j| j.generation), 4);
    d.render(2, SCISSOR, ALLOC_LIMIT, true).unwrap();
    println!("PASS render: unmapped address, closed VM and failed target allocation fail their job only; the session, its path and the client VMs serve on");

    drop(d);
    let d = device(false);
    UNMAPPED.with(|u| u.borrow_mut().push((1, SCISSOR)));
    let r = d.render(1, SCISSOR, 64, true); refused(&d, r, EFAULT, "first frame", 0);
    UNMAPPED.with(|u| u.borrow_mut().clear());
    d.render(1, SCISSOR, 64, true).unwrap();
    d.render(2, SCISSOR, 64, true).unwrap();
    println!("PASS render: a first frame refused after its kernel aliases were made is prepared again");

    drop(d);
    let d = device(false);
    d.render(1, SCISSOR, 64, false).unwrap();
    assert!(d.render(1, SCISSOR, 64, true).is_err() && !d.live(), "a job that did not retire must still end the session");
    drop(d);
    let d = device(true);
    UNMAPPED.with(|u| u.borrow_mut().push((1, SCISSOR)));
    assert!(d.render(1, SCISSOR, 64, true).is_err() && !d.live(), "a crashed firmware must still end the session");
    UNMAPPED.with(|u| u.borrow_mut().clear());
    println!("PASS render: an unretired job and a crashed firmware stay fatal");
    compute();
}
'''

COMPUTE_MAIN = r'''
fn compute() {
    let idle = || ApplicationCompute { retired: true, guard: None, aliases: KVec::new(), previous_pool: None, previous_binding: None };
    let memory = |job: Option<ApplicationCompute>, process: bool, armed: bool| G18FirmwareMemory {
        compute: ComputeMemory { process }, application_compute: job.map(Box::new), scheduler: Scheduler { armed } };
    assert!(memory(Some(idle()), false, true).application_compute_unpublished(), "a retired idle owner is unpublished");
    assert!(memory(None, false, false).application_compute_unpublished(), "before the owner exists nothing is armed");
    for (name, m) in [
        ("bound process", memory(Some(idle()), true, true)),
        ("unretired", memory(Some(ApplicationCompute { retired: false, ..idle() }), false, true)),
        ("held VM", memory(Some(ApplicationCompute { guard: Some(mmu::T8140VmJobGuard), ..idle() }), false, true)),
        ("previous pool", memory(Some(ApplicationCompute { previous_pool: Some(()), ..idle() }), false, true)),
        ("previous binding", memory(Some(ApplicationCompute { previous_binding: Some(()), ..idle() }), false, true)),
        ("armed record without owner", memory(None, false, true)),
    ] {
        assert!(!m.application_compute_unpublished(), "compute owner with {} must stay fatal", name);
    }
    println!("PASS compute: only a retired, unbound owner (or an unarmed one not yet built) makes a failed preparation a job failure");
}
'''


def build(read):
    code = handoff.build(read).replace('fn main() {', 'fn handoff_main() {', 1)
    for old, new in [
        ('pub fn retain_t8140_job(&self) -> Result<T8140VmJobGuard> { Ok(T8140VmJobGuard) }',
         'pub fn retain_t8140_job(&self) -> Result<T8140VmJobGuard> {\n'
         '            if CLOSED.with(|c| c.borrow().contains(&self.id)) { Err(ENOENT) } else { Ok(T8140VmJobGuard) } }\n'
         '        pub fn status(&self) -> Option<Status> { Some(Status(self.id)) }'),
        ('pub fn translate_iova(&self, a: u64) -> Result<u64> { Ok(a) }',
         'pub fn translate_iova(&self, a: u64) -> Result<u64> {\n'
         '            if UNMAPPED.with(|u| u.borrow().contains(&(self.id, a))) { Err(EFAULT) } else { Ok(a) } }'),
        ('fn regrow_wc_at(&mut self, _: &AsahiDrmDevice, _: &mmu::Vm, _: u64, size: usize) -> Result {',
         'fn regrow_wc_at(&mut self, _: &AsahiDrmDevice, _: &mmu::Vm, _: u64, size: usize) -> Result {\n'
         '        if size > ALLOC_LIMIT { return Err(ENOMEM); }'),
        ('fn retired(&mut self) { self.retired = true; self.guard = None; }',
         'fn retired(&mut self) { self.retired = true; self.guard = None; }\n    fn quarantine(&mut self) {}\n'
         # The code before the fix has no such method; the model states the same property.
         + (item(read(PREPARE), 'pub(crate) fn unpublished(', optional=True)
            or 'fn unpublished(&self) -> bool { self.retired && self.guard.is_none() }')),
    ]:
        assert code.count(old) == 1, old
        code = code.replace(old, new)
    recovery = read(RECOVERY)
    rec = '\n'.join(item(recovery, s).replace('pub(super) ', '') for s in
                    ('pub(super) enum JobFailure {', 'pub(super) enum Failure {', 'impl From<Error> for Failure {'))
    session = read(SESSION)
    submit = item(session, 'fn submit_application(')
    start = submit.index('let job=self.application_job.as_mut().ok_or(EIO)?;')
    end = submit.index('if job.pool_aliases.is_empty()')
    owner = '\n'.join(item(session, s) for s in ('fn outcome<F>(', 'fn decode(', 'fn finish('))
    compute = item(read(COMPUTE), 'pub(crate) fn application_compute_unpublished(', optional=True)
    extra = (EXTRA.replace('@@RECOVERY@@', rec).replace('@@SUBMIT@@', submit[start:end])
             .replace('@@OUTCOME_ENUM@@', item(session, 'enum JobOutcome {')).replace('@@OWNER@@', owner)
             .replace('@@COMPUTE@@', compute))
    extra += COMPUTE_MAIN if compute else 'fn compute() {}\n'
    return code + extra


sub = handoff.sub
MUTATIONS = {
    'prepare-commits-early': lambda c: sub(
        '        // Commit the frame only now: until here an error leaves the job\n'
        '        // retired, and the next prepare() rebuilds whatever this one changed.\n'
        '        self.guard=Some(guard);self.generation=frame.generation;self.retired=false;\n'
        '        self.client_frames=if new_client {1} else {self.client_frames.saturating_add(1)};\n', '')(
        sub('        self.job.program.rebind_retired(vm)?;\n',
            '        self.job.program.rebind_retired(vm)?;\n'
            '        self.guard=Some(guard);self.generation=frame.generation;self.retired=false;\n'
            '        self.client_frames=if new_client {1} else {self.client_frames.saturating_add(1)};\n')(c)),
    'first-frame-alias-repeated': sub('if self.generation==0 && self.job.aliases.is_empty() {', 'if self.generation==0 {'),
    'refusal-always-fatal': sub('Failure::Job(JobFailure::Refused(error))} else {Failure::Fatal(error)}',
                                'Failure::Fatal(error)} else {Failure::Fatal(error)}'),
    'refusal-ignores-state': sub('if job.unpublished() {Failure::Job', 'if true {Failure::Job'),
    'refusal-lost-in-ticket': sub('            code if code>JobOutcome::Refused as i32 =>', '            code if false =>'),
    'refusal-poisons-vm': sub('Err(Failure::Job(JobFailure::Refused(error))) => Err(error),',
                              'Err(Failure::Job(JobFailure::Refused(error))) => {if let Some(status)=vm.status(){status.record(error);}Err(error)}'),
    'compute-ignores-process': sub('self.compute.unpublished_idle() && match', 'match'),
    'compute-unbuilt-always': sub('None => self.scheduler.binding(0).is_err(),', 'None => true,'),
    'compute-ignores-previous-pool': sub('&& job.previous_pool.is_none() ', ''),
}


def source_checks(read):
    compute = item(read(COMPUTE_SESSION), 'pub(super) fn submit_application_compute(')
    arm = compute[compute.index('.prepare_application_compute(&self.dev, vm, translated)'):]
    arm = arm[:arm.index('if generation == 1')]
    assert 'memory.application_compute_unpublished()' in arm and 'Failure::Job(JobFailure::Refused(error))' in arm \
        and 'Failure::Fatal(error)' in arm, arm
    submit = item(read(SESSION), 'fn submit_application(')
    before = submit[:submit.index('job.prepare(')]
    for word in ('send_message', 'publish', 'install_records'):
        assert word not in before, 'render submission reaches the firmware before prepare: ' + word
    print('PASS source: compute classifies a failed preparation by the owner state; nothing of a render is published before prepare()')


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--output', type=Path, help='Work directory (default: a temporary one)')
    p.add_argument('--rustc', default=os.environ.get('HOSTRUSTC') or os.environ.get('RUSTC', 'rustc'))
    p.add_argument('--mutations', action='store_true',
                   help=f'Also require every mutant and the code at {BASE} to fail')
    a = p.parse_args()
    out = (a.output or Path(tempfile.mkdtemp(prefix='g18-job-error-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    rustc = [a.rustc]

    def tip(path):
        return (ROOT / path).read_text()

    def base(path):
        return subprocess.check_output(['git', 'show', f'{BASE}:{path}'], cwd=ROOT, text=True)

    source_checks(tip)
    result = handoff.compile_run(build(tip), out, 'fixed', rustc)
    if result.returncode != 0:
        sys.exit('FAIL fixed code\n' + result.stdout + result.stderr)
    print(result.stdout.strip())
    if not a.mutations:
        return
    # The code before the fix with only the hand-over test's read-only graph check relaxed.
    unchecked = sub('assert_eq!(m.prot, mmu::Prot::Ro,', 'assert!(true || m.prot == mmu::Prot::Ro,')
    mutants = [(BASE, unchecked(build(base)))]
    mutants += [(name, apply(build(tip))) for name, apply in MUTATIONS.items()]
    killed = 0
    for name, code in mutants:
        result = handoff.compile_run(code, out, 'mutant-' + name, rustc)
        if result.returncode == 0 or 'panicked at' not in result.stderr:
            sys.exit(f'FAIL mutant {name} survived\n{result.stdout}{result.stderr}')
        reason = next((l for l in result.stderr.splitlines() if l and 'panicked at' not in l and 'note:' not in l), '')
        print(f'PASS mutant killed: {name}: {reason.strip()}')
        killed += 1
    print(f'PASS {killed}/{len(mutants)} mutants killed')


if __name__ == '__main__':
    main()
