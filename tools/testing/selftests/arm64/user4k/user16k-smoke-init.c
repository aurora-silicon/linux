/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define GET_EXEC_PAGE_SIZE 0x41555002
static int failed;
static _Thread_local int tls = 7;
static void result(int ok, const char *name)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", name);
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
static void *thread_worker(void *arg)
{
	unsigned char *p = arg;
	if (tls != 7)
		return (void *)1;
	tls = 91;
	for (unsigned i = 0; i < 16384; i++)
		if (p[i] != 0x42)
			return (void *)2;
	return NULL;
}
static void memory_checks(void)
{
	const size_t ps = 16384;
	int status = 0;
	pid_t c;
	unsigned char *p = mmap(NULL, 6 * ps, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	result(p != MAP_FAILED, "16K anonymous mmap");
	if (p == MAP_FAILED)
		return;
	for (unsigned i = 0; i < 6; i++)
		memset(p + i * ps, 0x40 + i, ps);
	c = fork();
	if (!c) {
		memset(p + ps, 0xb7, ps);
		_exit(p[0] == 0x40 && p[2 * ps] == 0x42 ? 0 : 1);
	}
	int ok = c > 0 && waitpid(c, &status, 0) == c && WIFEXITED(status) &&
		 !WEXITSTATUS(status);
	for (unsigned i = 0; i < 6 * ps; i++)
		ok &= p[i] == 0x40 + i / ps;
	result(ok, "16K fork COW preserves every parent byte");
	result(!mprotect(p + ps, ps, PROT_READ), "16K leaf write protection");
	c = fork();
	if (!c) {
		*(volatile unsigned char *)(p + ps + 8192) = 3;
		_exit(1);
	}
	result(c > 0 && waitpid(c, &status, 0) == c && WIFSIGNALED(status) &&
		       WTERMSIG(status) == SIGSEGV,
	       "write protection covers the interior of a16K leaf");
	result(!mprotect(p + ps, ps, PROT_READ | PROT_WRITE),
	       "16K write permission restored");
	errno = 0;
	result(mprotect(p + 4096, ps, PROT_READ) == -1 && errno == EINVAL,
	       "16K ABI rejects4K-only protection alignment");
	result(!madvise(p + ps, ps, MADV_DONTNEED),
	       "16K partial unmap via DONTNEED");
	ok = 1;
	for (unsigned i = 0; i < ps; i++)
		ok &= p[i] == 0x40 && p[ps + i] == 0 && p[2 * ps + i] == 0x42;
	result(ok, "16K discard clears one leaf and preserves neighbours");
	pthread_t thread;
	void *answer = (void *)3;
	int err = pthread_create(&thread, NULL, thread_worker, p + 2 * ps);
	if (!err)
		err = pthread_join(thread, &answer);
	result(!err && !answer && tls == 7, "16K pthread stack and TLS");
	struct timespec ts;
	result(!clock_gettime(CLOCK_MONOTONIC, &ts), "16K vDSO clock access");
	munmap(p, 6 * ps);
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);
	if (argc == 6 && !strcmp(argv[1], "--child")) {
		int fd = atoi(argv[2]), ready = atoi(argv[3]),
		    release = atoi(argv[4]);
		size_t native = strtoul(argv[5], NULL, 10);
		const size_t ps = 16384;
		result(getauxval(AT_PAGESZ) == ps && getpagesize() == ps,
		       "EL0 selects16K pages");
		result(prctl(GET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL) == 0,
		       "exec consumes alternative selection");
		if (!failed)
			memory_checks();
		{
			int status = 0;
			pid_t c = fork();
			if (!c) {
				if (prctl(SET_EXEC_PAGE_SIZE, 16384UL, 0UL, 0UL,
					  0UL))
					_exit(4);
				execl("/page-contract", "/page-contract", NULL);
				_exit(5);
			}
			result(c > 0 && waitpid(c, &status, 0) == c &&
				       WIFEXITED(status) &&
				       !WEXITSTATUS(status),
			       "16K-to16K exec and page/x18 contract");
		}
		unsigned char *shared = mmap(NULL, ps, PROT_READ | PROT_WRITE,
					     MAP_SHARED, fd, native - ps);
		unsigned char *private = mmap(NULL, ps, PROT_READ | PROT_WRITE,
					      MAP_PRIVATE, fd, native - ps);
		int ok = shared != MAP_FAILED && private != MAP_FAILED;
		if (ok) {
			for (unsigned i = 0; i < ps; i++)
				ok &= shared[i] == 0x7b && private[i] == 0x7b;
			memset(private, 0xd8, ps);
			for (unsigned i = 0; i < ps; i++)
				ok &= shared[i] == 0x7b && private[i] == 0xd8;
		}
		result(ok, "16K file COW copies the whole leaf");
		if (private != MAP_FAILED)
			munmap(private, ps);
		result(shared != MAP_FAILED && pss(shared) == 8,
		       "16K file PSS shares all four4K slices");
		if (shared != MAP_FAILED)
			shared[ps - 1] = 0xc9;
		char token = failed ? 'x' : 'a';
		if (write(ready, &token, 1) != 1 ||
		    read(release, &token, 1) != 1)
			return 3;
		if (shared != MAP_FAILED)
			munmap(shared, ps);
		return failed ? 1 : 0;
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Disposable VM PID1 only\n");
		return 2;
	}
	mount("proc", "/proc", "proc", 0, NULL);
	size_t native = getauxval(AT_PAGESZ);
	int fd = memfd_create("mixed16-smoke", 0);
	if (native != 65536 || fd < 0 || ftruncate(fd, native)) {
		result(0, "64K parent setup");
		goto done;
	}
	unsigned char *p =
		mmap(NULL, native, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		result(0, "parent shared mmap");
		goto done;
	}
	memset(p, 0x7b, native);
	result(pss(p) == 64, "native64K initial PSS");
	int ready[2], release[2];
	if (pipe(ready) || pipe(release)) {
		result(0, "pipes");
		goto done;
	}
	pid_t c = fork();
	int status = 0;
	if (!c) {
		char f[24], r[24], w[24], n[24];
		snprintf(f, sizeof(f), "%d", fd);
		snprintf(r, sizeof(r), "%d", ready[1]);
		snprintf(w, sizeof(w), "%d", release[0]);
		snprintf(n, sizeof(n), "%zu", native);
		close(ready[0]);
		close(release[1]);
		if (prctl(SET_EXEC_PAGE_SIZE, 16384UL, 0UL, 0UL, 0UL))
			_exit(4);
		execl("/init", "/init", "--child", f, r, w, n, NULL);
		_exit(5);
	}
	close(ready[1]);
	close(release[0]);
	char token = 0;
	int ok = c > 0 && read(ready[0], &token, 1) == 1 && token == 'a';
	result(ok && p[native - 1] == 0xc9,
	       "native64K and16K shared bytes remain coherent");
	result(pss(p) == 56,
	       "native64K PSS accounts one16K alias as four shared slices");
	token = 'b';
	if (write(release[1], &token, 1) != 1)
		failed++;
	result(c > 0 && waitpid(c, &status, 0) == c && WIFEXITED(status) &&
		       !WEXITSTATUS(status),
	       "16K child completes");
	printf("# child status=%#x\n", status);
	result(pss(p) == 64, "native PSS returns after16K exit");
	munmap(p, native);
done:
	printf("USER16K SMOKE %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
