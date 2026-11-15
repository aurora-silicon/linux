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
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long swapouts(void)
{
	FILE *f = fopen("/proc/vmstat", "re");
	char key[128];
	unsigned long value, found = 0;

	if (!f)
		return 0;
	while (fscanf(f, "%127s %lu", key, &value) == 2)
		if (!strcmp(key, "pswpout"))
			found = value;
	fclose(f);
	return found;
}

static int native_swap_smoke(void)
{
	const size_t length = 4 * 1024 * 1024, page = 16384;
	unsigned long before = swapouts(), after;
	unsigned char *p = mmap(NULL, length, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	size_t i;
	unsigned int tries;
	int ret = 0;

	if (p == MAP_FAILED)
		return -1;
	for (i = 0; i < length; i++)
		p[i] = (i ^ (i >> 12) ^ (i >> 20)) & 255;
	if (madvise(p, length, MADV_PAGEOUT)) {
		ret = -1;
		goto out;
	}
	for (tries = 0; tries < 100 && swapouts() == before; tries++)
		usleep(20000);
	after = swapouts();
	for (i = 0; i < length; i++)
		if (p[i] != ((i ^ (i >> 12) ^ (i >> 20)) & 255)) {
			ret = -1;
			break;
		}
	if (after <= before)
		ret = -1;
	printf("%s - native %zuK swap transport: pswpout %lu -> %lu, verified %zu bytes\n",
	       ret ? "not ok" : "ok", page / 1024, before, after, i);
out:
	munmap(p, length);
	return ret;
}

static int run_swap_suite(void)
{
	const char *base = "/sys/kernel/debug/kunit/arm64-user4k-swap/";
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
	    strstr(log, "\nok 1 arm64-user4k-swap\n") &&
	    !strstr(log, "not ok") && !strstr(log, "# SKIP"))
		ret = 0;
	free(log);
	return ret;
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
	ret = !strcmp(operation, "--swapoff-helper") ? swapoff("/dev/vda") :
						       swapon("/dev/vda", 0);
	if (ret)
		perror(operation);
	return ret ? 1 : 0;
}

static int pageout_exec(bool small)
{
	pid_t pid = fork();
	int status = 0;

	if (!pid) {
		if (small &&
		    prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, 4096UL, 0UL, 0UL, 0UL))
			_exit(125);
		execl("/reclaim-advice", "/reclaim-advice",
		      small ? "--expect-4k" : NULL, NULL);
		_exit(126);
	}
	if (pid <= 0 || waitpid(pid, &status, 0) != pid)
		return -1;
	printf("# EL0 pageout %s child status=%#x\n", small ? "4K" : "native",
	       status);
	return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

int main(int argc, char **argv)
{
	bool smoke = argc == 2 && !strcmp(argv[1], "--smoke");
	bool pageout = argc == 2 && !strcmp(argv[1], "--pageout");
	bool native_only = argc == 2 && !strcmp(argv[1], "--pageout-native");
	bool active = false;
	unsigned int tries;
	struct stat st;
	pid_t child;
	int status, ret = 1;

	if (argc == 2 && (!strcmp(argv[1], "--swapoff-helper") ||
			  !strcmp(argv[1], "--swapon-helper")))
		return swap_control_helper(argv[1]);
	/* This runner must never mount filesystems or enable swap on the host. */
	if (getpid() != 1 ||
	    (argc != 1 && !smoke && !pageout && !native_only)) {
		fprintf(stderr,
			"Use only as /init in the dedicated disposable test VM.\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	if (getauxval(AT_PAGESZ) != 16384 || sysconf(_SC_PAGESIZE) != 16384)
		goto out;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL))
		goto out;
	child = fork();
	if (!child) {
		execl("/page-contract", "page-contract", NULL);
		_exit(127);
	}
	if (child < 0 || waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status))
		goto out;
	for (tries = 0; tries < 100 && stat("/dev/vda", &st); tries++)
		usleep(20000);
	if (stat("/dev/vda", &st) || !S_ISBLK(st.st_mode) ||
	    swapon("/dev/vda", 0))
		goto out;
	active = true;
	if (native_swap_smoke())
		goto out;
	if (!smoke && !native_only && run_swap_suite())
		goto out;
	if ((pageout || native_only) && pageout_exec(false))
		goto out;
	if (pageout && pageout_exec(true))
		goto out;
	ret = 0;
out:
	if (ret)
		printf("not ok - swap fixture runner: errno=%d (%s)\n", errno,
		       strerror(errno));
	if (active && swapoff("/dev/vda")) {
		perror("swapoff");
		ret = 1;
	}
	printf("%s - swap fixture runner (%s)\n", ret ? "not ok" : "ok",
	       smoke ? "native transport smoke only" : "4K swap suite");
	fflush(NULL);
	reboot(RB_POWER_OFF);
	return ret;
}
