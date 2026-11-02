#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the driver's completion path with a fetch-ahead MMIO device model."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(text, name):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\)\n\{', text, re.M)
    assert match, name
    end = match.end()
    level = 1
    while level:
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=('tail-only', 'release-early', 'publish-batch'))
    args = parser.parse_args()
    driver = args.root / 'drivers/media/platform/apple/prores'
    header = (driver / 'apple-prores.h').read_text()
    source = (driver / 'apple-prores-drv.c').read_text()
    batch = header[header.index('struct apr_batch {'):header.index('/* Quantisation tables')]
    take = function(source, 'apr_take_done')
    irq = function(source, 'apr_irq')
    if args.mutation == 'tail-only':
        take = take.replace('if (!status)\n\t\treturn NULL;', '')
        batch = batch.replace('if (!status)\n\t\treturn -EAGAIN;', '')
        batch = batch.replace('status != APR_IRQ_DONE', '(status && status != APR_IRQ_DONE)')
    elif args.mutation == 'release-early':
        take = take.replace('if (!*err) {', 'if (0) {')
    elif args.mutation == 'publish-batch':
        batch = batch.replace('(batch->done + 1)', 'batch->count')
    # Keep register values shared with the driver, without host Linux headers.
    defines = '\n'.join(line for line in header.splitlines() if
                        line.startswith('#define APR_') and
                        any(name in line for name in ('APR_REG_', 'APR_IRQ_', 'APR_MODE_',
                                                       'APR_RING_PTR_MASK', 'APR_DESC_BYTES')))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef int irqreturn_t;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define spin_lock(p) ((void)(p))
#define spin_unlock(p) ((void)(p))
#define dev_err_ratelimited(...) ((void)0)
#define dma_wmb() ((void)0)
#define system_wq NULL
''' + defines + '\n' + batch + r'''
struct apr_ctx { int owner; };
struct apr_dev {
    struct apr_ctx *active;
    struct apr_batch batch;
    bool recovering;
    unsigned long started;
    u32 head;
    unsigned char *regs;
    int hw_lock, watchdog;
};
static u32 mmio[0x2000 / 4];
static unsigned int released, pm_puts, slots_finished, resets_queued;
static unsigned long jiffies = 100;
static u32 apr_read(struct apr_dev *a, u32 reg) { (void)a; return mmio[reg/4]; }
static void apr_write(struct apr_dev *a, u32 reg, u32 value) {
    (void)a;
    if (reg == APR_REG_IRQ_STATUS) mmio[reg/4] &= ~value;
    else mmio[reg/4] = value;
}
static void writel(u32 value, void *addr) { *(u32 *)addr = value; }
static void mod_delayed_work(void *q, int *w, int delay) {
    (void)q; (void)w; (void)delay; resets_queued++;
}
static void cancel_delayed_work(int *w) { (void)w; }
static void apr_finish_job(struct apr_dev *a, struct apr_ctx *ctx, int err) {
    assert(ctx && !err);
    released += a->batch.count;
    pm_puts++;
    slots_finished++;
}
''' + take + '\n' + irq + r'''
static struct apr_ctx owner = { 42 };
static struct apr_dev begin(unsigned int depth, u32 base) {
    struct apr_dev a = {
        .active = &owner, .batch = { .start = base, .count = depth },
        .regs = (unsigned char *)mmio,
    };
    released = pm_puts = slots_finished = resets_queued = 0;
    for (unsigned int i = 0; i < sizeof(mmio)/sizeof(mmio[0]); i++) mmio[i] = 0;
    a.head = apr_batch_head(&a.batch);
    assert(a.head == base + APR_DESC_BYTES);
    mmio[APR_REG_RING_HEAD / 4] = a.head;
    return a;
}
int main(void) {
    for (unsigned int depth = 1; depth <= 4; depth++) {
        /* The last batch ends at the last whole descriptor in the ring. */
        u32 base = (0x20000 / APR_DESC_BYTES - depth) * APR_DESC_BYTES;
        struct apr_dev a = begin(depth, base);
        for (unsigned int i = 0; i < depth; i++) {
            u32 expected = base + (i + 1) * APR_DESC_BYTES;
            assert(a.head == expected);
            /* Fetch finishes first. Repeated watchdog/IRQ reads must retain ownership. */
            mmio[APR_REG_RING_TAIL / 4] = expected;
            for (unsigned int poll = 0; poll < 20; poll++) {
                assert(apr_irq(0, &a) == IRQ_NONE);
                assert(a.batch.done == i && a.active == &owner);
                assert(!released && !pm_puts && !slots_finished);
            }
            mmio[APR_REG_IRQ_STATUS / 4] = APR_IRQ_DONE;
            assert(apr_irq(0, &a) == IRQ_HANDLED);
            assert(mmio[APR_REG_IRQ_STATUS / 4] == 0);
            if (i + 1 < depth) {
                assert(a.active == &owner);
                assert(mmio[APR_REG_RING_HEAD / 4] == expected + APR_DESC_BYTES);
                assert(!released && !pm_puts && !slots_finished);
            }
        }
        assert(!a.active && released == depth && pm_puts == 1 && slots_finished == 1);
        assert(!resets_queued);
        assert(apr_irq(0, &a) == IRQ_NONE);
        assert(released == depth && pm_puts == 1);
    }
    for (unsigned int fault = 0; fault < 4; fault++) {
        struct apr_dev a = begin(4, 0);
        mmio[APR_REG_IRQ_STATUS / 4] = fault < 2 ? APR_IRQ_DONE :
            (fault == 2 ? APR_IRQ_ERROR : APR_IRQ_DONE | APR_IRQ_ERROR);
        mmio[APR_REG_RING_TAIL / 4] = a.head + (fault == 0 ? -0x80 : APR_DESC_BYTES);
        assert(apr_irq(0, &a) == IRQ_HANDLED);
        assert(a.recovering && a.active == &owner && resets_queued == 1);
        assert(mmio[APR_REG_MODE / 4] == APR_MODE_OFF);
        assert(!released && !pm_puts && !slots_finished);
        assert(apr_irq(0, &a) == IRQ_NONE);
    }
    puts("PASS: fetch-ahead ownership, IRQ handoff, depths 1-4, ring end, error quarantine");
}
'''
    with tempfile.TemporaryDirectory(prefix='prores-queue-') as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        c = out / 'prores-queue.c'
        exe = out / 'prores-queue'
        c.write_text(harness)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], check=False)
        if result.returncode:
            raise SystemExit(f'Assertion: completion model exited {result.returncode}')


if __name__ == '__main__':
    main()
