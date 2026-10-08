// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/aio_abi.h>
#include "../../kselftest.h"

static bool use_file;
static unsigned char pattern(size_t i)
{
	return (i * 17 + (i >> 12) * 29 + 71) & 255;
}
static void fill(unsigned char *p, size_t len)
{
	for (size_t i = 0; i < len; i++)
		p[i] = pattern(i);
}
static bool check(const unsigned char *p, size_t len, size_t off)
{
	for (size_t i = 0; i < len; i++)
		if (p[i] != pattern(i + off)) {
			ksft_print_msg("mismatch at %zu: %u vs %u\n", i + off,
				       p[i], pattern(i + off));
			return false;
		}
	return true;
}
static void report(bool ok, size_t ps, const char *name)
{
	ksft_test_result(ok, "%zuK %s (errno=%d)\n", ps / 1024, name, errno);
}
static bool aio_transfer(int fd, void *p, size_t len, off_t off, bool write)
{
	aio_context_t ctx = 0;
	struct iocb cb = { .aio_lio_opcode = write ? IOCB_CMD_PWRITE :
						     IOCB_CMD_PREAD,
			   .aio_fildes = fd,
			   .aio_buf = (uintptr_t)p,
			   .aio_nbytes = len,
			   .aio_offset = off };
	struct iocb *ptr = &cb;
	struct io_event ev;
	struct timespec timeout = { .tv_sec = 10 };
	bool ok;

	if (syscall(__NR_io_setup, 8, &ctx))
		return false;
	ok = syscall(__NR_io_submit, ctx, 1, &ptr) == 1;
	if (ok)
		ok = syscall(__NR_io_getevents, ctx, 1, 1, &ev, &timeout) ==
			     1 &&
		     ev.res == (long)len && !ev.res2;
	ok &= !syscall(__NR_io_destroy, ctx);
	return ok;
}
static int exercise(void)
{
	size_t ps = getauxval(AT_PAGESZ), len = 2 * 1024 * 1024;
	unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char *q = mmap(NULL, len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct iovec vec[4];
	int fd = open(use_file ? "/fs/test" : "/dev/user4k-test",
		      O_RDWR | O_DIRECT | (use_file ? O_CREAT | O_TRUNC : 0),
		      0600),
	    memfd;
	bool ok;
	ssize_t ret;

	ksft_print_header();
	ksft_set_plan(10);
	ksft_print_msg("backing=%s\n", use_file ? "ext4" : "raw block");
	if ((ps != 4096 && ps != 16384) || p == MAP_FAILED || q == MAP_FAILED ||
	    fd < 0)
		ksft_exit_fail_msg("setup failed errno=%d\n", errno);
	if (use_file && ftruncate(fd, 32 * 1024 * 1024))
		ksft_exit_fail_msg("ftruncate failed\n");
	fill(p, len);
	memset(q, 0, len);
	ok = pwrite(fd, p + ps, 3 * ps, 0) == (ssize_t)(3 * ps) &&
	     pread(fd, q + ps, 3 * ps, 0) == (ssize_t)(3 * ps) &&
	     check(q + ps, 3 * ps, ps);
	report(ok, ps, "O_DIRECT exact user offsets and bytes");

	vec[0] = (struct iovec){ p + 512, ps - 512 };
	vec[1] = (struct iovec){ NULL, 0 };
	vec[2] = (struct iovec){ p + 5 * ps, ps + 512 };
	vec[3] = (struct iovec){ p + 11 * ps + 512, 512 };
	ok = pwritev(fd, vec, 4, 65536) == (ssize_t)(2 * ps + 512);
	memset(q, 0, 3 * ps);
	ok &= pread(fd, q, 2 * ps + 512, 65536) == (ssize_t)(2 * ps + 512);
	ok &= check(q, ps - 512, 512) &&
	      check(q + ps - 512, ps + 512, 5 * ps) &&
	      check(q + 2 * ps, 512, 11 * ps + 512);
	report(ok, ps, "fragmented vectors and empty segment");

	ok = pwrite(fd, p, len, 4 * 1024 * 1024) == (ssize_t)len;
	memset(q, 0, len);
	ok &= pread(fd, q, len, 4 * 1024 * 1024) == (ssize_t)len &&
	      check(q, len, 0);
	report(ok, ps, "multi-bio two-megabyte transfer");

	ok = !mprotect(p + ps, ps, PROT_NONE);
	ret = pwrite(fd, p, 2 * ps, 8 * 1024 * 1024);
	ok &= use_file ? ret == -1 && errno == EFAULT : ret == (ssize_t)ps;
	ok &= !mprotect(p + ps, ps, PROT_READ | PROT_WRITE);
	if (use_file)
		ok &= pwrite(fd, p, ps, 8 * 1024 * 1024) == (ssize_t)ps;
	memset(q, 0, ps);
	ok &= pread(fd, q, ps, 8 * 1024 * 1024) == (ssize_t)ps &&
	      check(q, ps, 0);
	report(ok, ps, "source fault and subsequent I/O recovery");

	ok = !mprotect(q + ps, ps, PROT_NONE);
	ret = pread(fd, q, 2 * ps, 4 * 1024 * 1024);
	ok &= (use_file ? ret == -1 && errno == EFAULT : ret == (ssize_t)ps) &&
	      check(q, ps, 0);
	ok &= !mprotect(q + ps, ps, PROT_READ | PROT_WRITE);
	report(ok, ps, "partial destination fault");

	ok = !mprotect(p + ps, ps, PROT_NONE);
	/* Only 256 accessible bytes: none can form a 512-byte device block. */
	ret = pwrite(fd, p + ps - 256, 512, 10 * 1024 * 1024);
	ok &= ret == -1 && errno == EFAULT;
	ok &= !mprotect(p + ps, ps, PROT_READ | PROT_WRITE);
	report(ok, ps, "alignment trimming releases all short-fragment pins");

	memfd = memfd_create("dio-shared", 0);
	ok = memfd >= 0 && !ftruncate(memfd, 4 * ps);
	unsigned char *shared = ok ? mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE,
					  MAP_SHARED, memfd, 0) :
				     MAP_FAILED;
	ok &= shared != MAP_FAILED;
	if (ok) {
		memset(shared, 0xa7, 4 * ps);
		ok = pread(fd, shared + ps, 2 * ps, 4 * 1024 * 1024) ==
			     (ssize_t)(2 * ps) &&
		     !msync(shared, 4 * ps, MS_SYNC) &&
		     pread(memfd, q, 4 * ps, 0) == (ssize_t)(4 * ps);
		ok &= check(q + ps, 2 * ps, 0);
		for (size_t i = 0; i < ps; i++)
			ok &= q[i] == 0xa7 && q[3 * ps + i] == 0xa7;
		munmap(shared, 4 * ps);
	}
	if (memfd >= 0)
		close(memfd);
	report(ok, ps, "shared-file destination dirtying and adjacent guards");

	ok = aio_transfer(fd, p + ps, 7 * ps, 12 * 1024 * 1024, true);
	memset(q, 0, 8 * ps);
	ok &= aio_transfer(fd, q + ps, 7 * ps, 12 * 1024 * 1024, false) &&
	      check(q + ps, 7 * ps, ps);
	report(ok, ps, "asynchronous AIO read/write completion");

	memset(q, 0x96, 8 * ps);
	pid_t child = fork();
	int status = 0;
	if (!child) {
		_exit(aio_transfer(fd, q + ps, 7 * ps, 12 * 1024 * 1024,
				   false) &&
				      check(q + ps, 7 * ps, ps) ?
			      0 :
			      1);
	}
	ok = child > 0 && waitpid(child, &status, 0) == child &&
	     WIFEXITED(status) && !WEXITSTATUS(status);
	for (size_t i = 0; i < 8 * ps; i++)
		ok &= q[i] == 0x96;
	report(ok, ps, "writable-pin COW after fork");

	ok = true;
	for (unsigned i = 0; i < 128 && ok; i++) {
		ok = !madvise(q + ps, ps, MADV_DONTNEED) &&
		     pread(fd, q + ps, ps, 4 * 1024 * 1024) == (ssize_t)ps &&
		     check(q + ps, ps, 0);
		ok &= !mprotect(q + 2 * ps, ps, PROT_NONE);
		ok &= pread(fd, q + 2 * ps, ps, 4 * 1024 * 1024) == -1 &&
		      errno == EFAULT;
		ok &= !mprotect(q + 2 * ps, ps, PROT_READ | PROT_WRITE);
	}
	report(ok, ps, "128 reuse and failed-pin cleanup cycles");
	close(fd);
	munmap(p, len);
	munmap(q, len);
	ksft_print_cnts();
	return ksft_get_fail_cnt() ? 1 : 0;
}
int main(int argc, char **argv)
{
	bool ok = true;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 2 &&
	    (!strcmp(argv[1], "--test") || !strcmp(argv[1], "--file")) &&
	    getppid() == 1) {
		use_file = !strcmp(argv[1], "--file");
		return exercise();
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Disposable VM PID1 only\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL))
		return 3;
	for (unsigned disk = 0; disk < 2; disk++) {
		char path[80], serial[64];
		unsigned maj, min;
		snprintf(path, sizeof(path), "/sys/class/block/vd%c/serial",
			 'a' + disk);
		FILE *dev = fopen(path, "r");
		if (!dev || fscanf(dev, "%63s", serial) != 1)
			return 4;
		fclose(dev);
		snprintf(path, sizeof(path), "/sys/class/block/vd%c/dev",
			 'a' + disk);
		dev = fopen(path, "r");
		if (!dev || fscanf(dev, "%u:%u", &maj, &min) != 2)
			return 4;
		fclose(dev);
		const char *node =
			!strcmp(serial, "au4k-raw")  ? "/dev/user4k-test" :
			!strcmp(serial, "au4k-ext4") ? "/dev/user4k-fs" :
						       NULL;
		if (!node || mknod(node, S_IFBLK | 0600, makedev(maj, min)))
			return 5;
	}
	if (mount("/dev/user4k-fs", "/fs", "ext4", 0, NULL))
		return 7;
	for (unsigned mode = 0; mode < 4; mode++) {
		unsigned small = mode & 1;
		pid_t pid = fork();
		int status = 0;
		if (!pid) {
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
				  small ? 4096UL : 0UL, 0UL, 0UL, 0UL))
				_exit(3);
			execl("/init", "/init", mode >= 2 ? "--file" : "--test",
			      NULL);
			_exit(4);
		}
		ok &= pid > 0 && waitpid(pid, &status, 0) == pid &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
	}
	ok &= !umount("/fs");
	printf("%s - direct I/O fixture complete\n", ok ? "ok" : "not ok");
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
