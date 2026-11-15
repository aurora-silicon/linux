// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

static size_t page;

static void residency(void)
{
	unsigned char *p, vec[6];
	bool ok;
	unsigned int i;

	p = mmap(NULL, 5 * page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		ksft_exit_fail_msg("anonymous mmap\n");
	memset(vec, 0x55, sizeof(vec));
	ok = mincore(p, 5 * page - 1, vec) == 0;
	for (i = 0; i < 5; i++)
		ok &= vec[i] == 0;
	ok &= vec[5] == 0x55;
	ksft_test_result(
		ok, "mincore emits exactly one byte per unfaulted user page\n");
	ok = mprotect(p + 2 * page, page, PROT_READ | PROT_WRITE) == 0;
	p[2 * page] = 0x37;
	ok &= mincore(p, 5 * page, vec) == 0;
	for (i = 0; i < 5; i++)
		ok &= vec[i] == (i == 2);
	ksft_test_result(
		ok,
		"mincore reports a separately faulted VMA among cold neighbors\n");
	ok = munmap(p + 3 * page, page) == 0;
	errno = 0;
	ok &= mincore(p, 5 * page, vec) == -1 && errno == ENOMEM;
	errno = 0;
	ok &= mincore(p + 1, page, vec) == -1 && errno == EINVAL;
	ksft_test_result(ok,
			 "mincore rejects an unaligned start and a VMA hole\n");
	munmap(p, 5 * page);
}

static void file_residency(void)
{
	const size_t native = 16384, count = 3 * native / page;
	unsigned char *p, *guard, *vec, byte = 0x7b;
	int fd = memfd_create("residency", 0);
	bool ok;
	size_t i;

	if (fd < 0 || ftruncate(fd, 4 * native) ||
	    pwrite(fd, &byte, 1, native) != 1)
		ksft_exit_fail_msg("memfd setup\n");
	p = mmap(NULL, 3 * native, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 page);
	guard = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED || guard == MAP_FAILED ||
	    mprotect(guard + page, page, PROT_NONE))
		ksft_exit_fail_msg("file/vector mmap\n");
	vec = guard + page - count;
	memset(vec, 0x55, count);
	ok = mincore(p, 3 * native, vec) == 0;
	for (i = 0; i < count; i++)
		ok &= vec[i] == ((page + i * page) / native == 1);
	ksft_test_result(
		ok,
		"unmapped file leaves share residency at native cache indices\n");
	ok = p[native - page] == byte && mincore(p, 3 * native, vec) == 0;
	for (i = 0; i < count; i++)
		ok &= vec[i] == ((page + i * page) / native == 1);
	ksft_test_result(
		ok,
		"present and cache-only file residency agree at a guarded vector boundary\n");
	ok = msync(p + page, 1, MS_SYNC) == 0;
	errno = 0;
	ok &= msync(p + page + 1, page, MS_SYNC) == -1 && errno == EINVAL;
	ok &= munmap(p + page, page) == 0;
	errno = 0;
	ok &= msync(p, 3 * page, MS_SYNC) == -1 && errno == ENOMEM;
	ksft_test_result(ok,
			 "msync uses process alignment and reports holes\n");
	munmap(p, 3 * native);
	munmap(guard, 2 * page);
	close(fd);
}

static void large_vector(void)
{
	const size_t count = 16384 + 3;
	unsigned char *p = mmap(NULL, count * page, PROT_READ,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1,
				0);
	unsigned char *vec = malloc(count + 1);
	bool ok;
	size_t i;

	if (p == MAP_FAILED || !vec)
		ksft_exit_fail_msg("large vector allocation\n");
	memset(vec, 0x55, count + 1);
	ok = mincore(p, count * page - 7, vec) == 0;
	for (i = 0; i < count; i++)
		ok &= vec[i] == 0;
	ok &= vec[count] == 0x55;
	ksft_test_result(
		ok, "mincore advances across multiple native output buffers\n");
	free(vec);
	munmap(p, count * page);
}

static void seals(void)
{
	unsigned char *p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	bool ok;
	pid_t pid;
	int status = 0;

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("seal mmap\n");
	ok = syscall(SYS_mseal, p + page, 1UL, 0UL) == 0;
	errno = 0;
	ok &= mprotect(p + page, page, PROT_READ) == -1 && errno == EPERM;
	errno = 0;
	ok &= munmap(p + page, page) == -1 && errno == EPERM;
	ok &= mprotect(p, page, PROT_READ) == 0;
	ok &= mprotect(p + 2 * page, page, PROT_READ) == 0;
	ksft_test_result(
		ok,
		"a one-byte seal covers only its user page and protects it\n");
	pid = fork();
	if (!pid)
		_exit(!(munmap(p + page, page) == -1 && errno == EPERM));
	ok = pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	     !WEXITSTATUS(status);
	ksft_test_result(ok, "fork inherits the sealed user page\n");
	munmap(p, page);
	munmap(p + 2 * page, page);
	/* The sealed VMA is released by process exit. */
}

int main(int argc, char **argv)
{
	page = getauxval(AT_PAGESZ);
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--expect-4k")) ||
	    page != (argc == 2 ? 4096UL : 16384UL))
		ksft_exit_fail_msg("unexpected process page ABI: %zu\n", page);
	ksft_print_msg("process page size: %zu\n", page);
	ksft_print_header();
	ksft_set_plan(9);
	residency();
	file_residency();
	large_vector();
	seals();
	ksft_finished();
}
