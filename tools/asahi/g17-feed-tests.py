#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for G17 feed refill/turnaround detector and uniform CPU floor.

Extracts the production constants, `Detector`, `impl Detector`, and
`Requests::target_floor()` from `drivers/gpu/drm/asahi/g17/feed.rs` and
verifies:
1. All G17 parts request 100% of policy max frequency (no SoC gate in mod.rs).
2. Existing GPU-bound (`2 ms / 40 us`, `8 ms / 1.7 ms`), refill-bound
   (`20 us / 60 us`), and paced/short-streak invariants still hold.
3. Medium depth-1 turnaround-exposed passes (`1.35 ms / 195 us` -> `78 us`)
   engage and hold the floor.
4. Staggered two-queue depth-1 passes (`2.28 ms`, 180 us stagger so GPU-wide
   gap is 0, per-queue gap 165 us -> 72 us) engage and hold without masking
   each queue's refill gap behind the sibling queue.
5. Pipelined two-queue streams (depth > 1 per queue, 0 us per-queue gap) stay
   GPU-bound and do not request a CPU floor; transitioning from boosted to
   zero-gap pipelined drops the floor cleanly.
"""
import argparse
import os
import re
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
    feed_src = (G17 / 'feed.rs').read_text()
    mod_src = (G17 / 'mod.rs').read_text()
    comp_src = (G17 / 'completion.rs').read_text()

    assert 'feed::Feed::new()' in mod_src, 'expected uniform Feed::new() without SoC gate'
    assert 'feed.note_pass(self.context_id,' in comp_src, 'expected per-context note_pass call'

    consts = []
    for name in (
        'MAX_REFILL_TICKS',
        'EWMA_WINDOW',
        'REFILL_BOUND_DIV',
        'STREAK_ON',
        'STREAK_MAX',
        'LINGER_NS',
        'TURNAROUND_MAX_PASS_MS',
        'TURNAROUND_BUSY_SUM_TICKS',
        'TURNAROUND_ENTER_DIV',
        'TURNAROUND_HOLD_DIV',
        'CONTEXT_TRACKS',
        'OVERLAP_CONCURRENT_DIV',
        'FLOOR_PERCENT',
    ):
        m = re.search(rf'^const {name}:[^;]+;', feed_src, re.MULTILINE)
        assert m, f'missing constant {name}'
        consts.append(m.group(0))

    det_struct = item(feed_src, 'struct Detector ')
    det_impl = item(feed_src, 'impl Detector ')
    target_floor = item(feed_src, 'fn target_floor(')

    code = '\n'.join(consts + [det_struct, det_impl, target_floor])

    if mutation == 'half-floor':
        assert 'const FLOOR_PERCENT: u64 = 100;' in code
        code = code.replace('const FLOOR_PERCENT: u64 = 100;', 'const FLOOR_PERCENT: u64 = 50;', 1)
    elif mutation == 'global-last-end':
        old = 'let ctx_prev = self.context_end[(context as usize) % CONTEXT_TRACKS]\n            .fetch_max(end, Ordering::AcqRel);'
        assert old in code
        code = code.replace(old, 'let ctx_prev = global_prev;', 1)
    elif mutation == 'decay-subsumed':
        old = 'if idle == 0 && busy == 0 {'
        assert old in code
        code = code.replace(old, 'if false {', 1)
    elif mutation == 'drop-turnaround':
        old = 'if refill_bound || turnaround_exposed {'
        assert old in code
        code = code.replace(old, 'if refill_bound {', 1)
    elif mutation == 'no-hold-hysteresis':
        old = 'const TURNAROUND_HOLD_DIV: u64 = TURNAROUND_ENTER_DIV * 3;'
        assert old in code
        code = code.replace(old, 'const TURNAROUND_HOLD_DIV: u64 = TURNAROUND_ENTER_DIV;', 1)
    elif mutation == 'drop-busy-cap':
        old = 'busy_sum <= TURNAROUND_BUSY_SUM_TICKS'
        assert old in code
        code = code.replace(old, 'true', 1)
    elif mutation == 'unconditional-ctx-prev':
        old = 'let prev = if start < global_prev && now < self.overlap_until.load(Ordering::Acquire) {\n            ctx_prev\n        } else {\n            global_prev\n        };'
        assert old in code
        code = code.replace(old, 'let prev = ctx_prev;', 1)

    return f'''
use std::sync::atomic::{{AtomicBool, AtomicU32, AtomicU64, Ordering}};

const CLOCK_HZ: u64 = 24_000_000;

mod bindings {{
    pub const FREQ_QOS_MIN_DEFAULT_VALUE: u32 = 0;
}}

{code}

fn run(detector: &Detector, passes: u64, busy: u64, gap: u64) -> u64 {{
    let mut start = 1000;
    let mut now = 1_000_000;
    for _ in 0..passes {{
        detector.note(0, start, start + busy, now);
        start += busy + gap;
        now += 1_000_000;
    }}
    now
}}

fn main() {{
    // 1. Uniform full frequency floor across all G17 parts.
    assert_eq!(target_floor(4_044_000, true), 4_044_000);
    assert_eq!(target_floor(4_044_000, false), 0);

    // 2. Existing GPU-bound cases remain bound and do not request a CPU floor.
    {{
        let detector = Detector::new();
        let now = run(&detector, 16, CLOCK_HZ / 500, CLOCK_HZ / 25_000);
        assert!(detector.bound(now));
        assert!(!detector.wanted(now));
        assert!(detector.bound(now + 15_000_000));
        assert!(!detector.bound(now + LINGER_NS));
    }}
    {{
        let detector = Detector::new();
        let busy = CLOCK_HZ / 125;
        let gap = CLOCK_HZ * 17 / 10_000;
        let step_ns = 9_700_000;
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..16 {{
            detector.note(0, start, start + busy, now);
            start += busy + gap;
            now += step_ns;
        }}
        let last = now - step_ns;
        assert!(detector.bound(last));
        assert!(!detector.wanted(last));
        assert!(detector.bound(last + 16_000_000));
        assert!(detector.bound(last + LINGER_NS - 1));
        assert!(!detector.bound(last + LINGER_NS));
    }}

    // 3. Classic host-refill-bound small passes still request a CPU floor.
    {{
        let detector = Detector::new();
        let now = run(&detector, 16, CLOCK_HZ / 50_000, CLOCK_HZ / 16_667);
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
    }}

    // 4. Paced or short runs remain neither.
    {{
        let detector = Detector::new();
        let now = run(&detector, 16, CLOCK_HZ / 500, MAX_REFILL_TICKS);
        assert!(!detector.bound(now) && !detector.wanted(now));
        let detector = Detector::new();
        let now = run(&detector, u64::from(STREAK_ON) - 1, CLOCK_HZ / 500, 0);
        assert!(!detector.bound(now) && !detector.wanted(now));
    }}

    // 5. Completely subsumed same-context completions must not decay EWMA sums.
    {{
        let detector = Detector::new();
        let busy = CLOCK_HZ / 1000;
        let gap = CLOCK_HZ / 10_000;
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..12 {{
            detector.note(5, start, start + busy, now);
            start += busy + gap;
            now += 1_100_000;
        }}
        let idle_before = detector.idle.load(Ordering::Acquire);
        let busy_before = detector.busy.load(Ordering::Acquire);
        detector.note(5, start - busy - gap + 50, start - gap - 50, now + 1000);
        assert_eq!(detector.idle.load(Ordering::Acquire), idle_before);
        assert_eq!(detector.busy.load(Ordering::Acquire), busy_before);
    }}

    // 6. Medium depth-1 turnaround-exposed passes engage and hold the floor.
    {{
        let detector = Detector::new();
        let busy = CLOCK_HZ * 1350 / 1_000_000;
        let gap_unboosted = CLOCK_HZ * 195 / 1_000_000;
        let gap_boosted = CLOCK_HZ * 78 / 1_000_000;
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..16 {{
            detector.note(1, start, start + busy, now);
            start += busy + gap_unboosted;
            now += 1_550_000;
        }}
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
        for _ in 0..32 {{
            detector.note(1, start, start + busy, now);
            start += busy + gap_boosted;
            now += 1_430_000;
        }}
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
    }}

    // 7. Staggered two-queue depth-1 passes engage and hold without masking.
    {{
        let detector = Detector::new();
        let busy = CLOCK_HZ * 2280 / 1_000_000;
        let gap_unboosted = CLOCK_HZ * 165 / 1_000_000;
        let gap_boosted = CLOCK_HZ * 72 / 1_000_000;
        let stagger = CLOCK_HZ * 180 / 1_000_000;
        let mut s0 = 1000;
        let mut s1 = s0 + stagger;
        let mut now = 1_000_000;
        for _ in 0..12 {{
            detector.note(3, s0, s0 + busy, now);
            detector.note(4, s1, s1 + busy, now + 180_000);
            s0 += busy + gap_unboosted;
            s1 += busy + gap_unboosted;
            now += 2_450_000;
        }}
        assert!(detector.wanted(now));
        for _ in 0..40 {{
            detector.note(3, s0, s0 + busy, now);
            detector.note(4, s1, s1 + busy, now + 180_000);
            s0 += busy + gap_boosted;
            s1 += busy + gap_boosted;
            now += 2_350_000;
        }}
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
    }}

    // 8. Pipelined two-queue stream (0 us per-queue gap) stays GPU-bound,
    //    and transitioning from boosted to pipelined drops the floor cleanly.
    {{
        let detector = Detector::new();
        let busy = CLOCK_HZ * 1150 / 1_000_000;
        let gap_unboosted = CLOCK_HZ * 180 / 1_000_000;
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..16 {{
            detector.note(2, start, start + busy, now);
            start += busy + gap_unboosted;
            now += 1_330_000;
        }}
        assert!(detector.wanted(now));
        for _ in 0..48 {{
            detector.note(2, start, start + busy, now);
            start += busy;
            now += 1_150_000;
        }}
        assert!(detector.bound(now));
        assert!(!detector.wanted(now));
    }}

    // 9. Alternating multi-queue back-to-back passes on a saturated GPU (and
    //    serial turn-taking with minor 10% tail drain overlap, plus concurrent
    //    falling back to serial) must remain GPU-bound and never raise the floor.
    {{
        for &(contexts, pass_us) in &[
            (1u32, 1000u64),
            (2, 1000),
            (2, 400),
            (3, 500),
            (2, 1900),
            (2, 2100),
        ] {{
            let detector = Detector::new();
            let pass = CLOCK_HZ * pass_us / 1_000_000;
            let (mut start, mut now) = (1000u64, 1_000_000u64);
            for _ in 0..32 {{
                for c in 0..contexts {{
                    detector.note(c + 1, start, start + pass, now);
                    start += pass;
                    now += pass_us * 1000;
                }}
            }}
            assert!(detector.bound(now), "expected bound for {{}}x{{}}us", contexts, pass_us);
            assert!(!detector.wanted(now), "expected !wanted for {{}}x{{}}us", contexts, pass_us);
        }}
        // Serial turn-taking with 10% hardware drain overlap stays GPU-bound.
        let detector = Detector::new();
        let pass = CLOCK_HZ * 1000 / 1_000_000;
        let step = CLOCK_HZ * 900 / 1_000_000;
        let (mut start, mut now) = (1000u64, 1_000_000u64);
        for _ in 0..32 {{
            for c in 0..2u32 {{
                detector.note(c + 1, start, start + pass, now);
                start += step;
                now += 900_000;
            }}
        }}
        assert!(detector.bound(now));
        assert!(!detector.wanted(now));
        // Concurrent staggered queues transitioning to serial back-to-back drop the floor.
        let detector = Detector::new();
        let busy = CLOCK_HZ * 2280 / 1_000_000;
        let gap = CLOCK_HZ * 165 / 1_000_000;
        let stagger = CLOCK_HZ * 180 / 1_000_000;
        let (mut s0, mut s1, mut now) = (1000u64, 1000u64 + stagger, 1_000_000u64);
        for _ in 0..12 {{
            detector.note(3, s0, s0 + busy, now);
            detector.note(4, s1, s1 + busy, now + 180_000);
            s0 += busy + gap;
            s1 += busy + gap;
            now += 2_450_000;
        }}
        assert!(detector.wanted(now));
        let mut start = s1 + busy;
        let serial_pass = CLOCK_HZ * 1000 / 1_000_000;
        for _ in 0..32 {{
            for c in [3u32, 4u32] {{
                detector.note(c, start, start + serial_pass, now);
                start += serial_pass;
                now += 1_000_000;
            }}
        }}
        assert!(detector.bound(now));
        assert!(!detector.wanted(now));
    }}

    println!("PASS g17-feed-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--mutation',
        choices=[
            'half-floor',
            'global-last-end',
            'decay-subsumed',
            'drop-turnaround',
            'no-hold-hysteresis',
            'drop-busy-cap',
            'unconditional-ctx-prev',
        ],
    )
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-feed-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
