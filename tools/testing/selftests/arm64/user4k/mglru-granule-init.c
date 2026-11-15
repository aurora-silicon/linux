/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sched.h>
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
#define CONTROL "/sys/kernel/debug/lru_gen"
#define FULL "/sys/kernel/debug/lru_gen_full"
static int ready_fd = -1, command_fd = -1;
static unsigned worker_index;
static unsigned char pattern(unsigned region, size_t byte, unsigned round)
{
	return (byte ^ (byte >> 12) ^ (region * 37) ^ (round * 53)) & 255;
}
static int generation(unsigned long long *cg, unsigned *node,
		      unsigned long *max)
{
	FILE *f = fopen(CONTROL, "r");
	char line[256];
	int have_cg = 0, have_node = 0, have_seq = 0;
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long id;
		unsigned n;
		unsigned long seq, age, anon, file;
		if (sscanf(line, "memcg %llu", &id) == 1) {
			if (have_cg)
				break;
			*cg = id;
			have_cg = 1;
		} else if (sscanf(line, " node %u", &n) == 1) {
			if (have_node)
				break;
			*node = n;
			have_node = 1;
		} else if (have_node && sscanf(line, "%lu %lu %lu %lu", &seq,
					       &age, &anon, &file) == 4) {
			*max = seq;
			have_seq = 1;
		}
	}
	fclose(f);
	return have_seq ? 0 : -1;
}
static unsigned long scan_evidence(void)
{
	FILE *f = fopen(FULL, "r");
	char line[512];
	unsigned long total = 0;
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long t, y, found, added;
		if (sscanf(line, " %lut %luy %luf %lua", &t, &y, &found,
			   &added) == 4) {
			printf("# MGLRU scan total=%lu young=%lu tables=%lu added=%lu\n",
			       t, y, found, added);
			total += y;
		}
	}
	fclose(f);
	return total;
}
static int age(void)
{
	if (ready_fd >= 0) {
		char token = 'A';
		return write(ready_fd, &token, 1) != 1 ||
		       read(command_fd, &token, 1) != 1 || token != 'G';
	}
	unsigned long long cg = 0, next_cg = 0;
	unsigned node = 0, next_node = 0;
	unsigned long seq = 0, next = 0;
	char command[128];
	if (generation(&cg, &node, &seq))
		return 1;
	/* This tree accepts swappiness here; 200 scans both anonymous and file pages. */
	int len = snprintf(command, sizeof(command), "+ %llu %u %lu 200 1\n",
			   cg, node, seq);
	int fd = open(CONTROL, O_WRONLY);
	if (fd < 0)
		return 1;
	ssize_t written = write(fd, command, len);
	close(fd);
	if (written != len || generation(&next_cg, &next_node, &next) ||
	    next <= seq) {
		printf("not ok - MGLRU generation command=%s result=%zd errno=%d next=%lu\n",
		       command, written, errno, next);
		return 1;
	}
	printf("ok - MGLRU generation %lu -> %lu\n", seq, next);
	return 0;
}
static int check_referenced(unsigned char **regions, unsigned nr,
			    size_t expected)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	char line[512];
	int region = -1, failed = 0;
	unsigned seen = 0;
	unsigned long start, end, kb;
	if (!f)
		return 1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			region = -1;
			for (unsigned i = 0; i < nr; i++)
				if (start == (uintptr_t)regions[i])
					region = i;
		} else if (region >= 0 &&
			   sscanf(line, "Referenced: %lu kB", &kb) == 1) {
			printf("%s - region=%d start=%p referenced=%lu expected=%zu kB\n",
			       kb == expected ? "ok" : "not ok", region,
			       regions[region], kb, expected);
			failed |= kb != expected;
			seen++;
		}
	}
	fclose(f);
	return failed || seen != nr;
}
static int exercise(unsigned long expected)
{
	static const unsigned shifts[] = { 21, 25, 29, 30, 36, 39, 42, 47 };
	unsigned char *regions[sizeof(shifts) / sizeof(shifts[0])] = {};
	size_t ps = getauxval(AT_PAGESZ), len = 8 * ps;
	unsigned nr = sizeof(shifts) / sizeof(shifts[0]);
	int fail = 0, fd = memfd_create("granule-aging", 0);
	cpu_set_t cpus;
	void *scratch = MAP_FAILED;
	CPU_ZERO(&cpus);
	CPU_SET(ready_fd >= 0 ? worker_index % 2 : 0, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus))
		return 1;
	if (ps != expected || fd < 0 || ftruncate(fd, nr * len))
		return 1;
	for (unsigned i = 0; i < nr; i++) {
		uintptr_t address = (1UL << shifts[i]) - 4 * ps;
		regions[i] = mmap((void *)address, len, PROT_READ | PROT_WRITE,
				  MAP_FIXED_NOREPLACE |
					  (i % 2 ? MAP_SHARED :
						   MAP_PRIVATE | MAP_ANONYMOUS),
				  i % 2 ? fd : -1, i % 2 ? i * len : 0);
		if (regions[i] != (void *)address) {
			printf("not ok - %zuK boundary %u mmap errno=%d\n",
			       ps / 1024, shifts[i], errno);
			fail = 1;
			goto out;
		}
	}
	for (unsigned i = 0; i < nr; i++)
		memset(regions[i], 0x19, len);
	/* Publish earlier folios from the bounded per-CPU LRU-add batch. */
	scratch = mmap(NULL, 8UL << 20, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (scratch == MAP_FAILED) {
		fail = 1;
		goto out;
	}
	memset(scratch, 0x7b, 8UL << 20);
	/* Seed MGLRU's first-access referenced state before testing the next access. */
	if (age()) {
		fail = 1;
		goto out;
	}
	for (unsigned round = 0; round < 4; round++) {
		if (ready_fd >= 0) {
			unsigned cpu = (worker_index + round) % 2;
			CPU_ZERO(&cpus);
			CPU_SET(cpu, &cpus);
			if (sched_setaffinity(0, sizeof(cpus), &cpus) ||
			    sched_getcpu() != (int)cpu) {
				fail = 1;
				break;
			}
			printf("# concurrent ABI=%zuK round=%u cpu=%u\n",
			       ps / 1024, round, cpu);
		}
		for (unsigned i = 0; i < nr; i++)
			for (size_t j = 0; j < len; j++)
				regions[i][j] = pattern(i, j, round);
		fail |= check_referenced(regions, nr, len / 1024);
		if (age() || age()) {
			fail = 1;
			break;
		}
		fail |= check_referenced(regions, nr, 0);
		int good = 1;
		for (unsigned i = 0; i < nr; i++)
			for (size_t j = 0; j < len; j++)
				if (regions[i][j] != pattern(i, j, round)) {
					good = 0;
					break;
				}
		unsigned long young = scan_evidence();
		printf("%s - %zuK round%u eight table boundaries bytes and aging young=%lu\n",
		       good && young ? "ok" : "not ok", ps / 1024, round,
		       young);
		fail |= !good || !young;
	}
out:
	if (scratch != MAP_FAILED)
		munmap(scratch, 8UL << 20);
	for (unsigned i = 0; i < nr; i++)
		if (regions[i] && regions[i] != MAP_FAILED)
			munmap(regions[i], len);
	close(fd);
	return fail;
}
static int concurrent(unsigned long *sizes, unsigned nr)
{
	int ready[3][2], commands[3][2], failed = 0;
	pid_t workers[3];

	for (unsigned i = 0; i < nr; i++)
		if (pipe(ready[i]) || pipe(commands[i]))
			return 1;
	for (unsigned i = 0; i < nr; i++) {
		workers[i] = fork();
		if (!workers[i]) {
			char size[32], index[16], ready_arg[16],
				command_arg[16];
			for (unsigned j = 0; j < nr; j++) {
				close(ready[j][0]);
				close(commands[j][1]);
				if (j != i) {
					close(ready[j][1]);
					close(commands[j][0]);
				}
			}
			snprintf(size, sizeof(size), "%lu", sizes[i]);
			snprintf(index, sizeof(index), "%u", i);
			snprintf(ready_arg, sizeof(ready_arg), "%d",
				 ready[i][1]);
			snprintf(command_arg, sizeof(command_arg), "%d",
				 commands[i][0]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(3);
			execl("/init", "/init", "--worker", size, index,
			      ready_arg, command_arg, NULL);
			_exit(4);
		}
		if (workers[i] < 0)
			failed = 1;
	}
	for (unsigned i = 0; i < nr; i++) {
		close(ready[i][1]);
		close(commands[i][0]);
	}
	/* One warm-up barrier plus two aging barriers for each of four rounds. */
	for (unsigned phase = 0; phase < 9 && !failed; phase++) {
		for (unsigned i = 0; i < nr; i++) {
			char token;
			if (read(ready[i][0], &token, 1) != 1 || token != 'A')
				failed = 1;
		}
		if (failed || age()) {
			failed = 1;
			break;
		}
		for (unsigned i = 0; i < nr; i++) {
			char token = 'G';
			if (write(commands[i][1], &token, 1) != 1)
				failed = 1;
		}
	}
	for (unsigned i = 0; i < nr; i++) {
		close(ready[i][0]);
		close(commands[i][1]);
	}
	for (unsigned i = 0; i < nr; i++) {
		int status = 0;
		int ok = workers[i] > 0 &&
			 waitpid(workers[i], &status, 0) == workers[i] &&
			 WIFEXITED(status) && !WEXITSTATUS(status);
		printf("%s - concurrent MGLRU ABI %luK status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		failed |= !ok;
	}
	return failed;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);
	if (argc == 3 && !strcmp(argv[1], "--exercise")) {
		if (getppid() != 1)
			return 2;
		return exercise(strtoul(argv[2], NULL, 10));
	}
	if (argc == 6 && !strcmp(argv[1], "--worker")) {
		if (getppid() != 1)
			return 2;
		worker_index = strtoul(argv[3], NULL, 10);
		ready_fd = atoi(argv[4]);
		command_fd = atoi(argv[5]);
		return exercise(strtoul(argv[2], NULL, 10));
	}
	int concurrent_mode = argc == 2 && !strcmp(argv[1], "--concurrent");
	if (getpid() != 1 || (argc != 1 && !concurrent_mode))
		return 2;
	int fail = 0;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL)) {
		perror("mount");
		fail = 1;
		goto done;
	}
	FILE *f = fopen("/sys/kernel/mm/lru_gen/enabled", "r");
	unsigned enabled = 0;
	if (f) {
		fscanf(f, "%x", &enabled);
		fclose(f);
	}
	if ((enabled & 3) != 3) {
		printf("not ok - MGLRU/MM walk disabled %#x\n", enabled);
		fail = 1;
		goto done;
	}
	printf("ok - MGLRU enabled=%#x\n", enabled);
	unsigned long sizes[] = { getauxval(AT_PAGESZ), 4096, 16384 };
	if (concurrent_mode) {
		fail = concurrent(sizes, sizes[0] > 16384 ? 3 : 2);
		goto done;
	}
	for (unsigned i = 0; i < (sizes[0] > 16384 ? 3U : 2U); i++) {
		pid_t child = fork();
		int status = 0;
		if (!child) {
			char value[32];
			snprintf(value, sizeof(value), "%lu", sizes[i]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(3);
			execl("/init", "/init", "--exercise", value, NULL);
			_exit(4);
		}
		int good = child > 0 && waitpid(child, &status, 0) == child &&
			   WIFEXITED(status) && !WEXITSTATUS(status);
		printf("%s - MGLRU ABI %luK status=%#x\n",
		       good ? "ok" : "not ok", sizes[i] / 1024, status);
		fail |= !good;
	}
done:
	printf("GRANULE MGLRU%s %s\n", concurrent_mode ? " CONCURRENT" : "",
	       fail ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
