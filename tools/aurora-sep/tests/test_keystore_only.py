#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production initrd cache and refusal paths without SEP hardware."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def function(path, name):
    source = (ROOT / path).read_text()
    start = source.index("fn " + name)
    start = source.rfind("\n", 0, start) + 1
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@unittest.skipUnless(shutil.which("rustc"), "rustc required")
class KeyStoreCacheTests(unittest.TestCase):
    def test_production_cache_and_unseal_refusal(self):
        harness = r'''
use std::cell::{Cell, RefCell};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering::Relaxed};
type Result<T> = std::result::Result<T, u8>;
type KVec<T> = Vec<T>;
const EIO: u8 = 5;
const ENODEV: u8 = 19;
thread_local! {
    static READS: Cell<usize> = const { Cell::new(0) };
    static FILE: Cell<Result<Option<u8>>> = const { Cell::new(Ok(Some(7))) };
    static WIPED: Cell<bool> = const { Cell::new(false) };
}
struct Secret(Vec<u8>);
impl Drop for Secret {
    fn drop(&mut self) { self.0.fill(0); WIPED.with(|w| w.set(true)); }
}
struct Cache(RefCell<Option<keybag::StoredKeyBag>>);
impl Cache {
    fn lock(&self) -> std::cell::RefMut<'_, Option<keybag::StoredKeyBag>> {
        self.0.borrow_mut()
    }
}
mod keybag {
    use super::*;
    pub struct StoredKeyBag(pub u8);
    pub enum Slot { Identity }
    pub enum State { Present(StoredKeyBag), Absent(()) }
    pub fn read(_: Slot) -> Result<State> {
        READS.with(|n| n.set(n.get()+1));
        FILE.with(|f| f.get()).map(|v| match v {
            Some(v) => State::Present(StoredKeyBag(v)), None => State::Absent(()),
        })
    }
}
mod attach {
ALLOW
}
struct SepData {
    identity_keybag: Cache,
    keystore_only: AtomicBool,
    keystore_write_refusals: AtomicU32,
    refuse: bool,
}
impl SepData {
CACHE
UNSEAL
    fn refkey_unseal_cached(&self, _: &[u8]) -> Result<Vec<u8>> {
        if self.refuse { self.keystore_write_refusals.fetch_add(1, Relaxed); }
        Ok(vec![0x5a; 32])
    }
}
fn device(only: bool, refuse: bool) -> SepData {
    SepData { identity_keybag: Cache(RefCell::new(None)),
        keystore_only: AtomicBool::new(only),
        keystore_write_refusals: AtomicU32::new(0), refuse }
}
#[test] fn cache_survives_loss_of_initrd_files() {
    let d = device(true, false);
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(7));
    FILE.with(|f| f.set(Err(2)));
    for _ in 0..5 { assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(7)); }
    READS.with(|n| assert_eq!(n.get(), 1));
}
#[test] fn consumer_failure_does_not_discard_initrd_material() {
    let d = device(true, false);
    assert_eq!(d.with_identity_keybag::<()>(|_| Err(EIO)), Err(EIO));
    FILE.with(|f| f.set(Err(2)));
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(7));
    READS.with(|n| assert_eq!(n.get(), 1));
}
#[test] fn absent_or_invalid_files_are_not_provisioned_or_cached() {
    let d = device(true, false);
    FILE.with(|f| f.set(Ok(None)));
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Err(ENODEV));
    FILE.with(|f| f.set(Err(22)));
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Err(22));
    FILE.with(|f| f.set(Ok(Some(9))));
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(9));
    READS.with(|n| assert_eq!(n.get(), 3));
}
#[test] fn default_attach_keeps_reading_current_real_root_bag() {
    let d = device(false, false);
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(7));
    FILE.with(|f| f.set(Ok(Some(11))));
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(11));
    READS.with(|n| assert_eq!(n.get(), 2));
}
#[test] fn completion_switches_to_real_root_material() {
    let d = device(true, false);
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(7));
    d.keystore_only.store(false, Relaxed);
    *d.identity_keybag.lock() = None;
    FILE.with(|f| f.set(Ok(Some(11))));
    assert_eq!(d.with_identity_keybag(|k| Ok(k.0)), Ok(11));
}
#[test] fn refused_write_overrides_even_enclave_success_and_wipes_key() {
    assert_eq!(device(true, true).refkey_unseal_trusted(&[]), Err(EIO));
    WIPED.with(|w| assert!(w.get()));
}
#[test] fn previous_refusal_does_not_poison_later_read_only_unseal() {
    let d = device(true, false);
    d.keystore_write_refusals.store(3, Relaxed);
    assert_eq!(d.refkey_unseal_trusted(&[]), Ok(vec![0x5a; 32]));
}
'''
        harness = harness.replace("CACHE", function("drivers/soc/apple/refkey.rs", "with_identity_keybag"))
        harness = harness.replace("UNSEAL", function("drivers/soc/apple/refkey.rs", "refkey_unseal_trusted"))
        harness = harness.replace("ALLOW", function("drivers/soc/apple/attach.rs", "unseal_allowed"))
        with tempfile.TemporaryDirectory(prefix="sep-keystore-") as tmp:
            path = Path(tmp)
            (path / "test.rs").write_text(harness)
            subprocess.run(["rustc", "--edition=2021", "--test", str(path / "test.rs"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test"), "--test-threads=1"], check=True)


@unittest.skipUnless(shutil.which("rustc"), "rustc required")
class MachineRefkeyTests(unittest.TestCase):
    def test_production_read_only_refkey_cache(self):
        harness = r'''
use std::cell::{Cell, RefCell};
use std::ffi::CStr;
use std::ops::{Deref, DerefMut};
use std::sync::atomic::{AtomicBool, Ordering::Relaxed};
type Result<T> = std::result::Result<T, u8>;
const GFP_KERNEL: u8 = 0;
macro_rules! dev_err { ($($arg:tt)*) => {}; }
macro_rules! dev_warn { ($($arg:tt)*) => {}; }
thread_local! {
    static READABLE: Cell<bool> = const { Cell::new(true) };
    static VALID: Cell<bool> = const { Cell::new(true) };
    static READS: Cell<usize> = const { Cell::new(0) };
    static WRITES: Cell<usize> = const { Cell::new(0) };
    static CREATES: Cell<usize> = const { Cell::new(0) };
}
struct KVec<T>(Vec<T>);
impl<T: Clone> KVec<T> {
    fn new() -> Self { Self(Vec::new()) }
    fn resize(&mut self, len: usize, value: T, _: u8) -> Result<()> {
        self.0.resize(len, value); Ok(())
    }
}
impl<T> Deref for KVec<T> {
    type Target = [T];
    fn deref(&self) -> &[T] { &self.0 }
}
impl<T> DerefMut for KVec<T> {
    fn deref_mut(&mut self) -> &mut [T] { &mut self.0 }
}
mod sks { pub type KeyBagHandle = u8; }
mod module_parameters {
    pub struct Param;
    impl Param { pub fn value(&self) -> u8 { 0 } }
    #[allow(non_upper_case_globals)]
    pub const refkey_v2: Param = Param;
}
mod shim {
    use super::*;
    pub struct StoreFile;
    impl StoreFile {
        pub fn open_readonly(_: &CStr) -> Result<Self> {
            READS.with(|n| n.set(n.get()+1));
            if READABLE.with(|v| v.get()) { Ok(Self) } else { Err(2) }
        }
        pub fn open(_: &CStr) -> Result<Self> {
            WRITES.with(|n| n.set(n.get()+1)); Ok(Self)
        }
        pub fn size(&self) -> Result<u64> { Ok(3) }
        pub fn read_exact(&self, _: u64, out: &mut [u8]) -> Result<()> {
            out.fill(0xa5); Ok(())
        }
        pub fn write_all(&self, _: u64, _: &[u8]) -> Result<()> { Ok(()) }
        pub fn sync(&self) -> Result<()> { Ok(()) }
    }
}
struct Cache(RefCell<Option<()>>);
impl Cache {
    fn lock(&self) -> std::cell::RefMut<'_, Option<()>> { self.0.borrow_mut() }
}
struct SepData { machine_refkey: Cache, keystore_only: AtomicBool }
impl SepData {
MACHINE
    fn sks_create_refkey(&self, _: u8, _: &[u8]) -> Option<KVec<u8>> {
        CREATES.with(|n| n.set(n.get()+1)); Some(KVec(vec![1,2,3]))
    }
    fn cache_machine_refkey(&self, _: KVec<u8>) -> bool {
        if !VALID.with(|v| v.get()) { return false; }
        *self.machine_refkey.lock() = Some(()); true
    }
}
fn device(only: bool) -> SepData {
    SepData { machine_refkey: Cache(RefCell::new(None)), keystore_only: AtomicBool::new(only) }
}
fn no_writes() {
    WRITES.with(|n| assert_eq!(n.get(), 0));
    CREATES.with(|n| assert_eq!(n.get(), 0));
}
#[test] fn cached_refkey_never_reopens_initrd_file() {
    let d = device(true); d.sks_machine_refkey(1, &[]);
    READABLE.with(|v| v.set(false));
    for _ in 0..5 { d.sks_machine_refkey(1, &[]); }
    READS.with(|n| assert_eq!(n.get(), 1)); no_writes();
}
#[test] fn missing_initrd_refkey_never_creates_or_persists() {
    READABLE.with(|v| v.set(false));
    let d = device(true); d.sks_machine_refkey(1, &[]);
    assert!(d.machine_refkey.lock().is_none()); no_writes();
}
#[test] fn invalid_existing_refkey_is_never_replaced_in_either_phase() {
    VALID.with(|v| v.set(false));
    for only in [false,true] { device(only).sks_machine_refkey(1, &[]); }
    no_writes();
}
#[test] fn default_attach_retains_missing_refkey_provisioning() {
    READABLE.with(|v| v.set(false));
    let d = device(false); d.sks_machine_refkey(1, &[]);
    assert!(d.machine_refkey.lock().is_some());
    CREATES.with(|n| assert_eq!(n.get(), 1));
    WRITES.with(|n| assert_eq!(n.get(), 1));
}
'''
        harness = harness.replace("MACHINE", function("drivers/soc/apple/refkey.rs", "sks_machine_refkey"))
        with tempfile.TemporaryDirectory(prefix="sep-machine-refkey-") as tmp:
            path = Path(tmp)
            (path / "test.rs").write_text(harness)
            subprocess.run(["rustc", "--edition=2021", "--test", str(path / "test.rs"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test"), "--test-threads=1"], check=True)


class LoaderTests(unittest.TestCase):
    def test_completion_replaces_modprobe(self):
        source = (ROOT / "tools/aurora-sep/load-driver").read_text().split("work=$(mktemp", 1)[0]
        for parameter in (None, "0", "1", "Y"):
            with self.subTest(parameter=parameter), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp)
                param = path / "keystore_only"
                if parameter is not None:
                    param.write_text(parameter + "\n")
                devices = path / "devices"
                attribute = devices / "1234.sep/complete_attach"
                attribute.parent.mkdir(parents=True)
                attribute.touch()
                binary = path / "modprobe"
                binary.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >"$CALLS"\n')
                binary.chmod(0o755)
                script = path / "load-driver"
                script.write_text(source.replace('/sys/module/apple_sep/parameters/keystore_only', str(param))
                                  .replace('/sys/bus/platform/devices', str(devices)))
                calls = path / "calls"
                env = dict(os.environ, AURORA_SEP_SESSION="1", CALLS=str(calls),
                           PATH=str(path) + os.pathsep + os.environ['PATH'])
                done = subprocess.run(["sh", str(script)], env=env, capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertEqual(attribute.read_text(), "1\n" if parameter in ("1", "Y") else "")
                self.assertEqual(calls.exists(), parameter not in ("1", "Y"))


if __name__ == "__main__":
    unittest.main()
