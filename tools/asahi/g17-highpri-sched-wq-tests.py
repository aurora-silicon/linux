#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for G17 high-priority drm_sched submit workqueues.

Compiles production `new_ordered_highpri_reclaim()` / `ordered_flags()` from
`rust/kernel/workqueue.rs` and verifies:
1. `new_ordered_highpri_reclaim()` sets both `WQ_MEM_RECLAIM` and `WQ_HIGHPRI`
   and records `highpri: true` so `restrict()` preserves high priority.
2. `g17/job.rs::Scheduler::new()` allocates `new_ordered_highpri_reclaim()`
   unconditionally (both single-die and multi-die) and calls `queue.restrict()`
   only when `backend.creator_cluster_scheduling()` is true.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WQ = ROOT / 'rust/kernel/workqueue.rs'
JOB = ROOT / 'drivers/gpu/drm/asahi/g17/job.rs'

MUTATIONS = ['drop-highpri', 'drop-reclaim', 'drop-highpri-flag']


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def build_harness(mutation=None):
    wq_src = WQ.read_text()
    job_src = JOB.read_text()

    hp_fn = item(wq_src, 'pub fn new_ordered_highpri_reclaim(')
    ord_fn = item(wq_src, 'fn ordered_flags(')
    sched_fn = item(job_src, 'pub(crate) fn new(dev: &AsahiDrmDevice,')

    if mutation == 'drop-highpri':
        old = 'bindings::wq_flags_WQ_MEM_RECLAIM | bindings::wq_flags_WQ_HIGHPRI'
        assert old in hp_fn
        hp_fn = hp_fn.replace(old, 'bindings::wq_flags_WQ_MEM_RECLAIM', 1)
    elif mutation == 'drop-reclaim':
        old = 'bindings::wq_flags_WQ_MEM_RECLAIM | bindings::wq_flags_WQ_HIGHPRI'
        assert old in hp_fn
        hp_fn = hp_fn.replace(old, 'bindings::wq_flags_WQ_HIGHPRI', 1)
    elif mutation == 'drop-highpri-flag':
        old = 'true,\n        )'
        assert old in hp_fn
        hp_fn = hp_fn.replace(old, 'false,\n        )', 1)

    job_uses_hp = (
        'let queue = workqueue::OwnedQueue::new_ordered_highpri_reclaim(name)?;' in sched_fn
        and 'if backend.creator_cluster_scheduling() {' in sched_fn
        and 'queue.restrict(cluster)?;' in sched_fn
        and 'let submit = Some(queue);' in sched_fn
    )

    return f'''
use std::cell::Cell;
use std::ptr::NonNull;

const ENOMEM: i32 = -12;
type Result<T = ()> = std::result::Result<T, i32>;

struct CStr;
impl CStr {{
    fn as_char_ptr(&self) -> *const u8 {{ std::ptr::null() }}
}}

thread_local! {{
    static LAST_FLAGS: Cell<u32> = const {{ Cell::new(0) }};
}}

mod bindings {{
    use super::LAST_FLAGS;
    #[allow(non_upper_case_globals)]
    pub const wq_flags_WQ_MEM_RECLAIM: u32 = 1 << 3;
    #[allow(non_upper_case_globals)]
    pub const wq_flags_WQ_HIGHPRI: u32 = 1 << 4;

    pub unsafe fn alloc_ordered_workqueue(_name: *const u8, flags: u32) -> *mut u8 {{
        LAST_FLAGS.with(|f| f.set(flags));
        0x1000 as *mut u8
    }}
}}

struct OwnedQueue {{
    #[allow(dead_code)]
    queue: NonNull<u8>,
    ordered: bool,
    unbound: bool,
    highpri: bool,
}}

impl OwnedQueue {{
    {hp_fn}
    {ord_fn}
}}

fn main() {{
    assert!({str(job_uses_hp).lower()}, "g17 Scheduler::new must allocate highpri ordered reclaim queue");
    let q = OwnedQueue::new_ordered_highpri_reclaim(&CStr).unwrap();
    let flags = LAST_FLAGS.with(|f| f.get());
    assert!(flags & bindings::wq_flags_WQ_MEM_RECLAIM != 0, "must set WQ_MEM_RECLAIM");
    assert!(flags & bindings::wq_flags_WQ_HIGHPRI != 0, "must set WQ_HIGHPRI");
    assert!(q.ordered && q.unbound, "must stay ordered and unbound");
    assert!(q.highpri, "must record highpri=true for workqueue_restrict()");
    println!("PASS g17 highpri sched wq");
}}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutation', choices=MUTATIONS)
    args = ap.parse_args()

    src = build_harness(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-highpri-wq-') as tmp:
        rs = os.path.join(tmp, 'harness.rs')
        bin_path = os.path.join(tmp, 'harness')
        Path(rs).write_text(src)
        c = subprocess.run(['rustc', '--edition=2021', '-O', rs, '-o', bin_path], capture_output=True, text=True)
        if c.returncode != 0:
            sys.stderr.write(c.stderr)
            return 2
        r = subprocess.run([bin_path], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode


if __name__ == '__main__':
    sys.exit(main())
