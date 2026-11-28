#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production AOP ambient light sensor stream state machine on the host."""
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
    als = (root / "drivers/iio/common/aop_sensors/aop_als.rs").read_text()
    parts = [
        "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nstruct Error(i32);",
        "type Result<T = ()> = std::result::Result<T, Error>;",
        "const EIO: Error = Error(-5);",
        "const ETIMEDOUT: Error = Error(-110);",
        const(als, "DIRECT_IDLE_MS"),
        block(als, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum SyncAction"),
        block(als, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum IdleStep"),
        block(als, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum RunState"),
        block(als, "trait AlsIo"),
        block(als, "struct Stream<T>"),
        block(als, "impl<T> Stream<T>"),
        r'''
use std::cell::RefCell;
use std::collections::VecDeque;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Op {
    Calibrate,
    Start,
    Stop,
}

struct MockAlsIo {
    ops: RefCell<Vec<Op>>,
    cal_results: RefCell<VecDeque<Result>>,
    start_results: RefCell<VecDeque<Result>>,
    stop_results: RefCell<VecDeque<Result>>,
}

impl MockAlsIo {
    fn new() -> Self {
        Self {
            ops: RefCell::new(Vec::new()),
            cal_results: RefCell::new(VecDeque::new()),
            start_results: RefCell::new(VecDeque::new()),
            stop_results: RefCell::new(VecDeque::new()),
        }
    }

    fn take_ops(&self) -> Vec<Op> {
        std::mem::take(&mut *self.ops.borrow_mut())
    }
}

impl AlsIo for MockAlsIo {
    fn calibrate(&self) -> Result {
        self.ops.borrow_mut().push(Op::Calibrate);
        self.cal_results.borrow_mut().pop_front().unwrap_or(Ok(()))
    }

    fn start_reports(&self) -> Result {
        self.ops.borrow_mut().push(Op::Start);
        self.start_results.borrow_mut().pop_front().unwrap_or(Ok(()))
    }

    fn stop_reports(&self) -> Result {
        self.ops.borrow_mut().push(Op::Stop);
        self.stop_results.borrow_mut().pop_front().unwrap_or(Ok(()))
    }
}

#[test]
fn probe_calibrates_once_and_idle_stops_reads() {
    let io = MockAlsIo::new();
    let mut s = Stream::<u64>::new();
    assert_eq!(s.probe_with(&io), Ok(()));
    assert_eq!(io.take_ops(), vec![Op::Calibrate, Op::Start, Op::Stop]);
    assert!(s.calibrated);
    assert_eq!(s.run_state, RunState::Stopped);
    assert_eq!(s.next_action(), SyncAction::None);

    // Direct read starts without re-calibrating and invalidates stale samples.
    let mut invalidated = 0;
    s.direct_readers = 1;
    assert_eq!(s.sync_with(&io, || invalidated += 1), Ok(()));
    assert_eq!(invalidated, 1);
    assert_eq!(io.take_ops(), vec![Op::Start]);
    assert_eq!(s.run_state, RunState::Running);

    // Idle work firing while a reader is still waiting rearms instead of stopping.
    s.direct_until = Some(5_000);
    assert_eq!(s.check_idle(0), IdleStep::RearmMs(DIRECT_IDLE_MS as u32));
    assert_eq!(s.next_action(), SyncAction::None);

    // Once readers leave and idle window expires, reports stop cleanly.
    s.direct_readers = 0;
    assert_eq!(s.check_idle(2_500), IdleStep::RearmMs(2_500));
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert_eq!(s.sync_with(&io, || invalidated += 1), Ok(()));
    assert_eq!(invalidated, 1);
    assert_eq!(io.take_ops(), vec![Op::Stop]);
    assert_eq!(s.run_state, RunState::Stopped);
}

#[test]
fn probe_rollback_attempts_best_effort_stop() {
    let io = MockAlsIo::new();
    io.start_results.borrow_mut().push_back(Err(ETIMEDOUT));
    let mut s = Stream::<u64>::new();
    assert_eq!(s.probe_with(&io), Err(ETIMEDOUT));
    assert_eq!(io.take_ops(), vec![Op::Calibrate, Op::Start, Op::Stop]);
    assert_eq!(s.run_state, RunState::Stopped);

    // An unacknowledged stop after successful start keeps probe succeeding
    // with run_state Unknown so idle retry or shutdown finishes stopping it.
    let io2 = MockAlsIo::new();
    io2.stop_results.borrow_mut().extend([Err(ETIMEDOUT), Err(ETIMEDOUT)]);
    let mut s2 = Stream::<u64>::new();
    assert_eq!(s2.probe_with(&io2), Ok(()));
    assert_eq!(io2.take_ops(), vec![Op::Calibrate, Op::Start, Op::Stop, Op::Stop]);
    assert_eq!(s2.run_state, RunState::Unknown);
    assert!(s2.arm_stop_retry(5_000));
    assert_eq!(s2.check_idle(0), IdleStep::Expire);
    assert_eq!(s2.sync_with(&io2, || {}), Ok(()));
    assert_eq!(io2.take_ops(), vec![Op::Stop]);
    assert_eq!(s2.run_state, RunState::Stopped);

    // Shutdown also stops an active stream and parks it.
    s2.direct_readers = 1;
    assert_eq!(s2.sync_with(&io2, || {}), Ok(()));
    io2.take_ops();
    assert_eq!(s2.shutdown_with(&io2), Ok(()));
    assert_eq!(io2.take_ops(), vec![Op::Stop]);
    assert_eq!(s2.run_state, RunState::Stopped);
}

#[test]
fn sleep_does_not_wake_idle_sensor_and_recalibrates_after_fw_sleep() {
    let io = MockAlsIo::new();
    let mut s = Stream::<u64>::new();
    assert_eq!(s.probe_with(&io), Ok(()));
    io.take_ops();

    // Idle s2idle suspend/resume issues zero AOP calls.
    let (res, retry) = s.set_parked_with(&io, true, false, false, 5_000, || {});
    assert_eq!((res, retry), (Ok(()), false));
    assert!(io.take_ops().is_empty());
    let (res, retry) = s.set_parked_with(&io, false, false, false, 5_000, || {});
    assert_eq!((res, retry), (Ok(()), false));
    assert!(io.take_ops().is_empty());
    assert!(s.calibrated);
    assert_eq!(s.run_state, RunState::Stopped);

    // Idle firmware sleep clears calibration without waking the sensor.
    let (res, retry) = s.set_parked_with(&io, true, false, false, 5_000, || {});
    assert_eq!((res, retry), (Ok(()), false));
    let (res, retry) = s.set_parked_with(&io, false, true, false, 5_000, || {});
    assert_eq!((res, retry), (Ok(()), false));
    assert!(io.take_ops().is_empty());
    assert!(!s.calibrated);

    // Next direct read after firmware sleep resends cached calibration once and starts.
    s.direct_readers = 1;
    assert_eq!(s.sync_with(&io, || {}), Ok(()));
    assert_eq!(io.take_ops(), vec![Op::Calibrate, Op::Start]);
    assert!(s.calibrated);
    assert_eq!(s.run_state, RunState::Running);

    // Active stream parks on suspend and stays stopped on wake when nobody reads.
    s.direct_readers = 0;
    s.direct_until = Some(5_000);
    let (res, retry) = s.set_parked_with(&io, true, false, false, 5_000, || {});
    assert_eq!((res, retry), (Ok(()), false));
    assert_eq!(io.take_ops(), vec![Op::Stop]);
    assert_eq!(s.direct_until, None);
    let (res, retry) = s.set_parked_with(&io, false, false, false, 5_000, || {});
    assert_eq!((res, retry), (Ok(()), false));
    assert!(io.take_ops().is_empty());
    assert_eq!(s.run_state, RunState::Stopped);
}

#[test]
fn failed_suspend_stop_retries_on_s2idle_wake_and_arms_idle_retry() {
    let io = MockAlsIo::new();
    let mut s = Stream::<u64>::new();
    assert_eq!(s.probe_with(&io), Ok(()));
    s.direct_until = Some(5_000);
    assert_eq!(s.sync_with(&io, || {}), Ok(()));
    io.take_ops();

    // Stop fails during suspend prepare -> Unknown.
    io.stop_results.borrow_mut().extend([Err(ETIMEDOUT), Err(ETIMEDOUT)]);
    let (res, retry) = s.set_parked_with(&io, true, false, false, 5_000, || {});
    assert_eq!((res, retry), (Err(ETIMEDOUT), false));
    assert_eq!(s.run_state, RunState::Unknown);

    // s2idle resume while idle retries Stop and arms idle retry on failure.
    let (res, retry) = s.set_parked_with(&io, false, false, false, 10_000, || {});
    assert_eq!((res, retry), (Err(ETIMEDOUT), true));
    assert_eq!(s.direct_until, Some(10_000));

    // Idle retry expires and succeeds.
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert_eq!(s.sync_with(&io, || {}), Ok(()));
    assert_eq!(s.run_state, RunState::Stopped);

    // Unknown state also survives firmware-sleep resume (reset_cal = true) so
    // a failed pre-sleep stop is still retried while idle.
    s.direct_until = Some(15_000);
    assert_eq!(s.sync_with(&io, || {}), Ok(()));
    io.take_ops();
    io.stop_results.borrow_mut().push_back(Err(ETIMEDOUT));
    let (res, retry) = s.set_parked_with(&io, true, false, false, 15_000, || {});
    assert_eq!((res, retry), (Err(ETIMEDOUT), false));
    assert_eq!(s.run_state, RunState::Unknown);
    io.take_ops();
    let (res, retry) = s.set_parked_with(&io, false, true, false, 20_000, || {});
    assert_eq!((res, retry), (Ok(()), false));
    assert_eq!(io.take_ops(), vec![Op::Stop]);
    assert_eq!(s.run_state, RunState::Stopped);
    assert!(!s.calibrated);
}

#[test]
fn ct817_rearms_if_active_across_sleep_and_backoff_expires() {
    let io = MockAlsIo::new();
    let mut s = Stream::<u64>::new();
    assert_eq!(s.probe_with(&io), Ok(()));
    s.direct_readers = 1;
    assert_eq!(s.sync_with(&io, || {}), Ok(()));
    io.take_ops();

    // Even if run_state were Running on CT817 at POST_SUSPEND, an active reader re-arms.
    let mut invalidated = 0;
    let (res, retry) = s.set_parked_with(&io, false, false, true, 5_000, || invalidated += 1);
    assert_eq!((res, retry), (Ok(()), false));
    assert_eq!(invalidated, 1);
    assert_eq!(io.take_ops(), vec![Op::Start]);

    // Backoff caps repeated failed direct starts.
    s.failed_until = Some((5_000, EIO));
    assert_eq!(s.check_backoff(1_000), Some(EIO));
    assert_eq!(s.check_backoff(0), None);
    assert!(s.failed_until.is_none());
}
''',
    ]
    with tempfile.TemporaryDirectory(prefix="aop-als-") as tmp:
        source = Path(tmp) / "test.rs"
        binary = Path(tmp) / "test"
        source.write_text("\n".join(parts))
        subprocess.run(
            shlex.split(os.environ.get("RUSTC", "rustc"))
            + ["--edition=2021", "--test", "-Dwarnings", str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
