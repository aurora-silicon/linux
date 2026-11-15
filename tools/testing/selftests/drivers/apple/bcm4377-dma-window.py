#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile and exercise the production BCM4377 DMA-window admission helper.

This CPU test checks arithmetic and preservation of the old zero-based
contract. Real firmware DMA and simultaneous Wi-Fi require a reboot test.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def definition(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[5])
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (args.source / 'drivers/bluetooth/hci_bcm4377.c').read_text()
    common = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned long long u64;
typedef uint32_t u32;
typedef u64 dma_addr_t;
#define BCM4377_DMA_MASK 0xfffffe00ULL
#define IS_ALIGNED(x, a) (!((x) & ((a) - 1)))
#define DMA_BIT_MASK(n) (~0ULL >> (64 - (n)))
#define fls64(n) (64 - __builtin_clzll(n))
struct device { int unused; };
struct pci_dev { struct device dev; };
struct iommu_domain {
    struct { u64 aperture_start, aperture_end; bool force_aperture; } geometry;
    bool dma;
};
struct bcm4377_hw { unsigned int id; };
struct bcm4377_data {
    struct pci_dev *pdev;
    const struct bcm4377_hw *hw;
    dma_addr_t dma_window_base;
    u32 dma_window_size;
};
static struct iommu_domain *test_domain;
static int mask_result, mask_calls;
static u64 last_mask;
static struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{ (void)dev; return test_domain; }
static bool iommu_is_dma_domain(struct iommu_domain *d) { return d->dma; }
static int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{ (void)dev; mask_calls++; last_mask = mask; return mask_result; }
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err_probe(dev, ret, ...) (ret)
'''
    tests = r'''
int main(void)
{
    struct pci_dev pci = {0};
    struct bcm4377_hw hw = { .id = 0x4388 };
    struct bcm4377_data b = { .pdev = &pci, .hw = &hw };
    struct iommu_domain d = {
        .dma = true,
        .geometry = { 0x10000100000ULL, 0x1003fefffffULL, true },
    };
    assert(bcm4377_setup_dma(&b) == 0); /* no IOMMU: original contract */
    assert(b.dma_window_base == 0 && b.dma_window_size == BCM4377_DMA_MASK);
    assert(last_mask == BCM4377_DMA_MASK);
    test_domain = &d;
    assert(bcm4377_setup_dma(&b) == 0); /* real J713 ADT aperture */
    assert(b.dma_window_base == 0x10000100000ULL && b.dma_window_size == 0x3fe00000);
    assert(last_mask == DMA_BIT_MASK(41));
    u64 top = b.dma_window_base + b.dma_window_size - 1;
    assert(top == d.geometry.aperture_end && top <= last_mask);
    mask_result = -EIO;
    assert(bcm4377_setup_dma(&b) == -EIO); /* retain DMA API failure */
    mask_result = 0;
    d.dma = false;
    assert(bcm4377_setup_dma(&b) == 0 && b.dma_window_base == 0);
    d.dma = true;
    d.geometry.force_aperture = false;
    assert(bcm4377_setup_dma(&b) == 0 && b.dma_window_base == 0);
    d.geometry.force_aperture = true;
    hw.id = 0x4387;
    assert(bcm4377_setup_dma(&b) == 0 && b.dma_window_base == 0);
    hw.id = 0x4388;
    d.geometry.aperture_start = 0;
    d.geometry.aperture_end = 0xffffffffULL;
    assert(bcm4377_setup_dma(&b) == 0 && b.dma_window_base == 0);
    assert(last_mask == BCM4377_DMA_MASK);
    const u64 base = 0x10000100000ULL;
    const struct { u64 base, end; int rc; } cases[] = {
        { 0x10000010000ULL, 0x1003fe0ffffULL, 0 }, /* inherited DT aperture */
        { base, base + 0x1ff, 0 },
        { base, base + BCM4377_DMA_MASK - 1, 0 },
        { base, base - 1, -ERANGE },
        { base, base, -ERANGE },
        { base + 1, base + 0x3ff, -ERANGE },
        { base, base + 0x200, -ERANGE },
        { base, base + BCM4377_DMA_MASK, -ERANGE },
        { base, base + 0xffffffffULL, -ERANGE },
        { base, ~0ULL, -ERANGE },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        d.geometry.aperture_start = cases[i].base;
        d.geometry.aperture_end = cases[i].end;
        int before = mask_calls;
        assert(bcm4377_setup_dma(&b) == cases[i].rc);
        if (cases[i].rc)
            assert(mask_calls == before); /* no mask changes on invalid input */
        else {
            assert(b.dma_window_base == cases[i].base);
            assert((u64)b.dma_window_size == cases[i].end - cases[i].base + 1);
        }
    }
    puts("BCM4377 DMA window: 17 admission/error groups passed");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='bcm4377-window-') as temp:
        c = Path(temp) / 'window.c'
        exe = Path(temp) / 'window'
        c.write_text(common + definition(source, 'static int bcm4377_setup_dma(') + tests)
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
