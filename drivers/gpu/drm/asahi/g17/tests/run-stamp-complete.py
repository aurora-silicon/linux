#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""CPU-only production KUnit logic and output/ownership regression checks.

Replaces only kernel scaffolding. Never opens a GPU or submits work.
Pass --output on persistent scratch storage; requires Python and rustc.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
g17 = Path(__file__).resolve().parents[1]
sources = {}


def read(name):
    text = (g17 / name).read_text()
    sources[name] = hashlib.sha256(text.encode()).hexdigest()
    return text


def host(text):
    text = text.replace('use kernel::prelude::*;', 'use crate::shim::*;')
    return re.sub(r'#\[kunit_tests\([^\n]+\)\]', '#[cfg(test)]', text)


policy = host(read('stamp_complete.rs'))
render = host(read('queue/render/retirement.rs'))
compute = read('queue/compute/retirement.rs')
# Compile the complete poll/witness logic and its KUnit suite. Shared-ring index
# tests require kernel KVVec and are covered by the kernel build instead.
poll_end = compute.index('/// Walks a completion ring')
tests_start = compute.index('#[kunit_tests(asahi_g17_compute_stamp_wait)]')
tests_end = compute.index('#[kunit_tests(asahi_g17_completion_index)]')
compute = host(compute[:poll_end] + compute[tests_start:tests_end])
compute = compute.replace('use crate::g17::initdata::{CompletionFold, ComputeCompletion};', '')
for name, text in [('policy', policy), ('render', render), ('compute', compute)]:
    (out / (name + '.rs')).write_text(text)
completion = read('completion.rs')


def method(name):
    start = re.search(r'^    (?:pub\(crate\) )?fn ' + name + r'\(', completion, re.M).start()
    begin = completion.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (completion[end] == '{') - (completion[end] == '}')
        end += 1
    return completion[start:end]


# Run the actual output-only callback against mocked resource owners, destinations
# and failure attribution. Physical resources intentionally have no release method:
# adding a release to complete_stamp must fail this host compilation.
callback = r'''
mod output_tests {
    use crate::shim::*;
    use std::cell::{Cell, RefCell};
    use std::sync::atomic::{AtomicU8, AtomicI32, Ordering};
    struct Status(Cell<i32>);
    impl Status { fn get(&self) -> i32 { self.0.get() } }
    struct Output { ticks: RefCell<Option<[u64; 4]>>, calls: Cell<u32> }
    impl Output { fn finish(&self, result: Result<[u64; 4]>) {
        self.ticks.replace(Some(result.unwrap())); self.calls.set(self.calls.get() + 1);
    } }
    struct Feed(Cell<u32>);
    impl Feed { fn note_pass(&self, _: u64, _: u64) { self.0.set(self.0.get()+1); } }
    struct Completion {
        feed: Option<Feed>, compute: Option<()>, released: Cell<bool>, reported: RefCell<Vec<i32>>,
        signalled: AtomicU8, timeout_error: AtomicI32, status: Status,
        interrupted: Cell<bool>, blamed: Cell<bool>, input_error: Cell<Option<i32>>,
        output: Output, terminal: Cell<u32>, failures: RefCell<Vec<i32>>,
        // Retained ownership includes the work-state node as well as VM and rings.
        physical: std::sync::Arc<()>, work: std::sync::Arc<()>,
    }
    impl Completion {
        fn new(physical: std::sync::Arc<()>, work: std::sync::Arc<()>) -> Self {
            Self { feed: Some(Feed(Cell::new(0))), compute: Some(()),
                released: Cell::new(false), reported: RefCell::new(vec![]),
                signalled: AtomicU8::new(0), timeout_error: AtomicI32::new(0),
                status: Status(Cell::new(0)), interrupted: Cell::new(false), blamed: Cell::new(false),
                input_error: Cell::new(None), output: Output { ticks: RefCell::new(None), calls: Cell::new(0) },
                terminal: Cell::new(0), failures: RefCell::new(vec![]), physical, work }
        }
        fn interrupted(&self) -> bool { self.interrupted.get() }
        fn blamed(&self) -> bool { self.blamed.get() }
        fn check_source_inputs(&self) -> Result { self.input_error.get().map_or(Ok(()), Err) }
        fn fail_while_owned(&self, error: i32) { self.failures.borrow_mut().push(error); }
        fn signal_terminal(&self, _: Result<[u64; 4]>) { self.terminal.set(self.terminal.get()+1); }
        fn release_healthy(&self) { self.released.set(true); }
        fn report(&self, error: i32) { self.reported.borrow_mut().push(error); }
        __CALLBACK__
        __COMPLETE__
        __NOTE_PASS__
    }
    #[test]
    fn timestamp_output_is_once_and_ownership_is_retained() {
        let physical = std::sync::Arc::new(()); let work = std::sync::Arc::new(());
        let c = Completion::new(physical.clone(), work.clone());
        c.complete_stamp([10, 10, 10, 20]); c.complete_stamp([30, 30, 30, 40]);
        assert_eq!(*c.output.ticks.borrow(), Some([10, 10, 10, 20]));
        assert_eq!(c.output.calls.get(), 1); assert_eq!(c.terminal.get(), 1);
        assert_eq!(std::sync::Arc::strong_count(&physical), 2);
        assert_eq!(std::sync::Arc::strong_count(&work), 2);
        drop(c);
        assert_eq!(std::sync::Arc::strong_count(&physical), 1);
        assert_eq!(std::sync::Arc::strong_count(&work), 1);
    }
    #[test]
    fn ordinary_output_releases_but_stamp_output_waits_for_retirement_accounting() {
        let c = Completion::new(std::sync::Arc::new(()), std::sync::Arc::new(()));
        c.complete_stamp([10, 20, 0, 0]);
        assert_eq!(c.feed.as_ref().unwrap().0.get(), 0); assert!(!c.released.get());
        c.complete(Ok([10, 20, 0, 0]));
        assert_eq!(c.feed.as_ref().unwrap().0.get(), 1);
        assert_eq!(c.output.calls.get(), 1); assert!(!c.released.get());
        let normal = Completion::new(std::sync::Arc::new(()), std::sync::Arc::new(()));
        normal.complete(Ok([30, 40, 0, 0]));
        assert_eq!(normal.feed.as_ref().unwrap().0.get(), 1);
        assert!(normal.released.get()); assert_eq!(normal.output.calls.get(), 1);
    }
    #[test]
    fn failed_candidate_never_accounts_as_success() {
        let c = Completion::new(std::sync::Arc::new(()), std::sync::Arc::new(()));
        c.signalled.store(1, Ordering::Release); // An ordinary failure won first.
        c.complete_stamp([10, 20, 0, 0]); c.complete(Ok([10, 20, 0, 0]));
        assert_eq!(c.feed.as_ref().unwrap().0.get(), 0); assert_eq!(c.output.calls.get(), 0);
    }
    #[test]
    fn queued_timeout_recovery_and_vm_error_keep_original_attribution() {
        for reason in 0..4 {
            let c = Completion::new(std::sync::Arc::new(()), std::sync::Arc::new(()));
            match reason { 0 => c.interrupted.set(true), 1 => c.blamed.set(true),
                2 => c.timeout_error.store(-110, Ordering::Release), _ => c.status.0.set(-19) }
            c.complete_stamp([10, 20, 0, 0]);
            assert_eq!(c.output.calls.get(), 0); assert!(c.failures.borrow().is_empty());
            assert_eq!(c.signalled.load(Ordering::Acquire), 0);
        }
    }
    #[test]
    fn source_error_is_not_replaced_and_other_command_is_isolated() {
        let failed = Completion::new(std::sync::Arc::new(()), std::sync::Arc::new(()));
        let other = Completion::new(std::sync::Arc::new(()), std::sync::Arc::new(()));
        failed.input_error.set(Some(-125)); failed.complete_stamp([10, 20, 0, 0]);
        assert!(failed.failures.borrow().is_empty());
        assert_eq!(failed.input_error.get(), Some(-125));
        assert_eq!(failed.output.calls.get(), 0); assert_eq!(other.output.calls.get(), 0);
        other.complete_stamp([30, 40, 0, 0]); assert_eq!(*other.output.ticks.borrow(), Some([30, 40, 0, 0]));
    }
}
'''.replace('__CALLBACK__', method('complete_stamp')).replace('__COMPLETE__', method('complete')).replace('__NOTE_PASS__', method('note_pass'))
# These values are public build-time bounds, read from the same source as Kbuild.
asahi = g17.parents[0]
hw = (asahi / 'hw/t8140.rs').read_text()
depth = re.search(r'const RENDER_DEPTH: u8 = (\d+);', hw).group(1)
timeout = (g17 / 'timeout.rs').read_text()
poll_ms = re.search(r'const POLL_MS: [^=]+ = (\d+);', timeout).group(1)
harness = r'''
#![allow(dead_code, unused_imports)]
macro_rules! pr_info { ($($arg:tt)*) => {{ let _ = format_args!($($arg)*); }}; }
mod shim {
    pub type Result<T = ()> = std::result::Result<T, i32>;
    pub const EINVAL: i32 = -22; pub const EIO: i32 = -5;
    pub const EOVERFLOW: i32 = -75; pub const ENODATA: i32 = -61;
    pub const ECANCELED: i32 = -125;
}
mod module_parameters {
    pub struct Param;
    impl Param { pub fn value(&self) -> u32 { 0 } }
    #[allow(non_upper_case_globals)] pub static g17_stamp_complete_arm: Param = Param;
}
mod hw { pub mod t8140 { pub mod queues { pub const RENDER_DEPTH: u8 = __DEPTH__; } } }
mod g17 {
    pub mod job { pub const TIMEOUT_MS: u32 = __TIMEOUT__; }
    #[path = "__POLICY__"] pub(crate) mod stamp_complete;
    pub(crate) mod queue {
        #[path = "__RENDER__"] pub(crate) mod render;
        #[path = "__COMPUTE__"] pub(crate) mod compute;
    }
}
'''.replace('__DEPTH__', depth).replace('__TIMEOUT__', poll_ms)
for name in ['policy', 'render', 'compute']:
    harness = harness.replace('__' + name.upper() + '__', str(out / (name + '.rs')))
unit = out / 'stamp-complete.rs'
unit.write_text(harness + callback)
log = ''
for command in [shlex.split(os.environ.get('RUSTC', 'rustc')) +
                ['--edition=2021', '--test', '-Dwarnings', str(unit), '-o', str(out / 'stamp-complete')],
                [str(out / 'stamp-complete'), '--nocapture']]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log += result.stdout
    print(result.stdout, end='')
    (out / 'stamp-complete.log').write_text(log)
    if result.returncode:
        raise SystemExit(result.returncode)
(out / 'result.json').write_text(json.dumps({
    'passed': True, 'cpu_only': True, 'source_sha256': sources,
    'scope': 'Production policy and render/compute retirement KUnit tests, plus actual output-only callback with mocked destinations and ownership. No DMA, firmware or kernel KUnit execution.',
}, indent=2) + '\n')
