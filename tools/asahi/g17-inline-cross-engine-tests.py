#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for G17 inline cross-engine publication behind already-published producers.

Compiles production `has_host_dependencies()` (`job.rs`), the firmware-producer
dependency filtering in `queue/mod.rs`, and the early-publication guards in
`try_compute_early()` (`runtime.rs`) and `try_render_early()`
(`runtime/render/submit.rs`) to prove:
1. Already-inline-published or already-scheduled cross-engine producers do not
   add `scheduled` waits to `dependencies`, so consumers stay eligible for
   inline publication while retaining firmware completion ordering.
2. Unpublished cross-engine producers still add their `scheduled` fence to
   `dependencies` so the consumer waits for producer publication in `drm_sched`.
3. `try_compute_early()` and `try_render_early()` gate on host dependencies
   only (`has_host_dependencies()`), allowing firmware-ordered cross-engine
   packets to publish inline while still keeping host-waited packets on
   `drm_sched`.
4. Failed firmware producers are caught by `check_firmware_producers()` in both
   `try_compute_early()` and `try_render_early()`, failing the consumer
   unpublished with the producer's error before any hardware publication.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / 'drivers/gpu/drm/asahi/g17'

MUTATIONS = [
    'always-wait-scheduled',
    'skip-unpublished-scheduled',
    'block-firmware-early',
    'skip-compute-producer-check',
    'skip-render-producer-check',
]


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
    job_src = (G17 / 'job.rs').read_text()
    queue_src = (G17 / 'queue/mod.rs').read_text()
    runtime_src = (G17 / 'runtime.rs').read_text()
    submit_src = (G17 / 'runtime/render/submit.rs').read_text()

    has_host_fn = item(job_src, 'pub(crate) fn has_host_dependencies(')
    has_deps_fn = item(job_src, 'pub(crate) fn has_dependencies(')
    comp_early_fn = item(runtime_src, 'pub(super) fn try_compute_early(')
    rend_early_fn = item(submit_src, 'pub(in crate::g17) fn try_render_early(')

    # Extract the exact producer-wait loop from Queue::enqueue().
    loop_sig = 'for ((scheduled, completed), published) in ordering_firmware_waits'
    assert queue_src.count(loop_sig) == 1, loop_sig
    loop_start = queue_src.index(loop_sig)
    loop_end = queue_src.index('producer_waits.finish(', loop_start)
    loop_block = queue_src[loop_start:loop_end]

    if mutation == 'always-wait-scheduled':
        old = 'if !*published && job::fence_status(scheduled) <= 0 {'
        assert old in loop_block
        loop_block = loop_block.replace(old, 'if true {', 1)
    elif mutation == 'skip-unpublished-scheduled':
        old = 'if !*published && job::fence_status(scheduled) <= 0 {'
        assert old in loop_block
        loop_block = loop_block.replace(old, 'if false {', 1)
    elif mutation == 'block-firmware-early':
        old = 'packet.has_host_dependencies()'
        assert old in comp_early_fn and old in rend_early_fn
        comp_early_fn = comp_early_fn.replace(old, 'packet.has_dependencies()', 1)
        rend_early_fn = rend_early_fn.replace(old, 'packet.has_dependencies()', 1)
    elif mutation == 'skip-compute-producer-check':
        old = 'if let Err(error) = packet.check_firmware_producers() {'
        assert old in comp_early_fn
        comp_early_fn = comp_early_fn.replace(old, 'if let Err::<(), i32>(error) = Ok(()) {', 1)
    elif mutation == 'skip-render-producer-check':
        old = 'if let Err(error) = packet.check_firmware_producers() {'
        assert old in rend_early_fn
        rend_early_fn = rend_early_fn.replace(old, 'if let Err::<(), i32>(error) = Ok(()) {', 1)

    # Verify structural pieces that feed into inline_published.
    fences_has_inline = (
        'pub(crate) inline_published: bool,' in job_src
        and 'inline_published: inline,' in job_src
        and 'firmware_published.push(entry.inline_published(), GFP_KERNEL)?;' in queue_src
    )

    # Extract the guard prefix of try_compute_early and try_render_early up to begin_publication.
    comp_guard_start = comp_early_fn.index('{') + 1
    comp_guard_end = comp_early_fn.index('let mut deferred =', comp_guard_start)
    comp_guard = comp_early_fn[comp_guard_start:comp_guard_end]

    rend_guard_start = rend_early_fn.index('{') + 1
    rend_guard_end = rend_early_fn.index('let admission =', rend_guard_start)
    rend_guard = rend_early_fn[rend_guard_start:rend_guard_end]

    # Normalize loop_block for std Vec instead of KVec(..., GFP_KERNEL)?.
    loop_std = (
        loop_block
        .replace('Ok(dependencies.push(fence.clone(), GFP_KERNEL)?)', '{ dependencies.push(fence.clone()); Ok::<(), i32>(()) }')
        .replace('firmware.push(completed.clone(), GFP_KERNEL)?;', 'firmware.push(completed.clone());')
    )

    return f'''
use std::cell::Cell;
use std::sync::atomic::{{AtomicUsize, Ordering}};

const ECANCELED: i32 = -125;
const EIO: i32 = -5;
type Result<T = ()> = std::result::Result<T, i32>;

#[derive(Clone, Copy)]
struct Fence {{
    id: u32,
    status: i32,
}}

mod job {{
    use super::Fence;
    pub fn fence_status(f: &Fence) -> i32 {{
        f.status
    }}
}}

struct ProducerWaits {{
    last: Option<Fence>,
}}

impl ProducerWaits {{
    fn new() -> Self {{ Self {{ last: None }} }}
    fn producer(&mut self, fence: &Fence, mut emit: impl FnMut(&Fence) -> Result) -> Result {{
        if let Some(prev) = self.last.replace(*fence) {{
            emit(&prev)?;
        }}
        Ok(())
    }}
    fn finish(self, mut emit: impl FnMut(&Fence) -> Result) -> Result {{
        if let Some(last) = self.last {{
            emit(&last)?;
        }}
        Ok(())
    }}
}}

struct Completion {{
    failed_unpublished: Cell<Option<i32>>,
}}

impl Completion {{
    fn fail_unpublished(&self, err: i32) {{
        self.failed_unpublished.set(Some(err));
    }}
}}

struct Packet {{
    inputs: Vec<Fence>,
    firmware: Vec<Fence>,
    producer_err: Option<i32>,
    began: Cell<bool>,
    early_result: Cell<Option<Result>>,
    completion: Completion,
}}

impl Packet {{
    {has_deps_fn.replace('pub(crate) ', '')}
    {has_host_fn.replace('pub(crate) ', '')}
    fn check_firmware_producers(&self) -> Result {{
        match self.producer_err {{
            Some(err) => Err(err),
            None => Ok(()),
        }}
    }}
    fn begin_publication(&self) -> bool {{
        let ok = !self.began.get();
        self.began.set(true);
        ok
    }}
    fn set_early_result(&self, res: Result) {{
        self.early_result.set(Some(res));
    }}
}}

struct Backend {{
    backlog: AtomicUsize,
    compute_backlog: AtomicUsize,
    traversal_ok: bool,
}}

impl Backend {{
    fn traversal_inline(&self, _packet: &Packet) -> bool {{
        self.traversal_ok
    }}
    fn run_compute_early_guard(&self, packet: &Packet) -> bool {{
        {comp_guard}
        true
    }}
    fn run_render_early_guard(&self, packet: &Packet) -> bool {{
        {rend_guard}
        true
    }}
}}

fn filter_producers(
    ordering_firmware_waits: &[(Fence, Fence)],
    ordering_firmware_published: &[bool],
) -> (Vec<Fence>, Vec<Fence>) {{
    let mut dependencies: Vec<Fence> = Vec::new();
    let mut firmware: Vec<Fence> = Vec::new();
    let mut producer_waits = ProducerWaits::new();
    let _: Result = (|| {{
        {loop_std}
        producer_waits.finish(|fence| {{ dependencies.push(fence.clone()); Ok(()) }})?;
        Ok(())
    }})();
    (dependencies, firmware)
}}

fn pkt(inputs: Vec<Fence>, firmware: Vec<Fence>, producer_err: Option<i32>) -> Packet {{
    Packet {{
        inputs,
        firmware,
        producer_err,
        began: Cell::new(false),
        early_result: Cell::new(None),
        completion: Completion {{ failed_unpublished: Cell::new(None) }},
    }}
}}

fn main() {{
    assert!({str(fences_has_inline).lower()}, "Fences::inline_published must flow into EnqueueOrdering");

    let sched_pending = Fence {{ id: 10, status: 0 }};
    let sched_done = Fence {{ id: 11, status: 1 }};
    let comp_pending = Fence {{ id: 20, status: 0 }};

    // 1. Producer published inline: no scheduled dependency, firmware completion kept.
    let (deps, fw) = filter_producers(&[(sched_pending, comp_pending)], &[true]);
    assert!(deps.is_empty(), "inline-published producer must not add scheduled dependency");
    assert_eq!(fw.len(), 1);
    assert_eq!(fw[0].id, 20);

    // 2. Producer already scheduled via drm_sched: no scheduled dependency, firmware kept.
    let (deps, fw) = filter_producers(&[(sched_done, comp_pending)], &[false]);
    assert!(deps.is_empty(), "already-scheduled producer must not add scheduled dependency");
    assert_eq!(fw.len(), 1);

    // 3. Producer neither inline-published nor scheduled: MUST wait on scheduled fence.
    let (deps, fw) = filter_producers(&[(sched_pending, comp_pending)], &[false]);
    assert_eq!(deps.len(), 1, "unpublished producer must add scheduled dependency");
    assert_eq!(deps[0].id, 10);
    assert_eq!(fw.len(), 1);

    // 4. Early guards allow firmware-only cross-engine dependencies, block host waits.
    let b = Backend {{
        backlog: AtomicUsize::new(0),
        compute_backlog: AtomicUsize::new(0),
        traversal_ok: true,
    }};
    let p_fw_c = pkt(vec![], vec![comp_pending], None);
    assert!(b.run_compute_early_guard(&p_fw_c), "compute must publish inline with firmware-only deps");
    assert!(p_fw_c.completion.failed_unpublished.get().is_none());

    let p_fw_r = pkt(vec![], vec![comp_pending], None);
    assert!(b.run_render_early_guard(&p_fw_r), "render must publish inline with firmware-only deps");
    assert!(p_fw_r.completion.failed_unpublished.get().is_none());

    let p_host = pkt(vec![sched_pending], vec![comp_pending], None);
    assert!(!b.run_compute_early_guard(&p_host), "compute with host dep must stay on drm_sched");
    assert!(!b.run_render_early_guard(&p_host), "render with host dep must stay on drm_sched");

    // 5. Failed firmware producer fails consumer unpublished in both compute and render.
    let p_err_c = pkt(vec![], vec![comp_pending], Some(EIO));
    assert!(b.run_compute_early_guard(&p_err_c));
    assert_eq!(p_err_c.completion.failed_unpublished.get(), Some(EIO), "compute must catch failed producer");
    assert_eq!(p_err_c.early_result.get(), Some(Err(EIO)));

    let p_err_r = pkt(vec![], vec![comp_pending], Some(EIO));
    assert!(b.run_render_early_guard(&p_err_r));
    assert_eq!(p_err_r.completion.failed_unpublished.get(), Some(EIO), "render must catch failed producer");
    assert_eq!(p_err_r.early_result.get(), Some(Err(EIO)));

    println!("PASS g17 inline cross-engine publication");
}}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutation', choices=MUTATIONS)
    args = ap.parse_args()

    src = build_harness(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-inline-xeng-') as tmp:
        rs = os.path.join(tmp, 'harness.rs')
        bin_path = os.path.join(tmp, 'harness')
        Path(rs).write_text(src)
        c = subprocess.run(['rustc', '-O', rs, '-o', bin_path], capture_output=True, text=True)
        if c.returncode != 0:
            sys.stderr.write(c.stderr)
            return 2
        r = subprocess.run([bin_path], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode


if __name__ == '__main__':
    sys.exit(main())
