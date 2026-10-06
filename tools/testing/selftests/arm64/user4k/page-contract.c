// SPDX-License-Identifier: GPL-2.0-only
/*
 * Run unchanged under the current process ABI, including native 4K/16K/64K.
 * Explicit --expect-{4k,16k,64k} options assert the selected size; they never
 * launch a different ABI or emulate it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "../../kselftest.h"

#define X18_SENTINEL UINT64_C(0xfedcba9876543210)

extern long x18_syscall(long nr, long arg0, long arg1, long arg2,
			uint64_t *seen);

static volatile sig_atomic_t signal_seen;
static uint64_t signal_x18;

static void x18_signal(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;

	(void)sig;
	(void)info;
	signal_x18 = uc->uc_mcontext.regs[18];
	signal_seen = 1;
	/* rt_sigreturn must restore the interrupted value, not this value. */
	/* Built with -ffixed-x18: the compiler never allocates this register. */
	asm volatile("mov x18, xzr");
}

static bool wait_for(pid_t child, int expected_signal)
{
	int status;
	pid_t ret;

	if (child < 0)
		return false;
	do {
		ret = waitpid(child, &status, 0);
	} while (ret < 0 && errno == EINTR);
	if (ret != child)
		return false;
	if (expected_signal)
		return WIFSIGNALED(status) &&
		       WTERMSIG(status) == expected_signal;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool access_faults(volatile unsigned char *ptr, bool write)
{
	pid_t child = fork();

	if (!child) {
		if (write)
			*ptr = 0x5a;
		else
			(void)*ptr;
		_exit(1);
	}
	return wait_for(child, SIGSEGV);
}

static void private_mappings(size_t page)
{
	unsigned char *p = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	bool ok;
	pid_t child;

	if (p == MAP_FAILED)
		ksft_exit_fail_msg("anonymous mmap: %s\n", strerror(errno));
	memset(p, 0x41, 4 * page);
	if (page > 4096) {
		errno = 0;
		ok = mprotect(p + 4096, 4096, PROT_READ) == -1 &&
		     errno == EINVAL;
	} else {
		ok = mprotect(p + page, page, PROT_READ) == 0;
	}
	ksft_test_result(ok,
			 "mapping alignment agrees with the selected ABI\n");
	ok = mprotect(p + page, page, PROT_READ) == 0 &&
	     mprotect(p + 2 * page, page, PROT_NONE) == 0;
	ksft_test_result(
		ok && access_faults(p + page, true) &&
			access_faults(p + 2 * page, false),
		"read-only and inaccessible neighbors fault independently\n");
	p[0] = 0x10;
	p[3 * page] = 0x30;
	ksft_test_result(
		p[page] == 0x41 && p[0] == 0x10 && p[3 * page] == 0x30,
		"neighbor permissions do not remove writable mappings\n");
	child = fork();
	if (!child) {
		p[0] = 0x99;
		_exit(p[3 * page] != 0x30);
	}
	ksft_test_result(wait_for(child, 0) && p[0] == 0x10,
			 "fork preserves private COW isolation\n");
	ok = munmap(p + page, page) == 0;
	ksft_test_result(ok && access_faults(p + page, false) && p[0] == 0x10 &&
				 p[3 * page] == 0x30,
			 "single-page unmap preserves surrounding mappings\n");
	munmap(p, 4 * page);
}

static void shared_mappings(size_t page)
{
	int fd = memfd_create("page-contract", MFD_CLOEXEC);
	unsigned char *a, *b;
	bool ok = true;
	pid_t child;
	unsigned int i;

	if (fd < 0 || ftruncate(fd, 4 * page))
		ksft_exit_fail_msg("memfd: %s\n", strerror(errno));
	a = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	b = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (a == MAP_FAILED || b == MAP_FAILED)
		ksft_exit_fail_msg("shared mmap: %s\n", strerror(errno));
	for (i = 0; i < 4; i++) {
		a[i * page] = 0x50 + i;
		ok &= b[i * page] == 0x50 + i;
	}
	ksft_test_result(ok, "shared aliases expose the same bytes\n");
	ok = mprotect(a + page, page, PROT_READ) == 0;
	child = fork();
	if (!child) {
		b[page] = 0x73;
		_exit(a[page] != 0x73);
	}
	ok &= wait_for(child, 0);
	ksft_test_result(
		ok && a[page] == 0x73 && access_faults(a + page, true),
		"shared writes cross fork while alias permissions stay local\n");
	munmap(a, 4 * page);
	munmap(b, 4 * page);
	close(fd);
}

static void x18_contract(void)
{
	struct sigaction sa = { .sa_sigaction = x18_signal,
				.sa_flags = SA_SIGINFO };
	struct timespec pause = { .tv_nsec = 1000000 };
	struct rusage before, after;
	uint64_t seen;
	long ret;
	bool ok;

	ret = x18_syscall(SYS_getpid, 0, 0, 0, &seen);
	ksft_test_result(ret == getpid() && seen == X18_SENTINEL,
			 "x18 survives a raw syscall\n");
	ok = getrusage(RUSAGE_SELF, &before) == 0;
	ret = x18_syscall(SYS_nanosleep, (long)&pause, 0, 0, &seen);
	ok &= getrusage(RUSAGE_SELF, &after) == 0;
	ksft_test_result(ok && ret == 0 && seen == X18_SENTINEL &&
				 after.ru_nvcsw > before.ru_nvcsw,
			 "x18 survives a measured voluntary context switch\n");
	sigemptyset(&sa.sa_mask);
	ok = sigaction(SIGUSR1, &sa, NULL) == 0;
	if (ok)
		ret = x18_syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
				  SIGUSR1, &seen);
	ksft_test_result(
		ok && ret == 0 && signal_seen && signal_x18 == X18_SENTINEL &&
			seen == X18_SENTINEL,
		"signal frame and sigreturn preserve x18 after handler clobber\n");
}

int main(int argc, char **argv)
{
	const struct rlimit no_core = { 0, 0 };
	size_t expected = getauxval(AT_PAGESZ);
	int result;

	if (argc == 2 && !strcmp(argv[1], "--expect-4k")) {
		expected = 4096;
	} else if (argc == 2 && !strcmp(argv[1], "--expect-16k")) {
		expected = 16384;
	} else if (argc == 2 && !strcmp(argv[1], "--expect-64k")) {
		expected = 65536;
	} else if (argc != 1) {
		fprintf(stderr,
			"usage: %s [--expect-4k|--expect-16k|--expect-64k]\n",
			argv[0]);
		return KSFT_FAIL;
	}
	ksft_print_header();
	ksft_set_plan(11);
	if (expected != 4096 && expected != 16384 && expected != 65536)
		ksft_exit_skip("unsupported process page size: %zu\n",
			       expected);
	if (sysconf(_SC_PAGESIZE) != (long)expected ||
	    getauxval(AT_PAGESZ) != expected)
		ksft_exit_fail_msg(
			"expected %zu-byte ABI; sysconf=%ld AT_PAGESZ=%lu\n",
			expected, sysconf(_SC_PAGESIZE), getauxval(AT_PAGESZ));
	ksft_test_result_pass(
		"process reports the expected %zu-byte page ABI\n", expected);
	if (setrlimit(RLIMIT_CORE, &no_core))
		ksft_exit_fail_msg("disable child core dumps: %s\n",
				   strerror(errno));
	private_mappings(expected);
	shared_mappings(expected);
	x18_contract();
	ksft_print_cnts();
	result = ksft_get_fail_cnt() ? KSFT_FAIL : KSFT_PASS;
	/* This static executable can also be /init in a disposable test VM. */
	if (getpid() == 1) {
		fflush(NULL);
		reboot(RB_POWER_OFF);
	}
	return result;
}
