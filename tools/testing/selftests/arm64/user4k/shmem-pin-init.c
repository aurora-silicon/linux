// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <linux/io_uring.h>
#include <linux/userfaultfd.h>
#include <linux/falloc.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define CHECK(x)                                                        \
	do {                                                            \
		if (!(x)) {                                             \
			printf("not ok - shmem pin line=%d errno=%d\n", \
			       __LINE__, errno);                        \
			exit(1);                                        \
		}                                                       \
	} while (0)
struct ring {
	int fd;
	struct io_uring_params p;
	void *sq, *cq, *sqes;
	size_t sqlen, cqlen, sqelen;
};
static unsigned load(unsigned *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void store(unsigned *p, unsigned v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static size_t align_size(size_t n)
{
	size_t ps = getauxval(AT_PAGESZ);
	return (n + ps - 1) & ~(ps - 1);
}
static void destroy(struct ring *r)
{
	if (r->sq && r->sq != MAP_FAILED)
		munmap(r->sq, r->sqlen);
	if (r->cq && r->cq != MAP_FAILED && r->cq != r->sq)
		munmap(r->cq, r->cqlen);
	if (r->sqes && r->sqes != MAP_FAILED)
		munmap(r->sqes, r->sqelen);
	if (r->fd >= 0)
		close(r->fd);
	*r = (struct ring){ .fd = -1 };
}
static bool setup(struct ring *r, unsigned entries, unsigned flags)
{
	*r = (struct ring){ .fd = -1 };
	r->p.flags = flags;
	r->fd = syscall(__NR_io_uring_setup, entries, &r->p);
	if (r->fd < 0)
		return false;
	r->sqlen = align_size(r->p.sq_off.array +
			      r->p.sq_entries * sizeof(unsigned));
	r->cqlen = align_size(r->p.cq_off.cqes +
			      r->p.cq_entries * sizeof(struct io_uring_cqe) *
				      (flags & IORING_SETUP_CQE32 ? 2 : 1));
	r->sqelen = align_size(r->p.sq_entries * sizeof(struct io_uring_sqe) *
			       (flags & IORING_SETUP_SQE128 ? 2 : 1));
	if (r->p.features & IORING_FEAT_SINGLE_MMAP) {
		if (r->cqlen > r->sqlen)
			r->sqlen = r->cqlen;
		r->cqlen = r->sqlen;
	}
	r->sq = mmap(NULL, r->sqlen, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd,
		     IORING_OFF_SQ_RING);
	r->cq = (r->p.features & IORING_FEAT_SINGLE_MMAP) ?
			r->sq :
			mmap(NULL, r->cqlen, PROT_READ | PROT_WRITE, MAP_SHARED,
			     r->fd, IORING_OFF_CQ_RING);
	r->sqes = mmap(NULL, r->sqelen, PROT_READ | PROT_WRITE, MAP_SHARED,
		       r->fd, IORING_OFF_SQES);
	return r->sq != MAP_FAILED && r->cq != MAP_FAILED &&
	       r->sqes != MAP_FAILED;
}
static bool request(struct ring *r, unsigned op, int fd, void *buf,
		    unsigned len, uint64_t off, bool async)
{
	unsigned *sqtail = r->sq + r->p.sq_off.tail,
		 *sqmask = r->sq + r->p.sq_off.ring_mask;
	unsigned *cqhead = r->cq + r->p.cq_off.head,
		 *cqtail = r->cq + r->p.cq_off.tail,
		 *cqmask = r->cq + r->p.cq_off.ring_mask;
	unsigned tail = load(sqtail), head = load(cqhead),
		 idx = tail & load(sqmask);
	unsigned sqstride = sizeof(struct io_uring_sqe) *
			    (r->p.flags & IORING_SETUP_SQE128 ? 2 : 1);
	unsigned cqstride = sizeof(struct io_uring_cqe) *
			    (r->p.flags & IORING_SETUP_CQE32 ? 2 : 1);
	struct io_uring_sqe *sqe = r->sqes + idx * sqstride;
	memset(sqe, 0, sqstride);
	sqe->opcode = op;
	sqe->fd = fd;
	sqe->addr = (uintptr_t)buf;
	sqe->len = len;
	sqe->off = off;
	sqe->user_data = (uint64_t)tail + 1234;
	if (async)
		sqe->flags = IOSQE_ASYNC;
	((unsigned *)(r->sq + r->p.sq_off.array))[idx] = idx;
	store(sqtail, tail + 1);
	int ret = syscall(__NR_io_uring_enter, r->fd, 1, 1,
			  IORING_ENTER_GETEVENTS, NULL, 0);
	if (ret != 1 || load(cqtail) != head + 1) {
		printf("enter=%d errno=%d\n", ret, errno);
		return false;
	}
	struct io_uring_cqe *cqe =
		r->cq + r->p.cq_off.cqes + (head & load(cqmask)) * cqstride;
	unsigned expected = op == IORING_OP_NOP ? 0 : len;
	if (op == IORING_OP_READV_FIXED || op == IORING_OP_WRITEV_FIXED) {
		expected = 0;
		for (unsigned i = 0; i < len; i++)
			expected += ((struct iovec *)buf)[i].iov_len;
	}
	bool ok = cqe->user_data == (uint64_t)tail + 1234 &&
		  cqe->res == (int)expected;
	if (!ok)
		printf("cqe res=%d expected=%u\n", cqe->res, expected);
	store(cqhead, head + 1);
	return ok;
}

static void fragment_geometry(unsigned char *p, size_t ps, int input)
{
	struct ring r;
	CHECK(setup(&r, 8, 0));
	struct iovec reg = { p + 123, 3 * ps - 246 };
	unsigned char *src = malloc(3 * ps), *expected = malloc(3 * ps);
	CHECK(src && expected);
	memset(expected, 0x61, 3 * ps);
	memset(src, 0xb7, 3 * ps);
	CHECK(pwrite(input, src, 3 * ps, 0) == (ssize_t)(3 * ps));
	CHECK(!syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_BUFFERS,
		       &reg, 1));
	CHECK(request(&r, IORING_OP_READ_FIXED, input, p + ps + 71, ps + 31, 17,
		      true));
	memset(expected + ps + 71, 0xb7, ps + 31);
	CHECK(!memcmp(p, expected, 3 * ps));
	memset(src, 0xc8, 3 * ps);
	CHECK(pwrite(input, src, 3 * ps, 0) == (ssize_t)(3 * ps));
	CHECK(request(&r, IORING_OP_WRITE_FIXED, input, p + ps + 71, ps + 31, 9,
		      true));
	CHECK(pread(input, src, ps + 31, 9) == (ssize_t)(ps + 31));
	for (size_t i = 0; i < ps + 31; i++)
		CHECK(src[i] == 0xb7);
	struct iovec vec[2] = { { p + 2 * ps + 17, ps - 400 },
				{ p + ps + 91, ps - 600 } };
	memset(src, 0xd5, 3 * ps);
	CHECK(pwrite(input, src, 3 * ps, 0) == (ssize_t)(3 * ps));
	CHECK(request(&r, IORING_OP_READV_FIXED, input, vec, 2, 13, true));
	for (unsigned i = 0; i < 2; i++)
		memset(expected + ((unsigned char *)vec[i].iov_base - p), 0xd5,
		       vec[i].iov_len);
	CHECK(!memcmp(p, expected, 3 * ps));
	memset(src, 0xc8, 3 * ps);
	CHECK(pwrite(input, src, 3 * ps, 0) == (ssize_t)(3 * ps));
	CHECK(request(&r, IORING_OP_WRITEV_FIXED, input, vec, 2, 23, true));
	size_t total = vec[0].iov_len + vec[1].iov_len;
	CHECK(pread(input, src, total, 23) == (ssize_t)total);
	for (size_t i = 0; i < total; i++)
		CHECK(src[i] == 0xd5);
	CHECK(!syscall(SYS_io_uring_register, r.fd, IORING_UNREGISTER_BUFFERS,
		       NULL, 0));
	destroy(&r);
	free(src);
	free(expected);
	memset(p, 0x61, 3 * ps);
	printf("ok - %zuK fixed/vector IO uses user fragment geometry and preserves guards\n",
	       ps / 1024);
}

struct punch_worker {
	int file;
	size_t off, ps;
	pthread_barrier_t start;
};
static void bind_cpu(unsigned cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	CHECK(!sched_setaffinity(0, sizeof(set), &set));
}
static void *punch_repeatedly(void *arg)
{
	struct punch_worker *w = arg;
	bind_cpu(1);
	pthread_barrier_wait(&w->start);
	for (unsigned i = 0; i < 256; i++) {
		CHECK(!fallocate(w->file,
				 FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				 w->off, w->ps));
		sched_yield();
	}
	return NULL;
}
static void pin_hole_concurrency(struct ring *r, int file, int input,
				 unsigned char *p, size_t off, size_t ps)
{
	struct punch_worker w = { .file = file, .off = off, .ps = ps };
	pthread_t thread;
	CHECK(!pthread_barrier_init(&w.start, NULL, 2));
	bind_cpu(0);
	CHECK(!pthread_create(&thread, NULL, punch_repeatedly, &w));
	pthread_barrier_wait(&w.start);
	for (unsigned i = 0; i < 256; i++)
		CHECK(request(r, IORING_OP_READ_FIXED, input, p + off, ps, 0,
			      true));
	CHECK(!pthread_join(thread, NULL) &&
	      !pthread_barrier_destroy(&w.start));
	printf("ok - %zuK concurrent fixed writes and 256 hole punches on separate CPUs\n",
	       ps / 1024);
}

static int target(size_t native, size_t ps, int resize)
{
	CHECK(getauxval(AT_PAGESZ) == ps);
	alarm(30);
	size_t len = 4 * native, off = native + ps;
	bool retained = ps < native;
	int file = memfd_create("pin-cache", 0),
	    input = memfd_create("pin-input", 0);
	CHECK(file >= 0 && input >= 0 && !ftruncate(file, len) &&
	      !ftruncate(input, ps));
	unsigned char *src = malloc(len), *back = malloc(len);
	CHECK(src && back);
	memset(src, 0x61, len);
	CHECK(pwrite(file, src, len, 0) == (ssize_t)len);
	memset(src, 0xb7, ps);
	CHECK(pwrite(input, src, ps, 0) == (ssize_t)ps);
	unsigned char *p =
		mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	CHECK(p != MAP_FAILED && !madvise(p, len, MADV_NOHUGEPAGE));
	CHECK(p[off] == 0x61);
	fragment_geometry(p, ps, input);
	memset(src, 0xb7, ps);
	CHECK(pwrite(input, src, ps, 0) == (ssize_t)ps);
	struct ring r;
	CHECK(setup(&r, 8, 0));
	struct iovec reg = { p + off, ps };
	CHECK(!syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_BUFFERS,
		       &reg, 1));
	int uffd = syscall(SYS_userfaultfd,
			   O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(uffd >= 0);
	struct uffdio_api api = { .api = UFFD_API };
	CHECK(!ioctl(uffd, UFFDIO_API, &api));
	struct uffdio_register ur = { .range = { (uintptr_t)p, len },
				      .mode = UFFDIO_REGISTER_MODE_MISSING };
	CHECK(!ioctl(uffd, UFFDIO_REGISTER, &ur));
	if (resize)
		CHECK(!ftruncate(file, off) && !ftruncate(file, len));
	else
		CHECK(!fallocate(file,
				 FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				 off, ps));
	if (retained && !resize)
		pin_hole_concurrency(&r, file, input, p, off, ps);
	/* Fixed IO accesses its original FOLL_PIN reference after PTE removal. */
	CHECK(request(&r, IORING_OP_READ_FIXED, input, p + off, ps, 0, true));
	/* Also prove the original pinned object contains the completed write. */
	CHECK(request(&r, IORING_OP_WRITE_FIXED, input, p + off, ps, 0, true));
	CHECK(pread(input, back, ps, 0) == (ssize_t)ps);
	for (size_t i = 0; i < ps; i++)
		CHECK(back[i] == 0xb7);
	CHECK(!syscall(SYS_io_uring_register, r.fd, IORING_UNREGISTER_BUFFERS,
		       NULL, 0));
	memset(src, 0xc8, ps);
	struct uffdio_copy c = { .dst = (uintptr_t)(p + off),
				 .src = (uintptr_t)src,
				 .len = ps };
	errno = 0;
	int ret = ioctl(uffd, UFFDIO_COPY, &c);
	printf("# pin %zuK/%zuK resize=%d retained=%d COPY=%d errno=%d bytes=%lld\n",
	       ps / 1024, native / 1024, resize, retained, ret, errno,
	       (long long)c.copy);
	if (retained)
		CHECK(ret == -1 && errno == EEXIST && c.copy == -EEXIST);
	else
		CHECK(!ret && c.copy == (long long)ps);
	CHECK(pread(file, back, len, 0) == (ssize_t)len);
	for (size_t i = 0; i < len; i++) {
		unsigned char expected = i >= off && i < off + ps ?
						 (retained ? 0xb7 : 0xc8) :
					 resize && i >= off ? 0 :
							      0x61;
		CHECK(back[i] == expected);
	}
	for (size_t i = 0; i < ps; i++)
		CHECK(p[off + i] == (retained ? 0xb7 : 0xc8));
	/* A new punch after unpin can once again be populated by the pager. */
	CHECK(!fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off,
			 ps));
	c.copy = 0;
	CHECK(!ioctl(uffd, UFFDIO_COPY, &c) && c.copy == (long long)ps);
	for (size_t i = 0; i < ps; i++)
		CHECK(p[off + i] == 0xc8);
	destroy(&r);
	CHECK(!close(uffd) && !munmap(p, len) && !close(file) && !close(input));
	free(src);
	free(back);
	printf("ok - writable pin %zuK on %zuK resize=%d retention and detach\n",
	       ps / 1024, native / 1024, resize);
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
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
				char n[24], g[24], r[12];
				snprintf(n, sizeof(n), "%zu", native);
				snprintf(g, sizeof(g), "%zu", ps);
				snprintf(r, sizeof(r), "%d", resize);
				CHECK(!prctl(SET_EXEC_PAGE_SIZE, ps, 0, 0, 0));
				execl("/init", "init", "--target", n, g, r,
				      NULL);
				_exit(127);
			}
			int status;
			CHECK(waitpid(pid, &status, 0) == pid &&
			      WIFEXITED(status) && !WEXITSTATUS(status));
		}
	puts("ok - shmem writable pin fixture complete");
	reboot(RB_POWER_OFF);
	return 0;
}
