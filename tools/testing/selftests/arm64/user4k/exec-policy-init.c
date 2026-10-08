// SPDX-License-Identifier: GPL-2.0-only
/* Disposable guest PID1: per-thread one-shot and inherited exec preferences. */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(x)                                                     \
	do {                                                           \
		if (!(x)) {                                            \
			fprintf(stderr, "FAIL line %d: %s errno=%d\n", \
				__LINE__, #x, errno);                  \
			exit(1);                                       \
		}                                                      \
	} while (0)

static int policy(int op, unsigned long value)
{
	return prctl(op, value, 0UL, 0UL, 0UL);
}

static void state(unsigned long pages, unsigned long preferred, int pending)
{
	REQUIRE(getauxval(AT_PAGESZ) == pages);
	REQUIRE(policy(PR_AURORA_GET_DEFAULT_PAGE_SIZE, 0) == (int)preferred);
	REQUIRE(policy(PR_AURORA_GET_EXEC_PAGE_SIZE, 0) == pending);
}

static void chain(int stage, unsigned long native, unsigned long preferred)
{
	char s[32], n[32], p[32];

	snprintf(s, sizeof(s), "%d", stage);
	snprintf(n, sizeof(n), "%lu", native);
	snprintf(p, sizeof(p), "%lu", preferred);
	execl("/init", "/init", "--chain", s, n, p, NULL);
	REQUIRE(0);
}

static void *thread_policy(void *arg)
{
	unsigned long preferred = *(unsigned long *)arg;

	REQUIRE(policy(PR_AURORA_GET_DEFAULT_PAGE_SIZE, 0) == (int)preferred);
	REQUIRE(policy(PR_AURORA_GET_EXEC_PAGE_SIZE, 0) == (int)preferred);
	REQUIRE(!policy(PR_AURORA_SET_DEFAULT_PAGE_SIZE, 0));
	REQUIRE(!policy(PR_AURORA_SET_EXEC_PAGE_SIZE, 0));
	state(getauxval(AT_PAGESZ), 0, 0);
	return NULL;
}

static void *thread_exec(void *arg)
{
	unsigned long *sizes = arg;

	REQUIRE(!policy(PR_AURORA_SET_DEFAULT_PAGE_SIZE, sizes[1]));
	chain(0, sizes[0], sizes[1]);
	return NULL;
}

static int wait_child(pid_t pid)
{
	int status;

	REQUIRE(pid > 0);
	REQUIRE(waitpid(pid, &status, 0) == pid);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int run_preference(unsigned long native, unsigned long preferred)
{
	pid_t pid = fork();

	if (!pid) {
		pthread_t thread;
		REQUIRE(!policy(PR_AURORA_SET_DEFAULT_PAGE_SIZE, preferred));
		REQUIRE(!policy(PR_AURORA_SET_EXEC_PAGE_SIZE, preferred));
		REQUIRE(!pthread_create(&thread, NULL, thread_policy,
					&preferred));
		REQUIRE(!pthread_join(thread, NULL));
		state(native, preferred, preferred);
		/* Rejected values and reserved arguments must preserve both policies. */
		REQUIRE(policy(PR_AURORA_SET_DEFAULT_PAGE_SIZE, 8192) == -1 &&
			errno == EINVAL);
		REQUIRE(policy(PR_AURORA_SET_EXEC_PAGE_SIZE, 8192) == -1 &&
			errno == EINVAL);
		int ops[] = { PR_AURORA_SET_DEFAULT_PAGE_SIZE,
			      PR_AURORA_GET_DEFAULT_PAGE_SIZE,
			      PR_AURORA_SET_EXEC_PAGE_SIZE,
			      PR_AURORA_GET_EXEC_PAGE_SIZE };
		for (unsigned int i = 0; i < sizeof(ops) / sizeof(ops[0]);
		     i++) {
			REQUIRE(prctl(ops[i], 0UL, 1UL, 0UL, 0UL) == -1 &&
				errno == EINVAL);
			REQUIRE(prctl(ops[i], 0UL, 0UL, 1UL, 0UL) == -1 &&
				errno == EINVAL);
			REQUIRE(prctl(ops[i], 0UL, 0UL, 0UL, 1UL) == -1 &&
				errno == EINVAL);
		}
		REQUIRE(policy(PR_AURORA_GET_DEFAULT_PAGE_SIZE, 1) == -1 &&
			errno == EINVAL);
		REQUIRE(policy(PR_AURORA_GET_EXEC_PAGE_SIZE, 1) == -1 &&
			errno == EINVAL);
		execl("/does-not-exist", "/does-not-exist", NULL);
		REQUIRE(errno == ENOENT);
		state(native, preferred, preferred);
		chain(0, native, preferred);
	}
	return wait_child(pid);
}

static int compat(unsigned long native, unsigned long preferred, int no4k)
{
	pid_t pid = fork();

	if (!pid) {
		char n[32], p[32], e[32];
		REQUIRE(!policy(PR_AURORA_SET_DEFAULT_PAGE_SIZE, preferred));
		REQUIRE(!policy(PR_AURORA_SET_EXEC_PAGE_SIZE, native));
		snprintf(n, sizeof(n), "%lu", native);
		snprintf(p, sizeof(p), "%lu", preferred);
		snprintf(e, sizeof(e), "%lu", no4k ? native : 4096UL);
		execl("/compat-policy", "/compat-policy", n, p, e, NULL);
		REQUIRE(0);
	}
	return wait_child(pid);
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ);
	int failed = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 5 && !strcmp(argv[1], "--chain")) {
		int stage = atoi(argv[2]);
		unsigned long n = strtoul(argv[3], NULL, 10);
		unsigned long p = strtoul(argv[4], NULL, 10);

		state(stage == 2 || stage == 5 ? n : p, stage < 4 ? p : 0, 0);
		if (stage == 5)
			return 0;
		if (stage == 1) {
			REQUIRE(!policy(PR_AURORA_SET_EXEC_PAGE_SIZE, 0));
			state(p, p, n);
		}
		if (stage == 3) {
			REQUIRE(!policy(PR_AURORA_SET_DEFAULT_PAGE_SIZE, 0));
			REQUIRE(!policy(PR_AURORA_SET_EXEC_PAGE_SIZE, p));
		}
		chain(stage + 1, n, p);
	}
	if (argc == 4 && !strcmp(argv[1], "--compat-return")) {
		unsigned long n = strtoul(argv[2], NULL, 10);
		unsigned long p = strtoul(argv[3], NULL, 10);
		state(p ? p : n, p, 0);
		return 0;
	}
	REQUIRE(getpid() == 1);
	REQUIRE(!mount("proc", "/proc", "proc", 0, NULL));
	state(native, 0, 0);
	int no4k = argc == 2 && !strcmp(argv[1], "--no4k");
	unsigned long preferences[] = { 4096, 16384 };
	for (unsigned int i = 0; i < 2; i++) {
		unsigned long p = preferences[i];
		if (p >= native || (no4k && p == 4096))
			continue;
		int ok = run_preference(native, p);
		printf("%s - exec policy native=%lu alternative=%lu\n",
		       ok ? "ok" : "not ok", native, p);
		failed |= !ok;
		pid_t pid = fork();
		if (!pid) {
			pthread_t thread;
			unsigned long sizes[] = { native, p };
			REQUIRE(!pthread_create(&thread, NULL, thread_exec,
						sizes));
			for (;;)
				pause();
		}
		ok = wait_child(pid);
		printf("%s - non-leader exec alternative=%lu\n",
		       ok ? "ok" : "not ok", p);
		failed |= !ok;
		ok = compat(native, p, no4k);
		printf("%s - compat policy alternative=%lu no4k=%d\n",
		       ok ? "ok" : "not ok", p, no4k);
		failed |= !ok;
	}
	if (no4k) {
		int ok = policy(PR_AURORA_SET_DEFAULT_PAGE_SIZE, 4096) == -1 &&
			 errno == EOPNOTSUPP;
		ok &= policy(PR_AURORA_SET_EXEC_PAGE_SIZE, 4096) == -1 &&
		      errno == EOPNOTSUPP;
		ok &= compat(native, 0, 1);
		printf("%s - no4k preserves native compat restrictions\n",
		       ok ? "ok" : "not ok");
		failed |= !ok;
	}
	state(native, 0, 0);
	printf("EXEC POLICY %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	return failed;
}
