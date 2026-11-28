# SPDX-License-Identifier: MIT
"""Execute the kernel's owner-file preflight against scripted file outcomes."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[3] / "drivers/soc/apple/catacomb.rs"


def method():
    source = SOURCE.read_text()
    match = re.search(r"^pub\(crate\) fn prepare_missing_owner\(", source, re.M)
    if match is None:
        raise AssertionError("missing owner preparation function")
    opening = source.index("{", match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
use std::ffi::CStr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};
type Error = i32;
type Result<T> = std::result::Result<T, Error>;
const EPERM: Error = -1;
const EEXIST: Error = -17;
const EINVAL: Error = -22;
const ENODATA: Error = -61;
const EROFS: Error = -30;
const PRIVATE_TYPE_CATACOMB_OWNER: u8 = 0xf2;

fn path_for(kind: u8) -> Option<&'static CStr> {
    (kind == PRIVATE_TYPE_CATACOMB_OWNER).then_some(c"/var/lib/apple-sep-catacomb-owner.bin")
}

mod shim {
    use super::*;
    pub static PUBLISH_ERR: AtomicI32 = AtomicI32::new(0);
    pub static CREATE: AtomicBool = AtomicBool::new(true);
    pub static FAIL_OPEN: AtomicBool = AtomicBool::new(false);
    pub static OPEN: AtomicU32 = AtomicU32::new(0);
    pub static SYNC: AtomicU32 = AtomicU32::new(0);
    pub static PARENT: AtomicU32 = AtomicU32::new(0);
    pub static ORDER: Mutex<Vec<&'static str>> = Mutex::new(Vec::new());
    pub fn publishable(_: &CStr) -> Result<()> {
        ORDER.lock().unwrap().push("publishable");
        let err = PUBLISH_ERR.load(Relaxed);
        if err != 0 { Err(err) } else { Ok(()) }
    }
    pub struct StoreFile;
    impl StoreFile {
        pub fn open_host(_: &CStr) -> Result<(StoreFile, bool)> {
            ORDER.lock().unwrap().push("open_host");
            OPEN.fetch_add(1, Relaxed);
            if FAIL_OPEN.load(Relaxed) { Err(EROFS) }
            else { Ok((StoreFile, CREATE.load(Relaxed))) }
        }
        pub fn sync(&self) -> Result<()> {
            ORDER.lock().unwrap().push("sync");
            SYNC.fetch_add(1, Relaxed);
            Ok(())
        }
        pub fn sync_parent(&self) -> Result<()> {
            ORDER.lock().unwrap().push("sync_parent");
            PARENT.fetch_add(1, Relaxed);
            Ok(())
        }
    }
}

METHOD

fn main() {
    assert_eq!(prepare_missing_owner(), Ok(()));
    assert_eq!(shim::OPEN.load(Relaxed), 1);
    assert_eq!(shim::SYNC.load(Relaxed), 1);
    assert_eq!(shim::PARENT.load(Relaxed), 1);
    assert_eq!(*shim::ORDER.lock().unwrap(),
               ["publishable", "open_host", "sync", "sync_parent"]);

    // Append-only parent or blocked reserved name refuses before open_host so
    // no empty owner placeholder is created.
    shim::ORDER.lock().unwrap().clear();
    shim::PUBLISH_ERR.store(EPERM, Relaxed);
    assert_eq!(prepare_missing_owner(), Err(EPERM));
    assert_eq!(shim::OPEN.load(Relaxed), 1);
    assert_eq!(shim::SYNC.load(Relaxed), 1);
    assert_eq!(shim::PARENT.load(Relaxed), 1);
    assert_eq!(*shim::ORDER.lock().unwrap(), ["publishable"]);

    shim::ORDER.lock().unwrap().clear();
    shim::PUBLISH_ERR.store(EEXIST, Relaxed);
    assert_eq!(prepare_missing_owner(), Err(EEXIST));
    assert_eq!(shim::OPEN.load(Relaxed), 1);
    assert_eq!(*shim::ORDER.lock().unwrap(), ["publishable"]);

    shim::PUBLISH_ERR.store(0, Relaxed);
    shim::CREATE.store(false, Relaxed);
    assert_eq!(prepare_missing_owner(), Err(ENODATA));
    assert_eq!(shim::OPEN.load(Relaxed), 2);
    assert_eq!(shim::SYNC.load(Relaxed), 1);
    assert_eq!(shim::PARENT.load(Relaxed), 1);

    shim::FAIL_OPEN.store(true, Relaxed);
    assert_eq!(prepare_missing_owner(), Err(EROFS));
    assert_eq!(shim::OPEN.load(Relaxed), 3);
    assert_eq!(shim::SYNC.load(Relaxed), 1);
    assert_eq!(shim::PARENT.load(Relaxed), 1);
}
'''


class OwnerPreparationTests(unittest.TestCase):
    def test_invalid_file_is_preserved_before_sep_write(self):
        source = HARNESS.replace("METHOD", method())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.rs").write_text(source)
            subprocess.run(["rustc", "--edition=2021", "test.rs", "-o", "test"],
                           cwd=path, check=True, capture_output=True, text=True)
            subprocess.run([str(path / "test")], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
