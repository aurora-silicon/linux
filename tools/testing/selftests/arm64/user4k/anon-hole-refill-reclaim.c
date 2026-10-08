// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in own-cgroup reclaim of shared partial-hole backing after owner exit. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kselftest.h"

#define LEAF 4096UL
#define NATIVE 16384UL
#define BYTES (16UL << 20)
#define LEAVES (BYTES / LEAF)
#define WORKERS 4
#define ROUNDS 2

struct result {
	bool concurrent, lifetime, reclaimed;
	size_t swapped, non_swap_markers;
	unsigned int attempts;
};

static unsigned char *data;
static pthread_barrier_t barrier;
static volatile sig_atomic_t fixture_group;

static void stop_fixture(int signal_number)
{
	if (fixture_group > 0)
		kill(-fixture_group, SIGKILL);
	_exit(128 + signal_number);
}

static unsigned char tag(size_t leaf, unsigned int generation)
{
	unsigned char value = leaf % 251 + 1;

	if (generation == 1 && leaf < 4)
		value ^= 0xa5;
	else if (generation == 2)
		value ^= 0x5a;
	return value;
}

static bool verify_leaf(size_t leaf, unsigned int generation)
{
	size_t byte;

	if (data[leaf * LEAF] != tag(leaf, generation))
		return false;
	for (byte = 1; byte < LEAF; byte++)
		if (data[leaf * LEAF + byte] != 0x5a)
			return false;
	return true;
}

static bool verify(unsigned int generation)
{
	size_t leaf;

	for (leaf = 0; leaf < LEAVES; leaf++)
		if (!verify_leaf(leaf, generation))
			return false;
	return true;
}

static void *worker(void *arg)
{
	unsigned int id = (uintptr_t)arg, round, quarter, q;
	size_t group, leaf, byte;
	bool ok = true;

	for (round = 0; round < ROUNDS; round++) {
		for (quarter = 0; quarter < 4; quarter++) {
			pthread_barrier_wait(&barrier);
			for (group = id; group < BYTES / NATIVE; group += WORKERS) {
				unsigned char *p;

				leaf = group * 4 + quarter;
				p = data + leaf * LEAF;
				if (madvise(p, LEAF, MADV_DONTNEED)) {
					ok = false;
					continue;
				}
				/* A write fault, without a preceding zero-page read. */
				*(volatile unsigned char *)p = tag(leaf, 0);
				for (byte = 1; byte < LEAF; byte++)
					if (p[byte])
						ok = false;
				memset(p + 1, 0x5a, LEAF - 1);
				/* This worker owns the entire native virtual window. */
				for (q = 0; q < 4; q++)
					ok &= verify_leaf(group * 4 + q, 0);
			}
		}
	}
	return (void *)(uintptr_t)!ok;
}

static size_t swapped_leaves(size_t *markers)
{
	uint64_t entries[LEAVES];
	size_t i, count = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return SIZE_MAX;
	n = pread(fd, entries, sizeof(entries), (uintptr_t)data / LEAF * 8);
	close(fd);
	if (n != sizeof(entries))
		return SIZE_MAX;
	*markers = 0;
	for (i = 0; i < LEAVES; i++) {
		if ((entries[i] & (UINT64_C(1) << 63)) ||
		    !(entries[i] & (UINT64_C(1) << 62)))
			continue;
		/* Audited target config:27..31 are poison/migration/markers. */
		if ((entries[i] & 31) < 27)
			count++;
		else
			(*markers)++;
	}
	return count;
}

static bool wait_ok(pid_t pid)
{
	int status;
	pid_t got;

	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	return got > 0 && WIFEXITED(status) && !WEXITSTATUS(status);
}

static void owner(int go, int result_fd)
{
	struct result result = {};
	pthread_t threads[WORKERS];
	void *mapping, *status;
	unsigned int i;
	bool readback_ok;
	struct pollfd control = { .fd = STDIN_FILENO, .events = POLLIN };
	size_t leaf;
	pid_t survivor;
	char token;

	alarm(30);
	mapping = mmap(NULL, BYTES + NATIVE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		_exit(2);
	data = (void *)(((uintptr_t)mapping + NATIVE - 1) & ~(NATIVE - 1));
	memset(data, 0x5a, BYTES);
	for (leaf = 0; leaf < LEAVES; leaf++)
		data[leaf * LEAF] = tag(leaf, 0);
	if (pthread_barrier_init(&barrier, NULL, WORKERS))
		_exit(2);
	for (i = 0; i < WORKERS; i++)
		if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i))
			_exit(2);
	result.concurrent = true;
	for (i = 0; i < WORKERS; i++) {
		if (pthread_join(threads[i], &status))
			_exit(2);
		result.concurrent &= !status;
	}
	pthread_barrier_destroy(&barrier);
	result.concurrent &= verify(0);
	if (!result.concurrent)
		_exit(3);
	survivor = fork();
	if (survivor < 0)
		_exit(2);
	if (survivor) {
		/* The survivor must retain the original contents across this COW. */
		for (leaf = 0; leaf < 4; leaf++)
			data[leaf * LEAF] = tag(leaf, 1);
		_exit(verify(1) ? 0 : 3);
	}
	alarm(30);
	/* Supervisor waits for the original allocator's mm to disappear first. */
	if (read(go, &token, 1) != 1 || token != 'g')
		_exit(2);
	close(go);
	result.lifetime = verify(0);
	for (leaf = 0; leaf < LEAVES; leaf++)
		data[leaf * LEAF] = tag(leaf, 2);
	result.lifetime &= verify(2);
	/* Controller acts only on this fixture's cgroup. No pressure here. */
	printf("RECLAIM_READY pid=%d base=%p bytes=%lu owner_mm_exited=1\n",
	       getpid(), data, BYTES);
	fflush(stdout);
	if (poll(&control, 1, 20000) != 1 ||
	    read(STDIN_FILENO, &token, 1) != 1 || token != 'r')
		_exit(2);
	result.attempts = 1; /* One post-controller pagemap snapshot. */
	result.swapped = swapped_leaves(&result.non_swap_markers);
	readback_ok = verify(2);
	result.reclaimed = result.swapped != SIZE_MAX &&
		result.swapped >= LEAVES * 3 / 4 && readback_ok;

	if (write(result_fd, &result, sizeof(result)) != sizeof(result))
		_exit(2);
	close(result_fd);
	munmap(mapping, BYTES + NATIVE);
	_exit(result.concurrent && result.lifetime && result.reclaimed ? 0 : 3);
}

static bool visible_swap_metadata(void)
{
	FILE *file = fopen("/proc/self/status", "re");
	char line[256];
	unsigned long long caps = 0;

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file))
		if (sscanf(line, "CapEff: %llx", &caps) == 1)
			break;
	fclose(file);
	return caps & (1ULL << 21); /* CAP_SYS_ADMIN, needed for swap type bits. */
}

int main(int argc, char **argv)
{
	struct result result = {};
	int go[2], results[2];
	pid_t child;
	bool owner_ok, survivor_ok = false;
	ssize_t n = 0;

	ksft_print_header();
	fflush(stdout);
	if (argc != 2 || strcmp(argv[1], "--external-reclaim"))
		ksft_exit_skip("Needs explicit --external-reclaim and a bounded own-cgroup controller\n");
	if (getauxval(AT_PAGESZ) != LEAF)
		ksft_exit_skip("Run through the established 4K ABI launcher\n");
	if (!visible_swap_metadata())
		ksft_exit_skip("CAP_SYS_ADMIN required to distinguish real swap entries\n");
	signal(SIGPIPE, SIG_IGN);
	signal(SIGALRM, stop_fixture);
	signal(SIGTERM, stop_fixture);
	alarm(35);
	if (prctl(PR_SET_CHILD_SUBREAPER, 1) || pipe(go) || pipe(results))
		ksft_exit_fail_msg("setup: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (!child) {
		if (setpgid(0, 0))
			_exit(2);
		close(go[1]);
		close(results[0]);
		owner(go[0], results[1]);
	}
	fixture_group = child;
	if (setpgid(child, child) && errno != EACCES && errno != ESRCH)
		stop_fixture(SIGTERM);
	close(go[0]);
	close(results[1]);
	owner_ok = wait_ok(child);
	if (owner_ok && write(go[1], "g", 1) == 1) {
		n = read(results[0], &result, sizeof(result));
		survivor_ok = wait_ok(-1);
	}
	close(go[1]);
	close(results[0]);
	if (!owner_ok || !survivor_ok) {
		kill(-child, SIGKILL);
		while (waitpid(-1, NULL, 0) > 0 || errno == EINTR)
			;
	}
	fixture_group = 0;
	alarm(0);
	ksft_set_plan(3);
	ksft_test_result(owner_ok && n == sizeof(result) && result.concurrent,
			 "four-worker partial-hole refill zeroing and neighbors\n");
	ksft_test_result(owner_ok && n == sizeof(result) && result.lifetime,
			 "original pool owner exits; descendant fork isolation/readwrite\n");
	ksft_print_msg("post-controller snapshots=%u real swapped=%zu/%lu excluded markers=%zu\n",
		       result.attempts, result.swapped, LEAVES, result.non_swap_markers);
	ksft_test_result(survivor_ok && result.reclaimed,
			 "own-cgroup reclaim swaps at least75percent, full survivor readback\n");
	ksft_finished();
}
