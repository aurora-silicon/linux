#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for staged and compute-prefixed renders on the G17 direct render path.

Compiles production `Pair::own_dependencies()` (`queue/render.rs`),
`Registry::has_other_pair_dependencies()` (`runtime/render/dependencies.rs`),
and `Registry::render_dependencies()` (`runtime/render/submit.rs`), and
inspects `render_direct()` / `lend_render()` (`runtime/render/direct.rs`) to
verify:
1. `render_direct()` admits both `stage_prefixes` and compute-prefixed renders.
2. `lend_render()` checks `has_other_pair_dependencies()` before sampling host
   growth, resolves both `tiling_wait_through[1]` and `wait_through[1]` across
   every compute queue owned by `owner` under the device mutex (returning
   `Ok(None)` on `EAGAIN` or non-zero recovery state), and `render_direct()`
   copies those resolved dependencies into `RenderDependencies`.
3. Direct-vs-mutex parity: when no sibling pair holds prefix-covered work,
   `Pair::own_dependencies()` produces identical `(render, render_tiling,
   tiling)` to `Registry::render_dependencies()` for both staged and plain
   renders.
4. Sibling-pair fallback: when another render pair of the same owner (either a
   live resident pair or a borrowed pair via `borrowed_dependencies`) holds
   work covered by `wait_through[0]` OR `tiling_wait_through[0]` (including a
   live or borrowed sibling whose dependency belongs to the tiling prefix),
   `has_other_pair_dependencies()` returns `true` so `lend_render()` falls back
   to `Registry::render_dependencies()`, which captures the sibling's
   dependencies that `Pair::own_dependencies()` alone cannot see.
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
    'reject-staged',
    'skip-render-tiling',
    'skip-tiling-prefix',
    'skip-compute-resolve',
    'skip-eagain-fallback',
    'skip-other-pair-guard',
    'skip-borrowed-sibling',
    'skip-tiling-sibling',
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
    render_src = (G17 / 'queue/render.rs').read_text()
    direct_src = (G17 / 'runtime/render/direct.rs').read_text()
    deps_src = (G17 / 'runtime/render/dependencies.rs').read_text()
    submit_src = (G17 / 'runtime/render/submit.rs').read_text()

    own_fn = item(render_src, 'pub(crate) fn own_dependencies(')
    lend_fn = item(direct_src, 'fn lend_render(')
    direct_fn = item(direct_src, 'pub(in crate::g17::runtime) fn render_direct(')
    other_fn = item(deps_src, 'pub(in crate::g17::runtime) fn has_other_pair_dependencies(')
    reg_deps_fn = item(submit_src, 'fn render_dependencies(')

    if mutation == 'reject-staged':
        old = '|| self.admission_open().is_err()'
        assert old in direct_fn
        direct_fn = direct_fn.replace(old, '|| pass.stage_prefixes\n            ' + old, 1)
    elif mutation == 'skip-render-tiling':
        old = 'if staged {'
        assert old in own_fn
        own_fn = own_fn.replace(old, 'if false && staged {', 1)
    elif mutation == 'skip-tiling-prefix':
        old = 'if staged\n            ||'
        assert old in own_fn
        own_fn = own_fn.replace(old, 'if false\n            ||', 1)
    elif mutation == 'skip-compute-resolve':
        old = 'dependencies.compute_count = loan.compute_count;'
        assert old in direct_fn
        direct_fn = direct_fn.replace(old, 'dependencies.compute_count = 0;', 1)
    elif mutation == 'skip-eagain-fallback':
        old = 'Err(error) if error == EAGAIN => return Ok(None),'
        assert old in lend_fn
        lend_fn = lend_fn.replace(old, 'Err(error) if error == EAGAIN => return Err(error),', 1)
    elif mutation == 'skip-other-pair-guard':
        old = 'if self.queues.has_other_pair_dependencies(slot, owner, packet)? {'
        assert old in lend_fn
        lend_fn = lend_fn.replace(old, 'if false {', 1)
    elif mutation == 'skip-borrowed-sibling':
        old = 'else if let Some(dependencies) = entry.borrowed_dependencies.as_ref() {'
        assert old in other_fn
        other_fn = other_fn.replace(old, 'else if false { let _ = entry.borrowed_dependencies.as_ref(); if let Some(dependencies) = None::<&BorrowedDeps> {', 1)
        other_fn = other_fn.replace('                if !frontier.as_slice().is_empty() {', '                }\n                if !frontier.as_slice().is_empty() {', 1)
    elif mutation == 'skip-tiling-sibling':
        old = 'packet.order.tiling_wait_through[0],'
        assert old in other_fn
        other_fn = other_fn.replace(old, 'None,', 1)

    guard_end = direct_fn.index('return None;\n        }')
    direct_guard = direct_fn[:guard_end]
    admits_staged_and_compute = (
        'stage_prefixes' not in direct_guard
        and 'wait_through[1]' not in direct_guard
        and 'tiling_wait_through[1]' not in direct_guard
    )
    guards_other_pairs_before_growth = (
        'if self.queues.has_other_pair_dependencies(slot, owner, packet)? {' in lend_fn
        and lend_fn.index('has_other_pair_dependencies(slot, owner, packet)')
        < lend_fn.index('host_growth_target(packet)')
    )
    resolves_compute_in_lend = (
        'packet.order.tiling_wait_through[1]' in lend_fn
        and 'packet.order.wait_through[1]' in lend_fn
        and 'self\n                .queues\n                .compute' in lend_fn
        and lend_fn.count('Err(error) if error == EAGAIN => return Ok(None),') == 2
        and 'dependencies.compute_count = loan.compute_count;' in direct_fn
        and 'dependencies.fragment_compute_count = loan.fragment_compute_count;' in direct_fn
    )

    own_host = (
        own_fn
        .replace('pub(crate) ', '')
        .replace('crate::g17::dependency::Frontier', 'Frontier')
    )
    other_host = other_fn.replace('pub(in crate::g17::runtime) ', '')
    reg_deps_host = (
        reg_deps_fn
        .replace('crate::g17::queue::render::RenderDependencies', 'RenderDependencies')
        .replace('crate::g17::command::Validated', 'Validated')
    )

    return f'''
use std::sync::Arc;

const EOVERFLOW: i32 = -75;
type Result<T = ()> = std::result::Result<T, i32>;

struct Pass {{
    stage_prefixes: bool,
    fragment_barriers: bool,
}}

enum Validated {{
    Render {{ pass: Pass }},
}}

struct Order {{
    wait_through: [Option<u64>; 2],
    tiling_wait_through: [Option<u64>; 2],
}}

struct Completion;
impl Completion {{
    fn status(&self) -> u32 {{ 0 }}
}}

struct Packet {{
    command: Validated,
    order: Order,
    completion: Completion,
}}

struct Frontier(Vec<u64>);
impl Frontier {{
    fn new() -> Self {{ Self(Vec::new()) }}
    fn as_slice(&self) -> &[u64] {{ &self.0 }}
}}

#[derive(Debug, PartialEq, Eq)]
struct RenderDependencies {{
    render: [u64; 4],
    render_count: usize,
    render_tiling: [u64; 4],
    render_tiling_count: usize,
    tiling: [u64; 4],
    tiling_count: usize,
    compute: [u64; 3],
    compute_count: usize,
    fragment_compute: [u64; 3],
    fragment_compute_count: usize,
}}

impl RenderDependencies {{
    const EMPTY: Self = Self {{
        render: [0; 4],
        render_count: 0,
        render_tiling: [0; 4],
        render_tiling_count: 0,
        tiling: [0; 4],
        tiling_count: 0,
        compute: [0; 3],
        compute_count: 0,
        fragment_compute: [0; 3],
        fragment_compute_count: 0,
    }};
}}

#[derive(Clone)]
struct Pair {{
    owner: u64,
    seqs: Vec<u64>,
}}

impl Pair {{
    fn collect_dependencies(&self, owner: u64, _status: u32, prefix: Option<u64>, out: &mut Frontier, _compute: bool) -> Result {{
        if self.owner != owner {{ return Ok(()); }}
        if let Some(p) = prefix {{
            for &s in &self.seqs {{
                if s <= p {{
                    out.0.push(100 + s);
                }}
            }}
        }}
        Ok(())
    }}
    fn collect_tiling_dependencies(&self, owner: u64, _status: u32, prefix: Option<u64>, out: &mut Frontier) -> Result {{
        if self.owner != owner {{ return Ok(()); }}
        if let Some(p) = prefix {{
            for &s in &self.seqs {{
                if s <= p {{
                    out.0.push(200 + s);
                }}
            }}
        }}
        Ok(())
    }}
    {own_host}
}}

#[derive(Clone)]
struct BorrowedDeps {{
    pair: Pair,
}}

impl BorrowedDeps {{
    fn collect(&self, owner: u64, status: u32, prefix: Option<u64>, out: &mut Frontier, compute: bool) -> Result {{
        self.pair.collect_dependencies(owner, status, prefix, out, compute)
    }}
    fn collect_tiling(&self, owner: u64, status: u32, prefix: Option<u64>, out: &mut Frontier) -> Result {{
        self.pair.collect_tiling_dependencies(owner, status, prefix, out)
    }}
}}

struct Reservation {{
    owner: u64,
}}

struct RenderEntry {{
    reservation: Reservation,
    pair: Option<Pair>,
    borrowed_dependencies: Option<BorrowedDeps>,
}}

struct RenderState {{
    entries: Vec<Option<RenderEntry>>,
}}

struct ComputeQueue;
impl ComputeQueue {{
    fn owner(&self) -> Option<u64> {{ None }}
    fn dependency(&self, _owner: u64, _status: u32, _prefix: Option<u64>, _state: u32) -> Result<Option<u64>> {{
        Ok(None)
    }}
}}

struct ComputeEntry {{
    queue: Option<Box<ComputeQueue>>,
}}

struct Registry {{
    render: RenderState,
    compute: Vec<Option<ComputeEntry>>,
}}

impl Registry {{
    fn render_prefix(&self, owner: u64, packet: &Arc<Packet>) -> Result<Frontier> {{
        self.render_prefix_at(owner, packet, packet.order.wait_through[0])
    }}
    fn render_prefix_at(&self, owner: u64, packet: &Arc<Packet>, prefix: Option<u64>) -> Result<Frontier> {{
        let mut frontier = Frontier::new();
        for entry in self.render.entries.iter().flatten() {{
            if let Some(pair) = entry.pair.as_ref() {{
                pair.collect_dependencies(owner, packet.completion.status(), prefix, &mut frontier, false)?;
            }} else if let Some(deps) = entry.borrowed_dependencies.as_ref() {{
                deps.collect(owner, packet.completion.status(), prefix, &mut frontier, false)?;
            }}
        }}
        Ok(frontier)
    }}
    fn tiling_prefix(&self, owner: u64, packet: &Arc<Packet>) -> Result<Frontier> {{
        let mut frontier = Frontier::new();
        for entry in self.render.entries.iter().flatten() {{
            if let Some(pair) = entry.pair.as_ref() {{
                pair.collect_tiling_dependencies(owner, packet.completion.status(), packet.order.wait_through[0], &mut frontier)?;
            }} else if let Some(deps) = entry.borrowed_dependencies.as_ref() {{
                deps.collect_tiling(owner, packet.completion.status(), packet.order.wait_through[0], &mut frontier)?;
            }}
        }}
        Ok(frontier)
    }}
    {other_host}
    {reg_deps_host}
}}

fn staged_pkt(whole: Option<u64>, tiling: Option<u64>) -> Arc<Packet> {{
    Arc::new(Packet {{
        command: Validated::Render {{ pass: Pass {{ stage_prefixes: true, fragment_barriers: false }} }},
        order: Order {{ wait_through: [whole, None], tiling_wait_through: [tiling, None] }},
        completion: Completion,
    }})
}}

fn main() {{
    assert!({str(admits_staged_and_compute).lower()}, "render_direct must admit staged and compute-prefixed renders");
    assert!({str(guards_other_pairs_before_growth).lower()}, "lend_render must guard cross-pair dependencies before host_growth_target");
    assert!({str(resolves_compute_in_lend).lower()}, "lend_render must resolve all owner compute prefixes with EAGAIN fallback");

    // 1. Single-pair staged render: direct own_dependencies() == mutex Registry::render_dependencies().
    let slot1_pair = Pair {{ owner: 1, seqs: vec![3, 5] }};
    let reg_single = Registry {{
        render: RenderState {{
            entries: vec![
                None,
                Some(RenderEntry {{ reservation: Reservation {{ owner: 1 }}, pair: Some(slot1_pair.clone()), borrowed_dependencies: None }}),
            ],
        }},
        compute: vec![],
    }};
    let staged = staged_pkt(Some(5), Some(3));
    assert!(!reg_single.has_other_pair_dependencies(1, 1, &staged).unwrap());
    let direct_deps = slot1_pair.own_dependencies(1, &staged).unwrap();
    let mutex_deps = reg_single.render_dependencies(1, &staged, 0).unwrap();
    assert_eq!(direct_deps, mutex_deps, "direct and mutex staged dependencies must match on one pair");
    assert_eq!(&direct_deps.render[..direct_deps.render_count], &[103, 105]);
    assert_eq!(&direct_deps.render_tiling[..direct_deps.render_tiling_count], &[103]);
    assert_eq!(&direct_deps.tiling[..direct_deps.tiling_count], &[203, 205]);

    // 2. Non-staged render without fragment_barriers keeps render_tiling and tiling empty.
    let plain = Arc::new(Packet {{
        command: Validated::Render {{ pass: Pass {{ stage_prefixes: false, fragment_barriers: false }} }},
        order: Order {{ wait_through: [Some(5), None], tiling_wait_through: [Some(5), None] }},
        completion: Completion,
    }});
    let plain_direct = slot1_pair.own_dependencies(1, &plain).unwrap();
    let plain_mutex = reg_single.render_dependencies(1, &plain, 0).unwrap();
    assert_eq!(plain_direct, plain_mutex);
    assert_eq!(plain_direct.render_tiling_count, 0);
    assert_eq!(plain_direct.tiling_count, 0);

    // 3. Live resident sibling pair (slot 2) holds seq 3 belonging to tiling_wait_through[0]:
    //    direct own_dependencies(slot 1) misses seq 3, while mutex sees it — guard MUST fall back!
    let slot1_only5 = Pair {{ owner: 1, seqs: vec![5] }};
    let sib_seq3 = Pair {{ owner: 1, seqs: vec![3] }};
    let reg_live_sib = Registry {{
        render: RenderState {{
            entries: vec![
                None,
                Some(RenderEntry {{ reservation: Reservation {{ owner: 1 }}, pair: Some(slot1_only5.clone()), borrowed_dependencies: None }}),
                Some(RenderEntry {{ reservation: Reservation {{ owner: 1 }}, pair: Some(sib_seq3.clone()), borrowed_dependencies: None }}),
            ],
        }},
        compute: vec![],
    }};
    let direct_miss = slot1_only5.own_dependencies(1, &staged).unwrap();
    let mutex_full = reg_live_sib.render_dependencies(1, &staged, 0).unwrap();
    assert_ne!(direct_miss, mutex_full, "direct alone cannot see live sibling tiling prefix");
    assert_eq!(&mutex_full.render_tiling[..mutex_full.render_tiling_count], &[103]);
    assert!(reg_live_sib.has_other_pair_dependencies(1, 1, &staged).unwrap(),
            "live sibling holding tiling prefix must force fallback to mutex path");

    // 4. Tiling-only prefix scope (wait_through[0]=None, tiling_wait_through[0]=Some(3)) on live sibling:
    let tiling_only = staged_pkt(None, Some(3));
    assert!(reg_live_sib.has_other_pair_dependencies(1, 1, &tiling_only).unwrap(),
            "tiling_wait_through[0] alone must trigger sibling fallback");

    // 5. Borrowed sibling pair (slot 2 lent out, pair=None, borrowed_dependencies=Some) holding seq 3:
    let reg_borrowed_sib = Registry {{
        render: RenderState {{
            entries: vec![
                None,
                Some(RenderEntry {{ reservation: Reservation {{ owner: 1 }}, pair: Some(slot1_only5.clone()), borrowed_dependencies: None }}),
                Some(RenderEntry {{ reservation: Reservation {{ owner: 1 }}, pair: None, borrowed_dependencies: Some(BorrowedDeps {{ pair: sib_seq3 }}) }}),
            ],
        }},
        compute: vec![],
    }};
    let mutex_borrowed = reg_borrowed_sib.render_dependencies(1, &staged, 0).unwrap();
    assert_eq!(&mutex_borrowed.render_tiling[..mutex_borrowed.render_tiling_count], &[103]);
    assert!(reg_borrowed_sib.has_other_pair_dependencies(1, 1, &staged).unwrap(),
            "borrowed sibling holding tiling prefix must force fallback to mutex path");

    // 6. Sibling pair holding only newer work (seq 10 > wait_through[0]=5): direct allowed and matches mutex.
    let sib_newer = Pair {{ owner: 1, seqs: vec![10] }};
    let reg_newer_sib = Registry {{
        render: RenderState {{
            entries: vec![
                None,
                Some(RenderEntry {{ reservation: Reservation {{ owner: 1 }}, pair: Some(slot1_pair.clone()), borrowed_dependencies: None }}),
                Some(RenderEntry {{ reservation: Reservation {{ owner: 1 }}, pair: Some(sib_newer), borrowed_dependencies: None }}),
            ],
        }},
        compute: vec![],
    }};
    assert!(!reg_newer_sib.has_other_pair_dependencies(1, 1, &staged).unwrap(),
            "sibling with only newer sequences must not block direct render");
    assert_eq!(slot1_pair.own_dependencies(1, &staged).unwrap(),
               reg_newer_sib.render_dependencies(1, &staged, 0).unwrap());

    println!("PASS g17 direct render stage_prefixes + compute prefixes + sibling guard");
}}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutation', choices=MUTATIONS)
    args = ap.parse_args()

    src = build_harness(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-direct-staged-') as tmp:
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
