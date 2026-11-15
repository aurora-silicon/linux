// SPDX-License-Identifier: GPL-2.0-only
/* Disposable PID1: SysV shared memory with per-process page granules. */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int ok, const char *expression, int line)
{
	if (!ok) {
		fprintf(stderr, "FAIL line %d: %s errno=%d\n", line, expression,
			errno);
		exit(1);
	}
}

#define CHECK(x) check(!!(x), #x, __LINE__)

static int unmapped(void *address, size_t page)
{
	unsigned char resident;

	errno = 0;
	return mincore(address, page, &resident) == -1 && errno == ENOMEM;
}

static int mapping_case(unsigned long native, int rounded, int split)
{
	unsigned long page = getauxval(AT_PAGESZ), length = 4 * page;
	unsigned char *reserve, *target, *mapping, *alias;
	struct shmid_ds info;
	int id;

	reserve = mmap(NULL, 16 * native, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(reserve != MAP_FAILED);
	target = (void *)(((uintptr_t)reserve + native - 1) & ~(native - 1));
	target += native + (page < native ? page : 0);
	target[-1] = 0x51;
	target[length] = 0x72;
	CHECK(!munmap(target, length));
	id = shmget(IPC_PRIVATE, length - 1, 0600);
	CHECK(id >= 0);
	mapping = shmat(id, target + (rounded ? page - 1 : 0),
			rounded ? SHM_RND : 0);
	if (mapping != target) {
		int saved = errno;

		shmctl(id, IPC_RMID, NULL);
		errno = saved;
		CHECK(mapping == target);
	}
	alias = shmat(id, NULL, 0);
	CHECK(alias != (void *)-1);
	CHECK(!shmctl(id, IPC_RMID, NULL));
	mapping[0] = 0x31;
	mapping[page + 7] = 0x49;
	mapping[3 * page] = 0x63;
	CHECK(alias[0] == 0x31 && alias[page + 7] == 0x49 &&
	      alias[3 * page] == 0x63);
	if (split) {
		CHECK(!mprotect(mapping + page, page, PROT_READ));
		CHECK(!munmap(mapping + 2 * page, page));
		if (split == 2)
			CHECK(!munmap(mapping, page));
	}
	CHECK(!shmdt(mapping));
	for (unsigned long i = 0; i < length; i += page)
		CHECK(unmapped(target + i, page));
	CHECK(target[-1] == 0x51 && target[length] == 0x72);
	CHECK(alias[page + 7] == 0x49 && alias[3 * page] == 0x63);
	CHECK(!shmdt(alias));
	errno = 0;
	CHECK(shmctl(id, IPC_STAT, &info) == -1 &&
	      (errno == EINVAL || errno == EIDRM));
	CHECK(!munmap(reserve, 16 * native));
	printf("ok - SysV page=%lu rounded=%d split=%d detach and alias lifetime\n",
	       page, rounded, split);
	return 0;
}

static int wait_child(pid_t pid)
{
	int status;
	pid_t got;

	if (pid < 0)
		return 1;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	return got != pid || !WIFEXITED(status) || WEXITSTATUS(status);
}

static int sharing_child(int id, unsigned long expected)
{
	unsigned char *map;

	CHECK(getauxval(AT_PAGESZ) == expected);
	map = shmat(id, NULL, 0);
	CHECK(map != (void *)-1);
	CHECK(map[37] == 0x35 && map[4096 + 9] == 0x56);
	map[4096 + 9] = 0x78;
	CHECK(!shmdt(map));
	return 0;
}

static int sharing(unsigned long other, unsigned long native)
{
	unsigned long page = getauxval(AT_PAGESZ);
	unsigned char *map;
	pid_t pid;
	int id;

	id = shmget(IPC_PRIVATE, 2 * native, 0600);
	CHECK(id >= 0);
	map = shmat(id, NULL, 0);
	CHECK(map != (void *)-1);
	CHECK(!shmctl(id, IPC_RMID, NULL));
	map[37] = 0x35;
	map[4096 + 9] = 0x56;
	pid = fork();
	if (!pid) {
		char idarg[32], sizearg[32];

		snprintf(idarg, sizeof(idarg), "%d", id);
		snprintf(sizearg, sizeof(sizearg), "%lu", other);
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, other, 0UL, 0UL, 0UL))
			_exit(2);
		execl("/init", "/init", "--share", idarg, sizearg, NULL);
		_exit(3);
	}
	CHECK(!wait_child(pid));
	CHECK(map[4096 + 9] == 0x78);
	CHECK(!shmdt(map));
	printf("ok - SysV shared object %lu -> %lu after IPC_RMID\n", page,
	       other);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ);
	int failed = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 4 && !strcmp(argv[1], "--share"))
		return sharing_child(atoi(argv[2]), strtoul(argv[3], NULL, 10));
	if (argc == 5 && !strcmp(argv[1], "--case")) {
		unsigned long expected = strtoul(argv[3], NULL, 10);
		unsigned long sizes[] = { 4096, 16384, 65536 };

		native = strtoul(argv[2], NULL, 10);
		CHECK(getauxval(AT_PAGESZ) == expected);
		failed |= mapping_case(native, 0, 0);
		failed |= mapping_case(native, 1, 0);
		failed |= mapping_case(native, 0, 1);
		failed |= mapping_case(native, 0, 2);
		for (unsigned int i = 0; i < 3; i++)
			if (sizes[i] <= native && sizes[i] != expected &&
			    !(atoi(argv[4]) && sizes[i] == 4096))
				failed |= sharing(sizes[i], native);
		return failed;
	}
	CHECK(getpid() == 1);
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	int no4k = argc == 2 && !strcmp(argv[1], "--no4k");
	unsigned long sizes[] = { 4096, 16384, 65536 };

	for (unsigned int i = 0; i < 3; i++) {
		unsigned long page = sizes[i];
		pid_t pid;

		if (page > native || (no4k && page == 4096))
			continue;
		pid = fork();
		if (!pid) {
			char n[32], e[32];

			snprintf(n, sizeof(n), "%lu", native);
			snprintf(e, sizeof(e), "%lu", page);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, page, 0UL, 0UL,
				  0UL))
				_exit(2);
			execl("/init", "/init", "--case", n, e,
			      no4k ? "1" : "0", NULL);
			_exit(3);
		}
		int bad = wait_child(pid);

		printf("%s - SysV AArch64 native=%lu process=%lu\n",
		       bad ? "not ok" : "ok", native, page);
		failed |= bad;
	}
	if (access("/compat-shm", X_OK) == 0) {
		pid_t pid = fork();

		if (!pid) {
			char n[32], e[32];

			snprintf(n, sizeof(n), "%lu", native);
			snprintf(e, sizeof(e), "%lu", no4k ? native : 4096UL);
			execl("/compat-shm", "/compat-shm", n, e, NULL);
			_exit(3);
		}
		int bad = wait_child(pid);

		printf("%s - SysV AArch32 native=%lu process=%lu\n",
		       bad ? "not ok" : "ok", native, no4k ? native : 4096UL);
		failed |= bad;
	}
	printf("SYSV SHM GRANULE %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
