/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
static int failed;
static void result(int ok, const char *s)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", s);
	failed += !ok;
}
static long pss(void *p)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	char *line = NULL;
	size_t cap = 0;
	unsigned long start, end;
	int active = 0;
	long value = -1;
	if (!f)
		return -1;
	while (getline(&line, &cap, f) > 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2)
			active = (unsigned long)p >= start &&
				 (unsigned long)p < end;
		else if (active && sscanf(line, "Pss: %ld kB", &value) == 1)
			break;
	}
	free(line);
	fclose(f);
	return value;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 6 && !strcmp(argv[1], "--peer")) {
		int fd = atoi(argv[2]), ready = atoi(argv[3]),
		    release = atoi(argv[4]);
		size_t native = strtoul(argv[5], NULL, 10);
		unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, native - 4096);
		char token = 'a';
		int ok = getauxval(AT_PAGESZ) == 4096 && p != MAP_FAILED;
		if (ok) {
			for (unsigned i = 0; i < 4096; i++)
				ok &= p[i] == 0x7b;
			p[0] = 0xc9;
			ok &= pss(p) == 2;
		}
		token = ok ? 'a' : 'x';
		if (write(ready, &token, 1) != 1)
			return 3;
		if (read(release, &token, 1) != 1)
			return 4;
		return ok ? 0 : 2;
	}
	if (getpid() != 1) {
		fprintf(stderr, "Disposable VM PID1 only\n");
		return 2;
	}
	mount("proc", "/proc", "proc", 0, NULL);
	size_t native = getauxval(AT_PAGESZ);
	int fd = memfd_create("high-slot-pss", 0);
	result(fd >= 0 && !ftruncate(fd, native),
	       "native file backing created");
	unsigned char *p =
		mmap(NULL, native, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		result(0, "parent mapping");
		goto done;
	}
	memset(p, 0x7b, native);
	long before = pss(p);
	printf("# native=%zu initial PSS=%ldKB\n", native, before);
	result(before == (long)native / 1024,
	       "native PSS accounts every physical slot");
	int ready[2], release[2];
	if (pipe(ready) || pipe(release)) {
		result(0, "pipes");
		goto done;
	}
	pid_t child = fork();
	int status = 0;
	if (!child) {
		char f[24], r[24], w[24], n[24];
		snprintf(f, sizeof(f), "%d", fd);
		snprintf(r, sizeof(r), "%d", ready[1]);
		snprintf(w, sizeof(w), "%d", release[0]);
		snprintf(n, sizeof(n), "%zu", native);
		close(ready[0]);
		close(release[1]);
		if (prctl(SET_EXEC_PAGE_SIZE, 4096UL, 0UL, 0UL, 0UL))
			_exit(4);
		execl("/init", "/init", "--peer", f, r, w, n, NULL);
		_exit(5);
	}
	close(ready[1]);
	close(release[0]);
	char token = 0;
	int ok = child > 0 && read(ready[0], &token, 1) == 1 && token == 'a';
	result(ok && p[native - 4096] == 0xc9,
	       "4K peer shares the final physical slot and observes 2KB PSS");
	long during = pss(p);
	printf("# parent mixed PSS=%ldKB expected=%zuKB\n", during,
	       native / 1024 - 2);
	result(during == (long)native / 1024 - 2,
	       "native PSS weights the high shared slot independently");
	token = 'b';
	if (write(release[1], &token, 1) != 1)
		failed++;
	ok = waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	     !WEXITSTATUS(status);
	result(ok && pss(p) == (long)native / 1024,
	       "PSS returns to native size after peer releases its mapping");
	FILE *f = fopen("/proc/self/smaps_rollup", "r");
	char buf[512];
	ok = f != NULL;
	if (f) {
		while (fgets(buf, sizeof(buf), f)) {
		}
		ok &= !ferror(f);
		fclose(f);
	}
	result(ok, "smaps_rollup reads complete native slot accounting");
	munmap(p, native);
done:
	printf("GRANULE PROC %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
