#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production motion report decoder and calibration selector on the host."""
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
    motion = (root / "drivers/iio/common/aop_sensors/aop_motion.rs").read_text()
    parts = ["#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nstruct Error(i32);",
             const(motion, "REPORT_LEN"), const(motion, "XYZ_OFFSET"),
             const(motion, "DIRECT_IDLE_MS"),
             block(motion, "fn decode("), block(motion, "fn le_u32("),
             block(motion, "fn location0_packet("),
             const(motion, "CALIBRATION_LOADED"),
             block(motion, "fn calibration_state_set("),
             const(motion, "STATUS_UNSUPPORTED"),
             block(motion, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum CalStatus"), block(motion, "fn cal_status("),
             block(motion, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum SyncAction"),
             block(motion, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum IdleStep"),
             block(motion, "struct Stream<T>"),
             block(motion, "impl<T> Stream<T>"), r'''
fn report(prefix: u8, xyz: [i32; 3], suffix: u8) -> Vec<u8> {
    let mut r = vec![prefix; 6];
    for v in xyz {
        r.extend_from_slice(&v.to_le_bytes());
    }
    r.extend_from_slice(&[suffix; 4]);
    r
}
#[test]
fn decodes_signed_axes() {
    // Synthetic reports; the bytes around the samples carry no sample.
    for (prefix, suffix) in [(0, 0), (0xff, 0xff), (0x5a, 0xa5)] {
        for xyz in [[65536, -65536, 0], [i32::MIN, i32::MAX, -1]] {
            assert_eq!(decode(&report(prefix, xyz, suffix)), Some(xyz));
        }
    }
}
#[test]
fn rejects_other_lengths() {
    let r = report(0, [1, 2, 3], 0);
    for len in 0..40 {
        if len == REPORT_LEN {
            continue;
        }
        let mut v = r.clone();
        v.resize(len, 0);
        assert_eq!(decode(&v), None);
    }
}
fn container(entries: &[(u32, &[u8])]) -> Vec<u8> {
    let mut c = vec![2, 0, entries.len() as u8, 1];
    for (location, packet) in entries {
        c.extend_from_slice(&location.to_le_bytes());
        c.extend_from_slice(&(packet.len() as u32).to_le_bytes());
        c.extend_from_slice(packet);
    }
    c
}
#[test]
fn calibration_state() {
    assert!(!calibration_state_set(0, &0u32.to_le_bytes()));
    // The firmware's default state is not a calibration this driver sent.
    assert!(!calibration_state_set(0, &1u32.to_le_bytes()));
    assert!(calibration_state_set(0, &2u32.to_le_bytes()));
    assert!(!calibration_state_set(0, &3u32.to_le_bytes()));
    assert!(!calibration_state_set(0xe000_02c7, &2u32.to_le_bytes()));
    assert!(!calibration_state_set(0, &[2, 0]));
    assert!(!calibration_state_set(0, &[]));
}

#[test]
fn calibration_status() {
    assert_eq!(cal_status(0), CalStatus::Taken);
    // Answered once the service holds a calibration from an earlier start.
    assert_eq!(cal_status(0xe000_02c7), CalStatus::Held);
    for status in [1, 0xe000_02bc, 0xe000_02c2, 0xe000_02e2, 0xe000_02c6, u32::MAX] {
        assert_eq!(cal_status(status), CalStatus::Refused, "{status:#x}");
    }
}
#[test]
fn selects_location_zero() {
    let a = [1u8; 44];
    let b = [2u8; 40];
    let one = container(&[(0, &a)]);
    assert_eq!(location0_packet(&one), Some(&a[..]));
    // Trailing bytes after the entries are not part of the packet.
    let mut padded = one.clone();
    padded.extend_from_slice(&[0; 4]);
    assert_eq!(location0_packet(&padded), Some(&a[..]));
    let two = container(&[(3, &b), (0, &a)]);
    assert_eq!(location0_packet(&two), Some(&a[..]));
    assert_eq!(location0_packet(&container(&[(1, &a)])), None);
}
#[test]
fn rejects_malformed_containers() {
    let a = [1u8; 44];
    let good = container(&[(0, &a)]);
    for len in 0..good.len() {
        assert_eq!(location0_packet(&good[..len]), None, "truncated to {len}");
    }
    let mut not_container = good.clone();
    not_container[3] = 0;
    assert_eq!(location0_packet(&not_container), None);
    let mut huge = good.clone();
    huge[8..12].copy_from_slice(&u32::MAX.to_le_bytes());
    assert_eq!(location0_packet(&huge), None);
    let mut many = good.clone();
    many[2] = 255;
    assert_eq!(location0_packet(&many), Some(&a[..]));
}
#[test]
fn calibrates_once_and_reuses_direct_stream() {
    let mut s = Stream::<i64>::new();
    assert!(!s.want());
    assert_eq!(s.next_action(), SyncAction::None);

    // Probe leaves calibration recorded and reports idle.
    s.calibrated = true;

    // First direct read (X) starts reports without re-sending calibration.
    s.direct_until = Some(2_000);
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
    s.active = true;

    // Subsequent Y/Z and polling reads reuse the running stream with no RPCs.
    s.direct_until = Some(2_010);
    assert_eq!(s.next_action(), SyncAction::None);
    assert_eq!(s.check_idle(10), IdleStep::RearmMs(10));
    assert_eq!(s.check_idle(DIRECT_IDLE_MS + 500), IdleStep::RearmMs(DIRECT_IDLE_MS as u32));
    assert!(s.direct_until.is_some());

    // Once reads go quiet, idle expiry stops the reports.
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert!(s.direct_until.is_none());
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.active = false;
    assert_eq!(s.next_action(), SyncAction::None);
    assert_eq!(s.check_idle(-1), IdleStep::Ignore);
}
#[test]
fn buffer_handoff_and_pm_calibration_policy() {
    let mut s = Stream::<i64>::new();
    // Uncalibrated buffer start calibrates first.
    s.buffer_on = true;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: true });
    s.calibrated = true;
    s.active = true;

    // Suspend parks without clearing calibration; resume restarts interval only.
    s.parked = true;
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.active = false;
    s.parked = false;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
    s.active = true;

    // Hibernation clears calibration so the next start sends it once.
    s.parked = true;
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.active = false;
    s.parked = false;
    s.calibrated = false;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: true });
    s.calibrated = true;
    s.active = true;

    // Disabling buffer stops immediately.
    s.buffer_on = false;
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.active = false;

    // Direct hold handed off cleanly to buffer; stale idle callback is ignored.
    s.direct_until = Some(5_000);
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
    s.active = true;
    s.buffer_on = true;
    assert_eq!(s.check_idle(0), IdleStep::Ignore);
    assert!(s.direct_until.is_none());
    assert_eq!(s.next_action(), SyncAction::None);
    s.buffer_on = false;
    assert_eq!(s.next_action(), SyncAction::Stop);
}
/// Counts the starts a sequence of direct reads tries on a failing service.
struct Service {
    fail: Option<Error>,
    starts: u32,
}

/// One direct read at `now` ms, as `State::read_now()` does it.
fn read(s: &mut Stream<i64>, svc: &mut Service, now: i64) -> Result<(), Error> {
    if let Some((until, _)) = s.failed_until {
        if let Some(e) = s.check_backoff(until - now) {
            return Err(e);
        }
    }
    if !s.buffer_on {
        s.direct_until = Some(now + DIRECT_IDLE_MS);
    }
    if let SyncAction::Start { .. } = s.next_action() {
        svc.starts += 1;
        if let Some(e) = svc.fail {
            s.direct_until = None;
            s.failed_until = Some((now + DIRECT_IDLE_MS, e));
            return Err(e);
        }
        s.calibrated = true;
        s.active = true;
    }
    Ok(())
}
#[test]
fn failed_start_backs_off_direct_reads() {
    const EIO: Error = Error(5);
    const ETIMEDOUT: Error = Error(110);
    let mut s = Stream::<i64>::new();
    let mut svc = Service { fail: Some(EIO), starts: 0 };

    // A 25 Hz poller on a failing service tries one start per idle window
    // and gets the error of that start in between.
    let mut now = 0;
    while now < 10_000 {
        assert_eq!(read(&mut s, &mut svc, now), Err(EIO));
        assert!(!s.want());
        now += 40;
    }
    assert_eq!(svc.starts, (10_000 / DIRECT_IDLE_MS) as u32);

    // The cached error is the one of the latest try.
    svc.fail = Some(ETIMEDOUT);
    assert_eq!(read(&mut s, &mut svc, 10_000), Err(ETIMEDOUT));
    assert_eq!(read(&mut s, &mut svc, 10_000 + DIRECT_IDLE_MS - 1), Err(ETIMEDOUT));
    assert_eq!(svc.starts, 6);

    // The first read after the window tries again; success ends the backoff.
    svc.fail = None;
    assert_eq!(read(&mut s, &mut svc, 10_000 + DIRECT_IDLE_MS), Ok(()));
    assert_eq!(svc.starts, 7);
    assert!(s.failed_until.is_none());
    assert!(s.active);
    assert_eq!(read(&mut s, &mut svc, 10_040 + DIRECT_IDLE_MS), Ok(()));
    assert_eq!(svc.starts, 7);

    // The deadline itself is the first moment of the next try.
    assert_eq!(s.check_backoff(-1), None);
    s.failed_until = Some((500, EIO));
    assert_eq!(s.check_backoff(1), Some(EIO));
    assert!(s.failed_until.is_some());
    assert_eq!(s.check_backoff(0), None);
    assert!(s.failed_until.is_none());
}
#[test]
fn buffer_and_sleep_end_the_backoff() {
    const EIO: Error = Error(5);
    let mut s = Stream::<i64>::new();
    let mut svc = Service { fail: Some(EIO), starts: 0 };
    assert_eq!(read(&mut s, &mut svc, 0), Err(EIO));
    assert_eq!(read(&mut s, &mut svc, 40), Err(EIO));
    assert_eq!(svc.starts, 1);

    // Enabling a buffer is a new request: it tries a start of its own, and
    // a read under the running buffer is served, as `set_buffer()` does it.
    svc.fail = None;
    s.buffer_on = true;
    s.direct_until = None;
    s.failed_until = None;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: true });
    s.calibrated = true;
    s.active = true;
    assert_eq!(read(&mut s, &mut svc, 80), Ok(()));
    assert_eq!(svc.starts, 1);
    s.buffer_on = false;
    s.active = false;

    // Sleep drops a backoff, as `set_parked()` does it, so the first read
    // after resume tries at once.
    svc.fail = Some(EIO);
    assert_eq!(read(&mut s, &mut svc, 1_000), Err(EIO));
    assert_eq!(svc.starts, 2);
    s.parked = true;
    s.failed_until = None;
    s.parked = false;
    s.failed_until = None;
    svc.fail = None;
    assert_eq!(read(&mut s, &mut svc, 1_040), Ok(()));
    assert_eq!(svc.starts, 3);
}
#[test]
fn failed_stop_keeps_active_until_reset_cal() {
    let mut s = Stream::<i64>::new();
    s.calibrated = true;
    s.buffer_on = true;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
    s.active = true;

    // A failed pre-sleep stop leaves `active` true instead of pretending the
    // reports stopped, so an s2idle wake with no user retries stopping them.
    s.parked = true;
    assert_eq!(s.next_action(), SyncAction::Stop);
    // Simulate stop_reports() returning Err: `active` stays true.
    assert!(s.active);

    // After firmware suspend / hibernation (`reset_cal == true`), both
    // `calibrated` and `active` are cleared so a running buffer cleanly
    // recalibrates and restarts even if the pre-sleep stop failed.
    s.parked = false;
    s.calibrated = false;
    s.active = false;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: true });
    s.calibrated = true;
    s.active = true;

    // Same for a direct read after a failed stop + `reset_cal`: next read
    // recalibrates and starts cleanly.
    s.buffer_on = false;
    assert_eq!(s.next_action(), SyncAction::Stop);
    // Simulate failed stop leaving `active` true.
    assert!(s.active);
    s.parked = true;
    s.direct_until = None;
    s.parked = false;
    s.calibrated = false;
    s.active = false;
    assert_eq!(s.next_action(), SyncAction::None);
    s.direct_until = Some(2_000);
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: true });
}
''']
    with tempfile.TemporaryDirectory(prefix="aop-motion-") as tmp:
        source = Path(tmp) / "test.rs"
        binary = Path(tmp) / "test"
        source.write_text("\n".join(parts))
        subprocess.run(shlex.split(os.environ.get("RUSTC", "rustc")) + [
            "--edition=2021", "--test", "-Dwarnings", str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
