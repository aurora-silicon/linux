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
use std::sync::{Mutex, atomic::{AtomicBool, AtomicU8, Ordering}};
#[derive(Clone, Copy, PartialEq, Eq)]
struct Error(i32);
impl Error {
    fn to_errno(self) -> i32 { self.0 }
}
type Result = std::result::Result<(), Error>;
const EIO: Error = Error(-5);
const EBUSY: Error = Error(-16);
const ENODEV: Error = Error(-19);
const ETIMEDOUT: Error = Error(-110);
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
    fn record_if_device_loss(&self, error: Error) {
        if error == EIO || error == ETIMEDOUT || error == ENODEV {
            self.record(error);
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
    finish_state: AtomicU8,
    guard_pinned: AtomicBool,
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
            finish_state: AtomicU8::new(0),
            guard_pinned: AtomicBool::new(false),
            completion: Fence::default(),
            commands: [()],
        }
    }
    fn profile_id(&self) -> usize { 0 }
"""

TESTS = r"""
}

#[test]
fn direct_packet_rejection_leaves_vm_admissible() {
    let guilty = Vm::default();
    let other = Vm::default();
    let faulty = Packet::new(&guilty, true);
    let rejected = Packet::new(&other, false);
    faulty.finish(Err(EIO));
    rejected.fail_packet(EBUSY);
    assert_eq!(guilty.error.get(), -5);
    assert!(faulty.vm_job.lock().as_ref().unwrap().0.get());
    assert_eq!(rejected.completion.error.get(), -16);
    assert_eq!(other.error.get(), 0);
    assert!(other.submit().is_ok());

    let later = Packet::new(&other, true);
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
    // Clean retirement closes the state to TERMINAL: a racing post-retirement fault must not poison VmStatus.
    packet.finish(Err(EIO));
    assert_eq!(vm.error.get(), 0);
    assert_eq!(packet.completion.signals.get(), 1);
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

#[test]
fn cancel_before_terminal_fault_records_vm_error_without_resignaling() {
    let vm = Vm::default();
    let packet = Packet::new(&vm, true);
    packet.fail_packet(ECANCELED);
    assert_eq!(vm.error.get(), 0);
    assert_eq!(packet.completion.signals.get(), 1);
    assert_eq!(packet.completion.error.get(), -125);
    packet.finish(Err(EIO));
    assert_eq!(vm.error.get(), -5);
    assert_eq!(packet.completion.signals.get(), 1);
    assert_eq!(packet.completion.error.get(), -125);
}

#[test]
fn fail_packet_records_device_loss_errors() {
    for err in [EIO, ETIMEDOUT, ENODEV] {
        let vm = Vm::default();
        let packet = Packet::new(&vm, false);
        packet.fail_packet(err);
        assert_eq!(vm.error.get(), err.0);
        assert_eq!(packet.completion.error.get(), err.0);
        assert_eq!(packet.completion.signals.get(), 1);
    }
}

#[test]
fn terminal_fault_pins_guard_against_release_guard() {
    let vm = Vm::default();
    let packet = Packet::new(&vm, true);
    packet.finish(Err(EIO));
    assert!(packet.vm_job.lock().as_ref().unwrap().0.get());
    // Subsequent fail_front() / withdraw() calls must not release a quarantined guard.
    packet.release_guard();
    assert!(packet.vm_job.lock().is_some());
    assert!(packet.vm_job.lock().as_ref().unwrap().0.get());
}

#[test]
fn cancel_then_terminal_fault_pins_guard_against_release_guard() {
    let vm = Vm::default();
    let packet = Packet::new(&vm, true);
    packet.fail_packet(ECANCELED);
    packet.finish(Err(EIO));
    packet.release_guard();
    assert!(packet.vm_job.lock().is_some());
    assert!(packet.vm_job.lock().as_ref().unwrap().0.get());
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
    extracted = [method(source, f"fn {name}(&self") for name in methods]
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
