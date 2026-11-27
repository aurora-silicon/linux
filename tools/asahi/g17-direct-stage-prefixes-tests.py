#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for staged and compute-prefixed renders on the G17 direct render path.

Compiles production `own_dependencies()` (`queue/render.rs`) and inspects
`render_direct()` / `lend_render()` (`runtime/render/direct.rs`) to verify:
1. `render_direct()` admits both `stage_prefixes` and compute-prefixed renders.
2. `lend_render()` resolves both `tiling_wait_through[1]` and `wait_through[1]`
   across every compute queue owned by `owner` under the device mutex, returning
   `Ok(None)` on `EAGAIN` or non-zero recovery state, and `render_direct()`
   copies those resolved dependencies into `RenderDependencies`.
3. `own_dependencies()` populates `render`, `render_tiling` (using
   `tiling_wait_through[0]`), and `tiling` (using `wait_through[0]`) for
   staged renders, matching `Registry::render_dependencies()`.
4. Non-staged renders keep `render_tiling` empty and only collect `tiling`
   when `fragment_barriers` is set and `render_count != 0`.
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

    own_fn = item(render_src, 'pub(crate) fn own_dependencies(')
    lend_fn = item(direct_src, 'fn lend_render(')
    direct_fn = item(direct_src, 'pub(in crate::g17::runtime) fn render_direct(')

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

    guard_end = direct_fn.index('return None;\n        }')
    direct_guard = direct_fn[:guard_end]
    admits_staged_and_compute = (
        'stage_prefixes' not in direct_guard
        and 'wait_through[1]' not in direct_guard
        and 'tiling_wait_through[1]' not in direct_guard
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

struct RenderDependencies {{
    render: [u64; 4],
    render_count: usize,
    render_tiling: [u64; 4],
    render_tiling_count: usize,
    tiling: [u64; 4],
    tiling_count: usize,
}}

impl RenderDependencies {{
    const EMPTY: Self = Self {{
        render: [0; 4],
        render_count: 0,
        render_tiling: [0; 4],
        render_tiling_count: 0,
        tiling: [0; 4],
        tiling_count: 0,
    }};
}}

struct Pair;
impl Pair {{
    fn collect_dependencies(&self, _owner: u64, _status: u32, prefix: Option<u64>, out: &mut Frontier, _compute: bool) -> Result {{
        if let Some(p) = prefix {{
            out.0.push(100 + p);
        }}
        Ok(())
    }}
    fn collect_tiling_dependencies(&self, _owner: u64, _status: u32, prefix: Option<u64>, out: &mut Frontier) -> Result {{
        if let Some(p) = prefix {{
            out.0.push(200 + p);
        }}
        Ok(())
    }}
    {own_host}
}}

fn main() {{
    assert!({str(admits_staged_and_compute).lower()}, "render_direct must admit staged and compute-prefixed renders");
    assert!({str(resolves_compute_in_lend).lower()}, "lend_render must resolve all owner compute prefixes with EAGAIN fallback");

    let pair = Pair;

    // 1. Staged render with wait_through[0]=Some(5), tiling_wait_through[0]=Some(3).
    let staged = Arc::new(Packet {{
        command: Validated::Render {{ pass: Pass {{ stage_prefixes: true, fragment_barriers: false }} }},
        order: Order {{ wait_through: [Some(5), None], tiling_wait_through: [Some(3), None] }},
        completion: Completion,
    }});
    let deps = pair.own_dependencies(1, &staged).unwrap();
    assert_eq!(deps.render_count, 1);
    assert_eq!(deps.render[0], 105);
    assert_eq!(deps.render_tiling_count, 1, "staged render must populate render_tiling");
    assert_eq!(deps.render_tiling[0], 103);
    assert_eq!(deps.tiling_count, 1, "staged render must populate tiling prefix");
    assert_eq!(deps.tiling[0], 205);

    // 2. Non-staged render without fragment_barriers keeps render_tiling and tiling empty.
    let plain = Arc::new(Packet {{
        command: Validated::Render {{ pass: Pass {{ stage_prefixes: false, fragment_barriers: false }} }},
        order: Order {{ wait_through: [Some(5), None], tiling_wait_through: [Some(5), None] }},
        completion: Completion,
    }});
    let deps = pair.own_dependencies(1, &plain).unwrap();
    assert_eq!(deps.render_count, 1);
    assert_eq!(deps.render_tiling_count, 0);
    assert_eq!(deps.tiling_count, 0);

    println!("PASS g17 direct render stage_prefixes + compute prefixes");
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
