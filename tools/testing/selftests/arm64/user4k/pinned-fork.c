// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define LEAF 4096UL
#define NATIVE 16384UL
#define SIZE (1024UL * 1024)
#define COUNT (SIZE / LEAF)
#define PFNMASK ((1ULL << 55) - 1)
static pid_t child = -1;
static uint64_t parent_pfn[COUNT], child_pfn[COUNT];
static void die(const char *s) { perror(s); exit(1); }
static void cleanup(void)
{
	if (child > 0) {
		kill(child, SIGKILL);
		while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
	}
}
static void timeout(int sig)
{
	(void)sig;
	if (child > 0) kill(child, SIGKILL);
	_exit(124);
}
static void transfer(int fd, void *buffer, size_t len, int writing)
{
	unsigned char *p = buffer;
	while (len) {
		ssize_t n = writing ? write(fd, p, len) : read(fd, p, len);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) die(writing ? "write protocol" : "read protocol");
		p += n; len -= n;
	}
}
static unsigned char pattern(size_t i) { return (unsigned char)(1 + (i * 17 + i / LEAF) % 251); }
static void check(unsigned char *p, int mode)
{
	for (size_t i = 0; i < SIZE; i++) {
		unsigned char expected = pattern(i);
		if ((mode == 1 && i % LEAF == 0) || (mode == 2 && i % LEAF == 1)) expected ^= 0x80;
		if (p[i] != expected) {
			fprintf(stderr, "FAIL bytes mode=%d offset=%zu expected=%u actual=%u\n", mode, i, expected, p[i]);
			exit(1);
		}
	}
}
static void pfns(unsigned char *p, uint64_t *out)
{
	int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0) die("pagemap open");
	for (size_t i = 0; i < COUNT; i++) {
		uint64_t e;
		if (pread(fd, &e, sizeof(e), (((uintptr_t)p / LEAF) + i) * sizeof(e)) != sizeof(e)) die("pagemap read");
		if (!(e >> 63) || (e & (1ULL << 62)) || !(e & PFNMASK)) {
			fprintf(stderr, "FAIL pagemap leaf=%zu entry=%016llx (root PFNs required)\n", i, (unsigned long long)e); exit(1);
		}
		out[i] = e & PFNMASK;
	}
	close(fd);
}
static size_t native_count(uint64_t *pf)
{
	size_t n = 0;
	for (size_t i = 0; i < COUNT; i++) {
		size_t j;
		for (j = 0; j < i; j++) if (pf[j] / 4 == pf[i] / 4) break;
		if (j == i) n++;
	}
	return n;
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--selftest")) {
		for (size_t i = 0; i < COUNT; i++) parent_pfn[i] = 4 + i;
		if (native_count(parent_pfn) != COUNT / 4) return 1;
		for (size_t i = 0; i < COUNT; i++) parent_pfn[i] = 4 + 4 * i;
		if (native_count(parent_pfn) != COUNT) return 1;
		puts("PASS native PFN grouping selftest (no pin/fork coverage)"); return 0;
	}
	int packed = 0, hold = 0;
	if (argc < 2 || strcmp(argv[1], "--native-16k") || getauxval(AT_PAGESZ) != LEAF) return 77;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--expect-packed")) packed = 1;
		else if (!strcmp(argv[i], "--hold")) hold = 1;
		else return 77;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, timeout); signal(SIGPIPE, SIG_IGN); alarm(15); atexit(cleanup);
	unsigned char *raw = mmap(NULL, SIZE + NATIVE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED) die("mmap");
	unsigned char *p = (void *)(((uintptr_t)raw + NATIVE - 1) & ~(NATIVE - 1));
	for (size_t i = 0; i < SIZE; i++) p[i] = pattern(i);
	check(p, 0); pfns(p, parent_pfn);
	if (native_count(parent_pfn) * NATIVE != SIZE) die("initial dense native backing");
	struct io_uring_params params = {0};
	int ring = syscall(__NR_io_uring_setup, 2, &params);
	if (ring < 0) { perror("UNEXERCISED io_uring_setup"); return 77; }
	struct iovec iov = { .iov_base = p, .iov_len = SIZE };
	if (syscall(__NR_io_uring_register, ring, IORING_REGISTER_BUFFERS, &iov, 1)) {
		perror("UNEXERCISED register real long-term pins"); close(ring); return 77;
	}
	/* Long-term pin setup can migrate backing; capture the pinned identity. */
	pfns(p, parent_pfn); check(p, 0);
	if (native_count(parent_pfn) * NATIVE != SIZE) die("pinned dense native backing");
	int to_child[2], from_child[2];
	if (pipe(to_child) || pipe(from_child)) die("pipe");
	pid_t parent = getpid();
	child = fork();
	if (child < 0) die("fork");
	if (!child) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() != parent) _exit(1);
		alarm(10); close(ring); close(to_child[1]); close(from_child[0]);
		pfns(p, child_pfn); check(p, 0);
		transfer(from_child[1], child_pfn, sizeof(child_pfn), 1);
		char token; transfer(to_child[0], &token, 1, 0);
		if (token != 'w') die("child token");
		check(p, 0);
		for (size_t i = 0; i < COUNT; i++) p[i * LEAF + 1] ^= 0x80;
		check(p, 2);
		token = 'c'; transfer(from_child[1], &token, 1, 1);
		transfer(to_child[0], &token, 1, 0);
		if (token != 'e') die("child exit token");
		check(p, 2); _exit(0);
	}
	close(to_child[0]); close(from_child[1]);
	transfer(from_child[0], child_pfn, sizeof(child_pfn), 0);
	for (size_t i = 0; i < COUNT; i++)
		for (size_t j = 0; j < COUNT; j++)
			if (child_pfn[i] / 4 == parent_pfn[j] / 4) die("child copy overlaps pinned parent backing");
	printf("METADATA real_registered_pin=1 parent_native=%zu child_native=%zu copied_leaves=%lu native_overlap=0\n", native_count(parent_pfn) * NATIVE, native_count(child_pfn) * NATIVE, COUNT);
	if (hold) {
		printf("HOLD parent=%d child=%d base=%lx bytes=%lu (write c to continue)\n", getpid(), child, (unsigned long)p, SIZE);
		char ack; transfer(STDIN_FILENO, &ack, 1, 0);
		if (ack != 'c') die("controller hold token");
	}
	size_t child_bytes = native_count(child_pfn) * NATIVE;
	if (child_bytes < SIZE || child_bytes > COUNT * NATIVE ||
	    (packed && child_bytes > SIZE + 256 * 1024)) die("child density bound");
	for (size_t i = 0; i < COUNT; i++) p[i * LEAF] ^= 0x80;
	check(p, 1);
	char token = 'w'; transfer(to_child[1], &token, 1, 1);
	transfer(from_child[0], &token, 1, 0);
	if (token != 'c') die("parent token");
	check(p, 1);
	token = 'e'; transfer(to_child[1], &token, 1, 1);
	int status;
	if (waitpid(child, &status, 0) != child) die("waitpid");
	child = -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) die("child failed");
	if (syscall(__NR_io_uring_register, ring, IORING_UNREGISTER_BUFFERS, NULL, 0)) die("unregister");
	close(ring); close(to_child[1]); close(from_child[0]);
	if (munmap(raw, SIZE + NATIVE)) die("munmap");
	puts("PASS real pin/fork copy and full-byte two-way isolation; density is separately reported");
	return 0;
}
