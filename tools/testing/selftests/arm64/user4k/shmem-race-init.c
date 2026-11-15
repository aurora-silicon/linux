// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define ROUNDS 512
#define CHECK(x)                                                         \
	do {                                                             \
		if (!(x)) {                                              \
			printf("not ok - shmem race line=%d errno=%d\n", \
			       __LINE__, errno);                         \
			exit(1);                                         \
		}                                                        \
	} while (0)
struct control {
	unsigned start, stop, ready;
	unsigned long aliases;
};
struct state {
	struct control *control;
	int file, uffd, resize;
	size_t ps, native, len;
	unsigned char *p, *q, *source;
	unsigned copied, existed, eof, alloc_eof, retry;
};
static unsigned load(unsigned *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void store(unsigned *p, unsigned value)
{
	__atomic_store_n(p, value, __ATOMIC_RELEASE);
}
static void bind_cpu(unsigned cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	CHECK(!sched_setaffinity(0, sizeof(set), &set));
}
static void start_wait(struct control *c)
{
	__atomic_add_fetch(&c->ready, 1, __ATOMIC_RELEASE);
	while (!load(&c->start))
		sched_yield();
}
static void *mutate(void *arg)
{
	struct state *s = arg;
	bind_cpu(0);
	start_wait(s->control);
	for (unsigned i = 0; i < ROUNDS; i++) {
		if (s->resize)
			CHECK(!ftruncate(s->file, s->ps) &&
			      !ftruncate(s->file, s->len));
		else
			CHECK(!fallocate(s->file,
					 FALLOC_FL_PUNCH_HOLE |
						 FALLOC_FL_KEEP_SIZE,
					 s->ps, s->ps));
		sched_yield();
	}
	return NULL;
}
static void *populate(void *arg)
{
	struct state *s = arg;
	bind_cpu(1);
	start_wait(s->control);
	for (unsigned i = 0; i < ROUNDS; i++) {
		struct uffdio_copy c = { .dst = (uintptr_t)(s->p + s->ps),
					 .src = (uintptr_t)s->source,
					 .len = s->ps };
		errno = 0;
		int ret = ioctl(s->uffd, UFFDIO_COPY, &c);
		if (!ret) {
			CHECK(c.copy == (long long)s->ps);
			s->copied++;
		} else {
			CHECK(ret == -1 && c.copy == -errno);
			if (errno == EEXIST)
				s->existed++;
			else if (errno == EFAULT && s->resize)
				s->eof++;
			/* Native shmem allocator preserves its NULL/ENOMEM EOF ABI. */
			else if (errno == ENOMEM && s->resize &&
				 s->ps == s->native)
				s->alloc_eof++;
			else if (errno == EAGAIN)
				s->retry++;
			else {
				printf("# unexpected COPY error=%d bytes=%lld\n",
				       errno, (long long)c.copy);
				CHECK(0);
			}
		}
		sched_yield();
	}
	return NULL;
}
static int valid_byte(unsigned char value)
{
	return value == 0 || value == 0x61 || value == 0xa2;
}
static void *fine_alias(void *arg)
{
	struct state *s = arg;
	bind_cpu(0);
	start_wait(s->control);
	for (unsigned i = 0; i < ROUNDS; i++) {
		if (!s->resize) {
			CHECK(!madvise(s->q, s->ps, MADV_DONTNEED));
			CHECK(valid_byte(*(volatile unsigned char *)s->q));
		} else {
			/* The retained prefix remains inside EOF throughout resize. */
			CHECK(*(volatile unsigned char *)s->p == 0x61);
		}
		sched_yield();
	}
	return NULL;
}
static int native_alias(int file, int control, size_t native, size_t ps,
			int resize)
{
	CHECK(getauxval(AT_PAGESZ) == native);
	bind_cpu(1);
	alarm(60);
	struct control *c = mmap(NULL, native, PROT_READ | PROT_WRITE,
				 MAP_SHARED, control, 0);
	unsigned char *p = mmap(NULL, 4 * native, PROT_READ | PROT_WRITE,
				MAP_SHARED, file, 0);
	CHECK(c != MAP_FAILED && p != MAP_FAILED &&
	      !madvise(p, 4 * native, MADV_NOHUGEPAGE));
	start_wait(c);
	unsigned long cycles = 0;
	while (!load(&c->stop)) {
		CHECK(!madvise(p, 2 * native, MADV_DONTNEED));
		CHECK(*(volatile unsigned char *)p == 0x61);
		if (!resize)
			CHECK(valid_byte(*(volatile unsigned char *)(p + ps)));
		cycles++;
		sched_yield();
	}
	c->aliases = cycles;
	CHECK(!munmap(p, 4 * native) && !munmap(c, native));
	return 0;
}
static int target(size_t native, size_t ps, int resize)
{
	CHECK(getauxval(AT_PAGESZ) == ps);
	alarm(60);
	struct state s = {
		.native = native, .ps = ps, .len = 4 * native, .resize = resize
	};
	s.file = memfd_create("race-cache", 0);
	int control = memfd_create("race-control", 0);
	CHECK(s.file >= 0 && control >= 0 && !ftruncate(s.file, s.len) &&
	      !ftruncate(control, native));
	s.control = mmap(NULL, native, PROT_READ | PROT_WRITE, MAP_SHARED,
			 control, 0);
	s.source = malloc(s.len);
	CHECK(s.control != MAP_FAILED && s.source);
	memset(s.source, 0x61, s.len);
	CHECK(pwrite(s.file, s.source, s.len, 0) == (ssize_t)s.len);
	s.p = mmap(NULL, s.len, PROT_READ | PROT_WRITE, MAP_SHARED, s.file, 0);
	s.q = mmap(NULL, s.len - ps, PROT_READ | PROT_WRITE, MAP_SHARED, s.file,
		   ps);
	CHECK(s.p != MAP_FAILED && s.q != MAP_FAILED &&
	      !madvise(s.p, s.len, MADV_NOHUGEPAGE));
	CHECK(s.p[0] ==
	      0x61); /* Avoid blocking the resize prefix reader on UFFD. */
	unsigned char *cow = MAP_FAILED;
	if (!resize) {
		cow = mmap(NULL, s.len, PROT_READ | PROT_WRITE, MAP_PRIVATE,
			   s.file, 0);
		CHECK(cow != MAP_FAILED);
		memset(cow + ps, 0xe1, ps);
	}
	s.uffd = syscall(SYS_userfaultfd,
			 O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(s.uffd >= 0);
	struct uffdio_api api = { .api = UFFD_API };
	CHECK(!ioctl(s.uffd, UFFDIO_API, &api));
	struct uffdio_register reg = { .range = { (uintptr_t)s.p, s.len },
				       .mode = UFFDIO_REGISTER_MODE_MISSING };
	CHECK(!ioctl(s.uffd, UFFDIO_REGISTER, &reg));
	memset(s.source, 0xa2, ps);
	pid_t alias = fork();
	CHECK(alias >= 0);
	if (!alias) {
		char f[24], c[24], n[24], p[24], r[12];
		snprintf(f, sizeof(f), "%d", s.file);
		snprintf(c, sizeof(c), "%d", control);
		snprintf(n, sizeof(n), "%zu", native);
		snprintf(p, sizeof(p), "%zu", ps);
		snprintf(r, sizeof(r), "%d", resize);
		CHECK(!prctl(SET_EXEC_PAGE_SIZE, native, 0, 0, 0));
		execl("/init", "init", "--alias", f, c, n, p, r, NULL);
		_exit(127);
	}
	pthread_t mutation, copy, fine;
	CHECK(!pthread_create(&mutation, NULL, mutate, &s) &&
	      !pthread_create(&copy, NULL, populate, &s) &&
	      !pthread_create(&fine, NULL, fine_alias, &s));
	while (load(&s.control->ready) != 4)
		sched_yield();
	store(&s.control->start, 1);
	CHECK(!pthread_join(mutation, NULL) && !pthread_join(copy, NULL) &&
	      !pthread_join(fine, NULL));
	store(&s.control->stop, 1);
	int status;
	CHECK(waitpid(alias, &status, 0) == alias && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	CHECK(s.control->aliases &&
	      s.copied + s.existed + s.eof + s.alloc_eof + s.retry == ROUNDS);
	printf("# race %zuK/%zuK resize=%d COPY=%u EEXIST=%u EOF=%u native-alloc-EOF=%u retry=%u native-refaults=%lu\n",
	       ps / 1024, native / 1024, resize, s.copied, s.existed, s.eof,
	       s.alloc_eof, s.retry, s.control->aliases);
	/* Once concurrent actors stop, exact cache/PTE state must be recoverable. */
	CHECK(!ftruncate(s.file, s.len));
	CHECK(!fallocate(s.file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, ps,
			 ps));
	memset(s.source, 0xd4, ps);
	struct uffdio_copy fill = { .dst = (uintptr_t)(s.p + ps),
				    .src = (uintptr_t)s.source,
				    .len = ps };
	CHECK(!ioctl(s.uffd, UFFDIO_COPY, &fill) && fill.copy == (long long)ps);
	unsigned char *back = malloc(s.len);
	CHECK(back && pread(s.file, back, s.len, 0) == (ssize_t)s.len);
	for (size_t i = 0; i < s.len; i++) {
		unsigned char expected = i >= ps && i < 2 * ps ? 0xd4 :
					 resize && i >= ps     ? 0 :
								 0x61;
		if (back[i] != expected) {
			printf("# byte=%zu actual=%#x expected=%#x\n", i,
			       back[i], expected);
			CHECK(0);
		}
	}
	for (size_t i = 0; i < ps; i++) {
		CHECK(s.p[ps + i] == 0xd4 && s.q[i] == 0xd4);
		if (!resize)
			CHECK(cow[ps + i] == 0xe1);
	}
	CHECK(!close(s.uffd) && !munmap(s.p, s.len) &&
	      !munmap(s.q, s.len - ps));
	if (!resize)
		CHECK(!munmap(cow, s.len));
	CHECK(!munmap(s.control, native) && !close(control) && !close(s.file));
	free(back);
	free(s.source);
	printf("ok - concurrent shmem %zuK on %zuK resize=%d cache bytes, aliases and private COW\n",
	       ps / 1024, native / 1024, resize);
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 7 && !strcmp(argv[1], "--alias"))
		return native_alias(atoi(argv[2]), atoi(argv[3]),
				    strtoul(argv[4], NULL, 10),
				    strtoul(argv[5], NULL, 10), atoi(argv[6]));
	if (argc == 5 && !strcmp(argv[1], "--target"))
		return target(strtoul(argv[2], NULL, 10),
			      strtoul(argv[3], NULL, 10), atoi(argv[4]));
	if (getpid() != 1 || argc != 1)
		return 2;
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	size_t native = getauxval(AT_PAGESZ);
	for (size_t ps = 4096; ps <= native; ps *= 4)
		for (int resize = 0; resize < 2; resize++) {
			pid_t pid = fork();
			CHECK(pid >= 0);
			if (!pid) {
				char n[24], p[24], r[12];
				snprintf(n, sizeof(n), "%zu", native);
				snprintf(p, sizeof(p), "%zu", ps);
				snprintf(r, sizeof(r), "%d", resize);
				CHECK(!prctl(SET_EXEC_PAGE_SIZE, ps, 0, 0, 0));
				execl("/init", "init", "--target", n, p, r,
				      NULL);
				_exit(127);
			}
			int status;
			CHECK(waitpid(pid, &status, 0) == pid &&
			      WIFEXITED(status) && !WEXITSTATUS(status));
		}
	puts("SHMEM RACE CONTRACT PASS");
	reboot(RB_POWER_OFF);
	return 0;
}
