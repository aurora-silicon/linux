// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Bounded application memcpy throughput; this is not an AMCC bus counter. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_THREADS 64

static pthread_barrier_t ready;
static uint64_t start_ns, duration_ns;
static size_t bytes;
/* A volatile function pointer keeps each libc copy observable to the compiler. */
static void *(*volatile copy_memory)(void *, const void *, size_t) = memcpy;

struct worker {
	void *src, *dst;
	uint64_t copies, end_ns;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
		perror("clock_gettime");
		exit(1);
	}
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *copy_worker(void *arg)
{
	struct worker *worker = arg;

	memset(worker->src, 0x5a, bytes);
	memset(worker->dst, 0, bytes);
	pthread_barrier_wait(&ready);
	pthread_barrier_wait(&ready);
	do {
		copy_memory(worker->dst, worker->src, bytes);
		worker->copies++;
		worker->end_ns = now_ns();
	} while (worker->end_ns - start_ns < duration_ns);
	return NULL;
}

static unsigned long number(const char *arg, unsigned long max)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(arg, &end, 10);
	if (errno || !*arg || *end || !value || value > max) {
		fprintf(stderr, "Invalid argument: %s (expected 1..%lu)\n", arg, max);
		exit(2);
	}
	return value;
}

int main(int argc, char **argv)
{
	struct worker workers[MAX_THREADS] = {};
	pthread_t threads[MAX_THREADS];
	unsigned long count = 8, mib = 256, seconds = 10;
	uint64_t copies = 0, end_ns = 0;
	unsigned long i;
	int ret;

	if (argc > 4) {
		fprintf(stderr, "Usage: %s [threads [MiB-per-buffer [seconds]]]\n", argv[0]);
		return 2;
	}
	if (argc > 1)
		count = number(argv[1], MAX_THREADS);
	if (argc > 2)
		mib = number(argv[2], 1024);
	if (argc > 3)
		seconds = number(argv[3], 60);
	bytes = mib * 1024 * 1024;
	duration_ns = seconds * 1000000000ULL;
	for (i = 0; i < count; i++) {
		ret = posix_memalign(&workers[i].src, 16384, bytes);
		if (!ret)
			ret = posix_memalign(&workers[i].dst, 16384, bytes);
		if (ret) {
			fprintf(stderr, "Allocation: %s\n", strerror(ret));
			return 1;
		}
	}
	ret = pthread_barrier_init(&ready, NULL, count + 1);
	if (ret) {
		fprintf(stderr, "Barrier: %s\n", strerror(ret));
		return 1;
	}
	for (i = 0; i < count; i++) {
		ret = pthread_create(&threads[i], NULL, copy_worker, &workers[i]);
		if (ret) {
			fprintf(stderr, "Thread: %s\n", strerror(ret));
			/* Terminate any workers already waiting at the barrier. */
			exit(1);
		}
	}
	pthread_barrier_wait(&ready);
	start_ns = now_ns();
	pthread_barrier_wait(&ready);
	for (i = 0; i < count; i++) {
		pthread_join(threads[i], NULL);
		copies += workers[i].copies;
		if (workers[i].end_ns > end_ns)
			end_ns = workers[i].end_ns;
		if (memcmp(workers[i].src, workers[i].dst, bytes)) {
			fprintf(stderr, "Copy verification failed\n");
			return 1;
		}
		free(workers[i].src);
		free(workers[i].dst);
	}
	pthread_barrier_destroy(&ready);
	printf("threads=%lu bytes_per_buffer=%zu elapsed_s=%.6f copies=%llu "
	       "application_rd_wr_GBps=%.3f\n", count, bytes,
	       (end_ns - start_ns) / 1e9, (unsigned long long)copies,
	       2.0 * bytes * copies / (end_ns - start_ns));
	return 0;
}
