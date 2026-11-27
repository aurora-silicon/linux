#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production AOP service registration against host mocks.

Each check announces services to AopData::register_service and runs
AopServiceRegisterWork::run to count the child-device registrations it queues
and completes. A service gets one device however often, and under whatever
other channel or name, it is announced again, and any registration failure
releases the claim so the next announcement tries again.
"""
import argparse
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
AOP = 'drivers/soc/apple/aop.rs'
SERVICES = 'drivers/soc/apple/aop_services.rs'

MUTATIONS = {
    'no-work-release': (
        'register',
        'claims.release(svc);',
        0,
        ['work-item allocation failure releases the claim'],
    ),
    'no-slot-release': (
        'run',
        'this.data.registration_gate.lock().release(this.service);',
        0,
        ['subdevice slot allocation failure releases the claim'],
    ),
    'no-register-release': (
        'run',
        'this.data.registration_gate.lock().release(this.service);',
        1,
        [
            'platform device registration error releases the claim',
            'null platform device return releases the claim',
        ],
    ),
}


def read(ref, path):
    if ref is None:
        file = ROOT / path
        return file.read_text() if file.is_file() else None
    probe = subprocess.run(['git', 'cat-file', '-e', f'{ref}:{path}'], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if probe.returncode:
        return None
    return subprocess.check_output(['git', 'show', f'{ref}:{path}'], cwd=ROOT, text=True)


def block(source, marker):
    start = source.index(marker)
    brace = source.index('{', source.index(')', start))
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def drop_occurrence(source, needle, which, expected_count):
    positions = []
    pos = 0
    while True:
        idx = source.find(needle, pos)
        if idx < 0:
            break
        positions.append(idx)
        pos = idx + len(needle)
    if len(positions) != expected_count:
        raise SystemExit(
            f'mutation target {needle!r}: found {len(positions)} occurrences, expected {expected_count}'
        )
    idx = positions[which]
    return source[:idx] + source[idx + len(needle):]


def program(ref, mutation=None):
    aop = read(ref, AOP)
    services = read(ref, SERVICES)
    parts = [STUBS]
    if services is None:
        parts.append('type Gate = ();\nfn gate() -> Gate {}\n')
    else:
        body = services[:services.index('#[kunit_tests')]
        body = '\n'.join(line for line in body.splitlines()
                         if not line.startswith(('use kernel', '//!')))
        parts.append('mod aop_services {\nuse super::*;\n' + body + '\n}\n')
        parts.append('type Gate = aop_services::Claims;\n'
                     'fn gate() -> Gate { aop_services::Claims::new().unwrap() }\n')
    reg_fn = block(aop, '    fn register_service(')
    run_fn = block(aop, '    fn run(this: Pin<KBox<AopServiceRegisterWork>>)')
    if mutation is not None:
        target, needle, which, _ = MUTATIONS[mutation]
        if target == 'register':
            reg_fn = drop_occurrence(reg_fn, needle, which, 1)
        else:
            run_fn = drop_occurrence(run_fn, needle, which, 2)
    parts.append('impl AopServiceRegisterWork {\n' + run_fn + '\n}\n')
    parts.append('impl AopData {\n' + reg_fn + '\n}\n')
    parts.append(CHECKS)
    return '\n'.join(parts)


STUBS = r'''
#![allow(dead_code, non_camel_case_types, unused_imports, unused_macros)]
use std::collections::VecDeque;
use std::ffi::{c_char, c_void, CStr};
use std::mem;
use std::ops::{Deref, DerefMut};
use std::pin::Pin;
use std::ptr::{self, NonNull};
use std::sync::atomic::{
    AtomicBool, AtomicUsize,
    Ordering::{Acquire, SeqCst},
};
use std::sync::Arc;
type KBox<T> = Box<T>;
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(i32);
const ENOMEM: Error = Error(-12);
const EEXIST: Error = Error(-17);
const EINVAL: Error = Error(-22);
const ENOSPC: Error = Error(-28);
type Result<T = ()> = std::result::Result<T, Error>;
const GFP_KERNEL: u32 = 0;
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EPICService { pub channel: u32, pub endpoint: u8 }
pub struct PushError<T>(pub T);
static FAIL_WORK_NEW: AtomicUsize = AtomicUsize::new(0);
static FAIL_SUBDEV_RESERVE: AtomicUsize = AtomicUsize::new(0);
static REGISTER_CALLS: AtomicUsize = AtomicUsize::new(0);
static NEXT_PDEV: AtomicUsize = AtomicUsize::new(1);
static UNREGISTER_CALLS: AtomicUsize = AtomicUsize::new(0);
static SUBDEVICES_LOCKED: AtomicBool = AtomicBool::new(false);
#[derive(Clone, Copy)]
enum RegisterOutcome { Ok, ErrPtr(i32), Null }
static REGISTER_OUTCOMES: std::sync::Mutex<VecDeque<RegisterOutcome>> =
    std::sync::Mutex::new(VecDeque::new());
static DEV_ERRS: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
fn take_fail(counter: &AtomicUsize) -> bool {
    let mut cur = counter.load(SeqCst);
    while cur > 0 {
        match counter.compare_exchange(cur, cur - 1, SeqCst, SeqCst) {
            Ok(_) => return true,
            Err(next) => cur = next,
        }
    }
    false
}
fn push_register_outcome(outcome: RegisterOutcome) {
    REGISTER_OUTCOMES.lock().unwrap().push_back(outcome);
}
pub struct KVec<T> { items: Vec<T>, cap: usize }
impl<T> KVec<T> {
    fn new() -> Self { Self { items: Vec::new(), cap: 0 } }
    fn with_capacity(cap: usize, _flags: u32) -> Result<Self> { Ok(Self { items: Vec::new(), cap }) }
    fn reserve(&mut self, additional: usize, _flags: u32) -> Result<()> {
        if take_fail(&FAIL_SUBDEV_RESERVE) {
            return Err(ENOMEM);
        }
        self.cap = self.cap.max(self.items.len() + additional);
        Ok(())
    }
    fn push_within_capacity(&mut self, v: T) -> std::result::Result<(), PushError<T>> {
        if self.items.len() < self.cap { self.items.push(v); Ok(()) } else { Err(PushError(v)) }
    }
    fn iter(&self) -> std::slice::Iter<'_, T> { self.items.iter() }
    fn retain(&mut self, f: impl FnMut(&mut T) -> bool) { self.items.retain_mut(f) }
    fn len(&self) -> usize { self.items.len() }
}
macro_rules! c_str {
    ($s:literal) => {
        match CStr::from_bytes_with_nul(concat!($s, "\0").as_bytes()) { Ok(c) => c, Err(_) => panic!() }
    };
}
macro_rules! dev_err {
    ($dev:expr, $($arg:tt)+) => {{
        let _ = &$dev;
        DEV_ERRS.lock().unwrap().push(format!($($arg)+));
    }};
}
trait CStrExt { fn as_char_ptr(&self) -> *const c_char; }
impl CStrExt for CStr { fn as_char_ptr(&self) -> *const c_char { self.as_ptr() } }
struct GateMutex<T>(std::sync::Mutex<T>);
impl<T> GateMutex<T> {
    fn lock(&self) -> std::sync::MutexGuard<'_, T> {
        assert!(!SUBDEVICES_LOCKED.load(SeqCst), "registration_gate locked while holding subdevices");
        self.0.lock().unwrap()
    }
}
struct SubdevGuard<'a, T>(std::sync::MutexGuard<'a, T>);
impl<T> Deref for SubdevGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T { &self.0 }
}
impl<T> DerefMut for SubdevGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T { &mut self.0 }
}
impl<T> Drop for SubdevGuard<'_, T> {
    fn drop(&mut self) { SUBDEVICES_LOCKED.store(false, SeqCst); }
}
struct SubdevMutex<T>(std::sync::Mutex<T>);
impl<T> SubdevMutex<T> {
    fn lock(&self) -> SubdevGuard<'_, T> {
        let g = self.0.lock().unwrap();
        SUBDEVICES_LOCKED.store(true, SeqCst);
        SubdevGuard(g)
    }
}
struct Atomic<T>(std::sync::Mutex<T>);
impl<T: Copy> Atomic<T> { fn load(&self, _: std::sync::atomic::Ordering) -> T { *self.0.lock().unwrap() } }
mod bindings {
    use super::*;
    pub const PLATFORM_DEVID_AUTO: i32 = -2;
    pub struct device;
    pub struct platform_device;
    pub struct platform_device_info {
        pub parent: *mut device,
        pub name: *const c_char,
        pub id: i32,
        pub res: *mut c_void,
        pub num_res: u32,
        pub data: *const c_void,
        pub size_data: usize,
        pub dma_mask: u64,
        pub fwnode: *mut c_void,
        pub swnode: *mut c_void,
        pub properties: *mut c_void,
        pub of_node_reused: bool,
    }
    pub unsafe fn platform_device_register_full(
        _info: *const platform_device_info,
    ) -> *mut platform_device {
        REGISTER_CALLS.fetch_add(1, SeqCst);
        match REGISTER_OUTCOMES.lock().unwrap().pop_front().unwrap_or(RegisterOutcome::Ok) {
            RegisterOutcome::Ok => (NEXT_PDEV.fetch_add(1, SeqCst) * 16) as *mut platform_device,
            RegisterOutcome::ErrPtr(errno) => (errno as isize) as *mut platform_device,
            RegisterOutcome::Null => ptr::null_mut(),
        }
    }
}
fn from_err_ptr<T>(p: *mut T) -> Result<*mut T> {
    let addr = p as usize as isize;
    if (-4095..=-1).contains(&addr) {
        Err(Error(addr as i32))
    } else {
        Ok(p)
    }
}
struct ChildDevice(NonNull<bindings::platform_device>);
impl ChildDevice {
    fn unregister(self) { UNREGISTER_CALLS.fetch_add(1, SeqCst); }
}
struct FwNode;
impl FwNode {
    fn get_child_by_name(&self, _name: &CStr) -> Option<FwNode> { None }
    fn as_raw(&self) -> *mut c_void { ptr::null_mut() }
}
struct Queue(std::sync::Mutex<Vec<Pin<Box<AopServiceRegisterWork>>>>);
impl Queue {
    fn enqueue(&self, work: Pin<Box<AopServiceRegisterWork>>) {
        self.0.lock().unwrap().push(work);
    }
    fn drain(&self) {
        loop {
            let items: Vec<_> = {
                let mut q = self.0.lock().unwrap();
                if q.is_empty() { break; }
                q.drain(..).collect()
            };
            for work in items {
                AopServiceRegisterWork::run(work);
            }
        }
    }
}
struct AopServiceRegisterWork { name: &'static CStr, data: Arc<AopData>, service: EPICService }
impl AopServiceRegisterWork {
    fn new(name: &'static CStr, data: Arc<AopData>, service: EPICService) -> Result<Pin<Box<Self>>> {
        if take_fail(&FAIL_WORK_NEW) {
            return Err(ENOMEM);
        }
        Ok(Box::pin(Self { name, data, service }))
    }
}
macro_rules! fmt { ($($t:tt)*) => { format_args!($($t)*) }; }
struct BStr<'a>(&'a [u8]);
impl<'a> BStr<'a> { fn from_bytes(b: &'a [u8]) -> Self { BStr(b) } }
impl std::fmt::Display for BStr<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(&String::from_utf8_lossy(self.0)) }
}
struct Device;
impl Device {
    fn fwnode(&self) -> Option<FwNode> { None }
    fn as_raw(&self) -> *mut bindings::device { ptr::null_mut() }
}
struct AFKEndpoint { index: u8, dropped: Vec<String> }
impl AFKEndpoint {
    fn drop_message(&mut self, _dev: &Device, why: std::fmt::Arguments<'_>) { self.dropped.push(why.to_string()); }
}
struct AopData {
    dev: Device,
    registration_gate: GateMutex<Gate>,
    removing: Atomic<bool>,
    registration_queue: Queue,
    subdevices: SubdevMutex<KVec<ChildDevice>>,
}
'''

CHECKS = r'''
const NAMES: [&[u8]; 7] = [b"aop-audio", b"las", b"als", b"accel", b"gyro", b"devmotion6", b"als-temp"];
fn data(removing: bool) -> Arc<AopData> {
    Arc::new(AopData {
        dev: Device,
        registration_gate: GateMutex(std::sync::Mutex::new(gate())),
        removing: Atomic(std::sync::Mutex::new(removing)),
        registration_queue: Queue(std::sync::Mutex::new(Vec::new())),
        subdevices: SubdevMutex(std::sync::Mutex::new(KVec::new())),
    })
}
fn announce_result(d: &Arc<AopData>, endpoint: u8, channel: u32, name: &[u8]) -> (Result<()>, Vec<String>) {
    let mut ep = AFKEndpoint { index: endpoint, dropped: Vec::new() };
    let res = d.clone().register_service(&mut ep, channel, name);
    (res, ep.dropped)
}
fn announce_logged(d: &Arc<AopData>, endpoint: u8, channel: u32, name: &[u8]) -> Vec<String> {
    announce_result(d, endpoint, channel, name).1
}
fn announce(d: &Arc<AopData>, endpoint: u8, channel: u32, name: &[u8]) { announce_logged(d, endpoint, channel, name); }
fn queued(d: &Arc<AopData>) -> usize { d.registration_queue.0.lock().unwrap().len() }
fn drain(d: &Arc<AopData>) { d.registration_queue.drain(); }
fn registered(d: &Arc<AopData>) -> usize { d.subdevices.lock().len() }
fn main() {
    let mut fixed = Vec::new();
    let mut control = Vec::new();
    let d = data(false);
    for _ in 0..5 { announce(&d, 0x23, 1, b"als"); }
    fixed.push(("an unchanged announcement repeated 5 times", queued(&d), 1));
    let d = data(false);
    announce(&d, 0x23, 1, b"als");
    announce(&d, 0x23, 2, b"als");
    announce(&d, 0x24, 1, b"als");
    fixed.push(("a name announced again on another channel and endpoint", queued(&d), 1));
    let d = data(false);
    announce(&d, 0x21, 1, b"aop-audio");
    announce(&d, 0x21, 1, b"als");
    fixed.push(("a channel announced again under another name", queued(&d), 1));
    let d = data(false);
    announce(&d, 0x23, 1, b"als");
    let log = announce_logged(&d, 0x23, 2, b"als");
    let named = log.len() == 1 && log[0].contains("\"als\"") && log[0].contains("channel 2");
    fixed.push(("a refused announcement logged once as a dropped message, with its name and channel", named as usize, 1));
    let d = data(false);
    announce(&d, 0x23, 1, b"als");
    let quiet = announce_logged(&d, 0x23, 1, b"als").len();
    control.push(("an unchanged repeat logs nothing", quiet, 0));
    let d = data(false);
    for channel in 0..64u32 {
        for name in NAMES { announce(&d, 0x20 + (channel % 8) as u8, channel, name); }
    }
    fixed.push(("every known name on 64 channels", queued(&d), NAMES.len()));
    let d = data(false);
    for (i, name) in NAMES.iter().enumerate() { announce(&d, 0x20 + i as u8, i as u32, name); }
    control.push(("each known service once", queued(&d), NAMES.len()));
    let d = data(false);
    announce(&d, 0x20, 1, b"misc");
    announce(&d, 0x20, 2, b"aop-audio\0x");
    control.push(("unknown services", queued(&d), 0));
    let d = data(true);
    announce(&d, 0x23, 1, b"als");
    control.push(("an announcement during removal", queued(&d), 0));

    // Claim-release path 1 (register_service): work-item allocation fails once.
    let d = data(false);
    announce(&d, 0x21, 1, b"aop-audio");
    FAIL_WORK_NEW.store(1, SeqCst);
    let (first, _) = announce_result(&d, 0x23, 1, b"als");
    let (retry, _) = announce_result(&d, 0x23, 1, b"als");
    drain(&d);
    let ok_work = first == Err(ENOMEM) && retry.is_ok() && registered(&d) == 2;
    control.push(("work-item allocation failure releases the claim", ok_work as usize, 1));

    // Claim-release path 2 (AopServiceRegisterWork::run): subdevice slot reserve fails once.
    let d = data(false);
    announce(&d, 0x21, 1, b"aop-audio");
    drain(&d);
    let calls_before = REGISTER_CALLS.load(SeqCst);
    announce(&d, 0x23, 1, b"als");
    FAIL_SUBDEV_RESERVE.store(1, SeqCst);
    drain(&d);
    let skipped_reg = REGISTER_CALLS.load(SeqCst) == calls_before && registered(&d) == 1;
    announce(&d, 0x23, 1, b"als");
    drain(&d);
    let ok_slot = skipped_reg && registered(&d) == 2;
    control.push(("subdevice slot allocation failure releases the claim", ok_slot as usize, 1));

    // Claim-release path 3a (AopServiceRegisterWork::run): platform_device_register_full returns ERR_PTR.
    let d = data(false);
    announce(&d, 0x21, 1, b"aop-audio");
    drain(&d);
    announce(&d, 0x23, 1, b"als");
    push_register_outcome(RegisterOutcome::ErrPtr(-12));
    drain(&d);
    let after_err = registered(&d) == 1;
    announce(&d, 0x23, 1, b"als");
    drain(&d);
    let ok_err = after_err && registered(&d) == 2;
    control.push(("platform device registration error releases the claim", ok_err as usize, 1));

    // Claim-release path 3b (AopServiceRegisterWork::run): platform_device_register_full returns NULL.
    let d = data(false);
    announce(&d, 0x23, 1, b"als");
    push_register_outcome(RegisterOutcome::Null);
    drain(&d);
    let after_null = registered(&d) == 0;
    announce(&d, 0x23, 1, b"als");
    drain(&d);
    let ok_null = after_null && registered(&d) == 1;
    control.push(("null platform device return releases the claim", ok_null as usize, 1));

    // After failing through all three paths and succeeding on retry, repeats stay refused
    // and previously registered services keep their claims.
    let d = data(false);
    announce(&d, 0x21, 1, b"aop-audio");
    drain(&d);
    FAIL_WORK_NEW.store(1, SeqCst);
    announce(&d, 0x23, 1, b"als");
    announce(&d, 0x23, 1, b"als");
    FAIL_SUBDEV_RESERVE.store(1, SeqCst);
    drain(&d);
    announce(&d, 0x23, 1, b"als");
    push_register_outcome(RegisterOutcome::ErrPtr(-12));
    drain(&d);
    announce(&d, 0x23, 1, b"als");
    drain(&d);
    announce(&d, 0x21, 1, b"aop-audio");
    announce(&d, 0x23, 1, b"als");
    announce(&d, 0x23, 2, b"als");
    drain(&d);
    fixed.push(("repeats stay refused after failed registrations retry and succeed", registered(&d), 2));

    let mut out = String::new();
    let mut fixed_failed = 0;
    let mut control_failed = 0;
    for (kind, list) in [("check", &fixed), ("control", &control)] {
        for (what, got, want) in list.iter() {
            let ok = got == want;
            if !ok { if kind == "check" { fixed_failed += 1 } else { control_failed += 1 } }
            out += &format!("{} {}: {}: got {}, expected {}\n", if ok { "ok" } else { "FAIL" }, kind, what, got, want);
        }
    }
    print!("{}", out);
    println!("checks failed {} of {}, controls failed {} of {}", fixed_failed, fixed.len(), control_failed, control.len());
    std::process::exit(if control_failed > 0 { 3 } else if fixed_failed > 0 { 1 } else { 0 });
}
'''


def run(ref, tmp, mutation=None):
    prefix = f'mut-{mutation}-' if mutation else ('old-' if ref else '')
    source = Path(tmp) / (prefix + 'aop-service.rs')
    binary = source.with_suffix('')
    source.write_text(program(ref, mutation))
    subprocess.run(shlex.split(os.environ.get('RUSTC', 'rustc')) +
                   ['--edition=2021', '-Dwarnings', str(source), '-o', str(binary)], check=True)
    res = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sys.stdout.write(res.stdout)
    return res.returncode, res.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    group = ap.add_mutually_exclusive_group()
    group.add_argument('--old-ref', help='require this older revision to fail the checks and pass the controls')
    group.add_argument('--mutation', choices=sorted(MUTATIONS),
                       help='drop one claim-release call and require its control to fail')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='aop-service-') as tmp:
        rc, out = run(args.old_ref, tmp, args.mutation)
    if args.old_ref:
        if rc == 1:
            print(f'{args.old_ref} fails the checks, as expected')
            return 0
        raise SystemExit(f'{args.old_ref}: rc {rc}; the test does not catch the original code')
    if args.mutation:
        expected = MUTATIONS[args.mutation][3]
        missing = [name for name in expected if f'FAIL control: {name}:' not in out]
        if rc == 3 and not missing:
            print(f'mutation {args.mutation} fails its control, as expected')
            return 0
        raise SystemExit(f'mutation {args.mutation}: rc {rc}, missing failures {missing}')
    return rc


if __name__ == '__main__':
    sys.exit(main())
