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


def main():
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
    t6041_dt = (
        'static const u32 t6041_gpu_states[] = {' + ', '.join(str(x) for x in gpu_states) + '};\n'
        'static const u32 t6041_gpu_busy[] = {' + ', '.join(str(x) for x in gpu_busy) + '};\n')
    methods = '\n'.join(item(source, s) for s in [
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
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h,l) (((1u << ((h)-(l)+1))-1) << (l))
#define BIT(n) (1u << (n))
#define APPLE_PMC_RAIL_SHIFT(r) ((r)*8)
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
#define time_after(a,b) ((long)((a)-(b)) > 0)
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define msecs_to_jiffies(ms) (ms)
static int warned, dt_warned;
#define dev_warn_once(d, ...) ((void)(d), warned++)
#define dev_warn(d, ...) ((void)(d), dt_warned++)
enum apple_clpc_rail { APPLE_CLPC_DCS, APPLE_CLPC_SOC, APPLE_CLPC_RAILS };
''' + types + r'''
struct apple_clpc_gpu {
 int lock; bool active; u64 sequence;
 unsigned int busy; bool busy_valid; unsigned int level; unsigned long level_since;
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
 bool low_power;
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
  struct apple_clpc c = {.hw = &out, .agent = 1, .mask = 3, .gpu = &g};
  read_error = vote_error = 0; pmgr_status = 0xf0; pmp_floor = 0; jiffies = 2900; published = ~0u;
  apple_clpc_sample(&c);
  assert(published == 0x35 && c.last_reason == 2);
 }

 assert(apple_clpc_dt_busy(&bad, busy, 3) == -EINVAL);
 busy[0] = 160; busy[1] = 500; busy[2] = 1001;
 assert(apple_clpc_dt_busy(&bad, busy, 3) == -EINVAL);
 assert(apple_clpc_dt_busy(&bad, busy, 2) == -EINVAL);
}
// T8140 ANE work holds its DRAM and fabric states while it runs, a request
// shorter than one sample is still seen once, and the hold then expires.
static void check_t8140_ane(void) {
 struct apple_clpc_ane ane={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.agent=1,.mask=3,.ane=&ane};
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

// T6040 ANE work also holds the second P cluster at its qualified frequency,
// added on first use, for the vote hold after the last request, then drops it.
static void check_t6040_ane(void) {
 struct apple_clpc_ane ane={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t6040,.agent=1,.mask=3,.ane=&ane};
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
 struct apple_clpc c={.hw=&t6041_dt_hw,.agent=1,.mask=3,.gpu=&gpu};
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

// T8132's G16 firmware keeps graphics powered while the GPU idles: the
// M5/F3 floor follows the work the GPU driver reports, as on T6040.
static void check_t8132(void) {
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t8132,.agent=1,.mask=3,.gpu=&gpu};
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
 struct apple_clpc c={.hw=hw,.agent=1,.mask=3,.gpu=&gpu,.ane=&ane};
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
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.agent=1,.mask=3,.gpu_pmgr=(void *)1};
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
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.agent=1,.mask=3,.gpu_pmgr=(void *)1};
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
 struct apple_clpc c={.hw=&apple_clpc_hw_t8140,.agent=1,.mask=3,.gpu_pmgr=(void *)1,.gpu=&gpu};
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

int main(void) {
 // T6040's supplier is always powered: it must NEVER substitute for work.
 struct apple_clpc_gpu gpu={0};
 struct apple_clpc c={.hw=&apple_clpc_hw_t6040,.agent=1,.mask=3,.gpu=&gpu};
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
 check_t8132();
 check_t6041(&apple_clpc_hw_t6041);
 check_t6041(&t6041_dt_hw);
 check_read_error();
 check_t8140();
 check_t8140_dense();
 check_t8140_ane();
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
