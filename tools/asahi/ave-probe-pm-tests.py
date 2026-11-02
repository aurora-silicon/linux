#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the AVE probe unchanged against a supplier-reference model.

Exercise never-opened idle, immediate registration-time use and probe errors.
The model follows runtime.c: setting SUSPENDED drops supplier references even
when the consumer status was already SUSPENDED. This tests probe ordering and
reference ownership, not hardware power gating or firmware restart.
"""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = 'drivers/media/platform/apple/ave/ave-drv.c'

PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
struct ave_dev;
struct device { bool enabled, suspended; unsigned supplier_refs; struct ave_dev *data;
    const void *of_node; };
struct platform_device { struct device dev; };
struct iommu_domain { unsigned type; };
struct ave_dev { struct device *dev; const void *variant; int hw_lock;
    void *codec, *mbox, *coproc, *pd_list; struct iommu_domain *domain; };
struct dev_pm_domain_attach_data {
    const char *const *pd_names; unsigned num_pd_names, pd_flags;
};
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define GFP_KERNEL 0
#define __IOMMU_DOMAIN_PAGING 1
#define PD_FLAG_DEV_LINK_ON 1
#define AVE_AUTOSUSPEND_MS 2000
#define DMA_BIT_MASK(x) ((UINT64_C(1) << (x)) - 1)
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
#define PTR_ERR(x) ((int)(intptr_t)(x))
static const char *const ave_pd_names[] = {"dma", "pipe4", "pipe5", "me0", "me1", "perf"};
/* Whether the node lists the optional "perf" domain (the M4 Pro SoC floor). */
static bool has_perf;
static int of_property_match_string(const void *np, const char *prop, const char *str)
{ (void)np; (void)prop; (void)str; return has_perf ? 5 : -61; }
static struct ave_dev context;
static struct iommu_domain domain = {.type = __IOMMU_DOMAIN_PAGING};
static int attach_error, state_error, enable_error, fw_error, register_error;
static unsigned registrations, fw_probes, fw_removes, state_updates;
static void *devm_kzalloc(struct device *d, size_t n, int f)
{ (void)d; (void)f; assert(n == sizeof(context)); return &context; }
static const void *of_device_get_match_data(struct device *d) { return d; }
static void mutex_init(int *m) { *m = 0; }
static void platform_set_drvdata(struct platform_device *p, void *v) { p->dev.data = v; }
static void *devm_platform_ioremap_resource_byname(struct platform_device *p, const char *n)
{ (void)p; return (void *)n; }
static struct iommu_domain *iommu_get_domain_for_dev(struct device *d)
{ (void)d; return &domain; }
static int dma_set_mask_and_coherent(struct device *d, uint64_t mask)
{ (void)d; assert(mask == DMA_BIT_MASK(42)); return 0; }
#define dev_err_probe(d, e, ...) (e)
static int devm_pm_domain_attach_list(struct device *d,
    const struct dev_pm_domain_attach_data *data, void **list)
{
    assert(!d->enabled && d->suspended);
    assert(data->num_pd_names == (has_perf ? 6U : 5U) && data->pd_flags == PD_FLAG_DEV_LINK_ON);
    assert(data->num_pd_names < 6 || data->pd_names[5][0] == 'p');
    assert(!*list);
    if (attach_error) return attach_error;
    /* One virtual device per domain plus the two preactivated DART supplier links. */
    d->supplier_refs = data->num_pd_names + 2;
    *list = d;
    return data->num_pd_names;
}
static int pm_runtime_set_suspended(struct device *d)
{
    assert(!d->enabled);
    assert(fw_probes == 1 && !fw_error);
    state_updates++;
    if (state_error) return state_error;
    d->suspended = true;
    d->supplier_refs = 0;
    return 0;
}
static void pm_runtime_set_autosuspend_delay(struct device *d, unsigned delay)
{ (void)d; assert(delay == 2000); }
static void pm_runtime_use_autosuspend(struct device *d) { (void)d; }
static void pm_runtime_dont_use_autosuspend(struct device *d) { (void)d; }
static int devm_pm_runtime_enable(struct device *d)
{
    assert(!d->enabled);
    if (enable_error) return enable_error;
    d->enabled = true;
    return 0;
}
static int ave_fw_probe(struct ave_dev *a)
{
    assert(!a->dev->enabled && a->dev->supplier_refs == (has_perf ? 8U : 7U));
    fw_probes++;
    return fw_error;
}
static void ave_fw_remove(struct ave_dev *a) { (void)a; fw_removes++; }
static int ave_v4l2_register(struct ave_dev *a)
{
    struct device *d = a->dev;
    registrations++;
    assert(d->enabled && d->suspended && d->supplier_refs == 0);
    if (register_error) return register_error;
    /* A client can open immediately once its video device is published. */
    for (unsigned n = 0; n < 10; n++) {
        d->suspended = false;
        d->supplier_refs = 7;
        assert(d->enabled && !d->suspended);
        d->supplier_refs = 0;
        d->suspended = true;
    }
    return 0;
}
static void ave_debugfs_init(struct ave_dev *a) { (void)a; }
'''

CASES = r'''
static void reset(struct platform_device *p)
{
    *p = (struct platform_device){.dev.suspended = true};
    context = (struct ave_dev){0};
    attach_error = state_error = enable_error = fw_error = register_error = 0;
    registrations = fw_probes = fw_removes = state_updates = 0;
}
int main(void)
{
    struct platform_device p;
    reset(&p);
    assert(ave_probe(&p) == 0);
    assert(state_updates == 1 && registrations == 1 && fw_probes == 1);
    assert(p.dev.suspended && !p.dev.supplier_refs);
    reset(&p);
    attach_error = -EIO;
    assert(ave_probe(&p) == -EIO);
    assert(!state_updates && !p.dev.enabled && !registrations);
    reset(&p);
    state_error = -EBUSY;
    assert(ave_probe(&p) == -EBUSY);
    assert(!p.dev.enabled && fw_probes == 1 && fw_removes == 1 && !registrations);
    reset(&p);
    enable_error = -ENOMEM;
    assert(ave_probe(&p) == -ENOMEM);
    assert(!p.dev.supplier_refs && fw_probes == 1 && fw_removes == 1 && !registrations);
    reset(&p);
    fw_error = -EINVAL;
    assert(ave_probe(&p) == -EINVAL);
    assert(p.dev.supplier_refs == 7 && !state_updates && !p.dev.enabled && !registrations);
    reset(&p);
    register_error = -EIO;
    assert(ave_probe(&p) == -EIO);
    assert(!p.dev.supplier_refs && fw_removes == 1);
    /* A node with the optional performance floor attaches it with the rest, and it is idle too. */
    reset(&p);
    has_perf = true;
    assert(ave_probe(&p) == 0);
    assert(!p.dev.supplier_refs);
    has_perf = false;
    puts("AVE probe PM: idle suppliers, immediate use, 10 cycles, 5 error paths and the perf domain passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--negative-control', action='store_true')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = (args.source or ROOT / SOURCE).read_text()
    start = source.index('static int ave_probe(')
    probe = source[start:source.index('\n}', start) + 2]
    if args.negative_control:
        old = '\tret = pm_runtime_set_suspended(dev);'
        if probe.count(old) != 1:
            raise SystemExit('cannot locate the production fix for the negative control')
        probe = probe.replace(old, '\tret = 0;')
    with tempfile.TemporaryDirectory(prefix='ave-probe-pm-') as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        c = out / 'ave-probe-pm.c'
        c.write_text(PREFIX + probe + CASES)
        exe = out / 'ave-probe-pm'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(c), '-o', str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        (out / 'run.log').write_text(run.stdout + run.stderr)
        if args.negative_control:
            if run.returncode != -6 or 'supplier_refs == 0' not in run.stderr:
                raise SystemExit('pre-fix probe did not fail the idle-supplier assertion')
            print('AVE probe PM: pre-fix probe compiled and failed the idle-supplier assertion')
        elif run.returncode:
            raise SystemExit(run.stdout + run.stderr)
        else:
            print(run.stdout, end='')


if __name__ == '__main__':
    main()
