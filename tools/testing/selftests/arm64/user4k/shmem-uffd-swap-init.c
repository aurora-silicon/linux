// SPDX-License-Identifier: GPL-2.0-only
/*
 * PID-1 runner for a disposable VM with a dedicated virtio swap disk.
 * Use kunit.autorun=0: tests skipped at boot stay skipped on a debugfs rerun.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_swap_suite(void)
{
	const char *base = "/sys/kernel/debug/kunit/arm64-user4k-shmem-swap/";
	char path[128], *log;
	size_t used = 0, capacity = 1024 * 1024;
	ssize_t n;
	int fd, ret = -1;

	snprintf(path, sizeof(path), "%srun", base);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = write(fd, "1", 1);
	close(fd);
	if (n != 1)
		return -1;
	snprintf(path, sizeof(path), "%sresults", base);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	log = calloc(1, capacity);
	if (!log) {
		close(fd);
		return -1;
	}
	while (used < capacity - 1 &&
	       (n = read(fd, log + used, capacity - 1 - used)) > 0)
		used += n;
	close(fd);
	fputs(log, stdout);
	if (n == 0 && used < capacity - 1 &&
	    strstr(log, "\nok 1 arm64-user4k-shmem-swap\n") &&
	    !strstr(log, "not ok") && !strstr(log, "# SKIP"))
		ret = 0;
	free(log);
	return ret;
}

static int fault_setting(const char *name, const char *value)
{
	char path[160];
	snprintf(path, sizeof(path), "/sys/kernel/debug/failslab/%s", name);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = write(fd, value, strlen(value));
	close(fd);
	return n == (ssize_t)strlen(value) ? 0 : -1;
}
static int configure_failslab(bool enable)
{
	if (!enable)
		return fault_setting("probability", "0");
	/* The KUnit worker selects itself only around metadata writeout. */
	return fault_setting("task-filter", "Y") ||
	       fault_setting("verbose", "0") || fault_setting("times", "-1") ||
	       fault_setting("interval", "1") || fault_setting("space", "0") ||
	       fault_setting("probability", "100");
}

static int swap_control_helper(const char *operation)
{
	struct stat init, self;
	int ret;

	/* A helper is allowed only when this same executable is the VM's PID 1. */
	if (getpid() == 1 || stat("/proc/1/exe", &init) ||
	    stat("/proc/self/exe", &self) || init.st_dev != self.st_dev ||
	    init.st_ino != self.st_ino) {
		fprintf(stderr,
			"swap helper requires its fixture runner as PID 1\n");
		return 1;
	}
	if (!strcmp(operation, "--memcg-setup-helper")) {
		if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL) &&
		    errno != EBUSY)
			return 1;
		int fd = open("/sys/fs/cgroup/cgroup.subtree_control",
			      O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			return 1;
		ssize_t n = write(fd, "+memory", 7);
		close(fd);
		if (n != 7)
			return 1;
		return mkdir("/sys/fs/cgroup/uffd-metadata", 0755) &&
		       errno != EEXIST;
	}
	if (!strcmp(operation, "--failslab-on-helper"))
		return configure_failslab(true);
	if (!strcmp(operation, "--failslab-off-helper"))
		return configure_failslab(false);
	ret = !strcmp(operation, "--swapoff-helper") ? swapoff("/dev/vda") :
						       swapon("/dev/vda", 0);
	if (ret)
		perror(operation);
	return ret ? 1 : 0;
}

int main(int argc, char **argv)
{
	struct stat st;
	bool active = false;
	int ret = 1;
	if (argc == 2 && (!strcmp(argv[1], "--swapon-helper") ||
			  !strcmp(argv[1], "--swapoff-helper") ||
			  !strcmp(argv[1], "--failslab-on-helper") ||
			  !strcmp(argv[1], "--failslab-off-helper") ||
			  !strcmp(argv[1], "--memcg-setup-helper")))
		return swap_control_helper(argv[1]);
	if (getpid() != 1 || argc != 1)
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	unsigned long ps = getauxval(AT_PAGESZ);
	if (ps != 16384 && ps != 65536)
		goto out;
	if (mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL))
		goto out;
	for (unsigned i = 0; i < 100 && stat("/dev/vda", &st); i++)
		usleep(20000);
	if (stat("/dev/vda", &st) || !S_ISBLK(st.st_mode) ||
	    swapon("/dev/vda", 0))
		goto out;
	active = true;
	ret = run_swap_suite() ? 1 : 0;
out:
	if (active && swapoff("/dev/vda"))
		ret = 1;
	printf("SHMEM UFFD STATE SWAP %s errno=%d\n", ret ? "FAIL" : "PASS",
	       errno);
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
