#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production stamp-reseed admission body with retired/live queues.

Jobs model the completion gate and stamp writes; this is not hardware retirement
qualification. The extracted Runtime method must drain every active batch before
changing either engine's epoch, including cross-engine consumers.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def method(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREFIX = r'''
#![allow(dead_code)]
type Result<T = ()> = std::result::Result<T, i32>;
mod m3 { pub mod agx_memory { pub fn publish() {} } }
mod mem { pub fn sync() {} }
struct Job { due: bool, complete: bool, writes: usize }
impl Job {
    fn stamps_need_reseed(&self) -> bool { self.due }
    fn reseed_stamps(&mut self) -> Result {
        if !self.complete { return Err(-16); }
        self.writes += 1; self.due = false; Ok(())
    }
}
enum NativeJob { Render(Job), Compute(Job) }
struct Inner { jobs: Vec<NativeJob>, active: Vec<()> }
impl Inner {
'''
TESTS = r'''
}
fn main() {
    for render_due in [false, true] { for compute_due in [false, true] {
        // Even a due, fully completed producer cannot be reseeded while an
        // active batch could still reference it as a cross-engine predecessor.
        for active in 0..=3 {
            let make = |due| Job { due, complete: true, writes: 0 };
            let mut inner = Inner {
                jobs: vec![NativeJob::Render(make(render_due)), NativeJob::Compute(make(compute_due))],
                active: vec![(); active],
            };
            let due = render_due || compute_due;
            assert_eq!(inner.reseed_stamp_epochs(), Ok(!due || active == 0));
            for (job, was_due) in inner.jobs.iter().zip([render_due, compute_due]) {
                let j = match job { NativeJob::Render(j) | NativeJob::Compute(j) => j };
                assert_eq!(j.writes, usize::from(was_due && active == 0));
                assert_eq!(j.due, was_due && active != 0);
            }
            // Draining the final active batch permits exactly one reset per
            // due engine; repeated admission must not reset it again.
            inner.active.clear();
            assert_eq!(inner.reseed_stamp_epochs(), Ok(true));
            assert_eq!(inner.reseed_stamp_epochs(), Ok(true));
            for (job, was_due) in inner.jobs.iter().zip([render_due, compute_due]) {
                let j = match job { NativeJob::Render(j) | NativeJob::Compute(j) => j };
                assert_eq!(j.writes, usize::from(was_due));
            }
        }
    }}
    // An unexpected incomplete queue must propagate failure, preventing publication.
    for render in [false, true] {
        let j = Job { due: true, complete: false, writes: 0 };
        let mut inner = Inner { jobs: vec![if render { NativeJob::Render(j) } else { NativeJob::Compute(j) }], active: vec![] };
        assert_eq!(inner.reseed_stamp_epochs(), Err(-16));
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=['live-reseed'])
    args = parser.parse_args()
    runtime = (ROOT / 'drivers/gpu/drm/aurora/m3/m3_runtime.rs').read_text()
    body = method(runtime, 'fn reseed_stamp_epochs(')
    if args.mutation:
        gate = 'if !self.active.is_empty() {return Ok(false);}'
        assert gate in body
        body = body.replace(gate, '')
    out = args.out or Path(tempfile.mkdtemp(prefix='m3-stamp-admission-'))
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'admission.rs'
    source.write_text(PREFIX + body + TESTS)
    binary = out / 'admission'
    subprocess.run(['rustc', '--edition=2021', str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)])
    if result.returncode:
        # Preserve the Rust assertion's exit and diagnostic. A Python traceback
        # means a broken harness to run-host-tests, not a killed mutation.
        raise SystemExit(result.returncode)
    print('PASS: production stamp epoch admission, pipeline drain, mixed engines and completion errors')


if __name__ == '__main__':
    main()
