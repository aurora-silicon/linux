// SPDX-License-Identifier: GPL-2.0-only
/*
 * Disposable two-node guest: real automatic NUMA hinting and migration.
 * Volatile accesses intentionally trigger page faults and repeated accesses;
 * thread coordination uses an atomic flag, not volatile storage.
 */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/mempolicy.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void check(int good, const char *expr, int line)
{
	if (!good) {
		fprintf(stderr, "FAIL line %d: %s errno=%d\n", line, expr,
			errno);
		exit(1);
	}
}
#define CHECK(x) check(!!(x), #x, __LINE__)

static void setting(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);

	CHECK(fd >= 0);
	CHECK(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
	CHECK(!close(fd));
}

static void cpu(int id)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(id, &set);
	CHECK(!sched_setaffinity(0, sizeof(set), &set));
}

static unsigned long counter(const char *key)
{
	FILE *f = fopen("/proc/vmstat", "r");
	char name[128];
	unsigned long value;

	CHECK(f);
	while (fscanf(f, "%127s %lu", name, &value) == 2) {
		if (!strcmp(name, key)) {
			CHECK(!fclose(f));
			return value;
		}
	}
	CHECK(0);
	return 0;
}

static double now(void)
{
	struct timespec ts;

	CHECK(!clock_gettime(CLOCK_MONOTONIC, &ts));
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int node_at(void *address)
{
	int node = -1;

	CHECK(!syscall(SYS_get_mempolicy, &node, NULL, 0UL, address,
		       MPOL_F_ADDR | MPOL_F_NODE));
	return node;
}

static unsigned long sample(unsigned char *map, unsigned long length,
			    unsigned long page)
{
	unsigned long moved = 0;

	for (unsigned long off = 0; off < length; off += page)
		if (off != 2 * page)
			moved += node_at(map + off + 16) == 1;
	return moved;
}

static void forbidden(unsigned char *address, int write_access)
{
	pid_t child = fork();
	int status;

	CHECK(child >= 0);
	if (!child) {
		if (write_access)
			*(volatile unsigned char *)address = 0x73;
		else
			(void)*(volatile unsigned char *)address;
		_exit(2);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

struct local_reader {
	unsigned char *map;
	unsigned long length, page;
	atomic_int stop;
};

static void *read_local(void *arg)
{
	struct local_reader *r = arg;

	cpu(1);
	while (!atomic_load_explicit(&r->stop, memory_order_relaxed)) {
		for (unsigned long off = 0; off < r->length; off += r->page) {
			if (off == 2 * r->page)
				continue;
			CHECK(*(volatile uint64_t *)(r->map + off) ==
			      0x81720000 + off / r->page);
		}
	}
	return NULL;
}

static void local_restore(unsigned char *map, unsigned long length,
			  unsigned long page)
{
	struct local_reader r = { .map = map, .length = length, .page = page };
	unsigned long hints = counter("numa_hint_faults_local");
	pthread_t worker;
	double deadline = now() + 2;

	/* Two threads permit hinting of already-local private mappings. */
	CHECK(!pthread_create(&worker, NULL, read_local, &r));
	do {
		for (unsigned long off = 0; off < length; off += page) {
			if (off == 2 * page)
				continue;
			CHECK(*(volatile uint64_t *)(map + off) ==
			      0x81720000 + off / page);
		}
	} while (now() < deadline);
	atomic_store_explicit(&r.stop, 1, memory_order_relaxed);
	CHECK(!pthread_join(worker, NULL));
	hints = counter("numa_hint_faults_local") - hints;
	printf("NUMAB local page=%lu hints=%lu\n", page, hints);
	CHECK(hints > 0);
}

static void exercise(void)
{
	unsigned long page = getauxval(AT_PAGESZ), length = 8UL << 20;
	unsigned long mask = 1, allowed = 0, hints, migrated, iterations = 0;
	unsigned long moved = 0, total = length / page - 1;
	unsigned char *map;
	int mode;

	CHECK(!syscall(SYS_get_mempolicy, &mode, &allowed, 64UL, NULL,
		       MPOL_F_MEMS_ALLOWED));
	CHECK(allowed == 3);
	cpu(0);
	map = mmap(NULL, length, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(map != MAP_FAILED);
	CHECK(!madvise(map, length, MADV_NOHUGEPAGE));
	CHECK(!syscall(SYS_mbind, map, length, MPOL_BIND, &mask, 64UL, 0));
	for (unsigned long off = 0; off < length; off += page) {
		*(uint64_t *)(map + off) = 0x81720000 + off / page;
		*(uint64_t *)(map + off + 8) = 0;
		*(uint64_t *)(map + off + page - 8) = 0x61320000 + off / page;
		CHECK(node_at(map + off + 16) == 0);
	}
	CHECK(!mprotect(map + page, page, PROT_READ));
	CHECK(!mprotect(map + 2 * page, page, PROT_NONE));
	CHECK(!syscall(SYS_mbind, map, length, MPOL_DEFAULT, NULL, 0UL, 0));
	cpu(1);
	hints = counter("numa_hint_faults");
	migrated = counter("numa_pages_migrated");
	double started = now(), deadline = started + 20;

	do {
		for (unsigned long off = 0; off < length; off += page) {
			volatile uint64_t *p = (void *)(map + off);

			if (off == 2 * page)
				continue;
			CHECK(p[0] == 0x81720000 + off / page);
			CHECK(p[page / 8 - 1] == 0x61320000 + off / page);
			if (off != page)
				p[1]++;
		}
		iterations++;
		if (!(iterations % 128)) {
			moved = sample(map, length, page);
			if (moved == total)
				break;
		}
	} while (now() < deadline);
	moved = sample(map, length, page);
	hints = counter("numa_hint_faults") - hints;
	migrated = counter("numa_pages_migrated") - migrated;
	printf("NUMAB page=%lu moved=%lu/%lu hints=%lu migrated=%lu loops=%lu seconds=%.3f\n",
	       page, moved, total, hints, migrated, iterations,
	       now() - started);
	CHECK(moved == total && hints > 0 && migrated > 0);
	for (unsigned long off = 0; off < length; off += page) {
		if (off != page && off != 2 * page)
			CHECK(*(uint64_t *)(map + off + 8) == iterations);
	}
	local_restore(map, length, page);
	forbidden(map + page, 1);
	forbidden(map + 2 * page, 0);
	CHECK(!mprotect(map + 2 * page, page, PROT_READ));
	CHECK(*(uint64_t *)(map + 2 * page) == 0x81720002);
	CHECK(!munmap(map, length));
	printf("ok - NUMAB page=%lu automatic placement, bytes and permissions\n",
	       page);
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ),
		      sizes[] = { 4096, 16384, 65536 };
	int failed = 0, compat_only = 0, no4k = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 3 && !strcmp(argv[1], "--case")) {
		CHECK(getauxval(AT_PAGESZ) == strtoul(argv[2], NULL, 10));
		exercise();
		return 0;
	}
	CHECK(getpid() == 1);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--compat-only"))
			compat_only = 1;
		else if (!strcmp(argv[i], "--no4k"))
			no4k = 1;
		else
			CHECK(0);
	}
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	CHECK(!mount("debugfs", "/debug", "debugfs", 0, NULL));
	setting("/proc/sys/kernel/numa_balancing", "1\n");
	setting("/debug/sched/numa_balancing/scan_delay_ms", "100\n");
	setting("/debug/sched/numa_balancing/scan_period_min_ms", "100\n");
	setting("/debug/sched/numa_balancing/scan_period_max_ms", "100\n");
	setting("/debug/sched/numa_balancing/scan_size_mb", "16\n");
	for (unsigned int i = 0; i < 3; i++) {
		pid_t child;
		int status;

		if (compat_only || sizes[i] > native ||
		    (no4k && sizes[i] == 4096))
			continue;
		child = fork();
		CHECK(child >= 0);
		if (!child) {
			char s[32];

			snprintf(s, sizeof(s), "%lu", sizes[i]);
			CHECK(!prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, sizes[i],
				     0UL, 0UL, 0UL));
			execl("/init", "/init", "--case", s, NULL);
			_exit(2);
		}
		CHECK(waitpid(child, &status, 0) == child);
		failed |= !WIFEXITED(status) || WEXITSTATUS(status);
	}
	CHECK(!access("/compat-numa-balancing-probe", X_OK));
	pid_t child = fork();
	int status;

	CHECK(child >= 0);
	if (!child) {
		char s[32];

		snprintf(s, sizeof(s), "%lu", no4k ? native : 4096UL);
		execl("/compat-numa-balancing-probe",
		      "/compat-numa-balancing-probe", s, NULL);
		_exit(2);
	}
	CHECK(waitpid(child, &status, 0) == child);
	failed |= !WIFEXITED(status) || WEXITSTATUS(status);
	printf("A32 NUMAB expected-page=%lu status=%d\n",
	       no4k ? native : 4096UL, status);
	printf("NUMA BALANCING %s\n", failed ? "FAIL" : "PASS");
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
