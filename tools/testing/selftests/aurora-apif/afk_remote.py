#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the T6050 callback DMA translation boundary in the production helper."""
from pathlib import Path
import os
import subprocess
import tempfile
from common import block

ROOT = Path(__file__).resolve().parents[4]
helper = block((ROOT / 'drivers/gpu/drm/apple/afk.c').read_text(),
               r'^static int afk_v27_access_remote\(')
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint64_t u64;
typedef uintptr_t phys_addr_t;
#define SZ_1M (1U << 20)
#define PAGE_SIZE 4096UL
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define U64_MAX UINT64_MAX
#define MEMREMAP_WB 0
#define min_t(type,a,b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define dev_err_ratelimited(...) ((void)0)
struct iommu_domain {};
struct apple_dcp { void *dev; };
struct apple_dcp_afkep { struct apple_dcp *dcp; unsigned endpoint; };
static struct iommu_domain domain;
static unsigned char backing[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static bool attached = true, second_page = true;
static unsigned copies;
static struct iommu_domain *iommu_get_domain_for_dev(void *dev) {
 (void)dev; return attached ? &domain : NULL;
}
static phys_addr_t iommu_iova_to_phys(struct iommu_domain *d, u64 dva) {
 assert(d == &domain);
 if (dva < 0x10000 || dva >= 0x12000 || (!second_page && dva >= 0x11000)) return 0;
 return (uintptr_t)backing + dva - 0x10000;
}
static void *memremap(phys_addr_t pa, size_t n, int mode) {
 assert(mode == MEMREMAP_WB && pa >= (uintptr_t)backing);
 assert(pa + n <= (uintptr_t)backing + sizeof(backing)); copies++; return (void *)pa;
}
static void memunmap(void *ptr) {(void)ptr;}
''' + helper + r'''
int main(void) {
 struct apple_dcp dcp = {};
 struct apple_dcp_afkep ep = { .dcp = &dcp, .endpoint = 0x2c };
 unsigned char in[16], out[16] = {};
 memset(in, 0x5a, sizeof(in));
 assert(!afk_v27_access_remote(&ep, 0x10ff8, in, 16, true, true) && copies == 2);
 assert(!afk_v27_access_remote(&ep, 0x10ff8, out, 16, false, true));
 assert(!memcmp(in, out, sizeof(in)));
 copies = 0;
 assert(!afk_v27_access_remote(&ep, 0x10ff8, NULL, 16, false, false) && !copies);
 second_page = false;
 assert(afk_v27_access_remote(&ep, 0x11000, out, 16, false, true) == -ENXIO && !copies);
 attached = false;
 assert(afk_v27_access_remote(&ep, 0x10000, out, 16, false, true) == -EINVAL);
 attached = true;
 assert(afk_v27_access_remote(&ep, UINT64_MAX - 2, out, 16, false, true) == -EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='afk-remote-') as directory:
    source, binary = Path(directory) / 'test.c', Path(directory) / 'test'
    source.write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], env={**os.environ, 'LD_PRELOAD': ''}, check=True)
print('TAP version 13\n1..1\nok 1 - T6050 AFK mapped callback boundaries')
