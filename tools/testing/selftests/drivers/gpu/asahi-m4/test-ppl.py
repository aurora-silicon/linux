#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the actual M4 PPL RAII guard with concurrent hosts and bounded firmware waits.

The host mutex and firmware register transport are modeled. No hardware claim.
"""
from common import kernel_root, output_dir, rustc, sha
import json
import subprocess

root = kernel_root()
out = output_dir()
core = root / 'drivers/gpu/drm/asahi/mmu/ppl.rs'
mmu = root / 'drivers/gpu/drm/asahi/mmu.rs'
window = root / 'drivers/gpu/drm/asahi/g16/uat.rs'
source = r'''
#![allow(dead_code)]
mod production {
    #[path="__CORE__"] mod ppl;
    #[cfg(test)] mod tests {
        use super::ppl::{Guard, Memory};
        use std::sync::{Arc, Mutex, MutexGuard, TryLockError, mpsc};
        use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
        struct Registers {
            ap: AtomicBool, fw: AtomicBool, turn: AtomicBool, failed: AtomicBool,
            ticks: AtomicUsize, yield_after: usize, trace: Mutex<Vec<&'static str>>,
        }
        impl Registers {
            fn new(fw: bool, turn: bool, yield_after: usize) -> Self {
                Self { ap: AtomicBool::new(false), fw: AtomicBool::new(fw),
                    turn: AtomicBool::new(turn), failed: AtomicBool::new(false),
                    ticks: AtomicUsize::new(0), yield_after, trace: Mutex::new(Vec::new()) }
            }
            fn note(&self, s: &'static str) { self.trace.lock().unwrap().push(s); }
        }
        struct Host<'a> {
            _lock: MutexGuard<'a, ()>, registers: &'a Registers,
        }
        impl Drop for Host<'_> {
            fn drop(&mut self) {
                assert!(!self.registers.ap.load(Ordering::SeqCst), "host mutex released before AP withdrawal");
                if self.registers.fw.load(Ordering::SeqCst) {
                    assert!(self.registers.failed.load(Ordering::SeqCst), "timeout failure not latched before host unlock");
                }
                self.registers.note("host-unlock");
            }
        }
        impl Memory for Host<'_> {
            fn firmware_interested(&self) -> bool { self.registers.fw.load(Ordering::SeqCst) }
            fn firmware_turn(&self) -> bool { self.registers.turn.load(Ordering::SeqCst) }
            fn ap_interest(&self, v: bool) {
                self.registers.ap.store(v, Ordering::SeqCst);
                self.registers.note(if v { "ap=1" } else { "ap=0" });
            }
            fn give_firmware_turn(&self) {
                self.registers.turn.store(true, Ordering::SeqCst); self.registers.note("turn=fw");
            }
            fn barrier(&self) { self.registers.note("barrier"); }
            fn expired(&self) -> bool { self.registers.ticks.fetch_add(1, Ordering::SeqCst) >= 100 }
            fn relax(&self) {
                if self.registers.ticks.load(Ordering::SeqCst) >= self.registers.yield_after {
                    self.registers.turn.store(false, Ordering::SeqCst);
                    self.registers.fw.store(false, Ordering::SeqCst);
                }
            }
            fn fail(&self) { self.registers.failed.store(true, Ordering::SeqCst); self.registers.note("failed"); }
        }
        fn acquire<'a>(host: &'a Mutex<()>, registers: &'a Registers) -> Result<Guard<Host<'a>>, ()> {
            let lock = host.lock().unwrap();
            if registers.failed.load(Ordering::SeqCst) { return Err(()); }
            Guard::acquire(Host { _lock: lock, registers })
        }
        #[test] fn successful_guard_owns_host_lock_until_full_withdrawal() {
            let host = Mutex::new(()); let r = Registers::new(false, false, usize::MAX);
            let guard = acquire(&host, &r).unwrap();
            assert!(matches!(host.try_lock(), Err(TryLockError::WouldBlock)));
            assert!(r.ap.load(Ordering::SeqCst));
            drop(guard);
            assert!(host.try_lock().is_ok());
            assert_eq!(*r.trace.lock().unwrap(), vec!["ap=1","barrier","barrier","turn=fw","barrier","ap=0","barrier","host-unlock"]);
        }
        #[test] fn early_error_drops_both_exclusions() {
            fn operation(host: &Mutex<()>, r: &Registers) -> Result<(), ()> {
                let _guard = acquire(host, r)?; Err(())
            }
            let host = Mutex::new(()); let r = Registers::new(false, false, usize::MAX);
            assert!(operation(&host, &r).is_err());
            assert!(!r.ap.load(Ordering::SeqCst)); assert!(host.try_lock().is_ok());
        }
        #[test] fn competing_host_never_enters_or_clears_first_owners_interest() {
            let host = Arc::new(Mutex::new(()));
            let r = Arc::new(Registers::new(false, false, usize::MAX));
            let (entered_tx, entered_rx) = mpsc::channel();
            let (release_tx, release_rx) = mpsc::channel();
            let ah = host.clone(); let ar = r.clone();
            let a = std::thread::spawn(move || {
                let _guard = acquire(&ah, &ar).unwrap(); entered_tx.send(()).unwrap();
                release_rx.recv().unwrap();
            });
            entered_rx.recv().unwrap();
            let (blocked_tx, blocked_rx) = mpsc::channel();
            let bh = host.clone(); let br = r.clone();
            let b = std::thread::spawn(move || {
                assert!(matches!(bh.try_lock(), Err(TryLockError::WouldBlock)));
                assert!(br.ap.load(Ordering::SeqCst)); blocked_tx.send(()).unwrap();
                let _guard = acquire(&bh, &br).unwrap();
                assert!(br.ap.load(Ordering::SeqCst));
            });
            blocked_rx.recv().unwrap();
            assert!(r.ap.load(Ordering::SeqCst)); release_tx.send(()).unwrap();
            a.join().unwrap(); b.join().unwrap();
            assert!(!r.ap.load(Ordering::SeqCst));
            let trace = r.trace.lock().unwrap();
            assert_eq!(trace.iter().filter(|s| **s == "host-unlock").count(), 2);
            let boundary = trace.iter().position(|s| *s == "host-unlock").unwrap();
            assert_eq!(trace[boundary + 1], "ap=1");
        }
        #[test] fn firmware_busy_timeout_cleans_interest_and_latches_before_unlock() {
            let host = Mutex::new(()); let r = Registers::new(true, false, usize::MAX);
            assert!(acquire(&host, &r).is_err()); assert!(host.try_lock().is_ok());
            assert!(r.failed.load(Ordering::SeqCst)); assert!(!r.ap.load(Ordering::SeqCst));
            let trace = r.trace.lock().unwrap();
            assert_eq!(&trace[trace.len()-4..], ["ap=0","barrier","failed","host-unlock"]);
        }
        #[test] fn yielded_turn_timeout_also_cleans_interest() {
            let host = Mutex::new(()); let r = Registers::new(true, true, usize::MAX);
            assert!(acquire(&host, &r).is_err()); assert!(r.failed.load(Ordering::SeqCst));
            assert!(!r.ap.load(Ordering::SeqCst));
            assert!(r.ticks.load(Ordering::SeqCst) <= 102);
        }
        #[test] fn firmware_yield_reacquires_before_entering() {
            let host = Mutex::new(()); let r = Registers::new(true, true, 2);
            let guard = acquire(&host, &r).unwrap(); assert!(!r.failed.load(Ordering::SeqCst));
            assert!(r.ap.load(Ordering::SeqCst));
            assert_eq!(&r.trace.lock().unwrap()[..6], ["ap=1","barrier","ap=0","barrier","ap=1","barrier"]);
            drop(guard);
        }
        #[test] fn later_host_cannot_republish_after_timeout() {
            let host = Mutex::new(()); let r = Registers::new(true, false, usize::MAX);
            assert!(acquire(&host, &r).is_err());
            let before = r.trace.lock().unwrap().len();
            r.fw.store(false, Ordering::SeqCst);
            assert!(acquire(&host, &r).is_err());
            assert_eq!(r.trace.lock().unwrap().len(), before);
        }
    }
}
'''.replace('__CORE__', str(core))

# Guard the integration seams as well as executing the actual protocol. Register writes
# must have a single implementation, and every live Window transaction must use it.
mm = mmu.read_text()
win = window.read_text()
assert 'shared: Guard<\'a, UatShared, MutexBackend>' in mm
assert 'fn lock_handoff(&self) -> Result<HandoffGuard' in mm
assert 'ppl::Guard::acquire(transport)' in mm
assert 'failed: &self.failed' in mm
assert 'fn try_lock_m4(' not in mm and 'fn unlock_m4(' not in mm
assert 'fn ppl_lock(' not in win and 'fn ppl_unlock(' not in win
assert win.count('uat.lock_m4_handoff()?') == 2
assert 'self.uat_inner.lock_handoff()' in mm and 'self.inner.lock_handoff()' in mm

path = out / 'ppl.rs'
path.write_text(source)
binary = out / 'ppl-tests'
subprocess.run(rustc() + ['--edition=2021', '--test', str(path), '-o', str(binary)], check=True)
run = subprocess.run([str(binary), '--test-threads=1'], check=True, text=True, capture_output=True)
print(run.stdout, end='')
report = {'passed': True, 'test_groups': 7, 'hardware_validated': False,
          'production_sha256': {str(p.relative_to(root)): sha(p) for p in [core, mmu, window]}}
(out / 'ppl-results.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
