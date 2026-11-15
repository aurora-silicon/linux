// SPDX-License-Identifier: GPL-2.0-only
/* Disposable PID1 runner; fault injection is selected by the KUnit worker. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <unistd.h>
#include "swapfile-fixture.h"

static int setting(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, strlen(value));
	close(fd);
	return n == (ssize_t)strlen(value) ? 0 : -1;
}

static int fault_setting(const char *kind, const char *name, const char *value)
{
	char path[192];

	snprintf(path, sizeof(path), "/sys/kernel/debug/%s/%s", kind, name);
	return setting(path, value);
}

static int injection_off(void)
{
	int ret = fault_setting("failslab", "probability", "0");

	ret |= fault_setting("fail_page_alloc", "probability", "0");
	ret |= fault_setting("failslab", "ignore-gfp-wait", "Y");
	ret |= fault_setting("fail_page_alloc", "ignore-gfp-wait", "Y");
	ret |= fault_setting("fail_page_alloc", "ignore-gfp-highmem", "Y");
	return ret;
}

static int injection_on(bool page)
{
	const char *kind = page ? "fail_page_alloc" : "failslab";

	if (injection_off() || fault_setting(kind, "task-filter", "Y") ||
	    fault_setting(kind, "verbose", "0") ||
	    fault_setting(kind, "ignore-gfp-wait", "N") ||
	    fault_setting(kind, "times", "-1") ||
	    fault_setting(kind, "interval", "1") ||
	    fault_setting(kind, "space", "0"))
		return -1;
	if (!page)
		return 0; /* Only current->fail_nth selects the slab allocation. */
	return fault_setting(kind, "ignore-gfp-highmem", "N") ||
	       fault_setting(kind, "min-order", "2") ||
	       fault_setting(kind, "probability", "100");
}

static int move_worker(const char *text, bool enter, bool swap_limit)
{
	char *end, path[192], pid[32], comm[64];
	long nr = strtol(text, &end, 10);
	const char *group = swap_limit ? "coarse-swap-limit" : "coarse-zero";
	int fd;
	ssize_t n;

	if (*end || nr <= 1 || nr == getpid())
		return -1;
	snprintf(path, sizeof(path), "/proc/%ld/comm", nr);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, comm, sizeof(comm) - 1);
	close(fd);
	if (n < 1)
		return -1;
	comm[n] = 0;
	if (strncmp(comm, "kunit_", 6))
		return -1;
	if (enter) {
		if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL) &&
		    errno != EBUSY)
			return -1;
		if (setting("/sys/fs/cgroup/cgroup.subtree_control", "+memory"))
			return -1;
		snprintf(path, sizeof(path), "/sys/fs/cgroup/%s", group);
		if (mkdir(path, 0755) && errno != EEXIST)
			return -1;
		snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.max",
			 group);
		if (setting(path, swap_limit ? "max" : "0"))
			return -1;
		if (swap_limit &&
		    setting("/sys/fs/cgroup/coarse-swap-limit/memory.swap.max",
			    "0"))
			return -1;
	}
	snprintf(pid, sizeof(pid), "%ld", nr);
	if (enter)
		snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.procs",
			 group);
	else
		snprintf(path, sizeof(path), "/sys/fs/cgroup/cgroup.procs");
	return setting(path, pid);
}

static int helper(const char *op)
{
	struct stat init, self;

	if (getpid() == 1 || stat("/proc/1/exe", &init) ||
	    stat("/proc/self/exe", &self) || init.st_dev != self.st_dev ||
	    init.st_ino != self.st_ino)
		return 1;
	if (!strcmp(op, "--coarse-fixture-check"))
		return getauxval(AT_PAGESZ) != 16384;
	if (!strcmp(op, "--swapoff-helper"))
		return swapoff(swap_path()) != 0;
	if (!strcmp(op, "--swapon-helper"))
		return swapon(swap_path(), 0) != 0;
	if (!strncmp(op, "--coarse-swap-enter=", 20))
		return move_worker(op + 20, true, true) != 0;
	if (!strncmp(op, "--coarse-swap-leave=", 20))
		return move_worker(op + 20, false, true) != 0;
	if (!strcmp(op, "--coarse-swap-unlimit"))
		return setting("/sys/fs/cgroup/coarse-swap-limit/memory.swap.max",
			       "max") != 0;
	if (!strcmp(op, "--coarse-injection-off"))
		return injection_off() != 0;
	if (!strcmp(op, "--coarse-slab-on"))
		return injection_on(false) != 0;
	if (!strcmp(op, "--coarse-page-on"))
		return injection_on(true) != 0;
	if (!strncmp(op, "--coarse-memcg-enter=", 21))
		return move_worker(op + 21, true, false) != 0;
	if (!strncmp(op, "--coarse-memcg-leave=", 21))
		return move_worker(op + 21, false, false) != 0;
	return 1;
}

static int run_suite(void)
{
	const char *base = "/sys/kernel/debug/kunit/arm64-user4k-coarse-shmem/";
	char path[192], *log = calloc(1, 1024 * 1024);
	size_t used = 0, capacity = 1024 * 1024;
	ssize_t n = 0;
	int fd, ret = -1;

	if (!log)
		return -1;
	snprintf(path, sizeof(path), "%srun", base);
	if (setting(path, "1"))
		goto out;
	snprintf(path, sizeof(path), "%sresults", base);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		goto out;
	while (used < capacity - 1 &&
	       (n = read(fd, log + used, capacity - used - 1)) > 0)
		used += n;
	close(fd);
	fputs(log, stdout);
	if (!n && used < capacity - 1 &&
	    strstr(log, "\nok 1 arm64-user4k-coarse-shmem\n") &&
	    !strstr(log, "not ok") && !strstr(log, "# SKIP"))
		ret = 0;
out:
	free(log);
	return ret;
}

int main(int argc, char **argv)
{
	struct stat st;
	bool active = false;
	int ret = 1;
	unsigned int i;

	if (getpid() != 1 && argc == 2)
		return helper(argv[1]);
	if (getpid() != 1 ||
	    (argc != 1 && (argc != 2 || strcmp(argv[1], "--swapfile"))))
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (getauxval(AT_PAGESZ) != 16384)
		goto out;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL))
		goto out;
	for (i = 0; i < 100 && stat("/dev/vda", &st); i++)
		usleep(20000);
	if (stat("/dev/vda", &st) || !S_ISBLK(st.st_mode))
		goto out;
	if (argc == 2 && prepare_swapfile())
		goto out;
	printf("COARSE SWAP BACKING=%s\n", swap_path());
	if (swapon(swap_path(), 0))
		goto out;
	active = true;
	ret = run_suite() ? 1 : 0;
	if (injection_off())
		ret = 1;
out:
	if (active && swapoff(swap_path()))
		ret = 1;
	if (active && argc == 2 && umount("/swapdir"))
		ret = 1;
	printf("COARSE SHMEM SWAP PASS=%d errno=%d\n", !ret, errno);
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
