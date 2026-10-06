// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

#define PRESENT (1ULL << 63)
#define EXCLUSIVE (1ULL << 56)
#define GUARD (1ULL << 58)
#define FRAME ((1ULL << 55) - 1)
#define NPAGE 5

struct ready {
	uintptr_t address;
	size_t page;
	uint64_t entries[NPAGE];
	uint64_t shared_entry;
};

static int open_proc(pid_t pid, const char *name, int flags)
{
	char path[80];

	snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
	return open(path, flags);
}

static bool entries(int fd, uintptr_t address, size_t page, uint64_t *values,
		    int nr)
{
	return pread(fd, values, nr * sizeof(*values),
		     address / page * sizeof(*values)) ==
	       nr * (ssize_t)sizeof(*values);
}

static unsigned char *mapping(size_t page)
{
	const size_t boundary = 32 * 1024 * 1024;
	size_t reserve = boundary + 8 * page, head, tail;
	unsigned char *base, *p;

	base = mmap(NULL, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
		    0);
	if (base == MAP_FAILED)
		return NULL;
	/* Cross both native 32 MiB and alternative 2 MiB PTE-table boundaries. */
	p = (void *)(((uintptr_t)base + 2 * page + boundary - 1) &
		     ~(boundary - 1));
	p -= 2 * page;
	head = p - base;
	tail = reserve - head - NPAGE * page;
	if ((head && munmap(base, head)) ||
	    (tail && munmap(p + NPAGE * page, tail)) ||
	    mprotect(p, NPAGE * page, PROT_READ | PROT_WRITE))
		return NULL;
	memset(p, 0x67, NPAGE * page);
	if (munmap(p + 2 * page, page) ||
	    madvise(p + 3 * page, page, MADV_GUARD_INSTALL) ||
	    mprotect(p + 4 * page, page, PROT_NONE))
		return NULL;
	return p;
}

static bool scan(int fd, uintptr_t addr, size_t page)
{
	struct page_region vec[8];
	struct pm_scan_arg arg = {
		.size = sizeof(arg),
		.start = addr,
		.end = addr + NPAGE * page,
		.vec = (uintptr_t)vec,
		.vec_len = 8,
		.return_mask = PAGE_IS_PRESENT | PAGE_IS_GUARD,
	};
	int nr = ioctl(fd, PAGEMAP_SCAN, &arg);
	bool ok = nr == 3 && arg.walk_end == arg.end;
	if (!ok)
		printf("# scan initial nr=%d errno=%d end_delta=%lld\n", nr,
		       errno, (long long)(arg.walk_end - arg.end));

	/* The unmapped middle page produces no VMA result. */
	if (nr == 3)
		ok &= vec[0].start == addr && vec[0].end == addr + 2 * page &&
		      vec[0].categories == PAGE_IS_PRESENT &&
		      vec[1].start == addr + 3 * page &&
		      vec[1].end == addr + 4 * page &&
		      vec[1].categories == PAGE_IS_GUARD &&
		      vec[2].start == addr + 4 * page &&
		      vec[2].end == arg.end &&
		      vec[2].categories == PAGE_IS_PRESENT;
	arg.max_pages = 1;
	arg.walk_end = 0;
	nr = ioctl(fd, PAGEMAP_SCAN, &arg);
	ok &= nr == 1 && vec[0].start == addr && vec[0].end == addr + page &&
	      arg.walk_end == addr + page;
	arg.start = addr + 1;
	errno = 0;
	ok &= ioctl(fd, PAGEMAP_SCAN, &arg) == -1 && errno == EINVAL;
	return ok;
}

static int remote_worker(int output, int input, size_t expected, int shared_fd)
{
	struct ready ready = { .page = getauxval(AT_PAGESZ) };
	unsigned char *p;
	int fd = open_proc(getpid(), "pagemap", O_RDONLY);
	char byte;
	unsigned char *shared =
		mmap(NULL, 16384, PROT_READ, MAP_SHARED, shared_fd, 0);
	size_t offset = ready.page == 4096 ? 4096 : 0;

	if (shared == MAP_FAILED || shared[offset] != 0x83 ||
	    !entries(fd, (uintptr_t)shared + offset, ready.page,
		     &ready.shared_entry, 1))
		return 2;
	/* Allocate aliases first so mmap cannot reuse our deliberate hole. */
	p = mapping(ready.page);
	ready.address = (uintptr_t)p;
	if (ready.page != expected || !p || fd < 0 ||
	    !entries(fd, ready.address, ready.page, ready.entries, NPAGE) ||
	    write(output, &ready, sizeof(ready)) != sizeof(ready) ||
	    read(input, &byte, 1) != 1)
		return 1;
	return 0;
}

static bool remote_read(size_t own_page)
{
	int ready_pipe[2], done_pipe[2], status, fd, shared_fd, self_fd;
	unsigned char *shared;
	uint64_t self_entry;
	size_t offset = own_page == 4096 ? 4096 : 0;
	size_t target = own_page == 4096 ? 16384 : 4096;
	struct ready ready;
	uint64_t actual[NPAGE];
	pid_t child;
	bool ok;

	shared_fd = memfd_create("mixed-pagemap", 0);
	if (shared_fd < 0 || ftruncate(shared_fd, 16384))
		return false;
	shared = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED,
		      shared_fd, 0);
	self_fd = open_proc(getpid(), "pagemap", O_RDONLY);
	if (shared == MAP_FAILED || self_fd < 0)
		return false;
	memset(shared, 0x83, 16384);
	if (!entries(self_fd, (uintptr_t)shared + offset, own_page, &self_entry,
		     1))
		return false;
	close(self_fd);
	if (pipe(ready_pipe) || pipe(done_pipe))
		return false;
	child = fork();
	if (!child) {
		char output[16], input[16], expected[16], shared_arg[16];

		close(ready_pipe[0]);
		close(done_pipe[1]);
		snprintf(output, sizeof(output), "%d", ready_pipe[1]);
		snprintf(input, sizeof(input), "%d", done_pipe[0]);
		snprintf(expected, sizeof(expected), "%zu", target);
		snprintf(shared_arg, sizeof(shared_arg), "%d", shared_fd);
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, target, 0UL, 0UL, 0UL))
			_exit(2);
		execl("/proc/self/exe", "/proc/self/exe", "--remote", output,
		      input, expected, shared_arg, NULL);
		_exit(3);
	}
	close(ready_pipe[1]);
	close(done_pipe[0]);
	ok = child > 0 &&
	     read(ready_pipe[0], &ready, sizeof(ready)) == sizeof(ready);
	fd = open_proc(child, "pagemap", O_RDONLY);
	ok = ok && ready.page == target && fd >= 0 &&
	     entries(fd, ready.address, target, actual, NPAGE) &&
	     !memcmp(actual, ready.entries, sizeof(actual)) &&
	     scan(fd, ready.address, target);
	if (!ok)
		printf("# remote geometry/read failure: target=%zu reported=%zu fd=%d errno=%d\n",
		       target, ready.page, fd, errno);
	if (ok) {
		size_t target_offset = target == 4096 ? 4096 : 0;

		/* Compare physical byte addresses, not differently sized PFNs. */
		ok &= (self_entry & FRAME) && (ready.shared_entry & FRAME) &&
		      (self_entry & FRAME) * own_page - offset ==
			      (ready.shared_entry & FRAME) * target -
				      target_offset;
		if (!ok)
			printf("# shared PFN mismatch: own_nonzero=%d target_nonzero=%d byte_delta=%lld\n",
			       !!(self_entry & FRAME),
			       !!(ready.shared_entry & FRAME),
			       (long long)((self_entry & FRAME) * own_page -
					   offset -
					   (ready.shared_entry & FRAME) *
						   target +
					   target_offset));
	}
	if (fd >= 0)
		close(fd);
	write(done_pipe[1], "x", 1);
	close(done_pipe[1]);
	close(ready_pipe[0]);
	ok &= child > 0 && waitpid(child, &status, 0) == child &&
	      WIFEXITED(status) && !WEXITSTATUS(status);
	munmap(shared, 16384);
	close(shared_fd);
	return ok;
}

int main(int argc, char **argv)
{
	size_t page = getauxval(AT_PAGESZ);
	unsigned char *p;
	uint64_t values[NPAGE], again[NPAGE];
	int fd, clear, i;
	bool ok;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 6 && !strcmp(argv[1], "--remote"))
		return remote_worker(atoi(argv[2]), atoi(argv[3]),
				     strtoul(argv[4], NULL, 10), atoi(argv[5]));
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--expect-4k")) ||
	    page != (argc == 2 ? 4096UL : 16384UL))
		return 2;
	ksft_print_header();
	ksft_set_plan(6);
	p = mapping(page);
	fd = open_proc(getpid(), "pagemap", O_RDONLY);
	if (!p || fd < 0)
		ksft_exit_fail_msg("proc mapping setup: %s\n", strerror(errno));
	ok = entries(fd, (uintptr_t)p, page, values, NPAGE);
	ok &= (values[0] & PRESENT) && (values[1] & PRESENT) && !values[2] &&
	      (values[3] & GUARD) && !(values[3] & PRESENT) &&
	      (values[4] & PRESENT);
	ok &= (values[0] & EXCLUSIVE) && (values[1] & EXCLUSIVE) &&
	      (values[4] & EXCLUSIVE);
	ksft_test_result(
		ok,
		"pagemap reports each target leaf, hole, guard and PROT_NONE entry\n");
	ok = true;
	for (i = 0; i < NPAGE; i++) {
		uint64_t value;

		ok &= entries(fd, (uintptr_t)p + i * page, page, &value, 1) &&
		      value == values[i];
	}
	errno = 0;
	ok &= pread(fd, again, 7, 0) == -1 && errno == EINVAL;
	ksft_test_result(
		ok,
		"pagemap per-entry offsets agree with batch reads and reject short entries\n");
	ksft_test_result(
		scan(fd, (uintptr_t)p, page),
		"PAGEMAP_SCAN ranges, max_pages and alignment use target granules\n");
	clear = open_proc(getpid(), "clear_refs", O_WRONLY);
	ok = clear >= 0;
	for (i = 1; ok && i <= 3; i++) {
		char operation = '0' + i;

		ok &= write(clear, &operation, 1) == 1;
	}
	if (clear >= 0)
		close(clear);
	ok &= entries(fd, (uintptr_t)p, page, again, NPAGE) &&
	      !memcmp(values, again, sizeof(values)) && p[0] == 0x67 &&
	      p[page] == 0x67;
	ksft_test_result(
		ok,
		"clear_refs traverses short and split VMAs without changing mappings or bytes\n");
	ksft_test_result(
		remote_read(page),
		"opposite-ABI reader uses target pagemap and scan geometry\n");
	{
		pid_t child = fork();
		int status;

		if (!child) {
			uint64_t value = 0;
			int unprivileged;

			if (setuid(1000) || prctl(PR_SET_DUMPABLE, 1))
				_exit(2);
			unprivileged = open_proc(getpid(), "pagemap", O_RDONLY);
			ok = unprivileged >= 0 &&
			     entries(unprivileged, (uintptr_t)p, page, &value,
				     1) &&
			     (value & PRESENT) && !(value & FRAME);
			if (!ok)
				printf("# unprivileged fd=%d errno=%d flags=%#llx\n",
				       unprivileged, errno,
				       (unsigned long long)(value & ~FRAME));
			_exit(!ok);
		}
		ok = child > 0 && waitpid(child, &status, 0) == child &&
		     WIFEXITED(status) && !WEXITSTATUS(status);
		ksft_test_result(
			ok,
			"unprivileged pagemap reads retain flags and hide physical addresses\n");
	}
	close(fd);
	munmap(p, NPAGE * page);
	ksft_finished();
}
