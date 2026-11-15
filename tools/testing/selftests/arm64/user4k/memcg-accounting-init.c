/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define CG "/sys/fs/cgroup/account"
#define LENGTH (32UL * 1024 * 1024)
static int put(const char *p, const char *v)
{
	int fd = open(p, O_WRONLY);
	if (fd < 0)
		return 1;
	int bad = write(fd, v, strlen(v)) != (ssize_t)strlen(v);
	close(fd);
	return bad;
}
static unsigned long long stat_value(const char *key)
{
	FILE *f = fopen(CG "/memory.stat", "r");
	char name[128];
	unsigned long long value = 0;
	if (!f)
		exit(20);
	while (fscanf(f, "%127s %llu", name, &value) == 2)
		if (!strcmp(key, name)) {
			fclose(f);
			return value;
		}
	fclose(f);
	exit(21);
}
static int worker(unsigned long expected, int ready, int release)
{
	if (getauxval(AT_PAGESZ) != expected)
		return 2;
	unsigned char *p = mmap(NULL, LENGTH, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED || madvise(p, LENGTH, MADV_NOHUGEPAGE))
		return 3;
	memset(p, 0x71, LENGTH);
	char token = 'R';
	if (write(ready, &token, 1) != 1 || read(release, &token, 1) != 1)
		return 4;
	/* Keep the writes observable independently of the accounting check. */
	for (size_t i = 0; i < LENGTH; i++)
		if (p[i] != 0x71)
			return 5;
	return munmap(p, LENGTH) ? 6 : 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (getppid() == 1 && argc == 5 && !strcmp(argv[1], "--worker"))
		return worker(strtoul(argv[2], NULL, 10), atoi(argv[3]),
			      atoi(argv[4]));
	if (getpid() != 1 || argc != 1)
		return 2;
	signal(SIGPIPE, SIG_IGN);
	alarm(60);
	int failed = 0;
	unsigned long long slab[3] = {};
	unsigned long sizes[3] = { getauxval(AT_PAGESZ), 4096, 16384 };
	unsigned int modes = sizes[0] == 65536 ? 3 : 2;
	if ((sizes[0] != 16384 && sizes[0] != 65536) ||
	    mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL) ||
	    put("/sys/fs/cgroup/cgroup.subtree_control", "+memory")) {
		perror("setup");
		failed = 1;
		goto done;
	}
	for (unsigned i = 0; i < modes; i++) {
		int ready[2], release[2];
		if (mkdir(CG, 0755) || put(CG "/memory.max", "134217728") ||
		    pipe(ready) || pipe(release)) {
			failed = 1;
			break;
		}
		pid_t child = fork();
		if (!child) {
			char ps[32], rfd[16], cfd[16];
			close(ready[0]);
			close(release[1]);
			snprintf(ps, sizeof(ps), "%lu", sizes[i]);
			snprintf(rfd, sizeof(rfd), "%d", ready[1]);
			snprintf(cfd, sizeof(cfd), "%d", release[0]);
			if (put(CG "/cgroup.procs", "0") ||
			    prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(7);
			execl("/init", "/init", "--worker", ps, rfd, cfd, NULL);
			_exit(8);
		}
		close(ready[1]);
		close(release[0]);
		char token;
		int ok = child > 0 && read(ready[0], &token, 1) == 1;
		if (ok) {
			/* Let per-CPU slab statistics become visible before the comparison. */
			usleep(1200000);
			slab[i] = stat_value("slab_unreclaimable");
			printf("# ABI=%lu anon=%llu slab_unreclaimable=%llu pagetables=%llu\n",
			       sizes[i], stat_value("anon"), slab[i],
			       stat_value("pagetables"));
			ok = stat_value("anon") >= LENGTH &&
			     write(release[1], "G", 1) == 1;
		}
		int status = 0;
		if (child > 0 && !ok)
			kill(child, SIGKILL);
		ok = child > 0 && waitpid(child, &status, 0) == child &&
		     WIFEXITED(status) && !WEXITSTATUS(status) && ok;
		close(ready[0]);
		close(release[1]);
		printf("%s - memcg accounting ABI %luK worker status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		failed |= !ok;
		for (unsigned retry = 0; retry < 100 && stat_value("anon");
		     retry++)
			usleep(20000);
		if (stat_value("anon") || rmdir(CG))
			failed = 1;
		if (failed)
			break;
	}
	/* Slot metadata must be charged for every alternative granule. The
	 * loose lower bound tolerates slab/KASAN layout differences. */
	for (unsigned i = 1; !failed && i < modes; i++) {
		if (slab[i] < slab[0] + 512 * 1024) {
			printf("not ok - slot metadata escaped cgroup accounting ABI=%lu native=%llu alternative=%llu\n",
			       sizes[i], slab[0], slab[i]);
			failed = 1;
		}
	}

done:
	printf("GRANULE MEMCG ACCOUNTING %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
