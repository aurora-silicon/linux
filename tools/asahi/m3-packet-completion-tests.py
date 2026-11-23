#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Build a host test of M3's production packet completion with mock fences and VM state."""
import argparse
from pathlib import Path
import subprocess


def method(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


MOCKS = r"""
use std::cell::Cell;
use std::sync::{Mutex, atomic::{AtomicBool, Ordering}};
#[derive(Clone, Copy)]
struct Error(i32);
impl Error {
    fn to_errno(self) -> i32 { self.0 }
}
type Result = std::result::Result<(), Error>;
const EIO: Error = Error(-5);
const EBUSY: Error = Error(-16);
const ECANCELED: Error = Error(-125);

#[derive(Default)]
struct Vm {
    error: Cell<i32>,
}
impl Vm {
    fn status(&self) -> Option<&Self> { Some(self) }
    fn id(&self) -> u32 { 1 }
    fn record(&self, error: Error) {
        if self.error.get() == 0 {
            self.error.set(error.0);
        }
    }
    fn submit(&self) -> Result {
        match self.error.get() {
            0 => Ok(()),
            code => Err(Error(code)),
        }
    }
}
#[derive(Default)]
struct Fence {
    error: Cell<i32>,
    signals: Cell<usize>,
}
impl Fence {
    fn set_error(&self, error: Error) { self.error.set(error.0); }
    fn signal(&self) { self.signals.set(self.signals.get() + 1); }
}
#[derive(Default)]
struct Guard(Cell<bool>);
impl Guard {
    fn quarantine(&self) { self.0.set(true); }
}
struct Packet<'a> {
    vm: &'a Vm,
    vm_job: JobGuard,
    finish_claimed: AtomicBool,
    completion: Fence,
    commands: [(); 1],
}
struct JobGuard(Mutex<Option<Guard>>);
impl JobGuard {
    fn lock(&self) -> std::sync::MutexGuard<'_, Option<Guard>> { self.0.lock().unwrap() }
}
macro_rules! pr_info { ($($arg:tt)*) => { let _ = format_args!($($arg)*); }; }
mod debug {
    pub enum DebugFlags { M3PassTiming }
    pub fn debug_enabled(_: DebugFlags) -> bool { false }
}
mod m3 {
    pub mod agx_memory { pub fn publish() {} }
    pub mod m3_runtime { pub fn physical_counter() -> u64 { 0 } }
}

impl<'a> Packet<'a> {
    fn new(vm: &'a Vm, guard: bool) -> Self {
        Self {
            vm,
            vm_job: JobGuard(Mutex::new(guard.then(Guard::default))),
            finish_claimed: AtomicBool::new(false),
            completion: Fence::default(),
            commands: [()],
        }
    }
    fn profile_id(&self) -> usize { 0 }
"""

TESTS = r"""
}

#[test]
fn rejected_bystander_remains_admissible_after_another_vm_faults() {
    let guilty = Vm::default();
    let bystander = Vm::default();
    let faulty = Packet::new(&guilty, true);
    let rejected = Packet::new(&bystander, false);
    faulty.finish(Err(EIO));
    rejected.fail_packet(EBUSY);
    assert_eq!(guilty.error.get(), -5);
    assert!(faulty.vm_job.lock().as_ref().unwrap().0.get());
    assert_eq!(rejected.completion.error.get(), -16);
    assert_eq!(bystander.error.get(), 0);
    assert!(bystander.submit().is_ok());

    let later = Packet::new(&bystander, true);
    later.retired();
    assert_eq!(later.completion.error.get(), 0);
    assert_eq!(later.completion.signals.get(), 1);
    assert!(later.vm_job.lock().is_none());
}

#[test]
fn cancellation_preserves_live_guard_and_signals_once() {
    let vm = Vm::default();
    let packet = Packet::new(&vm, true);
    packet.fail_packet(ECANCELED);
    assert_eq!(vm.error.get(), 0);
    assert_eq!(packet.completion.error.get(), -125);
    assert!(packet.vm_job.lock().as_ref().unwrap().0.get());
    packet.retired();
    assert!(packet.vm_job.lock().is_none());
    assert_eq!(packet.completion.signals.get(), 1);
    assert_eq!(packet.completion.error.get(), -125);
}

#[test]
fn terminal_vm_error_remains_visible_and_blocks_the_same_vm() {
    let vm = Vm::default();
    let faulty = Packet::new(&vm, true);
    faulty.finish(Err(EIO));
    assert_eq!(vm.submit().unwrap_err().0, -5);
    assert_eq!(faulty.completion.signals.get(), 1);
    assert_eq!(faulty.completion.error.get(), -5);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--static", action="store_true", help="link without guest libraries")
    parser.add_argument("--mutate", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    path = repo / "drivers/gpu/drm/asahi/m3/m3_submit.rs"
    source = path.read_text().split("impl Packet {", 1)[1]
    if args.mutate:
        source = source.replace(
            "self.complete(Err(error), false);", "self.complete(Err(error), true);"
        )
    methods = ("release_guard", "retired", "finish", "fail_packet", "complete")
    extracted = [method(source, f"fn {name}(") for name in methods]
    args.out.mkdir(parents=True, exist_ok=True)
    program = args.out / "packet-completion.rs"
    program.write_text(MOCKS + "\n".join(extracted) + TESTS)
    binary = args.out.resolve() / "tests"
    flags = ["-C", "target-feature=+crt-static"] if args.static else []
    subprocess.run(
        ["rustc", "--test", "--edition=2021", *flags, str(program), "-o", str(binary)],
        check=True,
    )
    return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
