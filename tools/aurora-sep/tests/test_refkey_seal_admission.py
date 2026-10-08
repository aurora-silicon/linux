"""Compile the production seal entry point with private-key failure controls."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def seal_method():
    text = (ROOT / "drivers/soc/apple/refkey.rs").read_text()
    start = text.index("    pub(crate) fn refkey_seal_trusted(")
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


@unittest.skipUnless(shutil.which("rustc"), "rustc required")
class SealAdmissionTests(unittest.TestCase):
    def test_production_private_key_admission(self):
        harness = r'''
use std::cell::{Cell, RefCell};
type Result<T> = std::result::Result<T, u8>;
type KVec<T> = Vec<T>;
const EIO: u8 = 5;
const ENODEV: u8 = 19;
macro_rules! dev_err { ($($arg:tt)*) => {}; }
thread_local! {
    static ENCRYPT_ERROR: Cell<bool> = const { Cell::new(false) };
    static WIPED: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}
struct Secret(Vec<u8>);
impl std::ops::Deref for Secret {
    type Target = [u8];
    fn deref(&self) -> &[u8] { &self.0 }
}
impl Drop for Secret {
    fn drop(&mut self) {
        self.0.fill(0);
        WIPED.with(|w| *w.borrow_mut() = self.0.clone());
    }
}
mod refkey_seal {
    pub fn ecies_seal(_: &[u8], key: &[u8]) -> super::Result<Vec<u8>> {
        if super::ENCRYPT_ERROR.with(|v| v.get()) { return Err(12); }
        let mut sealed = vec![0xa5];
        sealed.extend_from_slice(key);
        Ok(sealed)
    }
}
struct MachineRefKey { pub_raw: Vec<u8> }
struct Cache(RefCell<Option<MachineRefKey>>);
impl Cache {
    fn lock(&self) -> std::cell::Ref<'_, Option<MachineRefKey>> { self.0.borrow() }
}
struct SepData {
    machine_refkey: Cache,
    ready: Result<()>,
    private_error: Option<u8>,
    bad_plaintext: Option<Vec<u8>>,
    calls: Cell<usize>,
}
impl SepData {
    fn ensure_machine_refkey(&self) -> Result<()> { self.ready }
    fn refkey_unseal_trusted(&self, blob: &[u8]) -> Result<Vec<u8>> {
        // The real unseal path takes the cache lock; mutable borrowing here
        // detects a seal caller retaining its lock across that call.
        let _guard = self.machine_refkey.0.borrow_mut();
        self.calls.set(self.calls.get() + 1);
        if let Some(err) = self.private_error { return Err(err); }
        Ok(self.bad_plaintext.clone().unwrap_or_else(|| blob[1..].to_vec()))
    }
METHOD
}
fn device(private_error: Option<u8>, bad_plaintext: Option<Vec<u8>>) -> SepData {
    SepData {
        machine_refkey: Cache(RefCell::new(Some(MachineRefKey { pub_raw: vec![4; 65] }))),
        ready: Ok(()), private_error, bad_plaintext, calls: Cell::new(0),
    }
}
#[test] fn usable_private_key_returns_original_ciphertext() {
    let d = device(None, None);
    for len in [32, 64, 128] {
        let key = vec![0x5a; len];
        let mut expected = vec![0xa5]; expected.extend_from_slice(&key);
        assert_eq!(d.refkey_seal_trusted(&key), Ok(expected));
        WIPED.with(|w| assert_eq!(*w.borrow(), vec![0; len]));
    }
    assert_eq!(d.calls.get(), 3);
}
#[test] fn orphaned_private_key_does_not_return_ciphertext() {
    let d = device(Some(EIO), None);
    assert_eq!(d.refkey_seal_trusted(&[0x5a; 32]), Err(EIO));
    assert_eq!(d.calls.get(), 1);
}
#[test] fn transient_private_failure_is_propagated() {
    let d = device(Some(110), None);
    assert_eq!(d.refkey_seal_trusted(&[0x5a; 32]), Err(110));
}
#[test] fn different_key_and_truncated_plaintext_are_refused_and_wiped() {
    for plain in [vec![0xa7; 32], vec![0x5a; 31], vec![]] {
        let d = device(None, Some(plain.clone()));
        assert_eq!(d.refkey_seal_trusted(&[0x5a; 32]), Err(EIO));
        WIPED.with(|w| assert_eq!(*w.borrow(), vec![0; plain.len()]));
    }
}
#[test] fn encryption_failure_does_not_touch_private_key() {
    ENCRYPT_ERROR.with(|v| v.set(true));
    let d = device(None, None);
    assert_eq!(d.refkey_seal_trusted(&[0x5a; 32]), Err(12));
    assert_eq!(d.calls.get(), 0);
}
#[test] fn missing_cache_does_not_touch_private_key() {
    let d = device(None, None); *d.machine_refkey.0.borrow_mut() = None;
    assert_eq!(d.refkey_seal_trusted(&[0x5a; 32]), Err(ENODEV));
    assert_eq!(d.calls.get(), 0);
}
#[test] fn unavailable_store_error_is_preserved() {
    let mut d = device(None, None); d.ready = Err(28);
    assert_eq!(d.refkey_seal_trusted(&[0x5a; 32]), Err(28));
    assert_eq!(d.calls.get(), 0);
}
'''
        with tempfile.TemporaryDirectory(prefix="aurora-refkey-admission-") as directory:
            path = Path(directory)
            source = path / "test.rs"
            binary = path / "test"
            source.write_text(harness.replace("METHOD", seal_method()))
            subprocess.run(["rustc", "--edition=2021", "--test", str(source),
                            "-o", str(binary)], check=True, capture_output=True, text=True)
            subprocess.run([str(binary), "--test-threads=1"], check=True,
                           capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
