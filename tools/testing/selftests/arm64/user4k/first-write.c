// SPDX-License-Identifier: GPL-2.0-only
/*
 * Concurrent first writes to absent PTEs must preserve the untouched bytes,
 * neighbouring private mappings and the source file. Small mappings have an
 * inaccessible 4K guard between them, so anonymous fault-around cannot fill
 * the whole native page. Density/reclaim are measured separately on hardware.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kselftest.h"

#define LEAF 4096UL
#define LEAVES 1024UL
#define WORKERS 8
#define TMPFS_MAGIC 0x01021994

enum backing { ANONYMOUS, SHMEM, REGULAR };
static unsigned char *data;
static size_t stride;
static pthread_barrier_t barrier;
static bool race;
static volatile sig_atomic_t running_child;
static void stop_fixture(int sig)
{
	if (running_child > 0)
		kill(running_child, SIGKILL);
	_exit(128 + sig);
}

static unsigned char initial(size_t leaf, size_t byte)
{
	return leaf * 17 + byte * 3 + 0x39;
}

static void *writer(void *arg)
{
	size_t leaf, id = (uintptr_t)arg;

	pthread_barrier_wait(&barrier);
	if (race) {
		/* Distinct bytes avoid a C data race while faulting the same PTE. */
		for (leaf = 0; leaf < LEAVES; leaf += 3)
			data[leaf * stride + id] = initial(leaf, id) ^ 0xa5;
	} else {
		for (leaf = id; leaf < LEAVES; leaf += WORKERS)
			data[leaf * stride] = initial(leaf, 0) ^ 0xa5;
	}
	return NULL;
}

static bool verify(enum backing backing)
{
	size_t leaf, byte;

	for (leaf = 0; leaf < LEAVES; leaf++) {
		for (byte = 0; byte < LEAF; byte++) {
			unsigned char want = backing == ANONYMOUS ? 0 : initial(leaf, byte);

			if (!byte || (race && leaf % 3 == 0 && byte < WORKERS))
				want = initial(leaf, byte) ^ 0xa5;
			if (data[leaf * stride + byte] != want) {
				ksft_print_msg("leaf=%zu byte=%zu got=%u want=%u\n", leaf, byte,
					       data[leaf * stride + byte], want);
				return false;
			}
		}
	}
	return true;
}

static int exercise(enum backing backing, bool small)
{
	unsigned char buffer[LEAF];
	char name[] = "first-write-XXXXXX";
	pthread_t workers[WORKERS];
	struct statfs fs;
	int fd = -1, flags = MAP_PRIVATE;
	size_t leaf, byte, span;
	unsigned int t;

	stride = small ? 2 * LEAF : LEAF;
	span = LEAVES * stride;
	if (backing != ANONYMOUS) {
		fd = backing == SHMEM ? memfd_create("first-write", MFD_CLOEXEC) :
			mkstemp(name);
		if (fd < 0)
			return 2;
		if (backing == REGULAR)
			unlink(name);
		if (fstatfs(fd, &fs))
			return 2;
		if (backing == REGULAR && fs.f_type == TMPFS_MAGIC) {
			ksft_print_msg("regular-file arm needs a non-tmpfs working directory\n");
			return KSFT_SKIP;
		}
		if (ftruncate(fd, span))
			return 2;
		for (leaf = 0; leaf < LEAVES; leaf++) {
			for (byte = 0; byte < LEAF; byte++)
				buffer[byte] = initial(leaf, byte);
			if (pwrite(fd, buffer, LEAF, leaf * stride) != LEAF)
				return 2;
		}
	} else {
		flags |= MAP_ANONYMOUS;
	}
	data = mmap(NULL, span, small ? PROT_NONE : PROT_READ | PROT_WRITE,
		    small ? MAP_PRIVATE | MAP_ANONYMOUS : flags, small ? -1 : fd, 0);
	if (data == MAP_FAILED)
		return 2;
	if (small)
		for (leaf = 0; leaf < LEAVES; leaf++)
			if (mmap(data + leaf * stride, LEAF, PROT_READ | PROT_WRITE,
				 flags | MAP_FIXED, fd, backing == ANONYMOUS ? 0 : leaf * stride) ==
			    MAP_FAILED)
				return 2;

	/* No reads through the mapping before concurrent direct first writes. */
	if (pthread_barrier_init(&barrier, NULL, WORKERS))
		return 2;
	for (t = 0; t < WORKERS; t++)
		if (pthread_create(&workers[t], NULL, writer, (void *)(uintptr_t)t))
			return 2;
	for (t = 0; t < WORKERS; t++)
		if (pthread_join(workers[t], NULL))
			return 2;
	if (!verify(backing))
		return 3;
	/* Drop selected private slots, leaving the other slots live. */
	for (leaf = 0; leaf < LEAVES; leaf += 3)
		if (madvise(data + leaf * stride, LEAF, MADV_DONTNEED))
			return 2;
	/* Again no reads before the first writes: exercise abort and slot reuse. */
	race = true;
	for (t = 0; t < WORKERS; t++)
		if (pthread_create(&workers[t], NULL, writer, (void *)(uintptr_t)t))
			return 2;
	for (t = 0; t < WORKERS; t++)
		if (pthread_join(workers[t], NULL))
			return 2;
	pthread_barrier_destroy(&barrier);
	if (!verify(backing))
		return 3;
	if (backing != ANONYMOUS) {
		for (leaf = 0; leaf < LEAVES; leaf++) {
			if (pread(fd, buffer, LEAF, leaf * stride) != LEAF)
				return 2;
			for (byte = 0; byte < LEAF; byte++)
				if (buffer[byte] != initial(leaf, byte))
					return 3;
		}
		close(fd);
	}
	munmap(data, span);
	return 0;
}

int main(int argc, char **argv)
{
	static const char * const names[] = { "anonymous", "shmem", "regular-file" };
	unsigned int backing, small;
	int status;
	pid_t child, result;

	if (argc != 2 || strcmp(argv[1], "--native-16k"))
		return 2;
	signal(SIGALRM, stop_fixture);
	signal(SIGTERM, stop_fixture);
	signal(SIGINT, stop_fixture);
	ksft_print_header();
	if (getauxval(AT_PAGESZ) != LEAF)
		ksft_exit_skip("run under a selected 4K userspace ABI\n");
	ksft_set_plan(6);
	fflush(NULL);
	alarm(40);
	for (backing = ANONYMOUS; backing <= REGULAR; backing++) {
		for (small = 0; small <= 1; small++) {
			child = fork();
			if (child < 0)
				ksft_exit_fail_msg("fork: %s\n", strerror(errno));
			if (!child) {
				running_child = 0;
				alarm(15);
				_exit(exercise(backing, small));
			}
			running_child = child;
			do {
				result = waitpid(child, &status, 0);
			} while (result < 0 && errno == EINTR);
			running_child = 0;
			if (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP)
				ksft_test_result_skip("%s %s first writes\n", names[backing],
						      small ? "guarded" : "dense");
			else
				ksft_test_result(result > 0 && WIFEXITED(status) &&
						 !WEXITSTATUS(status),
						 "%s %s first writes and slot reuse preserve contents\n",
						 names[backing], small ? "guarded" : "dense");
		}
	}
	ksft_finished();
}
