// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

static size_t page;

static bool wait_child(pid_t pid, int sig)
{
	int status = 0;

	if (pid <= 0 || waitpid(pid, &status, 0) != pid)
		return false;
	if (sig)
		return WIFSIGNALED(status) && WTERMSIG(status) == sig;
	return WIFEXITED(status) && !WEXITSTATUS(status);
}

static void *anon(size_t length, int prot, int extra)
{
	void *p = mmap(NULL, length, prot, MAP_PRIVATE | MAP_ANONYMOUS | extra,
		       -1, 0);

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("anonymous mmap\n");
	return p;
}

static void discard_and_guards(void)
{
	unsigned char *p = anon(3 * page, PROT_READ | PROT_WRITE, 0);
	bool ok;
	pid_t child;

	memset(p, 0x61, 3 * page);
	ok = madvise(p + page, 1, MADV_DONTNEED) == 0;
	ok &= p[0] == 0x61 && p[page] == 0 && p[2 * page] == 0x61;
	ksft_test_result(ok,
			 "DONTNEED discards exactly one rounded user page\n");
	memset(p + page, 0x71, page);
	child = fork();
	if (!child) {
		ok = madvise(p + page, page, MADV_DONTNEED) == 0;
		_exit(!(ok && p[page] == 0 && p[0] == 0x61));
	}
	ksft_test_result(
		wait_child(child, 0) && p[page] == 0x71,
		"discard in a fork child leaves the parent quarter intact\n");
	ok = madvise(p + page, 1, MADV_GUARD_INSTALL) == 0;
	ok &= madvise(p + page, page, MADV_DONTNEED) == 0;
	child = fork();
	if (!child) {
		volatile unsigned char value = p[page];

		(void)value;
		_exit(1);
	}
	ksft_test_result(ok && wait_child(child, SIGSEGV),
			 "a user-page guard survives discard and fork\n");
	ok = madvise(p + page, 1, MADV_GUARD_REMOVE) == 0;
	ok &= p[page] == 0 && p[0] == 0x61 && p[2 * page] == 0x61;
	ksft_test_result(ok,
			 "guard removal restores only the requested page\n");
	munmap(p, 3 * page);
}

static void population(void)
{
	unsigned char *p = anon(5 * page, PROT_READ, 0), vec[5];
	bool ok;
	unsigned int i;

	ok = mprotect(p + 2 * page, page, PROT_READ | PROT_WRITE) == 0;
	ok &= madvise(p + 2 * page, 1, MADV_POPULATE_WRITE) == 0;
	ok &= mincore(p, 5 * page, vec) == 0;
	for (i = 0; i < 5; i++)
		ok &= vec[i] == (i == 2);
	ksft_test_result(
		ok, "POPULATE_WRITE faults only the selected user-page VMA\n");
	ok = madvise(p + page, 1, MADV_POPULATE_READ) == 0;
	ok &= mincore(p + page, page, vec) == 0 && vec[0] == 1;
	errno = 0;
	ok &= madvise(p, page, MADV_POPULATE_WRITE) == -1 && errno == EINVAL;
	ok &= mprotect(p + 3 * page, page, PROT_NONE) == 0;
	errno = 0;
	ok &= madvise(p + 3 * page, page, MADV_POPULATE_READ) == -1 &&
	      errno == EINVAL;
	ok &= munmap(p + 3 * page, page) == 0;
	errno = 0;
	ok &= madvise(p + 3 * page, page, MADV_POPULATE_READ) == -1 &&
	      errno == ENOMEM;
	ksft_test_result(
		ok, "POPULATE_READ honors permissions and reports holes\n");
	munmap(p, 5 * page);
	p = anon(5 * page, PROT_READ | PROT_WRITE, MAP_POPULATE);
	ok = mincore(p, 5 * page, vec) == 0;
	for (i = 0; i < 5; i++)
		ok &= vec[i] == 1;
	ksft_test_result(ok, "MAP_POPULATE materializes every user page\n");
	munmap(p, 5 * page);
}

static void fork_advice(void)
{
	unsigned char *p = anon(3 * page, PROT_READ | PROT_WRITE, 0);
	bool ok;
	pid_t child;

	memset(p, 0x58, 3 * page);
	ok = madvise(p + page, 1, MADV_WIPEONFORK) == 0;
	child = fork();
	if (!child)
		_exit(!(p[page] == 0 && p[0] == 0x58 && p[2 * page] == 0x58));
	ok &= wait_child(child, 0) && p[page] == 0x58;
	ksft_test_result(
		ok,
		"WIPEONFORK clears a single child page and preserves neighbors\n");
	ok = madvise(p + page, 1, MADV_KEEPONFORK) == 0;
	ok &= madvise(p + page, 1, MADV_DONTFORK) == 0;
	child = fork();
	if (!child) {
		unsigned char vec;

		errno = 0;
		_exit(!(mincore(p + page, page, &vec) == -1 &&
			errno == ENOMEM && p[0] == 0x58 &&
			p[2 * page] == 0x58));
	}
	ok &= wait_child(child, 0);
	ksft_test_result(ok, "DONTFORK omits only its selected user page\n");
	munmap(p, 3 * page);
}

static void remove_file(void)
{
	int fd = memfd_create("advice-file", 0);
	unsigned char *shared, *alias, *private;
	bool ok;
	size_t i;

	if (fd < 0 || ftruncate(fd, 5 * page))
		ksft_exit_fail_msg("memfd setup\n");
	shared =
		mmap(NULL, 5 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	alias = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		     page);
	private = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd,
		       page);
	if (shared == MAP_FAILED || alias == MAP_FAILED ||
	    private == MAP_FAILED)
		ksft_exit_fail_msg("file mmap\n");
	for (i = 0; i < 5 * page; i++)
		shared[i] = 0x40 + i / page;
	private[page] = 0x79;
	ok = madvise(alias + page, 1, MADV_REMOVE) == 0;
	for (i = 0; i < 5 * page; i++)
		ok &= shared[i] == (i / page == 2 ? 0 : 0x40 + i / page);
	ok &= private[page] == 0x79;
	ksft_test_result(
		ok,
		"REMOVE uses the file byte offset and preserves private COW\n");
	ok = madvise(alias, 3 * page, MADV_WILLNEED) == 0;
	ksft_test_result(
		ok, "WILLNEED accepts a file range with the process granule\n");
	munmap(shared, 5 * page);
	munmap(alias, 3 * page);
	munmap(private, 3 * page);
	close(fd);
}

int main(int argc, char **argv)
{
	const struct rlimit no_core = { 0, 0 };

	page = getauxval(AT_PAGESZ);
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--expect-4k")) ||
	    page != (argc == 2 ? 4096UL : 16384UL))
		ksft_exit_fail_msg("unexpected process page ABI: %zu\n", page);
	ksft_print_msg("process page size: %zu\n", page);
	if (setrlimit(RLIMIT_CORE, &no_core))
		return 1;
	ksft_print_header();
	ksft_set_plan(11);
	discard_and_guards();
	population();
	fork_advice();
	remove_file();
	ksft_finished();
}
