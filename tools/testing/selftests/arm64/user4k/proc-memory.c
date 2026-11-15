// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

struct stats {
	unsigned long size, rss, pss, private_clean, private_dirty,
		shared_clean, shared_dirty;
	unsigned long anonymous, kernel_page, mmu_page;
};

static bool get_stats(pid_t pid, uintptr_t address, struct stats *s)
{
	char path[64], line[512];
	unsigned long start, end, value;
	unsigned int fields = 0;
	char key[64];
	bool found = false;
	FILE *f;

	memset(s, 0, sizeof(*s));
	snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
	f = fopen(path, "re");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (found)
				break;
			found = start == address;
			continue;
		}
		if (!found)
			continue;
		if (sscanf(line, "%63[^:]: %lu", key, &value) != 2)
			continue;
		if (!strcmp(key, "Size")) {
			s->size = value;
			fields |= 1U << 0;
		} else if (!strcmp(key, "Rss")) {
			s->rss = value;
			fields |= 1U << 1;
		} else if (!strcmp(key, "Pss")) {
			s->pss = value;
			fields |= 1U << 2;
		} else if (!strcmp(key, "Private_Clean")) {
			s->private_clean = value;
			fields |= 1U << 3;
		} else if (!strcmp(key, "Private_Dirty")) {
			s->private_dirty = value;
			fields |= 1U << 4;
		} else if (!strcmp(key, "Shared_Clean")) {
			s->shared_clean = value;
			fields |= 1U << 5;
		} else if (!strcmp(key, "Shared_Dirty")) {
			s->shared_dirty = value;
			fields |= 1U << 6;
		} else if (!strcmp(key, "Anonymous")) {
			s->anonymous = value;
			fields |= 1U << 7;
		} else if (!strcmp(key, "KernelPageSize")) {
			s->kernel_page = value;
			fields |= 1U << 8;
		} else if (!strcmp(key, "MMUPageSize")) {
			s->mmu_page = value;
			fields |= 1U << 9;
		}
	}
	fclose(f);
	return found && fields == (1U << 10) - 1;
}

static bool check(pid_t pid, void *address, size_t page, size_t length,
		  unsigned long pss, unsigned long private,
		  unsigned long shared, bool anon)
{
	struct stats s;
	bool ok = get_stats(pid, (uintptr_t)address, &s);

	ok &= s.size == length / 1024 && s.rss == length / 1024 &&
	      s.pss == pss && s.private_clean + s.private_dirty == private &&
	      s.shared_clean + s.shared_dirty == shared &&
	      s.anonymous == (anon ? length / 1024 : 0) &&
	      s.kernel_page == page / 1024 && s.mmu_page == page / 1024;
	if (!ok)
		printf("# smaps size=%lu rss=%lu pss=%lu private=%lu shared=%lu anon=%lu pages=%lu/%lu\n",
		       s.size, s.rss, s.pss, s.private_clean + s.private_dirty,
		       s.shared_clean + s.shared_dirty, s.anonymous,
		       s.kernel_page, s.mmu_page);
	return ok;
}

static int check_numa(void *address, size_t page)
{
	char line[1024];
	unsigned long start, anonymous = 0, pages = 0, granule = 0;
	bool found = false;
	FILE *f = fopen("/proc/self/numa_maps", "re");

	if (!f)
		return errno == ENOENT ? -1 : 0;
	while (fgets(line, sizeof(line), f)) {
		char *save, *token;

		if (sscanf(line, "%lx", &start) != 1 ||
		    start != (uintptr_t)address)
			continue;
		found = true;
		for (token = strtok_r(line, "\n ", &save); token;
		     token = strtok_r(NULL, "\n ", &save)) {
			unsigned int node;
			unsigned long count;

			if (sscanf(token, "anon=%lu", &count) == 1)
				anonymous = count;
			else if (sscanf(token, "kernelpagesize_kB=%lu",
					&count) == 1)
				granule = count;
			else if (sscanf(token, "N%u=%lu", &node, &count) == 2)
				pages += count;
		}
		break;
	}
	fclose(f);
	if (!found || anonymous != 4 || pages != 4 || granule != page / 1024)
		printf("# numa found=%d anon=%lu pages=%lu granule=%lu\n",
		       found, anonymous, pages, granule);
	return found && anonymous == 4 && pages == 4 && granule == page / 1024;
}

static bool reap(pid_t child)
{
	int status;

	return child > 0 && waitpid(child, &status, 0) == child &&
	       WIFEXITED(status) && !WEXITSTATUS(status);
}

static bool resident_rewrite_reuses(unsigned char *p, size_t page)
{
	uint64_t before[4], after[4];
	int fd = open("/proc/self/pagemap", O_RDONLY);
	unsigned int i;
	bool ok = fd >= 0;

	if (ok)
		ok = pread(fd, before, sizeof(before),
			   (uintptr_t)p / page * 8) == sizeof(before);
	for (i = 0; i < 4; i++)
		p[i * page] ^= 0x21;
	if (ok)
		ok = pread(fd, after, sizeof(after), (uintptr_t)p / page * 8) ==
		     sizeof(after);
	for (i = 0; ok && i < 4; i++) {
		const uint64_t frame = (1ULL << 55) - 1;

		ok &= (before[i] & frame) &&
		      (before[i] & frame) == (after[i] & frame) &&
		      (before[i] & (1ULL << 63)) && (after[i] & (1ULL << 63));
	}
	if (fd >= 0)
		close(fd);
	return ok;
}

static void anonymous_sharing(size_t page)
{
	unsigned char *base, *p;
	int ready[2], go[2];
	pid_t child;
	char byte;
	bool ok;

	base = mmap(NULL, 6 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
		    0);
	if (base == MAP_FAILED ||
	    mprotect(base + page, 4 * page, PROT_READ | PROT_WRITE) ||
	    pipe(ready) || pipe(go))
		ksft_exit_fail_msg("anonymous fixture setup\n");
	p = base + page;
	memset(p, 0x57, 4 * page);
	ksft_test_result(
		check(getpid(), p, page, 4 * page, 4 * page / 1024,
		      4 * page / 1024, 0, true),
		"dense anonymous leaves have exact RSS/PSS/private bytes\n");
	{
		int numa = check_numa(p, page);

		if (numa < 0)
			ksft_test_result_skip(
				"NUMA statistics require CONFIG_NUMA\n");
		else
			ksft_test_result(
				numa,
				"NUMA statistics count target leaves and report their granule\n");
	}
	child = fork();
	if (!child) {
		close(ready[0]);
		close(go[1]);
		if (write(ready[1], "a", 1) != 1 || read(go[0], &byte, 1) != 1)
			_exit(1);
		p[page] = 0x96;
		if (write(ready[1], "b", 1) != 1 || read(go[0], &byte, 1) != 1)
			_exit(2);
		_exit(0);
	}
	close(ready[1]);
	close(go[0]);
	ok = child > 0 && read(ready[0], &byte, 1) == 1 &&
	     check(getpid(), p, page, 4 * page, 2 * page / 1024, 0,
		   4 * page / 1024, true);
	ksft_test_result(
		ok,
		"fork divides each anonymous leaf's PSS by its own mapcount\n");
	ok = write(go[1], "x", 1) == 1 && read(ready[0], &byte, 1) == 1 &&
	     check(getpid(), p, page, 4 * page, (5 * page) / 2048, page / 1024,
		   3 * page / 1024, true) &&
	     p[page] == 0x57;
	ksft_test_result(
		ok,
		"one child COW changes only that leaf's private/shared accounting\n");
	write(go[1], "x", 1);
	close(go[1]);
	close(ready[0]);
	ksft_test_result(reap(child) && check(getpid(), p, page, 4 * page,
					      4 * page / 1024, 4 * page / 1024,
					      0, true),
			 "child exit restores full anonymous PSS\n");
	ksft_test_result(
		resident_rewrite_reuses(p, page),
		"resident writes reuse physical backing after the child exits\n");
	munmap(base, 6 * page);
}

static int worker(int fd, int output, int input, size_t expected)
{
	size_t page = getauxval(AT_PAGESZ);
	unsigned char *p;
	uintptr_t addr;
	char byte;

	if (page != expected)
		return 1;
	p = mmap(NULL, page, PROT_READ, MAP_SHARED, fd,
		 page == 4096 ? 4096 : 0);
	if (p == MAP_FAILED || p[0] != 0x78)
		return 2;
	addr = (uintptr_t)p;
	if (write(output, &addr, sizeof(addr)) != sizeof(addr) ||
	    read(input, &byte, 1) != 1)
		return 3;
	return 0;
}

static void mixed_file(size_t page)
{
	size_t target = page == 4096 ? 16384 : 4096;
	int ready[2], go[2], fd = memfd_create("mixed-pss", 0);
	unsigned char data[16384], *p;
	uintptr_t remote;
	pid_t child;
	bool ok;

	memset(data, 0x78, sizeof(data));
	if (fd < 0 || write(fd, data, sizeof(data)) != sizeof(data) ||
	    pipe(ready) || pipe(go))
		ksft_exit_fail_msg("mixed file fixture setup\n");
	p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 page == 4096 ? 4096 : 0);
	if (p == MAP_FAILED || p[0] != 0x78)
		ksft_exit_fail_msg("mixed file mmap\n");
	child = fork();
	if (!child) {
		char farg[16], out[16], in[16], expected[16];

		close(ready[0]);
		close(go[1]);
		snprintf(farg, sizeof(farg), "%d", fd);
		snprintf(out, sizeof(out), "%d", ready[1]);
		snprintf(in, sizeof(in), "%d", go[0]);
		snprintf(expected, sizeof(expected), "%zu", target);
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, target, 0UL, 0UL, 0UL))
			_exit(4);
		execl("/proc/self/exe", "/proc/self/exe", "--worker", farg, out,
		      in, expected, NULL);
		_exit(5);
	}
	close(ready[1]);
	close(go[0]);
	ok = child > 0 &&
	     read(ready[0], &remote, sizeof(remote)) == sizeof(remote);
	ok = ok &&
	     check(getpid(), p, page, page, page == 4096 ? 2 : 14,
		   page == 4096 ? 0 : 12, 4, false) &&
	     check(child, (void *)remote, target, target,
		   target == 4096 ? 2 : 14, target == 4096 ? 0 : 12, 4, false);
	ksft_test_result(
		ok,
		"mixed file aliases divide only their overlapping physical quarter\n");
	write(go[1], "x", 1);
	close(go[1]);
	close(ready[0]);
	ksft_test_result(reap(child) &&
				 check(getpid(), p, page, page, page / 1024,
				       page / 1024, 0, false),
			 "mixed alias exit restores private file accounting\n");
	munmap(p, page);
	close(fd);
}

int main(int argc, char **argv)
{
	size_t page = getauxval(AT_PAGESZ);

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 6 && !strcmp(argv[1], "--worker"))
		return worker(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
			      strtoul(argv[5], NULL, 10));
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--expect-4k")) ||
	    page != (argc == 2 ? 4096UL : 16384UL))
		return 2;
	ksft_print_header();
	ksft_set_plan(8);
	anonymous_sharing(page);
	mixed_file(page);
	ksft_finished();
}
