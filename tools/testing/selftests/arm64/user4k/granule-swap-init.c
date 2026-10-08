/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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
#include "swapfile-fixture.h"
#define SET_EXEC_PAGE_SIZE 0x41555001
static unsigned char pattern(size_t i, unsigned round)
{
	return (i ^ (i >> 12) ^ (i >> 20) ^ round) & 255;
}
static int check(unsigned char *p, size_t size, unsigned round)
{
	for (size_t i = 0; i < size; i++)
		if (p[i] != pattern(i, round)) {
			printf("# byte=%zu got=%u expected=%u\n", i, p[i],
			       pattern(i, round));
			return 0;
		}
	return 1;
}
static unsigned long swapouts(void)
{
	FILE *f = fopen("/proc/vmstat", "r");
	char k[80];
	unsigned long n = 0, result = 0;
	if (!f)
		return 0;
	while (fscanf(f, "%79s %lu", k, &n) == 2)
		if (!strcmp(k, "pswpout"))
			result = n;
	fclose(f);
	return result;
}
static unsigned swapped(void *p, size_t size, size_t ps)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	uint64_t bits;
	unsigned n = 0;
	if (fd < 0)
		return 0;
	for (size_t i = 0; i < size; i += ps)
		if (pread(fd, &bits, 8, ((uintptr_t)p + i) / ps * 8) == 8)
			n += (bits >> 62) & 1;
	close(fd);
	return n;
}
static int exercise(unsigned long expected)
{
	size_t ps = getauxval(AT_PAGESZ), size = 8 * 1024 * 1024;
	unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int failed = 0;
	if (ps != expected || p == MAP_FAILED) {
		printf("not ok - expected ABI=%lu actual=%zu\n", expected, ps);
		return 1;
	}
	for (unsigned round = 0; round < 4; round++) {
		for (size_t i = 0; i < size; i++)
			p[i] = pattern(i, round);
		unsigned long before = swapouts();
		int advice = 0;
		unsigned n = 0, attempts = 0;
		/* PAGEOUT is advisory. Require evidence, with a bounded retry window. */
		do {
			advice = madvise(p, size, MADV_PAGEOUT);
			attempts++;
			if (advice)
				break;
			usleep(20000);
			n = swapped(p, size, ps);
		} while (!n && attempts < 100);
		unsigned long written = swapouts() - before;
		int ok = !advice && written > 0 && n > 0;
		printf("%s - %zuK round%u real swapped PTEs=%u/%zu writes=%lu attempts=%u\n",
		       ok ? "ok" : "not ok", ps / 1024, round, n, size / ps,
		       written, attempts);
		failed += !ok;
		int release[2], ready[2];
		if (pipe(release) || pipe(ready))
			return 2;
		pid_t child = fork();
		int status = 0;
		if (!child) {
			char token;
			close(release[1]);
			close(ready[0]);
			int ok = read(release[0], &token, 1) == 1 &&
				 check(p, size, round);
			if (ok) {
				for (size_t i = 0; i < size; i += ps)
					p[i] ^= 0x6d;
			}
			token = ok ? 'a' : 'x';
			write(ready[1], &token, 1);
			_exit(ok ? 0 : 3);
		}
		close(release[0]);
		close(ready[1]);
		ok = check(p, size, round);
		printf("%s - %zuK round%u disk roundtrip bytes\n",
		       ok ? "ok" : "not ok", ps / 1024, round);
		failed += !ok;
		char token = 'b';
		write(release[1], &token, 1);
		ok = read(ready[0], &token, 1) == 1 && token == 'a' &&
		     waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		     !WEXITSTATUS(status) && check(p, size, round);
		printf("%s - %zuK round%u forked swap-in and COW retain parent bytes\n",
		       ok ? "ok" : "not ok", ps / 1024, round);
		failed += !ok;
		close(release[1]);
		close(ready[0]);
	}
	munmap(p, size);
	return failed ? 1 : 0;
}
static int run_suite(bool nonlinear)
{
	const char *base =
		nonlinear ?
			"/sys/kernel/debug/kunit/arm64-user4k-nonlinear-swap/" :
			"/sys/kernel/debug/kunit/arm64-user4k-swap/";
	char path[160];
	snprintf(path, sizeof(path), "%srun", base);
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return 1;
	int ok = write(fd, "1", 1) == 1;
	close(fd);
	snprintf(path, sizeof(path), "%sresults", base);
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t cap = 0;
	int completed = 0;
	if (!f)
		return 1;
	while (getline(&line, &cap, f) > 0) {
		fputs(line, stdout);
		if (strstr(line, "not ok") || strstr(line, "# SKIP"))
			ok = 0;
		if (strstr(line, nonlinear ?
					 "ok 1 arm64-user4k-nonlinear-swap" :
					 "ok 1 arm64-user4k-swap"))
			completed = 1;
	}
	free(line);
	fclose(f);
	return !(ok && completed);
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 2 && (!strcmp(argv[1], "--swapoff-helper") ||
			  !strcmp(argv[1], "--swapon-helper"))) {
		struct stat a, b;
		if (getpid() == 1 || stat("/proc/1/exe", &a) ||
		    stat("/proc/self/exe", &b) || a.st_dev != b.st_dev ||
		    a.st_ino != b.st_ino)
			return 1;
		return (!strcmp(argv[1], "--swapoff-helper") ?
				swapoff(swap_path()) :
				swapon(swap_path(), 0)) ?
			       1 :
			       0;
	}
	if (argc == 3 && !strcmp(argv[1], "--exercise")) {
		if (getppid() != 1)
			return 1;
		return exercise(strtoul(argv[2], NULL, 10));
	}
	if (getpid() != 1 ||
	    (argc != 1 && (argc != 2 || (strcmp(argv[1], "--el0-only") &&
					 strcmp(argv[1], "--nonlinear") &&
					 strcmp(argv[1], "--swapfile"))))) {
		fprintf(stderr, "Dedicated disposable VM PID1 only\n");
		return 2;
	}
	int failed = 0, active = 0;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL)) {
		failed++;
		goto done;
	}
	struct stat st;
	for (int i = 0; i < 100 && stat("/dev/vda", &st); i++)
		usleep(20000);
	if (stat("/dev/vda", &st) || !S_ISBLK(st.st_mode) ||
	    (argc == 2 && !strcmp(argv[1], "--swapfile") &&
	     prepare_swapfile()) ||
	    swapon(swap_path(), 0)) {
		perror("swapon");
		failed++;
		goto done;
	}
	active = 1;
	printf("GRANULE SWAP BACKING=%s\n", swap_path());
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--nonlinear")))
		failed += run_suite(argc == 2);
	unsigned long sizes[] = { getauxval(AT_PAGESZ), 4096, 16384 };
	for (unsigned abi = 0; abi < (sizes[0] > 16384 ? 3U : 2U); abi++) {
		pid_t c = fork();
		int status = 0;
		if (!c) {
			char expected[32];
			snprintf(expected, sizeof(expected), "%lu", sizes[abi]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[abi], 0UL, 0UL,
				  0UL))
				_exit(4);
			execl("/init", "/init", "--exercise", expected, NULL);
			_exit(5);
		}
		int ok = c > 0 && waitpid(c, &status, 0) == c &&
			 WIFEXITED(status) && !WEXITSTATUS(status);
		printf("%s - swap ABI %luK child status=%#x\n",
		       ok ? "ok" : "not ok", sizes[abi] / 1024, status);
		failed += !ok;
	}
done:
	if (active && swapoff(swap_path())) {
		perror("swapoff");
		failed++;
	}
	if (active && argc == 2 && !strcmp(argv[1], "--swapfile") &&
	    umount("/swapdir")) {
		perror("umount");
		failed++;
	}
	printf("GRANULE SWAP %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
