// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long swapouts(void)
{
	FILE *f = fopen("/proc/vmstat", "re");
	char key[128];
	unsigned long value, result = 0;

	if (!f)
		return 0;
	while (fscanf(f, "%127s %lu", key, &value) == 2)
		if (!strcmp(key, "pswpout"))
			result = value;
	fclose(f);
	return result;
}

static unsigned long swapped_kb(void)
{
	FILE *f = fopen("/proc/self/status", "re");
	char line[256];
	unsigned long result = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "VmSwap: %lu kB", &result) == 1)
			break;
	fclose(f);
	return result;
}

static int advise(void *addr, size_t length, int operation, const char *name)
{
	int ret = madvise(addr, length, operation);

	if (ret)
		printf("# %s failed: errno=%d (%s)\n", name, errno,
		       strerror(errno));
	return ret;
}

static unsigned char pattern(size_t i, unsigned int round)
{
	return (i ^ (i >> 12) ^ (i >> 20) ^ (round * 0x55)) & 255;
}

static size_t swapped_pages(unsigned char *p, size_t length, size_t page)
{
	uint64_t *values = calloc(length / page, sizeof(*values));
	int fd = open("/proc/self/pagemap", O_RDONLY);
	size_t i, count = length / page, swapped = 0;

	if (!values || fd < 0 ||
	    pread(fd, values, count * sizeof(*values),
		  (uintptr_t)p / page * sizeof(*values)) !=
		    (ssize_t)(count * sizeof(*values))) {
		swapped = SIZE_MAX;
	} else {
		for (i = 0; i < count; i++)
			swapped += !!(values[i] & (1ULL << 62)) &&
				   !(values[i] & (1ULL << 63));
	}
	if (fd >= 0)
		close(fd);
	free(values);
	return swapped;
}

static void diagnose_resident(unsigned char *p, size_t length, size_t page)
{
	int pm = open("/proc/self/pagemap", O_RDONLY);
	int flags = open("/proc/kpageflags", O_RDONLY);
	int count = open("/proc/kpagecount", O_RDONLY);

	for (size_t i = 0; i < length / page; i++) {
		uint64_t entry = 0, state = 0, maps = 0;
		unsigned long native_pfn;

		if (pread(pm, &entry, 8, ((uintptr_t)p / page + i) * 8) != 8 ||
		    !(entry & (1ULL << 63)))
			continue;
		native_pfn = (entry & ((1ULL << 55) - 1)) * page / 16384;
		if (pread(flags, &state, 8, native_pfn * 8) != 8 ||
		    pread(count, &maps, 8, native_pfn * 8) != 8)
			printf("# resident metadata read failed: errno=%d\n",
			       errno);
		printf("# resident leaf=%zu native_pfn=%#lx flags=%#llx maps=%llu\n",
		       i, native_pfn, (unsigned long long)state,
		       (unsigned long long)maps);
	}
	close(pm);
	close(flags);
	close(count);
}

/* PAGEOUT drains only the calling CPU's pending LRU additions. */
static bool drain_pageout_cpus(unsigned char *p, size_t length)
{
	cpu_set_t allowed, one;
	bool ok = true;

	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		return false;
	for (unsigned int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &allowed))
			continue;
		CPU_ZERO(&one);
		CPU_SET(cpu, &one);
		if (sched_setaffinity(0, sizeof(one), &one)) {
			ok = false;
			break;
		}
		ok &= advise(p, length, MADV_COLD, "CPU drain COLD") == 0;
		ok &= advise(p, length, MADV_PAGEOUT, "CPU drain PAGEOUT") == 0;
	}
	ok &= sched_setaffinity(0, sizeof(allowed), &allowed) == 0;
	return ok;
}

static bool swapped_pagemap(unsigned char *p, size_t length, size_t page)
{
	uint64_t *values = calloc(length / page, sizeof(*values));
	int fd = open("/proc/self/pagemap", O_RDONLY);
	size_t i, count = length / page;
	bool ok = values && fd >= 0;

	if (ok)
		ok = pread(fd, values, count * sizeof(*values),
			   (uintptr_t)p / page * sizeof(*values)) ==
		     (ssize_t)(count * sizeof(*values));
	for (i = 0; ok && i < count; i++) {
		uint64_t frame = values[i] & ((1ULL << 55) - 1);

		ok &= !!(values[i] & (1ULL << 62)) &&
		      !(values[i] & (1ULL << 63));
		if (page == 4096) {
			ok &= ((frame >> 5) & 3) ==
			      (((uintptr_t)p / page + i) & 3);
			if (i && (((uintptr_t)p / page + i) & 3))
				ok &= (values[i] & ((1ULL << 55) - 1)) ==
				      (values[i - 1] & ((1ULL << 55) - 1)) +
					      (1U << 5);
		}
	}
	if (fd >= 0)
		close(fd);
	free(values);
	printf("%s - %zuK pagemap reports swapped leaves and exact quarter offsets\n",
	       ok ? "ok" : "not ok", page / 1024);
	return ok;
}

static bool check_swap_pss(pid_t pid, void *p, size_t length,
			   unsigned long expected_swap,
			   unsigned long expected_pss)
{
	char path[64], line[512];
	unsigned long start, end, swap = 0, pss = 0;
	unsigned int fields = 0;
	bool found = false, ok;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
	f = fopen(path, "re");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (found)
				break;
			found = start == (uintptr_t)p &&
				end == (uintptr_t)p + length;
			continue;
		}
		if (!found)
			continue;
		if (sscanf(line, "Swap: %lu", &swap) == 1)
			fields |= 1;
		else if (sscanf(line, "SwapPss: %lu", &pss) == 1)
			fields |= 2;
	}
	fclose(f);
	ok = found && fields == 3 && swap == expected_swap &&
	     pss == expected_pss;
	if (!ok)
		printf("# swap stats found=%d fields=%u Swap=%lu/%lu SwapPss=%lu/%lu\n",
		       found, fields, swap, expected_swap, pss, expected_pss);
	return ok;
}

static bool swapped_accounting(unsigned char *p, size_t length, size_t page,
			       bool forked)
{
	int ready[2], go[2], status;
	pid_t child;
	char byte;
	bool ok = check_swap_pss(getpid(), p, length, length / 1024,
				 length / 1024);

	if (!forked || !ok)
		goto out;
	if (pipe(ready) || pipe(go))
		return false;
	child = fork();
	if (child < 0)
		return false;
	if (!child) {
		close(ready[0]);
		close(go[1]);
		if (read(go[0], &byte, 1) != 1)
			_exit(1);
		p[page] ^= 0x37;
		if (write(ready[1], "x", 1) != 1 || read(go[0], &byte, 1) != 1)
			_exit(2);
		_exit(0);
	}
	close(ready[1]);
	close(go[0]);
	ok &= check_swap_pss(getpid(), p, length, length / 1024, length / 2048);
	ok &= check_swap_pss(child, p, length, length / 1024, length / 2048);
	ok &= write(go[1], "x", 1) == 1 && read(ready[0], &byte, 1) == 1;
	ok &= check_swap_pss(getpid(), p, length, length / 1024,
			     (length + page) / 2048);
	ok &= check_swap_pss(child, p, length, (length - page) / 1024,
			     (length - page) / 2048);
	write(go[1], "x", 1);
	close(go[1]);
	close(ready[0]);
	ok &= waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	      !WEXITSTATUS(status);
	ok &= check_swap_pss(getpid(), p, length, length / 1024, length / 1024);
out:
	printf("%s - %zuK SwapPss %s\n", ok ? "ok" : "not ok", page / 1024,
	       forked ? "fork, one-leaf swap-in/COW and child exit" :
			"exclusive quarters");
	return ok;
}

int main(int argc, char **argv)
{
	const size_t length = 4 * 1024 * 1024;
	size_t page = getauxval(AT_PAGESZ), count = length / page, i, j, absent;
	unsigned char *p, *vec, *reservation;
	unsigned long before, after, swapped_before, swapped_after;
	unsigned int round, tries;
	size_t bad_bytes, bad_residency;
	bool ok = true;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--expect-4k")) ||
	    page != (argc == 2 ? 4096UL : 16384UL))
		return 2;
	reservation = mmap(NULL, length + 2 * page, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return 3;
	p = reservation + page;
	vec = malloc(count);
	if (!vec || mprotect(p, length, PROT_READ | PROT_WRITE))
		return 3;
	for (round = 0; round < 4 && ok; round++) {
		for (i = 0; i < length; i++)
			p[i] = pattern(i, round);
		before = swapouts();
		swapped_before = swapped_kb();
		ok = advise(p, length, MADV_COLD, "COLD") == 0;
		ok &= advise(p, length, MADV_PAGEOUT, "PAGEOUT") == 0;
		after = before;
		absent = 0;
		for (tries = 0; ok && tries < 200; tries++) {
			if (mincore(p, length, vec)) {
				ok = false;
				break;
			}
			absent = 0;
			for (i = 0; i < count; i++)
				absent += !(vec[i] & 1);
			after = swapouts();
			if (after > before &&
			    swapped_pages(p, length, page) == count)
				break;
			if (tries == 32) {
				printf("# checking pending remote LRU additions before CPU sweep\n");
				diagnose_resident(p, length, page);
				ok &= drain_pageout_cpus(p, length);
				printf("# after CPU sweep: swapped=%zu/%zu\n",
				       swapped_pages(p, length, page), count);
			}
			/* PAGEOUT may skip busy folios; establish the full-swap fixture. */
			usleep(10000);
			ok &= advise(p, length, MADV_COLD, "retry COLD") == 0;
			ok &= advise(p, length, MADV_PAGEOUT,
				     "retry PAGEOUT") == 0;
		}
		printf("# pageout setup attempts=%u\n", tries + 1);
		if (tries == 200)
			diagnose_resident(p, length, page);
		ok &= tries < 200;
		swapped_after = swapped_kb();
		/* mincore also counts warm swap-cache data as resident. */
		ok &= after > before && swapped_after > swapped_before;
		ok &= swapped_pagemap(p, length, page);
		ok &= swapped_accounting(p, length, page, round == 1);
		if (!round)
			ok &= advise(p + page, page, MADV_WILLNEED,
				     "WILLNEED") == 0;
		else {
			if (round == 3) {
				ok &= mlock(p, length) == 0;
				ok &= swapped_pages(p, length, page) == 0;
			} else {
				ok &= advise(p, length, MADV_POPULATE_READ,
					     "POPULATE_READ") == 0;
			}
			ok &= mincore(p, length, vec) == 0;
			bad_residency = 0;
			for (i = 0; i < count; i++)
				bad_residency += !(vec[i] & 1);
			printf("# after %s: absent=%zu/%zu\n",
			       round == 3 ? "mlock" : "POPULATE_READ",
			       bad_residency, count);
			ok &= bad_residency == 0;
		}
		bad_bytes = 0;
		/* Nonsequential user-page order, every byte checked after swap. */
		for (i = 0; i < count; i++) {
			size_t index = ((i * 61) & (count - 1)) * page;

			for (j = index; j < index + page; j++)
				if (p[j] != pattern(j, round)) {
					if (!bad_bytes)
						printf("# first bad byte at %#zx: got=%#x expected=%#x\n",
						       j, p[j],
						       pattern(j, round));
					bad_bytes++;
					ok = false;
				}
		}
		if (round == 3)
			ok &= munlock(p, length) == 0;
		printf("# bad bytes=%zu\n", bad_bytes);
		printf("%s - %zuK EL0 pageout round %u: pswpout=%lu->%lu VmSwap=%lu->%lu kB absent=%zu/%zu %s\n",
		       ok ? "ok" : "not ok", page / 1024, round, before, after,
		       swapped_before, swapped_after, absent, count,
		       round == 3 ? "mlock/readback" :
		       round	  ? "prefault/readback" :
				    "willneed/readback");
	}
	munmap(reservation, length + 2 * page);
	free(vec);
	return ok ? 0 : 1;
}
