// SPDX-License-Identifier: GPL-2.0-only
/* PID-1 mixed-granule EL0 stress runner for disposable VMs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DATA_ADDRESS 0x200000000UL
#define CODE_ADDRESS 0x202000000UL
#define CONTEXTS 70000
#define SHORT_CONTEXTS 2048

static sigjmp_buf fault_return;
static volatile uintptr_t expected_fault;
static char generation_path[256];

static void fault(int sig, siginfo_t *info, void *context)
{
	(void)context;
	if (sig != SIGSEGV || !expected_fault ||
	    (uintptr_t)info->si_addr != expected_fault ||
	    info->si_code != SEGV_ACCERR)
		_exit(90);
	expected_fault = 0;
	siglongjmp(fault_return, 1);
}

static bool rejects_write(uintptr_t address)
{
	expected_fault = address;
	if (!sigsetjmp(fault_return, 1)) {
		*(volatile unsigned char *)expected_fault = 1;
		expected_fault = 0;
		return false;
	}
	return true;
}

static int migrate(unsigned int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static unsigned char pattern(unsigned long serial, unsigned long offset)
{
	serial ^= (serial >> 8) ^ (serial >> 16);
	return (serial * 37 + offset * 17 + (offset >> 8)) & 255;
}

static bool validate(unsigned char *p, unsigned long serial, unsigned long len)
{
	for (unsigned long i = 0; i < len; i++)
		if (p[i] != pattern(serial, i))
			return false;
	return true;
}

static int worker(unsigned long serial, unsigned long ps, int control)
{
	struct sigaction action = { .sa_sigaction = fault,
				    .sa_flags = SA_SIGINFO };
	unsigned char *data;
	uint32_t *code;
	unsigned int cpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct stat self, init;
	unsigned long iterations = 0;

	if (getauxval(AT_PAGESZ) != ps || getppid() != 1 ||
	    stat("/proc/self/exe", &self) || stat("/proc/1/exe", &init) ||
	    self.st_dev != init.st_dev || self.st_ino != init.st_ino ||
	    prctl(PR_AURORA_GET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL) != 0)
		return 91;
	if (cpus < 2 || migrate(serial % cpus) ||
	    sigaction(SIGSEGV, &action, NULL))
		return 92;
	data = mmap((void *)DATA_ADDRESS, 4 * ps, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	code = mmap((void *)CODE_ADDRESS, ps, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (data == MAP_FAILED || code == MAP_FAILED ||
	    mprotect(data, 3 * ps, PROT_READ | PROT_WRITE))
		return 93;
	for (unsigned long i = 0; i < 3 * ps; i++)
		data[i] = pattern(serial, i);
	if (!rejects_write(DATA_ADDRESS + 3 * ps))
		return 94;
	if (mprotect(data + ps, ps, PROT_READ))
		return 95;
	if (!rejects_write(DATA_ADDRESS + ps))
		return 96;
	do {
		unsigned int expected = (serial + iterations * 13) & 65535;
		int (*fn)(void) = (void *)code;
		char stop;
		ssize_t got;

		if (mprotect(code, ps, PROT_READ | PROT_WRITE))
			return 97;
		code[0] = 0x52800000 | (expected << 5); /* movz w0, #expected */
		code[1] = 0xd65f03c0; /* ret */
		__builtin___clear_cache((char *)code, (char *)(code + 2));
		if (mprotect(code, ps, PROT_READ | PROT_EXEC) ||
		    migrate((serial + iterations + 1) % cpus))
			return 98;
		sched_yield();
		if (fn() != (int)expected || !validate(data, serial, 3 * ps))
			return 99;
		if (!(iterations % 64) &&
		    (!rejects_write(DATA_ADDRESS + 3 * ps) ||
		     !rejects_write(DATA_ADDRESS + ps)))
			return 101;
		iterations++;
		if (control < 0)
			break;
		got = read(control, &stop, 1);
		if (got == 1)
			break;
		if (got != -1 || (errno != EAGAIN && errno != EINTR))
			return 100;
	} while (true);
	if (control >= 0)
		printf("# persistent %luK worker: %lu migration/JIT/data cycles\n",
		       ps / 1024, iterations);
	return 0;
}

static pid_t spawn(unsigned long serial, unsigned long ps, int control,
		   int close_fd)
{
	pid_t pid = fork();

	if (!pid) {
		char seq[32], size[32], fd[32];

		if (close_fd >= 0)
			close(close_fd);
		if (control >= 0 && fcntl(control, F_SETFD, 0))
			_exit(110);
		snprintf(seq, sizeof(seq), "%lu", serial);
		snprintf(size, sizeof(size), "%lu", ps);
		snprintf(fd, sizeof(fd), "%d", control);
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, ps, 0UL, 0UL, 0UL))
			_exit(111);
		execl("/init", "/init", "--worker", seq, size, fd, NULL);
		_exit(112);
	}
	return pid;
}

static bool reap(pid_t pid)
{
	int status = 0;
	pid_t got;

	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	if (got == pid && WIFEXITED(status) && !WEXITSTATUS(status))
		return true;
	printf("not ok - worker pid=%d wait=%d status=%#x\n", pid, got, status);
	return false;
}

static unsigned long long generation(void)
{
	unsigned long long value = 0;
	FILE *f = fopen(generation_path, "re");

	if (!f || fscanf(f, "%llu", &value) != 1) {
		perror("ASID generation");
		return 0;
	}
	fclose(f);
	return value;
}

int main(int argc, char **argv)
{
	unsigned long long before, after;
	unsigned char *data;
	pid_t persistent[3];
	int commands[3], pipes[2];
	unsigned long native = getauxval(AT_PAGESZ);
	unsigned long granules[] = { native, 4096, 16384 };
	unsigned int nr = native > 16384 ? 3 : 2;
	glob_t paths;
	bool ok = true;
	unsigned int contexts = CONTEXTS;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 5 && !strcmp(argv[1], "--worker"))
		return worker(strtoul(argv[2], NULL, 10),
			      strtoul(argv[3], NULL, 10), atoi(argv[4]));
	if (argc == 2 && !strcmp(argv[1], "--short"))
		contexts = SHORT_CONTEXTS;
	if (getpid() != 1 || (argc != 1 && contexts != SHORT_CONTEXTS)) {
		fprintf(stderr, "Run only as PID 1 in a disposable VM\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    glob("/sys/module/*/parameters/user4k_asid_generation", 0, NULL,
		 &paths) ||
	    paths.gl_pathc != 1) {
		perror("fixture setup");
		return 2;
	}
	snprintf(generation_path, sizeof(generation_path), "%s",
		 paths.gl_pathv[0]);
	globfree(&paths);
	data = mmap((void *)DATA_ADDRESS, 4 * native, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (data == MAP_FAILED)
		return 3;
	for (unsigned long i = 0; i < 4 * native; i++)
		data[i] = pattern(0, i);
	before = generation();
	printf("# mixed context stress: %u fork/exec workers, initial ASID generation=%llu\n",
	       contexts, before);
	for (unsigned int i = 0; i < nr; i++) {
		if (pipe2(pipes, O_NONBLOCK | O_CLOEXEC))
			return 4;
		persistent[i] = spawn(CONTEXTS + i + 1, granules[i], pipes[0],
				      pipes[1]);
		if (persistent[i] < 0)
			return 5;
		close(pipes[0]);
		commands[i] = pipes[1];
	}
	for (unsigned long i = 1; i <= contexts; i++) {
		pid_t pid = spawn(i, granules[i % nr], -1, -1);

		if (pid < 0 || !reap(pid) || !validate(data, 0, 4 * native)) {
			printf("not ok - churn/parent isolation at worker %lu\n",
			       i);
			ok = false;
			break;
		}
		if (!(i % 128))
			printf("# workers=%lu ASID generation=%llu\n", i,
			       generation());
	}
	after = generation();
	for (unsigned int i = 0; i < nr; i++) {
		ok &= write(commands[i], "x", 1) == 1;
		close(commands[i]);
		ok &= reap(persistent[i]);
	}
	ok &= before && after >= before + 2 * (1ULL << 16);
	printf("%s - mixed context stress: generation=%llu->%llu, measured rollovers=%llu\n",
	       ok ? "ok" : "not ok", before, after, (after - before) >> 16);
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
