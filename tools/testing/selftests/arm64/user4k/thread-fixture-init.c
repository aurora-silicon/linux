// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
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
#include "../../kselftest.h"

#define THREADS 4
#define ROUNDS 128
#define PAGES 32

static pthread_barrier_t barrier;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t robust;
static _Thread_local unsigned int tls_id;
static _Thread_local volatile sig_atomic_t signals;
static atomic_uint failures;
static unsigned long total;
static unsigned long ps;
static atomic_int waiter_tid;

static void handler(int sig)
{
	if (sig == SIGUSR1 && tls_id)
		signals++;
	else
		atomic_fetch_add(&failures, 1);
}

static bool bytes_equal(unsigned char *p, size_t n, unsigned char value)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != value)
			return false;
	return true;
}

static void *worker(void *arg)
{
	unsigned int id = (uintptr_t)arg;
	unsigned char *p, *heap;
	unsigned long request = id & 1 ? 4096 : 0;
	bool ok = true;

	tls_id = id;
	p = mmap(NULL, PAGES * ps, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (p == MAP_FAILED)
		ok = false;
	ok &= !prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, request, 0UL, 0UL, 0UL);
	pthread_barrier_wait(&barrier);
	for (unsigned int round = 0; round < ROUNDS; round++) {
		unsigned char pattern = 1 + (id * 31 + round) % 254;
		unsigned char residency[PAGES];
		cpu_set_t cpu;

		if (ok) {
			CPU_ZERO(&cpu);
			CPU_SET((id + round) % 2, &cpu);
			ok &= !sched_setaffinity(0, sizeof(cpu), &cpu);
			memset(p, pattern, PAGES * ps);
			ok &= !mprotect(p + ps, ps, PROT_READ) &&
			      p[ps] == pattern;
			ok &= !mprotect(p + ps, ps, PROT_READ | PROT_WRITE);
			ok &= !madvise(p + 2 * ps, ps, MADV_FREE);
			memset(p + 2 * ps, pattern, ps);
			ok &= !madvise(p + 3 * ps, ps, MADV_DONTNEED);
			ok &= bytes_equal(p + 3 * ps, ps, 0);
			memset(p + 3 * ps, pattern, ps);
			heap = malloc(5000 + id * 777);
			if (!heap) {
				ok = false;
			} else {
				memset(heap, pattern, 5000 + id * 777);
				ok &= bytes_equal(heap, 5000 + id * 777,
						  pattern);
				free(heap);
			}
			ok &= !pthread_kill(pthread_self(), SIGUSR1);
			ok &= tls_id == id &&
			      signals == (sig_atomic_t)(round + 1);
			ok &= prctl(PR_AURORA_GET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL,
				    0UL) == (int)request;
			ok &= !mincore(p, PAGES * ps, residency);
			ok &= bytes_equal(p, PAGES * ps, pattern);
		}
		pthread_mutex_lock(&mutex);
		total++;
		pthread_cond_signal(&condition);
		pthread_mutex_unlock(&mutex);
		pthread_barrier_wait(&barrier);
	}
	if (p != MAP_FAILED)
		munmap(p, PAGES * ps);
	if (!ok)
		atomic_fetch_add(&failures, 1);
	return NULL;
}

static void *owner_dies(void *arg)
{
	(void)arg;
	if (pthread_mutex_lock(&robust))
		atomic_fetch_add(&failures, 1);
	return NULL;
}

static void *pi_waiter(void *arg)
{
	int error;

	(void)arg;
	atomic_store(&waiter_tid, syscall(SYS_gettid));
	error = pthread_mutex_lock(&robust);
	if (!error)
		error = pthread_mutex_unlock(&robust);
	return (void *)(uintptr_t)error;
}

static bool wait_for_pi_block(void)
{
	for (unsigned int tries = 0; tries < 1000; tries++) {
		int tid = atomic_load(&waiter_tid);
		char path[80], line[256];
		unsigned long nr, address, operation;
		FILE *f;

		if (tid) {
			snprintf(path, sizeof(path),
				 "/proc/self/task/%d/syscall", tid);
			f = fopen(path, "re");
			if (f) {
				bool blocked = fgets(line, sizeof(line), f) &&
					       sscanf(line, "%lu %lx %lx", &nr,
						      &address,
						      &operation) == 3 &&
					       nr == SYS_futex &&
					       ((operation & FUTEX_CMD_MASK) ==
							FUTEX_LOCK_PI ||
						(operation & FUTEX_CMD_MASK) ==
							FUTEX_LOCK_PI2);

				fclose(f);
				if (blocked)
					return true;
			}
		}
		usleep(1000);
	}
	return false;
}

static int exercise(void)
{
	pthread_t threads[THREADS], owner;
	pthread_mutexattr_t attributes;
	struct sigaction action = { .sa_handler = handler };
	struct stat self, init;
	bool ok = true, forks = true;
	int error;

	if (stat("/proc/self/exe", &self) || stat("/proc/1/exe", &init) ||
	    self.st_dev != init.st_dev || self.st_ino != init.st_ino)
		return 2;
	ps = getauxval(AT_PAGESZ);
	ksft_print_header();
	ksft_set_plan(6);
	if (sigaction(SIGUSR1, &action, NULL) ||
	    pthread_barrier_init(&barrier, NULL, THREADS + 1))
		ksft_exit_fail_msg("thread setup\n");
	for (unsigned int i = 0; i < THREADS; i++)
		if (pthread_create(&threads[i], NULL, worker,
				   (void *)(uintptr_t)(i + 1)))
			ksft_exit_fail_msg("pthread_create\n");
	pthread_barrier_wait(&barrier);
	for (unsigned int round = 0; round < ROUNDS; round++) {
		pthread_mutex_lock(&mutex);
		while (total < (round + 1) * THREADS)
			pthread_cond_wait(&condition, &mutex);
		pthread_mutex_unlock(&mutex);
		ok &= prctl(PR_AURORA_GET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL) ==
		      0;
		if (!(round % 16)) {
			pid_t child = fork();
			int status = 0;

			if (!child) {
				unsigned char *p = malloc(32768);

				if (!p || tls_id ||
				    total != (round + 1) * THREADS ||
				    prctl(PR_AURORA_GET_EXEC_PAGE_SIZE, 0UL,
					  0UL, 0UL, 0UL))
					_exit(10);
				memset(p, 0x75, 32768);
				if (!bytes_equal(p, 32768, 0x75))
					_exit(11);
				free(p);
				_exit(0);
			}
			forks &= child > 0 &&
				 waitpid(child, &status, 0) == child &&
				 WIFEXITED(status) && !WEXITSTATUS(status);
		}
		pthread_barrier_wait(&barrier);
	}
	for (unsigned int i = 0; i < THREADS; i++)
		ok &= !pthread_join(threads[i], NULL);
	ksft_test_result(
		ok && !atomic_load(&failures) && total == THREADS * ROUNDS,
		"%luK %u concurrent memory/TLS/signal/exec-request rounds across %u threads\n",
		ps / 1024, ROUNDS, THREADS);
	ksft_test_result(
		forks, "%luK fork and allocator from a multithreaded process\n",
		ps / 1024);
	pthread_barrier_destroy(&barrier);
	ok = !pthread_mutexattr_init(&attributes);
	ok &= !pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
	ok &= !pthread_mutex_init(&robust, &attributes);
	ok &= !pthread_create(&owner, NULL, owner_dies, NULL);
	if (!ok)
		ksft_exit_fail_msg("robust mutex setup\n");
	ok &= !pthread_join(owner, NULL);
	error = pthread_mutex_lock(&robust);
	ok &= error == EOWNERDEAD;
	if (error == EOWNERDEAD)
		ok &= !pthread_mutex_consistent(&robust);
	if (!error || error == EOWNERDEAD)
		ok &= !pthread_mutex_unlock(&robust);
	ksft_test_result(ok, "%luK robust futex owner-death recovery\n",
			 ps / 1024);
	pthread_mutex_destroy(&robust);
	/* Exercise repeated TLS/stack allocation and set_tid_address clear/wake. */
	for (unsigned int i = 0; i < 128; i++) {
		ok = !pthread_mutex_init(&robust, &attributes);
		ok &= !pthread_create(&owner, NULL, owner_dies, NULL);
		if (!ok)
			break;
		ok &= !pthread_join(owner, NULL);
		error = pthread_mutex_lock(&robust);
		ok &= error == EOWNERDEAD;
		if (error == EOWNERDEAD)
			ok &= !pthread_mutex_consistent(&robust);
		if (!error || error == EOWNERDEAD)
			pthread_mutex_unlock(&robust);
		pthread_mutex_destroy(&robust);
		if (!ok)
			break;
	}
	ksft_test_result(ok && !atomic_load(&failures),
			 "%luK repeated thread exit/join and robust recovery\n",
			 ps / 1024);
	ok = !pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT);
	ok &= !pthread_mutex_init(&robust, &attributes);
	ok &= !pthread_mutex_lock(&robust);
	ok &= !pthread_create(&owner, NULL, pi_waiter, NULL);
	if (!ok)
		ksft_exit_fail_msg("PI mutex setup\n");
	ok &= wait_for_pi_block();
	ok &= !pthread_mutex_unlock(&robust);
	{
		void *result = (void *)1;

		ok &= !pthread_join(owner, &result) && !result;
	}
	ksft_test_result(
		ok,
		"%luK confirmed contended priority-inheritance futex handoff\n",
		ps / 1024);
	ok = !pthread_create(&owner, NULL, owner_dies, NULL);
	if (!ok)
		ksft_exit_fail_msg("PI owner setup\n");
	ok &= !pthread_join(owner, NULL);
	error = pthread_mutex_lock(&robust);
	ok &= error == EOWNERDEAD;
	if (error == EOWNERDEAD)
		ok &= !pthread_mutex_consistent(&robust);
	if (!error || error == EOWNERDEAD)
		ok &= !pthread_mutex_unlock(&robust);
	pthread_mutex_destroy(&robust);
	pthread_mutexattr_destroy(&attributes);
	ksft_test_result(
		ok, "%luK priority-inheritance robust owner-death recovery\n",
		ps / 1024);
	ksft_print_cnts();
	return ksft_get_fail_cnt() ? 1 : 0;
}

static pthread_barrier_t exec_barrier;

static void *exec_sibling(void *arg)
{
	(void)arg;
	pthread_barrier_wait(&exec_barrier);
	for (;;)
		pause();
	return NULL;
}

static void *exec_nonleader(void *arg)
{
	unsigned long target = (uintptr_t)arg;
	char size[32];

	snprintf(size, sizeof(size), "%lu", target);
	if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, target, 0UL, 0UL, 0UL))
		_exit(30);
	pthread_barrier_wait(&exec_barrier);
	execl("/init", "/init", "--after-thread-exec", size, NULL);
	_exit(31);
}

static int thread_exec(unsigned long target)
{
	pthread_t siblings[2], executor;

	if (pthread_barrier_init(&exec_barrier, NULL, 4))
		return 32;
	for (unsigned int i = 0; i < 2; i++)
		if (pthread_create(&siblings[i], NULL, exec_sibling, NULL))
			return 33;
	if (pthread_create(&executor, NULL, exec_nonleader,
			   (void *)(uintptr_t)target))
		return 34;
	pthread_barrier_wait(&exec_barrier);
	for (;;)
		pause();
	return 35;
}

int main(int argc, char **argv)
{
	bool ok = true;
	unsigned long granules[] = { getauxval(AT_PAGESZ), 4096, 16384 };
	unsigned int nr = granules[0] > 16384 ? 3 : 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 3 && !strcmp(argv[1], "--thread-exec") && getppid() == 1)
		return thread_exec(strtoul(argv[2], NULL, 10));
	if (argc == 3 && !strcmp(argv[1], "--after-thread-exec") &&
	    getppid() == 1) {
		unsigned long expected = strtoul(argv[2], NULL, 10);

		return getauxval(AT_PAGESZ) != expected ||
		       prctl(PR_AURORA_GET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL,
			     0UL) != 0;
	}
	if (argc == 3 && !strcmp(argv[1], "--test") && getppid() == 1) {
		if (getauxval(AT_PAGESZ) != strtoul(argv[2], NULL, 10))
			return 42;
		return exercise();
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Run only as PID 1 in a disposable VM\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL))
		return 3;
	for (unsigned int small = 0; small < nr; small++) {
		pid_t pid = fork();
		int status = 0;

		if (!pid) {
			char size[32];

			snprintf(size, sizeof(size), "%lu", granules[small]);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, granules[small],
				  0UL, 0UL, 0UL))
				_exit(3);
			execl("/init", "/init", "--test", size, NULL);
			_exit(4);
		}
		ok &= pid > 0 && waitpid(pid, &status, 0) == pid &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
	}
	for (unsigned int source = 0; source < nr; source++) {
		for (unsigned int target = 0; target < nr; target++) {
			pid_t pid = fork();
			int status = 0;
			bool passed;

			if (!pid) {
				char size[32];

				snprintf(size, sizeof(size), "%lu",
					 granules[target]);
				if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
					  granules[source], 0UL, 0UL, 0UL))
					_exit(36);
				execl("/init", "/init", "--thread-exec", size,
				      NULL);
				_exit(37);
			}
			passed = pid > 0 && waitpid(pid, &status, 0) == pid &&
				 WIFEXITED(status) && !WEXITSTATUS(status);
			printf("%s - non-leader exec %luK -> %luK status=%#x\n",
			       passed ? "ok" : "not ok",
			       granules[source] / 1024, granules[target] / 1024,
			       status);
			ok &= passed;
		}
	}
	printf("%s - thread fixture complete\n", ok ? "ok" : "not ok");
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
