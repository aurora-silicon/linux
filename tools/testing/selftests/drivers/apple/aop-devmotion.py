#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production device-motion stream state machine on the host."""
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
    devmotion = (root / "drivers/iio/common/aop_sensors/aop_devmotion.rs").read_text()
    parts = [const(devmotion, "DIRECT_IDLE_MS"),
             block(devmotion, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum SyncAction"),
             block(devmotion, "#[derive(Clone, Copy, Debug, PartialEq, Eq)]\nenum IdleStep"),
             block(devmotion, "struct Stream<T>"),
             block(devmotion, "impl<T> Stream<T>"), r'''
/// Counts the client opens and closes a sequence of state changes costs.
struct Service {
    opens: u32,
    closes: u32,
}

fn sync(s: &mut Stream<i64>, svc: &mut Service) {
    match s.next_action() {
        SyncAction::Start => {
            svc.opens += 1;
            s.active = true;
        }
        SyncAction::Stop => {
            s.active = false;
            svc.closes += 1;
        }
        SyncAction::None => {}
    }
}

/// One direct read at `now` ms, as `State::read_now()` does it.
fn read(s: &mut Stream<i64>, svc: &mut Service, now: i64) -> bool {
    if s.parked {
        return false;
    }
    if !s.buffer_on {
        s.direct_until = Some(now + DIRECT_IDLE_MS);
    }
    sync(s, svc);
    true
}

/// The idle work running at `now` ms; returns the re-arm delay, if any.
fn idle(s: &mut Stream<i64>, svc: &mut Service, now: i64) -> Option<u32> {
    let deadline = s.direct_until?;
    match s.check_idle(deadline - now) {
        IdleStep::Ignore => None,
        IdleStep::RearmMs(ms) => Some(ms),
        IdleStep::Expire => {
            sync(s, svc);
            None
        }
    }
}

#[test]
fn direct_reads_share_one_client() {
    let mut s = Stream::<i64>::new();
    let mut svc = Service { opens: 0, closes: 0 };
    assert!(!s.want());
    assert_eq!(s.next_action(), SyncAction::None);

    // The first direct read opens the client.
    s.direct_until = Some(2_000);
    assert_eq!(s.next_action(), SyncAction::Start);
    s.active = true;

    // Later reads reuse it with no service calls and extend the deadline.
    s.direct_until = Some(2_010);
    assert_eq!(s.next_action(), SyncAction::None);
    assert_eq!(s.check_idle(10), IdleStep::RearmMs(10));
    assert_eq!(s.check_idle(DIRECT_IDLE_MS + 500), IdleStep::RearmMs(DIRECT_IDLE_MS as u32));
    assert!(s.direct_until.is_some());

    // Once reads go quiet, idle expiry closes the client.
    assert_eq!(s.check_idle(0), IdleStep::Expire);
    assert!(s.direct_until.is_none());
    assert_eq!(s.next_action(), SyncAction::Stop);
    s.active = false;
    assert_eq!(s.next_action(), SyncAction::None);
    assert_eq!(s.check_idle(-1), IdleStep::Ignore);

    // A 25 Hz poller for ten seconds costs one open and one close.
    let mut now = 0;
    while now < 10_000 {
        assert!(read(&mut s, &mut svc, now));
        now += 40;
    }
    // The work armed by the first read finds the deadline moved and re-arms.
    assert_eq!(idle(&mut s, &mut svc, 2_000), Some(DIRECT_IDLE_MS as u32));
    assert_eq!((svc.opens, svc.closes), (1, 0));
    let last = now - 40;
    assert_eq!(idle(&mut s, &mut svc, last + DIRECT_IDLE_MS - 1), Some(1));
    assert_eq!(idle(&mut s, &mut svc, last + DIRECT_IDLE_MS), None);
    assert_eq!((svc.opens, svc.closes), (1, 1));
    assert!(!s.active);

    // The next read after the window opens a new client.
    assert!(read(&mut s, &mut svc, 20_000));
    assert_eq!((svc.opens, svc.closes), (2, 1));
}
#[test]
fn buffer_takes_over_a_direct_client() {
    let mut s = Stream::<i64>::new();
    let mut svc = Service { opens: 0, closes: 0 };
    assert!(read(&mut s, &mut svc, 0));
    assert_eq!((svc.opens, svc.closes), (1, 0));

    // Enabling the buffer keeps the open client and drops the direct hold.
    s.buffer_on = true;
    s.direct_until = None;
    sync(&mut s, &mut svc);
    assert_eq!((svc.opens, svc.closes), (1, 0));

    // The stale idle work and reads under the buffer change nothing.
    assert_eq!(idle(&mut s, &mut svc, DIRECT_IDLE_MS), None);
    assert!(read(&mut s, &mut svc, 2_500));
    assert!(s.direct_until.is_none());
    assert_eq!((svc.opens, svc.closes), (1, 0));
    assert!(s.active);

    // A hold that raced with the buffer is dropped, not expired.
    s.direct_until = Some(3_000);
    assert_eq!(s.check_idle(0), IdleStep::Ignore);
    assert!(s.direct_until.is_none());

    // Disabling the buffer closes the client at once.
    s.buffer_on = false;
    sync(&mut s, &mut svc);
    assert_eq!((svc.opens, svc.closes), (1, 1));
    assert!(!s.active);
}
#[test]
fn sleep_and_unbind_park_the_client() {
    let mut s = Stream::<i64>::new();
    let mut svc = Service { opens: 0, closes: 0 };

    // Suspend during a direct hold closes the client; resume leaves it closed.
    assert!(read(&mut s, &mut svc, 0));
    s.parked = true;
    s.direct_until = None;
    sync(&mut s, &mut svc);
    assert_eq!((svc.opens, svc.closes), (1, 1));
    assert!(!read(&mut s, &mut svc, 100));
    assert_eq!(idle(&mut s, &mut svc, DIRECT_IDLE_MS), None);
    s.parked = false;
    sync(&mut s, &mut svc);
    assert_eq!((svc.opens, svc.closes), (1, 1));

    // A buffer is closed for sleep and reopened on wake.
    s.buffer_on = true;
    sync(&mut s, &mut svc);
    s.parked = true;
    sync(&mut s, &mut svc);
    assert_eq!((svc.opens, svc.closes), (2, 2));
    s.parked = false;
    sync(&mut s, &mut svc);
    assert_eq!((svc.opens, svc.closes), (3, 2));

    // A parked stream ignores a hold left behind.
    s.parked = true;
    s.direct_until = Some(1);
    assert!(!s.want());
    assert_eq!(s.check_idle(-5), IdleStep::Ignore);
    assert!(s.direct_until.is_none());
    sync(&mut s, &mut svc);
    assert_eq!((svc.opens, svc.closes), (3, 3));
}
''']
    with tempfile.TemporaryDirectory(prefix="aop-devmotion-") as tmp:
        source = Path(tmp) / "test.rs"
        binary = Path(tmp) / "test"
        source.write_text("\n".join(parts))
        subprocess.run(shlex.split(os.environ.get("RUSTC", "rustc")) + [
            "--edition=2021", "--test", "-Dwarnings", str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
