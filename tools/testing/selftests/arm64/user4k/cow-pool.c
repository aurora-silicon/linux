// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exercise shared COW backing under a selected 4K userspace ABI. Run through
 * the platform's ABI launcher; this test never changes its own page contract.
 * Every selected leaf changes one byte, and all other bytes must survive.
 * Physical density and reclaim require privileged, platform-specific controls
 * and are deliberately not inferred from this unprivileged contents test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../kselftest.h"

#define BYTES (64UL << 20)
#define LEAF 4096UL
#define LEAVES (BYTES / LEAF)
#define WORKERS 8

enum mode { DENSE, SPARSE, PHASED, RANDOM, LOCKED, LATE_LOCKED, LAZYFREE };
struct scenario {
	const char *name;
	enum mode mode;
	unsigned int workers;
};

static unsigned char *data;
static size_t order[LEAVES];
static pthread_barrier_t barrier;
static const struct scenario *current;

static unsigned char initial(size_t leaf, size_t byte)
{
	return leaf * 17 + byte * 3 + 0x35;
}

static unsigned char changed(size_t leaf)
{
	return initial(leaf, 0) ^ 0xa5;
}

static bool verify(unsigned int generation, bool discard)
{
	unsigned int stride = current->mode == SPARSE ? 4 : 1;
	size_t leaf, byte;

	for (leaf = 0; leaf < LEAVES; leaf++) {
		if (generation && discard && leaf % 4 == 0)
			continue;
		for (byte = 0; byte < LEAF; byte++) {
			unsigned char want = initial(leaf, byte);

			if (generation && byte == 0 && leaf % stride == 0)
				want ^= generation == 1 ? 0xa5 : 0x5a;
			if (data[leaf * LEAF + byte] != want) {
				ksft_print_msg("pid=%d leaf=%zu byte=%zu got=%u want=%u\n",
					       getpid(), leaf, byte, data[leaf * LEAF + byte], want);
				return false;
			}
		}
	}
	return true;
}

static bool wait_ok(pid_t pid)
{
	pid_t result;
	int status;

	do {
		result = waitpid(pid, &status, 0);
	} while (result < 0 && errno == EINTR);
	return result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void *writer(void *arg)
{
	unsigned int id = (uintptr_t)arg;
	size_t leaf, index;
	unsigned int phase;

	pthread_barrier_wait(&barrier);
	if (current->mode == PHASED || current->mode == LATE_LOCKED ||
	    current->mode == LAZYFREE) {
		for (phase = 0; phase < 4; phase++) {
			for (leaf = phase + 4UL * id; leaf < LEAVES;
			     leaf += 4UL * current->workers)
				data[leaf * LEAF] = changed(leaf);
			pthread_barrier_wait(&barrier);
			if (phase == 0 && id == 0) {
				if (current->mode == LATE_LOCKED && mlock(data, BYTES))
					_exit(2);
				if (current->mode == LAZYFREE)
					for (leaf = 0; leaf < LEAVES; leaf += 4)
						if (madvise(data + leaf * LEAF, LEAF, MADV_FREE))
							_exit(2);
			}
			pthread_barrier_wait(&barrier);
		}
	} else {
		unsigned int stride = current->mode == SPARSE ? 4 : 1;

		for (index = id; index < LEAVES / stride; index += current->workers) {
			leaf = current->mode == RANDOM ? order[index] : index * stride;
			data[leaf * LEAF] = changed(leaf);
		}
	}
	return NULL;
}

static bool exercise(const struct scenario *scenario)
{
	pthread_t workers[WORKERS];
	struct timespec start, end;
	unsigned int t;
	pid_t child;

	current = scenario;
	child = fork();
	if (child < 0)
		return false;
	if (!child) {
		if (current->mode == LOCKED && mlock(data, BYTES))
			_exit(2);
		if (pthread_barrier_init(&barrier, NULL, current->workers))
			_exit(2);
		clock_gettime(CLOCK_MONOTONIC, &start);
		for (t = 0; t < current->workers; t++)
			if (pthread_create(&workers[t], NULL, writer, (void *)(uintptr_t)t))
				_exit(2);
		for (t = 0; t < current->workers; t++)
			if (pthread_join(workers[t], NULL))
				_exit(2);
		clock_gettime(CLOCK_MONOTONIC, &end);
		ksft_print_msg("%s fault/write phase %.6f s\n", current->name,
			       end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) * 1e-9);
		_exit(verify(1, current->mode == LAZYFREE) ? 0 : 3);
	}
	return wait_ok(child) && verify(0, false);
}

static bool owner_exit(void)
{
	static const struct scenario scenario = { "owner-mm exit", DENSE, 1 };
	int ready[2];
	pid_t child, descendant;
	size_t leaf;
	char signal;

	current = &scenario;
	if (prctl(PR_SET_CHILD_SUBREAPER, 1) || pipe(ready))
		return false;
	child = fork();
	if (child < 0) {
		close(ready[0]);
		close(ready[1]);
		return false;
	}
	if (!child) {
		close(ready[1]);
		for (leaf = 0; leaf < LEAVES; leaf++)
			data[leaf * LEAF] = changed(leaf);
		if (!verify(1, false))
			_exit(3);
		descendant = fork();
		if (descendant < 0)
			_exit(2);
		if (descendant)
			_exit(0);
		/* Root signals only after the original pool-owning mm has exited. */
		if (read(ready[0], &signal, 1) != 1 || signal != 'g')
			_exit(2);
		close(ready[0]);
		if (!verify(1, false))
			_exit(3);
		for (leaf = 0; leaf < LEAVES; leaf++)
			data[leaf * LEAF] ^= 0xff;
		_exit(verify(2, false) ? 0 : 3);
	}
	close(ready[0]);
	if (!wait_ok(child)) {
		close(ready[1]);
		return false;
	}
	if (write(ready[1], "g", 1) != 1) {
		close(ready[1]);
		return false;
	}
	close(ready[1]);
	return wait_ok(-1) && verify(0, false);
}

int main(void)
{
	static const struct scenario scenarios[] = {
		{ "dense single-thread COW", DENSE, 1 },
		{ "dense concurrent COW", DENSE, WORKERS },
		{ "single-offset sparse COW", SPARSE, 1 },
		{ "phased-offset concurrent COW", PHASED, WORKERS },
		{ "random-order concurrent COW", RANDOM, WORKERS },
		{ "locked COW", LOCKED, WORKERS },
		{ "mlock after first offset", LATE_LOCKED, WORKERS },
		{ "MADV_FREE after first offset", LAZYFREE, WORKERS },
	};
	struct rlimit limit;
	uint32_t seed = 0x9148140;
	size_t leaf, byte, i, j, tmp;
	bool can_lock;

	ksft_print_header();
	if (getauxval(AT_PAGESZ) != LEAF)
		ksft_exit_skip("run under a selected 4K userspace ABI\n");
	ksft_set_plan(9);
	alarm(180);
	data = mmap(NULL, BYTES, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (data == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	for (leaf = 0; leaf < LEAVES; leaf++)
		for (byte = 0; byte < LEAF; byte++)
			data[leaf * LEAF + byte] = initial(leaf, byte);
	for (i = 0; i < LEAVES; i++)
		order[i] = i;
	for (i = LEAVES; i > 1; i--) {
		seed = seed * 1664525 + 1013904223;
		j = seed % i;
		tmp = order[j];
		order[j] = order[i - 1];
		order[i - 1] = tmp;
	}
	can_lock = !getrlimit(RLIMIT_MEMLOCK, &limit) && limit.rlim_cur >= BYTES;
	for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (!can_lock && (scenarios[i].mode == LOCKED ||
				 scenarios[i].mode == LATE_LOCKED)) {
			ksft_test_result_skip("%s needs a 64MiB memlock limit\n", scenarios[i].name);
			continue;
		}
		ksft_test_result(exercise(&scenarios[i]), "%s preserves contents and parent\n",
				 scenarios[i].name);
	}
	ksft_test_result(owner_exit(), "descendant retains COW data after owner mm exits\n");
	munmap(data, BYTES);
	ksft_finished();
}
