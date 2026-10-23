#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the G15 receive-ring drain's actual bodies, before and after the stats ring is noted.

The harness extracts Channels::drain_one(), note_stats(), peek_stats() and drain_rings() from
drivers/gpu/drm/asahi/g15/channel.rs, and power/pstate.rs whole. It runs them against a
model of the firmware objects that logs every load, store and barrier in order. The reference is
drain_rings() as of 9f09c85f8d3b, before the stats ring was noted. It is embedded below, because
a shallow checkout lacks that commit, and checked against git when the commit is present.

What it shows, for the stats ring and the event, log and trace rings drained beside it:
- every (consumer, producer) pair, both inside and outside the 256-slot ring, gives the same
  stores the firmware sees (offsets, values, order, write barriers) and the same final state;
- the producer is loaded before the consumer, as before. Inside the ring the consumer is loaded
  again before the store; outside the ring both are loaded again by the earlier drain_one();
- with a producer that moves between loads, the consumer store is the earlier drain's for the
  producer loaded by the same code;
- the noted records: the pending slots in ring order (wrap, empty, full), the tag at +0 and the
  word at +0x20, read after a read barrier, the newest power-state record winning;
- a full barrier after the last record load and before the consumer store that frees the slots;
- peek_stats() stores nothing.

Not shown: kernel, firmware or GPU execution, nor timing. The noting drain loads up to 510 more
record words, so it takes longer than the earlier drain.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
G15 = "drivers/gpu/drm/asahi/g15"
CHANNEL = f"{G15}/channel.rs"
PSTATE = f"{G15}/power/pstate.rs"
HW = f"{G15}/hw.rs"
FW_CHANNELS = f"{G15}/fw/channels.rs"
OLD_REF = "9f09c85f8d3bae8512712686f5a91067fc4eb3de"

# drain_rings() at OLD_REF (the stats ring consumed unread), verbatim.
OLD_DRAIN_RINGS = '''    pub(crate) fn drain_rings(&self, dev: &kernel::device::Device, events: &mut Events) -> Result {
        events.drain(dev, &self.event)?;
        for i in 0..rings::LOG_RINGS {
            Self::drain_one(&self.log, i * 0x30)?;
        }
        Self::drain_one(&self.trace, 0)?;
        Self::drain_one(&self.stats, 0)
    }'''

MUTATIONS = {
    # Store the consumer even when it already equals the producer.
    'unconditional-consume': (CHANNEL, 'if self.stats.read()? != producer {', 'if true {'),
    # Leave the stats ring unconsumed when an index is out of the ring.
    'skip-fallback': (CHANNEL, '        Self::drain_one(&self.stats, 0)\n    }', '        Ok(())\n    }'),
    # Store a producer loaded after the records were noted.
    'resample-producer': (CHANNEL, 'self.stats.state.w32(STATE_READ, producer)?',
                          'self.stats.state.w32(STATE_READ, self.stats.state.r32(STATE_WRITE)?)?'),
    # Store the consumer without the full barrier after the record loads.
    'no-release-barrier': (CHANNEL, '                mb();\n                self.stats.state.w32(STATE_READ, producer)?;',
                           '                self.stats.state.w32(STATE_READ, producer)?;'),
    # Read the records without the read barrier.
    'no-read-barrier': (CHANNEL, '        rmb();\n        for slot in slots {', '        for slot in slots {'),
    # Walk the slots without the wrap.
    'no-wrap': (PSTATE, '(read + i) % STATS_SLOTS', 'read + i'),
    # Keep the oldest power-state record instead of the newest.
    'oldest-power-state': (PSTATE, 'if tag == TAG_POWER_STATE {', 'if tag == TAG_POWER_STATE && self.last.is_none() {'),
}


def item(text, start):
    a = text.index(start)
    b = text.index("{", a) + 1
    depth = 1
    while depth:
        depth += (text[b] == "{") - (text[b] == "}")
        b += 1
    return text[a:b]


def const(text, name):
    m = re.search(rf'pub\(crate\) const {name}: (\w+) = ([^;]+);', text)
    if not m:
        raise SystemExit(f'{name} not found')
    return f'pub const {name}: {m.group(1)} = {m.group(2)};'


PREFIX = r'''
#![allow(dead_code, unused_imports)]
use std::{cell::RefCell, sync::Arc};
type Result<T = ()> = std::result::Result<T, i32>;
const EINVAL: i32 = -22;
mod kernel { pub mod device { pub struct Device; } }
mod power {
    #[path = "PSTATE_PATH"]
    pub mod pstate;
}
use power::pstate::{self, StatsSeen};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Ev { Load(u8, usize, u32), Store(u8, usize, u32), Rmb, Mb, Wmb, Events }
thread_local! { static LOG: RefCell<Vec<Ev>> = RefCell::new(Vec::new()); }
fn log(e: Ev) { LOG.with(|l| l.borrow_mut().push(e)); }
fn take_log() -> Vec<Ev> { LOG.with(|l| std::mem::take(&mut *l.borrow_mut())) }
fn wmb() { log(Ev::Wmb); }
fn rmb() { log(Ev::Rmb); }
fn mb() { log(Ev::Mb); }

/// A firmware object: little-endian bytes, bounds-checked like FwObject's ptr(). A word the
/// firmware writes (a producer) is given as successive values, one per load; the last repeats.
struct FwObject { id: u8, mem: RefCell<Vec<u8>>, fw: RefCell<Vec<(usize, Vec<u32>)>> }
impl FwObject {
    fn new(id: u8, size: usize) -> Arc<Self> {
        Arc::new(Self { id, mem: RefCell::new(vec![0; size]), fw: RefCell::new(Vec::new()) })
    }
    fn set(&self, at: usize, v: u32) { self.mem.borrow_mut()[at..at + 4].copy_from_slice(&v.to_le_bytes()); }
    fn firmware(&self, at: usize, values: &[u32]) { self.fw.borrow_mut().push((at, values.to_vec())); }
    fn r32(&self, offset: usize) -> Result<u32> {
        if offset.checked_add(4).map_or(true, |e| e > self.mem.borrow().len()) { return Err(EINVAL); }
        let mut fw = self.fw.borrow_mut();
        if let Some((_, vals)) = fw.iter_mut().find(|(at, _)| *at == offset) {
            let v = if vals.len() > 1 { vals.remove(0) } else { vals[0] };
            drop(fw);
            self.set(offset, v);
        } else { drop(fw); }
        let m = self.mem.borrow();
        let v = u32::from_le_bytes(m[offset..offset + 4].try_into().unwrap());
        log(Ev::Load(self.id, offset, v));
        Ok(v)
    }
    fn w32(&self, offset: usize, value: u32) -> Result {
        if offset.checked_add(4).map_or(true, |e| e > self.mem.borrow().len()) { return Err(EINVAL); }
        self.set(offset, value);
        log(Ev::Store(self.id, offset, value));
        Ok(())
    }
}

pub(crate) struct Ring { pub(crate) state: Arc<FwObject>, pub(crate) ring: Arc<FwObject> }
impl Ring {
RING_READ
}

struct Events;
impl Events {
    fn drain(&mut self, _dev: &kernel::device::Device, _ring: &Ring) -> Result { log(Ev::Events); Ok(()) }
}
'''

MAIN = r'''
// Object ids in the access log.
const EVENT: u8 = 1; const LOG_ID: u8 = 2; const TRACE: u8 = 3; const STATS: u8 = 4; const STATS_RECORDS: u8 = 5;

struct World { event: Ring, log: Ring, trace: Ring, stats: Ring }

/// The rings: log and trace with fixed indices, some behind; the stats ring at `read` with the
/// producer's successive loads `producers`; record s carries tag `tag(s)` and word `index(s)`.
fn world(read: u32, producers: &[u32], records: usize, recs: &dyn Fn(usize) -> (u32, u32)) -> World {
    let ring = |id: u8, ring_id: u8, state: usize, size: usize| Ring { state: FwObject::new(id, state), ring: FwObject::new(ring_id, size) };
    let w = World {
        event: ring(EVENT, 11, rings::STATE_SIZE, 0x40),
        log: ring(LOG_ID, 12, rings::LOG_STATE_SIZE, 0x40),
        trace: ring(TRACE, 13, rings::STATE_SIZE, 0x40),
        stats: ring(STATS, STATS_RECORDS, rings::STATE_SIZE, records),
    };
    for i in 0..rings::LOG_RINGS {
        w.log.state.set(i * 0x30 + STATE_READ, (i * 7) as u32);
        w.log.state.set(i * 0x30 + STATE_WRITE, if i % 2 == 0 { (i * 7) as u32 } else { (i * 7 + 3) as u32 });
    }
    w.trace.state.set(STATE_READ, 40);
    w.trace.state.set(STATE_WRITE, 41);
    w.stats.state.set(STATE_READ, read);
    w.stats.state.firmware(STATE_WRITE, producers);
    for s in 0..records / pstate::STATS_RECORD {
        let (tag, index) = recs(s);
        w.stats.ring.set(s * pstate::STATS_RECORD + pstate::STATS_TAG, tag);
        w.stats.ring.set(s * pstate::STATS_RECORD + pstate::POWER_STATE_INDEX, index);
        w.stats.ring.set(s * pstate::STATS_RECORD + 4, 0xdead_0000 | s as u32);
    }
    w
}
fn old(w: &World) -> OldChannels { OldChannels { event: w.event.clone(), log: w.log.clone(), trace: w.trace.clone(), stats: w.stats.clone() } }
fn new(w: &World) -> Channels { Channels { event: w.event.clone(), log: w.log.clone(), trace: w.trace.clone(), stats: w.stats.clone(), stats_seen: StatsSeen::default() } }
impl Clone for Ring { fn clone(&self) -> Self { Ring { state: self.state.clone(), ring: self.ring.clone() } } }

/// What the firmware sees: stores and write barriers, in order.
fn visible(log: &[Ev]) -> Vec<Ev> { log.iter().copied().filter(|e| matches!(e, Ev::Store(..) | Ev::Wmb)).collect() }
fn state(w: &World) -> Vec<Vec<u8>> { [&w.event, &w.log, &w.trace, &w.stats].iter().map(|r| r.state.mem.borrow().clone()).collect() }
fn stats_index_loads(log: &[Ev]) -> Vec<(usize, u32)> { log.iter().filter_map(|e| match e { Ev::Load(STATS, at, v) => Some((*at, *v)), _ => None }).collect() }
fn record_loads(log: &[Ev]) -> Vec<usize> { log.iter().filter_map(|e| match e { Ev::Load(STATS_RECORDS, at, _) => Some(*at), _ => None }).collect() }

fn pattern(s: usize) -> (u32, u32) { ((s % 20) as u32, (s as u32) ^ 0x5a) }

/// The expected notes of slots `read..write` (with wrap) under `recs`.
fn expect(read: u32, write: u32, recs: &dyn Fn(usize) -> (u32, u32)) -> (StatsSeen, Vec<usize>) {
    let mut seen = StatsSeen::default();
    let mut loads = Vec::new();
    let mut s = read;
    while s != write {
        let (tag, index) = recs(s as usize);
        seen.records += 1;
        seen.tags[(tag as usize).min(pstate::TAGS - 1)] += 1;
        if tag == pstate::TAG_POWER_STATE { seen.last = Some(index); }
        loads.push(s as usize * pstate::STATS_RECORD + pstate::STATS_TAG);
        loads.push(s as usize * pstate::STATS_RECORD + pstate::POWER_STATE_INDEX);
        s = (s + 1) % 256;
    }
    (seen, loads)
}

fn main() {
    let dev = kernel::device::Device;
    let size = rings::STATS_RING_SIZE;
    // The record reads stay inside the ring object; the kernel holds the same with static_assert.
    assert_eq!(pstate::STATS_RECORD * pstate::STATS_SLOTS as usize, rings::STATS_RING_SIZE);
    assert!(pstate::STATS_TAG + 4 <= pstate::STATS_RECORD && pstate::POWER_STATE_INDEX + 4 <= pstate::STATS_RECORD);
    assert_eq!(pstate::STATS_SLOTS, RING_SLOTS);

    // 1. Every consumer/producer pair, inside and outside the ring, with a steady producer.
    let mut idx: Vec<u32> = (0..256).collect();
    idx.extend([256, 257, 0x1000, 0x7fff_ffff, u32::MAX]);
    let mut pairs = 0u64;
    for &r in &idx { for &p in &idx {
        let wo = world(r, &[p], size, &pattern);
        let mut ev = Events;
        take_log();
        old(&wo).drain_rings(&dev, &mut ev).unwrap();
        let lo = take_log();
        let wn = world(r, &[p], size, &pattern);
        let mut ch = new(&wn);
        ch.drain_rings(&dev, &mut ev).unwrap();
        let ln = take_log();
        assert_eq!(visible(&lo), visible(&ln), "stores for read {r:#x} producer {p:#x}");
        assert_eq!(state(&wo), state(&wn), "state for read {r:#x} producer {p:#x}");
        let (io, inew) = (stats_index_loads(&lo), stats_index_loads(&ln));
        assert_eq!(io, [(STATE_WRITE, p), (STATE_READ, r)]);
        assert_eq!(inew[..2], io[..], "producer before consumer");
        if r < 256 && p < 256 {
            let (seen, loads) = expect(r, p, &pattern);
            assert_eq!(inew, [(STATE_WRITE, p), (STATE_READ, r), (STATE_READ, r)], "consumer loaded again before the store");
            assert_eq!(record_loads(&ln), loads, "records {r}..{p}");
            assert_eq!(ch.stats_seen, seen);
            let rmb = ln.iter().position(|e| *e == Ev::Rmb).expect("read barrier");
            let second = ln.iter().position(|e| *e == Ev::Load(STATS, STATE_READ, r)).unwrap();
            assert!(second < rmb, "barrier after the indices");
            if let Some(first) = ln.iter().position(|e| matches!(e, Ev::Load(STATS_RECORDS, ..))) {
                assert!(rmb < first, "barrier before the records");
            }
            let last_record = ln.iter().rposition(|e| matches!(e, Ev::Load(STATS_RECORDS, ..))).unwrap_or(rmb);
            let reread = ln.iter().rposition(|e| *e == Ev::Load(STATS, STATE_READ, r)).unwrap();
            assert!(last_record < reread, "consumer loaded again after the records");
            // The record loads complete before the store hands their slots back.
            let mbs: Vec<usize> = ln.iter().enumerate().filter(|(_, e)| **e == Ev::Mb).map(|(i, _)| i).collect();
            match ln.iter().position(|e| matches!(e, Ev::Store(STATS, ..))) {
                Some(store) => {
                    assert_eq!(mbs.len(), 1, "one full barrier before the consumer store");
                    assert!(last_record < mbs[0] && mbs[0] < store, "full barrier after the records, before the store");
                    assert_eq!(ln.get(store + 1), Some(&Ev::Wmb));
                }
                None => assert!(mbs.is_empty()),
            }
        } else {
            assert_eq!(inew, [(STATE_WRITE, p), (STATE_READ, r), (STATE_WRITE, p), (STATE_READ, r)], "outside the ring: the earlier drain_one");
            assert!(record_loads(&ln).is_empty() && !ln.contains(&Ev::Rmb) && !ln.contains(&Ev::Mb));
            assert_eq!(ch.stats_seen, StatsSeen::default());
        }
        // peek_stats() on the same ring stores nothing and notes what the drain would.
        let wp = world(r, &[p], size, &pattern);
        let cp = new(&wp);
        take_log();
        let peeked = cp.peek_stats().unwrap();
        let lp = take_log();
        assert!(visible(&lp).is_empty(), "peek stores");
        assert_eq!(peeked, ch.stats_seen);
        pairs += 1;
    }}

    // 2. A producer that moves between loads.
    // Inside the ring: one producer load per drain, the same as the earlier drain's.
    for (r, ps) in [(10u32, vec![20u32, 30]), (250, vec![3, 9]), (5, vec![5, 6])] {
        let wo = world(r, &ps, size, &pattern);
        let wn = world(r, &ps, size, &pattern);
        let (o, mut n) = (old(&wo), new(&wn));
        let mut ev = Events;
        take_log(); o.drain_rings(&dev, &mut ev).unwrap(); o.drain_rings(&dev, &mut ev).unwrap(); let lo = take_log();
        n.drain_rings(&dev, &mut ev).unwrap(); let first = take_log();
        n.drain_rings(&dev, &mut ev).unwrap(); let second = take_log();
        let both: Vec<Ev> = first.iter().chain(second.iter()).copied().collect();
        assert_eq!(visible(&lo), visible(&both), "moving producer {ps:?}");
        let (s1, l1) = expect(r, ps[0], &pattern);
        assert_eq!(record_loads(&first), l1);
        let (_, l2) = expect(ps[0], ps[1], &pattern);
        assert_eq!(record_loads(&second), l2, "each record noted once");
        assert_eq!(n.stats_seen.records, s1.records + l2.len() as u64 / 2);
    }
    // Outside the ring: the earlier drain_one() loads the producer again and stores that load.
    for (r, ps) in [(10u32, vec![300u32, 40]), (300, vec![40, 41]), (10, vec![u32::MAX, 10])] {
        let wn = world(r, &ps, size, &pattern);
        let mut n = new(&wn);
        let mut ev = Events;
        take_log(); n.drain_rings(&dev, &mut ev).unwrap(); let ln = take_log();
        // The earlier drain with the producer the fallback loads.
        let wo = world(r, &ps[1..], size, &pattern);
        old(&wo).drain_rings(&dev, &mut ev).unwrap(); let lo = take_log();
        assert_eq!(visible(&ln), visible(&lo), "fallback store for {ps:?}");
        assert_eq!(stats_index_loads(&ln), [(STATE_WRITE, ps[0]), (STATE_READ, r), (STATE_WRITE, ps[1]), (STATE_READ, r)]);
        assert_eq!(n.stats_seen, StatsSeen::default());
    }

    // 3. Wrap, empty and full, with the parsing.
    let recs = |s: usize| match s {
        254 => (pstate::TAG_POWER_STATE, 3),
        2 => (pstate::TAG_POWER_STATE, 7),
        0 => (4, 99),
        1 => (200, 98),
        _ => (9, 97),
    };
    let wn = world(250, &[5], size, &recs);
    let mut n = new(&wn); let mut ev = Events;
    n.drain_rings(&dev, &mut ev).unwrap(); take_log();
    assert_eq!(n.stats_seen.records, 11);
    assert_eq!(n.stats_seen.last, Some(7), "the newest power-state record, across the wrap");
    assert_eq!(n.stats_seen.tags[pstate::TAG_POWER_STATE as usize], 2);
    assert_eq!(n.stats_seen.tags[4], 1);
    assert_eq!(n.stats_seen.tags[pstate::TAGS - 1], 1);
    assert_eq!(n.stats_seen.tags[9], 7);
    assert_eq!(wn.stats.state.mem.borrow()[0..4], 5u32.to_le_bytes());
    for (r, p, count) in [(0u32, 0u32, 0u64), (128, 128, 0), (255, 255, 0), (10, 9, 255), (0, 255, 255), (255, 254, 255), (255, 0, 1)] {
        let wn = world(r, &[p], size, &pattern);
        let mut n = new(&wn);
        n.drain_rings(&dev, &mut ev).unwrap();
        let l = take_log();
        assert_eq!(n.stats_seen.records, count, "{r}..{p}");
        assert_eq!(l.iter().filter(|e| matches!(e, Ev::Store(STATS, ..))).count(), (count != 0) as usize);
    }
    // Notes accumulate across drains.
    let wn = world(0, &[100, 200, 200], size, &pattern);
    let mut n = new(&wn);
    for _ in 0..3 { n.drain_rings(&dev, &mut ev).unwrap(); }
    take_log();
    assert_eq!(n.stats_seen, expect(0, 200, &pattern).0);

    // 4. A record that cannot be loaded (not reachable in the kernel: see the size checks above).
    // The drain then fails before its store and keeps its notes.
    let wn = world(250, &[5], 0x3000, &pattern);
    let mut n = new(&wn);
    take_log();
    assert_eq!(n.drain_rings(&dev, &mut ev), Err(EINVAL));
    let l = take_log();
    assert!(visible(&l).iter().all(|e| !matches!(e, Ev::Store(STATS, ..))));
    assert_eq!(n.stats_seen, StatsSeen::default());

    // 5. The HardwareDataA frequency single against the host's arithmetic.
    let mut singles = 0u32;
    for sign in [0u32, 1] { for exp in 0u32..=255 {
        for frac in [0u32, 1, 2, 0x3ff, 0x40_0000, 0x40_0001, 0x55_5555, 0x7f_fffe, 0x7f_ffff] {
            let bits = sign << 31 | exp << 23 | frac;
            let f = f32::from_bits(bits);
            let want = if f == 0.0 { Some(0) } else if !f.is_finite() || f < 0.0 { None } else {
                let r = (f as f64 * 1000.0).round_ties_even();
                if r > u32::MAX as f64 { None } else { Some(r as u32) }
            };
            assert_eq!(pstate::f32_mhz_to_khz(bits), want, "{bits:#010x}");
            singles += 1;
        }
    }}

    println!("PASS actual G15 drain bodies: {pairs} consumer/producer pairs, moving producers, wrap/empty/full parsing, peek, record fault, {singles} singles");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--output', type=Path, help='Work directory (default: a temporary one)')
    p.add_argument('--ref', help='Read the tested bodies at this git revision')
    p.add_argument('--mutation', choices=sorted(MUTATIONS))
    p.add_argument('--mutations', action='store_true', help='Also require every mutation to fail')
    a = p.parse_args()
    import tempfile
    out = (a.output or Path(tempfile.mkdtemp(prefix='g15-stats-drain-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)

    def read(path, ref=None):
        ref = ref or a.ref
        return (subprocess.check_output(['git', 'show', f'{ref}:{path}'], cwd=ROOT, text=True,
                                        stderr=subprocess.DEVNULL)
                if ref else (ROOT / path).read_text())

    # The embedded reference is the commit's own text, when the commit is here.
    try:
        old_git = item(read(CHANNEL.replace("drivers/gpu/drm/asahi/", "drivers/gpu/drm/asahi-m3/"), OLD_REF), '    pub(crate) fn drain_rings(')
    except (subprocess.CalledProcessError, ValueError):
        old_git = None
    if old_git is not None and old_git != OLD_DRAIN_RINGS:
        raise SystemExit('embedded drain_rings() differs from ' + OLD_REF)

    def run(mutation):
        src = {CHANNEL: read(CHANNEL), PSTATE: read(PSTATE)}
        if mutation:
            path, old, new = MUTATIONS[mutation]
            if src[path].count(old) != 1:
                raise SystemExit(f'mutation {mutation}: anchor not found once')
            src[path] = src[path].replace(old, new)
        channel, hw, fw = src[CHANNEL], read(HW), read(FW_CHANNELS)
        drain_one = item(channel, '    fn drain_one(ring: &Ring, offset: usize) -> Result {')
        bodies = [item(channel, s) for s in (
            '    fn note_stats(ring: &Ring, seen: &mut StatsSeen) -> Result<Option<u32>> {',
            '    pub(crate) fn peek_stats(&self) -> Result<StatsSeen> {',
            '    pub(crate) fn drain_rings(')]
        ring_read = item(channel, '    pub(crate) fn read(&self) -> Result<u32> {')
        consts = '\n'.join(const(fw, n) for n in ('STATE_READ', 'STATE_WRITE', 'RING_SLOTS'))
        consts += '\nmod rings {\n' + '\n'.join(
            const(hw, n) for n in ('STATE_SIZE', 'LOG_RINGS', 'LOG_STATE_SIZE', 'STATS_RING_SIZE')) + '\n}\n'
        tested = drain_one + ''.join(bodies) + ring_read + src[PSTATE]
        dir = out / (mutation or 'actual')
        dir.mkdir(exist_ok=True)
        (dir / 'pstate.rs').write_text(src[PSTATE])
        code = (PREFIX.replace('PSTATE_PATH', str(dir / 'pstate.rs')).replace('RING_READ', ring_read) + consts +
                'struct OldChannels { event: Ring, log: Ring, trace: Ring, stats: Ring }\n'
                'impl OldChannels {\n' + drain_one + '\n' + OLD_DRAIN_RINGS + '\n}\n'
                'struct Channels { event: Ring, log: Ring, trace: Ring, stats: Ring, stats_seen: StatsSeen }\n'
                'impl Channels {\n' + drain_one + '\n' + '\n'.join(bodies) + '\n}\n' + MAIN)
        path = dir / 'drain.rs'
        path.write_text(code)
        binary = dir / 'drain'
        subprocess.run(['rustup', 'run', '1.93.1', 'rustc', '--edition=2021', '-O', '-C', 'overflow-checks=on',
                        '-C', 'panic=abort', str(path), '-o', str(binary)], check=True)

        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        r = subprocess.run([str(binary)], capture_output=True, text=True, preexec_fn=no_core, timeout=300)
        return {'mutation': mutation, 'tested_sha256': hashlib.sha256(tested.encode()).hexdigest(),
                'exit': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr[-2000:]}

    result = run(a.mutation)
    receipt = {'ref': a.ref, 'old_ref': OLD_REF, 'old_reference_checked_against_git': old_git is not None,
               **result,
               'scope': 'Actual drain_one/note_stats/peek_stats/drain_rings and power/pstate.rs against '
                        f'drain_rings at {OLD_REF[:12]}, over a model of the firmware objects with an '
                        'ordered access log. Not kernel, firmware or GPU execution, and not timing: the '
                        'noting drain loads more words and takes longer.'}
    if a.mutations and not a.mutation:
        receipt['mutations'] = {}
        for m in sorted(MUTATIONS):
            res = run(m)
            receipt['mutations'][m] = {'exit': res['exit'], 'stderr': res['stderr'][-400:]}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k != 'stderr'}, indent=2))
    # The actual bodies must pass; a single requested mutation must fail.
    if (receipt['exit'] != 0) != bool(a.mutation):
        raise SystemExit(1)
    if a.mutations and any(v['exit'] == 0 for v in receipt.get('mutations', {}).values()):
        raise SystemExit('a mutation survived')


if __name__ == '__main__':
    main()
