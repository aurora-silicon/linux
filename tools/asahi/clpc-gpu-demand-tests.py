#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run production CLPC demand/hold logic with controlled PMGR reads and time."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(os.environ.get('CLPC_ROOT', Path(__file__).resolve().parents[2]))


def item(text, signature):
    start = text.index(signature)
    begin = text.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def test_g17_busy_share():
    """Compile and run production g17 BusyShare (power.rs) with negative controls.

    Returns the (t_ms, permille) reports emitted for the long-command-beside-
    light-compositor scenario, and the (t_ms, permille or -1 for no report)
    samples of a new burst after idle, so the C CLPC harness drives
    apple_clpc_sample() with them end-to-end.
    """
    power = (ROOT / 'drivers/gpu/drm/asahi/g17/power.rs').read_text()
    consts = '\n'.join(
        re.search(r'^pub\(super\) const ' + name + r': u64 = [^\n]+$', power, re.M).group(0)
        for name in ('BUSY_WINDOW_NS', 'BUSY_WINDOW_MAX_NS', 'BUSY_IDLE_RESET_NS', 'BUSY_LONG_COMMAND_NS'))
    struct_def = item(power, 'pub(super) struct BusyShare {')
    impl_def = item(power, 'impl BusyShare {')
    prod = f'{consts}\n{struct_def}\n{impl_def}\n'

    harness = r'''
#![allow(dead_code)]
mod usage {
    pub const NUM_ENGINES: usize = 3;
}
mod power {
''' + prod + r'''
}
use power::{BusyShare, BUSY_LONG_COMMAND_NS};

fn main() {
    const MS: u64 = 1_000_000;
    // Scenario 1: long 500 ms compute job running next to a light compositor.
    // Light compositor runs 10 ms per 200 ms window (50 permille -> level 0).
    let mut s = BusyShare::new();
    assert_eq!(s.sample(1000 * MS, [0, 0, 0], [0, 0, 0]), None);
    let r0 = s.sample(1200 * MS, [2 * MS, 10 * MS, 0], [0, 0, 0]).unwrap();
    assert_eq!(r0, 50);
    assert!(s.below_top());
    // Compute job (500 ms) starts at t=1650 ms (after CLPC has dropped to level 0
    // following its 400 ms down-step hold); compositor submits at t=1810 ms (210 ms
    // after a 1600 ms window start). Must immediately report 1000 while still running.
    let r1 = s.sample(1400 * MS, [4 * MS, 20 * MS, 0], [0, 0, 0]).unwrap();
    assert_eq!(r1, 50);
    let r2 = s.sample(1600 * MS, [6 * MS, 30 * MS, 0], [0, 0, 0]).unwrap();
    assert_eq!(r2, 50);
    let r3 = s.sample(1810 * MS, [8 * MS, 40 * MS, 0], [0, 0, 1650 * MS]).unwrap();
    assert_eq!(r3, 1000, "long in-flight compute next to light compositor must lift to 1000");
    assert!(!s.below_top());
    // Still running across the next 200 ms window at t=2010 ms: holds 1000.
    let r4 = s.sample(2010 * MS, [10 * MS, 50 * MS, 0], [0, 0, 1650 * MS]).unwrap();
    assert_eq!(r4, 1000);
    // Job retires at t=2150 ms (+500 ms retired). At t=2210 ms only the 140 ms in
    // [2010, 2150] counts (700 permille), proving zero double-counting.
    let r5 = s.sample(2210 * MS, [12 * MS, 60 * MS, 500 * MS], [0, 0, 0]).unwrap();
    assert_eq!(r5, 700, "post-retirement window must not double-count prior in-flight time");
    // Next window returns cleanly to compositor's 50 permille.
    let r6 = s.sample(2410 * MS, [14 * MS, 70 * MS, 500 * MS], [0, 0, 0]).unwrap();
    assert_eq!(r6, 50);

    // Scenario 2: long command starting late in a window lifts mid-window at 50 ms.
    let mut late = BusyShare::new();
    assert_eq!(late.sample(1000 * MS, [0, 0, 0], [0, 0, 0]), None);
    assert_eq!(late.sample(1200 * MS, [0, 10 * MS, 0], [0, 0, 0]), Some(50));
    assert_eq!(late.sample(1410 * MS, [0, 20 * MS, 0], [0, 0, 1390 * MS]), Some(95));
    assert!(late.below_top());
    assert_eq!(late.sample(1440 * MS, [0, 22 * MS, 0], [0, 0, 1390 * MS]), Some(1000));
    assert!(!late.below_top());
    assert_eq!(late.sample(1460 * MS, [0, 23 * MS, 0], [0, 0, 1390 * MS]), None);
    assert_eq!(late.sample(1610 * MS, [0, 30 * MS, 100 * MS], [0, 0, 0]), Some(400));

    // Scenario 3: solo long command driven by recheck_delay_for reaches 1000 and stops.
    let mut solo = BusyShare::new();
    assert_eq!(solo.sample(1000 * MS, [0, 0, 0], [0, 0, 0]), None);
    assert_eq!(solo.sample(1200 * MS, [0, 10 * MS, 0], [0, 0, 0]), Some(50));
    let mut t = 1222 * MS;
    let start = 1210 * MS;
    let mut lifted = false;
    let mut rechecks = 0;
    for _ in 0..8 {
        if let Some(p) = solo.sample(t, [0, 10 * MS, 0], [0, 0, start]) {
            assert_eq!(p, 1000);
            lifted = true;
            break;
        }
        let delay = BusyShare::recheck_delay_for(t, start).unwrap();
        assert!(delay <= BUSY_LONG_COMMAND_NS);
        t += delay;
        rechecks += 1;
    }
    // One wake-up at the long-command age, not a train of short ones.
    assert!(lifted && !solo.below_top() && t == start + BUSY_LONG_COMMAND_NS && rechecks == 1);

    // Scenario 4: short straddling command (no double count), multi-engine max, >2 s gaps.
    let mut misc = BusyShare::new();
    assert_eq!(misc.sample(1000 * MS, [0, 0, 0], [0, 0, 0]), None);
    assert_eq!(misc.sample(1200 * MS, [0, 0, 0], [0, 0, 1170 * MS]), Some(150));
    assert_eq!(misc.sample(1400 * MS, [0, 0, 40 * MS], [0, 0, 0]), Some(50));
    assert_eq!(misc.sample(1600 * MS, [0, 120 * MS, 150 * MS], [0, 0, 0]), Some(600));
    assert_eq!(misc.sample(5000 * MS, [0, 120 * MS, 150 * MS], [0, 0, 5000 * MS]), None);
    assert_eq!(misc.sample(8000 * MS, [0, 120 * MS, 150 * MS], [0, 0, 5000 * MS]), Some(1000));

    // Scenario 5: steady 600 permille work until 1400 ms, genuine idle (>= BUSY_IDLE_RESET_NS),
    // then a new burst at 2000 ms. Closing [1300, 2000] would report a diluted 85 permille
    // (60 ms in 700 ms); resume() clears the window so the first submission re-baselines
    // instead and nothing is reported until a fresh 200 ms window of the new burst completes.
    let mut burst = BusyShare::new();
    assert_eq!(burst.sample(900 * MS, [0, 0, 0], [0, 0, 0]), None);
    let mut idle_events = Vec::new();
    for (t, frag) in [(1100, 120), (1300, 240)] {
        let p = burst.sample(t * MS, [0, frag * MS, 0], [0, 0, 0]);
        assert_eq!(p, Some(600));
        idle_events.push((t, p));
    }
    burst.note_inactive(1416 * MS);
    assert!(burst.resume(2000 * MS));
    assert!(!burst.below_top());
    for (t, frag) in [(2000, 300), (2048, 328), (2100, 360), (2148, 388), (2196, 417), (2200, 420)] {
        let p = burst.sample(t * MS, [0, frag * MS, 0], [0, 0, 0]);
        assert_eq!(p, if t < 2200 { None } else { Some(600) },
                   "new burst after idle must not report a window diluted by the idle gap");
        idle_events.push((t, p));
    }
    assert!(burst.below_top());

    // Scenario 6: paced 24 fps light work (42 ms period, 0.5 ms GPU/frame). Effort::idle
    // fires 16 ms after each frame, leaving a 26 ms inactive gap (< BUSY_IDLE_RESET_NS).
    // BusyShare must preserve its 200 ms window across those brief clears and report ~11 permille.
    let mut paced = BusyShare::new();
    let mut paced_events = Vec::new();
    let mut frag_ns = 0u64;
    for frame in 0..18u64 {
        let t_ms = 1000 + frame * 42;
        assert!(!paced.resume(t_ms * MS), "brief inter-frame gap must not reset BusyShare");
        let p = paced.sample(t_ms * MS, [0, frag_ns, 0], [0, 0, 0]);
        paced_events.push((t_ms, p));
        frag_ns += 500_000;
        paced.note_inactive((t_ms + 16) * MS);
    }
    assert!(paced.below_top(), "24 fps light frames must close their 200 ms window");
    assert!(paced_events.iter().any(|&(_, p)| p == Some(11)));

    println!("EVENTS:1200={r0},1400={r1},1600={r2},1810={r3},2010={r4},2210={r5},2410={r6}");
    let idle: Vec<String> =
        idle_events.iter().map(|(t, p)| format!("{t}={}", p.map_or(-1, i64::from))).collect();
    println!("IDLE:{}", idle.join(","));
    let paced_out: Vec<String> =
        paced_events.iter().map(|(t, p)| format!("{t}={}", p.map_or(-1, i64::from))).collect();
    println!("PACED:{}", paced_out.join(","));
}
'''
    rustc = os.environ.get('RUSTC', 'rustc')
    events = idle_events = paced_events = None
    with tempfile.TemporaryDirectory(prefix='asahi-g17-busy-') as tmp:
        root = Path(tmp)
        for name, text in [
            ('fixed', harness),
            ('g17-ignore-in-flight',
             harness.replace('let long_in_flight = Self::is_long_in_flight(now_ns, oldest_in_flight);',
                             'let _ = oldest_in_flight; let oldest_in_flight = [0; crate::usage::NUM_ENGINES];\n'
                             '        let long_in_flight = Self::is_long_in_flight(now_ns, oldest_in_flight);')),
            ('g17-double-count-in-flight',
             harness.replace('let busy = delta.saturating_add(age).saturating_sub(prev_age);',
                             'let _ = prev_age; let busy = delta.saturating_add(age);')),
            ('g17-no-early-lift',
             harness.replace('if self.below_top && long_in_flight {',
                             'if false && self.below_top && long_in_flight {')),
            ('g17-long-idle-reports',
             harness.replace('if elapsed > BUSY_WINDOW_MAX_NS && !long_in_flight {',
                             'if false && elapsed > BUSY_WINDOW_MAX_NS && !long_in_flight {')),
            ('g17-idle-keeps-window',
             harness.replace('if now_ns.saturating_sub(since) >= BUSY_IDLE_RESET_NS {',
                             'if false && now_ns.saturating_sub(since) >= BUSY_IDLE_RESET_NS {')),
            ('g17-brief-idle-resets-window',
             harness.replace('if now_ns.saturating_sub(since) >= BUSY_IDLE_RESET_NS {',
                             'if true || now_ns.saturating_sub(since) >= BUSY_IDLE_RESET_NS {')),
        ]:
            assert name == 'fixed' or text != harness, 'stale mutation: ' + name
            src = root / (name + '.rs')
            src.write_text(text)
            binary = src.with_suffix('')
            subprocess.run([rustc, '--edition=2021', '-Dwarnings', str(src), '-o', str(binary)],
                           check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'fixed':
                assert run.returncode == 0, run.stderr
                lines = run.stdout.strip().splitlines()
                assert len(lines) == 3 and lines[0].startswith('EVENTS:') \
                    and lines[1].startswith('IDLE:') \
                    and lines[2].startswith('PACED:'), lines
                events = [tuple(int(x) for x in pair.split('='))
                          for pair in lines[0].removeprefix('EVENTS:').split(',')]
                idle_events = [tuple(int(x) for x in pair.split('='))
                               for pair in lines[1].removeprefix('IDLE:').split(',')]
                paced_events = [tuple(int(x) for x in pair.split('='))
                                for pair in lines[2].removeprefix('PACED:').split(',')]
                print('PASS: g17 BusyShare (in-flight long command, early lift, solo recheck, '
                      'non-double-counting, new burst after idle, 24 fps brief idle)')
            else:
                assert run.returncode != 0, name
                print('PASS negative control:', name)
    return events, idle_events, paced_events


def strip_dts_comments(text):
    return re.sub(
        r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*',
        lambda m: m.group(0) if m.group(0).startswith('"') else ' ',
        text,
        flags=re.S,
    )


def dt_cells(node_text, name):
    match = re.search(r'(?:^|[\s;])' + re.escape(name) + r'\s*=\s*([^;]*);', node_text)
    if not match:
        return None
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', match.group(1))]


def parse_clpc_dt_gpu_busy(dts_text, node_sig):
    clpc_node = item(strip_dts_comments(dts_text), node_sig)
    return (
        dt_cells(clpc_node, 'apple,gpu-busy-memory-states'),
        dt_cells(clpc_node, 'apple,gpu-busy-permille'),
    )


def parse_t8132_gpu_busy(dts_text):
    states, busy = parse_clpc_dt_gpu_busy(dts_text, 'clpc: performance-controller@3 {')
    assert states and busy, \
        't8132-memory.dtsi clpc node must define both gpu-busy DT properties'
    return states, busy


def test_dt_negative_controls(dts_text, label, parser):
    for prop, short in [
        ('apple,gpu-busy-memory-states', 'states'),
        ('apple,gpu-busy-permille', 'permille'),
    ]:
        pat = re.compile(r'^([ \t]*)(' + re.escape(prop) + r'\s*=[^;]*;)', re.M)
        for kind, repl in [
            ('delete', ''),
            ('block-comment', r'\1/* \2 */'),
            ('line-comment', r'\1// \2'),
        ]:
            mutated, count = pat.subn(repl, dts_text, count=1)
            assert count == 1 and mutated != dts_text, f'stale mutation: {label}-{kind}-{short}'
            try:
                parser(mutated)
            except AssertionError:
                print(f'PASS negative control: {label}-dt-{kind}-{short}')
            else:
                raise AssertionError(f'negative control passed unexpectedly: {label}-dt-{kind}-{short}')
def main():
    g17_events, g17_idle_events, g17_paced_events = test_g17_busy_share()
    source = (ROOT / 'drivers/soc/apple/clpc.c').read_text()
    header = (ROOT / 'include/linux/soc/apple/pmc.h').read_text()
    rail_shift = re.search(r'^#define APPLE_PMC_RAIL_SHIFT\(rail\).*$', header, re.M).group(0)
    dense_samples = re.search(r'^#define APPLE_CLPC_GPU_DENSE_SAMPLES\s.*$', source, re.M).group(0)
    reasons = '\n'.join(re.findall(r'^#define APPLE_CLPC_REASON_\w+\s.*$', source, re.M))
    gpu_down = re.search(r'^#define APPLE_CLPC_GPU_DOWN_MS\s.*$', source, re.M).group(0)
    types = '\n'.join(item(source, s) + ';' for s in [
        'struct apple_clpc_hw {', 'struct apple_clpc_vote {'])
    tables = '\n'.join(item(source, 'static const struct apple_clpc_hw ' + n + ' =') + ';'
                       for n in ['apple_clpc_hw_t8132', 'apple_clpc_hw_t8140',
                                 'apple_clpc_hw_t6040', 'apple_clpc_hw_t6041',
                                 'apple_clpc_hw_t6050'])
    # When t6041.dtsi supplies the GPU busy-share properties, parse and test them directly;
    # otherwise exercise the DT parsers and ladder against the reference values.
    dts = (ROOT / 'arch/arm64/boot/dts/apple/t6041.dtsi').read_text()
    clpc_node = item(dts, '&clpc {')

    def cells(name):
        match = re.search(r'\s' + re.escape(name) + r'\s*=\s*([^;]*);', clpc_node)
        if not match:
            return None
        return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', match.group(1))]

    dt_states = cells('apple,gpu-busy-memory-states')
    dt_busy = cells('apple,gpu-busy-permille')
    assert (dt_states is None) == (dt_busy is None)
    gpu_states = dt_states or [1, 2, 4, 5, 0, 0, 0, 3]
    gpu_busy = dt_busy or [160, 500, 800]
    # Parse T8132's shared CLPC node directly; both DT properties are required.
    t8132_dts = (ROOT / 'arch/arm64/boot/dts/apple/t8132-memory.dtsi').read_text()
    t8132_states, t8132_busy = parse_t8132_gpu_busy(t8132_dts)
    test_dt_negative_controls(t8132_dts, 't8132', parse_t8132_gpu_busy)

    t6041_dt = (
        'static const u32 t6041_gpu_states[] = {' + ', '.join(str(x) for x in gpu_states) + '};\n'
        'static const u32 t6041_gpu_busy[] = {' + ', '.join(str(x) for x in gpu_busy) + '};\n'
        'static const u32 t8132_gpu_states[] = {' + ', '.join(str(x) for x in t8132_states) + '};\n'
        'static const u32 t8132_gpu_busy[] = {' + ', '.join(str(x) for x in t8132_busy) + '};\n'
        'static const struct { unsigned long ms; unsigned int permille; } g17_long_events[] = {'
        + ', '.join(f'{{{t}, {p}}}' for t, p in g17_events) + '};\n'
        'static const struct { unsigned long ms; int permille; } g17_idle_events[] = {'
        + ', '.join(f'{{{t}, {p}}}' for t, p in g17_idle_events) + '};\n'
        'static const struct { unsigned long ms; int permille; } g17_paced_events[] = {'
        + ', '.join(f'{{{t}, {p}}}' for t, p in g17_paced_events) + '};\n')
    methods = '\n'.join(item(source, s) for s in [
        'void apple_clpc_gpu_set_active(',
        'void apple_clpc_gpu_set_busy(',
        'void apple_clpc_gpu_clear_busy(',
        'static unsigned int apple_clpc_vote(', 'static u32 apple_clpc_floor(',
        'static bool apple_clpc_gpu_streaming(',
        'static bool apple_clpc_gpu_dense_step(',
        'static bool apple_clpc_gpu_dense(',
        'static bool apple_clpc_gpu_limited(',
        'static void apple_clpc_gpu_floor_count(',
        'static unsigned int apple_clpc_gpu_level(',
        'static int apple_clpc_dt_map(',
        'static int apple_clpc_dt_busy(',
        'static bool apple_clpc_dt_apply(',
        'static bool apple_clpc_gpu_input_demand(',
        'static bool apple_clpc_ane_input_demand(',
        'static void apple_clpc_ane_cluster(',
        'static void apple_clpc_sample('])
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
struct device;
#define APPLE_CLPC_LEVELS 4
#define APPLE_CLPC_HOLD_MS 32
#define APPLE_CLPC_SAMPLE_MS 4
#define APPLE_CLPC_GPU_BACKOFF_MS 100
#define HZ 1000
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h,l) (((1u << ((h)-(l)+1))-1) << (l))
#define BIT(n) (1u << (n))
#define APPLE_PMC_RAIL_SHIFT(r) ((r)*8)
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
#define time_after(a,b) ((long)((a)-(b)) > 0)
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define msecs_to_jiffies(ms) (ms)
#define system_freezable_power_efficient_wq NULL
static void mod_delayed_work(void *wq, void *w, unsigned long d) { (void)wq; (void)w; (void)d; }
static int warned, dt_warned;
#define dev_warn_once(d, ...) ((void)(d), warned++)
#define dev_warn(d, ...) ((void)(d), dt_warned++)
enum apple_clpc_rail { APPLE_CLPC_DCS, APPLE_CLPC_SOC, APPLE_CLPC_RAILS };
''' + types + r'''
struct apple_clpc_gpu {
 int lock; bool active; u64 updated_ns; u64 sequence;
 unsigned int busy; bool busy_valid; unsigned int level; unsigned long level_since;
 void *wake;
};
struct apple_clpc_ane { int lock; bool active, pending; };
#define spin_lock_irqsave(l, f) ((void)(l), (f) = 0)
#define spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
struct apple_clpc_cluster { unsigned int request[APPLE_CLPC_RAILS]; };
// cpufreq QoS stand-ins: one policy, the last requested minimum is recorded.
#define FREQ_QOS_MIN 1
#define FREQ_QOS_MIN_DEFAULT_VALUE 0
struct freq_qos_request { bool active; int value; };
struct cpufreq_policy { int constraints; };
static struct cpufreq_policy policy;
static int qos_min = -1, qos_adds;
static bool freq_qos_request_active(struct freq_qos_request *r) { return r->active; }
static struct cpufreq_policy *cpufreq_cpu_get(int cpu) { (void)cpu; return &policy; }
static void cpufreq_cpu_put(struct cpufreq_policy *p) { (void)p; }
static int freq_qos_add_request(int *c, struct freq_qos_request *r, int type, int v) {
 (void)c; assert(type==FREQ_QOS_MIN && !r->active); r->active=true; r->value=v; qos_min=v; qos_adds++; return 1;
}
static int freq_qos_update_request(struct freq_qos_request *r, int v) { assert(r->active); r->value=v; qos_min=v; return 1; }
struct apple_clpc {
 const struct apple_clpc_hw *hw;
 struct apple_clpc_cluster clusters[1];
 struct apple_clpc_vote vote[APPLE_CLPC_RAILS];
 unsigned int nr_clusters, agent;
 unsigned long samples, next_scan, update_errors, gpu_read_errors;
 void *dev;
 void *covered, *pmc, *gpu_pmgr;
 u32 gpu_offset, mask, last_floor, last_reason;
 bool gpu_on, gpu_limited, gpu_floor_released;
 unsigned long gpu_on_since, gpu_sampled, gpu_limited_at;
 u64 gpu_sequence;
 unsigned int gpu_dense_streak;
 unsigned long gpu_floor_releases, gpu_floor_returns;
 unsigned long gpu_streaming_samples, gpu_dense_samples;
 u64 last_vote_ns;
 struct apple_clpc_gpu *gpu;
 struct apple_clpc_ane *ane;
 unsigned int ane_states[APPLE_CLPC_RAILS];
 u32 ane_cluster_khz;
 int ane_cpu;
 struct freq_qos_request ane_qos;
 unsigned int ane_qos_khz;
 unsigned long ane_until;
 bool has_pmc;
 bool low_power;
 unsigned int low_power_khz;
 unsigned int low_power_mw;
};
static unsigned long jiffies;
static u64 ktime_get_mono_fast_ns(void) { return jiffies * 1000000ull; }
static bool apple_clpc_gpu_active(struct apple_clpc_gpu *g, u64 *u, u64 *s) { *u=jiffies; *s=g->sequence; return g->active; }
static void *cpu_possible_mask;
static u32 pmgr_status, pmp_floor, published;
static int read_error, vote_error;
static bool queued_idle;
static bool cpumask_equal(void *a, void *b) { return a == b; }
static void apple_clpc_find_clusters(struct apple_clpc *c) { (void)c; }
static void apple_clpc_sample_cluster(struct apple_clpc *c, struct apple_clpc_cluster *cl) { (void)c; (void)cl; }
static int regmap_read(void *map, u32 offset, u32 *status) {
 assert(map); (void)offset; *status=pmgr_status; return read_error;
}
static u32 apple_pmc_floor_read(void *pmc, unsigned int agent) {
 (void)pmc; assert(agent==0); return pmp_floor;
}
static int apple_pmc_floor_update(void *pmc, unsigned int agent, u32 mask, u32 floor) {
 (void)pmc; assert(agent==1 && mask==3); published=floor; return vote_error;
}
static void apple_clpc_queue(struct apple_clpc *c, bool idle) { (void)c; queued_idle=idle; }
#define READ_ONCE(x) (x)
static void apple_clpc_profile_limit(struct apple_clpc *c) { (void)c; }
''' + dense_samples + '\n' + reasons + '\n' + gpu_down + '\n' + tables + '\n' + methods + '\n' + t6041_dt + r'''
// Apply DT GPU busy-share properties through the driver's parsers: the CPU map
// stays untouched, valid GPU states/thresholds populate the ladder, states
// above the compatible table's rail maximum are rejected, and any malformed
// optional ladder warns and falls back to the compatible's static table.
static struct apple_clpc_hw t6041_dt_hw;
static void t6041_from_dt(void) {
 static const u8 cpu[2][4] = {{5, 5, 5, 5}, {3, 3, 3, 3}}, gpu[2][4] = {{1, 2, 4, 5}, {0, 0, 0, 3}};
 struct apple_clpc_hw out, bad = apple_clpc_hw_t6040;
 u32 over_4bit[8] = {1, 2, 4, 5, 0, 0, 0, 16};
 u32 t6041_over_dcs[8] = {1, 2, 4, 7, 0, 0, 0, 3};
 u32 t6041_over_dcs6[8] = {1, 2, 4, 6, 0, 0, 0, 3};
 u32 t6041_over_soc[8] = {1, 2, 4, 5, 0, 0, 0, 4};
 u32 t6050_ok[8] = {1, 2, 4, 6, 0, 0, 0, 2};
 u32 t6050_over_dcs[8] = {1, 2, 4, 7, 0, 0, 0, 2};
 u32 t6050_over_soc[8] = {1, 2, 4, 6, 0, 0, 0, 3};
 u32 busy[3] = {500, 160, 800};

 dt_warned = 0;
 assert(apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6041, &t6041_dt_hw,
                            t6041_gpu_states, ARRAY_SIZE(t6041_gpu_states),
                            t6041_gpu_busy, ARRAY_SIZE(t6041_gpu_busy)));
 assert(dt_warned == 0);
 for (int r = 0; r < 2; r++)
  for (int l = 0; l < 4; l++)
   assert(t6041_dt_hw.map[r][l] == cpu[r][l] && t6041_dt_hw.gpu_busy[r][l] == gpu[r][l]);

 // Direct parser bounds: 4-bit max, rail-specific compatible max, counts, thresholds.
 assert(apple_clpc_dt_map(&bad, over_4bit, 8) == -EINVAL);
 assert(apple_clpc_dt_map(&bad, over_4bit, 7) == -EINVAL);
 assert(apple_clpc_dt_map(&bad, t6041_gpu_states, 8) == -EINVAL); // fabric 3 > T6040 max 2
 bad = apple_clpc_hw_t6041;
 assert(apple_clpc_dt_map(&bad, t6041_over_dcs, 8) == -EINVAL);   // DCS 7 > T6041 max 5
 assert(apple_clpc_dt_map(&bad, t6041_over_dcs6, 8) == -EINVAL);  // DCS 6 > T6041 max 5
 assert(apple_clpc_dt_map(&bad, t6041_over_soc, 8) == -EINVAL);   // fabric 4 > T6041 max 3
 // T6050 rail max uses max(map[rail][top], gpu[rail]): DCS 6 from gpu[DCS], fabric 2.
 assert(apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6050, &out,
                            t6050_ok, 8, t6041_gpu_busy, 3));
 assert(dt_warned == 0 && out.gpu_busy[APPLE_CLPC_DCS][3] == 6);
 assert(!apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6050, &out,
                             t6050_over_dcs, 8, t6041_gpu_busy, 3));
 assert(dt_warned == 1 && !memcmp(&out, &apple_clpc_hw_t6050, sizeof(out)));
 assert(!apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6050, &out,
                             t6050_over_soc, 8, t6041_gpu_busy, 3));
 assert(dt_warned == 2 && !memcmp(&out, &apple_clpc_hw_t6050, sizeof(out)));

 // Malformed optional DT properties warn and fall back to the static soc table unchanged.
 assert(!apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6041, &out,
                             t6041_over_dcs, 8, t6041_gpu_busy, 3));
 assert(dt_warned == 3 && !memcmp(&out, &apple_clpc_hw_t6041, sizeof(out)));
 assert(!apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6041, &out,
                             t6041_over_soc, 8, t6041_gpu_busy, 3));
 assert(dt_warned == 4 && !memcmp(&out, &apple_clpc_hw_t6041, sizeof(out)));
 // Valid states paired with bad permille or missing partner must not leave partial ladder.
 assert(!apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6041, &out,
                             t6041_gpu_states, 8, busy, 3));
 assert(dt_warned == 5 && !memcmp(&out, &apple_clpc_hw_t6041, sizeof(out)));
 assert(!apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6041, &out,
                             t6041_gpu_states, 8, NULL, -EINVAL));
 assert(dt_warned == 6 && !memcmp(&out, &apple_clpc_hw_t6041, sizeof(out)));
 assert(!apple_clpc_dt_apply(NULL, &apple_clpc_hw_t6041, &out,
                             NULL, -EINVAL, t6041_gpu_busy, 3));
 assert(dt_warned == 7 && !memcmp(&out, &apple_clpc_hw_t6041, sizeof(out)));
 // Falling back keeps the compatible's full GPU floor even when a low busy share arrives.
 {
  struct apple_clpc_gpu g = {.active = true, .busy_valid = true, .busy = 100};
  struct apple_clpc c = {.hw = &out, .has_pmc = true, .agent = 1, .mask = 3, .gpu = &g};
  read_error = vote_error = 0; pmgr_status = 0xf0; pmp_floor = 0; jiffies = 2900; published = ~0u;
  apple_clpc_sample(&c);
  assert(published == 0x35 && c.last_reason == 2);
 }

 assert(apple_clpc_dt_busy(&bad, busy, 3) == -EINVAL);
 busy[0] = 160; busy[1] = 500; busy[2] = 1001;
 assert(apple_clpc_dt_busy(&bad, busy, 3) == -EINVAL);
 assert(apple_clpc_dt_busy(&bad, busy, 2) == -EINVAL);
}
static struct apple_clpc_hw t8132_dt_hw;
static void t8132_from_dt(void) {
 static const u8 cpu[2][4] = {{5, 5, 5, 5}, {3, 3, 3, 3}}, gpu[2][4] = {{1, 2, 4, 5}, {0, 0, 0, 3}};
 int prev = dt_warned;

 dt_warned = 0;
 assert(apple_clpc_dt_apply(NULL, &apple_clpc_hw_t8132, &t8132_dt_hw,
                            t8132_gpu_states, ARRAY_SIZE(t8132_gpu_states),
                            t8132_gpu_busy, ARRAY_SIZE(t8132_gpu_busy)));
 assert(dt_warned == 0);
 for (int r = 0; r < 2; r++)
  for (int l = 0; l < 4; l++)
   assert(t8132_dt_hw.map[r][l] == cpu[r][l] && t8132_dt_hw.gpu_busy[r][l] == gpu[r][l]);
 dt_warned = prev;
}

static void check_t8132_busy(void) {
 struct apple_clpc_gpu gpu = {0};
 struct apple_clpc c = {.hw = &t8132_dt_hw, .has_pmc = true, .agent = 1, .mask = 3, .gpu = &gpu};
 assert(c.hw->gpu_busy_mid == 160 && c.hw->gpu_busy_high == 500 && c.hw->gpu_busy_top == 800);
 read_error = vote_error = 0; pmgr_status = 0xf0; pmp_floor = 0; published = ~0u;
 gpu.active = true; jiffies = 8000; apple_clpc_sample(&c);
 assert(published == 0x35 && c.last_reason == 2 && gpu.level == 3);
 gpu.active = false; jiffies = 8100; apple_clpc_sample(&c); assert(published == 0);
 gpu.active = true; gpu.busy_valid = true; gpu.busy = 159; jiffies = 8500;
 apple_clpc_sample(&c); assert(published == 0x01 && gpu.level == 0);
 gpu.busy = 160; jiffies = 8504; apple_clpc_sample(&c); assert(published == 0x02 && gpu.level == 1);
 gpu.busy = 499; jiffies = 8508; apple_clpc_sample(&c); assert(published == 0x02 && gpu.level == 1);
 gpu.busy = 500; jiffies = 8512; apple_clpc_sample(&c); assert(published == 0x04 && gpu.level == 2);
 gpu.busy = 799; jiffies = 8516; apple_clpc_sample(&c); assert(published == 0x04 && gpu.level == 2);
 gpu.busy = 800; jiffies = 8520; apple_clpc_sample(&c); assert(published == 0x35 && gpu.level == 3);
 gpu.busy = 100;
 for (jiffies = 8524; jiffies < 8920; jiffies += 4) {
  apple_clpc_sample(&c); assert(published == 0x35 && gpu.level == 3);
 }
 jiffies = 8920; apple_clpc_sample(&c); assert(published == 0x31 && gpu.level == 0);
 jiffies = 8952; apple_clpc_sample(&c); assert(published == 0x01 && c.last_reason == 2);
 gpu.active = false; jiffies = 9000; apple_clpc_sample(&c); assert(published == 0 && queued_idle);
}
// T8140 ANE work holds its DRAM and fabric states while it runs, a request
// shorter than one sample is still seen once, and the hold then expires.
static void check_t8140_ane(void) {
 struct apple_clpc_ane ane={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.has_pmc=true,.agent=1,.mask=3,.ane=&ane};
 c.ane_states[APPLE_CLPC_DCS]=c.hw->ane[APPLE_CLPC_DCS];
 c.ane_states[APPLE_CLPC_SOC]=c.hw->ane[APPLE_CLPC_SOC];
 read_error=vote_error=0; pmgr_status=0; pmp_floor=0; jiffies=1000; published=~0u;
 apple_clpc_sample(&c); assert(published==0 && c.last_reason==0);
 ane.active=true; jiffies=1004; apple_clpc_sample(&c);
 assert(published==0x34 && c.last_reason==32);
 ane.active=false; ane.pending=true; jiffies=1008; apple_clpc_sample(&c);
 assert(published==0x34 && c.last_reason==32 && !ane.pending);
 jiffies=1012; apple_clpc_sample(&c); assert(published==0x34 && c.last_reason==16);
 jiffies=1040; apple_clpc_sample(&c); assert(published==0 && queued_idle);
}

// T6050 ANE work holds DCS 6 and fabric 2 while it runs (and keeps on-time
// idle sampling), sees a sub-sample request once, and releases after HOLD_MS.
static void check_t6050_ane(void) {
 struct apple_clpc_ane ane={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t6050,.has_pmc=true,.agent=1,.mask=3,.ane=&ane};
 c.ane_states[APPLE_CLPC_DCS]=c.hw->ane[APPLE_CLPC_DCS];
 c.ane_states[APPLE_CLPC_SOC]=c.hw->ane[APPLE_CLPC_SOC];
 assert(c.hw->ane[APPLE_CLPC_DCS]==6 && c.hw->ane[APPLE_CLPC_SOC]==2 && c.hw->idle_on_time);
 read_error=vote_error=0; pmgr_status=0; pmp_floor=0; jiffies=3000; published=~0u;
 apple_clpc_sample(&c); assert(published==0 && c.last_reason==0);
 ane.active=true; jiffies=3004; apple_clpc_sample(&c);
 assert(published==0x26 && c.last_reason==32);
 ane.active=false; ane.pending=true; jiffies=3008; apple_clpc_sample(&c);
 assert(published==0x26 && c.last_reason==32 && !ane.pending);
 jiffies=3012; apple_clpc_sample(&c); assert(published==0x26 && c.last_reason==16);
 jiffies=3040; apple_clpc_sample(&c); assert(published==0 && c.last_reason==0 && queued_idle);
}

// T6040 ANE work also holds the second P cluster at its qualified frequency,
// added on first use, for the vote hold after the last request, then drops it.
static void check_t6040_ane(void) {
 struct apple_clpc_ane ane={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t6040,.has_pmc=true,.agent=1,.mask=3,.ane=&ane};
 c.ane_states[APPLE_CLPC_DCS]=c.hw->ane[APPLE_CLPC_DCS];
 c.ane_states[APPLE_CLPC_SOC]=c.hw->ane[APPLE_CLPC_SOC];
 c.ane_cpu=9; c.ane_cluster_khz=c.hw->ane_cluster_khz;
 assert(c.hw->ane_cluster==2 && c.ane_cluster_khz==3756000);
 read_error=vote_error=0; pmgr_status=0; pmp_floor=0; jiffies=2000; published=~0u; qos_min=-1; qos_adds=0;
 apple_clpc_sample(&c); assert(published==0 && qos_adds==0 && queued_idle);
 ane.active=true; jiffies=2004; apple_clpc_sample(&c);
 assert(published==0x25 && c.last_reason==32 && qos_adds==1 && qos_min==3756000);
 ane.active=false; jiffies=2008; apple_clpc_sample(&c);
 assert(qos_min==3756000 && !queued_idle);
 jiffies=2036; apple_clpc_sample(&c); assert(published==0 && qos_min==0 && c.ane_qos_khz==0 && queued_idle);
 ane.active=true; jiffies=2040; apple_clpc_sample(&c); assert(qos_adds==1 && qos_min==3756000);
}

// DT GPU busy-share ladder: DCS 1, 2, 4 or 5 and fabric 0, 0, 0 or 3, split at
// 160, 500 and 800 permille. A higher level applies at once, a lower one at the
// first sample at least 400 ms after the last one that asked for the current
// level or more. A table without a non-zero top DCS state falls back to @map.
static void check_t6041_busy(void) {
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc c={.hw=&t6041_dt_hw,.has_pmc=true,.agent=1,.mask=3,.gpu=&gpu};
 struct apple_clpc_hw no_top = t6041_dt_hw;
 assert(c.hw->gpu_busy_mid==160 && c.hw->gpu_busy_high==500 && c.hw->gpu_busy_top==800);
 read_error=vote_error=0; pmgr_status=0xf0; pmp_floor=0; published=~0u;
 // No share reported: the top of the ladder, as before the busy share.
 gpu.active=true; jiffies=3000; apple_clpc_sample(&c);
 assert(published==0x35 && c.last_reason==2 && gpu.level==3);
 gpu.active=false; jiffies=3100; apple_clpc_sample(&c); assert(published==0);
 // Below gpu_busy_mid: the lowest GPU state (after a long idle, at once).
 gpu.active=true; gpu.busy_valid=true; gpu.busy=100; jiffies=3500;
 apple_clpc_sample(&c); assert(published==0x01 && c.last_reason==2 && gpu.level==0);
 // Between the thresholds, going up: each level applies at the next sample.
 gpu.busy=160; jiffies=3504; apple_clpc_sample(&c); assert(published==0x02 && gpu.level==1);
 gpu.busy=499; jiffies=3508; apple_clpc_sample(&c); assert(published==0x02 && gpu.level==1);
 gpu.busy=500; jiffies=3512; apple_clpc_sample(&c); assert(published==0x04 && gpu.level==2);
 gpu.busy=799; jiffies=3516; apple_clpc_sample(&c); assert(published==0x04 && gpu.level==2);
 gpu.busy=800; jiffies=3520; apple_clpc_sample(&c); assert(published==0x35 && gpu.level==3);
 // Going down waits 400 ms from the last sample at the top.
 gpu.busy=100;
 for (jiffies=3524; jiffies<3920; jiffies+=4) {
  apple_clpc_sample(&c); assert(published==0x35 && gpu.level==3);
 }
 jiffies=3919; apple_clpc_sample(&c); assert(published==0x35 && gpu.level==3);
 // At 400 ms the DCS drops at once; the fabric state keeps HOLD_MS.
 jiffies=3920; apple_clpc_sample(&c); assert(published==0x31 && gpu.level==0);
 jiffies=3952; apple_clpc_sample(&c); assert(published==0x01 && c.last_reason==2);
 // A sample that asks for the current level again restarts the 400 ms.
 gpu.busy=600; jiffies=4000; apple_clpc_sample(&c); assert(published==0x04 && gpu.level==2);
 gpu.busy=100; jiffies=4200; apple_clpc_sample(&c); assert(gpu.level==2);
 gpu.busy=550; jiffies=4300; apple_clpc_sample(&c); assert(gpu.level==2);
 gpu.busy=100; jiffies=4699; apple_clpc_sample(&c); assert(published==0x04 && gpu.level==2);
 jiffies=4700; apple_clpc_sample(&c); assert(published==0x01 && gpu.level==0);
 gpu.active=false; jiffies=4800; apple_clpc_sample(&c); assert(published==0 && queued_idle);
 // Without a non-zero top DCS state, even a low reported share keeps the map top.
 no_top.gpu_busy[APPLE_CLPC_DCS][APPLE_CLPC_LEVELS - 1] = 0;
 c.hw = &no_top; gpu.active=true; gpu.busy_valid=true; gpu.busy=100; jiffies=4900;
 apple_clpc_sample(&c); assert(published==0x35 && c.last_reason==2);
 // Same for a static table with no DT properties at all.
 c.hw = &apple_clpc_hw_t6041; jiffies=4904;
 apple_clpc_sample(&c); assert(published==0x35 && c.last_reason==2);
 gpu.active=false; jiffies=5000; apple_clpc_sample(&c); assert(published==0 && queued_idle);
}

// End-to-end: feed g17 BusyShare's actual reports for a 500 ms compute job
// running beside a light compositor into CLPC's T6041 ladder. CLPC drops to
// DCS 1 / fabric 0 during light compositing, lifts IMMEDIATELY to DCS 5 /
// fabric 3 (0x35) at t=1810 ms while the compute job is still in flight, holds
// 0x35 while it runs, and drops back to 0x01 after it finishes + 400 ms hold.
static void check_t6041_long_in_flight(void) {
 struct apple_clpc_gpu gpu = {0};
 struct apple_clpc c = {.hw = &t6041_dt_hw, .has_pmc = true, .agent = 1, .mask = 3, .gpu = &gpu};
 read_error = vote_error = 0; pmgr_status = 0xf0; pmp_floor = 0; published = ~0u;
 gpu.active = true;
 // Initial burst starts at top (no share yet).
 jiffies = 1000; apple_clpc_sample(&c);
 assert(published == 0x35 && gpu.level == 3);
 // Light compositor windows (1200, 1400, 1600 ms -> 50 permille) drop to DCS 1 / fabric 0.
 assert(ARRAY_SIZE(g17_long_events) == 7);
 for (size_t i = 0; i < 3; i++) {
  gpu.busy = g17_long_events[i].permille; gpu.busy_valid = true;
  jiffies = g17_long_events[i].ms; apple_clpc_sample(&c);
 }
 // After 400 ms down-step hold + 32 ms fabric hold, CLPC is at DCS 1 / fabric 0.
 jiffies = 1640; apple_clpc_sample(&c);
 assert(published == 0x01 && gpu.level == 0);
 // At t=1810 ms (160 ms into the 500 ms compute job, still in flight!), BusyShare
 // reports 1000 permille and CLPC lifts IMMEDIATELY to DCS 5 / fabric 3.
 gpu.busy = g17_long_events[3].permille; gpu.busy_valid = true;
 jiffies = g17_long_events[3].ms; apple_clpc_sample(&c);
 assert(published == 0x35 && gpu.level == 3);
 // Still running at t=2010 ms: stays at DCS 5 / fabric 3.
 gpu.busy = g17_long_events[4].permille; gpu.busy_valid = true;
 jiffies = g17_long_events[4].ms; apple_clpc_sample(&c);
 assert(published == 0x35 && gpu.level == 3);
 // Retires at t=2150 ms; window at t=2210 ms reports exact 700 (still in 400 ms hold).
 gpu.busy = g17_long_events[5].permille; gpu.busy_valid = true;
 for (jiffies = g17_long_events[5].ms; jiffies < g17_long_events[6].ms; jiffies += 4) {
  apple_clpc_sample(&c);
  assert(published == 0x35 && gpu.level == 3);
 }
 // Next light window at t=2410 ms (400 ms after last top request at 2010 ms):
 // DCS drops immediately to 1, fabric keeps HOLD_MS (32 ms) from the 2406 ms sample.
 gpu.busy = g17_long_events[6].permille; gpu.busy_valid = true;
 jiffies = g17_long_events[6].ms; apple_clpc_sample(&c);
 assert(published == 0x31 && gpu.level == 0);
 jiffies = g17_long_events[6].ms + APPLE_CLPC_HOLD_MS; apple_clpc_sample(&c);
 assert(published == 0x01 && gpu.level == 0);
}

// End-to-end: g17 BusyShare's samples (-1: no report) for steady 600 permille
// work, a genuine idle gap in which the GPU clears its activity and busy share
// (via apple_clpc_gpu_clear_busy), and a new burst 600 ms later. The new burst starts
// at the top (0x35) and stays there until a fresh 200 ms window reports; the
// diluted 85 permille a stale window would report drops it to 0x01 at once.
static void check_t6041_idle_burst(void) {
 struct apple_clpc_gpu gpu = {0};
 struct apple_clpc c = {.hw = &t6041_dt_hw, .has_pmc = true, .agent = 1, .mask = 3, .gpu = &gpu};
 size_t i = 0, n = ARRAY_SIZE(g17_idle_events);
 read_error = vote_error = 0; pmgr_status = 0xf0; pmp_floor = 0; published = ~0u;
 assert(n == 8 && g17_idle_events[0].ms == 1100 && g17_idle_events[2].ms == 2000);
 gpu.active = true; jiffies = 900; apple_clpc_sample(&c);
 assert(published == 0x35 && gpu.level == 3);
 for (; i < 2; i++) {
  assert(g17_idle_events[i].permille == 600);
  gpu.busy = g17_idle_events[i].permille; gpu.busy_valid = true;
  jiffies = g17_idle_events[i].ms; apple_clpc_sample(&c);
 }
 assert(gpu.level == 2);
 // Idle: after genuine idle (>= BUSY_IDLE_RESET_NS), g17 clears both activity and busy.
 apple_clpc_gpu_set_active(&gpu, false);
 apple_clpc_gpu_clear_busy(&gpu);
 for (jiffies = 1416; jiffies < 2000; jiffies += 4)
  apple_clpc_sample(&c);
 assert(published == 0);
 apple_clpc_gpu_set_active(&gpu, true);
 for (jiffies = 2000; jiffies <= 2200; jiffies += 4) {
  if (i < n && g17_idle_events[i].ms == jiffies) {
   if (g17_idle_events[i].permille >= 0)
    apple_clpc_gpu_set_busy(&gpu, g17_idle_events[i].permille);
   i++;
  }
  assert(gpu.busy_valid == (jiffies >= 2200));
  apple_clpc_sample(&c);
  assert(published == 0x35 && gpu.level == 3);
 }
 assert(i == n);
 // The stale window's diluted share would have dropped the new burst at once.
 apple_clpc_gpu_set_active(&gpu, false); apple_clpc_gpu_clear_busy(&gpu);
 jiffies = 2400; apple_clpc_sample(&c);
 apple_clpc_gpu_set_active(&gpu, true); apple_clpc_gpu_set_busy(&gpu, 85);
 jiffies = 3000; apple_clpc_sample(&c); assert(published == 0x01 && gpu.level == 0);
 apple_clpc_gpu_set_active(&gpu, false); jiffies = 3100; apple_clpc_sample(&c); assert(published == 0);
}

// End-to-end: paced 24 fps light work (42 ms period). Every frame clears activity
// after 16 ms (Effort::idle) and resumes 26 ms later (inside CLPC's 32 ms vote
// hold). Keeping busy_valid across set_active(false) lets g17's 200 ms window
// drop CLPC from 0x35 (level 3) to 0x01 (level 0) after APPLE_CLPC_GPU_DOWN_MS.
static void check_t6041_paced_24fps(void) {
 struct apple_clpc_gpu gpu = {0};
 struct apple_clpc c = {.hw = &t6041_dt_hw, .has_pmc = true, .agent = 1, .mask = 3, .gpu = &gpu};
 size_t n = ARRAY_SIZE(g17_paced_events);
 read_error = vote_error = 0; pmgr_status = 0xf0; pmp_floor = 0; published = ~0u;
 assert(n == 18 && g17_paced_events[0].ms == 1000);
 for (size_t i = 0; i < n; i++) {
  unsigned long start = g17_paced_events[i].ms;
  unsigned long end = (i + 1 < n) ? g17_paced_events[i + 1].ms : start + 42;
  apple_clpc_gpu_set_active(&gpu, true);
  if (g17_paced_events[i].permille >= 0)
   apple_clpc_gpu_set_busy(&gpu, g17_paced_events[i].permille);
  for (jiffies = start; jiffies < end; jiffies += 4) {
   if (jiffies >= start + 16)
    apple_clpc_gpu_set_active(&gpu, false);
   apple_clpc_sample(&c);
  }
 }
 // By frame 17 (~1714..1756 ms), 200 ms window + 400 ms down-step + 32 ms fabric hold
 // have all elapsed: floor must have dropped to DCS 1 / fabric 0 (0x01).
 assert(gpu.busy_valid && gpu.level == 0 && published == 0x01);
}

// T8132's G16 firmware keeps graphics powered while the GPU idles: the
// M5/F3 floor follows the work the GPU driver reports, as on T6040.
static void check_t8132(void) {
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t8132,.has_pmc=true,.agent=1,.mask=3,.gpu=&gpu};
 assert(!c.hw->gpu_demand);
 read_error=vote_error=0; pmgr_status=0xf0; pmp_floor=0; jiffies=0; published=~0u;
 apple_clpc_sample(&c); assert(published==0 && queued_idle && c.last_reason==0);
 gpu.active=true; jiffies=100;
 apple_clpc_sample(&c); assert(published==0x35 && c.last_reason==2);
 gpu.active=false; jiffies=131; apple_clpc_sample(&c);
 assert(published==0x35 && c.last_reason==16);
 jiffies=132; apple_clpc_sample(&c); assert(published==0 && queued_idle);
}

// T6041 (M4 Max) votes DCS 5 / fabric 3 for CPU and host GPU demand without a
// streak, samples on time while idle, preserves T6041's DCS 5 / fabric 2 ANE
// floor, and carries the 2808 MHz Low Power ceiling.
static void check_t6041(const struct apple_clpc_hw *hw) {
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc_ane ane={0};
 struct apple_clpc c={.hw=hw,.has_pmc=true,.agent=1,.mask=3,.gpu=&gpu,.ane=&ane};
 c.ane_states[APPLE_CLPC_DCS]=c.hw->ane[APPLE_CLPC_DCS];
 c.ane_states[APPLE_CLPC_SOC]=c.hw->ane[APPLE_CLPC_SOC];
 assert(!c.hw->gpu_demand && c.hw->gpu_input && c.hw->sustain==0 && c.hw->idle_on_time);
 assert(!apple_clpc_hw_t8132.idle_on_time && !apple_clpc_hw_t8140.idle_on_time && !apple_clpc_hw_t6040.idle_on_time);
 assert(c.hw->low_power_khz==2808000 && apple_clpc_hw_t6040.low_power_khz==0);
 read_error=vote_error=0; pmgr_status=0xf0; pmp_floor=0; jiffies=3000; published=~0u; qos_adds=0;
 apple_clpc_sample(&c); assert(published==0 && queued_idle && c.last_reason==0);
 gpu.active=true; jiffies=3004; apple_clpc_sample(&c);
 assert(published==0x35 && c.last_reason==2);
 gpu.active=false; jiffies=3035; apple_clpc_sample(&c);
 assert(published==0x35 && c.last_reason==16);
 jiffies=3036; apple_clpc_sample(&c);
 assert(published==0 && queued_idle);
 ane.active=true; jiffies=3040; apple_clpc_sample(&c);
 assert(published==0x25 && c.last_reason==32 && qos_adds==0);
 gpu.active=true; jiffies=3044; apple_clpc_sample(&c);
 assert(published==0x35 && c.last_reason==34);
 gpu.active=false; jiffies=3048; apple_clpc_sample(&c);
 assert(published==0x25 && c.last_reason==32);
 ane.active=false; jiffies=3080; apple_clpc_sample(&c);
 assert(published==0 && queued_idle);
}

// A failed power-state read still protects an active graphics consumer.
static void check_read_error(void) {
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.has_pmc=true,.agent=1,.mask=3,.gpu_pmgr=(void *)1};
 read_error=vote_error=0; pmp_floor=4; pmgr_status=0xf0; warned=0;
 for (jiffies=100; jiffies<=164; jiffies+=4) apple_clpc_sample(&c);
 assert(published==0x34 && c.last_reason==4);
 jiffies=168; pmgr_status=0; read_error=-EIO;
 apple_clpc_sample(&c); assert(published==0x34 && c.last_reason==4);
 // The failure is counted for debugfs and warned about once.
 assert(c.gpu_read_errors==1 && warned==1);
 jiffies=172; read_error=0; apple_clpc_sample(&c);
 assert(published==0x34 && c.last_reason==16);
 jiffies=204; apple_clpc_sample(&c); assert(published==0);
 assert(c.gpu_read_errors==1);
}

static void check_t8140(void) {
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.has_pmc=true,.agent=1,.mask=3,.gpu_pmgr=(void *)1};
 read_error=vote_error=0; pmp_floor=4; pmgr_status=0xf0;
 for (jiffies=100; jiffies<=164; jiffies+=4) {
  apple_clpc_sample(&c);
  assert(published==(jiffies<164 ? 0 : 0x34));
 }
 // The PMP's lower DCS vote suppresses the GPU floor during heavy work.
 pmp_floor=3;
 for (jiffies=168; jiffies<=204; jiffies+=4) apple_clpc_sample(&c);
 assert(published==0 && c.gpu_limited && !queued_idle);
 assert(c.gpu_floor_releases==1 && c.gpu_floor_returns==0);
 pmp_floor=4;
 for (jiffies=208; jiffies<=304; jiffies+=4) apple_clpc_sample(&c);
 assert(published==0x34 && c.last_reason==4);
 assert(c.gpu_floor_releases==1 && c.gpu_floor_returns==1);
 assert(c.gpu_streaming_samples && !c.gpu_dense_samples);
 jiffies=308; pmgr_status=0; apple_clpc_sample(&c);
 jiffies=340; apple_clpc_sample(&c); assert(published==0 && queued_idle);
}
// Dense light work: graphics powers off between submissions, so it never
// streams, but 2+ submissions in each of 4 periods earn the GPU floor.
static void check_t8140_dense(void) {
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.has_pmc=true,.agent=1,.mask=3,.gpu_pmgr=(void *)1,.gpu=&gpu};
 read_error=vote_error=0; pmp_floor=4; pmgr_status=0; published=~0u;
 jiffies=100; apple_clpc_sample(&c); assert(published==0 && queued_idle);
 for (jiffies=104; jiffies<=116; jiffies+=4) {
  gpu.sequence+=2; apple_clpc_sample(&c);
  assert(published==(jiffies<116 ? 0 : 0x34));
 }
 assert(c.last_reason==8 && c.gpu_dense_samples==1 && !c.gpu_streaming_samples);
 // One sparse period ends the streak; the vote holds, then drops.
 jiffies=120; gpu.sequence+=1; apple_clpc_sample(&c);
 assert(published==0x34 && c.last_reason==16);
 jiffies=148; apple_clpc_sample(&c); assert(published==0 && queued_idle);
 // A sample more than one period late does not count as a dense period.
 for (jiffies=152; jiffies<=164; jiffies+=4) { gpu.sequence+=2; apple_clpc_sample(&c); }
 assert(published==0x34);
 jiffies=176; gpu.sequence+=40; apple_clpc_sample(&c);
 assert(c.last_reason==16);
}

// Low power samples on time only when a package power target (low_power_mw)
// is active; a static ceiling alone or High Power-only profile stays
// deferrable when idle.
static void check_low_power(void) {
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc_hw lp=apple_clpc_hw_t6040;
 struct apple_clpc c={.hw=&lp,.has_pmc=true,.agent=1,.mask=3,.gpu=&gpu,.low_power=true};
 read_error=vote_error=0; pmgr_status=0xf0; published=~0u;
 jiffies=3000; apple_clpc_sample(&c); assert(published==0 && queued_idle);
 c.low_power_khz=2808000;
 jiffies=3004; apple_clpc_sample(&c); assert(published==0 && queued_idle);
 c.low_power_mw=15000;
 jiffies=3008; apple_clpc_sample(&c); assert(published==0 && !queued_idle);
 c.low_power=false;
 jiffies=3012; apple_clpc_sample(&c); assert(published==0 && queued_idle);
}

int main(void) {
 // T6040's supplier is always powered: it must NEVER substitute for work.
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t6040,.has_pmc=true,.agent=1,.mask=3,.gpu=&gpu};
 assert(!c.hw->gpu_demand);
 jiffies=0; read_error=vote_error=0; pmgr_status=0xf0; published=~0u;
 apple_clpc_sample(&c); assert(published==0 && queued_idle && c.last_reason==0);
 gpu.active=true; jiffies=100; apple_clpc_sample(&c);
 assert(published==0x25 && c.last_reason==2 && c.last_floor==0x25);
 assert(c.last_vote_ns==100000000ull);
 // Core/firmware supplier is still powered, but continuous idle ends demand.
 gpu.active=false; jiffies=131; apple_clpc_sample(&c);
 assert(published==0x25 && c.last_reason==16);
 jiffies=132; apple_clpc_sample(&c);
 assert(published==0 && queued_idle && c.last_reason==0);
 // A new submission reasserts demand; independent CPU requests survive release.
 c.nr_clusters=1; c.clusters[0].request[0]=2; c.clusters[0].request[1]=1;
 gpu.active=true; jiffies=200; apple_clpc_sample(&c);
 assert(published==0x25 && c.last_reason==3);
 gpu.active=false; jiffies=232; apple_clpc_sample(&c);
 assert(published==0x12 && c.last_reason==1);
 c.clusters[0].request[0]=c.clusters[0].request[1]=0;
 jiffies=264; apple_clpc_sample(&c); assert(published==0);
 t6041_from_dt();
 check_t6041_busy();
 t8132_from_dt();
 check_t8132_busy();
 check_t6041_long_in_flight();
 check_t6041_idle_burst();
 check_t6041_paced_24fps();
 check_t8132();
 check_t6041(&apple_clpc_hw_t6041);
 check_t6041(&t6041_dt_hw);
 check_read_error();
 check_t8140();
 check_t8140_dense();
 check_t8140_ane();
 check_t6050_ane();
 check_low_power();
 check_t6040_ane();
 puts("PASS: T6040, T6041 and T8132 host demand; DT GPU busy share and down-step hold; T8140/T604x ANE, streaming, dense work, read errors and PMP backoff; idle release");
}
'''
    code = code.replace('#define APPLE_PMC_RAIL_SHIFT(r) ((r)*8)', rail_shift)
    with tempfile.TemporaryDirectory(prefix='apple-clpc-gpu-') as tmp:
        root = Path(tmp)
        for name, text in [
            ('fixed', code),
            ('omit-host-demand', code.replace('if (apple_clpc_gpu_input_demand(clpc->hw, clpc->gpu, demand))',
                                              'if (false && apple_clpc_gpu_input_demand(clpc->hw, clpc->gpu, demand))')),            ('omit-ane-demand', code.replace('if (apple_clpc_ane_input_demand(clpc, demand))',
                                             'if (false && apple_clpc_ane_input_demand(clpc, demand))')),
            ('omit-ane-cluster', code.replace('apple_clpc_ane_cluster(clpc, reason & APPLE_CLPC_REASON_ANE, now);',
                                              '(void)apple_clpc_ane_cluster;')),
            ('ane-cluster-no-hold', code.replace('if (time_before(now, clpc->ane_until))',
                                                 'if (active)')),
            ('omit-streaming-demand', code.replace('bool streaming = apple_clpc_gpu_streaming(clpc, now);',
                                                   'bool streaming = (apple_clpc_gpu_streaming(clpc, now), false);')),
            ('omit-dense-demand', code.replace('if (streaming || dense) {', 'if (streaming || (dense && false)) {')),
            ('dense-ignores-late', code.replace('if (!threshold || late || submissions < threshold)',
                                                'if (!threshold || (late && false) || submissions < threshold)')),
            ('uncounted-floor-return', code.replace('clpc->gpu_floor_returns++;', 'clpc->gpu_floor_returns += 0;')),
            ('uncounted-read-error', code.replace('clpc->gpu_read_errors++;', 'clpc->gpu_read_errors += 0;')),
            ('omit-pmp-backoff', code.replace('bool released = apple_clpc_gpu_limited(clpc, now);',
                                              'bool released = (apple_clpc_gpu_limited(clpc, now), false);')),
            ('release-too-early', code.replace('msecs_to_jiffies(APPLE_CLPC_HOLD_MS)', '0')),
            ('gpu-down-at-once', code.replace('msecs_to_jiffies(APPLE_CLPC_GPU_DOWN_MS)', '0')),
            ('gpu-busy-ignored', code.replace('if (!gpu->busy_valid || gpu->busy >= hw->gpu_busy_top)',
                                              'if (true || gpu->busy >= hw->gpu_busy_top)')),
            ('gpu-no-share-uses-table', code.replace('if (!gpu->busy_valid || gpu->busy >= hw->gpu_busy_top)',
                                                     'if (gpu->busy >= hw->gpu_busy_top)')),
            ('clpc-set-active-clears-busy', code.replace('\tgpu->active = active;\n',
                                                         '\tgpu->active = active;\n\tif (!active)\n\t\tgpu->busy_valid = false;\n')),
            ('gpu-busy-without-dt', code.replace('bool has_busy = hw->gpu_busy[APPLE_CLPC_DCS][APPLE_CLPC_LEVELS - 1];',
                                                 'bool has_busy = true;')),
            ('retain-at-idle', code.replace('vote->state = 0;', '')),
            ('dt-state-unchecked', code.replace('if (state > top || state > 15)', 'if ((void)top, false)')),
            ('dt-rail-bound-unchecked', code.replace('if (state > top || state > 15)', 'if ((void)top, state > 15)')),
            ('dt-gpu-rail-max-ignored', code.replace('max(hw->map[rail][APPLE_CLPC_LEVELS - 1],\n\t\t\t      hw->gpu[rail])',
                                                     'hw->map[rail][APPLE_CLPC_LEVELS - 1]')),
            ('dt-fallback-keeps-partial', code.replace('\t\t*out = *soc;\n\t\treturn false;', '\t\treturn false;')),
            ('dt-fallback-silent', code.replace('dev_warn(dev,\n\t\t\t "ignoring invalid GPU busy-share states in the device tree (%d)\\n",\n\t\t\t err);',
                                                '(void)dev; (void)err;')),
            ('dt-busy-unordered', code.replace('cells[0] > cells[1] || ', '')),
        ]:
            # A mutation that no longer matches the driver tests nothing.
            assert name == 'fixed' or text != code, 'stale mutation: ' + name
            path = root / (name + '.c')
            path.write_text(text)
            binary = path.with_suffix('')
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Werror', str(path), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'fixed':
                assert run.returncode == 0, run.stderr
                print(run.stdout.strip())
            else:
                assert run.returncode != 0, name
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
