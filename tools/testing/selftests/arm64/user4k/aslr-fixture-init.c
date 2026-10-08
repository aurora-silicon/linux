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
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

#define SET_EXEC_PAGE_SIZE 0x41555001
#define SAMPLES 64
struct sample {
	unsigned long page_size, stack, mapping, brk;
};

static int get_sample(struct sample *s)
{
	char buf[4096], *pos, *end;
	unsigned long low, high;
	FILE *f;
	int fd;
	ssize_t size;
	void *p;

	s->page_size = getauxval(AT_PAGESZ);
	p = mmap(NULL, s->page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return 1;
	s->mapping = (unsigned long)p;
	f = fopen("/proc/self/maps", "r");
	if (!f)
		return 1;
	while (fgets(buf, sizeof(buf), f))
		if (strstr(buf, "[stack]") &&
		    sscanf(buf, "%lx-%lx", &low, &high) == 2)
			s->stack = high;
	fclose(f);
	fd = open("/proc/self/stat", O_RDONLY);
	if (fd < 0)
		return 1;
	size = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (size <= 0)
		return 1;
	buf[size] = 0;
	pos = strrchr(buf, ')');
	if (!pos)
		return 1;
	pos += 2;
	/* Fields 1 and 2 precede the closing parenthesis; start_brk is 47. */
	for (unsigned int field = 3; field < 47; field++) {
		pos = strchr(pos, ' ');
		if (!pos)
			return 1;
		pos++;
	}
	errno = 0;
	s->brk = strtoul(pos, &end, 10);
	return errno || end == pos || !s->brk || !s->stack;
}

static int collect(unsigned int mode, bool disabled, struct sample *s)
{
	int pipefd[2], status;
	pid_t pid;
	ssize_t size;

	if (pipe(pipefd))
		return 1;
	pid = fork();
	if (!pid) {
		char fd[32];
		close(pipefd[0]);
		snprintf(fd, sizeof(fd), "%d", pipefd[1]);
		if (prctl(SET_EXEC_PAGE_SIZE, mode == 1 ? 4096UL : 0UL, 0UL,
			  0UL, 0UL))
			_exit(2);
		if (disabled && personality(ADDR_NO_RANDOMIZE) < 0)
			_exit(3);
		execl(mode == 2 ? "/compat-aslr" : "/init", "/init", "--sample",
		      fd, NULL);
		_exit(4);
	}
	close(pipefd[1]);
	size = read(pipefd[0], s, sizeof(*s));
	close(pipefd[0]);
	return pid < 0 || waitpid(pid, &status, 0) != pid ||
	       !WIFEXITED(status) || WEXITSTATUS(status) || size != sizeof(*s);
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ);
	int failed = 0;

	if (argc == 3 && !strcmp(argv[1], "--sample")) {
		struct sample s = { 0 };
		if (getppid() != 1 || get_sample(&s))
			return 1;
		return write(atoi(argv[2]), &s, sizeof(s)) != sizeof(s);
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Dedicated disposable VM PID1 only\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	if (mount("proc", "/proc", "proc", 0, NULL))
		failed++;
	int control = open("/proc/sys/kernel/randomize_va_space", O_WRONLY);
	if (control < 0 || write(control, "2", 1) != 1)
		failed++;
	if (control >= 0)
		close(control);
	int has4k = !prctl(SET_EXEC_PAGE_SIZE, 4096UL, 0UL, 0UL, 0UL);
	if (prctl(SET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL))
		failed++;
	for (unsigned int mode = 0; mode < 3; mode++) {
		if (mode == 1 && !has4k)
			continue;
		unsigned long ps = mode && has4k ? 4096 : native;
		struct sample first = { 0 };
		bool varied[3] = { 0 }, subnative[3] = { 0 };
		int ok = 1;

		for (unsigned int i = 0; i < SAMPLES; i++) {
			struct sample s = { 0 };
			if (collect(mode, false, &s)) {
				ok = 0;
				break;
			}
			unsigned long address[] = { s.stack, s.mapping, s.brk };
			unsigned long original[] = { first.stack, first.mapping,
						     first.brk };
			ok &= s.page_size == ps;
			for (unsigned int n = 0; n < 3; n++) {
				ok &= !(address[n] & (ps - 1));
				varied[n] |= i && address[n] != original[n];
				subnative[n] |= i &&
						((address[n] ^ original[n]) &
						 (native - 1));
			}
			if (!i)
				first = s;
		}
		for (unsigned int n = 0; n < 3; n++)
			ok &= varied[n] && (ps == native || subnative[n]);
		printf("%s - %s %luK ASLR: 64 execs, stack/mmap/brk varied=%d%d%d low-bits-varied=%d%d%d\n",
		       ok ? "ok" : "not ok", mode == 2 ? "AArch32" : "AArch64",
		       ps / 1024, varied[0], varied[1], varied[2], subnative[0],
		       subnative[1], subnative[2]);
		failed += !ok;
		struct sample a = { 0 }, b = { 0 };
		ok = !collect(mode, true, &a) && !collect(mode, true, &b) &&
		     !memcmp(&a, &b, sizeof(a)) && a.page_size == ps;
		printf("%s - %s %luK ADDR_NO_RANDOMIZE retains stable layout\n",
		       ok ? "ok" : "not ok", mode == 2 ? "AArch32" : "AArch64",
		       ps / 1024);
		failed += !ok;
	}
	printf("GRANULE ASLR %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
