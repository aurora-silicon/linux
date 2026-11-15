// SPDX-License-Identifier: GPL-2.0-only
/* Missing-PTE refill must retain contents, faultaround and native density. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../kselftest.h"

#define LEAF 4096UL
#define NATIVE 16384UL
#define BYTES (16UL << 20)
#define LEAVES (BYTES / LEAF)

static unsigned char *data;
static int pm = -1;
static bool density;

static unsigned char tag(size_t leaf)
{
	return leaf % 251 + 1;
}

static bool refill(size_t leaf)
{
	unsigned char *p = data + leaf * LEAF;
	size_t i;

	/* Write first: a read would test zero-page COW instead of hole refill. */
	*(volatile unsigned char *)p = tag(leaf);
	for (i = 1; i < LEAF; i++)
		if (p[i])
			return false;
	memset(p + 1, 0x5a, LEAF - 1);
	return true;
}

static bool contents(void)
{
	size_t leaf, i;

	for (leaf = 0; leaf < LEAVES; leaf++) {
		if (data[leaf * LEAF] != tag(leaf))
			return false;
		for (i = 1; i < LEAF; i++)
			if (data[leaf * LEAF + i] != 0x5a)
				return false;
	}
	return true;
}

static int cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static int native_bytes(size_t *bytes)
{
	uint64_t entries[LEAVES];
	size_t i, unique = 0;
	off_t offset = (uintptr_t)data / LEAF * sizeof(uint64_t);

	if (pm < 0 || pread(pm, entries, sizeof(entries), offset) != sizeof(entries))
		return -1;
	for (i = 0; i < LEAVES; i++) {
		uint64_t pfn = entries[i] & ((UINT64_C(1) << 55) - 1);

		if (!(entries[i] & (UINT64_C(1) << 63)) || !pfn)
			return -1;
		entries[i] = pfn / (NATIVE / LEAF);
	}
	qsort(entries, LEAVES, sizeof(*entries), cmp);
	for (i = 0; i < LEAVES; i++)
		unique += !i || entries[i] != entries[i - 1];
	*bytes = unique * NATIVE;
	return 0;
}

static void check_density(const char *stage, size_t limit)
{
	size_t bytes;

	if (!density) {
		ksft_test_result_skip("%s physical density (needs --native-16k and visible PFNs)\n", stage);
		return;
	}
	if (native_bytes(&bytes)) {
		ksft_test_result_fail("%s pagemap unavailable, swapped or hidden\n", stage);
		return;
	}
	ksft_print_msg("%s logical=%lu native=%zu limit=%zu\n", stage, BYTES, bytes, limit);
	ksft_test_result(bytes <= limit, "%s physical density\n", stage);
}

static bool phased_refill(void)
{
	unsigned int quarter;
	size_t leaf;

	for (quarter = 0; quarter < 4; quarter++) {
		for (leaf = quarter; leaf < LEAVES; leaf += 4)
			if (madvise(data + leaf * LEAF, LEAF, MADV_DONTNEED) || !refill(leaf))
				return false;
		if (!contents())
			return false;
	}
	return true;
}

static bool paired_refill(void)
{
	unsigned int parity;
	size_t leaf;

	for (parity = 0; parity < 2; parity++) {
		for (leaf = parity; leaf < LEAVES; leaf += 4) {
			uint64_t entry;

			if (madvise(data + leaf * LEAF, LEAF, MADV_DONTNEED) ||
			    madvise(data + (leaf + 2) * LEAF, LEAF, MADV_DONTNEED) ||
			    !refill(leaf))
				return false;
			/* Present is visible without privileged PFNs. Prove the other
			 * missing neighbor was prefaulted before touching it ourselves.
			 */
			if (pm < 0 || pread(pm, &entry, sizeof(entry),
				((uintptr_t)data / LEAF + leaf + 2) * 8) != sizeof(entry) ||
			    !(entry & (UINT64_C(1) << 63)) || !refill(leaf + 2))
				return false;
		}
	}
	return contents();
}

int main(int argc, char **argv)
{
	void *mapping;
	size_t leaf;

	ksft_print_header();
	if (getauxval(AT_PAGESZ) != LEAF)
		ksft_exit_skip("Run through the established 4K ABI launcher\n");
	if (argc == 2 && !strcmp(argv[1], "--native-16k"))
		density = true;
	else if (argc != 1)
		ksft_exit_fail_msg("Usage: %s [--native-16k]\n", argv[0]);
	alarm(30);
	mapping = mmap(NULL, BYTES + NATIVE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	data = (void *)(((uintptr_t)mapping + NATIVE - 1) & ~(NATIVE - 1));
	pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	memset(data, 0x5a, BYTES);
	for (leaf = 0; leaf < LEAVES; leaf++)
		data[leaf * LEAF] = tag(leaf);
	ksft_set_plan(8);
	ksft_test_result(contents(), "initial dense content\n");
	check_density("initial dense", BYTES + NATIVE);
	ksft_test_result(phased_refill(), "phased hole refill zeroing and neighbors\n");
	/* Allow bounded pool-bucket slop, but reject the old 64MiB outcome. */
	check_density("phased refill", BYTES + (4UL << 20));
	ksft_test_result(phased_refill(), "reused-slot refill zeroing and neighbors\n");
	check_density("reused-slot refill", BYTES + (4UL << 20));
	ksft_test_result(paired_refill(), "two-hole mask retains missing-neighbor prefault\n");
	check_density("two-hole refill", BYTES + (4UL << 20));
	munmap(mapping, BYTES + NATIVE);
	if (pm >= 0)
		close(pm);
	ksft_finished();
}
