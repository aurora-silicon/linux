#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production DART paging attach against a modelled DART, and fail it part way.

The model records, for each of a device's stream maps, what the hardware
translates through: a domain's page tables, bypass, or nothing (blocked). The
attach is made to fail at a chosen stream map, the way a TLB invalidate that
times out fails it after the new tables are already installed. Whatever the
domain the device was on before (none, identity, blocked, paging, DCP), no
stream may be left on the failed domain's tables, since the IOMMU core may
free them as soon as the attach returns. The failed device's streams must
also leave the failed domain's stream maps, except a locked stream the attach
reached, whose firmware root the domain still has to unmap when it is freed.

--ref tests drivers/iommu/apple-dart.c of an older revision instead.
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = ('apple_dart_mod_streams', 'apple_dart_domain_add_streams',
             'apple_dart_attach_paging_undo', 'apple_dart_attach_paging_drop_streams',
             'apple_dart_attach_dev_paging_unguarded')
# Revisions before the undo have only the attach and the stream helpers.
OPTIONAL = {'apple_dart_attach_paging_undo', 'apple_dart_attach_paging_drop_streams'}

PRELUDE = r'''
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define MAX_DARTS_PER_DEVICE 2
#define DART_MAX_STREAMS 64
#define BITS_PER_LONG 64
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DECLARE_BITMAP(name, n) unsigned long name[BITS_TO_LONGS(n)]
#define BIT(n) (1UL << (n))
#define WARN_ON(x) ({ bool w_ = !!(x); w_; })
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
typedef struct { long counter; } atomic_long_t;
#define atomic_long_or(v, p) ((p)->counter |= (long)(v))
#define atomic_long_and(v, p) ((p)->counter &= (long)(v))
#define atomic_long_read(p) ((p)->counter)
#define spin_lock_irqsave(l, f) ((void)(l), (f) = 0)
#define spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
enum { IOMMU_DOMAIN_BLOCKED, IOMMU_DOMAIN_IDENTITY, IOMMU_DOMAIN_DMA };

struct device { int unused; };
struct iommu_domain { int type; const void *ops; };
struct apple_dart {
	int num_streams;
	unsigned long managed_sids;
	int managed_lock;
	bool managed_failed, locked, live_mtp, retained_mtp, supports_bypass;
	struct device *dev;
};
struct apple_dart_stream_map { struct apple_dart *dart; DECLARE_BITMAP(sidmap, DART_MAX_STREAMS); };
struct apple_dart_atomic_stream_map {
	struct apple_dart *dart;
	atomic_long_t sidmap[BITS_TO_LONGS(DART_MAX_STREAMS)];
};
struct apple_dart_master_cfg { struct apple_dart_stream_map stream_maps[MAX_DARTS_PER_DEVICE]; };
struct apple_dart_domain {
	void *pgtbl_ops;
	void *dcp;
	bool finalized;
	struct apple_dart_atomic_stream_map stream_maps[MAX_DARTS_PER_DEVICE];
	struct iommu_domain domain;
};
#define for_each_stream_map(i, base, stream_map)                               \
	for (i = 0, stream_map = &(base)->stream_maps[0];                      \
	     i < MAX_DARTS_PER_DEVICE && stream_map->dart;                     \
	     stream_map = &(base)->stream_maps[++i])

static struct apple_dart_domain *to_dart_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct apple_dart_domain, domain);
}

/* What each of the device's stream maps translates through. */
static const char BYPASS[] = "bypass", BLOCKED[] = "blocked", UNTOUCHED[] = "untouched";
static const void *state[MAX_DARTS_PER_DEVICE];
static bool quarantined[MAX_DARTS_PER_DEVICE];
static struct apple_dart_master_cfg cfg;
static struct apple_dart_domain *fail_domain[MAX_DARTS_PER_DEVICE];
static int finalize_ret, pm_count;

static int idx_of_map(struct apple_dart_stream_map *s) { return (int)(s - cfg.stream_maps); }
static int idx_of_dart(struct apple_dart *d)
{
	for (int i = 0; i < MAX_DARTS_PER_DEVICE; i++)
		if (cfg.stream_maps[i].dart == d)
			return i;
	return -1;
}
static void *dev_iommu_priv_get(struct device *dev) { (void)dev; return &cfg; }
static int pm_runtime_get_sync(struct device *dev) { (void)dev; pm_count++; return 0; }
static void pm_runtime_put(struct device *dev) { (void)dev; pm_count--; }
static int apple_dart_finalize_domain(struct apple_dart_domain *d, struct apple_dart_master_cfg *c)
{ (void)c; if (!finalize_ret) d->finalized = true; return finalize_ret; }
/* The tables go in first; the invalidate after them is what fails. */
static int apple_dart_setup_translation(struct apple_dart_domain *d, struct apple_dart_stream_map *s)
{ int i = idx_of_map(s); state[i] = d; return fail_domain[i] == d ? -ETIMEDOUT : 0; }
static int apple_dart_setup_translation_locked(struct apple_dart_domain *d,
					       struct apple_dart_stream_map *s)
{ int i = idx_of_map(s); state[i] = d; return fail_domain[i] == d ? -ETIMEDOUT : 0; }
static void apple_dart_hw_enable_bypass(struct apple_dart_stream_map *s) { state[idx_of_map(s)] = BYPASS; }
static void apple_dart_hw_disable_dma(struct apple_dart_stream_map *s) { state[idx_of_map(s)] = BLOCKED; }
static void apple_dart_managed_quarantine_locked(struct apple_dart *d)
{ int i = idx_of_dart(d); d->managed_failed = true; quarantined[i] = true; state[i] = BLOCKED; }
static void apple_dart_managed_quarantine(struct apple_dart *d) { apple_dart_managed_quarantine_locked(d); }
'''

MAIN = r'''
static int failures, cases;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s: ", __LINE__, name); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static const int paging_ops, identity_ops, blocked_ops;
static struct apple_dart darts[MAX_DARTS_PER_DEVICE];
static struct device dart_dev;
static struct apple_dart_domain new_dom, old_dom;
static struct iommu_domain identity = { IOMMU_DOMAIN_IDENTITY, &identity_ops };
static struct iommu_domain blocked = { IOMMU_DOMAIN_BLOCKED, &blocked_ops };
/* Device stream 1 on the first DART, 2 on the second; another device has stream 5. */
#define DEV_SIDS(i) BIT((i) + 1)
#define OTHER_SID BIT(5)

static void setup(int n)
{
	memset(darts, 0, sizeof(darts)); memset(&cfg, 0, sizeof(cfg));
	memset(&new_dom, 0, sizeof(new_dom)); memset(&old_dom, 0, sizeof(old_dom));
	memset(fail_domain, 0, sizeof(fail_domain)); memset(quarantined, 0, sizeof(quarantined));
	finalize_ret = pm_count = 0;
	new_dom.pgtbl_ops = old_dom.pgtbl_ops = &new_dom;
	new_dom.domain = (struct iommu_domain){ IOMMU_DOMAIN_DMA, &paging_ops };
	old_dom.domain = (struct iommu_domain){ IOMMU_DOMAIN_DMA, &paging_ops };
	old_dom.finalized = true;
	for (int i = 0; i < n; i++) {
		darts[i].num_streams = 16; darts[i].dev = &dart_dev; darts[i].supports_bypass = true;
		cfg.stream_maps[i].dart = &darts[i]; cfg.stream_maps[i].sidmap[0] = DEV_SIDS(i);
		new_dom.stream_maps[i].dart = old_dom.stream_maps[i].dart = &darts[i];
		new_dom.stream_maps[i].sidmap[0].counter = OTHER_SID;
		state[i] = UNTOUCHED;
	}
}

static const char *show(const void *s)
{
	return s == &new_dom ? "the failed domain" : s == &old_dom ? "the old domain" : (const char *)s;
}

/* No stream may be left on the failed domain's tables; reached ones go back or are blocked. */
static void check_failed(const char *name, int ret, int n, const void *want[])
{
	CHECK(ret, "the attach reported success");
	CHECK(pm_count == 0, "runtime PM unbalanced by %d", pm_count);
	for (int i = 0; i < n; i++)
		CHECK(state[i] == want[i], "stream map %d is on %s, expected %s", i, show(state[i]),
		      show(want[i]));
}

/* The failed device's streams leave the domain; another device's stay. */
static void check_dropped(const char *name, int n, unsigned long keep)
{
	for (int i = 0; i < n; i++) {
		long map = atomic_long_read(&new_dom.stream_maps[i].sidmap[0]);
		bool kept = keep & BIT(i);

		CHECK(!!(map & DEV_SIDS(i)) == kept, "stream map %d: the device's stream %s the domain",
		      i, kept ? "left" : "is still in");
		CHECK(map & OTHER_SID, "stream map %d: another device's stream left the domain", i);
	}
}

static int attach(struct iommu_domain *old)
{
	return apple_dart_attach_dev_paging_unguarded(&new_dom.domain, &dart_dev, old);
}

int main(void)
{
	const char *name;
	int ret;

	name = "first attach, second stream fails";
	setup(2); fail_domain[1] = &new_dom; ret = attach(NULL);
	check_failed(name, ret, 2, (const void *[]){ BLOCKED, BLOCKED }); check_dropped(name, 2, 0); cases++;

	name = "first attach, only stream fails";
	setup(1); fail_domain[0] = &new_dom; ret = attach(NULL);
	check_failed(name, ret, 1, (const void *[]){ BLOCKED }); check_dropped(name, 1, 0); cases++;

	name = "from identity";
	setup(2); state[0] = state[1] = BYPASS; fail_domain[1] = &new_dom; ret = attach(&identity);
	check_failed(name, ret, 2, (const void *[]){ BYPASS, BYPASS }); check_dropped(name, 2, 0); cases++;

	name = "from identity, retained MTP stream";
	setup(2); darts[0].retained_mtp = true; fail_domain[1] = &new_dom; ret = attach(&identity);
	check_failed(name, ret, 2, (const void *[]){ BLOCKED, BYPASS }); cases++;

	name = "from blocked";
	setup(2); state[0] = state[1] = BLOCKED; fail_domain[1] = &new_dom; ret = attach(&blocked);
	check_failed(name, ret, 2, (const void *[]){ BLOCKED, BLOCKED }); check_dropped(name, 2, 0); cases++;

	name = "from a paging domain";
	setup(2); state[0] = state[1] = &old_dom; fail_domain[1] = &new_dom; ret = attach(&old_dom.domain);
	check_failed(name, ret, 2, (const void *[]){ &old_dom, &old_dom }); check_dropped(name, 2, 0); cases++;

	name = "from a paging domain, first stream fails, second not reached";
	setup(2); state[0] = state[1] = &old_dom; fail_domain[0] = &new_dom; ret = attach(&old_dom.domain);
	check_failed(name, ret, 2, (const void *[]){ &old_dom, &old_dom }); check_dropped(name, 2, 0); cases++;

	name = "from a paging domain that cannot be put back";
	setup(2); state[0] = state[1] = &old_dom; fail_domain[1] = &new_dom; fail_domain[0] = &old_dom;
	ret = attach(&old_dom.domain);
	check_failed(name, ret, 2, (const void *[]){ BLOCKED, &old_dom }); cases++;

	name = "from a DCP domain";
	setup(2); old_dom.dcp = &old_dom; fail_domain[1] = &new_dom; ret = attach(&old_dom.domain);
	check_failed(name, ret, 2, (const void *[]){ BLOCKED, BLOCKED }); cases++;

	name = "locked stream reached, second stream fails";
	setup(2); darts[0].locked = true; fail_domain[1] = &new_dom; ret = attach(NULL);
	/* The locked root is unmapped when the domain is freed, so it stays in the domain. */
	check_failed(name, ret, 2, (const void *[]){ &new_dom, BLOCKED }); check_dropped(name, 2, BIT(0)); cases++;

	name = "managed DART fails";
	setup(1); darts[0].managed_sids = BIT(1) | BIT(2); fail_domain[0] = &new_dom; ret = attach(NULL);
	check_failed(name, ret, 1, (const void *[]){ BLOCKED }); check_dropped(name, 1, 0);
	CHECK(quarantined[0], "the managed DART was not quarantined"); cases++;

	name = "managed DART reached, second stream fails";
	setup(2); darts[0].managed_sids = BIT(1) | BIT(2); fail_domain[1] = &new_dom; ret = attach(NULL);
	check_failed(name, ret, 2, (const void *[]){ BLOCKED, BLOCKED }); check_dropped(name, 2, 0);
	CHECK(quarantined[0], "the managed DART was not quarantined"); cases++;

	name = "domain not finalized";
	setup(2); finalize_ret = -EINVAL; ret = attach(NULL);
	check_failed(name, ret, 2, (const void *[]){ UNTOUCHED, UNTOUCHED }); check_dropped(name, 2, 0); cases++;

	name = "success";
	setup(2); state[0] = state[1] = &old_dom; ret = attach(&old_dom.domain);
	CHECK(!ret, "the attach failed: %d", ret); CHECK(pm_count == 0, "runtime PM unbalanced");
	CHECK(state[0] == &new_dom && state[1] == &new_dom, "not every stream is on the new domain");
	check_dropped(name, 2, BIT(0) | BIT(1)); cases++;

	if (failures)
		return 1;
	printf("%d paging attach undo cases PASS\n", cases);
	return 0;
}
'''


def function(text, name):
    # A definition: 'static', its type (maybe on a line of its own), the name.
    match = re.search(r'^static[^;{}()]*?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text, re.M)
    if not match:
        return None
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--source', type=Path, help='an apple-dart.c to test (default: this tree)')
    parser.add_argument('--ref', help='test drivers/iommu/apple-dart.c of this revision instead')
    args = parser.parse_args()
    if args.ref:
        text = subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                        args.ref + ':drivers/iommu/apple-dart.c'], text=True)
    else:
        text = (args.source or ROOT / 'drivers/iommu/apple-dart.c').read_text()

    bodies = []
    for name in FUNCTIONS:
        body = function(text, name)
        if body is None and name not in OPTIONAL:
            print(f'FAIL line 0: {name} is missing')
            sys.exit(1)
        if body:
            bodies.append(body)

    with tempfile.TemporaryDirectory(prefix='dart-attach-undo-') as tmp:
        source = Path(tmp) / 'production.c'
        binary = Path(tmp) / 'production'
        source.write_text(PRELUDE + '\n'.join(bodies) + MAIN)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    sys.stdout.write(run.stderr)
    sys.exit(1 if run.returncode else 0)


if __name__ == '__main__':
    main()
