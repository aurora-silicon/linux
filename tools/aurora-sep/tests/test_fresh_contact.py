# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute the real contact gate and capture wait with scripted SPI/IRQ IO."""
from pathlib import Path
import subprocess
import tempfile
import unittest

APPLE = Path(__file__).resolve().parents[3] / 'drivers/soc/apple'


def item(source, prefix):
    begin = source.index(prefix)
    opening = source.index('{', begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


PRELUDE = r'''
#![allow(dead_code)]
use std::cell::RefCell;
use std::collections::VecDeque;
use std::sync::{Mutex, atomic::{AtomicU64, Ordering::Relaxed}};
const ENROL_POLL_MS: u32 = 2;
const ENROL_IRQ_WAIT_MS: u32 = 250;
const ENROL_POLL_ATTEMPTS: u32 = 1000;
#[derive(Debug, PartialEq)]
enum CaptureWait { Ready(u32), RemoveFinger, Timeout, Fault(u8), Abandon }
#[derive(Default)]
struct Mock {
    script: VecDeque<Result<u8, ()>>, clock: u64, irq: bool, verify: bool,
    cancel_at: Option<u64>, guides: Vec<bio::Guidance>, drained: u32, wiped: u32,
    rearms: u32, idle_fail: bool, arm_fail: bool, read_fail: u8,
}
thread_local! { static MOCK: RefCell<Mock> = RefCell::new(Mock::default()); }
mod shim {
    pub fn monotonic_ns() -> u64 { super::MOCK.with(|m| m.borrow().clock) }
}
mod kernel { pub mod time {
    pub struct Delta(pub i64);
    impl Delta { pub fn from_millis(ms: i64) -> Self { Self(ms) } }
    pub mod delay {
        pub fn fsleep(d: super::Delta) { crate::advance(d.0 as u64); }
    }
}}
fn advance(ms: u64) { MOCK.with(|m| m.borrow_mut().clock += ms * 1_000_000); }
mod bio {
    #[derive(Clone, Copy, Debug, PartialEq)]
    pub enum Guidance { Place, HoldStill, RemoveFinger }
    pub struct Session;
    pub fn capture_is_live(_: &Session, _: u64) -> bool {
        super::MOCK.with(|m| { let m=m.borrow(); m.cancel_at.is_none_or(|n| m.clock < n) })
    }
    pub fn verify_is_live(s: &Session, e: u64) -> bool {
        capture_is_live(s, e) && super::MOCK.with(|m| m.borrow().verify)
    }
    pub fn capture_guide(_: &mut Session, _: u64, guide: Guidance) -> bool {
        super::MOCK.with(|m| { let mut m=m.borrow_mut();
            if m.guides.last() == Some(&guide) { false }
            else { m.guides.push(guide); true }
        })
    }
}
mod sensor {
    pub const STATE_IDLE: u8 = 0;
    pub const STATE_ARMED: u8 = 17;
    pub const STATE_READING: u8 = 19;
    pub const STATE_DATA_READY: u8 = 7;
    pub const STATE_NEEDS_PATCH: u8 = 9;
    pub const STATUS_LEN: usize = 16;
    pub struct Status { pub state: u8, pub raw: [u8; STATUS_LEN] }
    pub enum Offset12 { Available(u32), Undefined }
    impl Status {
        pub fn offset12(&self) -> Offset12 {
            if self.state == STATE_DATA_READY { Offset12::Available(32) }
            else { Offset12::Undefined }
        }
    }
    pub fn status() -> Result<Status, ()> {
        super::MOCK.with(|m| {
            let state = m.borrow_mut().script.pop_front().unwrap_or(Ok(STATE_READING))?;
            Ok(Status { state, raw: [state; STATUS_LEN] })
        })
    }
    pub fn irq_available() -> bool { super::MOCK.with(|m| m.borrow().irq) }
    pub fn irq_arm() {}
    pub fn irq_wait(ms: u32) -> bool { super::advance(ms.into()); false }
    pub struct Capture;
    impl Drop for Capture {
        fn drop(&mut self) { super::MOCK.with(|m| m.borrow_mut().wiped += 1); }
    }
    pub enum CaptureError { Checksum { advertised: u16, computed: u16 }, Length(u32), Bus, NoMemory }
    pub fn read_capture(_: u32) -> Result<Capture, CaptureError> {
        super::MOCK.with(|m| { let mut m=m.borrow_mut(); m.drained += 1;
            match m.read_fail {
                1 => Err(CaptureError::Checksum { advertised: 1, computed: 2 }),
                2 => Err(CaptureError::Length(32)),
                3 => Err(CaptureError::Bus),
                4 => Err(CaptureError::NoMemory),
                _ => Ok(Capture),
            }
        })
    }
    pub fn idle() -> Result<(), ()> {
        super::MOCK.with(|m| if m.borrow().idle_fail { Err(()) } else { Ok(()) })
    }
    pub fn start_capture() -> Result<(), ()> {
        super::MOCK.with(|m| { let mut m=m.borrow_mut(); m.rearms += 1;
            if m.arm_fail { Err(()) } else { Ok(()) }
        })
    }
@CONTACT@
}
struct SessionLock(Mutex<bio::Session>);
impl SessionLock { fn lock(&self) -> std::sync::MutexGuard<'_, bio::Session> { self.0.lock().unwrap() } }
struct Driver { bio_session: SessionLock, capture_epoch: AtomicU64 }
impl Driver {
    fn bio_wake(&self) {}
@WAIT@
}
fn run(states: &[u8], irq: bool, verify: bool, setup: impl FnOnce(&mut Mock)) -> (CaptureWait, Mock) {
    MOCK.with(|m| { let mut m=m.borrow_mut(); *m=Mock::default();
        m.script = states.iter().copied().map(Ok).collect(); m.irq=irq; m.verify=verify; setup(&mut m);
    });
    let d=Driver { bio_session: SessionLock(Mutex::new(bio::Session)), capture_epoch: AtomicU64::new(1) };
    let result=d.await_capture();
    (result, MOCK.with(|m| std::mem::take(&mut *m.borrow_mut())))
}
#[test]
fn clean_finger_is_not_discarded() {
    for irq in [false, true] {
        let (r,m)=run(&[17,19,7],irq,true,|_|{});
        assert_eq!(r,CaptureWait::Ready(32)); assert_eq!(m.drained,0);
        assert_eq!(m.guides,[bio::Guidance::Place,bio::Guidance::HoldStill]);
    }
}
#[test]
fn held_verify_and_fast_ready_never_reach_processing() {
    for states in [&[19,7][..],&[7][..],&[19,17,7][..]] {
        for irq in [false,true] {
            let (r,m)=run(states,irq,true,|_|{});
            assert_eq!(r,CaptureWait::RemoveFinger);
            assert_eq!((m.drained,m.rearms),(0,0));
            assert_eq!(m.guides,[bio::Guidance::RemoveFinger]);
        }
    }
}
#[test]
fn enrol_discards_and_wipes_then_waits_for_fresh_contact() {
    for irq in [false,true] {
        let (r,m)=run(&[19,7,19,7,17,19,7],irq,false,|_|{});
        assert_eq!(r,CaptureWait::Ready(32));
        assert_eq!((m.drained,m.wiped,m.rearms),(2,2,2));
        assert_eq!(m.guides,[bio::Guidance::RemoveFinger,bio::Guidance::Place,bio::Guidance::HoldStill]);
    }
}
#[test]
fn bad_frame_is_discarded_but_bus_and_memory_faults_abort() {
    for fail in 1..=4 {
        let (r,m)=run(&[19,7,17,7],false,false,|m|m.read_fail=fail);
        assert_eq!(r,if fail <= 2 {CaptureWait::Ready(32)} else {CaptureWait::Abandon});
        assert_eq!(m.wiped,0);
    }
}
#[test]
fn cancel_status_fault_and_rearm_failures_stop() {
    let (r,m)=run(&[19,7],false,false,|m|m.cancel_at=Some(1));
    assert_eq!(r,CaptureWait::Abandon); assert_eq!(m.rearms,0);
    assert_eq!(run(&[9],false,false,|_|{}).0,CaptureWait::Fault(9));
    assert_eq!(run(&[],false,false,|m|m.script.push_back(Err(()))).0,CaptureWait::Abandon);
    for idle in [false,true] {
        let (r,_)=run(&[19,7],false,false,|m|{m.idle_fail=idle;m.arm_fail=!idle;});
        assert_eq!(r,CaptureWait::Abandon);
    }
}
#[test]
fn repeated_stale_frames_do_not_extend_deadline() {
    for irq in [false,true] {
        let states: Vec<u8>=(0..2000).flat_map(|_|[19,7]).collect();
        let (r,m)=run(&states,irq,false,|_|{});
        assert_eq!(r,CaptureWait::Timeout);
        assert_eq!(m.clock,2_000_000_000);
        assert!(m.rearms < 2000);
    }
}
'''


BUFFER_HARNESS = r'''
#![allow(dead_code)]
use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};
const MAX_CAPTURE: usize = 0x10000;
const CRC_LEN: usize = 2;
const CMD_LEN: usize = 7;
const READ_CMD_LEN: usize = 11;
const CMD_READ_PREFIX: [u8; CMD_LEN] = [0; CMD_LEN];
const GFP_KERNEL: u32 = 0;
static WIPED: AtomicU32 = AtomicU32::new(0);
static MODE: AtomicU32 = AtomicU32::new(0);
struct Error(i32);
impl Error { fn from_errno(n: i32) -> Self { Self(n) } }
struct KVec<T: Copy + Default + PartialEq>(Vec<T>);
impl<T: Copy + Default + PartialEq> KVec<T> {
    fn with_capacity(n: usize, _: u32) -> Result<Self, ()> { Ok(Self(Vec::with_capacity(n))) }
    fn resize(&mut self, n: usize, value: T, _: u32) -> Result<(), ()> { self.0.resize(n, value); Ok(()) }
}
impl<T: Copy + Default + PartialEq> std::ops::Deref for KVec<T> {
    type Target = [T]; fn deref(&self) -> &[T] { &self.0 }
}
impl<T: Copy + Default + PartialEq> std::ops::DerefMut for KVec<T> {
    fn deref_mut(&mut self) -> &mut [T] { &mut self.0 }
}
impl<T: Copy + Default + PartialEq> Drop for KVec<T> {
    fn drop(&mut self) {
        assert!(self.0.iter().all(|v| *v == T::default()), "capture buffer freed without wiping");
        WIPED.fetch_add(1, Relaxed);
    }
}
unsafe fn sep_sensor_xfer2(_: *const c_void, _: usize, rx: *mut c_void, len: usize) -> i32 {
    let buf = unsafe { std::slice::from_raw_parts_mut(rx.cast::<u8>(), len) };
    buf.fill(0xa5);
    let split = len - CRC_LEN;
    let crc = crc16_ansi(&buf[..split]);
    buf[split..].copy_from_slice(&crc.to_le_bytes());
    match MODE.load(Relaxed) {
        1 => { buf[0] ^= 1; 0 },
        2 => -5,
        _ => 0,
    }
}
@ITEMS@
#[test]
fn success_checksum_and_bus_error_all_wipe() {
    for mode in 0..=2 {
        MODE.store(mode, Relaxed);
        match (mode, read_capture(32)) {
            (0, Ok(capture)) => drop(capture),
            (1, Err(CaptureError::Checksum { .. })) => {},
            (2, Err(CaptureError::Bus(_))) => {},
            _ => panic!("unexpected capture outcome"),
        }
        assert_eq!(WIPED.load(Relaxed), mode + 1);
    }
    for length in [0, 1, 2, MAX_CAPTURE as u32 + 1] {
        assert!(matches!(read_capture(length), Err(CaptureError::Length(_))));
    }
    assert_eq!(WIPED.load(Relaxed), 3);
}
'''

class FreshContactTest(unittest.TestCase):
    def test_real_capture_wait(self):
        sensor = (APPLE / 'sensor.rs').read_text()
        contact = item(sensor, 'pub(crate) struct CaptureContact {') + '\n' + item(sensor, 'impl CaptureContact {')
        wait = item((APPLE / 'sbio.rs').read_text(), '    fn await_capture(')
        program = PRELUDE.replace('@CONTACT@', contact).replace('@WAIT@', wait)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.rs').write_text(program)
            subprocess.run(['rustc', '--edition=2021', '--test', str(root / 'test.rs'), '-o', str(root / 'test')], check=True)
            subprocess.run([str(root / 'test')], check=True)


    def test_real_capture_buffer_wiping(self):
        sensor = (APPLE / 'sensor.rs').read_text()
        declaration = sensor.index('pub(crate) struct Capture(')
        items = [sensor[declaration:sensor.index(';', declaration) + 1]]
        items.extend(item(sensor, prefix) for prefix in (
            'fn crc16_ansi(', 'impl Capture {', 'impl Drop for Capture {',
            'pub(crate) enum CaptureError {', 'pub(crate) fn read_capture('))
        program = BUFFER_HARNESS.replace('@ITEMS@', '\n'.join(items))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.rs').write_text(program)
            subprocess.run(['rustc', '--edition=2021', '--test', str(root / 'test.rs'), '-o', str(root / 'test')], check=True)
            subprocess.run([str(root / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
