#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""J613 CPU memory floor: governor and admission logic on the host.

Compiles drivers/pmdomain/apple/pmp-j613-floor-gov.h (the production header,
unmodified unless --mutant names a deliberate defect) into a harness that
drives it with synthetic time, CPU/GPU load traces and PMP tables. Also checks
the driver source statically: one read of the request word, every store
through the single update helper, and floor 0 on every exit path.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HDR = ROOT / 'drivers/pmdomain/apple/pmp-j613-floor-gov.h'
SRC = ROOT / 'drivers/pmdomain/apple/pmp-report.c'

MUTANTS = {
    'no-board': ('return j613 && native_default && allowed;', 'return (void)j613, native_default && allowed;'),
    'no-default': ('return j613 && native_default && allowed;', 'return (void)native_default, j613 && allowed;'),
    'no-disable': ('return j613 && native_default && allowed;', 'return (void)allowed, j613 && native_default;'),
    # Drop as soon as demand stops, ignoring the hold time.
    'no-hold': ('now - g->last_demand_ns >= c->hold_ms * PMP_FLOOR_NS_PER_MS', '1'),
    # Never defer a transition.
    'no-rate': ('if (!pmp_floor_rate_ok(g, c, now)) {', 'if (0) {'),
    # Raise on the first demand sample.
    'no-streak': ('g->streak >= (c->raise_samples ? c->raise_samples : 1)', '1'),
    # Lose the valid bit.
    'bad-word': ('return PMP_FLOOR_VALID | (unsigned long long)(dcs & 0xf)', 'return (unsigned long long)(dcs & 0xf)'),
    # Accept a foreign producer's request.
    'foreign-ok': ('return word == 0 || word == PMP_FLOOR_VALID;', 'return (void)word, 1;'),
    # Ignore the cluster sum.
    'no-sum': ('(c->cpu_sum_pct && cluster_sum_pct >= c->cpu_sum_pct)', '(cluster_sum_pct && 0)'),
    # Skip the CPU-client name check.
    'no-names': ('return "CPU clients EACC0/PACC0 not named once";', '(void)0;'),
}

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BIT_ULL(x) (1ULL << (x))
#include "gov.h"

#define CHECK(c) do { if (!(c)) { printf("Assertion failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
#define MS 1000000ULL

static struct pmp_floor_gov_cfg cfg(void)
{
	struct pmp_floor_gov_cfg c = { .cpu_busy_pct = 80, .cpu_sum_pct = 90, .gpu_busy_pct = 40,
		.raise_samples = 2, .hold_ms = 500, .min_interval_ms = 100 };
	return c;
}

static int words(void)
{
	/* Only native J613 is enabled, and the parameter can always veto it. */
	for (unsigned int board = 0; board <= 1; board++)
		for (unsigned int native = 0; native <= 1; native++)
			for (unsigned int allowed = 0; allowed <= 1; allowed++)
				CHECK(pmp_floor_enabled(board, native, allowed) ==
				      (bool)(board && native && allowed));
	CHECK(pmp_floor_word(4, 0) == 0x2004000000000000ULL);
	CHECK(pmp_floor_word(0, 0) == 0x2000000000000000ULL);
	CHECK(pmp_floor_word(4, 2) == 0x2004000200000000ULL);
	CHECK(pmp_floor_word(0, 0) == PMP_FLOOR_VALID);
	CHECK(pmp_floor_initial_ok(0) && pmp_floor_initial_ok(PMP_FLOOR_VALID));
	CHECK(!pmp_floor_initial_ok(pmp_floor_word(4, 0)) && !pmp_floor_initial_ok(1));
	CHECK(pmp_floor_clamp(9, 1, 4) == 4 && pmp_floor_clamp(0, 1, 4) == 1 && pmp_floor_clamp(3, 0, 2) == 2);
	/* Measured J613 responses: 0x3204 held (DCS4 SOC0), 0x3200 released. */
	CHECK(pmp_floor_raise_confirmed(0x3204, 4, 0) && !pmp_floor_raise_confirmed(0x3200, 4, 0));
	CHECK(!pmp_floor_raise_confirmed(0x3204, 4, 1) && pmp_floor_raise_confirmed(0x3224, 4, 2));
	CHECK(pmp_floor_drop_confirmed(0x3200, 0x3204) && !pmp_floor_drop_confirmed(0x3204, 0x3204));
	return 0;
}

static int busy(void)
{
	struct pmp_floor_gov_cfg c = cfg();

	/* One thread split 50/45 over two CPUs of a cluster: the sum is demand. */
	CHECK(!pmp_floor_cpu_demand(&c, 50, 0) && pmp_floor_cpu_demand(&c, 50, 95));
	CHECK(pmp_floor_cpu_demand(&c, 80, 0) && !pmp_floor_cpu_demand(&c, 79, 89));
	/* Light work over four CPUs (4 x 20 %) is not. */
	CHECK(!pmp_floor_cpu_demand(&c, 20, 80));
	c.cpu_sum_pct = 0;
	CHECK(!pmp_floor_cpu_demand(&c, 50, 400));
	CHECK(pmp_floor_busy_pct(0, 0) == 0 && pmp_floor_busy_pct(5, 10) == 50);
	CHECK(pmp_floor_busy_pct(20, 10) == 100);
	CHECK(pmp_floor_cpu_pct(100, 1000, 106, 1030) == 80);	/* 24/30 busy */
	CHECK(pmp_floor_cpu_pct(100, 1000, 130, 1030) == 0);	/* idle */
	CHECK(pmp_floor_cpu_pct(100, 1000, 140, 1030) == 0);	/* idle > wall */
	CHECK(pmp_floor_cpu_pct(100, 1000, 90, 1030) == 0);	/* idle went back */
	CHECK(pmp_floor_cpu_pct(100, 1000, 100, 1000) == 0);	/* no time */
	return 0;
}

/* Run @n samples of @period ms with @demand; returns the count of each action. */
static void run(struct pmp_floor_gov *g, struct pmp_floor_gov_cfg *c, unsigned long long *t,
		unsigned int n, unsigned int period, bool demand, unsigned int *raise, unsigned int *drop)
{
	while (n--) {
		*t += period * MS;
		switch (pmp_floor_gov_step(g, c, *t, demand)) {
		case PMP_FLOOR_RAISE: (*raise)++; break;
		case PMP_FLOOR_DROP: (*drop)++; break;
		default: break;
		}
	}
}

static int governor(void)
{
	struct pmp_floor_gov_cfg c = cfg();
	struct pmp_floor_gov g = { 0 };
	unsigned long long t = 0;
	unsigned int r = 0, d = 0, i;

	/* One demand sample is a burst, not demand. */
	run(&g, &c, &t, 1, 30, true, &r, &d);
	CHECK(!r && !g.raised);
	run(&g, &c, &t, 3, 30, false, &r, &d);
	CHECK(!r);
	/* Two consecutive samples raise, on the second. */
	t += 30 * MS; CHECK(pmp_floor_gov_step(&g, &c, t, true) == PMP_FLOOR_KEEP);
	t += 30 * MS; CHECK(pmp_floor_gov_step(&g, &c, t, true) == PMP_FLOOR_RAISE);
	CHECK(g.raised);
	/* Held under demand. */
	run(&g, &c, &t, 100, 30, true, &r, &d);
	CHECK(!d && g.raised);
	/* No drop before the hold time... */
	unsigned long long last = t;
	for (i = 0; i < 16; i++) {
		t += 30 * MS;
		CHECK(pmp_floor_gov_step(&g, &c, t, false) == PMP_FLOOR_KEEP);
	}
	CHECK(t - last == 480 * MS && g.raised);
	/* ...and the drop at the first sample past it. */
	t += 30 * MS; CHECK(pmp_floor_gov_step(&g, &c, t, false) == PMP_FLOOR_DROP);
	CHECK(!g.raised);
	/* Demand inside the hold window extends it. */
	struct pmp_floor_gov h = { 0 };
	t = 0;
	run(&h, &c, &t, 2, 30, true, &r, &d);
	CHECK(h.raised);
	run(&h, &c, &t, 10, 30, false, &r, &d);
	run(&h, &c, &t, 1, 30, true, &r, &d);
	run(&h, &c, &t, 16, 30, false, &r, &d);
	CHECK(h.raised);
	run(&h, &c, &t, 1, 30, false, &r, &d);
	CHECK(!h.raised);
	return 0;
}

static int rate_limit(void)
{
	struct pmp_floor_gov_cfg c = cfg();
	struct pmp_floor_gov g = { 0 };
	unsigned long long t = 0;
	unsigned int r = 0, d = 0;

	c.hold_ms = 100;
	c.min_interval_ms = 300;
	c.raise_samples = 1;
	run(&g, &c, &t, 1, 10, true, &r, &d);
	CHECK(r == 1);
	/* Hold passes at 100 ms, but the drop waits for min_interval (300 ms). */
	run(&g, &c, &t, 20, 10, false, &r, &d);
	CHECK(!d && g.raised && g.rate_limited);
	run(&g, &c, &t, 10, 10, false, &r, &d);
	CHECK(d == 1 && !g.raised);
	/* A new raise right after the drop is deferred too, then applied. */
	unsigned long long dropped = t;
	run(&g, &c, &t, 1, 10, true, &r, &d);
	CHECK(r == 1);
	while (!g.raised)
		run(&g, &c, &t, 1, 10, true, &r, &d);
	CHECK(r == 2 && t - dropped >= 300 * MS);
	/* Out-of-band release is immediate and resets the streak. */
	pmp_floor_gov_reset(&g, t);
	CHECK(!g.raised && !g.streak);
	return 0;
}

/*
 * Load traces at 30 ms sampling with default thresholds: a CPU STREAM run
 * raises within two samples and releases within hold + one sample; a
 * compositor-like GPU duty cycle (12%) never raises; a flapping input never
 * makes transitions closer than min_interval.
 */
static int traces(void)
{
	struct pmp_floor_gov_cfg c = cfg();
	struct pmp_floor_gov g = { 0 };
	unsigned long long t = 0, raised_at = 0, stop, last_tr = 0;
	unsigned int i, r = 0, d = 0, gpu_pct;
	bool have_tr = false;

	run(&g, &c, &t, 50, 30, false, &r, &d);
	for (i = 0; i < 500 && !g.raised; i++) {
		t += 30 * MS;
		if (pmp_floor_gov_step(&g, &c, t, pmp_floor_cpu_pct(0, 0, 0, 30000) >= c.cpu_busy_pct) == PMP_FLOOR_RAISE)
			raised_at = t;
	}
	CHECK(g.raised && raised_at == (50 + 2) * 30 * MS);
	run(&g, &c, &t, 500, 30, true, &r, &d);	/* 15 s of STREAM */
	stop = t;
	while (g.raised)
		run(&g, &c, &t, 1, 30, false, &r, &d);
	CHECK(t - stop <= (500 + 30) * MS);

	/* Compositor: 2 ms of GPU work per 16.7 ms frame. */
	struct pmp_floor_gov q = { 0 };
	t = 0; r = 0;
	gpu_pct = pmp_floor_busy_pct(2 * 2 * MS, 30 * MS);
	CHECK(gpu_pct < c.gpu_busy_pct);
	run(&q, &c, &t, 2000, 30, gpu_pct >= c.gpu_busy_pct, &r, &d);
	CHECK(!r && !q.raised);

	/* Flapping: demand for 2 samples, then 18 idle, for a minute. */
	struct pmp_floor_gov f = { 0 };
	t = 0; r = d = 0;
	c.hold_ms = 100;
	for (i = 0; i < 2000; i++) {
		enum pmp_floor_action a;

		t += 30 * MS;
		a = pmp_floor_gov_step(&f, &c, t, (i % 20) < 2);
		if (a != PMP_FLOOR_KEEP) {
			CHECK(!have_tr || t - last_tr >= c.min_interval_ms * MS);
			have_tr = true;
			last_tr = t;
			r += a == PMP_FLOOR_RAISE;
			d += a == PMP_FLOOR_DROP;
		}
	}
	CHECK(r == 100 && d == 100);
	return 0;
}

static void put32(unsigned char *p, unsigned int v)
{
	for (int i = 0; i < 4; i++)
		p[i] = v >> (8 * i);
}

/* A synthetic 20-record soc-device table and PTD range table, J613-shaped. */
#define RECORDS 20
static unsigned char dev[RECORDS * PMP_FLOOR_REC_SIZE], rng[6 * 32];

static void tables(void)
{
	static const unsigned int ids[RECORDS] = { 1, 2, 9, 10, 11, 12, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 18, 19, 20 };
	memset(dev, 0, sizeof(dev));
	memset(rng, 0, sizeof(rng));
	for (int i = 0; i < RECORDS; i++)
		put32(dev + i * PMP_FLOOR_REC_SIZE, ids[i]);
	memcpy(dev + 6 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME, "EACC0", 5);
	memcpy(dev + 7 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME, "PACC0", 5);
	memcpy(dev + 8 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME, "AGX", 3);
	/* PMP-STATUS, PS-REQ, PS-ACK, DVFS-STATE, SOC-DEV-DVFS, filler. */
	unsigned int r[6][3] = { { 1, 1, 1 }, { 10, 0x100, 8 }, { 11, 0x108, 8 }, { 3, 8, 4 }, { 12, 0x110, 16 }, { 40, 0x200, 4 } };
	const char *n[6] = { "PMP-STATUS", "SOC-DEV-PS-REQ", "SOC-DEV-PS-ACK", "DVFS-STATE", "SOC-DEV-DVFS", "OTHER" };
	for (int i = 0; i < 6; i++) {
		put32(rng + PMP_FLOOR_RANGE_SIZE * i, r[i][0]);
		put32(rng + PMP_FLOOR_RANGE_SIZE * i + PMP_FLOOR_RANGE_BASE, r[i][1]);
		put32(rng + PMP_FLOOR_RANGE_SIZE * i + PMP_FLOOR_RANGE_COUNT, r[i][2]);
		memcpy(rng + PMP_FLOOR_RANGE_SIZE * i + 16, n[i], strlen(n[i]));
	}
}

static struct pmp_floor_layout lay;

#define CHECKED() pmp_floor_tables_check(dev, sizeof(dev), rng, sizeof(rng), &lay)
#define REFUSED(why) do { CHECK(CHECKED() && !strcmp(CHECKED(), why)); tables(); } while (0)

static int admission(void)
{
	tables();
	CHECK(CHECKED() == NULL);
	/* Discovered from the tables, not assumed. */
	CHECK(lay.cpu_first == 0x110 && lay.state_first == 8);
	CHECK(pmp_floor_req_update(&lay) == 0x10888 && pmp_floor_req_read(&lay) == 0x1110);
	CHECK(pmp_floor_state_read(&lay, 0) == 0x80 && pmp_floor_state_read(&lay, 3) == 0xb0);
	CHECK(pmp_floor_layout_end(&lay) == 0x10890);
	/* Another table's bases move every address with them. */
	put32(rng + PMP_FLOOR_RANGE_SIZE * 4 + 4, 0x120); put32(rng + PMP_FLOOR_RANGE_SIZE * 3 + 4, 0x10);
	CHECK(CHECKED() == NULL && pmp_floor_req_update(&lay) == 0x10908 &&
	      pmp_floor_req_read(&lay) == 0x1210 && pmp_floor_state_read(&lay, 0) == 0x100);
	tables();
	/* Records may be in any order; only the names matter. */
	memcpy(dev + 7 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME, "\0\0\0\0\0", 5);
	memcpy(dev + 15 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME, "PACC0", 5);
	CHECK(CHECKED() == NULL);
	tables();
	CHECK(!strcmp(pmp_floor_tables_check(dev, sizeof(dev) - 1, rng, sizeof(rng), &lay), "soc-device table size"));
	CHECK(!strcmp(pmp_floor_tables_check(NULL, 0, rng, sizeof(rng), &lay), "soc-device table size"));
	dev[7 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME] = 'E'; REFUSED("CPU clients EACC0/PACC0 not named once");
	dev[7 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME + 5] = 'X'; REFUSED("CPU clients EACC0/PACC0 not named once");
	memcpy(dev + 9 * PMP_FLOOR_REC_SIZE + PMP_FLOOR_REC_NAME, "EACC0", 5); REFUSED("CPU clients EACC0/PACC0 not named once");
	put32(rng + PMP_FLOOR_RANGE_SIZE * 4 + 8, 1); REFUSED("SOC-DEV-DVFS range");
	rng[PMP_FLOOR_RANGE_SIZE * 4 + 16] = 'X'; REFUSED("SOC-DEV-DVFS range");
	put32(rng + PMP_FLOOR_RANGE_SIZE * 5, 12); REFUSED("SOC-DEV-DVFS range");	/* duplicate id */
	put32(rng + PMP_FLOOR_RANGE_SIZE * 4, 13); REFUSED("SOC-DEV-DVFS range");	/* missing */
	put32(rng + PMP_FLOOR_RANGE_SIZE * 3 + 8, 3); REFUSED("DVFS-STATE range");
	memcpy(rng + PMP_FLOOR_RANGE_SIZE * 3 + 16, "DVFS-STATX", 10); REFUSED("DVFS-STATE range");
	put32(rng + PMP_FLOOR_RANGE_SIZE * 4 + 4, 0xfffffff0); REFUSED("ptd-range base out of range");
	CHECK(!strcmp(pmp_floor_tables_check(dev, sizeof(dev), rng, sizeof(rng) - 1, &lay), "ptd-range table size"));
	CHECK(!strcmp(pmp_floor_tables_check(dev, sizeof(dev), NULL, 0, &lay), "ptd-range table size"));
	return 0;
}

int main(void)
{
	int (*t[])(void) = { words, busy, governor, rate_limit, traces, admission };
	for (unsigned int i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i]())
			return 1;
	printf("pmp-j613-floor: %zu groups passed\n", sizeof(t) / sizeof(t[0]));
	return 0;
}
'''


def block(text, start):
    i = text.index(start)
    j = text.index('\n}\n', i)
    return text[i:j + 3]


def static_checks():
    src = SRC.read_text()
    begin = src.index('J613 (T8122, 25G83 firmware) demand-driven CPU memory floor')
    end = src.index('static int apple_pmp_report_probe(struct platform_device *pdev)')
    floor = src[begin:end]
    req_reads = re.findall(r'readq\([^;]*pmp_floor_req_read\(', src)
    assert len(req_reads) == 1, f'request-word reads: {len(req_reads)}'
    assert not re.search(r'readq\([^;]*0x111[0-9a-f]', src), 'literal request-alias read'
    # Every store in the floor code goes through j613_floor_write().
    writes = re.findall(r'\bwrite[bwlq](?:_relaxed)?\(', floor)
    assert writes == ['writeq('], writes
    w = block(floor, 'static void j613_floor_write(')
    assert 'writeq(word, f->update);' in w and 'readq' not in w
    # The update alias is the PACC0 word only.
    assert floor.count('f->update = ') == 1
    assert 'f->update = rep->base + pmp_floor_req_update(&f->layout);' in floor
    # The request read happens before any write could have been issued: in admit,
    # which runs only before f->admitted is set.
    admit = block(floor, 'static int j613_floor_admit(')
    assert 'pmp_floor_req_read(&f->layout)' in admit
    assert 'j613_floor_write' not in admit
    # Every exit path publishes floor 0 through j613_floor_release().
    for fn, needle in [('static void j613_floor_stop(', 'j613_floor_release('),
                       ('static int apple_pmp_report_suspend(', 'j613_floor_release('),
                       ('static void j613_floor_fault(', 'j613_floor_release('),
                       ('static ssize_t enable_store(', 'j613_floor_release(')]:
        assert needle in block(floor, fn), fn
    rel = block(floor, 'static void j613_floor_release(')
    assert 'j613_floor_write(f, PMP_FLOOR_VALID);' in rel
    assert '.shutdown = apple_pmp_report_shutdown' in src
    assert 'devm_register_reboot_notifier' in floor and 'devm_add_action_or_reset(dev, j613_floor_remove' in floor
    assert 'pm_sleep_ptr(&apple_pmp_report_pm_ops)' in src
    # Only the native J613 match opts in; all other match data stays off.
    assert re.search(r'^static bool j613_floor = true;$', floor, re.M)
    native = block(src, 'static const struct apple_pmp_report_offsets apple_pmp_offsets_j613_25g83 =')
    assert '.j613_floor_default = true,' in native
    assert src.count('.j613_floor_default =') == 1
    probe = block(floor, 'static int j613_floor_probe(')
    assert 'pmp_floor_enabled(of_machine_is_compatible("apple,j613"),' in probe
    assert 'rep->offsets->j613_floor_default, j613_floor)' in probe
    assert 'module_param(j613_floor, bool, 0444);' in floor
    print('pmp-j613-floor: static source checks passed')



def sampling_checks():
    # Failure modes: absent/removed policy, leaked reference, mask use after
    # put, hotplug during iteration, stale utilization after CPU re-online.
    src = block(SRC.read_text(), 'static void j613_floor_sample_cpus(')
    harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define BIT_ULL(x) (1ULL << (x))
#define u64 uint64_t
#define NSEC_PER_USEC 1000
#define nr_cpu_ids 2
#define max(a,b) ((a) > (b) ? (a) : (b))
#define for_each_possible_cpu(cpu) for ((cpu)=0; (cpu)<2; (cpu)++)
#define for_each_online_cpu(cpu) for_each_possible_cpu(cpu) if (cpu_online(cpu))
static bool online[2] = {true, true}, locked, present = true, remove_on_get;
static int refs, gets, puts;
static unsigned int mask = 0;
struct cpufreq_policy { unsigned int *related_cpus; } policy = { &mask };
struct j613_floor { bool primed; u64 cpu_idle[2], cpu_wall[2]; unsigned int cluster_sum[2]; };
static void cpus_read_lock(void) { assert(!locked); locked = true; }
static void cpus_read_unlock(void) { assert(locked && !refs); locked = false; }
static bool cpu_online(unsigned int cpu) { assert(locked); return online[cpu]; }
static struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu)
{
    (void)cpu; assert(locked); gets++;
    if (!present) return 0;
    refs++;
    if (remove_on_get) present = false; /* Unpublish after referenced lookup. */
    return &policy;
}
static unsigned int cpumask_first(unsigned int *m)
{ assert(locked && refs == 1 && m == &mask); return *m; }
static void cpufreq_cpu_put(struct cpufreq_policy *p)
{ assert(p == &policy && refs == 1); refs--; puts++; }
static u64 get_cpu_idle_time(unsigned int cpu, u64 *wall, int io)
{ (void)cpu; (void)io; assert(locked); *wall = 1030; return 100; }
#include "gov.h"
""" + src + r"""
int main(void)
{
    struct j613_floor f = { .primed = true, .cpu_idle = {100,100}, .cpu_wall = {1000,1000} };
    unsigned int pct = 0, sum = 0;
    j613_floor_sample_cpus(&f, 30000000, &pct, &sum);
    assert(pct == 100 && sum == 200 && gets == 2 && puts == 2 && !refs);
    present = false; pct = sum = 0; f.cpu_wall[0] = f.cpu_wall[1] = 1000;
    j613_floor_sample_cpus(&f, 30000000, &pct, &sum);
    assert(pct == 100 && sum == 200 && puts == 2 && !refs);
    present = remove_on_get = true; pct = sum = 0; f.cpu_wall[0] = f.cpu_wall[1] = 1000;
    j613_floor_sample_cpus(&f, 30000000, &pct, &sum);
    assert(pct == 100 && sum == 200 && puts == 3 && !refs);
    online[1] = false; f.cpu_wall[1] = 1000;
    j613_floor_sample_cpus(&f, 30000000, &pct, &sum);
    assert(f.cpu_wall[1] == 0);
    online[1] = true; pct = sum = 0;
    j613_floor_sample_cpus(&f, 30000000, &pct, &sum);
    assert(pct == 0 && sum == 0 && f.cpu_wall[1] == 1030 && !locked && !refs);
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / 'gov.h').write_text(HDR.read_text())
        (d / 'sample.c').write_text(harness)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(d / 'sample.c'), '-o', str(d / 'sample')], check=True)
        subprocess.run([str(d / 'sample')], check=True)
    print('pmp-j613-floor: referenced sampling, removal and hotplug passed')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutant', choices=sorted(MUTANTS))
    ap.add_argument('--out')
    a = ap.parse_args()
    hdr = HDR.read_text()
    if a.mutant:
        old, new = MUTANTS[a.mutant]
        assert hdr.count(old) == 1, a.mutant
        hdr = hdr.replace(old, new)
    else:
        static_checks()
        sampling_checks()
    with tempfile.TemporaryDirectory(dir=a.out) as d:
        d = Path(d)
        (d / 'gov.h').write_text(hdr)
        (d / 'h.c').write_text(HARNESS)
        cc = os.environ.get('CC', 'cc')
        subprocess.run([cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-o', str(d / 'h'), str(d / 'h.c')], check=True)
        r = subprocess.run([str(d / 'h')])
        sys.exit(r.returncode)


if __name__ == '__main__':
    main()
