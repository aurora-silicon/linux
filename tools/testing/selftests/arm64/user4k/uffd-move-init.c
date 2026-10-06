// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <linux/userfaultfd.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define CHECK(x)                                                        \
	do {                                                            \
		if (!(x)) {                                             \
			printf("not ok - MOVE line=%d errno=%d (%s)\n", \
			       __LINE__, errno, strerror(errno));       \
			exit(1);                                        \
		}                                                       \
	} while (0)
#define PRESENT (1ULL << 63)
#define SWAPPED (1ULL << 62)
#define VALUE_MASK ((1ULL << 55) - 1)
static uint64_t entry(void *p, size_t ps)
{
	uint64_t e;
	int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	CHECK(fd >= 0 && pread(fd, &e, 8, (uintptr_t)p / ps * 8) == 8);
	CHECK(!close(fd));
	return e;
}
static uint64_t physical(void *p, size_t ps)
{
	uint64_t e = entry(p, ps);
	CHECK(e & PRESENT);
	return (e & VALUE_MASK) * ps;
}
static unsigned char pattern(size_t i)
{
	return 1 + ((i ^ (i >> 12) ^ (i >> 16)) % 251);
}
static void bytes(unsigned char *p, size_t original, size_t len)
{
	for (size_t i = 0; i < len; i++)
		CHECK(p[i] == pattern(original + i));
}
static long move(int fd, void *dst, void *src, size_t len, unsigned mode,
		 int expected_errno)
{
	struct uffdio_move m = { .dst = (uintptr_t)dst,
				 .src = (uintptr_t)src,
				 .len = len,
				 .mode = mode };
	errno = 0;
	int r = ioctl(fd, UFFDIO_MOVE, &m);
	if (expected_errno)
		CHECK(r == -1 && errno == expected_errno);
	else
		CHECK(r == 0 && m.move == (long long)len);
	return m.move;
}
static void on_cpu(unsigned cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	CHECK(!sched_setaffinity(0, sizeof(set), &set));
}
struct tlb_reader {
	unsigned char *src, *dst;
	atomic_int ready, done;
};
static void *read_after_move(void *arg)
{
	struct tlb_reader *r = arg;
	on_cpu(1);
	unsigned char old = *(volatile unsigned char *)r->src;
	atomic_store(&r->ready, 1);
	while (!atomic_load(&r->done))
		sched_yield();
	CHECK(*(volatile unsigned char *)r->src == 0 &&
	      *(volatile unsigned char *)r->dst == old);
	return NULL;
}
struct pin_racer {
	void *a, *b;
	size_t ps;
	atomic_int stop;
	atomic_uint accepted, rejected;
};
static void *race_pins(void *arg)
{
	struct pin_racer *r = arg;
	struct io_uring_params p = { 0 };
	on_cpu(1);
	int fd = syscall(SYS_io_uring_setup, 2, &p);
	CHECK(fd >= 0);
	unsigned round = 0;
	while (!atomic_load(&r->stop)) {
		struct iovec iov = { .iov_base = (round++ & 1) ? r->a : r->b,
				     .iov_len = r->ps };
		int ret = syscall(SYS_io_uring_register, fd,
				  IORING_REGISTER_BUFFERS, &iov, 1);
		if (!ret) {
			atomic_fetch_add(&r->accepted, 1);
			usleep(50);
			CHECK(!syscall(SYS_io_uring_register, fd,
				       IORING_UNREGISTER_BUFFERS, NULL, 0));
		} else {
			CHECK(errno == EFAULT || errno == EAGAIN);
			atomic_fetch_add(&r->rejected, 1);
		}
	}
	CHECK(!close(fd));
	return NULL;
}
static void concurrent_move(size_t ps)
{
	unsigned char *a =
		mmap((void *)0x50000000, ps, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	unsigned char *b =
		mmap((void *)0x60000000, ps, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	CHECK(a != MAP_FAILED && b != MAP_FAILED);
	memset(a, 0x6d, ps);
	uint64_t phys = physical(a, ps);
	int fd = syscall(SYS_userfaultfd,
			 O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(fd >= 0);
	struct uffdio_api api = { .api = UFFD_API,
				  .features = UFFD_FEATURE_MOVE };
	CHECK(!ioctl(fd, UFFDIO_API, &api));
	struct uffdio_register reg = { .range = { (uintptr_t)a, ps },
				       .mode = UFFDIO_REGISTER_MODE_MISSING };
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &reg));
	reg.range.start = (uintptr_t)b;
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &reg));
	struct pin_racer race = { .a = a, .b = b, .ps = ps };
	pthread_t thread;
	CHECK(!pthread_create(&thread, NULL, race_pins, &race));
	while (!atomic_load(&race.accepted))
		sched_yield();
	unsigned char *cur = a, *next = b;
	unsigned moved = 0, busy = 0;
	for (unsigned tries = 0; moved < 256 && tries < 100000; tries++) {
		struct uffdio_move m = { .src = (uintptr_t)cur,
					 .dst = (uintptr_t)next,
					 .len = ps };
		int ret = ioctl(fd, UFFDIO_MOVE, &m);
		if (ret) {
			CHECK(errno == EBUSY && m.move == -EBUSY);
			busy++;
			sched_yield();
			continue;
		}
		CHECK(m.move == (long long)ps && physical(next, ps) == phys &&
		      !(entry(cur, ps) & (PRESENT | SWAPPED)));
		unsigned char *tmp = cur;
		cur = next;
		next = tmp;
		moved++;
	}
	atomic_store(&race.stop, 1);
	CHECK(!pthread_join(thread, NULL));
	CHECK(moved == 256 && atomic_load(&race.accepted) > 0);
	for (size_t i = 0; i < ps; i++)
		CHECK(cur[i] == 0x6d);
	printf("ok - MOVE %zuK CPU0/CPU1 race moves=%u busy=%u pins=%u holes=%u unchanged PFN/data\n",
	       ps / 1024, moved, busy, atomic_load(&race.accepted),
	       atomic_load(&race.rejected));
	CHECK(!close(fd) && !munmap(a, ps) && !munmap(b, ps));
}

static void allocation_failure(size_t ps, size_t native)
{
	if (ps == native)
		return;
	unsigned char *a =
		mmap((void *)0x70000000, 2 * native, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	unsigned char *b =
		mmap((void *)0x71000000, 2 * native, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	CHECK(a != MAP_FAILED && b != MAP_FAILED);
	memset(a, 0x35, 2 * native);
	int fd = syscall(SYS_userfaultfd,
			 O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(fd >= 0);
	struct uffdio_api api = { .api = UFFD_API,
				  .features = UFFD_FEATURE_MOVE };
	CHECK(!ioctl(fd, UFFDIO_API, &api));
	struct uffdio_register reg = { .range = { (uintptr_t)a, 2 * native },
				       .mode = UFFDIO_REGISTER_MODE_MISSING };
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &reg));
	reg.range.start = (uintptr_t)b;
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &reg));
	/* Prepare both roots and target tables using a different native folio. */
	move(fd, b + native, a + native, ps, 0, 0);
	uint64_t phys = physical(a, ps);
	int inject = open("/proc/self/fail-nth", O_RDWR | O_CLOEXEC);
	CHECK(inject >= 0);
	unsigned char *src = a, *dst = b;
	for (unsigned phase = 0; phase < 2; phase++) {
		struct uffdio_move m = { .src = (uintptr_t)src,
					 .dst = (uintptr_t)dst,
					 .len = ps };
		char pending[16] = { 0 };
		CHECK(write(inject, "1", 1) == 1);
		errno = 0;
		int ret = ioctl(fd, UFFDIO_MOVE, &m), saved = errno;
		CHECK(pread(inject, pending, sizeof(pending) - 1, 0) > 0);
		CHECK(write(inject, "0", 1) == 1);
		CHECK(ret == -1 && saved == ENOMEM && m.move == -ENOMEM &&
		      strtoul(pending, NULL, 10) == 0);
		CHECK(physical(src, ps) == phys &&
		      !(entry(dst, ps) & (PRESENT | SWAPPED)));
		for (size_t i = 0; i < ps; i++)
			CHECK(src[i] == 0x35);
		move(fd, dst, src, ps, 0, 0);
		CHECK(physical(dst, ps) == phys);
		unsigned char *tmp = src;
		src = dst;
		dst = tmp;
	}
	printf("ok - MOVE %zuK sidecar/retirement allocation failures preserve PTEs and retry\n",
	       ps / 1024);
	CHECK(!close(inject) && !close(fd) && !munmap(a, 2 * native) &&
	      !munmap(b, 2 * native));
}

static int exercise(size_t native)
{
	size_t ps = getauxval(AT_PAGESZ), len = 16 * native;
	on_cpu(0);
	unsigned char *src =
		mmap((void *)0x20000000, len, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	unsigned char *dst =
		mmap((void *)0x30000000, len, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	CHECK(src != MAP_FAILED && dst != MAP_FAILED);
	for (size_t i = 0; i < len; i++)
		src[i] = pattern(i);
	int fd = syscall(SYS_userfaultfd,
			 O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(fd >= 0);
	struct uffdio_api api = { .api = UFFD_API,
				  .features = UFFD_FEATURE_MOVE };
	CHECK(!ioctl(fd, UFFDIO_API, &api) &&
	      (api.features & UFFD_FEATURE_MOVE));
	struct uffdio_register reg = { .range = { (uintptr_t)dst, len },
				       .mode = UFFDIO_REGISTER_MODE_MISSING };
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &reg) &&
	      (reg.ioctls & (1ULL << _UFFDIO_MOVE)));
	uint64_t pfns[256];
	CHECK(native / ps <= sizeof(pfns) / sizeof(pfns[0]));
	for (size_t i = 0; i < native / ps; i++)
		pfns[i] = physical(src + i * ps, ps);
	uint64_t p = physical(src + 3 * ps, ps);
	struct tlb_reader reader = { .src = src + 3 * ps, .dst = dst + ps };
	pthread_t tlb_thread;
	CHECK(!pthread_create(&tlb_thread, NULL, read_after_move, &reader));
	while (!atomic_load(&reader.ready))
		sched_yield();
	move(fd, dst + ps, src + 3 * ps, ps, 0, 0);
	CHECK(physical(dst + ps, ps) == p &&
	      !(entry(src + 3 * ps, ps) & (PRESENT | SWAPPED)));
	atomic_store(&reader.done, 1);
	CHECK(!pthread_join(tlb_thread, NULL));
	bytes(dst + ps, 3 * ps, ps);
	for (size_t i = 0; i < native / ps; i++)
		if (i != 3) {
			CHECK(physical(src + i * ps, ps) == pfns[i]);
			bytes(src + i * ps, i * ps, ps);
		}
	CHECK(src[3 * ps] ==
	      0); /* Source has an independent zero-fault after MOVE. */
	printf("ok - MOVE %zuK exact PFN, divergent offsets, neighbour PFNs/data and source TLB\n",
	       ps / 1024);

	struct io_uring_params ur = { 0 };
	int ring = syscall(SYS_io_uring_setup, 2, &ur);
	CHECK(ring >= 0);
	struct iovec pin = { .iov_base = src + 2 * ps, .iov_len = ps };
	CHECK(!syscall(SYS_io_uring_register, ring, IORING_REGISTER_BUFFERS,
		       &pin, 1));
	p = physical(src + 2 * ps, ps);
	CHECK(move(fd, dst + 2 * ps, src + 2 * ps, ps, 0, EBUSY) == -EBUSY);
	CHECK(physical(src + 2 * ps, ps) == p &&
	      !(entry(dst + 2 * ps, ps) & (PRESENT | SWAPPED)));
	bytes(src + 2 * ps, 2 * ps, ps);
	uint64_t neighbour = physical(src + ps, ps);
	move(fd, dst + 3 * ps, src + ps, ps, 0, 0);
	CHECK(physical(dst + 3 * ps, ps) == neighbour);
	bytes(dst + 3 * ps, ps, ps);
	CHECK(!syscall(SYS_io_uring_register, ring, IORING_UNREGISTER_BUFFERS,
		       NULL, 0));
	CHECK(!close(ring));
	move(fd, dst + 2 * ps, src + 2 * ps, ps, 0, 0);
	CHECK(physical(dst + 2 * ps, ps) == p);
	bytes(dst + 2 * ps, 2 * ps, ps);
	printf("ok - MOVE %zuK source PIN rejected atomically, neighbour PIN allowed, unpin retry\n",
	       ps / 1024);

	CHECK(move(fd, dst + ps, src + 4 * ps, ps, 0, EEXIST) == -EEXIST);
	bytes(src + 4 * ps, 4 * ps, ps);
	bytes(dst + ps, 3 * ps, ps);
	CHECK(!madvise(src + 5 * native, native, MADV_DONTNEED));
	CHECK(*(volatile unsigned char *)(src + 5 * native) == 0);
	move(fd, dst + 5 * native, src + 5 * native, ps, 0, 0);
	CHECK(dst[5 * native] == 0);
	dst[5 * native] = 0x7d;
	CHECK(src[5 * native] == 0);
	CHECK(!madvise(src + 6 * native, native, MADV_DONTNEED));
	CHECK(move(fd, dst + 6 * native, src + 6 * native, ps, 0, ENOENT) ==
	      -ENOENT);
	move(fd, dst + 6 * native, src + 6 * native, ps,
	     UFFDIO_MOVE_MODE_ALLOW_SRC_HOLES, 0);
	CHECK(!(entry(dst + 6 * native, ps) & (PRESENT | SWAPPED)));
	CHECK(!madvise(src + 10 * native + ps, ps, MADV_DONTNEED));
	p = physical(src + 10 * native, ps);
	CHECK(move(fd, dst + 12 * native, src + 10 * native, 2 * ps, 0,
		   EAGAIN) == (long)ps);
	CHECK(physical(dst + 12 * native, ps) == p);
	bytes(dst + 12 * native, 10 * native, ps);
	CHECK(!(entry(dst + 12 * native + ps, ps) & (PRESENT | SWAPPED)));
	printf("ok - MOVE %zuK collision, zero, holes and partial progress\n",
	       ps / 1024);

	int pipefd[2];
	CHECK(!pipe(pipefd));
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(pipefd[1]);
		char c;
		_exit(read(pipefd[0], &c, 1) == 1 ? 0 : 1);
	}
	close(pipefd[0]);
	CHECK(move(fd, dst + 11 * native, src + 12 * native, ps, 0, EBUSY) ==
	      -EBUSY);
	CHECK(write(pipefd[1], "x", 1) == 1);
	close(pipefd[1]);
	int status;
	CHECK(waitpid(child, &status, 0) == child && status == 0);
	*(volatile unsigned char *)(src + 12 * native) = pattern(12 * native);
	p = physical(src + 12 * native, ps);
	move(fd, dst + 11 * native, src + 12 * native, ps, 0, 0);
	CHECK(physical(dst + 11 * native, ps) == p);
	bytes(dst + 11 * native, 12 * native, ps);
	printf("ok - MOVE %zuK fork-shared source rejected, exclusive write/retry\n",
	       ps / 1024);

	/* Keep fault-in coverage as well as restoring an untouched PTE at swapoff. */
	for (unsigned int unuse = 0; unuse < 2; unuse++) {
		size_t off = ps < native ? ps : 0;
		size_t base = (unuse ? 13 : 8) * native;
		unsigned char *s = src + base + off;
		unsigned char *d = dst + base + 2 * ps;

		*(volatile unsigned char *)s = pattern(base + off);
		for (unsigned int tries = 0;
		     !(entry(s, ps) & SWAPPED) && tries < 100; tries++) {
			CHECK(!madvise(src + base, native, MADV_PAGEOUT));
			usleep(20000);
		}
		uint64_t old = entry(s, ps);

		CHECK(old & SWAPPED);
		move(fd, d, s, ps, 0, 0);
		uint64_t now = entry(d, ps);

		CHECK((now & SWAPPED) && ((now & VALUE_MASK) == (old & VALUE_MASK)) &&
		      !(entry(s, ps) & (PRESENT | SWAPPED)));
		if (unuse) {
			/* Do not fault the moved slot in before swapoff restores it. */
			CHECK(!swapoff("/dev/vda"));
			CHECK(entry(d, ps) & PRESENT);
		}
		bytes(d, base + off, ps);
		if (unuse)
			CHECK(!swapon("/dev/vda", 0));
		printf("ok - MOVE %zuK %s restores transferred swap PTE at new offset\n",
		       ps / 1024, unuse ? "swapoff" : "fault");
	}
	CHECK(!close(fd));
	CHECK(!munmap(src, len));
	CHECK(!munmap(dst, len));
	concurrent_move(ps);
	allocation_failure(ps, native);
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 3 && !strcmp(argv[1], "--exercise")) {
		if (getppid() != 1)
			return 2;
		return exercise(strtoul(argv[2], NULL, 10));
	}
	if (getpid() != 1 || argc != 1)
		return 2;
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	CHECK(!mount("devtmpfs", "/dev", "devtmpfs", 0, NULL));
	struct stat st;
	for (unsigned i = 0; i < 100 && stat("/dev/vda", &st); i++)
		usleep(20000);
	CHECK(!stat("/dev/vda", &st) && S_ISBLK(st.st_mode) &&
	      !swapon("/dev/vda", 0));
	size_t native = getauxval(AT_PAGESZ), sizes[] = { native, 4096, 16384 };
	int failed = 0;
	for (unsigned i = 0; i < (native > 16384 ? 3U : 2U); i++) {
		pid_t c = fork();
		int status = 0;
		CHECK(c >= 0);
		if (!c) {
			char n[32];
			snprintf(n, sizeof(n), "%zu", native);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(4);
			execl("/init", "/init", "--exercise", n, NULL);
			_exit(5);
		}
		CHECK(waitpid(c, &status, 0) == c);
		printf("%s - MOVE ABI %zuK status=%#x\n",
		       status ? "not ok" : "ok", sizes[i] / 1024, status);
		failed += !!status;
	}
	if (swapoff("/dev/vda"))
		failed++;
	printf("UFFD MOVE CONTRACT %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
