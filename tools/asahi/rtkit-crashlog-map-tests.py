#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check which crash log buffers a co-processor places that RTKit maps.

apple_rtkit_crashlog_map(), apple_rtkit_maps_placed_crashlog() from
drivers/soc/apple/rtkit.c and apple_dart_fw_range() with its two helpers from
drivers/iommu/apple-dart.c are taken unchanged and built with ASan and UBSan
against a stand-in IOMMU (4 KiB granularity) and a stand-in resource map. The
buffer is mapped only when every IOMMU page it touches, through Linux's paging
mappings or those the firmware kept, translates at the same offset as its first
byte, and the CPU-page-rounded range is DRAM that isn't the kernel's System RAM.
Identity domains never answer APPLE_DART_FW_LINUX and are refused. Every
memremap() is checked against the pages the test gave the device and must be
write-combined.

--old-ref REV runs the same checks on the driver at REV (its own walk, where
apple-dart.c has no apple_dart_fw_range()) and requires them to fail there.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/soc/apple/rtkit.c'
DART = 'drivers/iommu/apple-dart.c'


def function(source, name):
    """The definition of function name (a forward declaration is skipped)."""
    pattern = r'^(?:static )?(?:void|int|bool|phys_addr_t)\s*\n?' + name + r'\('
    match = next((m for m in re.finditer(pattern, source, re.M)
                  if ';' not in source[m.end():source.index('{', m.end())]), None)
    assert match, name
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def constants(source):
    out = [line for line in source.splitlines()
           if re.match(r'#define APPLE_RTKIT_BUFFER_REQUEST_(SIZE|IOVA)\b', line)]
    assert len(out) == 2, out
    return '\n'.join(out) + '\n'


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef uint64_t dma_addr_t; typedef uint64_t phys_addr_t;
#define GENMASK_ULL(h,l) ((~0ULL << (l)) & (~0ULL >> (63-(h))))
#define FIELD_GET(m,v) (((v) & (m)) >> __builtin_ctzll(m))
#define PAGE_SIZE 16384UL
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define ALIGN(x, a) (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define __ffs(x) ((unsigned long)__builtin_ctzl(x))
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define IS_REACHABLE(x) 1
#define CONFIG_APPLE_DART 1
#define MEMREMAP_WB 1
#define MEMREMAP_WC 4
#define dev_err(...) ((void)0)
#define APPLE_RTKIT_EP_CRASHLOG 2
#define APPLE_RTKIT_FIRMWARE_OWNED_CRASHLOG (1u << 0)
#define APPLE_DART_FW_LINUX (1u << 0)
#define APPLE_DART_FW_RETAINED (1u << 1)
#define EXPORT_SYMBOL_GPL(x)
#define __IOMMU_DOMAIN_PAGING (1u << 1)
#define IOMMU_DOMAIN_IDENTITY (1u << 2)
#define IOMMU_DOMAIN_DMA (__IOMMU_DOMAIN_PAGING | (1u << 3))

struct device { int unused; };
struct iommu_domain { unsigned type; unsigned long pgsize_bitmap; };
struct apple_rtkit_ops {
	unsigned flags;
	void *shmem_setup;
	void *shmem_setup_ep;
};
struct apple_rtkit_shmem { void *buffer; size_t size; dma_addr_t iova; bool is_mapped; };
struct apple_rtkit {
	struct device *dev;
	const struct apple_rtkit_ops *ops;
	struct apple_rtkit_shmem crashlog_buffer;
	bool crashlog_placed;
};
static bool apple_rtkit_firmware_owns_crashlog(struct apple_rtkit *rtk)
{
	return rtk->ops && (rtk->ops->flags & APPLE_RTKIT_FIRMWARE_OWNED_CRASHLOG);
}

/*
 * The device's IOMMU at 4 KiB granularity (4 subpages per 16 KiB block), so a
 * 4 KiB pgsize_bitmap check genuinely tests subpage discontinuities.
 */
#define IOMMU_PAGE 0x4000ULL
#define SUB_PAGE 0x1000ULL
#define DVA_BASE 0x100000000ULL
#define NPAGES 16
#define NSUB (NPAGES * 4)
static phys_addr_t table[NSUB], kept[NSUB];
static struct iommu_domain the_domain = {
	.type = IOMMU_DOMAIN_DMA,
	.pgsize_bitmap = 0x4000 | 0x2000000,
};

static void map16(phys_addr_t *t, int page16, phys_addr_t phys)
{
	for (int s = 0; s < 4; s++)
		t[page16 * 4 + s] = phys ? phys + s * SUB_PAGE : 0;
}

static struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
	(void)dev; return &the_domain;
}
static phys_addr_t lookup(phys_addr_t *t, dma_addr_t iova)
{
	if (iova < DVA_BASE || iova >= DVA_BASE + NSUB * SUB_PAGE)
		return 0;
	phys_addr_t page = t[(iova - DVA_BASE) / SUB_PAGE];
	return page ? page + (iova & (SUB_PAGE - 1)) : 0;
}
static phys_addr_t iommu_iova_to_phys(struct iommu_domain *d, dma_addr_t iova)
{
	if (d->type == IOMMU_DOMAIN_IDENTITY)
		return iova;
	return lookup(table, iova);
}
/*
 * As the driver's: identity domains have no owner (-ENODEV), and a DVA Linux
 * mapped itself is refused (-EACCES).
 */
static int apple_dart_fw_remote(struct device *dev, dma_addr_t iova, bool write, phys_addr_t *phys)
{
	(void)dev; (void)write;
	if (the_domain.type == IOMMU_DOMAIN_IDENTITY)
		return -ENODEV;
	if (lookup(table, iova))
		return -EACCES;
	*phys = lookup(kept, iova);
	return *phys ? 0 : -ENXIO;
}

/*
 * The resource map: registers below 0x800000000, System RAM (the kernel's) at
 * 0x800000000-0x87fffcfff (ending mid-16 KiB CPU page so CPU-page rounding is
 * genuinely exercised), and above it DRAM the kernel doesn't use (carveouts).
 */
#define IORESOURCE_MEM 0x200
#define IORESOURCE_SYSTEM_RAM 0x1200
#define IORES_DESC_NONE 0
enum { REGION_INTERSECTS, REGION_DISJOINT, REGION_MIXED };
#define RAM_START 0x800000000ULL
#define RAM_END 0x87fffd000ULL
static int region_intersects(phys_addr_t start, size_t size, unsigned long flags, unsigned long desc)
{
	phys_addr_t end = start + size;
	(void)desc;
	assert(flags == IORESOURCE_SYSTEM_RAM);
	return start < RAM_END && end > RAM_START ? REGION_INTERSECTS : REGION_DISJOINT;
}

/* Is the physical byte range [pa, pa + len) inside pages the device was given? */
static bool device_owns(phys_addr_t pa, size_t len)
{
	for (phys_addr_t p = pa & ~(SUB_PAGE - 1); p < pa + len; p += SUB_PAGE) {
		bool found = false;
		for (int i = 0; i < NSUB; i++)
			found |= table[i] == p || kept[i] == p;
		if (!found)
			return false;
	}
	return true;
}
static unsigned remaps;
static char backing[1];
static void *memremap(phys_addr_t pa, size_t len, unsigned long flags)
{
	remaps++;
	if (!device_owns(pa, len) || ALIGN_DOWN(pa, PAGE_SIZE) < RAM_END) {
		fprintf(stderr, "FAIL: memremap(%#llx, %#zx) covers memory the device may not share\n",
			(unsigned long long)pa, len);
		assert(0);
	}
	if (flags != MEMREMAP_WC) {
		fprintf(stderr, "FAIL: memremap(%#llx, %#zx) not write-combined\n",
			(unsigned long long)pa, len);
		assert(0);
	}
	return backing;
}
static unsigned acks;
static int apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 msg, void *c, bool a)
{
	(void)rtk; (void)ep; (void)msg; (void)c; (void)a; acks++; return 0;
}
'''

MAIN = r'''
static struct device dev;

static u64 request(dma_addr_t iova, size_t size)
{
	return ((u64)(size >> 12) << 44) | iova;
}

static int place(dma_addr_t iova, size_t size)
{
	struct apple_rtkit rtk = { .dev = &dev };
	int ret;

	remaps = acks = 0;
	ret = apple_rtkit_crashlog_map(&rtk, request(iova, size));
	assert(ret || (remaps == 1 && acks == 1 && rtk.crashlog_placed &&
		       rtk.crashlog_buffer.size == size && rtk.crashlog_buffer.iova == iova));
	assert(!ret || (remaps == 0 && acks == 0 && !rtk.crashlog_placed));
	return ret;
}

int main(void)
{
	/*
	 * Linux's mappings: DVA pages 0-2 on contiguous physical pages, page 3 elsewhere,
	 * 4 unmapped; pages 6-7 straddle the end of System RAM; page 8 on registers.
	 */
	map16(table, 0, 0x900000000ULL);
	map16(table, 1, 0x900004000ULL);
	map16(table, 2, 0x900008000ULL);
	map16(table, 3, 0x980000000ULL);
	map16(table, 6, 0x87fffc000ULL);
	map16(table, 7, 0x880000000ULL);
	map16(table, 8, 0x200000000ULL);
	/* The firmware's kept mappings: pages 10-11 contiguous, 12 elsewhere. */
	map16(kept, 10, 0xa00000000ULL);
	map16(kept, 11, 0xa00004000ULL);
	map16(kept, 12, 0xb00000000ULL);

	/* Aligned buffers on contiguous pages. */
	assert(place(DVA_BASE, 0x4000) == 0);
	assert(place(DVA_BASE, 0xc000) == 0);
	/* Unaligned, inside one page or across contiguous ones. */
	assert(place(DVA_BASE + 0x1000, 0x2000) == 0);
	assert(place(DVA_BASE + 0x3000, 0x4000) == 0);
	assert(place(DVA_BASE + 0x6000, 0x6000) == 0);
	/* Through the firmware's kept mappings. */
	assert(place(DVA_BASE + 10 * IOMMU_PAGE + 0x2000, 0x4000) == 0);

	/* An aligned buffer whose second page lies elsewhere or isn't mapped. */
	assert(place(DVA_BASE + 0x8000, 0x8000) == -ERANGE);
	assert(place(DVA_BASE + 0xc000, 0x8000) == -ERANGE);
	assert(place(DVA_BASE + 11 * IOMMU_PAGE, 0x8000) == -ERANGE);

	/*
	 * Unaligned buffers whose last bytes are on a page that lies elsewhere
	 * or isn't mapped, including a last byte on byte 0 of the bad page
	 * (catches addr < last vs addr <= last).
	 */
	assert(place(DVA_BASE + 0x8001, 0x4000) == -ERANGE);
	assert(place(DVA_BASE + 0xb000, 0x4000) == -ERANGE);
	assert(place(DVA_BASE + 0xf000, 0x4000) == -ERANGE);
	assert(place(DVA_BASE + 0x9000, 0x8000) == -ERANGE);

	/* Nothing at the first byte. */
	assert(place(DVA_BASE + 0x10000, 0x4000) == -ENOENT);
	assert(place(DVA_BASE + 0x3c000, 0x1000) == -ENOENT);

	/*
	 * Contiguous, but reaching System RAM, wholly in it, sharing a 16 KiB
	 * CPU page with it (strictly past RAM_END), or on registers below DRAM;
	 * the first full CPU page past System RAM is a carveout and is mapped.
	 */
	assert(place(DVA_BASE + 6 * IOMMU_PAGE, 0x8000) == -EACCES);
	assert(place(DVA_BASE + 6 * IOMMU_PAGE, 0x1000) == -EACCES);
	assert(place(DVA_BASE + 6 * IOMMU_PAGE + 0x3000, 0x1000) == -EACCES);
	assert(place(DVA_BASE + 8 * IOMMU_PAGE, 0x4000) == -EACCES);
	assert(place(DVA_BASE + 7 * IOMMU_PAGE, 0x4000) == 0);

	/* A buffer that would wrap the DVA space. */
	assert(place(0xfffffffff000ULL, 0x2000) != 0);

	/*
	 * 4 KiB smallest IOMMU page: break only subpage 2 inside 16 KiB block 13.
	 * A 16 KiB walk misses it; the 4 KiB walk catches it.
	 */
	map16(table, 13, 0x910000000ULL);
	table[13 * 4 + 2] = 0x920000000ULL;
	the_domain.pgsize_bitmap = 0x1000 | 0x4000;
	assert(place(DVA_BASE + 0x3000, 0x4000) == 0);
	assert(place(DVA_BASE + 13 * IOMMU_PAGE, 0x4000) == -ERANGE);
	/*
	 * 4 KiB carveout strictly past RAM_END (0x87fffd000) sharing a 16 KiB
	 * CPU page with System RAM is refused by CPU-page rounding.
	 */
	table[14 * 4 + 0] = 0x87ffff000ULL;
	assert(place(DVA_BASE + 14 * IOMMU_PAGE, 0x1000) == -EACCES);

	/*
	 * Identity domain: APPLE_DART_FW_LINUX skips non-paging domains and
	 * apple_dart_fw_remote() returns -ENODEV on identity domains, so
	 * identity domains are refused.
	 */
	the_domain.type = IOMMU_DOMAIN_IDENTITY;
	the_domain.pgsize_bitmap = 0x4000;
	assert(place(0x900000000ULL, 0x4000) == -ENOENT);
	assert(place(DVA_BASE + 10 * IOMMU_PAGE + 0x2000, 0x4000) == -ENOENT);
	the_domain.type = IOMMU_DOMAIN_DMA;

#ifdef HAVE_MAPS_PLACED_ROUTING
	{
		struct apple_rtkit_ops plain = {};
		struct apple_rtkit_ops fw_owned = { .flags = APPLE_RTKIT_FIRMWARE_OWNED_CRASHLOG };
		struct apple_rtkit_ops with_setup = { .shmem_setup = (void *)1 };
		struct apple_rtkit_ops with_setup_ep = { .shmem_setup_ep = (void *)1 };
		struct apple_rtkit r_plain = { .ops = &plain };
		struct apple_rtkit r_fw = { .ops = &fw_owned };
		struct apple_rtkit r_setup = { .ops = &with_setup };
		struct apple_rtkit r_setup_ep = { .ops = &with_setup_ep };

		assert(apple_rtkit_maps_placed_crashlog(&r_plain));
		assert(!apple_rtkit_maps_placed_crashlog(&r_fw));
		assert(!apple_rtkit_maps_placed_crashlog(&r_setup));
		assert(!apple_rtkit_maps_placed_crashlog(&r_setup_ep));
	}
#endif

	puts("PASS crash log buffers are mapped, write-combined, only over pages that translate contiguously to DRAM outside System RAM");
	return 0;
}
'''


def show(ref, path):
    if not ref:
        return (ROOT / path).read_text()
    return subprocess.run(['git', '-C', str(ROOT), 'show', f'{ref}:{path}'], check=True,
                          capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, help='keep build and result files here')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--old-ref', help='require the driver at this revision to fail')
    args = parser.parse_args()
    source, dart = show(args.old_ref, DRIVER), show(args.old_ref, DART)
    extra = ''
    if 'int apple_dart_fw_range(' in dart:
        walk = ''.join(function(dart, name) for name in
                       ('apple_dart_fw_phys_shareable', 'apple_dart_fw_range_page', 'apple_dart_fw_range'))
    else:
        walk = function(source, 'apple_rtkit_crashlog_phys')
    if 'static bool apple_rtkit_maps_placed_crashlog(' in source:
        extra = '#define HAVE_MAPS_PLACED_ROUTING 1\n' + function(source, 'apple_rtkit_maps_placed_crashlog')
    code = PREFIX + constants(source) + walk + extra + function(source, 'apple_rtkit_crashlog_map') + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        cfile, exe = out / 'crashlog-map.c', out / 'crashlog-map'
        cfile.write_text(code)
        subprocess.run([args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(cfile), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if args.old_ref:
            if result.returncode == 0:
                raise SystemExit(f'{args.old_ref} passed; the test does not catch the original code')
            print(result.stderr, end='')
            print('old revision fails as required')
            return 0
        print(result.stdout + result.stderr, end='')
        return result.returncode

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='rtkit-crashlog-map-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
