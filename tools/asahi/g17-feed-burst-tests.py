#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test and mutation suite for G17 per-context sustained refill detection.

Extracts `Tracker`, `Detector`, and their constants directly from
`drivers/gpu/drm/asahi/g17/feed.rs`, verifies that `completion.rs` passes
`self.context_id` to both `feed.note_pass(...)` call sites, and exercises
refill-bound, GPU-bound, multi-client, out-of-order, and linger behaviors.
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

MUTATIONS = (
    'no-min-run',
    'single-context',
    'per-ctx-gap',
    'shared-linger',
    'short-bound-linger',
    'count-subsumed',
    'clip-overlap',
)


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def const_line(source, name):
    m = re.search(rf'^const\s+{re.escape(name)}\s*:[^;]+;', source, re.MULTILINE)
    assert m, f'missing const {name}'
    return m.group(0)


def check_completion_call_sites():
    comp = (G17 / 'completion.rs').read_text()
    calls = re.findall(r'feed\.note_pass\(([^)]+)\)', comp)
    assert len(calls) == 2, f'expected 2 feed.note_pass calls, found {calls}'
    for c in calls:
        assert c.lstrip().startswith('self.context_id,'), f'missing self.context_id in {c}'


def clock_hz_const():
    src = (ROOT / 'drivers/gpu/drm/asahi/hw/t8140.rs').read_text()
    m = re.search(
        r'^\s*pub\(crate\)\s+const\s+CLOCK_HZ\s*:\s*u64\s*=\s*([^;]+);',
        src,
        re.MULTILINE,
    )
    assert m, 'missing CLOCK_HZ in hw/t8140.rs'
    return f'const CLOCK_HZ: u64 = {m.group(1)};'


def build_harness(mutation=None):
    src = (G17 / 'feed.rs').read_text()
    consts = '\n'.join(
        const_line(src, name)
        for name in (
            'MAX_REFILL_PER_SEC',
            'STREAK_ON',
            'STREAK_MAX',
            'CONTEXT_SLOTS',
            'FEED_MIN_RUN_PER_SEC',
            'FEED_LINGER_NS',
            'BOUND_LINGER_NS',
        )
    )

    sample_enum = item(src, 'enum Sample ')
    ctx_run_struct = item(src, 'struct ContextRun ')
    ctx_run_impl = item(src, 'impl ContextRun ')
    tracker_struct = item(src, 'struct Tracker ')
    tracker_impl = item(src, 'impl Tracker ')
    detector_struct = item(src, 'struct Detector ')
    detector_impl = item(src, 'impl Detector ')

    if mutation == 'no-min-run':
        old = (
            'if slot.last_end.saturating_sub(slot.run_start) >= self.min_run_ticks {\n'
            '                Sample::RefillBound\n'
            '            } else {\n'
            '                Sample::None\n'
            '            }'
        )
        assert tracker_impl.count(old) == 1
        tracker_impl = tracker_impl.replace(old, 'Sample::RefillBound', 1)
    elif mutation == 'single-context':
        assert tracker_impl.count('slot.context == context') == 1
        assert tracker_impl.count('|| slot.context != context') == 1
        tracker_impl = tracker_impl.replace('slot.context == context', 'true', 1)
        tracker_impl = tracker_impl.replace('|| slot.context != context', '', 1)
    elif mutation == 'per-ctx-gap':
        old = 'let global_prev = self.global_last_end;'
        assert tracker_impl.count(old) == 1
        repl = (
            'let global_prev = self.slots.iter()'
            '.find(|s| s.used && s.context == context)'
            '.map_or(0, |s| s.last_end);'
        )
        tracker_impl = tracker_impl.replace(old, repl, 1)
    elif mutation == 'shared-linger':
        old = 'now.saturating_add(FEED_LINGER_NS)'
        assert detector_impl.count(old) == 1
        detector_impl = detector_impl.replace(old, 'now.saturating_add(BOUND_LINGER_NS)', 1)
    elif mutation == 'short-bound-linger':
        old = 'now.saturating_add(BOUND_LINGER_NS)'
        assert detector_impl.count(old) == 1
        detector_impl = detector_impl.replace(old, 'now.saturating_add(FEED_LINGER_NS)', 1)
    elif mutation == 'count-subsumed':
        old = '        } else {\n            return Sample::None;\n        };'
        assert tracker_impl.count(old) == 1
        tracker_impl = tracker_impl.replace(old, '        } else {\n            (0, 0)\n        };', 1)
    elif mutation == 'clip-overlap':
        old = (
            '        } else if end > global_prev {\n'
            '            // Overlapping parallel waves share no GPU idle; credit actual busy span.\n'
            '            (0, end - start)'
        )
        assert tracker_impl.count(old) == 1
        tracker_impl = tracker_impl.replace(
            old,
            '        } else if end > global_prev {\n            (0, end - global_prev)',
            1,
        )

    clock_hz = clock_hz_const()

    return f'''
use core::sync::atomic::{{AtomicBool, AtomicU64, Ordering}};

{clock_hz}
{consts}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
{sample_enum}
#[derive(Copy, Clone)]
{ctx_run_struct}
{ctx_run_impl}
{tracker_struct}
{tracker_impl}
{detector_struct}
{detector_impl}

fn us(u: u64) -> u64 {{
    CLOCK_HZ * u / 1_000_000
}}

fn ms(m: u64) -> u64 {{
    CLOCK_HZ * m / 1_000
}}

fn ticks_to_ns(t: u64) -> u64 {{
    t * 1_000_000_000 / CLOCK_HZ
}}

fn run_stream(
    tracker: &mut Tracker,
    detector: &Detector,
    ctx: u32,
    passes: u64,
    busy: u64,
    gap: u64,
    start_tick: u64,
    start_ns: u64,
) -> (u64, u64) {{
    let mut tick = start_tick;
    let mut now = start_ns;
    let mut last_now = start_ns;
    for _ in 0..passes {{
        let sample = tracker.note(ctx, tick, tick + busy);
        detector.record(sample, now);
        last_now = now;
        tick += busy + gap;
        now += ticks_to_ns(busy + gap);
    }}
    (tick, last_now)
}}

fn main() {{
    // 1. Short desktop bursts (60 Hz and 120 Hz, 25 passes, < 2 ms/frame) stay quiet.
    for &period_ms in &[17u64, 8u64] {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let mut t0 = ms(10);
        let mut n0 = 10_000_000u64;
        for _ in 0..10 {{
            let (_end_tick, last) = run_stream(&mut tracker, &detector, 18, 25, us(20), us(50), t0, n0);
            assert!(!detector.wanted(last), "short desktop burst must not boost CPU floor");
            assert!(!detector.bound(last), "short desktop burst must not mark GPU bound");
            t0 += ms(period_ms);
            n0 += period_ms * 1_000_000;
        }}
    }}

    // 2. Interleaved paced clients must not chain across contexts.
    {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let mut events: Vec<(u32, u64, u64)> = Vec::new();
        for frame in 0..6u64 {{
            let base = ms(10) + frame * ms(17);
            for c in 0..6u32 {{
                let c_start = base + u64::from(c) * us(1500);
                for p in 0..10u64 {{
                    let s = c_start + p * us(80);
                    events.push((10 + c, s, s + us(20)));
                }}
            }}
        }}
        events.sort_by_key(|e| (e.2, e.1));
        for &(ctx, s, e) in &events {{
            let sample = tracker.note(ctx, s, e);
            let now = ticks_to_ns(e);
            detector.record(sample, now);
            assert!(!detector.wanted(now), "interleaved paced clients chained on ctx {{ctx}}");
            assert!(!detector.bound(now), "interleaved paced clients falsely marked bound");
        }}
    }}

    // 3. Alternating saturated clients (0 GPU idle, per-ctx gap 1 ms < 2 ms) stay GPU-bound.
    {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let mut tick = ms(10);
        let mut last_now = 0u64;
        for i in 0..40u64 {{
            let ctx = if i % 2 == 0 {{ 1 }} else {{ 2 }};
            let s = tick;
            let e = s + us(1000);
            last_now = ticks_to_ns(e);
            let sample = tracker.note(ctx, s, e);
            detector.record(sample, last_now);
            assert!(!detector.wanted(last_now), "saturated GPU falsely triggered CPU floor");
            tick = e;
        }}
        assert!(detector.bound(last_now), "alternating saturated clients must mark GPU bound");
    }}

    // 4. Sustained refill-bound stream engages after >= 8 ms and lingers 8 ms (not 20 ms),
    //    even with an interleaved 120 Hz compositor context.
    {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let mut events: Vec<(u32, u64, u64)> = Vec::new();
        // Sustained depth-1 refill stream: 20 us busy + 60 us gap = 80 us period, 250 passes = 20 ms
        for p in 0..250u64 {{
            let s = ms(10) + p * us(80);
            events.push((100, s, s + us(20)));
        }}
        // Concurrent 120 Hz compositor passes cleanly interspersed in refill gaps/alongside
        for f in 0..3u64 {{
            let s = ms(10) + f * us(8333) + us(25);
            events.push((29, s, s + us(30)));
        }}
        events.sort_by_key(|e| (e.2, e.1));

        let mut engaged_at = None;
        let mut last_now = 0u64;
        for &(ctx, s, e) in &events {{
            let now = ticks_to_ns(e);
            let sample = tracker.note(ctx, s, e);
            detector.record(sample, now);
            last_now = now;
            if ctx == 100 && s - ms(10) < ms(7) {{
                assert!(!detector.wanted(now), "must not engage before FEED_MIN_RUN_PER_SEC");
            }}
            if engaged_at.is_none() && detector.wanted(now) {{
                engaged_at = Some(now - ticks_to_ns(ms(10)));
            }}
        }}
        let eng = engaged_at.expect("sustained refill stream did not engage wanted()");
        assert!(eng >= 8_000_000 && eng <= 10_000_000, "unexpected engage latency {{eng}}");
        assert!(detector.wanted(last_now));
        assert!(detector.wanted(last_now + FEED_LINGER_NS - 1));
        assert!(!detector.wanted(last_now + FEED_LINGER_NS), "refill linger exceeded FEED_LINGER_NS");
        assert!(!detector.bound(last_now));
    }}

    // 5. GPU-bound 60 Hz long passes keep 20 ms BOUND_LINGER_NS without raising wanted().
    {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let (_end_tick, last_now) = run_stream(
            &mut tracker, &detector, 77, 16, ms(8), us(1700), ms(10), 10_000_000
        );
        assert!(detector.bound(last_now));
        assert!(!detector.wanted(last_now));
        assert!(detector.bound(last_now + 16_000_000), "bound expired before 60 Hz frame");
        assert!(detector.bound(last_now + BOUND_LINGER_NS - 1));
        assert!(!detector.bound(last_now + BOUND_LINGER_NS));
    }}

    // 6. Subsumed parallel waves return Sample::None and do not advance streak/decay.
    {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        // Long wave executes [10ms, 15ms], followed by earlier-ended parallel waves of ctx 55
        assert_eq!(tracker.note(55, ms(10), ms(15)), Sample::None);
        for w in 0..16u64 {{
            let s = ms(10) + us(100) + w * us(50);
            assert_eq!(tracker.note(55, s, s + us(20)), Sample::None, "subsumed wave must return None");
        }}
        // Confirm streak stayed 0 (not >= STREAK_ON): next real wave extension must still return None.
        assert_eq!(tracker.note(55, ms(15) + us(100), ms(16)), Sample::None);

        // Also test slot eviction across > CONTEXT_SLOTS (16) contexts.
        let mut tracker2 = Tracker::new(CLOCK_HZ);
        for c in 1..=24u32 {{
            assert_eq!(tracker2.note(c, ms(10) + u64::from(c) * us(50), ms(10) + u64::from(c) * us(50) + us(10)), Sample::None);
        }}
        let detector2 = Detector::new();
        let (_t, last) = run_stream(&mut tracker2, &detector2, 24, 180, us(20), us(60), ms(20), 20_000_000);
        assert!(detector2.wanted(last));
    }}

    // 7. Overlapping parallel waves (depth-4 compute) credit actual busy span instead of tail.
    {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let s0 = ms(10);
        let d0 = us(2500);
        detector.record(tracker.note(18, s0, s0 + d0), ticks_to_ns(s0 + d0));
        let mut wave_end = s0 + d0;
        let mut last_now = 0u64;
        for _ in 0..6u64 {{
            let t = wave_end + us(1000);
            for &(off, dur) in &[(0u64, us(2500)), (us(50), us(2650)), (us(100), us(2800))] {{
                let s = t + off;
                let e = s + dur;
                let now = ticks_to_ns(e);
                let sample = tracker.note(18, s, e);
                detector.record(sample, now);
                assert!(!detector.wanted(now), "overlapping compute wave falsely boosted CPU floor");
                wave_end = e;
                last_now = now;
            }}
        }}
        assert!(detector.bound(last_now), "overlapping parallel waves must stay GPU-bound");
    }}

    // 8. Invalid/extreme timestamps, context == 0, monotonic deadlines, concurrent states, stopping.
    {{
        let mut tracker = Tracker::new(CLOCK_HZ);
        assert_eq!(tracker.note(1, 0, ms(1)), Sample::None);
        assert_eq!(tracker.note(1, ms(1), 0), Sample::None);
        assert_eq!(tracker.note(1, ms(1), ms(1)), Sample::None);
        assert_eq!(tracker.note(1, ms(2), ms(1)), Sample::None);

        let mut extreme = Tracker::new(CLOCK_HZ);
        assert_eq!(extreme.note(1, 1, u64::MAX - 100), Sample::None);
        assert_eq!(extreme.note(1, 2, u64::MAX - 50), Sample::None);

        let mut ctx0_tracker = Tracker::new(CLOCK_HZ);
        let ctx0_det = Detector::new();
        let (_t, last) = run_stream(&mut ctx0_tracker, &ctx0_det, 0, 180, us(20), us(60), ms(10), 10_000_000);
        assert!(ctx0_det.wanted(last), "context 0 must not collide with empty slot sentinel");

        let mut mixed_tracker = Tracker::new(CLOCK_HZ);
        let mixed_det = Detector::new();
        let (end2, bound_now) = run_stream(&mut mixed_tracker, &mixed_det, 2, 16, ms(2), us(40), ms(10), 10_000_000);
        assert!(mixed_det.bound(bound_now));
        let start1 = end2 + ms(1);
        let (_end1, mixed_now) = run_stream(
            &mut mixed_tracker,
            &mixed_det,
            1,
            180,
            us(20),
            us(60),
            start1,
            ticks_to_ns(start1),
        );
        assert!(mixed_det.wanted(mixed_now), "concurrent refill+bound must keep CPU boost wanted");
        assert!(mixed_det.bound(mixed_now), "concurrent refill+bound must keep GPU-bound wait hint");
        assert!(!(mixed_det.bound(mixed_now) && !mixed_det.wanted(mixed_now)));

        let detector = Detector::new();
        assert!(!detector.active.load(Ordering::Acquire));
        assert!(!detector.pending.load(Ordering::Acquire));
        detector.record(Sample::RefillBound, 100_000_000);
        assert!(detector.active.load(Ordering::Acquire));
        assert!(detector.pending.swap(false, Ordering::AcqRel));
        detector.record(Sample::RefillBound, 50_000_000);
        assert!(detector.wanted(100_000_000 + FEED_LINGER_NS - 1), "older RefillBound regressed until");

        detector.record(Sample::GpuBound, 200_000_000);
        detector.record(Sample::GpuBound, 120_000_000);
        assert!(detector.bound(200_000_000 + BOUND_LINGER_NS - 1), "older GpuBound regressed bound_until");

        detector.record(Sample::RefillBound, u64::MAX);
        detector.record(Sample::GpuBound, u64::MAX);
        assert!(detector.wanted(u64::MAX - 1));
        assert!(detector.bound(u64::MAX - 1));

        detector.stopping.store(true, Ordering::Release);
        assert!(!detector.wanted(u64::MAX - 1));
        assert!(!detector.bound(u64::MAX - 1));
    }}

    // 9. Zero counter_hz disables refill and GPU-bound detection (even on back-to-back/overlapping waves).
    {{
        let mut zero = Tracker::new(0);
        let zero_det = Detector::new();
        let (_t, last) = run_stream(&mut zero, &zero_det, 1, 180, us(20), us(60), ms(10), 10_000_000);
        assert!(!zero_det.wanted(last) && !zero_det.bound(last));
        let (_t, last) = run_stream(&mut zero, &zero_det, 2, 16, ms(2), us(40), ms(30), 30_000_000);
        assert!(!zero_det.wanted(last) && !zero_det.bound(last));
        assert_eq!(zero.note(3, ms(10), ms(15)), Sample::None);
        assert_eq!(zero.note(3, ms(11), ms(16)), Sample::None);
    }}

    println!("PASS g17-feed-burst-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=MUTATIONS)
    args = parser.parse_args()
    check_completion_call_sites()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC') or os.environ.get('RUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-feed-burst-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run(
            [rustc, '--edition=2021', '-O', '-C', 'debug-assertions=on', '-o', str(exe), str(rs)],
            check=True,
        )
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
