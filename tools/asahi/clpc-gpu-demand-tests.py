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
    types = '\n'.join(item(source, s) + ';' for s in [
        'struct apple_clpc_hw {', 'struct apple_clpc_vote {'])
    tables = '\n'.join(item(source, 'static const struct apple_clpc_hw ' + n) + ';'
                       for n in ['apple_clpc_hw_t8132', 'apple_clpc_hw_t8140', 'apple_clpc_hw_t6040'])
    methods = '\n'.join(item(source, s) for s in [
        'static unsigned int apple_clpc_vote(', 'static u32 apple_clpc_floor(',
        'static bool apple_clpc_gpu_streaming(',
        'static bool apple_clpc_gpu_dense_step(',
        'static bool apple_clpc_gpu_dense(',
        'static bool apple_clpc_gpu_limited(',
        'static void apple_clpc_gpu_floor_count(',
        'static void apple_clpc_sample('])
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define APPLE_CLPC_LEVELS 4
#define APPLE_CLPC_HOLD_MS 32
#define APPLE_CLPC_SAMPLE_MS 4
#define APPLE_CLPC_GPU_BACKOFF_MS 100
#define HZ 1000
#define max(a,b) ((a) > (b) ? (a) : (b))
#define GENMASK(h,l) (((1u << ((h)-(l)+1))-1) << (l))
#define APPLE_PMC_RAIL_SHIFT(r) ((r)*8)
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
#define time_after(a,b) ((long)((a)-(b)) > 0)
#define time_before(a,b) ((long)((a)-(b)) < 0)
#define msecs_to_jiffies(ms) (ms)
static int warned;
#define dev_warn_once(d, ...) ((void)(d), warned++)
enum apple_clpc_rail { APPLE_CLPC_DCS, APPLE_CLPC_SOC, APPLE_CLPC_RAILS };
''' + types + r'''
struct apple_clpc_gpu { bool active; u64 sequence; };
struct apple_clpc_cluster { unsigned int request[APPLE_CLPC_RAILS]; };
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
''' + dense_samples + '\n' + tables + '\n' + methods + r'''
static void check_t8132(void) {
 struct apple_clpc c={.hw=&apple_clpc_hw_t8132,.agent=1,.mask=3,.gpu_pmgr=(void *)1};
 read_error=vote_error=0; pmgr_status=0; pmp_floor=0; jiffies=0;
 apple_clpc_sample(&c); assert(published==0 && queued_idle);
 jiffies=100; pmgr_status=0xf0;
 apple_clpc_sample(&c); assert(published==0x35 && c.last_reason==4);
 jiffies=131; pmgr_status=0; apple_clpc_sample(&c);
 assert(published==0x35 && c.last_reason==16);
 jiffies=132; apple_clpc_sample(&c); assert(published==0 && queued_idle);
 // A failed power-state read still protects an active graphics consumer.
 jiffies=200; pmgr_status=0xe0; read_error=-EIO; warned=0;
 apple_clpc_sample(&c); assert(published==0x35);
 // The failure is counted for debugfs and warned about once.
 assert(c.gpu_read_errors==1 && warned==1);
 jiffies=232; read_error=0; apple_clpc_sample(&c); assert(published==0);
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
 check_t8132();
 check_t8140();
 check_t8140_dense();
 puts("PASS: T6040 host demand; T8132 powered floor; T8140 streaming, dense work and PMP backoff; idle release");
}
'''
    code = code.replace('#define APPLE_PMC_RAIL_SHIFT(r) ((r)*8)', rail_shift)
    with tempfile.TemporaryDirectory(prefix='apple-clpc-gpu-') as tmp:
        root = Path(tmp)
        for name, text in [
            ('fixed', code),
            ('omit-host-demand', code.replace('if (clpc->gpu && !clpc->hw->gpu_demand) {', 'if (false) {')),
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
            ('retain-at-idle', code.replace('vote->state = 0;', '')),
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
