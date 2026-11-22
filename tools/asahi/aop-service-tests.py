#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production AOP service registration against host mocks.

Each check announces services to AopData::register_service and counts the
child-device registrations it queues. A service gets one device however
often, and under whatever other channel or name, it is announced again.
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


def program(ref):
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
    parts.append('impl AopData {\n' + block(aop, '    fn register_service(') + '\n}\n')
    parts.append(CHECKS)
    return '\n'.join(parts)


STUBS = r'''
#![allow(dead_code, unused_imports, unused_macros)]
use std::ffi::CStr;
use std::pin::Pin;
use std::sync::atomic::Ordering::Acquire;
use std::sync::Arc;
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(i32);
const ENOMEM: Error = Error(-12);
const EEXIST: Error = Error(-17);
const ENOSPC: Error = Error(-28);
type Result<T = ()> = std::result::Result<T, Error>;
const GFP_KERNEL: u32 = 0;
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EPICService { pub channel: u32, pub endpoint: u8 }
pub struct KVec<T> { items: Vec<T>, cap: usize }
impl<T> KVec<T> {
    fn with_capacity(cap: usize, _flags: u32) -> Result<Self> { Ok(Self { items: Vec::new(), cap }) }
    fn push_within_capacity(&mut self, v: T) -> std::result::Result<(), T> {
        if self.items.len() < self.cap { self.items.push(v); Ok(()) } else { Err(v) }
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
struct Mutex<T>(std::sync::Mutex<T>);
impl<T> Mutex<T> { fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() } }
struct Atomic<T>(std::sync::Mutex<T>);
impl<T: Copy> Atomic<T> { fn load(&self, _: std::sync::atomic::Ordering) -> T { *self.0.lock().unwrap() } }
struct Queue(std::sync::Mutex<Vec<(String, EPICService)>>);
impl Queue {
    fn enqueue(&self, work: Pin<Box<AopServiceRegisterWork>>) {
        self.0.lock().unwrap().push((work.name.to_str().unwrap().into(), work.service));
    }
}
struct AopServiceRegisterWork { name: &'static CStr, service: EPICService }
impl AopServiceRegisterWork {
    fn new(name: &'static CStr, _data: Arc<AopData>, service: EPICService) -> Result<Pin<Box<Self>>> {
        Ok(Box::pin(Self { name, service }))
    }
}
macro_rules! fmt { ($($t:tt)*) => { format_args!($($t)*) }; }
struct BStr<'a>(&'a [u8]);
impl<'a> BStr<'a> { fn from_bytes(b: &'a [u8]) -> Self { BStr(b) } }
impl std::fmt::Display for BStr<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(&String::from_utf8_lossy(self.0)) }
}
struct Device;
struct AFKEndpoint { index: u8, dropped: Vec<String> }
impl AFKEndpoint {
    fn drop_message(&mut self, _dev: &Device, why: std::fmt::Arguments<'_>) { self.dropped.push(why.to_string()); }
}
struct AopData {
    dev: Device, registration_gate: Mutex<Gate>, removing: Atomic<bool>, registration_queue: Queue,
}
'''

CHECKS = r'''
const NAMES: [&[u8]; 7] = [b"aop-audio", b"las", b"als", b"accel", b"gyro", b"devmotion6", b"als-temp"];
fn data(removing: bool) -> Arc<AopData> {
    Arc::new(AopData {
        dev: Device,
        registration_gate: Mutex(std::sync::Mutex::new(gate())),
        removing: Atomic(std::sync::Mutex::new(removing)),
        registration_queue: Queue(std::sync::Mutex::new(Vec::new())),
    })
}
fn announce_logged(d: &Arc<AopData>, endpoint: u8, channel: u32, name: &[u8]) -> Vec<String> {
    let mut ep = AFKEndpoint { index: endpoint, dropped: Vec::new() };
    // An error return is logged by the caller; the queue and the endpoint's dropped messages are what matter here.
    let _ = d.clone().register_service(&mut ep, channel, name);
    ep.dropped
}
fn announce(d: &Arc<AopData>, endpoint: u8, channel: u32, name: &[u8]) { announce_logged(d, endpoint, channel, name); }
fn queued(d: &Arc<AopData>) -> usize { d.registration_queue.0.lock().unwrap().len() }
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


def run(ref, tmp):
    source = Path(tmp) / (('old-' if ref else '') + 'aop-service.rs')
    binary = source.with_suffix('')
    source.write_text(program(ref))
    subprocess.run(shlex.split(os.environ.get('RUSTC', 'rustc')) +
                   ['--edition=2021', '-Dwarnings', str(source), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)]).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--old-ref', help='require this older revision to fail the checks and pass the controls')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='aop-service-') as tmp:
        rc = run(args.old_ref, tmp)
    if args.old_ref:
        if rc == 1:
            print(f'{args.old_ref} fails the checks, as expected')
            return 0
        raise SystemExit(f'{args.old_ref}: rc {rc}; the test does not catch the original code')
    return rc


if __name__ == '__main__':
    sys.exit(main())
