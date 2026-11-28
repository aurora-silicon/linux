#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise ChunkWork's enqueue guard against early retire and early drop.

The production ChunkWork methods are extracted from drivers/gpu/drm/asahi/g17/fence.rs
and compiled against minimal host std stubs. When command k of a chunk retires or
drops before command k+1 attaches, ChunkWork must keep its WorkStateLease alive until
finish_enqueue() releases the initial enqueue guard.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(os.environ.get(
    'G17_ROOT',
    Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17',
))


def extract_block(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def check_wiring(fence_src, queue_src):
    drop_impl = extract_block(fence_src, 'impl Drop for Outputs')
    assert 'for chunk in &self.chunks' in drop_impl, 'Outputs::drop missing chunk finish loop'
    assert 'chunk.finish_enqueue(self.result);' in drop_impl, 'Outputs::drop missing chunk.finish_enqueue'
    submit_fn = extract_block(queue_src, 'fn submit(')
    assert 'chunks.push(chunk.clone(), GFP_KERNEL)?;' in submit_fn, 'submit() must record each ChunkWork'
    assert 'outputs.publish_with_status(' in submit_fn and 'chunks,' in submit_fn, (
        'submit() must hand chunk guards to Outputs'
    )


def build_fixture(fence_src, mutation=None):
    chunk_impl = extract_block(fence_src, 'impl ChunkWork')
    # Translate kernel-specific pin_init!/Arc::pin_init/Mutex calls to std equivalents
    # while keeping the exact counter initializers and lifecycle methods intact.
    new_fn = extract_block(chunk_impl, 'pub(crate) fn new(')
    assert 'remaining: AtomicU32::new(1)' in new_fn, 'ChunkWork::new must start remaining at 1'
    assert 'owners: AtomicU32::new(1)' in new_fn, 'ChunkWork::new must start owners at 1'
    methods = '\n'.join(
        extract_block(chunk_impl, sig)
        for sig in (
            'fn attach(',
            'fn work_state(',
            'fn release(',
            'fn member_done(',
            'fn owner_done(',
            'pub(crate) fn finish_enqueue(',
        )
    )
    init_rem = '1'
    init_own = '1'
    call_finish = True
    if mutation == 'no-guard':
        init_rem = '0'
        init_own = '0'
    elif mutation == 'no-finish-enqueue':
        call_finish = False
    elif mutation is not None:
        raise ValueError(f'unknown mutation {mutation!r}')

    methods = methods.replace('pub(crate) ', '')
    methods = methods.replace('self.work.lock()', 'self.work.lock().unwrap()')
    return f'''
#![allow(dead_code)]
use std::sync::{{Arc, Mutex}};
use std::sync::atomic::{{AtomicBool, AtomicU32, Ordering}};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Error(i32);
const ECANCELED: Error = Error(-125);
const ENOMEM: Error = Error(-12);
type Result<T = ()> = std::result::Result<T, Error>;

struct WorkStateLease(Arc<AtomicBool>);
impl Drop for WorkStateLease {{
    fn drop(&mut self) {{
        self.0.store(true, Ordering::Release);
    }}
}}

struct ChunkWork {{
    work: Mutex<Option<Arc<WorkStateLease>>>,
    remaining: AtomicU32,
    owners: AtomicU32,
    error: AtomicBool,
}}

impl ChunkWork {{
    fn new(work: Arc<WorkStateLease>) -> Arc<Self> {{
        Arc::new(Self {{
            work: Mutex::new(Some(work)),
            remaining: AtomicU32::new({init_rem}),
            owners: AtomicU32::new({init_own}),
            error: AtomicBool::new(false),
        }})
    }}
{methods}
}}

fn finish(chunk: &ChunkWork, result: Result) {{
    if {str(call_finish).lower()} {{
        chunk.finish_enqueue(result);
    }}
}}

fn main() {{
    // 1. Command 0 retires cleanly before command 1 attaches.
    let dropped = Arc::new(AtomicBool::new(false));
    let chunk = ChunkWork::new(Arc::new(WorkStateLease(dropped.clone())));
    chunk.attach();
    assert!(chunk.work_state().is_ok());
    chunk.member_done(Ok(()));
    chunk.owner_done();
    assert!(!dropped.load(Ordering::Acquire), "lease dropped before command 1 attached (retire)");
    chunk.attach();
    assert!(chunk.work_state().is_ok(), "command 1 got ECANCELED after command 0 retired");
    finish(&chunk, Ok(()));
    assert!(!dropped.load(Ordering::Acquire), "lease dropped while command 1 still in flight");
    chunk.member_done(Ok(()));
    assert!(dropped.load(Ordering::Acquire), "lease must release immediately when command 1 retires");
    chunk.owner_done();
    assert_eq!(chunk.remaining.load(Ordering::Acquire), 0);
    assert_eq!(chunk.owners.load(Ordering::Acquire), 0);

    // 2. Command 0 is dropped (canceled) before command 1 attaches.
    let dropped = Arc::new(AtomicBool::new(false));
    let chunk = ChunkWork::new(Arc::new(WorkStateLease(dropped.clone())));
    chunk.attach();
    chunk.member_done(Err(ECANCELED));
    chunk.owner_done();
    assert!(!dropped.load(Ordering::Acquire), "lease dropped before command 1 attached (drop)");
    chunk.attach();
    assert!(chunk.work_state().is_ok(), "command 1 got ECANCELED after command 0 dropped");
    finish(&chunk, Err(ENOMEM));
    assert!(!dropped.load(Ordering::Acquire), "errored chunk must keep lease until owner drops");
    chunk.member_done(Err(ECANCELED));
    assert!(!dropped.load(Ordering::Acquire), "errored chunk must keep lease until owner_done");
    chunk.owner_done();
    assert!(dropped.load(Ordering::Acquire), "errored chunk must release lease once last owner drops");

    // 3. All members retired before finish_enqueue(Ok): finish_enqueue releases immediately.
    let dropped = Arc::new(AtomicBool::new(false));
    let chunk = ChunkWork::new(Arc::new(WorkStateLease(dropped.clone())));
    chunk.attach();
    chunk.member_done(Ok(()));
    chunk.owner_done();
    finish(&chunk, Ok(()));
    assert!(dropped.load(Ordering::Acquire), "finish_enqueue(Ok) must release already-retired chunk");

    // 4. Unused chunk on enqueue error: finish_enqueue(Err) releases immediately.
    let dropped = Arc::new(AtomicBool::new(false));
    let chunk = ChunkWork::new(Arc::new(WorkStateLease(dropped.clone())));
    finish(&chunk, Err(ENOMEM));
    assert!(dropped.load(Ordering::Acquire), "finish_enqueue(Err) must release unused chunk");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=('no-guard', 'no-finish-enqueue'))
    args = parser.parse_args()

    fence_src = (ROOT / 'fence.rs').read_text()
    queue_src = (ROOT / 'queue/mod.rs').read_text()
    check_wiring(fence_src, queue_src)

    src = build_fixture(fence_src, args.mutation)
    with tempfile.TemporaryDirectory() as tmp:
        rs = Path(tmp) / 'test.rs'
        exe = Path(tmp) / 'test'
        rs.write_text(src)
        subprocess.run(['rustc', '--edition=2021', str(rs), '-o', str(exe)], check=True)
        rc = subprocess.run([str(exe)]).returncode
        if rc != 0:
            sys.exit(rc)
    print('PASS g17-chunk-work-guard-tests')


if __name__ == '__main__':
    main()
