#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production G17 fence-error count bodies on host models.

Submission::member_done and note_error, and Outputs::new, finish, install and
Drop (g17/fence.rs), and the VmStatus count, mirror registration and release
bodies (g17/status.rs) are compiled unchanged. Fences, syncobjs, GEM objects
and kernel mutexes are models: a model fence records the count and the mirrored
word when it signals, a model output sync the error and the word when a fence
is installed in it. Checks: an output fence that signals with an error is
counted once, before it signals, keyed on the final error; the error fence of a
SUBMIT that fails before publication (allocation failure, or ERESTARTSYS and
other errors before publish) is counted before it is installed; success and a
malformed sync split are never counted; registration starts at the current
count and VM drop releases the object; concurrent counts never move the word
backwards.
With --mutations, each listed source change must die at its intended assertion.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / "drivers/gpu/drm/asahi/g17"


def extract(text, marker):
    if text.count(marker) != 1:
        raise RuntimeError(f"marker not unique: {marker!r}")
    start = text.index(marker)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def line(text, marker):
    lines = [entry for entry in text.splitlines() if marker in entry]
    if len(lines) != 1:
        raise RuntimeError(f"line not unique: {marker!r}")
    return lines[0]


HARNESS = r'''
#![allow(dead_code)]
use std::sync::{Arc, Mutex as StdMutex, MutexGuard};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, AtomicU8, AtomicUsize, Ordering};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Error(i32);
impl Error {
    fn from_errno(errno: i32) -> Self { assert!(errno < 0); Self(errno) }
    fn to_errno(self) -> i32 { self.0 }
}
type Result<T = ()> = core::result::Result<T, Error>;
const ENOENT: Error = Error(-2); const EIO: Error = Error(-5); const ENOMEM: Error = Error(-12);
const EBUSY: Error = Error(-16); const EINVAL: Error = Error(-22); const ENODATA: Error = Error(-61);
const ECANCELED: Error = Error(-125); const ERESTARTSYS: Error = Error(-512);
struct Mutex<T>(StdMutex<T>);
impl<T> Mutex<T> { fn lock(&self) -> MutexGuard<'_, T> { self.0.lock().unwrap() } }

mod gem {
    use std::sync::{Arc, atomic::{AtomicU64, AtomicUsize, Ordering}};
    pub struct VMap { pub addr: *mut core::ffi::c_void }
    /// A GEM object reference: `words` backs `size` bytes; the test keeps its own view.
    pub struct ObjectRef { pub words: Arc<Vec<AtomicU64>>, pub size: usize, map: Option<VMap>, pub drops: Arc<AtomicUsize> }
    impl ObjectRef {
        pub fn new(words: &Arc<Vec<AtomicU64>>, size: usize, drops: &Arc<AtomicUsize>) -> Self {
            assert!(size <= words.len() * 8);
            Self { words: words.clone(), size, map: None, drops: drops.clone() }
        }
        pub fn size(&self) -> usize { self.size }
        pub fn vmap(&mut self) -> super::Result<&VMap> {
            let addr = self.words.as_ptr() as *mut core::ffi::c_void;
            Ok(self.map.get_or_insert(VMap { addr }))
        }
    }
    impl Drop for ObjectRef { fn drop(&mut self) { self.drops.fetch_add(1, Ordering::SeqCst); } }
}

__STATUS_ITEMS__

struct VmStatus { reported_error: AtomicI32, client_closed: AtomicBool, mirror: Mutex<Mirrors> }
impl VmStatus {
    fn new() -> Self {
        Self {
            reported_error: AtomicI32::new(0),
            client_closed: AtomicBool::new(false),
            mirror: Mutex(StdMutex::new(Mirrors { status: None, fence_errors: 0, fence_error_word: None })),
        }
    }
    fn count(&self) -> u64 { self.mirror.lock().fence_errors }
__STATUS__
}

/// The model fence notes, when it signals, the VM's count and the mirrored word.
struct ExportFence { scoped: bool, waited: AtomicBool }
struct UserFence<T> {
    payload: T,
    status: Arc<VmStatus>,
    view: Option<(Arc<Vec<AtomicU64>>, usize)>,
    error: AtomicI32,
    signalled: AtomicBool,
    seen: StdMutex<Option<(u64, Option<u64>)>>,
}
impl<T> core::ops::Deref for UserFence<T> { type Target = T; fn deref(&self) -> &T { &self.payload } }
impl UserFence<ExportFence> {
    fn new(status: &Arc<VmStatus>, view: Option<(Arc<Vec<AtomicU64>>, usize)>) -> Self {
        Self { payload: ExportFence { scoped: false, waited: AtomicBool::new(false) }, status: status.clone(), view,
               error: AtomicI32::new(0), signalled: AtomicBool::new(false), seen: StdMutex::new(None) }
    }
    fn set_error(&self, error: Error) {
        assert!(!self.signalled.load(Ordering::SeqCst), "error set after signal");
        self.error.store(error.to_errno(), Ordering::SeqCst);
    }
    fn signal(&self) {
        assert!(!self.signalled.swap(true, Ordering::SeqCst), "signalled twice");
        let word = self.view.as_ref().map(|(words, index)| words[*index].load(Ordering::Acquire));
        *self.seen.lock().unwrap() = Some((self.status.count(), word));
    }
    fn seen(&self) -> (u64, Option<u64>) { self.seen.lock().unwrap().expect("fence signalled") }
    fn errno(&self) -> i32 { assert!(self.signalled.load(Ordering::SeqCst)); self.error.load(Ordering::SeqCst) }
}
fn signal_exported(fence: &UserFence<ExportFence>) { fence.signal() }
fn unwaited(_: &AtomicBool) {}
mod trace { pub fn output(_: &super::UserFence<super::ExportFence>, _: i32) {} }

struct Submission { remaining: AtomicU8, error: AtomicI32, status: Arc<VmStatus>, released: AtomicBool }
impl Submission {
    fn new(status: &Arc<VmStatus>, members: u8) -> Self {
        // The enqueue guard plus each member, as Aggregate::member counts them.
        Self { remaining: AtomicU8::new(members + 1), error: AtomicI32::new(0), status: status.clone(),
               released: AtomicBool::new(false) }
    }
    fn release_state(&self) { self.released.store(true, Ordering::SeqCst); }
__FENCE__
}

/// What output syncs received: the installed fence's errno and the mirrored word then.
type Log = Arc<StdMutex<Vec<(i32, u64)>>>;
type View = (Arc<Vec<AtomicU64>>, usize);
type KVec<T> = Vec<T>;
type UniqueFence<T> = UserFence<T>;
#[derive(Clone)]
struct Fence { errno: i32 }
impl Fence {
    fn from_fence(fence: &UserFence<ExportFence>) -> Self { Self { errno: fence.error.load(Ordering::SeqCst) } }
}
fn note(view: &View, log: &Log, fence: &Fence) {
    log.lock().unwrap().push((fence.errno, view.0[view.1].load(Ordering::Acquire)));
}
struct SyncObj { view: View, log: Log }
impl SyncObj {
    fn add_point(&self, _chain: (), fence: &Fence, _point: u64) { note(&self.view, &self.log, fence) }
    fn replace_fence(&self, fence: Option<&Fence>) { note(&self.view, &self.log, fence.unwrap()) }
}
mod sync_output {
    pub struct SignalBundle { pub view: super::View, pub log: super::Log }
    impl SignalBundle { pub fn fail(&mut self, fence: &super::Fence) { super::note(&self.view, &self.log, fence) } }
}
struct SyncItem { syncobj: SyncObj, chain_fence: Option<()>, timeline_value: u64, cumulative: Option<sync_output::SignalBundle> }
struct FenceContexts { fail: bool, status: Arc<VmStatus>, view: Option<View> }
fn export_fallback(contexts: &FenceContexts) -> Result<UniqueFence<ExportFence>> {
    if contexts.fail { return Err(ENOMEM); }
    Ok(UserFence::new(&contexts.status, contexts.view.clone()))
}
/// Published submissions end in member_done, exercised above.
struct Aggregate;
impl Aggregate {
    fn fence(&self) -> Fence { unreachable!() }
    fn finish_enqueue(&self, _: Result) { unreachable!() }
}
__OUTPUTS__

/// One input sync, then `outputs` output syncs: a timeline point, then binary syncobjs.
fn syncs(view: &View, log: &Log, outputs: usize, cumulative: bool) -> Vec<SyncItem> {
    (0..=outputs).map(|index| SyncItem {
        syncobj: SyncObj { view: view.clone(), log: log.clone() },
        chain_fence: (index == 1).then_some(()),
        timeline_value: 7,
        cumulative: (cumulative && index == 1)
            .then(|| sync_output::SignalBundle { view: view.clone(), log: log.clone() }),
    }).collect()
}

fn object(words: usize) -> (Arc<Vec<AtomicU64>>, Arc<AtomicUsize>) {
    (Arc::new((0..words).map(|_| AtomicU64::new(0xdead_beef)).collect()), Arc::new(AtomicUsize::new(0)))
}

/// Complete one submission: the enqueue guard first, then each member's result.
fn run(status: &Arc<VmStatus>, view: &Option<(Arc<Vec<AtomicU64>>, usize)>, results: &[Result], early: Option<Error>)
    -> UserFence<ExportFence> {
    let fence = UserFence::new(status, view.clone());
    let submission = Submission::new(status, results.len() as u8);
    if let Some(error) = early { submission.note_error(error); }
    submission.member_done(&fence, Ok(()));
    for result in results { submission.member_done(&fence, *result); }
    assert!(fence.signalled.load(Ordering::SeqCst));
    assert_eq!(submission.released.load(Ordering::SeqCst), fence.errno() == 0);
    fence
}

fn main() {
    let status = Arc::new(VmStatus::new());
    // A failure before registration is counted and becomes the word's first value.
    let early = run(&status, &None, &[Err(ECANCELED)], None);
    assert_eq!(early.seen(), (1, None), "failed fence counted before it signals");
    let (words, drops) = object(2048);
    let size = 16 * 1024;
    assert_eq!(status.attach_fence_error_mirror(gem::ObjectRef::new(&words, size, &drops), 4), Err(EINVAL),
               "misaligned offset refused");
    assert_eq!(status.attach_fence_error_mirror(gem::ObjectRef::new(&words, size, &drops), size), Err(EINVAL),
               "offset past the object refused");
    let (big, big_drops) = object(80 * 128);
    assert_eq!(status.attach_fence_error_mirror(gem::ObjectRef::new(&big, 80 * 1024, &big_drops), 0), Err(EINVAL),
               "object over 64 KiB refused");
    assert_eq!(drops.load(Ordering::SeqCst), 2, "refused registrations keep no reference");
    status.attach_fence_error_mirror(gem::ObjectRef::new(&words, size, &drops), 8).unwrap();
    assert_eq!(words[1].load(Ordering::SeqCst), 1, "registration starts at the current count");
    assert_eq!(words[0].load(Ordering::SeqCst), 0xdead_beef, "registration writes only its word");
    assert_eq!(status.attach_fence_error_mirror(gem::ObjectRef::new(&words, size, &drops), 0), Err(EBUSY),
               "one registration per VM");
    let view = Some((words.clone(), 1));

    let ok = run(&status, &view, &[Ok(()), Ok(())], None);
    assert_eq!((ok.errno(), ok.seen()), (0, (1, Some(1))), "success leaves the count");
    let both = run(&status, &view, &[Err(ECANCELED), Err(ENODATA)], None);
    assert_eq!(both.errno(), ECANCELED.to_errno());
    assert_eq!(both.seen(), (2, Some(2)), "one count per failed fence, stored before it signals");
    let noted = run(&status, &view, &[Ok(())], Some(EIO));
    assert_eq!(noted.errno(), EIO.to_errno());
    assert_eq!(noted.seen(), (3, Some(3)), "an error noted early is counted on the final error");
    let late = run(&status, &view, &[Ok(()), Err(ENOMEM)], None);
    assert_eq!(late.seen(), (4, Some(4)), "failed fence counted before it signals");
    println!("PASS sequential: counted once per failed fence before its signal, success uncounted, \
              registration bounds/alignment/EBUSY, starts at the current count");

    // Concurrent failures: the client's protocol holds (sample the word before the submission;
    // a failed fence, once signalled, shows a larger word), and the word never goes back.
    let stop = Arc::new(AtomicBool::new(false));
    let reader = {
        let (words, stop) = (words.clone(), stop.clone());
        std::thread::spawn(move || {
            let mut last = 0;
            while !stop.load(Ordering::Acquire) {
                let now = words[1].load(Ordering::Acquire);
                assert!(now >= last, "the word never goes back");
                last = now;
            }
        })
    };
    let workers: Vec<_> = (0..8).map(|worker| {
        let (status, view) = (status.clone(), view.clone());
        std::thread::spawn(move || {
            for round in 0..2000u32 {
                let result = if (worker + round) % 3 == 0 { Ok(()) } else { Err(ECANCELED) };
                let before = view.as_ref().unwrap().0[1].load(Ordering::Acquire);
                let fence = run(&status, &view, &[result], None);
                let (count, word) = fence.seen();
                assert!(word.unwrap() <= count);
                if result.is_err() { assert!(word.unwrap() > before, "failed fence counted before it signals"); }
            }
        })
    }).collect();
    for worker in workers { worker.join().unwrap(); }
    stop.store(true, Ordering::Release);
    reader.join().unwrap();
    let failures = (0..8u32).map(|w| (0..2000u32).filter(|r| (w + r) % 3 != 0).count() as u64).sum::<u64>();
    assert_eq!((status.count(), words[1].load(Ordering::SeqCst)), (4 + failures, 4 + failures));
    println!("PASS concurrent: {} failures from 8 threads, word == count, monotonic", failures);

    // A SUBMIT that fails before publication installs an error fence instead of a published
    // submission's: it is counted before any output receives it.
    let outs = Arc::new(VmStatus::new());
    let (owords, odrops) = object(2048);
    outs.attach_fence_error_mirror(gem::ObjectRef::new(&owords, size, &odrops), 0).unwrap();
    let oview: View = (owords.clone(), 0);
    let log: Log = Default::default();
    let taken = |log: &Log| std::mem::take(&mut *log.lock().unwrap());
    let alloc = Fence { errno: ENOMEM.to_errno() };
    let failing = FenceContexts { fail: true, status: outs.clone(), view: Some(oview.clone()) };
    let working = FenceContexts { fail: false, status: outs.clone(), view: Some(oview.clone()) };
    assert_eq!(Outputs::new(&failing, syncs(&oview, &log, 2, false), 1, &alloc, &outs).err(), Some(ENOMEM));
    assert_eq!(taken(&log), [(ENOMEM.to_errno(), 1); 2], "allocation failure counted before it is installed");
    assert_eq!(Outputs::new(&failing, syncs(&oview, &log, 2, true), 1, &alloc, &outs).err(), Some(ENOMEM));
    assert_eq!(taken(&log), [(ENOMEM.to_errno(), 2)], "allocation failure counted before it is installed");
    // A malformed split installs nothing and counts nothing.
    assert_eq!(Outputs::new(&working, syncs(&oview, &log, 0, false), 2, &alloc, &outs).err(), Some(EINVAL));
    assert_eq!((taken(&log).len(), outs.count()), (0, 2), "a malformed split counts nothing");
    // An interrupted SUBMIT (restarted later at the same points), and any other error before
    // publication: the fallback fence carries the error and is counted first.
    let outputs = Outputs::new(&working, syncs(&oview, &log, 2, false), 1, &alloc, &outs).unwrap();
    assert_eq!(outs.count(), 2, "prepared outputs count nothing");
    assert_eq!(outputs.finish(Err(ERESTARTSYS)), Err(ERESTARTSYS));
    assert_eq!(taken(&log), [(ERESTARTSYS.to_errno(), 3); 2], "a failed SUBMIT's fence is counted before it is installed");
    let outputs = Outputs::new(&working, syncs(&oview, &log, 1, true), 1, &alloc, &outs).unwrap();
    assert_eq!(outputs.finish(Err(EBUSY)), Err(EBUSY));
    assert_eq!(taken(&log), [(EBUSY.to_errno(), 4)], "a failed SUBMIT's fence is counted before it is installed");
    assert_eq!(outs.count(), 4);
    println!("PASS unpublished: allocation-failure and fallback fences counted before they are \
              installed (plain and cumulative outputs), malformed split uncounted");

    // VM drop: both objects go, later failures still count but write nothing.
    status.attach_mirror(gem::ObjectRef::new(&words, size, &drops), 16).unwrap();
    status.release_mirror();
    assert_eq!(drops.load(Ordering::SeqCst), 5, "VM drop releases the object");
    let after = run(&status, &None, &[Err(EIO)], None);
    assert_eq!(after.seen().0, 5 + failures);
    assert_eq!(words[1].load(Ordering::SeqCst), 4 + failures, "no store after release");
    assert_eq!(status.attach_fence_error_mirror(gem::ObjectRef::new(&words, size, &drops), 8), Err(ENOENT),
               "no registration after VM drop");
    assert_eq!(status.attach_mirror(gem::ObjectRef::new(&words, size, &drops), 16), Err(ENOENT),
               "no status mirror after VM drop");
    assert_eq!(drops.load(Ordering::SeqCst), 7, "refused registrations keep no reference");
    assert!(status.mirror.lock().status.is_none() && status.mirror.lock().fence_error_word.is_none(),
            "a released status holds no object, so its last put sleeps nowhere");
    println!("PASS release: VM drop releases both objects, later errors counted but not stored, \
              no registration of either mirror after it");
}
'''

# (old, new, message the mutant must die with)
MEMBER_COUNT = ("                self.status.count_fence_error();\n"
                "                fence.set_error(Error::from_errno(error));\n")
MEMBER_SET = "                fence.set_error(Error::from_errno(error));\n"
FALLBACK_COUNT = ("                self.status.count_fence_error();\n"
                  "                fallback.set_error(error);\n")
FALLBACK_SET = "                fallback.set_error(error);\n"
MUTATIONS = {
    "count-after-signal": [
        (MEMBER_COUNT, MEMBER_SET),
        ("            unwaited(&fence.waited);",
         "            if error != 0 { self.status.count_fence_error(); }\n            unwaited(&fence.waited);"),
    ], "count-per-member-result": [
        (MEMBER_COUNT, MEMBER_SET),
        ("            self.note_error(error);\n", "            self.note_error(error);\n            self.status.count_fence_error();\n"),
    ], "count-success-too": [
        (MEMBER_COUNT, MEMBER_SET),
        ("            let error = self.error.load(Ordering::Acquire);\n",
         "            let error = self.error.load(Ordering::Acquire);\n            self.status.count_fence_error();\n"),
    ], "registration-starts-at-zero": [
        (".store(mirrors.fence_errors, Ordering::Release);\n        mirrors.fence_error_word",
         ".store(0, Ordering::Release);\n        mirrors.fence_error_word"),
    ], "release-keeps-object": [
        ("mirrors.fence_error_word.take())", "None::<Mirror<u64>>)"),
    ], "registration-after-drop": [
        ("        // As in attach_mirror: a registration racing with VM destruction is refused.\n"
         "        if self.client_closed() {\n            return Err(ENOENT);\n        }\n", ""),
    ], "fallback-uncounted": [
        (FALLBACK_COUNT, FALLBACK_SET),
    ], "fallback-counted-after-install": [
        (FALLBACK_COUNT, FALLBACK_SET),
        ("            trace::output(\n                &fallback,",
         "            if self.result.is_err() { self.status.count_fence_error(); }\n"
         "            trace::output(\n                &fallback,"),
    ], "status-mirror-after-drop": [
        ("        let mut mirrors = self.mirror.lock();\n        if self.client_closed() {\n"
         "            return Err(ENOENT);\n        }\n        if mirrors.status.is_some() {",
         "        let mut mirrors = self.mirror.lock();\n        if mirrors.status.is_some() {"),
    ], "allocation-failure-uncounted": [
        ("                status.count_fence_error();\n", ""),
    ],
}
EXPECTED = {
    "count-after-signal": "failed fence counted before it signals",
    "count-per-member-result": "one count per failed fence",
    "count-success-too": "success leaves the count",
    "registration-starts-at-zero": "registration starts at the current count",
    "release-keeps-object": "VM drop releases the object",
    "registration-after-drop": "no registration after VM drop",
    "fallback-uncounted": "a failed SUBMIT's fence is counted before it is installed",
    "fallback-counted-after-install": "a failed SUBMIT's fence is counted before it is installed",
    "allocation-failure-uncounted": "allocation failure counted before it is installed",
    "status-mirror-after-drop": "no status mirror after VM drop",
}


def fixture():
    fence = (ROOT / "fence.rs").read_text()
    status = (ROOT / "status.rs").read_text()
    fence_methods = "\n".join(extract(fence, marker) for marker in [
        "    fn note_error(&self, error: Error) -> bool {", "    fn member_done("])
    fence_methods = fence_methods.replace("super::trace::", "trace::")
    outputs = "\n".join([
        extract(fence, "fn install("), extract(fence, "pub(crate) struct Outputs {"),
        "impl Outputs {", extract(fence, "    pub(crate) fn new(\n        contexts: &FenceContexts,"),
        extract(fence, "    pub(crate) fn finish(mut self"), "}", extract(fence, "impl Drop for Outputs {")])
    outputs = outputs.replace("super::trace::", "trace::").replace("super::sync_output::", "sync_output::")
    items = "\n".join([line(status, "const MIRROR_MAX_SIZE"), extract(status, "struct Mirror<T> {"),
                       line(status, "unsafe impl<T> Send for Mirror<T>"), extract(status, "struct Mirrors {"),
                       extract(status, "fn map_word<T>(")])
    methods = "\n".join(extract(status, marker) for marker in [
        "    pub(crate) fn count_fence_error(", "    pub(crate) fn attach_fence_error_mirror(",
        "    pub(crate) fn release_mirror(", "    pub(crate) fn mark_client_closed(",
        "    pub(crate) fn attach_mirror(",
        "    pub(crate) fn client_closed("])
    return (HARNESS.replace("__STATUS_ITEMS__", items).replace("__STATUS__", methods)
            .replace("__FENCE__", fence_methods).replace("__OUTPUTS__", outputs))


def build_and_run(work, name, source):
    path = work / (name + ".rs")
    path.write_text(source)
    binary = work / name
    built = subprocess.run([os.environ.get("RUSTC", "rustc"), "--edition=2021", "-O", "-Dwarnings",
                            str(path), "-o", str(binary)], capture_output=True, text=True)
    if built.returncode:
        raise RuntimeError(f"{name}: NOBUILD, not a runtime kill:\n{built.stderr}")
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    control = fixture()
    with tempfile.TemporaryDirectory(prefix="asahi-g17-errcount-") as temporary:
        work = Path(temporary)
        ran = build_and_run(work, "control", control)
        print(ran.stdout, end="")
        if ran.returncode:
            raise SystemExit("control failed:\n" + ran.stderr)
        if args.mutations:
            for name, changes in MUTATIONS.items():
                source = control
                for old, new in changes:
                    if source.count(old) != 1:
                        raise RuntimeError(f"{name}: anchor count {source.count(old)}: {old!r}")
                    source = source.replace(old, new)
                ran = build_and_run(work, name, source)
                if ran.returncode == 0 or EXPECTED[name] not in ran.stderr:
                    raise SystemExit(f"{name}: did not die at '{EXPECTED[name]}':\n{ran.stdout}{ran.stderr}")
                print(f"KILLED {name}: {EXPECTED[name]}")
    print("PASS: G17 fence-error count")


if __name__ == "__main__":
    main()
