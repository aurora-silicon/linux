// SPDX-License-Identifier: GPL-2.0-only
/*
 * KSM and 4K processes on a 16K kernel. KSM hashes, compares and replaces
 * whole native pages, so a process with another user granule stays out of
 * it: PR_SET_MEMORY_MERGE fails there with EOPNOTSUPP, as MADV_MERGEABLE
 * does, and the setting of a native process does not carry over into an
 * exec that selects another granule, nor back into a native exec after
 * that. A native exec keeps it. ksmd need not run.
 *
 * On a kernel that lets a 4K process into KSM, run this only with KSM
 * stopped (/sys/kernel/mm/ksm/run 0): a running ksmd would merge the 4K
 * process's pages. A 4K process that finds itself let in backs out at once,
 * before ksmd can have compared its pages twice, which a merge needs.
 *
 * Each case runs in a child that execs this program again in its role and
 * reports through its exit status.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

#define NATIVE 16384UL
#define LEAF 4096UL

/* A yes/no field of /proc/self/ksm_stat: 1, 0, or -1 where there is none. */
static int ksm_stat_field(const char *key)
{
	char line[128];
	size_t n = strlen(key);
	int ret = -1;
	FILE *f = fopen("/proc/self/ksm_stat", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, n) && line[n] == ':')
			ret = !strncmp(line + n + 1, " yes", 4);
	fclose(f);
	return ret;
}

static int merge_any(void)
{
	return prctl(PR_GET_MEMORY_MERGE, 0UL, 0UL, 0UL, 0UL);
}

/*
 * The process is out of KSM: no process-wide setting, no mergeable VMA.
 * Where it is in, it backs out at once (see the top of this file).
 */
static int out_of_ksm(void)
{
	int any = merge_any(), mergeable = ksm_stat_field("ksm_mergeable");

	if (any > 0)
		prctl(PR_SET_MEMORY_MERGE, 0UL, 0UL, 0UL, 0UL);
	fprintf(stderr, "PR_GET_MEMORY_MERGE %d, ksm_mergeable %d\n",
		any, mergeable);
	return any == 0 && mergeable == 0;
}

/* The roles: each returns its exit status, 0 when it passes. */
static int role(const char *name, char *argv0)
{
	unsigned long size = getauxval(AT_PAGESZ);

	if (!strcmp(name, "refuses")) {
		int ret, err;

		if (size != LEAF)
			return KSFT_SKIP;
		ret = prctl(PR_SET_MEMORY_MERGE, 1UL, 0UL, 0UL, 0UL);
		err = errno;
		if (!ret)
			prctl(PR_SET_MEMORY_MERGE, 0UL, 0UL, 0UL, 0UL);
		fprintf(stderr, "PR_SET_MEMORY_MERGE in a 4K process: %d (%s)\n",
			ret, ret ? strerror(err) : "accepted");
		return !(ret == -1 && err == EOPNOTSUPP && out_of_ksm());
	}
	if (!strcmp(name, "inherited") ||
	    !strcmp(name, "inherited-then-native")) {
		if (size != LEAF)
			return KSFT_SKIP;
		if (!out_of_ksm())
			return 1;
		if (!strcmp(name, "inherited"))
			return 0;
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL))
			return 1;
		execl("/proc/self/exe", argv0, "native-after", NULL);
		return 1;
	}
	if (!strcmp(name, "native-after")) {
		if (size != NATIVE)
			return KSFT_SKIP;
		return !out_of_ksm();
	}
	if (!strcmp(name, "native-kept")) {
		int any = merge_any();

		if (size != NATIVE)
			return KSFT_SKIP;
		fprintf(stderr, "PR_GET_MEMORY_MERGE after a native exec: %d\n",
			any);
		return any != 1;
	}
	return 1;
}

/*
 * Runs @name in a child: with the process-wide KSM setting of a native
 * process first if @merge, then in an exec at @size (native for zero).
 */
static void run(const char *desc, char *argv0, int merge, unsigned long size,
		const char *name)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!pid) {
		if (merge && prctl(PR_SET_MEMORY_MERGE, 1UL, 0UL, 0UL, 0UL)) {
			fprintf(stderr, "PR_SET_MEMORY_MERGE: %s\n",
				strerror(errno));
			_exit(1);
		}
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, size, 0UL, 0UL, 0UL) &&
		    size)
			_exit(KSFT_SKIP);
		execl("/proc/self/exe", argv0, name, NULL);
		_exit(1);
	}
	if (waitpid(pid, &status, 0) != pid)
		ksft_exit_fail_msg("waitpid: %s\n", strerror(errno));
	if (WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP)
		ksft_test_result_skip("%s: no 4K processes\n", desc);
	else
		ksft_test_result(WIFEXITED(status) && !WEXITSTATUS(status),
				 "%s\n", desc);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		return role(argv[1], argv[0]);

	ksft_print_header();
	if (getauxval(AT_PAGESZ) != NATIVE)
		ksft_exit_skip("not a 16K process on a 16K kernel\n");
	if (merge_any() < 0)
		ksft_exit_skip("no KSM: %s\n", strerror(errno));
	ksft_set_plan(4);
	run("a 4K process cannot opt into KSM", argv[0], 0, LEAF, "refuses");
	run("the setting does not follow a native process into a 4K exec",
	    argv[0], 1, LEAF, "inherited");
	run("nor back into a native exec after that", argv[0], 1, LEAF,
	    "inherited-then-native");
	run("a native exec keeps the setting", argv[0], 1, 0, "native-kept");
	ksft_finished();
}
