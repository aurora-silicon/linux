# SPDX-License-Identifier: MIT
"""Run the driver's delete-and-persist method with scripted save failures."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[3] / "drivers/soc/apple/sbio.rs"


def driver_method(name):
    source = SOURCE.read_text()
    match = re.search(r"^    fn " + name + r"\(", source, re.M)
    if match is None:
        raise AssertionError(f"missing driver method {name}")
    opening = source.index("{", match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#![allow(dead_code, unused_variables)]
use std::ffi::CStr;
use std::ops::Deref;
use std::sync::atomic::{AtomicBool, Ordering::Relaxed};
use std::sync::{Mutex as StdMutex, MutexGuard};

type Error = i32;
type Result<T> = std::result::Result<T, Error>;
const GFP_KERNEL: u32 = 0;
const EIO: Error = -5;

macro_rules! dev_err { ($($arg:tt)*) => {}; }

struct Mutex<T>(StdMutex<T>);
impl<T> Mutex<T> {
    fn new(value: T) -> Self { Self(StdMutex::new(value)) }
    fn lock(&self) -> MutexGuard<'_, T> { self.0.lock().unwrap() }
}

struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn with_capacity(count: usize, _: u32) -> Result<Self> { Ok(Self(Vec::with_capacity(count))) }
    fn push_within_capacity(&mut self, value: T) -> Result<()> { self.0.push(value); Ok(()) }
    fn is_empty(&self) -> bool { self.0.is_empty() }
    fn iter(&self) -> std::slice::Iter<'_, T> { self.0.iter() }
}
impl<T> Deref for KVec<T> {
    type Target = [T];
    fn deref(&self) -> &[T] { &self.0 }
}

mod sbio {
    #[derive(Clone)]
    pub struct IdentityV1 { pub uuid: [u8; 16] }
    impl IdentityV1 { pub fn uuid(&self) -> &[u8; 16] { &self.uuid } }
}
mod bio {
    pub const UUID_LEN: usize = 16;
    pub struct Handled {
        pub delete_identity: Option<super::sbio::IdentityV1>,
        pub delete_identities: Vec<super::sbio::IdentityV1>,
    }
}

struct Index { held: Vec<[u8; 16]> }
impl Index {
    fn contains_uuid(&self, uuid: &[u8; 16]) -> bool { self.held.contains(uuid) }
}

struct SepData {
    dev: (),
    deletion_unsaved: AtomicBool,
    bio_index: Mutex<Index>,
    delete_replies: Mutex<Vec<Result<bool>>>,
    save_replies: Mutex<Vec<Result<()>>>,
    events: Mutex<Vec<String>>,
}
impl SepData {
    fn new(uuid: [u8; 16], saves: Vec<Result<()>>) -> Self {
        Self {
            dev: (), deletion_unsaved: AtomicBool::new(false),
            bio_index: Mutex::new(Index { held: vec![uuid] }),
            delete_replies: Mutex::new(vec![Ok(true), Ok(true)]),
            save_replies: Mutex::new(saves), events: Mutex::new(Vec::new()),
        }
    }
    fn delete_identity(&self, _: &sbio::IdentityV1, recovery: bool) -> Result<bool> {
        self.events.lock().push(format!("delete:{recovery}"));
        self.delete_replies.lock().remove(0)
    }
    fn persist_deletion(&self, retry: bool, recovery: bool) -> Result<()> {
        self.events.lock().push(format!("save:{retry}:{recovery}"));
        self.save_replies.lock().remove(0)
    }
    fn forget_identity(&self, uuid: &[u8; 16], _: &CStr) {
        self.events.lock().push("forget".into());
        self.bio_index.lock().held.retain(|held| held != uuid);
    }
METHOD
}

fn handled(uuid: [u8; 16]) -> bio::Handled {
    bio::Handled { delete_identity: Some(sbio::IdentityV1 { uuid }), delete_identities: vec![] }
}

fn main() {
    let uuid = [0xa5; 16];
    let sep = SepData::new(uuid, vec![Ok(())]);
    sep.delete_and_persist(&handled(uuid), true).unwrap();
    assert_eq!(*sep.events.lock(), ["delete:true", "save:false:true", "forget"]);
    assert!(!sep.bio_index.lock().contains_uuid(&uuid));

    let sep = SepData::new(uuid, vec![Err(EIO), Ok(())]);
    assert!(sep.delete_and_persist(&handled(uuid), true).is_err());
    assert!(sep.deletion_unsaved.load(Relaxed));
    assert!(sep.bio_index.lock().contains_uuid(&uuid));
    sep.delete_and_persist(&handled(uuid), true).unwrap();
    assert_eq!(*sep.events.lock(), ["delete:true", "save:false:true",
                                     "delete:true", "save:true:true", "forget"]);
    assert!(!sep.deletion_unsaved.load(Relaxed));
    assert!(!sep.bio_index.lock().contains_uuid(&uuid));

    let sep = SepData::new(uuid, vec![]);
    *sep.delete_replies.lock() = vec![Err(EIO)];
    assert!(sep.delete_and_persist(&handled(uuid), false).is_err());
    assert_eq!(*sep.events.lock(), ["delete:false"]);
    assert!(sep.bio_index.lock().contains_uuid(&uuid));
}
'''


class OrphanDeleteTests(unittest.TestCase):
    def test_failure_keeps_index_and_retry_saves(self):
        source = HARNESS.replace("METHOD", driver_method("delete_and_persist"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.rs").write_text(source)
            subprocess.run(["rustc", "--edition=2021", "test.rs", "-o", "test"],
                           cwd=path, check=True, capture_output=True, text=True)
            subprocess.run([str(path / "test")], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
