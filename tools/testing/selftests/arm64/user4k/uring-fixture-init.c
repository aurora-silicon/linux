// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <unistd.h>
#include <linux/io_uring.h>
#include "../../kselftest.h"

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
		ksft_print_msg("enter=%d errno=%d\n", ret, errno);
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
		ksft_print_msg("cqe res=%d expected=%u\n", cqe->res, expected);
	store(cqhead, head + 1);
	return ok;
}
static unsigned long pinned_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[160];
	unsigned long n = ~0UL;
	if (!f)
		return n;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "VmPin: %lu kB", &n) == 1)
			break;
	fclose(f);
	return n;
}
static bool bytes_are(unsigned char *p, size_t len, unsigned char value)
{
	for (size_t i = 0; i < len; i++)
		if (p[i] != value)
			return false;
	return true;
}
static void fixed_checks(int fd, unsigned char *p, size_t len, size_t ps)
{
	struct ring r = { .fd = -1 };
	unsigned char *q = mmap(NULL, len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct iovec reg = { p + 512, len - 1024 };
	bool live = false, ok = setup(&r, 8, 0);
	unsigned long baseline = pinned_kb();
	memset(p, 0xa6, len);
	if (ok)
		live = syscall(__NR_io_uring_register, r.fd,
			       IORING_REGISTER_BUFFERS, &reg, 1) == 0;
	ok &= live;
	ksft_test_result(ok, "register unaligned long-term buffer\n");
	if (ok) {
		ok = request(&r, IORING_OP_WRITE_FIXED, fd, p + 512, 7 * 512, 0,
			     false);
		memset(p + 512, 0, 7 * 512);
		ok &= request(&r, IORING_OP_READ_FIXED, fd, p + 512, 7 * 512, 0,
			      false) &&
		      bytes_are(p + 512, 7 * 512, 0xa6);
	}
	ksft_test_result(ok, "fixed first-quarter slice offsets\n");
	if (ok) {
		size_t off = len - 3 * ps;
		memset(p + off, 0x49, 2 * ps);
		ok = request(&r, IORING_OP_WRITE_FIXED, fd, p + off, 2 * ps,
			     65536, true);
		memset(p + off, 0, 2 * ps);
		ok &= request(&r, IORING_OP_READ_FIXED, fd, p + off, 2 * ps,
			      65536, true) &&
		      bytes_are(p + off, 2 * ps, 0x49);
	}
	ksft_test_result(ok, "fixed late-buffer fast indexing and async I/O\n");
	if (ok) {
		struct iovec v[3] = { { p + 512, 512 },
				      { p + 2 * ps + 512, ps - 512 },
				      { p + 17 * ps + 512, 1024 } };
		memset(p, 0x78, len);
		ok = request(&r, IORING_OP_WRITEV_FIXED, fd, v, 3, 131072,
			     true);
		memset(p, 0x11, len);
		ok &= request(&r, IORING_OP_READV_FIXED, fd, v, 3, 131072,
			      true);
		for (unsigned i = 0; i < 3; i++)
			ok &= bytes_are(v[i].iov_base, v[i].iov_len, 0x78);
		ok &= p[0] == 0x11 && p[ps + 512] == 0x11 && p[17 * ps] == 0x11;
	}
	ksft_test_result(ok, "fixed vectored physical offsets and guards\n");
	if (ok) {
		memset(p + 512, 0x3e, len - 1024);
		ok = request(&r, IORING_OP_WRITE_FIXED, fd, p + 512, len - 1024,
			     4 * 1024 * 1024, true);
		memset(p + 512, 0, len - 1024);
		ok &= request(&r, IORING_OP_READ_FIXED, fd, p + 512, len - 1024,
			      4 * 1024 * 1024, true) &&
		      bytes_are(p + 512, len - 1024, 0x3e);
	}
	ksft_test_result(ok, "fixed multi-bio transfer\n");
	if (ok) {
		ok = mmap(p, len, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == p;
		memset(p, 0x55, len);
		ok &= request(&r, IORING_OP_WRITE_FIXED, fd, p + 512, 3 * ps,
			      8 * 1024 * 1024, true);
		if (q != MAP_FAILED) {
			memset(q, 0, 4 * ps);
			ok &= request(&r, IORING_OP_READ, fd, q, 3 * ps,
				      8 * 1024 * 1024, false) &&
			      bytes_are(q, 3 * ps, 0x3e) &&
			      bytes_are(p, len, 0x55);
		} else
			ok = false;
	}
	ksft_test_result(ok,
			 "registered source survives unmap and replacement\n");
	if (ok) {
		memset(q, 0xc1, 3 * ps);
		ok = request(&r, IORING_OP_WRITE, fd, q, 3 * ps,
			     10 * 1024 * 1024, false);
		ok &= request(&r, IORING_OP_READ_FIXED, fd, p + 512, 3 * ps,
			      10 * 1024 * 1024, true);
		ok &= request(&r, IORING_OP_WRITE_FIXED, fd, p + 512, 3 * ps,
			      12 * 1024 * 1024, true);
		memset(q, 0, 3 * ps);
		ok &= request(&r, IORING_OP_READ, fd, q, 3 * ps,
			      12 * 1024 * 1024, false) &&
		      bytes_are(q, 3 * ps, 0xc1) && bytes_are(p, len, 0x55);
	}
	ksft_test_result(
		ok,
		"registered destination survives unmap without touching replacement\n");
	if (live) {
		ok = syscall(__NR_io_uring_register, r.fd,
			     IORING_UNREGISTER_BUFFERS, NULL, 0) == 0;
		live = false;
	} else
		ok = false;
	ok &= pinned_kb() == baseline;
	if (ok) {
		ok = !mprotect(p + ps, ps, PROT_NONE);
		struct iovec bad[2] = { { p, ps }, { p + ps, ps } };
		ok &= syscall(__NR_io_uring_register, r.fd,
			      IORING_REGISTER_BUFFERS, bad, 2) == -1 &&
		      errno == EFAULT;
		ok &= !mprotect(p + ps, ps, PROT_READ | PROT_WRITE) &&
		      pinned_kb() == baseline;
		live = syscall(__NR_io_uring_register, r.fd,
			       IORING_REGISTER_BUFFERS, &reg, 1) == 0;
		ok &= live;
	}
	ksft_test_result(
		ok,
		"failed multi-buffer registration rolls back pins and accounting\n");
	if (live) {
		ok &= syscall(__NR_io_uring_register, r.fd,
			      IORING_UNREGISTER_BUFFERS, NULL, 0) == 0;
		live = false;
	}
	for (unsigned i = 0; i < 64 && ok; i++) {
		reg = (struct iovec){ p + ps + 512, 2 * ps };
		ok = !madvise(p + ps, 3 * ps, MADV_DONTNEED);
		live = syscall(__NR_io_uring_register, r.fd,
			       IORING_REGISTER_BUFFERS, &reg, 1) == 0;
		ok &= live;
		if (live) {
			ok &= syscall(__NR_io_uring_register, r.fd,
				      IORING_UNREGISTER_BUFFERS, NULL, 0) == 0;
			live = false;
		}
		ok &= pinned_kb() == baseline;
	}
	ksft_test_result(ok,
			 "64 register/reuse/unregister cycles balance VmPin\n");
	/* The file offset deliberately differs from the virtual quarter index. */
	int mem = memfd_create("uring-fixed-shared", 0);
	ok &= mem >= 0 && !ftruncate(mem, len + 3 * ps) && q != MAP_FAILED;
	if (ok) {
		ok = mmap(p, len, PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_FIXED, mem, 3 * ps) == p;
		memset(p, 0x6b, len);
		reg = (struct iovec){ p + 512, len - 1024 };
		live = syscall(__NR_io_uring_register, r.fd,
			       IORING_REGISTER_BUFFERS, &reg, 1) == 0;
		ok &= live;
		struct iovec v[3] = { { p + 512, 512 },
				      { p + 3 * ps + 512, ps - 512 },
				      { p + 23 * ps + 512, 1024 } };
		if (ok) {
			ok = request(&r, IORING_OP_WRITEV_FIXED, fd, v, 3,
				     14 * 1024 * 1024, true);
			memset(p + 512, 0x18, len - 1024);
			ok &= request(&r, IORING_OP_READV_FIXED, fd, v, 3,
				      14 * 1024 * 1024, true);
			for (unsigned i = 0; i < 3; i++)
				ok &= bytes_are(v[i].iov_base, v[i].iov_len,
						0x6b);
			ok &= !msync(p, len, MS_SYNC) &&
			      pread(mem, q, ps, 3 * ps) == (ssize_t)ps;
			ok &= bytes_are(q, 1024, 0x6b) && q[1024] == 0x18 &&
			      p[len - 1] == 0x6b;
		}
	}
	ksft_test_result(
		ok, "registered shared-file quarters use physical offsets\n");
	if (ok) {
		struct iovec replacement = { q + 512, 3 * ps };
		struct io_uring_rsrc_update2 update = {
			.data = (uintptr_t)&replacement, .nr = 1
		};
		memset(q, 0x9d, 4 * ps);
		ok = syscall(__NR_io_uring_register, r.fd,
			     IORING_REGISTER_BUFFERS_UPDATE, &update,
			     sizeof(update)) == 1;
		ok &= request(&r, IORING_OP_WRITE_FIXED, fd, q + 512, 3 * ps,
			      16 * 1024 * 1024, true);
		memset(q + 512, 0, 3 * ps);
		ok &= request(&r, IORING_OP_READ_FIXED, fd, q + 512, 3 * ps,
			      16 * 1024 * 1024, true) &&
		      bytes_are(q + 512, 3 * ps, 0x9d);
	}
	if (live) {
		ok &= syscall(__NR_io_uring_register, r.fd,
			      IORING_UNREGISTER_BUFFERS, NULL, 0) == 0;
		live = false;
	}
	ok &= pinned_kb() == baseline;
	ksft_test_result(
		ok,
		"registered-buffer update transfers ownership and balances accounting\n");
	if (mem >= 0)
		close(mem);
	if (live)
		syscall(__NR_io_uring_register, r.fd, IORING_UNREGISTER_BUFFERS,
			NULL, 0);
	destroy(&r);
	if (q != MAP_FAILED)
		munmap(q, len);
}

static int exercise(void)
{
	struct ring r = { .fd = -1 };
	size_t ps = getauxval(AT_PAGESZ), len = 2 * 1024 * 1024;
	unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int fd = open("/dev/user4k-test", O_RDWR | O_DIRECT);
	bool ok;
	ksft_print_header();
	ksft_set_plan(19);
	ksft_print_msg("ABI=%zuK\n", ps / 1024);
	ok = setup(&r, 4, 0);
	ksft_test_result(ok, "minimum rounded ring mappings\n");
	void *bad = ok ? mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED,
			      r.fd, 4096) :
			 MAP_FAILED;
	bool rejected = ok && bad == MAP_FAILED && errno == EINVAL;
	if (bad != MAP_FAILED)
		munmap(bad, ps);
	ksft_test_result(rejected,
			 "reject sub-native offset in ring selector\n");
	if (ok) {
		ok = true;
		for (unsigned i = 0; i < 64 && ok; i++)
			ok = request(&r, IORING_OP_NOP, -1, NULL, 0, 0, false);
	}
	ksft_test_result(ok, "small submission/completion ring wrap\n");
	if (p == MAP_FAILED || fd < 0)
		ksft_exit_fail_msg("buffer/device setup errno=%d\n", errno);
	if (ok) {
		memset(p + ps, 0x69, 3 * ps);
		ok = request(&r, IORING_OP_WRITE, fd, p + ps, 3 * ps, 0, false);
		memset(p + ps, 0, 3 * ps);
		ok &= request(&r, IORING_OP_READ, fd, p + ps, 3 * ps, 0, false);
		for (size_t i = ps; i < 4 * ps; i++)
			ok &= p[i] == 0x69;
	}
	ksft_test_result(ok,
			 "direct read/write from unregistered user buffers\n");
	if (ok) {
		memset(p + ps, 0x37, 3 * ps);
		ok = request(&r, IORING_OP_WRITE, fd, p + ps, 3 * ps, 65536,
			     true);
		memset(p + ps, 0, 3 * ps);
		ok &= request(&r, IORING_OP_READ, fd, p + ps, 3 * ps, 65536,
			      true);
		for (size_t i = ps; i < 4 * ps; i++)
			ok &= p[i] == 0x37;
	}
	ksft_test_result(ok, "io-worker forced asynchronous read/write\n");
	if (ok) {
		memset(p, 0xb6, len);
		ok = request(&r, IORING_OP_WRITE, fd, p, len, 4 * 1024 * 1024,
			     true);
		memset(p, 0, len);
		ok &= request(&r, IORING_OP_READ, fd, p, len, 4 * 1024 * 1024,
			      true);
		for (size_t i = 0; i < len; i++)
			ok &= p[i] == 0xb6;
	}
	ksft_test_result(ok, "multi-bio io-worker transfer\n");
	destroy(&r);
	ok = setup(&r, 4096, 0);
	if (ok)
		for (unsigned i = 0; i < 8193 && ok; i++)
			ok = request(&r, IORING_OP_NOP, -1, NULL, 0, 0, false);
	ksft_test_result(ok, "large ring crosses native pages and wraps\n");
	destroy(&r);
	ok = setup(&r, 64, IORING_SETUP_SQE128 | IORING_SETUP_CQE32);
	if (ok)
		for (unsigned i = 0; i < 129 && ok; i++)
			ok = request(&r, IORING_OP_NOP, -1, NULL, 0, 0, false);
	ksft_test_result(ok, "128-byte SQEs and 32-byte CQEs\n");
	destroy(&r);
	fixed_checks(fd, p, len, ps);
	close(fd);
	munmap(p, len);
	ksft_print_cnts();
	return ksft_get_fail_cnt() ? 1 : 0;
}
int main(int argc, char **argv)
{
	bool ok = true;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 2 && !strcmp(argv[1], "--test") && getppid() == 1)
		return exercise();
	if (getpid() != 1 || argc != 1)
		return 2;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL))
		return 3;
	FILE *dev = fopen("/sys/class/block/vda/dev", "r");
	unsigned maj, min;
	if (!dev || fscanf(dev, "%u:%u", &maj, &min) != 2)
		return 4;
	fclose(dev);
	if (mknod("/dev/user4k-test", S_IFBLK | 0600, makedev(maj, min)))
		return 5;
	for (unsigned small = 0; small < 2; small++) {
		pid_t pid = fork();
		int status = 0;
		if (!pid) {
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
				  small ? 4096UL : 0UL, 0UL, 0UL, 0UL))
				_exit(3);
			execl("/init", "/init", "--test", NULL);
			_exit(4);
		}
		ok &= pid > 0 && waitpid(pid, &status, 0) == pid &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
	}
	printf("%s - io_uring fixture complete\n", ok ? "ok" : "not ok");
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
