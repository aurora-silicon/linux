// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#define LEAF 4096UL
#define WINDOW (2UL * 1024 * 1024)
#define WINDOWS 1024UL
#define LENGTH (WINDOWS * WINDOW)

static void fail(const char *why)
{
	perror(why);
	exit(1);
}

static unsigned char tag(size_t i)
{
	return (unsigned char)(1 + i % 251);
}

static void checkpoint(const char *phase)
{
	FILE *f = fopen("/proc/self/status", "re");
	char line[256], ack;
	unsigned long pte = 0;
	if (!f)
		fail("status");
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "VmPTE: %lu kB", &pte) == 1)
			break;
	fclose(f);
	printf("{\"phase\":\"%s\",\"pid\":%d,\"VmPTE_bytes\":%lu}\n",
	       phase, getpid(), pte * 1024);
	if (read(STDIN_FILENO, &ack, 1) != 1 || ack != 'c')
		fail("controller ack");
}

static void touch_serial(unsigned char *base, int missing_only)
{
	for (size_t i = 0; i < WINDOWS; i++) {
		if (missing_only && !(i % 4))
			continue;
		if (missing_only) {
			void *p = mmap(base + i * WINDOW, WINDOW,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
				-1, 0);
			if (p != base + i * WINDOW)
				fail("refill mmap");
		}
		memset(base + i * WINDOW, tag(i), LEAF);
	}
}

#define WORKERS 32
static pthread_barrier_t step;
static atomic_int start_workers;
struct worker { unsigned char *base; unsigned id; };
static void *fault_worker(void *arg)
{
 struct worker *w = arg;
 int start;
 do { start = atomic_load_explicit(&start_workers, memory_order_acquire);
      if (!start) sched_yield(); } while (!start);
 if (start != 1) return NULL;
 for (unsigned round = 0; round < WINDOWS / WORKERS; round++) {
  int ret = pthread_barrier_wait(&step);
  if (ret && ret != PTHREAD_BARRIER_SERIAL_THREAD) abort();
  size_t i = round * WORKERS + w->id;
  memset(w->base + i * WINDOW, tag(i), LEAF);
 }
 return NULL;
}
static void touch(unsigned char *base, int missing_only)
{
 if (missing_only) { touch_serial(base, missing_only); return; }
 pthread_t threads[WORKERS];
 struct worker args[WORKERS];
 pthread_attr_t attr;
 if (pthread_attr_init(&attr) || pthread_attr_setstacksize(&attr, 128 * 1024) ||
     pthread_barrier_init(&step, NULL, WORKERS)) fail("thread setup");
 unsigned made = 0;
 for (; made < WORKERS; made++) {
  args[made] = (struct worker){.base = base, .id = made};
  int err = pthread_create(&threads[made], &attr, fault_worker, &args[made]);
  if (err) { errno = err; break; }
 }
 pthread_attr_destroy(&attr);
 atomic_store_explicit(&start_workers, made == WORKERS ? 1 : 2, memory_order_release);
 for (unsigned i = 0; i < made; i++) if (pthread_join(threads[i], NULL)) abort();
 pthread_barrier_destroy(&step);
 if (made != WORKERS) fail("pthread_create");
}

static void verify(unsigned char *base, int sparse)
{
	for (size_t i = 0; i < WINDOWS; i++) {
		if (sparse && i % 4)
			continue;
		for (size_t j = 0; j < LEAF; j++)
			if (base[i * WINDOW + j] != tag(i)) {
				fprintf(stderr, "FAIL content window=%zu byte=%zu\n", i, j);
				exit(1);
			}
	}
}

static void thin(unsigned char *base)
{
	for (size_t i = 0; i < WINDOWS; i++)
		if (i % 4 && munmap(base + i * WINDOW, WINDOW))
			fail("thin munmap");
	verify(base, 1);
}

int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "--native-16k") ||
	    getauxval(AT_PAGESZ) != LEAF) {
		fputs("Requires established 4K launcher on native16K kernel\n", stderr);
		return 77;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(60);
	checkpoint("baseline");
	unsigned char *raw = mmap(NULL, LENGTH + WINDOW,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		fail("reserve mmap");
	unsigned char *base = (void *)(((uintptr_t)raw + WINDOW - 1) & ~(WINDOW - 1));
	size_t head = base - raw, tail = WINDOW - head;
	if (head && munmap(raw, head))
		fail("trim head");
	if (tail && munmap(base + LENGTH, tail))
		fail("trim tail");
	touch(base, 0);
	verify(base, 0);
	checkpoint("full");
	thin(base);
	checkpoint("quarter_survivors");
	touch(base, 1);
	verify(base, 0);
	checkpoint("refilled");
	thin(base);
	checkpoint("quarter_survivors_again");
	if (munmap(base, LENGTH))
		fail("final munmap");
	checkpoint("unmapped");
	puts("PASS contents, unmap and refill; concurrent firstfaults, accounting is a separate observation");
	return 0;
}
