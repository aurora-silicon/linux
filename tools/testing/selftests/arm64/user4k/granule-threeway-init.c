/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
struct snapshot {
	long pss;
	int first, last, status;
};
struct peer {
	pid_t pid;
	int command, reply;
	size_t size;
};
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
static int transfer(int fd, void *data, size_t size, int writing)
{
	size_t done = 0;
	while (done < size) {
		ssize_t n =
			writing ? write(fd, (char *)data + done, size - done) :
				  read(fd, (char *)data + done, size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return 0;
		done += n;
	}
	return 1;
}
static int worker(int fd, int command, int reply, size_t size)
{
	const struct rlimit no_core = { 0, 0 };
	setrlimit(RLIMIT_CORE, &no_core);
	if (getppid() != 1 || getauxval(AT_PAGESZ) != size)
		return 2;
	unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
				fd, 65536 - size);
	if (p == MAP_FAILED)
		return 3;
	for (;;) {
		char op;
		struct snapshot s = { .pss = -1, .first = -1, .last = -1 };
		if (!transfer(command, &op, 1, 0))
			return 4;
		if (op == 'x') {
			s.status = munmap(p, size);
			transfer(reply, &s, sizeof(s), 1);
			return s.status ? 5 : 0;
		}
		if (op == 'r')
			s.status = mprotect(p, size, PROT_READ);
		else if (op == 'w')
			p[size - 1] = 0x99;
		else if (op == 'd')
			s.status = madvise(p, size, MADV_DONTNEED);
		else if (op == 't') {
			pid_t c = fork();
			int status = 0;
			if (!c) {
				unsigned char value = *(
					volatile unsigned char *)(p + size - 1);
				_exit(value);
			}
			if (c < 0 || waitpid(c, &status, 0) != c)
				s.status = -1;
			else
				s.status = WIFSIGNALED(status) ?
						   256 + WTERMSIG(status) :
					   WIFEXITED(status) ?
						   WEXITSTATUS(status) :
						   -2;
		}
		if (op != 'd' && op != 't') {
			s.first = p[0];
			s.last = p[size - 1];
		}
		s.pss = pss(p);
		if (!transfer(reply, &s, sizeof(s), 1))
			return 6;
	}
}
static struct peer spawn(int fd, size_t size)
{
	int command[2], reply[2];
	struct peer p = { .pid = -1, .size = size };
	if (pipe(command) || pipe(reply))
		return p;
	p.pid = fork();
	if (!p.pid) {
		char f[24], c[24], r[24], s[24];
		close(command[1]);
		close(reply[0]);
		snprintf(f, sizeof(f), "%d", fd);
		snprintf(c, sizeof(c), "%d", command[0]);
		snprintf(r, sizeof(r), "%d", reply[1]);
		snprintf(s, sizeof(s), "%zu", size);
		if (prctl(SET_EXEC_PAGE_SIZE, size, 0UL, 0UL, 0UL))
			_exit(7);
		execl("/init", "/init", "--worker", f, c, r, s, NULL);
		_exit(8);
	}
	close(command[0]);
	close(reply[1]);
	p.command = command[1];
	p.reply = reply[0];
	return p;
}
static struct snapshot command(struct peer *p, char op)
{
	struct snapshot s = { .pss = -1, .first = -1, .last = -1, .status = -1 };
	if (p->pid <= 0 || !transfer(p->command, &op, 1, 1) ||
	    !transfer(p->reply, &s, sizeof(s), 0))
		failed++;
	return s;
}
static void finish(struct peer *p)
{
	struct snapshot s = command(p, 'x');
	int status = 0;
	result(!s.status && waitpid(p->pid, &status, 0) == p->pid &&
		       WIFEXITED(status) && !WEXITSTATUS(status),
	       "peer unmap and exit");
	close(p->command);
	close(p->reply);
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);
	if (argc == 6 && !strcmp(argv[1], "--worker"))
		return worker(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
			      strtoul(argv[5], NULL, 10));
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Disposable64K VM PID1 only\n");
		return 2;
	}
	mount("proc", "/proc", "proc", 0, NULL);
	if (getauxval(AT_PAGESZ) != 65536) {
		result(0, "native64K required");
		goto done;
	}
	int fd = memfd_create("three-granules", 0);
	if (fd < 0 || ftruncate(fd, 65536)) {
		result(0, "file setup");
		goto done;
	}
	unsigned char *p =
		mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		result(0, "native mapping");
		goto done;
	}
	memset(p, 0x42, 65536);
	struct peer medium = spawn(fd, 16384);
	struct snapshot s = command(&medium, 's');
	result(s.pss == 8 && s.first == 0x42 && s.last == 0x42 && pss(p) == 56,
	       "64K/16K initial sharing and PSS");
	struct peer small = spawn(fd, 4096);
	s = command(&small, 's');
	result(s.pss == 1 && s.first == 0x42 && s.last == 0x42,
	       "4K peer shares the final slice with both larger ABIs");
	s = command(&medium, 's');
	result(s.pss == 7 && pss(p) == 55,
	       "three-way PSS weights partial overlaps independently");
	s = command(&small, 'r');
	result(!s.status, "4K alias becomes read-only");
	s = command(&medium, 'w');
	result(!s.status && p[65535] == 0x99,
	       "16K alias remains writable across local4K protection");
	s = command(&small, 's');
	result(s.last == 0x99, "read-only4K alias observes16K writes");
	s = command(&small, 'd');
	result(!s.status && s.pss == 0 && pss(p) == 56,
	       "4K discard removes only its mapping");
	s = command(&medium, 's');
	result(s.pss == 8 && s.last == 0x99, "16K mapping survives4K discard");
	s = command(&small, 's');
	result(s.pss == 1 && s.last == 0x99 && pss(p) == 55,
	       "4K refault restores coherent three-way sharing");
	result(!ftruncate(fd, 65536 - 4096),
	       "truncate at4K boundary inside16K leaf");
	s = command(&small, 't');
	result(s.status == 256 + SIGBUS,
	       "4K mapping wholly beyond EOF faults SIGBUS");
	s = command(&medium, 't');
	result(s.status == 0, "partial16K EOF leaf exposes zero tail");
	result(p[65535] == 0 && p[0] == 0x42,
	       "native64K sees zero tail and preserved file prefix");
	s = command(&medium, 's');
	result(s.first == 0x42 && s.last == 0 && s.pss == 8 && pss(p) == 56,
	       "16K EOF refault keeps correct mixed PSS");
	result(!ftruncate(fd, 65536), "restore file length");
	s = command(&small, 's');
	result(s.first == 0 && s.last == 0 && s.pss == 1 && pss(p) == 55,
	       "4K alias refaults zeroed extension");
	finish(&small);
	s = command(&medium, 's');
	result(s.pss == 8 && pss(p) == 56, "16K alias survives4K exit");
	finish(&medium);
	result(pss(p) == 64,
	       "native PSS restored after all alternative aliases exit");
	munmap(p, 65536);
	close(fd);
done:
	printf("THREE GRANULES %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
