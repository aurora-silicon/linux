// SPDX-License-Identifier: GPL-2.0-only
/* Real EL0 mixed-granule sharing, futex queues, and process_vm copies. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NATIVE 16384U
#define QUARTER 4096U
#define ROUNDS 2000U
struct channel {
	atomic_int turn;
	unsigned int value;
	unsigned int worker_wakes;
};
struct ready {
	uintptr_t base;
	unsigned long page;
};

static int wait_turn(struct channel *channel, int wanted)
{
	const struct timespec timeout = { .tv_sec = 5 };
	int value;

	while ((value = atomic_load_explicit(&channel->turn,
					     memory_order_acquire)) != wanted) {
		if (syscall(SYS_futex, &channel->turn, FUTEX_WAIT, value,
			    &timeout, NULL, 0) < 0 &&
		    errno != EAGAIN && errno != EINTR)
			return -1;
	}
	return 0;
}

static int send_turn(struct channel *channel, int turn)
{
	atomic_store_explicit(&channel->turn, turn, memory_order_release);
	return syscall(SYS_futex, &channel->turn, FUTEX_WAKE, 1, NULL, NULL, 0);
}

static unsigned char pattern(size_t i)
{
	return (i ^ (i >> 8) ^ (i >> 16)) & 255;
}

static int worker(int fd, int ready_fd, unsigned long expected)
{
	unsigned long page = getauxval(AT_PAGESZ);
	unsigned char *mapping, *remote;
	struct channel *channel;
	struct ready ready;
	size_t i, begin = page - 13, end = 3 * page + 16;
	unsigned int round, wakes = 0;
	int ret;

	if (page != expected)
		return 10;
	mapping = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       page == QUARTER ? QUARTER : 0);
	remote = mmap(NULL, 5 * page, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED || remote == MAP_FAILED)
		return 11;
	channel = (void *)(mapping + (page == QUARTER ? 0 : QUARTER));
	for (i = 0; i < 4 * page; i++)
		remote[i] = pattern(i);
	if (mprotect(remote + 4 * page, page, PROT_NONE))
		return 12;
	ready = (struct ready){ (uintptr_t)remote, page };
	if (write(ready_fd, &ready, sizeof(ready)) != sizeof(ready))
		return 13;
	close(ready_fd);
	for (round = 0; round < ROUNDS; round++) {
		if (wait_turn(channel, 1) ||
		    channel->value != (round ^ 0xabcddcbaU))
			return 14;
		if (!round) {
			for (i = 0; i < 4 * page; i++)
				if (remote[i] !=
				    (pattern(i) ^
				     (i >= begin && i < end ? 0x5a : 0)))
					return 15;
		}
		channel->value = ~(round ^ 0xabcddcbaU);
		ret = send_turn(channel, 0);
		if (ret < 0)
			return 16;
		wakes += ret;
	}
	channel->worker_wakes = wakes;
	return 0;
}

static bool remote_copies(pid_t pid, const struct ready *ready)
{
	size_t begin = ready->page - 13, len = 2 * ready->page + 29, i;
	unsigned char *data = malloc(len), edge[2] = {};
	struct iovec local = { data, len },
		     remote = { (void *)(ready->base + begin), len };
	bool ok = data != NULL;

	if (!ok)
		return false;
	ok = process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
	for (i = 0; ok && i < len; i++)
		ok = data[i] == pattern(i + begin);
	for (i = 0; i < len; i++)
		data[i] = pattern(i + begin) ^ 0x5a;
	ok &= process_vm_writev(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
	local = (struct iovec){ edge, sizeof(edge) };
	remote = (struct iovec){ (void *)(ready->base + 4 * ready->page - 1),
				 sizeof(edge) };
	ok &= process_vm_readv(pid, &local, 1, &remote, 1, 0) == 1;
	ok &= edge[0] == pattern(4 * ready->page - 1);
	free(data);
	return ok;
}

int main(int argc, char **argv)
{
	unsigned long page = getauxval(AT_PAGESZ), other;
	unsigned char *mapping, *private;
	struct channel *channel;
	struct ready ready = {};
	int fd, pipefd[2], status = 0, wakes = 0, ret;
	unsigned int round = 0;
	pid_t pid;
	bool ok;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 5 && !strcmp(argv[1], "--worker"))
		return worker(atoi(argv[2]), atoi(argv[3]),
			      strtoul(argv[4], NULL, 10));
	if (argc != 1 || (page != QUARTER && page != NATIVE))
		return 2;
	other = page == NATIVE ? QUARTER : NATIVE;
	fd = memfd_create("mixed-process", 0);
	if (fd < 0 || ftruncate(fd, 2 * NATIVE) || pipe(pipefd))
		return 3;
	mapping = mmap(NULL, 2 * NATIVE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       0);
	private =
		mmap(NULL, NATIVE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (mapping == MAP_FAILED || private == MAP_FAILED)
		return 4;
	channel = (void *)(mapping + QUARTER);
	private[QUARTER] = 0x7e;
	pid = fork();
	if (!pid) {
		char fdarg[32], pipearg[32], sizearg[32];

		close(pipefd[0]);
		snprintf(fdarg, sizeof(fdarg), "%d", fd);
		snprintf(pipearg, sizeof(pipearg), "%d", pipefd[1]);
		snprintf(sizearg, sizeof(sizearg), "%lu", other);
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, other, 0UL, 0UL, 0UL))
			_exit(20);
		execl("/mixed-process", "/mixed-process", "--worker", fdarg,
		      pipearg, sizearg, NULL);
		_exit(21);
	}
	close(pipefd[1]);
	if (pid < 0)
		return 5;
	ok = read(pipefd[0], &ready, sizeof(ready)) == sizeof(ready) &&
	     ready.page == other;
	close(pipefd[0]);
	ok = ok && remote_copies(pid, &ready);
	printf("%s - process_vm copies %lu -> %lu, including partial inaccessible boundary\n",
	       ok ? "ok" : "not ok", page, other);
	for (round = 0; ok && round < ROUNDS; round++) {
		channel->value = round ^ 0xabcddcbaU;
		ret = send_turn(channel, 1);
		ok = ret >= 0 && !wait_turn(channel, 0) &&
		     channel->value == ~(round ^ 0xabcddcbaU);
		if (ret > 0)
			wakes += ret;
	}
	if (!ok)
		kill(pid, SIGKILL);
	ret = waitpid(pid, &status, 0);
	ok &= ret == pid && WIFEXITED(status) && !WEXITSTATUS(status);
	ok &= wakes > 0 && channel->worker_wakes > 0 &&
	      private[QUARTER] == 0x7e;
	ok &= mapping[0] == 0 && mapping[2 * QUARTER] == 0 &&
	      mapping[NATIVE - 1] == 0;
	printf("%s - mixed %lu/%lu shared futex/COW: rounds=%u wakes=%d/%u child=%#x\n",
	       ok ? "ok" : "not ok", page, other, round, wakes,
	       channel->worker_wakes, status);
	munmap(private, NATIVE);
	munmap(mapping, 2 * NATIVE);
	close(fd);
	return ok ? 0 : 1;
}
