# SPDX-License-Identifier: MIT
"""Run the driver's actual SBIO transfer method from two threads at once.

The verify/enrol work item and the ioctl path (GET_INFO and ENROL_START list
the enclave's identities, DELETE removes one) can reach the single SBIO
endpoint at the same time, e.g. when libfprint cancels an identify and starts
the next one while the previous work item is still registering the sensor.
Each exchange must complete with its own reply; none may be refused with
EBUSY or receive another caller's answer.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

APPLE = Path(__file__).resolve().parents[3] / 'drivers/soc/apple'


def method(file, name):
    source = (APPLE / file).read_text()
    match = re.search(r'^    (?:pub\(crate\) )?fn ' + name + r'\(', source, re.M)
    if match is None:
        raise AssertionError(f'Missing driver method {name}')
    opening = source.index('{', match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


HARNESS = r'''
#![allow(dead_code, unused_variables, unused_imports)]
use std::sync::atomic::{AtomicBool, Ordering::Relaxed};
use std::sync::mpsc;
use std::time::Duration;

type CStr = str;
type Result<T> = std::result::Result<T, i32>;
const ENODEV: i32 = -19;
const EBUSY: i32 = -16;
const ETIMEDOUT: i32 = -110;
const SBIO_TIMEOUT_MS: u64 = 2000;

mod time { pub fn msecs_to_jiffies(ms: u64) -> u64 { ms } }

pub enum CondVarTimeoutResult { Woken { jiffies: u64 }, Signal { jiffies: u64 }, Timeout }

/// Kernel-shaped mutex: lock() returns a guard a CondVar can wait on.
struct Mutex<T>(std::sync::Mutex<T>);
struct Guard<'a, T>(Option<std::sync::MutexGuard<'a, T>>);
impl<T> Mutex<T> {
    fn new(v: T) -> Self { Mutex(std::sync::Mutex::new(v)) }
    fn lock(&self) -> Guard<'_, T> { Guard(Some(self.0.lock().unwrap())) }
}
impl<'a, T> std::ops::Deref for Guard<'a, T> {
    type Target = T;
    fn deref(&self) -> &T { self.0.as_ref().unwrap() }
}
impl<'a, T> std::ops::DerefMut for Guard<'a, T> {
    fn deref_mut(&mut self) -> &mut T { self.0.as_mut().unwrap() }
}
struct CondVar(std::sync::Condvar);
impl CondVar {
    fn wait_interruptible_timeout<T>(&self, g: &mut Guard<'_, T>, j: u64) -> CondVarTimeoutResult {
        let inner = g.0.take().unwrap();
        let (inner, r) = self.0.wait_timeout(inner, Duration::from_millis(j)).unwrap();
        g.0 = Some(inner);
        if r.timed_out() { CondVarTimeoutResult::Timeout } else { CondVarTimeoutResult::Woken { jiffies: j / 2 } }
    }
    fn notify_all(&self) { self.0.notify_all() }
}

mod transfer {
    pub struct Completed { pub status: u32, pub payload: Vec<u8> }
    /// Same state rules as drivers/soc/apple/transfer.rs Reassembly.
    pub struct Reassembly { pub active: Option<u32>, pub done: Option<Completed> }
    impl Reassembly {
        pub fn begin(&mut self, opcode: u32) -> super::Result<()> {
            if self.active.is_some() { return Err(super::EBUSY); }
            self.done = None;
            self.active = Some(opcode);
            Ok(())
        }
        pub fn take_done(&mut self) -> Option<Completed> { self.done.take() }
        pub fn abort(&mut self) {
            if self.active.take().is_some() {
                self.done = Some(Completed { status: 0xdead, payload: Vec::new() });
            }
        }
        /// The enclave's reply: it echoes the opcode in the payload.
        pub fn on_reply(&mut self, opcode: u32) {
            if self.active == Some(opcode) {
                self.active = None;
                self.done = Some(Completed { status: 0, payload: opcode.to_le_bytes().to_vec() });
            }
        }
    }
}

struct SepData {
    shutting_down: AtomicBool,
    sbio_ready: AtomicBool,
    sbio_rx: Mutex<transfer::Reassembly>,
    sbio_wq: CondVar,
    sbio_exchange_lock: Mutex<()>,
    to_enclave: std::sync::Mutex<mpsc::Sender<u32>>,
}

impl SepData {
    fn sbio_start(&self, opcode: u16, _name: &'static CStr, _payload: &[u8]) -> Result<()> {
        self.to_enclave.lock().unwrap().send(u32::from(opcode)).unwrap();
        Ok(())
    }
METHOD
}

fn main() {
    let (tx, rx) = mpsc::channel::<u32>();
    let sep: &'static SepData = Box::leak(Box::new(SepData {
        shutting_down: AtomicBool::new(false),
        sbio_ready: AtomicBool::new(true),
        sbio_rx: Mutex::new(transfer::Reassembly { active: None, done: None }),
        sbio_wq: CondVar(std::sync::Condvar::new()),
        sbio_exchange_lock: Mutex::new(()),
        to_enclave: std::sync::Mutex::new(tx),
    }));
    // The enclave answers each request after a short delay, as the mailbox does.
    std::thread::spawn(move || {
        for opcode in rx {
            std::thread::sleep(Duration::from_millis(30));
            sep.sbio_rx.lock().on_reply(opcode);
            sep.sbio_wq.notify_all();
        }
    });
    let mut failures = 0;
    for round in 0..20u16 {
        let worker = std::thread::spawn(move || {
            // e.g. CLEAR_STATE from the work item's sensor registration
            sep.sbio_transfer_raw(0x10 + round, "WORKER", &[])
                .map(|d| (d.status, d.payload))
        });
        std::thread::sleep(Duration::from_millis(5));
        // e.g. LIST_IDENTITIES from GET_INFO on the next identify
        let ioctl = sep.sbio_transfer_raw(0x40 + round, "IOCTL", &[]).map(|d| (d.status, d.payload));
        let worker = worker.join().unwrap();
        let want_w = Ok((0, u32::from(0x10 + round).to_le_bytes().to_vec()));
        let want_i = Ok((0, u32::from(0x40 + round).to_le_bytes().to_vec()));
        if worker != want_w || ioctl != want_i {
            eprintln!("round {round}: worker {:?} ioctl {:?}", worker, ioctl);
            failures += 1;
        }
    }
    assert_eq!(failures, 0, "concurrent SBIO exchanges interfered");
}
'''


@unittest.skipUnless(shutil.which('rustc'), 'rustc required for driver method tests')
class SbioExchangeTest(unittest.TestCase):
    def test_concurrent_exchanges_each_get_their_own_reply(self):
        program = HARNESS.replace('METHOD', method('sbio.rs', 'sbio_transfer_raw'))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.rs').write_text(program)
            subprocess.run(['rustc', '--edition=2021', str(root / 'test.rs'),
                            '-o', str(root / 'test')], check=True)
            subprocess.run([str(root / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
