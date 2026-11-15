/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
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
#include <sys/wait.h>
#include <unistd.h>

#define SET_EXEC_PAGE_SIZE 0x41555001
static int failed;
static void result(int ok, const char *name)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", name);
	failed += !ok;
}
static int pin_cpu(pid_t pid, unsigned int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(pid, sizeof(set), &set);
}
static int online_cpu1(void)
{
	char c = '?';
	int fd = open("/sys/devices/system/cpu/cpu1/online", O_RDONLY);
	if (fd >= 0) {
		if (read(fd, &c, 1) != 1)
			c = '?';
		close(fd);
	}
	return c == '0' ? 0 : c == '1' ? 1 : -1;
}
static int worker(int ready, int command, unsigned long expected)
{
	unsigned char *p;
	unsigned long ps = getauxval(AT_PAGESZ);
	char cpu;
	int ok = getppid() == 1 && ps == expected;

	p = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return 2;
	for (unsigned long i = 0; i < 4 * ps; i++)
		p[i] = i ^ (i >> 12);
	if (write(ready, "r", 1) != 1 || read(command, &cpu, 1) != 1)
		return 3;
	ok &= sched_getcpu() == cpu - '0';
	for (unsigned int round = 0; round < 128; round++) {
		for (unsigned long i = 0; i < 4 * ps; i++)
			if (p[i] != (unsigned char)(i ^ (i >> 12)))
				ok = 0;
		sched_yield();
	}
	munmap(p, 4 * ps);
	return ok ? 0 : 4;
}
static int compat_sample(unsigned long expected)
{
	uint64_t data[4] = { 0 };
	int pipefd[2], status;
	pid_t child;
	ssize_t size;

	if (pipe(pipefd))
		return 0;
	child = fork();
	if (!child) {
		char fd[32];
		close(pipefd[0]);
		snprintf(fd, sizeof(fd), "%d", pipefd[1]);
		execl("/compat-aslr", "/compat-aslr", "--sample", fd, NULL);
		_exit(2);
	}
	close(pipefd[1]);
	size = read(pipefd[0], data, sizeof(data));
	close(pipefd[0]);
	return child > 0 && waitpid(child, &status, 0) == child &&
	       WIFEXITED(status) && !WEXITSTATUS(status) &&
	       size == sizeof(data) && data[0] == expected;
}
int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--worker"))
		return worker(atoi(argv[2]), atoi(argv[3]),
			      strtoul(argv[4], NULL, 10));
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr,
			"Dedicated two-CPU disposable VM with maxcpus=1 only\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL)) {
		perror("mount");
		failed++;
		goto done;
	}
	unsigned long native = getauxval(AT_PAGESZ);
	int has4k = !prctl(SET_EXEC_PAGE_SIZE, 4096UL, 0UL, 0UL, 0UL);
	int has16k = native > 16384 &&
		     !prctl(SET_EXEC_PAGE_SIZE, 16384UL, 0UL, 0UL, 0UL);
	result(!prctl(SET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL),
	       "native exec default restored");
	char cmdline[4096] = { 0 };
	int fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0 || read(fd, cmdline, sizeof(cmdline) - 1) < 0) {
		failed++;
		goto done;
	}
	close(fd);
	int reject = has16k && strstr(cmdline, "arm64.user16k_deny_late_cpu=1");
	result(online_cpu1() == 0, "CPU1 starts offline");
	if (online_cpu1() != 0)
		goto done;
	int ready[2], command[2];
	if (pipe(ready) || pipe(command)) {
		failed++;
		goto done;
	}
	pid_t child = fork();
	if (!child) {
		char ready_fd[32], command_fd[32], ps[32];
		close(ready[0]);
		close(command[1]);
		snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]);
		snprintf(command_fd, sizeof(command_fd), "%d", command[0]);
		snprintf(ps, sizeof(ps), "%lu", has16k ? 16384UL : native);
		if (prctl(SET_EXEC_PAGE_SIZE, has16k ? 16384UL : 0UL, 0UL, 0UL,
			  0UL))
			_exit(5);
		execl("/init", "/init", "--worker", ready_fd, command_fd, ps,
		      NULL);
		_exit(6);
	}
	close(ready[1]);
	close(command[0]);
	if (child < 0) {
		failed++;
		goto done;
	}
	char token;
	result(read(ready[0], &token, 1) == 1 && token == 'r',
	       "worker has live mappings before hotplug");
	close(ready[0]);
	fd = open("/sys/devices/system/cpu/cpu1/online", O_WRONLY);
	errno = 0;
	ssize_t written = fd < 0 ? -1 : write(fd, "1", 1);
	int saved_errno = errno;
	if (fd >= 0)
		close(fd);
	int online = online_cpu1();
	printf("# has16k=%d expected-reject=%d write=%zd errno=%d online=%d\n",
	       has16k, reject, written, saved_errno, online);
	result(reject ? written < 0 && online == 0 :
			written == 1 && online == 1,
	       "late CPU admission matches advertised user granule");
	unsigned int cpu = online == 1 ? 1 : 0;
	result(!pin_cpu(child, cpu), "worker affinity targets admitted CPU");
	token = '0' + cpu;
	result(write(command[1], &token, 1) == 1,
	       "worker resumes after CPU admission");
	close(command[1]);
	int status;
	result(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		       !WEXITSTATUS(status),
	       "live mapping bytes survive and worker runs on expected CPU");
	result(!pin_cpu(0, cpu), "launcher affinity targets admitted CPU");
	if (has16k)
		result(!prctl(SET_EXEC_PAGE_SIZE, 16384UL, 0UL, 0UL, 0UL),
		       "16K request staged before compat exec");
	result(compat_sample(has4k ? 4096 : native),
	       "AArch32 exec selects4K or native despite pending16K");
done:
	printf("CPU GRANULE %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
