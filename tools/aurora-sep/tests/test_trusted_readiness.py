#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify non-blocking trusted-key readiness checks and bring-up ordering."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
APPLE = ROOT / "drivers/soc/apple"


def function(filename, name):
    source = (APPLE / filename).read_text()
    match = re.search(
        r"^[ \t]*(?:pub(?:\(crate\))? )?(?:unsafe extern \"C\" )?fn " + re.escape(name) + r"\b",
        source,
        re.M,
    )
    if match is None:
        raise AssertionError(f"Missing function {name} in {filename}")
    opening = source.index("{", match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class TrustedReadinessTests(unittest.TestCase):
    def test_trusted_callbacks_never_block_under_key_construction_mutex(self):
        for filename, name in [
            ("trusted.rs", "tk_get_random"),
            ("trusted.rs", "tk_unseal"),
            ("refkey.rs", "refkey_seal_trusted"),
            ("refkey.rs", "refkey_unseal_cached"),
        ]:
            body = function(filename, name)
            self.assertIn("check_trusted_ready", body, f"{filename}:{name} must check readiness")
            self.assertNotIn(
                "wait_for_trusted_ready",
                body,
                f"{filename}:{name} runs under key_construction_mutex and must not wait",
            )

        tk_seal = function("trusted.rs", "tk_seal")
        self.assertIn("refkey_seal_trusted", tk_seal, "trusted.rs:tk_seal must delegate to refkey_seal_trusted")
        self.assertNotIn(
            "wait_for_trusted_ready",
            tk_seal,
            "trusted.rs:tk_seal runs under key_construction_mutex and must not wait",
        )

        # Callers outside key_construction_mutex keep waiting for bring-up.
        for filename, name in [
            ("sep.rs", "finish_attach"),
            ("refkey.rs", "with_identity_bag"),
        ]:
            body = function(filename, name)
            self.assertIn(
                "wait_for_trusted_ready",
                body,
                f"{filename}:{name} runs outside key_construction_mutex and should wait",
            )

    def test_phase_ready_published_before_diag_keystore_leaves_unknown(self):
        finish = function("sep.rs", "finish_bringup")
        lock_pos = finish.index("self.endpoints.lock()")
        phase_pos = finish.index("self.phase.store(PHASE_READY, Release)")
        self.assertLess(
            lock_pos,
            phase_pos,
            "endpoints lock must be held before storing PHASE_READY with Release ordering",
        )
        for diag_call in ("diag::keystore_open()", "diag::keystore_ended()", "diag::bringup_ended()"):
            diag_pos = finish.index(diag_call)
            self.assertLess(
                phase_pos,
                diag_pos,
                f"PHASE_READY must be stored before {diag_call}",
            )
        notify_pos = finish.index("self.trusted_ready.notify_all()")
        self.assertLess(finish.index("diag::keystore_open()"), notify_pos)

        run_ks = function("sbio.rs", "run_keystore_bringup")
        self.assertNotIn(
            "diag::keystore_open",
            run_ks,
            "run_keystore_bringup must not open diag/keystore before PHASE_READY",
        )

    @unittest.skipUnless(shutil.which("rustc"), "rustc required")
    def test_production_readiness_state_machine_and_check(self):
        harness = r'''
use std::cell::{Cell, RefCell};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering::{Acquire, Relaxed}};

type c_int = i32;
type Result<T> = std::result::Result<T, Error>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Error(c_int);
impl Error {
    fn to_errno(self) -> c_int { self.0 }
}

const EIO: Error = Error(-5);
const EAGAIN: Error = Error(-11);
const ENOMEM: Error = Error(-12);
const ENODEV: Error = Error(-19);
const EINVAL: Error = Error(-22);
const ETIMEDOUT: Error = Error(-110);
const EKEYREJECTED: Error = Error(-129);
const EBADMSG: c_int = -74;

const PHASE_ATTACH: u32 = 0;
const PHASE_EXCHANGE: u32 = 1;
const PHASE_READY: u32 = 2;

mod proto {
    pub const EP_SKS: u8 = 0x12;
}

mod pm {
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub enum SleepGate {
        Open = 0,
        Draining = 1,
        Told = 2,
        Lost = 3,
    }
}

mod trusted {
    use super::*;
READY_STATE
UNSEAL_ERRNO
SEAL_WARN
    pub fn check_unseal_errno(e: Error) -> c_int { unseal_failure_errno(e) }
    pub fn check_seal_warn(e: Error) -> bool { seal_should_warn(e) }
}

struct Endpoint { id: u8 }
struct EndpointTable { eps: Vec<Endpoint> }
struct Lock<T>(RefCell<T>);
impl<T> Lock<T> {
    fn lock(&self) -> std::cell::Ref<'_, T> { self.0.borrow() }
}

struct SepData {
    shutting_down: AtomicBool,
    phase: AtomicU32,
    endpoints: Lock<EndpointTable>,
    sks_initialized: AtomicBool,
    gate: Cell<pm::SleepGate>,
    waits: Cell<usize>,
}

impl SepData {
    fn sleep_gate(&self) -> pm::SleepGate { self.gate.get() }
    #[allow(dead_code)]
    fn wait_for_trusted_ready(&self) -> Result<()> {
        self.waits.set(self.waits.get() + 1);
        panic!("check_trusted_ready must never sleep");
    }
CHECK_TRUSTED_READY
}

fn device(phase: u32, has_sks: bool, sks_init: bool, gate: pm::SleepGate) -> SepData {
    let mut eps = Vec::new();
    if has_sks {
        eps.push(Endpoint { id: proto::EP_SKS });
    }
    SepData {
        shutting_down: AtomicBool::new(false),
        phase: AtomicU32::new(phase),
        endpoints: Lock(RefCell::new(EndpointTable { eps })),
        sks_initialized: AtomicBool::new(sks_init),
        gate: Cell::new(gate),
        waits: Cell::new(0),
    }
}

#[test]
fn not_ready_sep_returns_eagain_without_waiting() {
    for phase in [PHASE_ATTACH, PHASE_EXCHANGE] {
        let d = device(phase, false, false, pm::SleepGate::Open);
        assert_eq!(d.check_trusted_ready(), Err(EAGAIN));
        assert_eq!(d.waits.get(), 0);
    }
}

#[test]
fn ready_sep_proceeds_normally() {
    let d = device(PHASE_READY, true, true, pm::SleepGate::Open);
    assert_eq!(d.check_trusted_ready(), Ok(()));
    assert_eq!(d.waits.get(), 0);
}

#[test]
fn failed_and_stopped_sep_return_terminal_errors() {
    let d = device(PHASE_ATTACH, false, false, pm::SleepGate::Open);
    d.shutting_down.store(true, Relaxed);
    assert_eq!(d.check_trusted_ready(), Err(ENODEV));

    let no_sks = device(PHASE_READY, false, false, pm::SleepGate::Open);
    assert_eq!(no_sks.check_trusted_ready(), Err(ENODEV));

    let init_failed = device(PHASE_READY, true, false, pm::SleepGate::Open);
    assert_eq!(init_failed.check_trusted_ready(), Err(EIO));

    let lost = device(PHASE_READY, true, true, pm::SleepGate::Lost);
    assert_eq!(lost.check_trusted_ready(), Err(EIO));
}

#[test]
fn transient_sleep_returns_eagain_until_open() {
    for gate in [pm::SleepGate::Draining, pm::SleepGate::Told] {
        let d = device(PHASE_READY, true, true, gate);
        assert_eq!(d.check_trusted_ready(), Err(EAGAIN));
        d.gate.set(pm::SleepGate::Open);
        assert_eq!(d.check_trusted_ready(), Ok(()));
    }
}

#[test]
fn unseal_and_seal_error_policies() {
    assert_eq!(trusted::check_unseal_errno(EAGAIN), EAGAIN.to_errno());
    for e in [EIO, EINVAL, ENODEV, EKEYREJECTED, ENOMEM] {
        assert_eq!(trusted::check_unseal_errno(e), EBADMSG);
    }
    assert!(!trusted::check_seal_warn(EAGAIN));
    for e in [EIO, ENODEV, EINVAL, ENOMEM, ETIMEDOUT] {
        assert!(trusted::check_seal_warn(e));
    }
}
'''
        harness = (
            harness.replace("READY_STATE", function("trusted.rs", "ready_state"))
            .replace("UNSEAL_ERRNO", function("trusted.rs", "unseal_failure_errno"))
            .replace("SEAL_WARN", function("trusted.rs", "seal_should_warn"))
            .replace("CHECK_TRUSTED_READY", function("sep.rs", "check_trusted_ready"))
        )
        with tempfile.TemporaryDirectory(prefix="sep-trusted-ready-") as tmp:
            path = Path(tmp)
            src = path / "test.rs"
            bin_path = path / "test"
            src.write_text(harness)
            subprocess.run(
                ["rustc", "--edition=2021", "--test", str(src), "-o", str(bin_path)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(bin_path), "--test-threads=1"], check=True)


if __name__ == "__main__":
    unittest.main()
