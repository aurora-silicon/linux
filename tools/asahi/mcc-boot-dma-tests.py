#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run apple-mcc's boot DMA supplier walk against mock device tree nodes."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'drivers/soc/apple/mcc.c').read_text()


def function(name):
    a = SRC.rfind('\n', 0, SRC.index(name + '(struct')) + 1
    b = SRC.index('\n}\n', a) + 3
    return SRC[a:b]


CODE = (function('apple_mcc_scans_boot_framebuffer') + function('apple_mcc_wait_boot_dma') +
        function('apple_mcc_boot_dma_detached'))
UNRESOLVED = 'if (!it.node) {'
DETACHED = 'if (of_node_check_flag(it.node, OF_DETACHED)) {'
assert CODE.count(UNRESOLVED) == 2 and CODE.count(DETACHED) == 2
if '--accept-unresolved' in sys.argv:
    CODE = CODE.replace(UNRESOLVED, 'if (0) {', 1)
if '--final-skips-unresolved' in sys.argv:
    a = CODE.index('apple_mcc_boot_dma_detached(')
    CODE = CODE[:a] + CODE[a:].replace('return true;\n\t\t\tcontinue;', 'continue;\n\t\t\tcontinue;', 1)
if '--ignore-detached-supplier' in sys.argv:
    CODE = CODE.replace(DETACHED, 'if (0) {', 1)

PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
#define EPROBE_DEFER 517
#define OF_DETACHED 3
struct device_node {
	const char *name; u32 phandle; bool detached, detach_on_lookup, available, fb, bound, retired;
	struct device_node *display; u32 boot[4]; int nboot, refs;
};
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; struct device_node *np; };
static struct device_node *nodes[8];
static int nnodes, readies;
static struct device_node *get(struct device_node *n) { if (n) n->refs++; return n; }
static void of_node_put(struct device_node *n) { if (n) { assert(n->refs > 0); n->refs--; } }
static bool of_node_check_flag(struct device_node *n, int f) { assert(n && f == OF_DETACHED); return n->detached; }
static bool of_device_is_available(struct device_node *n) { return n->available; }
static struct device_node *of_parse_phandle(struct device_node *n, const char *p, int i)
{ return get(n->display); }
static struct device_node *next_fb(struct device_node *from)
{
	int i = 0;

	if (from) {
		while (nodes[i] != from)
			i++;
		of_node_put(from);
		i++;
	}
	for (; i < nnodes; i++)
		if (nodes[i]->fb)
			return get(nodes[i]);
	return NULL;
}
#define for_each_compatible_node(dn, type, compat) \
	for (dn = next_fb(NULL); dn; dn = next_fb(dn))
struct of_phandle_iterator { struct device_node *node, *parent; u32 phandle; int i; };
static struct device_node *by_phandle(u32 ph)
{
	/* A detached node has left the tree and its phandle cache. */
	for (int i = 0; i < nnodes; i++)
		if (nodes[i]->phandle == ph && !nodes[i]->detached) {
			/* Detached between lookup and use. */
			if (nodes[i]->detach_on_lookup)
				nodes[i]->detached = true;
			return get(nodes[i]);
		}
	return NULL;
}
static bool it_next(struct of_phandle_iterator *it, int *err)
{
	of_node_put(it->node);
	it->node = NULL;
	if (it->i >= it->parent->nboot) {
		*err = -ENOENT;
		return false;
	}
	it->phandle = it->parent->boot[it->i++];
	it->node = it->phandle ? by_phandle(it->phandle) : NULL;
	return true;
}
#define of_for_each_phandle(it, err, np, name, cells, count) \
	for ((it)->node = NULL, (it)->parent = (np), (it)->i = 0; it_next((it), &(err));)
static struct platform_device pdevs[8];
static struct platform_device *of_find_device_by_node(struct device_node *n)
{
	for (int i = 0; i < nnodes; i++)
		if (nodes[i] == n) {
			pdevs[i].np = get(n);
			return &pdevs[i];
		}
	return NULL;
}
static bool device_is_bound(struct device *d) { return container_of_pdev(d)->np->bound; }
'''.replace('container_of_pdev(d)', '((struct platform_device *)((char *)(d) - offsetof(struct platform_device, dev)))') + r'''
static void put_device(struct device *d)
{ of_node_put(((struct platform_device *)((char *)d - offsetof(struct platform_device, dev)))->np); }
#define dev_err_probe(dev, err, ...) (err)
static int apple_mcc_boot_scanout_ready(struct device *dev, struct device_node *supplier)
{
	assert(supplier && !supplier->detached);
	readies++;
	return supplier->retired ? 0 : -EPROBE_DEFER;
}
'''

MAIN = r'''
static struct device_node mcc, dcp, disp, fb;
static struct device dev = { .of_node = &mcc };

static void reset(void)
{
	mcc = (struct device_node){ .name = "mcc", .phandle = 1, .available = true };
	dcp = (struct device_node){ .name = "dcp", .phandle = 2 };
	disp = (struct device_node){ .name = "disp", .phandle = 3, .available = true, .bound = true };
	fb = (struct device_node){ .name = "fb", .phandle = 4, .available = true, .fb = true };
	nodes[0] = &mcc; nodes[1] = &dcp; nodes[2] = &disp; nodes[3] = &fb; nnodes = 4;
	readies = 0;
}

static int walk(void)
{
	int ret = apple_mcc_wait_boot_dma(&dev);

	for (int i = 0; i < nnodes; i++)
		assert(nodes[i]->refs == 0);
	return ret;
}

int main(void)
{
	/* Legacy supplier, bound: allocation allowed, no scanout wait. */
	reset(); mcc.boot[0] = 3; mcc.nboot = 1;
	assert(walk() == 0 && !readies);
	disp.bound = false;
	assert(walk() == -EPROBE_DEFER);

	/* Empty entry: no device. */
	reset(); mcc.nboot = 1;
	assert(walk() == 0);

	/* Unresolved phandle, and an enabled framebuffer naming no display. */
	reset(); mcc.boot[0] = 99; mcc.nboot = 1;
	assert(walk() == -EINVAL && !readies);
	/* Unresolved, then a legacy supplier: still refused. */
	reset(); mcc.boot[0] = 99; mcc.boot[1] = 3; mcc.nboot = 2;
	assert(walk() == -EINVAL);

	/* A disabled DCP scanning the boot framebuffer keeps allocation off. */
	reset(); mcc.boot[0] = 2; mcc.nboot = 1; fb.display = &dcp;
	assert(walk() == -EPROBE_DEFER && readies == 1);
	/* A bound DCP scanning the boot framebuffer waits until scanout retires. */
	dcp.available = true; dcp.bound = true;
	assert(walk() == -EPROBE_DEFER && readies == 2);
	dcp.retired = true;
	assert(walk() == 0 && readies == 3);
	/* An unbound DCP still defers to bind even after scanout retired. */
	dcp.bound = false;
	assert(walk() == -EPROBE_DEFER && readies == 4);
	/* Disabled framebuffer: no scanout wait, and a disabled supplier still defers to bind. */
	reset(); mcc.boot[0] = 2; mcc.nboot = 1; fb.display = &dcp; fb.available = false;
	assert(walk() == -EPROBE_DEFER && !readies);
	dcp.available = true; dcp.bound = true;
	assert(walk() == 0 && !readies);

	/* Detached consumer or supplier: cannot confirm. */
	reset(); mcc.boot[0] = 2; mcc.nboot = 1; fb.display = &dcp; dcp.retired = true;
	mcc.detached = true;
	assert(walk() == -ENODEV);
	/* A detached supplier no longer resolves; one detached after its lookup. */
	mcc.detached = false; dcp.detached = true;
	assert(walk() == -EINVAL && !readies);
	dcp.detached = false; dcp.detach_on_lookup = true;
	assert(walk() == -ENODEV && !readies);

	/* The final check under the lock, before allocation is enabled. */
	reset(); mcc.boot[0] = 3; mcc.boot[1] = 2; mcc.nboot = 2;
	assert(!apple_mcc_boot_dma_detached(&dev));
	dcp.detached = true;
	assert(apple_mcc_boot_dma_detached(&dev));
	dcp.detached = false; mcc.detached = true;
	assert(apple_mcc_boot_dma_detached(&dev));
	/* A supplier that disappears between the two walks. */
	reset(); mcc.boot[0] = 3; mcc.boot[1] = 2; mcc.boot[2] = 0; mcc.nboot = 3;
	fb.display = &dcp; dcp.retired = true; dcp.available = true; dcp.bound = true;
	assert(walk() == 0);
	dcp.detached = true;
	assert(apple_mcc_boot_dma_detached(&dev));
	dcp.detached = false;
	assert(!apple_mcc_boot_dma_detached(&dev));
	for (int i = 0; i < nnodes; i++)
		assert(nodes[i]->refs == 0);
	return 0;
}
'''

# The detach notifier in mcc-boot-dma.c, against an enabled controller.
BOOT = (ROOT / 'drivers/soc/apple/mcc-boot-dma.c').read_text()
HDR = (ROOT / 'include/linux/soc/apple/mcc.h').read_text()
LOSES = 'return !scanout || !scanout->retired;'
assert BOOT.count(LOSES) == 1
if '--detach-retired-only' in sys.argv:
    BOOT = BOOT.replace(LOSES, 'return !scanout;')
strip = lambda text: '\n'.join(l for l in text.splitlines() if not l.startswith('#include'))
HDR = strip(HDR).replace('#if IS_ENABLED(CONFIG_APPLE_MCC_BOOT_DMA)', '#if 1')

NPRE = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
typedef uint32_t u32;
#define EPROBE_DEFER 517
#define OF_DETACHED 3
#define __init
#define core_initcall(f)
#define EXPORT_SYMBOL_GPL(f)
#define IS_ENABLED(x) 1
#define WARN_ON_ONCE(c) assert(!(c))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &(n), &(n) }
static void INIT_LIST_HEAD(struct list_head *h) { h->next = h->prev = h; }
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->next = h; n->prev = h->prev; h->prev->next = n; h->prev = n; }
static void list_del(struct list_head *n) { n->prev->next = n->next; n->next->prev = n->prev; }
static void list_del_init(struct list_head *n) { list_del(n); INIT_LIST_HEAD(n); }
static void list_move_tail(struct list_head *n, struct list_head *h) { list_del(n); list_add_tail(n, h); }
static void list_splice_tail_init(struct list_head *n, struct list_head *h)
{ while (!list_empty(n)) list_move_tail(n->next, h); }
#define list_entry(p, t, m) container_of(p, t, m)
#define list_for_each_entry(p, h, m) \
	for (p = list_entry((h)->next, typeof(*p), m); &p->m != (h); p = list_entry(p->m.next, typeof(*p), m))
#define list_for_each_entry_safe(p, n, h, m) \
	for (p = list_entry((h)->next, typeof(*p), m), n = list_entry(p->m.next, typeof(*p), m); \
	     &p->m != (h); p = n, n = list_entry(n->m.next, typeof(*n), m))
static int locked;
struct mutex { int unused; };
#define DEFINE_MUTEX(n) struct mutex n
static void mutex_lock(struct mutex *m) { assert(!locked); locked = 1; }
static void mutex_unlock(struct mutex *m) { assert(locked); locked = 0; }
#define kzalloc_obj(x) calloc(1, sizeof(x))
#define kfree free
struct device_node { u32 phandle; bool detached; u32 boot[4]; int nboot; int refs; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
static struct device_node *of_node_get(struct device_node *n) { if (n) n->refs++; return n; }
static void of_node_put(struct device_node *n) { if (n) { assert(n->refs > 0); n->refs--; } }
static bool of_node_check_flag(struct device_node *n, int f) { return n->detached; }
static int of_property_count_u32_elems(struct device_node *n, const char *p) { return n->nboot; }
static int of_property_read_u32_index(struct device_node *n, const char *p, int i, u32 *v)
{ *v = n->boot[i]; return 0; }
static struct platform_device *of_find_device_by_node(struct device_node *n) { return NULL; }
static void device_initial_probe(struct device *d) { }
static void put_device(struct device *d) { }
struct work_struct { int unused; };
#define DECLARE_WORK(n, f) struct work_struct n
static void schedule_work(struct work_struct *w) { }
struct notifier_block { int (*notifier_call)(struct notifier_block *, unsigned long, void *); };
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define OF_RECONFIG_DETACH_NODE 2
struct of_reconfig_data { struct device_node *dn; };
static int of_reconfig_notifier_register(struct notifier_block *nb) { return 0; }
"""

NMAIN = r"""
static int lost_calls;
static void lost(struct apple_mcc_boot_dma_consumer *c)
{
	assert(locked);
	lost_calls++;
}

static struct device_node mcc = { .phandle = 1, .boot = { 3, 2 }, .nboot = 2 };
static struct device_node dcp = { .phandle = 2 }, disp = { .phandle = 3 }, other = { .phandle = 9 };
static struct device dev = { .of_node = &mcc };
static struct apple_mcc_boot_dma_consumer c = { .dev = &dev, .lost = lost };

static void enable(void)
{
	apple_mcc_boot_dma_lock();
	apple_mcc_boot_dma_register_locked(&c);
	apple_mcc_boot_dma_unlock();
}

static int detach(struct device_node *n)
{
	struct of_reconfig_data rd = { .dn = n };
	int before = lost_calls;

	n->detached = true;
	apple_mcc_of_notify(NULL, OF_RECONFIG_DETACH_NODE, &rd);
	n->detached = false;
	return lost_calls - before;
}

int main(void)
{
	INIT_LIST_HEAD(&c.link);
	enable();
	/* An unrelated node. */
	assert(detach(&other) == 0 && !list_empty(&c.link));
	/* Enabled, then a supplier still scanning out detaches: allocation off. */
	assert(apple_mcc_boot_scanout_reserve(&dcp) == 0);
	assert(detach(&dcp) == 1 && list_empty(&c.link));
	/* Not registered any more: a second detach reaches nothing. */
	assert(detach(&mcc) == 0);
	/* A supplier whose boot scanout retired may detach. */
	enable();
	assert(apple_mcc_boot_scanout_reserve(&dcp) == 0);
	apple_mcc_boot_scanout_retired(&dcp);
	assert(detach(&dcp) == 0 && !list_empty(&c.link));
	/* A supplier with no scanout record (bound only), and the controller's own node. */
	assert(detach(&disp) == 1);
	enable();
	assert(detach(&mcc) == 1);
	/* Removal unregisters first. */
	enable();
	apple_mcc_boot_dma_unregister(&c);
	assert(detach(&dcp) == 0 && detach(&mcc) == 0);
	assert(dcp.refs == 0 && mcc.refs == 0);
	return 0;
}
"""

# A deep sleep with a detach in between, through mcc.c's PM callbacks and the real notifier.
a = SRC.index('struct apple_mcc {')
MSTRUCT = SRC[a:SRC.index('\n};\n', a) + 4]
PMCODE = ''.join(function(n) for n in ('apple_mcc_turn_off', 'apple_mcc_boot_dma_lost',
                                       'apple_mcc_suspend_locked', 'apple_mcc_resume_locked',
                                       'apple_mcc_suspend_noirq', 'apple_mcc_resume_noirq'))
CLEAR = '\tmcc->sleep_disabled = false;\n}'
assert PMCODE.count(CLEAR) == 1
if '--resume-after-detach' in sys.argv:
    PMCODE = PMCODE.replace(CLEAR, '}')
MPRE = r"""
#define __iomem
#define MCC_MAX_PLANES 64
#define MCC_CACHE_WAYS 0x2800
#define FIELD_GET(m, v) (v)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
static u32 readl(void *p) { return 0; }
static bool alloc_on;
static bool pm_suspend_via_firmware(void) { return true; }
""" + MSTRUCT + r"""
static void *drvdata;
static void *dev_get_drvdata(struct device *d) { return drvdata; }
static void apple_mcc_quiesce(struct apple_mcc *mcc)
{
	assert(locked);
	if (!mcc->enabled || mcc->retained)
		return;
	alloc_on = false;
	mcc->enabled = false;
}
static int apple_mcc_disable_t8132(struct apple_mcc *mcc) { alloc_on = false; return 0; }
static int apple_mcc_enable(struct apple_mcc *mcc) { assert(locked); alloc_on = true; return 0; }
static int apple_mcc_enable_t8132(struct apple_mcc *mcc) { alloc_on = true; return 0; }
static int apple_mcc_set_ways(struct apple_mcc *mcc, u32 ways) { return 0; }
"""
PMAIN = r"""
static struct device_node pmcc = { .phandle = 1, .boot = { 2 }, .nboot = 1 };
static struct device_node pdcp = { .phandle = 2 };
static struct device pdev = { .of_node = &pmcc };
static struct apple_mcc m;

static void probe(void)
{
	m = (struct apple_mcc){ .dev = &pdev, .enabled = true };
	INIT_LIST_HEAD(&m.boot_dma.link);
	m.boot_dma.dev = &pdev;
	m.boot_dma.lost = apple_mcc_boot_dma_lost;
	drvdata = &m;
	alloc_on = true;
	apple_mcc_boot_dma_lock();
	apple_mcc_boot_dma_register_locked(&m.boot_dma);
	apple_mcc_boot_dma_unlock();
}

static void detach(struct device_node *n)
{
	struct of_reconfig_data rd = { .dn = n };

	n->detached = true;
	apple_mcc_of_notify(NULL, OF_RECONFIG_DETACH_NODE, &rd);
	n->detached = false;
}

int main(void)
{
	/* Suspend, a supplier still scanning out detaches, resume: allocation stays off. */
	assert(apple_mcc_boot_scanout_reserve(&pdcp) == 0);
	probe();
	assert(apple_mcc_suspend_noirq(&pdev) == 0 && !alloc_on && m.sleep_disabled);
	detach(&pdcp);
	assert(!m.sleep_disabled && list_empty(&m.boot_dma.link));
	assert(apple_mcc_resume_noirq(&pdev) == 0 && !alloc_on && !m.enabled);
	assert(!locked);

	/* The same with a retired supplier (the detach dropped its record): the wake turns allocation back on. */
	assert(apple_mcc_boot_scanout_reserve(&pdcp) == 0);
	apple_mcc_boot_scanout_retired(&pdcp);
	probe();
	assert(apple_mcc_suspend_noirq(&pdev) == 0 && !alloc_on);
	detach(&pdcp);
	assert(m.sleep_disabled && !list_empty(&m.boot_dma.link));
	assert(apple_mcc_resume_noirq(&pdev) == 0 && alloc_on && m.enabled);
	assert(!locked);
	return 0;
}
"""

with tempfile.TemporaryDirectory() as t:
    for name, text in (('mcc', PRE + CODE + MAIN), ('notify', NPRE + HDR + strip(BOOT) + NMAIN),
                       ('pm', NPRE + HDR + strip(BOOT) + MPRE + PMCODE + PMAIN)):
        src = Path(t) / (name + '.c')
        src.write_text(text)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                        '-Wno-unused-variable', '-fsanitize=address,undefined', str(src),
                        '-o', str(src.with_suffix(''))], check=True)
        if subprocess.run([str(src.with_suffix(''))]).returncode:
            sys.exit(1)
print('PASS boot DMA walk: legacy bound/unbound, empty entry, unresolved phandle, '
      'disabled scanning DCP, disabled framebuffer, detached consumer/supplier, final detach check, supplier gone between walks; '
      'enabled then supplier/own node detached, retired supplier, removal; '
      'suspend-detach-resume stays off, retired supplier resumes')
