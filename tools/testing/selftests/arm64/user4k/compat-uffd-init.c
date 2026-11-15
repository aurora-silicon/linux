// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                          \
	do {                                                              \
		if (!(x)) {                                               \
			printf("not ok - compat uffd line=%d errno=%d\n", \
			       __LINE__, errno);                          \
			exit(1);                                          \
		}                                                         \
	} while (0)
struct wire {
	uint32_t address, pagesize;
};
static void expect_invalid(int fd, uint64_t dst, uint64_t src, uint64_t len)
{
	struct uffdio_copy c = { .dst = dst, .src = src, .len = len };
	errno = 0;
	CHECK(ioctl(fd, UFFDIO_COPY, &c) == -1 && errno == EINVAL);
}
static void run(unsigned long native, unsigned long expected, int shared)
{
	int sock[2], file = -1, status;
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sock));
	if (shared) {
		file = memfd_create("compat-uffd", 0);
		CHECK(file >= 0 && !ftruncate(file, 3 * expected));
	}
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		char page_arg[24], sock_arg[24], file_arg[24];
		close(sock[0]);
		snprintf(page_arg, sizeof(page_arg), "%lu", expected);
		snprintf(sock_arg, sizeof(sock_arg), "%d", sock[1]);
		snprintf(file_arg, sizeof(file_arg), "%d", file);
		execl("/compat-uffd", "/compat-uffd", page_arg, sock_arg,
		      file_arg, NULL);
		_exit(99);
	}
	close(sock[1]);
	struct wire data = { 0 };
	struct iovec iov = { &data, sizeof(data) };
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = { 0 };
	struct msghdr msg = { .msg_iov = &iov,
			      .msg_iovlen = 1,
			      .msg_control = control.bytes,
			      .msg_controllen = sizeof(control.bytes) };
	CHECK(recvmsg(sock[0], &msg, 0) == sizeof(data));
	CHECK(!(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
	      data.pagesize == expected);
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	CHECK(cmsg && cmsg->cmsg_level == SOL_SOCKET &&
	      cmsg->cmsg_type == SCM_RIGHTS &&
	      cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
	int uffd;
	memcpy(&uffd, CMSG_DATA(cmsg), sizeof(uffd));
	/* Force the source above every possible AArch32 task limit. */
	unsigned char *source =
		mmap((void *)0x1000000000UL, 2 * native, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	CHECK(source != MAP_FAILED && (uintptr_t)source > UINT32_MAX);
	/* Source alignment is independent of both processes' granules. */
	unsigned char *bytes = source + 17;
	for (unsigned leaf = 0; leaf < 3; leaf++) {
		struct pollfd pfd = { .fd = uffd, .events = POLLIN };
		struct uffd_msg event;
		CHECK(poll(&pfd, 1, 10000) == 1 && (pfd.revents & POLLIN));
		CHECK(read(uffd, &event, sizeof(event)) == sizeof(event));
		uint64_t dst = (uint64_t)data.address + leaf * expected;
		CHECK(event.event == UFFD_EVENT_PAGEFAULT &&
		      !event.arg.pagefault.flags &&
		      event.arg.pagefault.address == dst);
		if (!leaf) {
			expect_invalid(uffd, dst, (uintptr_t)bytes, 0);
			expect_invalid(uffd, dst, (uintptr_t)bytes,
				       expected - 1);
			expect_invalid(uffd, dst + 1, (uintptr_t)bytes,
				       expected);
			expect_invalid(uffd, 0x100000000ULL, (uintptr_t)bytes,
				       expected);
			expect_invalid(uffd, dst, UINT64_MAX - expected + 1,
				       expected);
			if (expected > 4096)
				expect_invalid(uffd, dst, (uintptr_t)bytes,
					       4096);
		}
		if (leaf == 1) {
			/* A valid but inaccessible pager source must leave the target missing. */
			CHECK(!mprotect(source, 2 * native, PROT_NONE));
			struct uffdio_copy bad = { .dst = dst,
						   .src = (uintptr_t)bytes,
						   .len = expected };
			errno = 0;
			CHECK(ioctl(uffd, UFFDIO_COPY, &bad) == -1 &&
			      errno == EFAULT && bad.copy == -EFAULT);
			CHECK(!mprotect(source, 2 * native,
					PROT_READ | PROT_WRITE));
		}
		memset(bytes, 0x51 + leaf, expected);
		struct uffdio_copy c = { .dst = dst,
					 .src = (uintptr_t)bytes,
					 .len = expected };
		CHECK(!ioctl(uffd, UFFDIO_COPY, &c) &&
		      c.copy == (long long)expected);
	}
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	CHECK(!munmap(source, 2 * native));
	CHECK(!close(uffd) && !close(sock[0]));
	if (shared)
		CHECK(!close(file));
	printf("ok - native %lu pager high unaligned COPY to AArch32 %lu %s, source failure and target alignment\n",
	       native, expected, shared ? "shared" : "anonymous");
}
int main(void)
{
	if (getpid() != 1)
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(90);
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	char cmdline[2048] = { 0 };
	int fd = open("/proc/cmdline", O_RDONLY);
	CHECK(fd >= 0 && read(fd, cmdline, sizeof(cmdline) - 1) > 0 &&
	      !close(fd));
	unsigned long native = getauxval(AT_PAGESZ);
	unsigned long expected =
		strstr(cmdline, "id_aa64mmfr0.tgran4=f") ? native : 4096;
	run(native, expected, 0);
	run(native, expected, 1);
	puts("COMPAT UFFD PASS");
	reboot(RB_POWER_OFF);
	return 0;
}
