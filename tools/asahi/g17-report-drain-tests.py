#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host test for batched report-ring drains and fast-path render mask clearing.

Compiles the real `Rings::consume_report()` from `drivers/gpu/drm/asahi/g17/channel.rs`
on the host with a counting mock `InitData` and verifies:
1. Trace ring drains with `asahi_g17_report` disabled read only 1 word (the tag) per
   record instead of 18 words, and publish `consumer` once per batch instead of once per
   record.
2. Trace ring drains with `asahi_g17_report` enabled copy all 18 words per record and
   invoke `trace::report()`, still publishing `consumer` once per batch.
3. Statistics ring drains copy all 18 words, clear `statistics_cutoff` across ring wrap,
   and publish `consumer` once per batch (and zero times when empty).
4. An invalid tag mid-batch flushes `consumer` up to the invalid slot before returning
   `EIO` so preceding valid slots are returned to firmware.
5. Structural verification of `runtime_event_turn()` in `runtime/events.rs`: when
   `try_fast_observe_render()` succeeds (`render_clean`), stale render `masks` are
   cleared before the slow path while preserving `render_rescan` poll arming.
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
    channel_src = (G17 / 'channel.rs').read_text()
    events_src = (G17 / 'runtime/events.rs').read_text()

    consume_fn = item(channel_src, 'fn consume_report(')
    turn_fn = item(events_src, 'fn runtime_event_turn(')

    if mutation == 'per-record-store':
        consume_fn = consume_fn.replace(
            'consumer = (consumer + 1) & (capacity - 1);\n            drained = true;',
            'consumer = (consumer + 1) & (capacity - 1);\n'
            '            consumer_word.store(consumer, Ordering::Relaxed);\n'
            '            drained = true;',
            1,
        )
    elif mutation == 'always-read-full-trace':
        consume_fn = consume_fn.replace(
            'let read_full = !trace_record || super::trace::report_enabled();',
            'let read_full = true;',
            1,
        )
    elif mutation == 'drop-partial-error-flush':
        consume_fn = consume_fn.replace(
            'if drained {\n                        fence(Ordering::SeqCst);\n'
            '                        consumer_word.store(consumer, Ordering::Relaxed);\n'
            '                        fence(Ordering::SeqCst);\n'
            '                    }',
            '',
        )
    elif mutation == 'keep-stale-render-masks':
        turn_fn = turn_fn.replace(
            'let remaining_masks = if render_clean && !recover {\n'
            '                [0; 2]\n'
            '            } else {\n'
            '                masks\n'
            '            };',
            'let remaining_masks = masks;',
            1,
        )

    events_contract_ok = (
        'let remaining_masks = if render_clean && !recover {' in turn_fn
        and '(recover, remaining_masks, fast_done, render_changed, rescan)' in turn_fn
        and 'else if render_rescan && !firmware.recovery.pending()' in turn_fn
    )

    # Strip `super::` prefixes so consume_report compiles cleanly inside our mock module.
    consume_fn = consume_fn.replace('super::trace::', 'trace::')
    consume_fn = consume_fn.replace('super::reports::', 'reports::')
    consume_fn = consume_fn.replace('super::telemetry::', 'telemetry::')
    consume_fn = consume_fn.replace('super::now_ns()', 'now_ns()')

    return f'''
use std::cell::{{Cell, RefCell}};
use std::sync::atomic::{{fence, AtomicBool, AtomicU32, Ordering}};

type Result<T = ()> = std::result::Result<T, i32>;
const EIO: i32 = -5;
const EVENT_WORDS: usize = 18;
const EVENT_RECORD_SIZE: usize = 72;
const RX_PRODUCER_OFFSET: usize = 0x20;
const TRACE_SLOTS: u32 = 16;
const STATISTICS_SLOTS: u32 = 8;
const TRACE_STATE: usize = 0;
const TRACE_RING: usize = 0x100;
const STATS_STATE: usize = 0x1000;
const STATS_RING: usize = 0x2000;

fn now_ns() -> u64 {{ 123_456 }}

#[derive(Copy, Clone, Debug)]
enum Role {{ Primary = 0 }}

#[derive(Copy, Clone, Debug)]
enum ReportRing {{ Trace = 0, Statistics = 1 }}

impl ReportRing {{
    fn geometry(self, _init: &InitData) -> (usize, usize, u32, u64) {{
        match self {{
            Self::Trace => (TRACE_STATE, TRACE_RING, TRACE_SLOTS, 1u64 << 5),
            Self::Statistics => (STATS_STATE, STATS_RING, STATISTICS_SLOTS, 1u64 << 0x13),
        }}
    }}
}}

mod trace {{
    use super::*;
    pub static ENABLED: AtomicBool = AtomicBool::new(false);
    thread_local! {{ pub static CALLS: Cell<usize> = const {{ Cell::new(0) }}; }}
    pub fn report_enabled() -> bool {{ ENABLED.load(Ordering::Relaxed) }}
    pub fn report(_role: u8, _ring: u8, _words: &[u32; EVENT_WORDS]) {{
        CALLS.with(|c| c.set(c.get() + 1));
    }}
}}

mod reports {{
    use super::*;
    #[derive(Default)]
    pub struct Reports {{ pub seen: usize }}
    impl Reports {{
        pub fn record(&mut self, _: Role, _: [u32; EVENT_WORDS], _: u64, _: u64, _: bool) {{
            self.seen += 1;
        }}
    }}
}}

mod telemetry {{
    use super::*;
    #[derive(Default)]
    pub struct Telemetry {{ pub admitted: usize, pub busy_only: usize }}
    impl Telemetry {{
        pub fn record(&mut self, _: &[u32; EVENT_WORDS], _: u64, _: u64) {{
            self.admitted += 1;
        }}
        pub fn record_busy(&mut self, _: &[u32; EVENT_WORDS]) {{
            self.busy_only += 1;
        }}
    }}
}}

struct CountingAtomic {{
    val: AtomicU32,
    stores: Cell<usize>,
}}

impl CountingAtomic {{
    fn new(v: u32) -> Self {{ Self {{ val: AtomicU32::new(v), stores: Cell::new(0) }} }}
    fn load(&self, o: Ordering) -> u32 {{ self.val.load(o) }}
    fn store(&self, v: u32, o: Ordering) {{
        self.stores.set(self.stores.get() + 1);
        self.val.store(v, o);
    }}
}}

struct InitData {{
    trace_c: CountingAtomic,
    trace_p: CountingAtomic,
    stats_c: CountingAtomic,
    stats_p: CountingAtomic,
    trace_slots: RefCell<[[u32; EVENT_WORDS]; TRACE_SLOTS as usize]>,
    stats_slots: RefCell<[[u32; EVENT_WORDS]; STATISTICS_SLOTS as usize]>,
    words_read: Cell<usize>,
}}

impl InitData {{
    fn new() -> Self {{
        Self {{
            trace_c: CountingAtomic::new(0),
            trace_p: CountingAtomic::new(0),
            stats_c: CountingAtomic::new(0),
            stats_p: CountingAtomic::new(0),
            trace_slots: RefCell::new([[0; EVENT_WORDS]; TRACE_SLOTS as usize]),
            stats_slots: RefCell::new([[0; EVENT_WORDS]; STATISTICS_SLOTS as usize]),
            words_read: Cell::new(0),
        }}
    }}

    fn push_trace(&self, tag: u32) {{
        let p = self.trace_p.load(Ordering::Relaxed);
        let mut w = [0u32; EVENT_WORDS];
        w[0] = tag;
        self.trace_slots.borrow_mut()[p as usize] = w;
        self.trace_p.val.store((p + 1) & (TRACE_SLOTS - 1), Ordering::Relaxed);
    }}

    fn push_stats(&self, tag: u32) {{
        let p = self.stats_p.load(Ordering::Relaxed);
        let mut w = [0u32; EVENT_WORDS];
        w[0] = tag;
        self.stats_slots.borrow_mut()[p as usize] = w;
        self.stats_p.val.store((p + 1) & (STATISTICS_SLOTS - 1), Ordering::Relaxed);
    }}

    fn status_word(&self, _role: Role, offset: usize) -> Result<&CountingAtomic> {{
        match offset {{
            o if o == TRACE_STATE => Ok(&self.trace_c),
            o if o == TRACE_STATE + RX_PRODUCER_OFFSET => Ok(&self.trace_p),
            o if o == STATS_STATE => Ok(&self.stats_c),
            o if o == STATS_STATE + RX_PRODUCER_OFFSET => Ok(&self.stats_p),
            _ => Err(EIO),
        }}
    }}

    fn status_read_words<const N: usize>(&self, _role: Role, offset: usize) -> Result<[u32; N]> {{
        self.words_read.set(self.words_read.get() + N);
        let mut out = [0u32; N];
        if offset >= STATS_RING {{
            let idx = (offset - STATS_RING) / EVENT_RECORD_SIZE;
            out.copy_from_slice(&self.stats_slots.borrow()[idx][..N]);
        }} else {{
            let idx = (offset - TRACE_RING) / EVENT_RECORD_SIZE;
            out.copy_from_slice(&self.trace_slots.borrow()[idx][..N]);
        }}
        Ok(out)
    }}
}}

struct Harness;
impl Harness {{
    {consume_fn}
}}

fn main() {{
    assert!({str(events_contract_ok).lower()}, "runtime_event_turn must clear handled render masks while preserving rescan poll");

    // 1. Trace ring drain with report tracepoint disabled: 1 word/record, 1 store/batch.
    let init = InitData::new();
    let mut rep = reports::Reports::default();
    let mut tel = telemetry::Telemetry::default();
    let mut cutoff = None;
    for _ in 0..10 {{
        init.push_trace(5);
    }}
    Harness::consume_report(&init, Role::Primary, ReportRing::Trace, &mut rep, &mut tel, 0, true, true, &mut cutoff).unwrap();
    assert_eq!(init.trace_c.load(Ordering::Relaxed), 10);
    assert_eq!(init.trace_c.stores.get(), 1, "trace batch must update consumer once");
    assert_eq!(init.words_read.get(), 10, "disabled trace drain must read only 1 tag word per record");

    // Empty drain performs zero consumer stores and zero reads.
    Harness::consume_report(&init, Role::Primary, ReportRing::Trace, &mut rep, &mut tel, 0, true, true, &mut cutoff).unwrap();
    assert_eq!(init.trace_c.stores.get(), 1, "empty trace drain must not store consumer");

    // 2. Trace ring drain with report tracepoint enabled: full 18 words/record, 1 store/batch.
    trace::ENABLED.store(true, Ordering::Relaxed);
    init.words_read.set(0);
    for _ in 0..5 {{
        init.push_trace(5);
    }}
    Harness::consume_report(&init, Role::Primary, ReportRing::Trace, &mut rep, &mut tel, 0, true, true, &mut cutoff).unwrap();
    assert_eq!(init.trace_c.load(Ordering::Relaxed), 15);
    assert_eq!(init.trace_c.stores.get(), 2, "enabled trace batch must still update consumer once");
    assert_eq!(init.words_read.get(), 5 * EVENT_WORDS, "enabled trace drain must copy full records");
    assert_eq!(trace::CALLS.with(|c| c.get()), 5);
    trace::ENABLED.store(false, Ordering::Relaxed);

    // 3. Invalid tag mid-batch flushes consumer up to the failing slot before returning EIO.
    init.push_trace(5);
    init.push_trace(5);
    init.push_trace(99);
    let err = Harness::consume_report(&init, Role::Primary, ReportRing::Trace, &mut rep, &mut tel, 0, true, true, &mut cutoff);
    assert_eq!(err, Err(EIO));
    assert_eq!(init.trace_c.load(Ordering::Relaxed), 1, "partial valid prefix (wrapped 15->0->1) must be flushed before EIO");
    assert_eq!(init.trace_c.stores.get(), 3);

    // 4. Statistics ring drain: full 18 words, cutoff cleared, single store per batch.
    let sinit = InitData::new();
    let mut srep = reports::Reports::default();
    let mut stel = telemetry::Telemetry::default();
    let mut scutoff = Some(2u32);
    for _ in 0..6 {{
        sinit.push_stats(0x13);
    }}
    Harness::consume_report(&sinit, Role::Primary, ReportRing::Statistics, &mut srep, &mut stel, 1000, true, true, &mut scutoff).unwrap();
    assert_eq!(sinit.stats_c.load(Ordering::Relaxed), 6);
    assert_eq!(sinit.stats_c.stores.get(), 1, "statistics batch must update consumer once");
    assert_eq!(sinit.words_read.get(), 6 * EVENT_WORDS);
    assert_eq!(scutoff, None);
    assert_eq!(srep.seen, 6);
    assert_eq!(stel.busy_only, 2);
    assert_eq!(stel.admitted, 4);

    println!("PASS g17-report-drain-tests");
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=[
        'per-record-store',
        'always-read-full-trace',
        'drop-partial-error-flush',
        'keep-stale-render-masks',
    ])
    args = parser.parse_args()
    src = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-repdrain-') as td:
        rs = Path(td) / 'test.rs'
        exe = Path(td) / 'test'
        rs.write_text(src)
        subprocess.run([rustc, '-O', '-o', str(exe), str(rs)], check=True)
        res = subprocess.run([str(exe)])
        sys.exit(res.returncode)


if __name__ == '__main__':
    main()
