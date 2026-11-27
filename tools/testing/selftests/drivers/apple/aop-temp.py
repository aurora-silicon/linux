#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production AOP temperature decoder, calibration selector, and stream state machine."""
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
    temp = (root / "drivers/iio/common/aop_sensors/aop_temp.rs").read_text()
    parts = [
        "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nstruct Error(i32);",
        const(temp, "REPORT_LEN"),
        const(temp, "TEMP_OFFSET"),
        const(temp, "DIRECT_IDLE_MS"),
        block(temp, "fn le_u32("),
        block(temp, "fn location0_packet("),
        block(temp, "fn decode_millicelsius("),
        block(temp, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum SyncAction"),
        block(temp, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum IdleStep"),
        block(temp, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum RunState"),
        block(temp, "struct Stream<T>"),
        block(temp, "impl<T> Stream<T>"),
        r'''
fn report(q16_16: i32) -> Vec<u8> {
    let mut r = vec![0u8; REPORT_LEN];
    r[TEMP_OFFSET..TEMP_OFFSET + 4].copy_from_slice(&(q16_16 as u32).to_le_bytes());
    r
}

#[test]
fn decodes_signed_q16_16_celsius() {
    assert_eq!(decode_millicelsius(&report(25 << 16)), Some(25_000));
    assert_eq!(decode_millicelsius(&report((30 << 16) + 32_768)), Some(30_500));
    assert_eq!(decode_millicelsius(&report(-(5 << 16))), Some(-5_000));
    assert_eq!(decode_millicelsius(&vec![0u8; REPORT_LEN - 1]), None);
    assert_eq!(decode_millicelsius(&vec![0u8; REPORT_LEN + 1]), None);
}

#[test]
fn selects_location0_calibration_packet() {
    let mut c = vec![2, 0, 2, 1];
    c.extend_from_slice(&3u32.to_le_bytes());
    c.extend_from_slice(&2u32.to_le_bytes());
    c.extend_from_slice(&[0xaa, 0xbb]);
    c.extend_from_slice(&0u32.to_le_bytes());
    c.extend_from_slice(&3u32.to_le_bytes());
    c.extend_from_slice(&[1, 2, 3]);
    assert_eq!(location0_packet(&c), Some(&[1u8, 2, 3][..]));
    assert_eq!(location0_packet(&c[..c.len() - 1]), None);
}

#[test]
fn calibrates_once_and_idle_stops_direct_reads() {
    let mut s = Stream::<u64>::new();
    // Before on_demand is enabled, continuous mode wants reports on.
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: true });

    // Probe calibrates once and enables on-demand idle-stop.
    s.calibrated = true;
    s.on_demand = true;
    assert_eq!(s.next_action(), SyncAction::None);

    // Direct read holds direct_readers while waiting, then sets direct_until.
    s.direct_readers = 1;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
    s.run_state = RunState::Running;
    assert_eq!(s.next_action(), SyncAction::None);

    // Buffer enable then disable during an in-flight direct read keeps stream alive.
    s.buffer_on = true;
    s.direct_until = None;
    assert_eq!(s.next_action(), SyncAction::None);
    s.buffer_on = false;
    assert_eq!(s.next_action(), SyncAction::None);

    // Idle work firing while direct_readers > 0 rearms instead of stopping.
    s.direct_until = Some(5_000);
    assert_eq!(s.check_idle(0), IdleStep::RearmMs(DIRECT_IDLE_MS as u32));
    assert_eq!(s.next_action(), SyncAction::None);

    // Reader finishes and quiet passes the deadline -> stops stream.
    s.direct_readers = 0;
    assert_eq!(s.check_idle(3_000), IdleStep::RearmMs(3_000));
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.run_state = RunState::Stopped;
    assert_eq!(s.next_action(), SyncAction::None);

    // Unacknowledged stop leaves state Unknown: re-enabling buffer reasserts Start,
    // while remaining idle reasserts Stop.
    s.run_state = RunState::Unknown;
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.buffer_on = true;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
}

#[test]
fn buffer_and_sleep_transitions() {
    let mut s = Stream::<u64>::new();
    s.calibrated = true;
    s.on_demand = true;
    s.buffer_on = true;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
    s.run_state = RunState::Running;

    // Sleep parks active buffer stream.
    s.parked = true;
    s.direct_until = None;
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.run_state = RunState::Stopped;

    // Firmware sleep clears calibration and run_state; resume re-calibrates and starts buffer.
    s.parked = false;
    s.calibrated = false;
    s.run_state = RunState::Stopped;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: true });
    s.calibrated = true;
    s.run_state = RunState::Running;

    // Disabling buffer stops stream.
    s.buffer_on = false;
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.run_state = RunState::Stopped;
    assert_eq!(s.next_action(), SyncAction::None);

    // Continuous fallback (stop refused at probe): stays on after wake without buffer.
    s.on_demand = false;
    assert_eq!(s.next_action(), SyncAction::Start { calibrate: false });
}

#[test]
fn backoff_caps_failed_direct_starts() {
    let mut s = Stream::<u64>::new();
    s.failed_until = Some((5_000, Error(-5)));
    assert_eq!(s.check_backoff(1_000), Some(Error(-5)));
    assert_eq!(s.check_backoff(0), None);
    assert!(s.failed_until.is_none());
}
''',
    ]
    with tempfile.TemporaryDirectory(prefix="aop-temp-") as tmp:
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
