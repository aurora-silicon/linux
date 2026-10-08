// SPDX-License-Identifier: GPL-2.0-only
/* PID-1 runner for the experimental one-shot exec ABI in a disposable VM. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

static int exec_size(unsigned long size)
{
	return prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, size, 0UL, 0UL, 0UL);
}

static int pending_size(void)
{
	return prctl(PR_AURORA_GET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL);
}

static int run(const char *path, const char *arg, unsigned long size)
{
	pid_t pid = fork(), got;
	int status = 0;

	if (!pid) {
		if (size && exec_size(size)) {
			perror("set exec page size");
			_exit(125);
		}
		execl(path, path, arg, NULL);
		perror(path);
		_exit(126);
	}
	if (pid < 0)
		return -1;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	printf("# child %s %s: wait=%d status=%#x\n", path, arg ? arg : "", got,
	       status);
	return got == pid && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

static int argument_exec(unsigned long size)
{
	pid_t pid = fork(), got;
	int status = 0;

	if (!pid) {
		char *payload = malloc(140000), expected[16];
		unsigned int i;

		if (!payload || exec_size(size))
			_exit(120);
		for (i = 0; i < 139999; i++)
			payload[i] = 'a' + i % 23;
		payload[139999] = 0;
		if (size == 4096) {
			execl("/init", "/init", "--too-long", payload, NULL);
			if (errno != E2BIG || pending_size() != 4096)
				_exit(121);
		}
		payload[120000] = 0;
		if (setenv("ARG_PAYLOAD", payload, 1))
			_exit(122);
		snprintf(expected, sizeof(expected), "%lu",
			 size ? size : 16384UL);
		execl("/init", "/init", "--args-check", expected, payload,
		      NULL);
		perror("argument exec");
		_exit(123);
	}
	if (pid < 0)
		return -1;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	printf("# large arguments from %lu to %lu: status=%#x\n",
	       getauxval(AT_PAGESZ), size ? size : 16384UL, status);
	return got == pid && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

int main(int argc, char **argv)
{
	bool ok;
	int failures = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 4 && !strcmp(argv[1], "--args-check")) {
		const char *env = getenv("ARG_PAYLOAD");
		unsigned int i;

		if (getauxval(AT_PAGESZ) != strtoul(argv[2], NULL, 10) ||
		    pending_size() != 0 || strlen(argv[3]) != 120000 || !env ||
		    strlen(env) != 120000)
			return 1;
		for (i = 0; i < 120000; i++)
			if (argv[3][i] != 'a' + i % 23 || env[i] != argv[3][i])
				return 2;
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "--args-from-4k")) {
		if (getauxval(AT_PAGESZ) != 4096)
			return 1;
		return argument_exec(4096) || argument_exec(0);
	}
	if (argc == 2 && !strcmp(argv[1], "--exec-cycle")) {
		if (getauxval(AT_PAGESZ) != 4096 || pending_size() != 0)
			return 1;
		/* A successful exec consumes the request; ordinary exec is native. */
		execl("/init", "/init", "--native-after-4k", NULL);
		return 2;
	}
	if (argc == 2 && !strcmp(argv[1], "--native-after-4k"))
		return getauxval(AT_PAGESZ) != 16384 || pending_size() != 0;
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Run only as /init in a disposable VM.\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) && errno != EBUSY)
		return 3;
	printf("# exec fixture: native AT_PAGESZ=%lu\n", getauxval(AT_PAGESZ));
	failures += run("/page-contract", NULL, 0) != 0;
	ok = pending_size() == 0;
	errno = 0;
	ok &= exec_size(8192) == -1 && errno == EINVAL;
	ok &= prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, 4096UL, 1UL, 0UL, 0UL) == -1;
	ok &= exec_size(4096) == 0 && pending_size() == 4096;
	execl("/does-not-exist", "/does-not-exist", NULL);
	ok &= errno == ENOENT && pending_size() == 4096;
	ok &= getauxval(AT_PAGESZ) == 16384;
	printf("%s - request validation and failed exec preserve live geometry\n",
	       ok ? "ok" : "not ok");
	failures += !ok;
	/* fork inherits the pending request; successful child exec consumes it. */
	failures += run("/page-contract-4k", "--expect-4k", 0) != 0;
	ok = pending_size() == 4096 && exec_size(0) == 0 && pending_size() == 0;
	printf("%s - child exec leaves parent request intact; reset works\n",
	       ok ? "ok" : "not ok");
	failures += !ok;
	failures += run("/page-contract-dynamic-4k", "--expect-4k", 4096) != 0;
	failures += run("/init", "--exec-cycle", 4096) != 0;
	failures += run("/proc-memory", NULL, 0) != 0;
	failures += run("/proc-memory", "--expect-4k", 4096) != 0;
	failures += run("/proc-pages", NULL, 0) != 0;
	failures += run("/proc-pages", "--expect-4k", 4096) != 0;
	failures += run("/advice", NULL, 0) != 0;
	failures += run("/advice", "--expect-4k", 4096) != 0;
	failures += run("/memory-interfaces", NULL, 0) != 0;
	failures += run("/memory-interfaces", "--expect-4k", 4096) != 0;
	failures += run("/mixed-process", NULL, 0) != 0;
	failures += run("/mixed-process", NULL, 4096) != 0;
	failures += argument_exec(4096) != 0;
	failures += run("/init", "--args-from-4k", 4096) != 0;
	failures += run("/page-contract", NULL, 0) != 0;
	printf("%s - exec fixture complete: failures=%d\n",
	       failures ? "not ok" : "ok", failures);
	reboot(RB_POWER_OFF);
	return failures ? 1 : 0;
}
