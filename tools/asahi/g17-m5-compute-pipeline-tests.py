#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for J714 pipelined compute parent dependency selection.

Extracts production `parent_dependency()`, `counted_forms()`, and `kick_tuple()`
from `drivers/gpu/drm/asahi/g17/fw/m5/compute.rs`, and the barrier/context
independence predicate from `drivers/gpu/drm/asahi/g17/queue/compute.rs`,
verifying:
1. On J714 (`completion_inputs == true`), independent same-context compute kicks
   with no compute barrier, no explicit dependencies, and no visible-completion
   flag take `COMPUTE_PIPELINED_DEPENDENCY` (2) and `auxiliary_control = 1`.
2. Any prior compute barrier (`wait_through[1]`), explicit dependency,
   `visible_completion` flag, cross-context active kick, or replay/quarantine
   keeps `COMPLETION_DEPENDENCY` (1) and `auxiliary_control = 0`.
3. G17D (`g17d_forms == true`) and J813 (`j813_forms == true`) are unchanged.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / 'drivers/gpu/drm/asahi/g17'


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
    fw_src = (G17 / 'fw/m5/compute.rs').read_text()
    q_src = (G17 / 'queue/compute.rs').read_text()

    parent_fn = item(fw_src, 'pub(crate) fn parent_dependency(').replace('pub(crate) ', '')
    counted_fn = item(fw_src, 'pub(crate) fn counted_forms(').replace('pub(crate) ', '')
    tuple_fn = item(fw_src, 'pub(crate) fn kick_tuple(').replace('pub(crate) ', '')

    pred_start = q_src.index('let parent_retired = self.active.len == 0')
    pred_end = q_src.index('let parent_class =', pred_start)
    pred_block = q_src[pred_start:pred_end]

    if mutation == 'serialize-independent':
        pred_block = pred_block.replace(
            'let same_context_independent = !parent_retired',
            'let same_context_independent = false && !parent_retired',
            1,
        )
    elif mutation == 'ignore-cdm-barrier':
        pred_block = pred_block.replace(
            '&& !crate::g17::job::Order::contains(',
            '&& true && !false && (true || !crate::g17::job::Order::contains(',
            1,
        ).replace(
            'active.packet.order.sequence,\n                    )',
            'active.packet.order.sequence,\n                    ))',
            1,
        )
    elif mutation == 'ignore-visible-completion':
        pred_block = pred_block.replace('&& !*visible_completion', '', 1)

    pred_host = pred_block.replace('crate::g17::job::', 'job_stub::')

    return f'''
const COMPUTE_PIPELINED_DEPENDENCY: u8 = 2;
const COMPLETION_DEPENDENCY: u8 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct KickTuple {{
    preempt_state_offset: u32,
    dispatch_base: u64,
    auxiliary_control: u64,
}}
const NORMAL_TUPLE: KickTuple =
    KickTuple {{ preempt_state_offset: 0x1b80, dispatch_base: 0x14c0_8a60, auxiliary_control: 1 }};
const QUALIFIED_TUPLE: KickTuple =
    KickTuple {{ preempt_state_offset: 0x1480, dispatch_base: 0x10c0_8860, auxiliary_control: 0 }};
const J813_TUPLE: KickTuple =
    KickTuple {{ preempt_state_offset: 0x1480, dispatch_base: 0x14c0_8a60, auxiliary_control: 0 }};

struct Board {{
    g17d: bool,
    completion_inputs: bool,
    j813: bool,
}}
impl Board {{
    fn g17d_forms(&self) -> bool {{ self.g17d }}
    fn completion_inputs(&self) -> bool {{ self.completion_inputs }}
    fn j813_forms(&self) -> bool {{ self.j813 }}
}}

{counted_fn}
{parent_fn}
{tuple_fn}

mod job_stub {{
    pub enum Engine {{ Compute }}
    impl Engine {{ pub fn index(self) -> usize {{ 1 }} }}
    pub struct Order {{
        pub sequence: u64,
        pub wait_through: [Option<u64>; 2],
    }}
    impl Order {{
        pub fn contains(prefix: Option<u64>, sequence: u64) -> bool {{
            sequence != 0 && prefix.is_some_and(|last| sequence <= last)
        }}
    }}
}}

struct Context {{ id: u32 }}
impl Context {{ fn id(&self) -> u32 {{ self.id }} }}
struct Packet {{ context: Context, order: job_stub::Order }}
struct Active {{ packet: Packet }}
struct Ring<T> {{ items: Vec<T>, len: usize }}
impl<T> Ring<T> {{
    fn new(items: Vec<T>) -> Self {{ let len = items.len(); Self {{ items, len }} }}
    fn iter(&self) -> std::slice::Iter<'_, T> {{ self.items.iter() }}
}}

struct MockQueue {{
    active: Ring<Active>,
    replays: Ring<u8>,
    quarantined: bool,
    awaiting_witness: bool,
}}

impl MockQueue {{
    fn eval(&self, board: &Board, packet: &Packet, dependencies: &[u8], visible_completion: &bool) -> (u8, u64) {{
        {pred_host}
        let class = parent_dependency(board, parent_retired || same_context_independent);
        (class, kick_tuple(board, class).auxiliary_control)
    }}
}}

fn pkt(ctx: u32, seq: u64, cdm_wait: Option<u64>) -> Packet {{
    Packet {{ context: Context {{ id: ctx }}, order: job_stub::Order {{ sequence: seq, wait_through: [None, cdm_wait] }} }}
}}

fn main() {{
    let j714 = Board {{ g17d: false, completion_inputs: true, j813: false }};
    let j775d = Board {{ g17d: true, completion_inputs: false, j813: false }};
    let j813 = Board {{ g17d: false, completion_inputs: false, j813: true }};

    let q_active = MockQueue {{
        active: Ring::new(vec![Active {{ packet: pkt(1, 5, None) }}]),
        replays: Ring::new(vec![]),
        quarantined: false,
        awaiting_witness: false,
    }};

    // 1. Independent same-context compute on J714 pipelines (class 2, aux 1).
    assert_eq!(q_active.eval(&j714, &pkt(1, 6, None), &[], &false), (COMPUTE_PIPELINED_DEPENDENCY, 1));

    // 2. Prior compute barrier serializes on J714 (class 1, aux 0).
    assert_eq!(q_active.eval(&j714, &pkt(1, 6, Some(5)), &[], &false), (COMPLETION_DEPENDENCY, 0));

    // 3. Explicit cross-queue dependency serializes on J714.
    assert_eq!(q_active.eval(&j714, &pkt(1, 6, None), &[1], &false), (COMPLETION_DEPENDENCY, 0));

    // 4. Visible completion flag serializes on J714.
    assert_eq!(q_active.eval(&j714, &pkt(1, 6, None), &[], &true), (COMPLETION_DEPENDENCY, 0));

    // 5. Different context on same channel serializes on J714.
    assert_eq!(q_active.eval(&j714, &pkt(2, 6, None), &[], &false), (COMPLETION_DEPENDENCY, 0));

    // 6. G17D (M5 Ultra) and J813 keep class 1 regardless.
    assert_eq!(q_active.eval(&j775d, &pkt(1, 6, None), &[], &false), (COMPLETION_DEPENDENCY, 0));
    assert_eq!(q_active.eval(&j813, &pkt(1, 6, None), &[], &false), (COMPLETION_DEPENDENCY, 0));

    println!("PASS g17-m5-compute-pipeline-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=['serialize-independent',
                                               'ignore-cdm-barrier',
                                               'ignore-visible-completion'])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-m5-pipe-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
