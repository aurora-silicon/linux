// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#define LEAF 4096UL
#define NATIVE 16384UL
#define COUNT 256UL
#define STRIDE (2 * LEAF)
#define LENGTH (COUNT * STRIDE)
#define TAIL_SPAN (8UL * 1024 * 1024)
static unsigned char *maps[16];
static unsigned long leaves = COUNT, stride = STRIDE;
static void fail(const char *s) { perror(s); exit(1); }
static unsigned bucket(uintptr_t p)
{ return ((uint64_t)(p >> 14) * UINT64_C(0x61c8864680b583eb)) >> 60; }
static unsigned char value(unsigned phase, size_t leaf, size_t byte)
{ return (unsigned char)(1 + (phase * 53 + leaf * 7 + byte) % 251); }
static void check(unsigned count)
{
 for (unsigned phase = 0; phase < count; phase++)
  for (size_t leaf = 0; leaf < leaves; leaf++)
   for (size_t byte = 0; byte < LEAF; byte++)
    if (maps[phase][leaf * stride + byte] != value(phase, leaf, byte)) {
     fprintf(stderr, "FAIL bytes phase=%u leaf=%zu byte=%zu\n", phase, leaf, byte); exit(1);
    }
}
static void ack(void)
{
 char c; ssize_t n;
 do { n = read(0, &c, 1); } while (n < 0 && errno == EINTR);
 if (n != 1 || c != 'c') fail("controller ack");
}
#define WORKERS 4
#define RACE_LEAVES 64UL
static unsigned char *race_maps[WORKERS];
static atomic_int race_start;
static uint64_t monotonic_ns(void)
{
 struct timespec t;
 if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock_gettime");
 return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void race_check(void)
{
 for (unsigned w = 0; w < WORKERS; w++)
  for (size_t leaf = 0; leaf < RACE_LEAVES; leaf++)
   for (size_t byte = 0; byte < LEAF; byte++)
    if (race_maps[w][leaf * STRIDE + byte] != value(w + 32, leaf, byte)) fail("race bytes");
}
static void *race_worker(void *arg)
{
 unsigned w = (uintptr_t)arg;
 int start;
 do { start = atomic_load_explicit(&race_start, memory_order_acquire); if (!start) sched_yield(); } while (!start);
 if (start != 1) return NULL;
 for (size_t leaf = 0; leaf < RACE_LEAVES; leaf++) {
  unsigned char *p = race_maps[w] + leaf * STRIDE;
  if (mprotect(p, LEAF, PROT_READ | PROT_WRITE)) fail("race mprotect");
  for (size_t byte = 0; byte < LEAF; byte++) p[byte] = value(w + 32, leaf, byte);
  /* Test-only pacing keeps allocation active during bounded controller moves. */
  struct timespec delay = {.tv_nsec = 2000000};
  while (nanosleep(&delay, &delay) && errno == EINTR) {}
 }
 return NULL;
}
static void race_run(void)
{
 pthread_t threads[WORKERS];
 pthread_attr_t attr;
 if (pthread_attr_init(&attr) || pthread_attr_setstacksize(&attr, 128 * 1024)) fail("thread attributes");
 unsigned made = 0;
 for (; made < WORKERS; made++) {
  race_maps[made] = mmap(NULL, RACE_LEAVES * STRIDE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (race_maps[made] == MAP_FAILED) fail("race mmap");
  int err = pthread_create(&threads[made], &attr, race_worker, (void *)(uintptr_t)made);
  if (err) { errno = err; break; }
 }
 pthread_attr_destroy(&attr);
 if (made != WORKERS) {
  atomic_store_explicit(&race_start, 2, memory_order_release);
  for (unsigned i = 0; i < made; i++) pthread_join(threads[i], NULL);
  fail("pthread_create");
 }
 printf("RACE pid=%d base0=%lx base1=%lx base2=%lx base3=%lx leaves=%lu stride=%lu\n", getpid(), (unsigned long)race_maps[0], (unsigned long)race_maps[1], (unsigned long)race_maps[2], (unsigned long)race_maps[3], RACE_LEAVES, STRIDE);
 ack();
 uint64_t begin = monotonic_ns();
 atomic_store_explicit(&race_start, 1, memory_order_release);
 for (unsigned i = 0; i < WORKERS; i++) if (pthread_join(threads[i], NULL)) abort();
 uint64_t end = monotonic_ns();
 race_check();
 printf("RACE_DONE pid=%d begin_ns=%llu end_ns=%llu\n", getpid(), (unsigned long long)begin, (unsigned long long)end);
 ack();
}
int main(int argc, char **argv)
{
 if (argc == 2 && !strcmp(argv[1], "--selftest")) {
  unsigned count = 0;
  for (uintptr_t p = 0x100000; p < 0x100000 + TAIL_SPAN; p += NATIVE) count += bucket(p) == 0;
  if (count < 16 || value(0, 0, 0) == value(1, 0, 0)) return 1;
  puts("PASS patterns/hash selection only; no domain allocation coverage"); return 0;
 }
 int tail = argc == 3 && !strcmp(argv[2], "--tail");
 int race = argc == 3 && !strcmp(argv[2], "--race");
 if ((argc != 2 && !tail && !race) || strcmp(argv[1], "--native-16k") || getauxval(AT_PAGESZ) != LEAF) return 77;
 unsigned phases = tail ? 16 : race ? 1 : 3;
 unsigned char *reservation = NULL;
 alarm(15); setvbuf(stdout, NULL, _IONBF, 0);
 printf("READY pid=%d\n", getpid()); ack();
 if (race) race_run();
 if (tail) {
  leaves = 1; stride = LEAF;
  reservation = mmap(NULL, TAIL_SPAN + NATIVE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED) fail("tail reserve");
  uintptr_t begin = ((uintptr_t)reservation + NATIVE - 1) & ~(NATIVE - 1);
  unsigned found = 0;
  for (uintptr_t p = begin; p < begin + TAIL_SPAN && found < phases; p += NATIVE)
   if (!bucket(p)) maps[found++] = (void *)p;
  if (found != phases) fail("same-shard tail addresses");
 }
 for (unsigned phase = 0; phase < phases; phase++) {
  check(phase);
  if (!tail) {
   maps[phase] = mmap(NULL, LENGTH, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (maps[phase] == MAP_FAILED) fail("mmap");
  }
  for (size_t leaf = 0; leaf < leaves; leaf++) {
   unsigned char *p = maps[phase] + leaf * stride;
   if (mprotect(p, LEAF, PROT_READ | PROT_WRITE)) fail("mprotect");
   for (size_t byte = 0; byte < LEAF; byte++) p[byte] = value(phase, leaf, byte);
  }
  check(phase + 1);
  printf("PHASE index=%u pid=%d base=%lx leaves=%lu stride=%lu\n", phase, getpid(), (unsigned long)maps[phase], leaves, stride);
  ack();
 }
 check(phases);
 if (race) {
  race_check();
  for (unsigned w = 0; w < WORKERS; w++) if (munmap(race_maps[w], RACE_LEAVES * STRIDE)) fail("race munmap");
 }
 if (tail) {
  if (munmap(reservation, TAIL_SPAN + NATIVE)) fail("tail munmap");
 } else for (unsigned phase = 0; phase < phases; phase++) if (munmap(maps[phase], LENGTH)) fail("munmap");
 puts("PASS domain allocation phases and all prior bytes; charge/density separately verified");
 return 0;
}
