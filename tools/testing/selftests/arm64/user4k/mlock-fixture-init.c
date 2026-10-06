// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

static long locked_kb(void)
{
	char line[256];
	long value = -1;
	FILE *f = fopen("/proc/self/status", "re");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "VmLck: %ld", &value) == 1)
			break;
	fclose(f);
	return value;
}

static bool wait_ok(pid_t pid)
{
	int status = 0;

	return pid > 0 && waitpid(pid, &status, 0) == pid &&
	       WIFEXITED(status) && !WEXITSTATUS(status);
}

static bool remap_locked(unsigned long ps)
{
	unsigned char *p, *q, *target;
	unsigned long kb = ps / 1024;
	bool ok;

	p = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED || mlock(p, 2 * ps))
		ksft_exit_fail_msg("mlock remap setup\n");
	memset(p, 0x37, 2 * ps);
	ok = locked_kb() == (long)(2 * kb);
	q = mremap(p, 2 * ps, 4 * ps, MREMAP_MAYMOVE);
	if (q == MAP_FAILED)
		ksft_exit_fail_msg("locked remap grow: %s\n", strerror(errno));
	ok &= locked_kb() == (long)(4 * kb);
	for (unsigned long i = 0; i < 4 * ps; i++)
		ok &= q[i] == (i < 2 * ps ? 0x37 : 0);
	p = mremap(q, 4 * ps, 3 * ps, 0);
	if (p == MAP_FAILED)
		ksft_exit_fail_msg("locked remap shrink: %s\n",
				   strerror(errno));
	ok &= locked_kb() == (long)(3 * kb);
	p[2 * ps] = 0xb6;
	ok &= !munmap(p + ps, ps) && locked_kb() == (long)(2 * kb);
	target = mmap(NULL, ps, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (target == MAP_FAILED)
		ksft_exit_fail_msg("locked remap target\n");
	q = mremap(p, ps, ps, MREMAP_MAYMOVE | MREMAP_FIXED, target);
	if (q == MAP_FAILED)
		ksft_exit_fail_msg("locked remap move: %s\n", strerror(errno));
	ok &= q == target && locked_kb() == (long)(2 * kb);
	for (unsigned long i = 0; i < ps; i++)
		ok &= q[i] == 0x37;
	ok &= p[2 * ps] == 0xb6;
	ok &= !munmap(q, ps) && locked_kb() == (long)kb;
	ok &= !munmap(p + 2 * ps, ps) && locked_kb() == 0;
	return ok;
}

static int exercise(void)
{
	unsigned long ps = getauxval(AT_PAGESZ), kb = ps / 1024;
	unsigned char *p, *q, residency[4];
	int gate[2];
	pid_t pid;
	bool ok;

	ksft_print_header();
	ksft_set_plan(9);
	p = mmap(NULL, 32 * ps, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED || locked_kb() != 0)
		ksft_exit_fail_msg("initial mapping/accounting\n");
	ok = !mlock(p + ps + 17, ps - 32) && locked_kb() == (long)kb;
	ok &= !mincore(p + ps, ps, residency) && residency[0] & 1;
	p[ps] = 0x51;
	ksft_test_result(
		ok, "%luK unaligned mlock uses exactly one user leaf\n", kb);
	ok = !mlock(p + ps, ps) && locked_kb() == (long)kb;
	ok &= !mlock(p + 2 * ps, ps) && locked_kb() == (long)(2 * kb);
	ok &= !munlock(p + ps, ps) && locked_kb() == (long)kb;
	p[2 * ps] = 0x62;
	errno = 0;
	ok &= madvise(p + 2 * ps, ps, MADV_PAGEOUT) == -1 && errno == EINVAL &&
	      p[2 * ps] == 0x62;
	ok &= !munlock(p + 2 * ps, ps) && locked_kb() == 0;
	ksft_test_result(
		ok, "%luK repeat lock and independent adjacent unlock\n", kb);
	ok = !mlock2(p + 4 * ps, 4 * ps, MLOCK_ONFAULT) &&
	     locked_kb() == (long)(4 * kb);
	ok &= !mincore(p + 4 * ps, 4 * ps, residency);
	for (unsigned int i = 0; i < 4; i++)
		ok &= !(residency[i] & 1);
	p[4 * ps] = 0x74;
	ok &= !mincore(p + 4 * ps, ps, residency) && residency[0] & 1;
	ok &= !munlock(p + 4 * ps, 4 * ps) && locked_kb() == 0;
	ksft_test_result(
		ok, "%luK MLOCK_ONFAULT accounting and deferred faults\n", kb);
	q = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
	ok = q != MAP_FAILED && locked_kb() == (long)(4 * kb);
	if (q == MAP_FAILED || pipe(gate))
		ksft_exit_fail_msg("locked mmap/fork setup\n");
	memset(q, 0x8a, 4 * ps);
	pid = fork();
	if (!pid) {
		char token;

		close(gate[1]);
		if (read(gate[0], &token, 1) != 1 || locked_kb() != 0)
			_exit(2);
		for (unsigned long i = 0; i < 4 * ps; i++)
			if (q[i] != 0x8a)
				_exit(3);
		_exit(0);
	}
	close(gate[0]);
	memset(q + ps, 0x9b, ps);
	ok &= write(gate[1], "x", 1) == 1;
	close(gate[1]);
	ok &= wait_ok(pid) && locked_kb() == (long)(4 * kb);
	ok &= !munmap(q, 4 * ps) && locked_kb() == 0;
	ksft_test_result(
		ok, "%luK MAP_LOCKED, fork noninheritance and parent COW\n",
		kb);
	ok = !mlockall(MCL_FUTURE | MCL_ONFAULT);
	q = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		 -1, 0);
	ok &= q != MAP_FAILED && locked_kb() == (long)kb;
	if (q != MAP_FAILED)
		*q = 0xaf;
	ok &= !munlockall() && locked_kb() == 0;
	if (q != MAP_FAILED)
		munmap(q, ps);
	ksft_test_result(ok, "%luK future on-fault locks and munlockall\n", kb);
	pid = fork();
	if (!pid) {
		struct rlimit limit = { ps, ps };

		if (setrlimit(RLIMIT_MEMLOCK, &limit) || setuid(1000) ||
		    mlock(p, ps))
			_exit(4);
		errno = 0;
		if (mlock(p + ps, ps) != -1 || errno != ENOMEM ||
		    locked_kb() != (long)kb)
			_exit(5);
		_exit(0);
	}
	ksft_test_result(
		wait_ok(pid),
		"%luK unprivileged RLIMIT_MEMLOCK in user-page units\n", kb);
	pid = fork();
	if (!pid) {
		if (mlockall(MCL_CURRENT) || locked_kb() <= (long)kb ||
		    munlockall() || locked_kb() != 0)
			_exit(6);
		_exit(0);
	}
	ksft_test_result(wait_ok(pid), "%luK lock/unlock current mappings\n",
			 kb);
	{
		unsigned long huge = ((1UL << 31) + 1) * ps;

		q = mmap(NULL, huge, PROT_NONE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		ok = q != MAP_FAILED;
		if (q != MAP_FAILED) {
			ok &= !mlock2(q, huge, MLOCK_ONFAULT) &&
			      locked_kb() == (long)(huge / 1024);
			ok &= !munlock(q, huge) && locked_kb() == 0;
			munmap(q, huge);
		}
		ksft_test_result(
			ok,
			"%luK deferred lock count beyond signed 32-bit leaves\n",
			kb);
	}
	ksft_test_result(
		remap_locked(ps),
		"%luK locked grow/shrink/move and partial unmap accounting\n",
		kb);
	munmap(p, 32 * ps);
	ksft_print_cnts();
	return ksft_get_fail_cnt() ? 1 : 0;
}

int main(int argc, char **argv)
{
	bool ok = true;
	unsigned long native = getauxval(AT_PAGESZ);
	unsigned long granules[] = { native, 4096, 16384 };
	unsigned int nr = native > 16384 ? 3 : 2;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 4 && !strcmp(argv[1], "--test") && getppid() == 1) {
		if (getauxval(AT_PAGESZ) != strtoul(argv[2], NULL, 10))
			return 42;
		return exercise();
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Run only as PID 1 in a disposable VM\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL))
		return 3;
	for (unsigned int small = 0; small < nr; small++) {
		pid_t pid = fork();

		if (!pid) {
			char size[32], host_size[32];

			snprintf(size, sizeof(size), "%lu", granules[small]);
			snprintf(host_size, sizeof(host_size), "%lu", native);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, granules[small],
				  0UL, 0UL, 0UL))
				_exit(7);
			execl("/init", "/init", "--test", size, host_size,
			      NULL);
			_exit(8);
		}
		ok &= wait_ok(pid);
	}
	printf("%s - mlock fixture complete\n", ok ? "ok" : "not ok");
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
