// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/kernel-page-flags.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define CHECK(x)                                                        \
	do {                                                            \
		if (!(x)) {                                             \
			printf("not ok - shmem THP line=%d errno=%d\n", \
			       __LINE__, errno);                        \
			exit(1);                                        \
		}                                                       \
	} while (0)
static uint64_t read_entry(const char *path, uint64_t index)
{
	uint64_t value;
	int fd = open(path, O_RDONLY);
	CHECK(fd >= 0 && pread(fd, &value, 8, index * 8) == 8 && !close(fd));
	return value;
}
static uint64_t pfn(void *p, size_t ps)
{
	uint64_t entry = read_entry("/proc/self/pagemap", (uintptr_t)p / ps);
	CHECK(entry & (1ULL << 63));
	return entry & ((1ULL << 55) - 1);
}
static unsigned char *map_aligned(int fd, size_t len)
{
	void *reserve = mmap(NULL, 2 * len, PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(reserve != MAP_FAILED);
	uintptr_t address = ((uintptr_t)reserve + len - 1) & ~(len - 1);
	CHECK(!munmap(reserve, 2 * len));
	unsigned char *p = mmap((void *)address, len, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
	CHECK(p != MAP_FAILED);
	return p;
}
struct reader {
	volatile unsigned char *p;
	unsigned char value;
};
static void *read_byte(void *arg)
{
	struct reader *r = arg;
	r->value = *r->p;
	return NULL;
}
static int target(int file, size_t native, size_t ps, int hold, int zero,
		  int resize, uint64_t head)
{
	CHECK(getauxval(AT_PAGESZ) == ps);
	alarm(30);
	size_t len = 4 * native, target_off = resize ? 2 * native : native;
	size_t cut = native + ps / 2;
	unsigned char *p = map_aligned(file, len);
	for (size_t i = 0; i < len; i += ps)
		CHECK(p[i] == 0x61);
	CHECK(pfn(p, ps) * ps / native == head);
	int pipes[2];
	CHECK(!pipe(pipes));
	if (hold) {
		struct iovec iov = { p, ps };
		CHECK(vmsplice(pipes[1], &iov, 1, 0) == (ssize_t)ps);
	}
	int uffd = syscall(SYS_userfaultfd,
			   O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(uffd >= 0);
	struct uffdio_api api = { .api = UFFD_API };
	CHECK(!ioctl(uffd, UFFDIO_API, &api));
	struct uffdio_register reg = { .range = { (uintptr_t)p, len },
				       .mode = UFFDIO_REGISTER_MODE_MISSING };
	CHECK(!ioctl(uffd, UFFDIO_REGISTER, &reg));
	if (resize)
		CHECK(!ftruncate(file, cut) && !ftruncate(file, len));
	else
		CHECK(!fallocate(file,
				 FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				 native, ps));
	uint64_t flags = read_entry("/proc/kpageflags", head);
	printf("# THP %zuK native=%zuK hold=%d zero=%d resize=%d head_flags=%#llx\n",
	       ps / 1024, native / 1024, hold, zero, resize,
	       (unsigned long long)flags);
	if (hold)
		CHECK((flags &
		       ((1ULL << KPF_COMPOUND_HEAD) | (1ULL << KPF_THP))) ==
		      ((1ULL << KPF_COMPOUND_HEAD) | (1ULL << KPF_THP)));
	struct reader r = { .p = p + target_off + 17 };
	pthread_t thread;
	CHECK(!pthread_create(&thread, NULL, read_byte, &r));
	struct pollfd pollfd = { .fd = uffd, .events = POLLIN };
	struct uffd_msg msg;
	CHECK(poll(&pollfd, 1, 10000) == 1 && (pollfd.revents & POLLIN));
	CHECK(read(uffd, &msg, sizeof(msg)) == sizeof(msg));
	CHECK(msg.event == UFFD_EVENT_PAGEFAULT && !msg.arg.pagefault.flags &&
	      msg.arg.pagefault.address == (uintptr_t)(p + target_off));
	unsigned char *source = malloc(ps);
	CHECK(source);
	memset(source, 0x92, ps);
	int ret;
	struct uffdio_copy invalid = { .dst = (uintptr_t)(p + target_off),
				       .src = 1,
				       .len = ps };
	errno = 0;
	CHECK(ioctl(uffd, UFFDIO_COPY, &invalid) == -1 && errno == EFAULT &&
	      invalid.copy == -EFAULT);
	if (zero) {
		struct uffdio_zeropage z = {
			.range = { (uintptr_t)(p + target_off), ps }
		};
		ret = ioctl(uffd, UFFDIO_ZEROPAGE, &z);
		printf("# ZEROPAGE returned=%d errno=%d bytes=%lld\n", ret,
		       errno, (long long)z.zeropage);
		CHECK(!ret && z.zeropage == (long long)ps);
	} else {
		struct uffdio_copy c = { .dst = (uintptr_t)(p + target_off),
					 .src = (uintptr_t)source,
					 .len = ps };
		ret = ioctl(uffd, UFFDIO_COPY, &c);
		printf("# COPY returned=%d errno=%d bytes=%lld\n", ret, errno,
		       (long long)c.copy);
		CHECK(!ret && c.copy == (long long)ps);
	}
	CHECK(!pthread_join(thread, NULL) && r.value == (zero ? 0 : 0x92));
	memset(source, 0xd4, ps);
	struct uffdio_copy duplicate = { .dst = (uintptr_t)(p + target_off),
					 .src = (uintptr_t)source,
					 .len = ps };
	errno = 0;
	CHECK(ioctl(uffd, UFFDIO_COPY, &duplicate) == -1 && errno == EEXIST &&
	      duplicate.copy == -EEXIST);
	/* File reads verify zeroed tail holes without faulting unfilled UFFD leaves. */
	unsigned char *back = malloc(len);
	CHECK(back);
	CHECK(pread(file, back, len, 0) == (ssize_t)len);
	for (size_t i = 0; i < len; i++) {
		unsigned char expected = i >= target_off &&
							 i < target_off + ps ?
						 (zero ? 0 : 0x92) :
					 resize && i >= cut ? 0 :
							      0x61;
		CHECK(back[i] == expected);
		if (!resize || (i >= target_off && i < target_off + ps))
			CHECK(p[i] == expected);
	}
	free(back);
	if (hold) {
		CHECK(read(pipes[0], source, ps) == (ssize_t)ps);
		for (size_t i = 0; i < ps; i++)
			CHECK(source[i] == 0x61);
	}
	CHECK(!close(pipes[0]) && !close(pipes[1]) && !close(uffd) &&
	      !munmap(p, len) && !close(file));
	free(source);
	printf("ok - THP %zuK on %zuK hold=%d zero=%d resize=%d missing refill and all neighbour bytes\n",
	       ps / 1024, native / 1024, hold, zero, resize);
	return 0;
}
static int create_file(size_t native, uint64_t *head)
{
	int fd = open("/tmp/huge", O_CREAT | O_TRUNC | O_RDWR, 0600);
	CHECK(fd >= 0);
	CHECK(!ftruncate(fd, 4 * native));
	unsigned char *p = map_aligned(fd, 4 * native);
	CHECK(!madvise(p, 4 * native, MADV_HUGEPAGE));
	memset(p, 0x61, 4 * native);
	*head = pfn(p, native);
	uint64_t f = read_entry("/proc/kpageflags", *head);
	printf("# initial head=%llu flags=%#llx VA=%p native=%zu\n",
	       (unsigned long long)*head, (unsigned long long)f, p, native);
	CHECK((f & ((1ULL << KPF_COMPOUND_HEAD) | (1ULL << KPF_THP))) ==
	      ((1ULL << KPF_COMPOUND_HEAD) | (1ULL << KPF_THP)));
	for (unsigned i = 1; i < 4; i++) {
		CHECK(pfn(p + i * native, native) == *head + i);
		CHECK(read_entry("/proc/kpageflags", *head + i) &
		      (1ULL << KPF_COMPOUND_TAIL));
	}
	/* Keep the name until allocation: unlinked mappings use anon-shmem policy. */
	CHECK(!unlink("/tmp/huge") && !munmap(p, 4 * native));
	return fd;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 9 && !strcmp(argv[1], "--target"))
		return target(atoi(argv[2]), strtoul(argv[3], NULL, 10),
			      strtoul(argv[4], NULL, 10), atoi(argv[5]),
			      atoi(argv[6]), atoi(argv[7]),
			      strtoull(argv[8], NULL, 10));
	if (getpid() != 1 || argc != 1)
		return 2;
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	CHECK(!mount("tmpfs", "/tmp", "tmpfs", 0, "huge=within_size,size=32m"));
	size_t native = getauxval(AT_PAGESZ), sizes[] = { native, 4096, 16384 };
	int failed = 0;
	for (unsigned abi = 0; abi < (native == 65536 ? 3U : 2U); abi++)
		for (unsigned hold = 0; hold < 2; hold++)
			for (unsigned zero = 0; zero < 2; zero++)
				for (unsigned resize = 0; resize < 2;
				     resize++) {
					uint64_t head;
					int fd = create_file(native, &head);
					pid_t pid = fork();
					CHECK(pid >= 0);
					if (!pid) {
						char fs[24], ns[24], ps[24],
							hs[24], zs[24], rs[24],
							pfs[24];
						snprintf(fs, sizeof(fs), "%d",
							 fd);
						snprintf(ns, sizeof(ns), "%zu",
							 native);
						snprintf(ps, sizeof(ps), "%zu",
							 sizes[abi]);
						snprintf(hs, sizeof(hs), "%u",
							 hold);
						snprintf(zs, sizeof(zs), "%u",
							 zero);
						snprintf(rs, sizeof(rs), "%u",
							 resize);
						snprintf(pfs, sizeof(pfs),
							 "%llu",
							 (unsigned long long)
								 head);
						CHECK(!prctl(SET_EXEC_PAGE_SIZE,
							     sizes[abi], 0UL,
							     0UL, 0UL));
						execl("/init", "/init",
						      "--target", fs, ns, ps,
						      hs, zs, rs, pfs, NULL);
						_exit(3);
					}
					int status = 0;
					int ok = waitpid(pid, &status, 0) ==
							 pid &&
						 WIFEXITED(status) &&
						 !WEXITSTATUS(status);
					printf("%s - THP ABI %zuK hold=%u zero=%u resize=%u status=%#x\n",
					       ok ? "ok" : "not ok",
					       sizes[abi] / 1024, hold, zero,
					       resize, status);
					failed |= !ok;
					CHECK(!close(fd));
				}
	printf("SHMEM THP CONTRACT %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
