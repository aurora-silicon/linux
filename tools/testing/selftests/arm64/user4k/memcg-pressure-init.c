/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>

#define SET_EXEC_PAGE_SIZE 0x41555001
#define CG "/sys/fs/cgroup/pressure"
#define MIB (1024UL * 1024)
#define LENGTH (32 * MIB)
#define ROUNDS 4

static int put(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	ssize_t ret = write(fd, value, strlen(value));
	int saved = errno;
	close(fd);
	errno = saved;
	return ret == (ssize_t)strlen(value) ? 0 : -1;
}

static unsigned long long get(const char *path, const char *key)
{
	FILE *f = fopen(path, "r");
	char name[128];
	unsigned long long value = 0, result = 0;
	if (!f) {
		perror(path);
		exit(10);
	}
	if (!key) {
		if (fscanf(f, "%llu", &result) != 1)
			exit(11);
	} else
		while (fscanf(f, "%127s %llu", name, &value) == 2)
			if (!strcmp(name, key)) {
				result = value;
				break;
			}
	fclose(f);
	return result;
}

static unsigned char pattern(size_t offset, unsigned seed, unsigned round)
{
	return (offset ^ (offset >> 12) ^ (offset >> 20) ^ (seed * 79) ^
		(round * 43)) &
	       255;
}

static int contents(unsigned char *p, size_t ps, unsigned seed, unsigned round,
		    int write_data, int child)
{
	size_t pages = LENGTH / ps;
	/* Odd multiplication permutes every logical page of this power-of-two range. */
	for (size_t i = 0; i < pages; i++) {
		size_t page = (i * 4051 + round * 31) & (pages - 1);
		for (size_t j = 0; j < ps; j++) {
			size_t offset = page * ps + j;
			unsigned char want = pattern(offset, seed, round);
			if (child && !(page & 3))
				want ^= 0x69;
			if (write_data)
				p[offset] = want;
			else if (p[offset] != want) {
				printf("not ok - ABI=%zu round=%u child=%d byte=%zu got=%u expected=%u\n",
				       ps, round, child, offset, p[offset],
				       want);
				return 1;
			}
		}
	}
	return 0;
}

static int barrier(int ready, int command)
{
	char token = 'R';
	return write(ready, &token, 1) != 1 || read(command, &token, 1) != 1 ||
	       token != 'G';
}

static int worker(unsigned long expected, unsigned seed, int ready, int command)
{
	size_t ps = getauxval(AT_PAGESZ);
	unsigned char *p;
	if (ps != expected)
		return 2;
	p = mmap(NULL, LENGTH, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return 3;
	if (madvise(p, LENGTH, MADV_NOHUGEPAGE)) {
		int error = errno;
		struct stat st;

		/* Without THP this advice is unavailable and cannot change policy. */
		if (error != EINVAL ||
		    !stat("/sys/kernel/mm/transparent_hugepage/enabled", &st) ||
		    errno != ENOENT)
			return 3;
	}
	if (contents(p, ps, seed, 0, 1, 0) || barrier(ready, command))
		return 4;
	for (unsigned round = 0; round < ROUNDS; round++) {
		if (contents(p, ps, seed, round, 0, 0))
			return 5;
		pid_t child = fork();
		if (child < 0)
			return 6;
		if (!child) {
			/* Dirty a quarter of the logical pages, retaining untouched siblings. */
			for (size_t page = 0; page < LENGTH / ps; page += 4)
				for (size_t j = 0; j < ps; j++)
					p[page * ps + j] ^= 0x69;
			_exit(contents(p, ps, seed, round, 0, 1) ? 7 : 0);
		}
		int status;
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) || contents(p, ps, seed, round, 0, 0))
			return 8;
		printf("ok - memcg ABI %zuK round=%u swap-in bytes and fork COW isolation\n",
		       ps / 1024, round);
		if (round + 1 < ROUNDS &&
		    contents(p, ps, seed, round + 1, 1, 0))
			return 9;
		if (barrier(ready, command))
			return 10;
	}
	return munmap(p, LENGTH) ? 11 : 0;
}

static void report(unsigned phase)
{
	printf("# memcg phase=%u resident=%llu swap=%llu anon=%llu pgscan=%llu pgsteal=%llu max=%llu oom=%llu oom_kill=%llu\n",
	       phase, get(CG "/memory.current", NULL),
	       get(CG "/memory.swap.current", NULL),
	       get(CG "/memory.stat", "anon"), get(CG "/memory.stat", "pgscan"),
	       get(CG "/memory.stat", "pgsteal"),
	       get(CG "/memory.events", "max"), get(CG "/memory.events", "oom"),
	       get(CG "/memory.events", "oom_kill"));
}

static void pressure_stacks(void)
{
	FILE *tasks = fopen(CG "/cgroup.procs", "r");
	int pid;

	if (!tasks)
		return;
	while (fscanf(tasks, "%d", &pid) == 1) {
		char path[64], stack[4096];
		int fd;
		ssize_t n;

		snprintf(path, sizeof(path), "/proc/%d/stack", pid);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		n = read(fd, stack, sizeof(stack) - 1);
		close(fd);
		if (n > 0) {
			stack[n] = 0;
			printf("# pressure stack pid=%d\n%s", pid, stack);
		}
	}
	fclose(tasks);
}

static void observe_pressure(void)
{
	unsigned int samples = 0;

	for (;;) {
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		printf("# io-pressure ms=%lld resident=%llu swap=%llu writeback=%llu "
		       "swapcached=%llu scan=%llu steal=%llu max=%llu oom=%llu kill=%llu\n",
		       (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000,
		       get(CG "/memory.current", NULL),
		       get(CG "/memory.swap.current", NULL),
		       get(CG "/memory.stat", "file_writeback"),
		       get(CG "/memory.stat", "swapcached"),
		       get(CG "/memory.stat", "pgscan"),
		       get(CG "/memory.stat", "pgsteal"),
		       get(CG "/memory.events", "max"),
		       get(CG "/memory.events", "oom"),
		       get(CG "/memory.events", "oom_kill"));
		if (!(++samples % 8) &&
		    get(CG "/memory.stat", "file_writeback") > 8 * MIB)
			pressure_stacks();
		usleep(250000);
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 6 && !strcmp(argv[1], "--worker") && getppid() == 1)
		return worker(strtoul(argv[2], NULL, 10),
			      strtoul(argv[3], NULL, 10), atoi(argv[4]),
			      atoi(argv[5]));
	bool classic = false, native_control = false, user16k = false;

	if (getpid() != 1 || argc > 3) {
		fprintf(stderr, "Dedicated disposable VM PID1 only\n");
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--classic"))
			classic = true;
		else if (!strcmp(argv[i], "--native-control"))
			native_control = true;
		else if (!strcmp(argv[i], "--user16k"))
			user16k = true;
		else
			return 2;
	}
	if (native_control && user16k)
		return 2;
	signal(SIGPIPE, SIG_IGN);
	alarm(300);
	int failed = 0, active = 0;
	pid_t children[2] = { -1, -1 }, observer = -1;
	int ready[2][2], command[2][2];
	unsigned long sizes[2] = { getauxval(AT_PAGESZ), 4096 };
	if (native_control)
		sizes[1] = sizes[0];
	if (user16k)
		sizes[1] = 16384;
	if ((sizes[0] != 16384 && sizes[0] != 65536) || sizes[1] > sizes[0]) {
		failed = 1;
		goto done;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL) ||
	    put("/sys/fs/cgroup/cgroup.subtree_control", "+memory") ||
	    mkdir(CG, 0755) || put(CG "/memory.max", "50331648") ||
	    put(CG "/memory.swap.max", "268435456")) {
		perror("cgroup setup");
		failed = 1;
		goto done;
	}
	/* Explicit diagnostic control, never change the production default. */
	if (classic && put("/sys/kernel/mm/lru_gen/enabled", "0")) {
		perror("classic reclaim control");
		failed = 1;
		goto done;
	}
	printf("# reclaim mode=%s\n",
	       classic ? "classic diagnostic" : "default");
	struct stat st;
	for (int i = 0; i < 100 && stat("/dev/vda", &st); i++)
		usleep(20000);
	if (swapon("/dev/vda", 0)) {
		perror("swapon");
		failed = 1;
		goto done;
	}
	active = 1;
	for (unsigned i = 0; i < 2; i++)
		if (pipe(ready[i]) || pipe(command[i])) {
			failed = 1;
			goto done;
		}
	observer = fork();
	if (!observer) {
		for (unsigned int i = 0; i < 2; i++) {
			close(ready[i][0]);
			close(ready[i][1]);
			close(command[i][0]);
			close(command[i][1]);
		}
		observe_pressure();
		_exit(0);
	}
	if (observer < 0) {
		failed = 1;
		goto done;
	}
	for (unsigned i = 0; i < 2; i++) {
		children[i] = fork();
		if (!children[i]) {
			char ps[32], index[16], rfd[16], cfd[16];
			for (unsigned j = 0; j < 2; j++) {
				close(ready[j][0]);
				close(command[j][1]);
				if (i != j) {
					close(ready[j][1]);
					close(command[j][0]);
				}
			}
			snprintf(ps, sizeof(ps), "%lu", sizes[i]);
			snprintf(index, sizeof(index), "%u", i);
			snprintf(rfd, sizeof(rfd), "%d", ready[i][1]);
			snprintf(cfd, sizeof(cfd), "%d", command[i][0]);
			if (put(CG "/cgroup.procs", "0") ||
			    prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(12);
			execl("/init", "/init", "--worker", ps, index, rfd, cfd,
			      NULL);
			_exit(13);
		}
		if (children[i] < 0) {
			failed = 1;
			goto done;
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		close(ready[i][1]);
		close(command[i][0]);
	}
	for (unsigned phase = 0; phase <= ROUNDS; phase++) {
		char token;
		for (unsigned i = 0; i < 2; i++)
			if (read(ready[i][0], &token, 1) != 1 || token != 'R') {
				failed = 1;
				goto done;
			}
		report(phase);
		if (!get(CG "/memory.swap.current", NULL) ||
		    !get(CG "/memory.events", "max") ||
		    !get(CG "/memory.stat", "pgscan") ||
		    !get(CG "/memory.stat", "pgsteal") ||
		    get(CG "/memory.events", "oom") ||
		    get(CG "/memory.events", "oom_kill")) {
			printf("not ok - missing pressure evidence or unexpected OOM\n");
			failed = 1;
			goto done;
		}
		for (unsigned i = 0; i < 2; i++)
			if (write(command[i][1], "G", 1) != 1) {
				failed = 1;
				goto done;
			}
	}
	for (unsigned i = 0; i < 2; i++) {
		int status = 0;
		int ok = waitpid(children[i], &status, 0) == children[i] &&
			 WIFEXITED(status) && !WEXITSTATUS(status);
		printf("%s - memcg ABI %luK exit=%#x\n", ok ? "ok" : "not ok",
		       sizes[i] / 1024, status);
		children[i] = -1;
		failed |= !ok;
	}
	/* Pending swap writeback can outlive exit, especially on a slow device. */
	for (unsigned i = 0; i < 3000 && (get(CG "/memory.stat", "anon") ||
					  get(CG "/memory.swap.current", NULL));
	     i++) {
		if (!(i % 250))
			printf("# exit drain ms=%u swap=%llu writeback=%llu\n",
			       i * 20, get(CG "/memory.swap.current", NULL),
			       get(CG "/memory.stat", "file_writeback"));
		/* Exit may leave clean, unmapped swap cache after writeback. Reclaim
		 * it explicitly; EAGAIN means fewer than the requested bytes existed. */
		if (!get(CG "/memory.stat", "file_writeback") &&
		    put(CG "/memory.reclaim", "67108864 swappiness=200") &&
		    errno != EAGAIN) {
			perror("exit cache reclaim");
			failed = 1;
			break;
		}
		usleep(20000);
	}
	report(ROUNDS + 1);
	if (get(CG "/memory.stat", "anon") ||
	    get(CG "/memory.swap.current", NULL)) {
		printf("not ok - anonymous or swap charge remains after exit\n");
		failed = 1;
	}
done:
	if (observer > 0) {
		kill(observer, SIGTERM);
		waitpid(observer, NULL, 0);
	}
	if (active && failed)
		report(ROUNDS + 2);
	for (unsigned i = 0; i < 2; i++)
		if (children[i] > 0) {
			kill(children[i], SIGKILL);
			waitpid(children[i], NULL, 0);
		}
	if (active && failed) {
		put(CG "/cgroup.kill", "1");
		put(CG "/memory.max", "max");
	}
	if (active && swapoff("/dev/vda")) {
		perror("swapoff");
		failed = 1;
	}
	printf("GRANULE MEMCG PRESSURE %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
