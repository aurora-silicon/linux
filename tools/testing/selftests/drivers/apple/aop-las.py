#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production LAS decoder and IIO read callback with host Rust mocks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def const(source, name):
    start = source.index(f"const {name}")
    return source[start:source.index(";", start) + 1]


def main():
    root = Path(__file__).resolve().parents[5]
    common = (root / "rust/kernel/iio/common/aop_sensors.rs").read_text()
    las = (root / "drivers/iio/common/aop_sensors/aop_las.rs").read_text()
    parts = [r'''
#![allow(non_camel_case_types)]
use std::sync::atomic::Ordering::{Acquire, Relaxed, Release};
const MICRO: u64 = 1_000_000;
const MAX_MICRO: u64 = i32::MAX as u64 * MICRO + MICRO - 1;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Error(i32);
impl Error { fn to_errno(&self) -> i32 { self.0 } }
const EINVAL: Error = Error(-22);
const ENODATA: Error = Error(-61);
const EIO: Error = Error(-5);
const ETIMEDOUT: Error = Error(-110);
type Result<T = ()> = std::result::Result<T, Error>;
mod bindings {
    pub const BINDINGS_IIO_CHAN_INFO_RAW: u32 = 0;
    pub const BINDINGS_IIO_CHAN_INFO_PROCESSED: u32 = 1;
    pub const BINDINGS_IIO_CHAN_INFO_SCALE: u32 = 2;
    pub const IIO_VAL_INT: u32 = 1;
    pub const IIO_VAL_INT_PLUS_MICRO: u32 = 2;
    pub const IIO_VAL_INT_PLUS_NANO: u32 = 3;
    pub struct iio_dev { pub priv_: *const std::ffi::c_void }
    pub struct iio_chan_spec { pub type_: u32, pub address: u64 }
}
/// Host stand-in for `kernel::sync::atomic::Atomic`: only load/store are used.
struct Atomic<T: Copy>(std::sync::Mutex<T>);
impl<T: Copy> Atomic<T> {
    fn new(v: T) -> Self { Atomic(std::sync::Mutex::new(v)) }
    fn load(&self, _: std::sync::atomic::Ordering) -> T { *self.0.lock().unwrap() }
    fn store(&self, v: T, _: std::sync::atomic::Ordering) { *self.0.lock().unwrap() = v; }
}
struct Arc<T>(std::marker::PhantomData<T>);
impl<T> Arc<T> {
    unsafe fn borrow<'a>(p: *const std::ffi::c_void) -> &'a T {
        unsafe { &*p.cast::<T>() }
    }
}
trait FakehidListener {
    fn process_fakehid_report(&self, data: &[u8]) -> Result<()>;
}
struct AopSensorData<T: MessageProcessor, const N: usize = 1> {
    ty: [u32; N], value: [Atomic<u64>; N], valid: [Atomic<bool>; N], msg_proc: T,
}
struct MsgProc;
''', const(las, "MAX_ONDEMAND_INTERVAL_US"),
        const(las, "DIRECT_IDLE_MS"),
        block(las, "fn first_sample_wait_ms("),
        block(las, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum SyncAction"),
        block(las, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum IdleStep"),
        block(las, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum RunState"),
        block(las, "trait IntervalIo"),
        block(las, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum ProbeEvent"),
        block(las, "struct Stream<T>"),
        block(las, "impl<T> Stream<T>"),
        block(common, "pub trait MessageProcessor"),
        block(las, "impl MessageProcessor for MsgProc"),
        block(common, "impl<T: MessageProcessor, const N: usize> FakehidListener"),
        block(common, 'unsafe extern "C" fn aop_read_raw'), r'''
fn sensor<T: MessageProcessor>(msg_proc: T) -> AopSensorData<T> {
    AopSensorData {
        ty: [17], value: [Atomic::new(0)], valid: [Atomic::new(false)], msg_proc,
    }
}
fn read_chan<T: MessageProcessor + 'static>(data: &AopSensorData<T>, ty: u32, address: u64,
                                           mask: u32) -> (i32, i32, i32) {
    let mut dev = bindings::iio_dev { priv_: (data as *const AopSensorData<T>).cast() };
    let chan = bindings::iio_chan_spec { type_: ty, address };
    let (mut val, mut val2) = (-1, -1);
    let ret = unsafe { aop_read_raw::<T, 1>(&mut dev, &chan, &mut val, &mut val2, mask as isize) };
    (ret, val, val2)
}
fn read<T: MessageProcessor + 'static>(data: &AopSensorData<T>, mask: u32) -> (i32, i32, i32) {
    read_chan(data, data.ty[0], 0, mask)
}
#[test]
fn full_angle_and_padding() {
    // Synthetic reports: cover bit 8, both limits, invalid values and every padding pattern.
    for wire in 0..=u16::MAX {
        let bytes = wire.to_le_bytes();
        let angle = wire & 0x1ff;
        let expected = if angle <= 360 { Some(u64::from(angle) * MICRO) } else { None };
        assert_eq!(MsgProc.process(&[1, bytes[0], bytes[1]]), expected);
    }
    assert_eq!(MsgProc.process(&[1, 99, 0]), Some(99 * MICRO));
    assert_eq!(MsgProc.process(&[1, 0, 1]), Some(256 * MICRO));
    assert_eq!(MsgProc.process(&[1, 104, 1]), Some(360 * MICRO));
}
#[test]
fn report_identity_and_truncation() {
    for id in 0..=u8::MAX {
        if id != 1 { assert_eq!(MsgProc.process(&[id, 99, 0]), None); }
    }
    for truncated in [&[][..], &[1][..], &[1, 99][..]] {
        assert_eq!(MsgProc.process(truncated), None);
    }
}
#[test]
fn raw_scale_and_last_valid_sample() {
    use bindings::*;
    let data = sensor(MsgProc);
    assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_RAW).0, -61);
    assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_SCALE),
               (IIO_VAL_INT_PLUS_NANO as i32, 0, 17_453_293));
    data.process_fakehid_report(&[2, 39]).unwrap();
    assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_RAW).0, -61);
    data.process_fakehid_report(&[1, 99, 0]).unwrap();
    assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_RAW), (1, 99, 0));
    for report in [&[3, 0, 0, 0, 0, 1][..], &[1, 105, 1][..], &[1, 42][..]] {
        data.process_fakehid_report(report).unwrap();
        assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_RAW), (1, 99, 0));
    }
    // A channel the registration did not create is rejected, reading or not.
    assert_eq!(read_chan(&data, 17, 1, BINDINGS_IIO_CHAN_INFO_RAW).0, -22);
    assert_eq!(read_chan(&data, 18, 0, BINDINGS_IIO_CHAN_INFO_RAW).0, -22);
    let (_, int, nano) = read(&data, BINDINGS_IIO_CHAN_INFO_SCALE);
    let radians = 99.0 * (int as f64 + nano as f64 / 1e9);
    assert!((radians - 99.0_f64.to_radians()).abs() < 1e-7);
}
struct Processed;
impl MessageProcessor for Processed {
    fn process(&self, _: &[u8]) -> Option<u64> { Some(12_345_678) }
}
#[test]
fn processed_channel_unchanged() {
    use bindings::*;
    let data = sensor(Processed);
    assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_PROCESSED).0, -61);
    data.process_fakehid_report(&[]).unwrap();
    assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_PROCESSED), (2, 12, 345_678));
    assert_eq!(read(&data, BINDINGS_IIO_CHAN_INFO_SCALE).0, -22);
    assert_eq!(read(&data, 999).0, -22);
}
#[test]
fn on_demand_stream_lifecycle_and_passive_fallback() {
    assert_eq!(first_sample_wait_ms(100_000), 600);
    assert_eq!(first_sample_wait_ms(1_000_000), 1_500);
    assert_eq!(first_sample_wait_ms(2_000_000), 2_500);

    // Passive fallback (interval unreadable): never emits start/stop actions.
    let mut passive = Stream::<u64>::new();
    passive.direct_until = Some(5_000);
    assert_eq!(passive.next_action(), SyncAction::None);
    assert_eq!(passive.check_idle(0), IdleStep::Ignore);

    // On-demand lifecycle: idle off -> in-flight read starts -> idle rearms while reading -> expires -> stops.
    let mut s = Stream::<u64>::new();
    s.interval_us = Some(1_000_000);
    s.restore_interval = Some(1_000_000);
    assert_eq!(s.restore_interval, Some(1_000_000));
    assert_eq!(s.next_action(), SyncAction::None);
    s.direct_readers = 1;
    assert_eq!(s.next_action(), SyncAction::Start(1_000_000));
    s.run_state = RunState::Running;
    s.direct_until = Some(5_000);
    assert_eq!(s.check_idle(0), IdleStep::RearmMs(DIRECT_IDLE_MS as u32));
    s.direct_readers = 0;
    assert_eq!(s.check_idle(2_000), IdleStep::RearmMs(2_000));
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.run_state = RunState::Stopped;
    assert_eq!(s.next_action(), SyncAction::None);

    // Firmware reset on wake or unacknowledged stop marks Unknown:
    // idle reasserts Stop, while a new reader reasserts Start.
    s.parked = false;
    s.run_state = RunState::Unknown;
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.direct_readers = 1;
    assert_eq!(s.next_action(), SyncAction::Start(1_000_000));
    s.direct_readers = 0;
    s.run_state = RunState::Stopped;

    // Failed start backoff.
    s.failed_until = Some((5_000, Error(-5)));
    assert_eq!(s.check_backoff(500), Some(Error(-5)));
    assert_eq!(s.check_backoff(0), None);
}

struct MockAop {
    fw_interval: std::cell::Cell<u32>,
    reads: std::cell::RefCell<std::collections::VecDeque<Result<Option<u32>>>>,
    writes: std::cell::RefCell<std::collections::VecDeque<(bool, Result<u32>)>>,
    write_log: std::cell::RefCell<Vec<u32>>,
}

impl MockAop {
    fn new(
        initial: u32,
        reads: Vec<Result<Option<u32>>>,
        writes: Vec<(bool, Result<u32>)>,
    ) -> Self {
        Self {
            fw_interval: std::cell::Cell::new(initial),
            reads: std::cell::RefCell::new(reads.into()),
            writes: std::cell::RefCell::new(writes.into()),
            write_log: std::cell::RefCell::new(Vec::new()),
        }
    }
}

impl IntervalIo for MockAop {
    fn get_interval(&self) -> Result<Option<u32>> {
        if let Some(res) = self.reads.borrow_mut().pop_front() {
            return res;
        }
        Ok(Some(self.fw_interval.get()))
    }

    fn set_interval_status(&self, interval: u32) -> Result<u32> {
        self.write_log.borrow_mut().push(interval);
        if let Some((apply, res)) = self.writes.borrow_mut().pop_front() {
            if apply {
                self.fw_interval.set(interval);
            }
            return res;
        }
        self.fw_interval.set(interval);
        Ok(0)
    }

    fn set_interval(&self, interval: u32) -> Result {
        match self.set_interval_status(interval)? {
            0 => Ok(()),
            _ => Err(EIO),
        }
    }
}

#[test]
fn probe_applied_stop_failed_readback_and_failed_restore_recovers_on_read() {
    // 1) Probe reads 1_000_000 us.
    // 2) Stop write applies 0 in firmware, but its reply times out.
    // 3) Readback also times out.
    // 4) First restore write fails (timeout).
    let aop = MockAop::new(
        1_000_000,
        vec![Ok(Some(1_000_000)), Err(ETIMEDOUT)],
        vec![(true, Err(ETIMEDOUT)), (false, Err(ETIMEDOUT))],
    );
    let mut s = Stream::<u64>::new();
    let (event, retry) = s.probe_with(&aop, 5_000);
    assert_eq!(event, ProbeEvent::StopAmbiguousRecoverable(ETIMEDOUT, 1_000_000));
    assert!(retry);
    assert_eq!(s.interval_us, Some(1_000_000));
    assert_eq!(s.restore_interval, Some(1_000_000));
    assert_eq!(s.run_state, RunState::Unknown);
    assert_eq!(aop.fw_interval.get(), 0);
    assert_eq!(s.direct_until, Some(5_000));

    // A direct read must NOT stay passive; it restarts firmware reports and
    // transitions cleanly to Running -> Stopped -> restored on shutdown.
    s.direct_readers = 1;
    assert_eq!(s.sync_with(&aop), Ok(()));
    assert_eq!(s.run_state, RunState::Running);
    assert_eq!(aop.fw_interval.get(), 1_000_000);

    s.direct_readers = 0;
    s.direct_until = Some(10_000);
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert_eq!(s.sync_with(&aop), Ok(()));
    assert_eq!(s.run_state, RunState::Stopped);
    assert_eq!(aop.fw_interval.get(), 0);

    assert_eq!(s.shutdown_with(&aop), Ok(()));
    assert_eq!(aop.fw_interval.get(), 1_000_000);
    assert_eq!(s.restore_interval, None);
    assert_eq!(s.interval_us, None);
}

#[test]
fn probe_applied_stop_failed_readback_and_refused_restore_recovers_on_idle_retry() {
    let aop = MockAop::new(
        1_000_000,
        vec![Ok(Some(1_000_000)), Ok(None)],
        vec![(true, Err(ETIMEDOUT)), (false, Ok(0xe000_02bc))],
    );
    let mut s = Stream::<u64>::new();
    let (event, retry) = s.probe_with(&aop, 5_000);
    assert_eq!(event, ProbeEvent::StopAmbiguousRecoverable(ETIMEDOUT, 1_000_000));
    assert!(retry);
    assert_eq!(s.interval_us, Some(1_000_000));
    assert_eq!(s.run_state, RunState::Unknown);
    assert_eq!(aop.fw_interval.get(), 0);

    // Without any direct read, the armed idle retry reasserts Stop.
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert_eq!(s.sync_with(&aop), Ok(()));
    assert_eq!(s.run_state, RunState::Stopped);
    assert_eq!(aop.fw_interval.get(), 0);

    // A later direct read starts reports again and shutdown restores 1_000_000.
    s.direct_readers = 1;
    assert_eq!(s.sync_with(&aop), Ok(()));
    assert_eq!(aop.fw_interval.get(), 1_000_000);
    s.direct_readers = 0;
    assert_eq!(s.shutdown_with(&aop), Ok(()));
    assert_eq!(aop.fw_interval.get(), 1_000_000);
}

#[test]
fn probe_reconciliation_variants() {
    // Timed-out stop that did apply 0, confirmed by readback -> on-demand Stopped.
    let aop = MockAop::new(
        1_000_000,
        vec![Ok(Some(1_000_000)), Ok(Some(0))],
        vec![(true, Err(ETIMEDOUT))],
    );
    let mut s = Stream::<u64>::new();
    let (event, retry) = s.probe_with(&aop, 5_000);
    assert_eq!(event, ProbeEvent::StopTimedOutStopped(ETIMEDOUT));
    assert!(!retry);
    assert_eq!(s.interval_us, Some(1_000_000));
    assert_eq!(s.restore_interval, Some(1_000_000));
    assert_eq!(s.run_state, RunState::Stopped);

    // Ambiguous stop/readback where first restore succeeds -> passive, untouched on shutdown.
    let aop2 = MockAop::new(
        1_000_000,
        vec![Ok(Some(1_000_000)), Err(ETIMEDOUT)],
        vec![(true, Err(ETIMEDOUT)), (true, Ok(0))],
    );
    let mut s2 = Stream::<u64>::new();
    let (event2, retry2) = s2.probe_with(&aop2, 5_000);
    assert_eq!(event2, ProbeEvent::StopAmbiguousRestored(ETIMEDOUT, 1_000_000));
    assert!(!retry2);
    assert_eq!(s2.interval_us, None);
    assert_eq!(s2.restore_interval, None);
    assert_eq!(aop2.fw_interval.get(), 1_000_000);
}
''']
    with tempfile.TemporaryDirectory(prefix="aop-las-") as tmp:
        source = Path(tmp) / "test.rs"
        binary = Path(tmp) / "test"
        source.write_text("\n".join(parts))
        subprocess.run(shlex.split(os.environ.get("RUSTC", "rustc")) + [
            "--edition=2021", "--test", "-Dwarnings", str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
