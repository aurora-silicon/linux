// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/kernel-page-flags.h>
#include <linux/userfaultfd.h>
#include <poll.h>
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
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#define CHECK(x)                                                       \
	do {                                                           \
		if (!(x)) {                                            \
			printf("not ok - collapse line=%d errno=%d\n", \
			       __LINE__, errno);                       \
			exit(1);                                       \
		}                                                      \
	} while (0)
struct command {
	uint64_t op, offset, len, value;
};
struct ready {
	uint64_t base, head;
};
static uint64_t entry(const char *path, uint64_t index)
{
	uint64_t value;
	int fd = open(path, O_RDONLY);
	CHECK(fd >= 0 && pread(fd, &value, 8, index * 8) == 8 && !close(fd));
	return value;
}
static uint64_t mapped_pfn(pid_t pid, uintptr_t address, size_t ps)
{
	char path[80];
	snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
	uint64_t e = entry(path, address / ps);
	CHECK(e & (1ULL << 63));
	return e & ((1ULL << 55) - 1);
}
static unsigned char *map(int file, size_t len, size_t align, off_t offset)
{
	void *reserve = mmap(NULL, len + align, PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(reserve != MAP_FAILED);
	uintptr_t addr = ((uintptr_t)reserve + align - 1) & ~(align - 1);
	CHECK(!munmap(reserve, len + align));
	unsigned char *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_FIXED_NOREPLACE, file, offset);
	CHECK(p != MAP_FAILED);
	return p;
}
static int register_range(unsigned char *p, size_t len, int minor)
{
	int fd = syscall(SYS_userfaultfd,
			 O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(fd >= 0);
	struct uffdio_api api = { .api = UFFD_API,
				  .features = minor ? UFFD_FEATURE_MINOR_SHMEM :
						      0 };
	CHECK(!ioctl(fd, UFFDIO_API, &api));
	struct uffdio_register r = {
		.range = { (uintptr_t)p, len },
		.mode = UFFDIO_REGISTER_MODE_MISSING |
			(minor ? UFFDIO_REGISTER_MODE_MINOR : 0)
	};
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &r));
	return fd;
}
static void copy_leaf(int fd, uintptr_t dst, unsigned char value)
{
	unsigned char src[4096];
	memset(src, value, sizeof(src));
	struct uffdio_copy c = { .dst = dst,
				 .src = (uintptr_t)src,
				 .len = sizeof(src) };
	CHECK(!ioctl(fd, UFFDIO_COPY, &c) && c.copy == sizeof(src));
}
static void cpu(unsigned id)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(id, &set);
	CHECK(!sched_setaffinity(0, sizeof(set), &set));
}
static int worker(int file, int sock, size_t native, size_t len)
{
	CHECK(getauxval(AT_PAGESZ) == 4096);
	alarm(90);
	cpu(1);
	unsigned char *p = map(file, len, len, 0);
	unsigned char *q = map(file, len - 4096, len, 4096);
	CHECK(p[0] == 0x61 && q[0] == 0x61);
	uint64_t old_head =
		mapped_pfn(getpid(), (uintptr_t)p, 4096) * 4096 / native;
	CHECK(entry("/proc/kpageflags", old_head) &
	      (1ULL << KPF_COMPOUND_HEAD));
	int fd = register_range(p, len, 1);
	for (size_t i = 4 * native; i < len; i += native)
		copy_leaf(fd, (uintptr_t)p + i, 0x71);
	int pipes[2];
	CHECK(!pipe(pipes));
	struct iovec iov = { p, 4096 };
	CHECK(vmsplice(pipes[1], &iov, 1, 0) == 4096);
	CHECK(!fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			 native, native));
	CHECK(entry("/proc/kpageflags", old_head) &
	      (1ULL << KPF_COMPOUND_HEAD));
	CHECK(!close(pipes[0]) && !close(pipes[1]));
	struct ready r = { (uintptr_t)p, old_head };
	struct iovec v = { &r, sizeof(r) };
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(fd))];
	} control = { 0 };
	struct msghdr msg = { .msg_iov = &v,
			      .msg_iovlen = 1,
			      .msg_control = control.bytes,
			      .msg_controllen = sizeof(control.bytes) };
	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(fd));
	memcpy(CMSG_DATA(c), &fd, sizeof(fd));
	CHECK(sendmsg(sock, &msg, 0) == sizeof(r));
	struct command cmd;
	while (recv(sock, &cmd, sizeof(cmd), 0) == sizeof(cmd)) {
		CHECK(cmd.offset < len && cmd.len <= len - cmd.offset);
		unsigned char value = 0;
		if (!cmd.op)
			break;
		if (cmd.op == 1)
			value = *(volatile unsigned char *)(p + cmd.offset);
		else if (cmd.op == 2)
			CHECK(!fallocate(file,
					 FALLOC_FL_PUNCH_HOLE |
						 FALLOC_FL_KEEP_SIZE,
					 cmd.offset, cmd.len));
		else if (cmd.op == 3) {
			CHECK(cmd.offset >= 4096);
			value = *(volatile unsigned char *)(q + cmd.offset -
							    4096);
		} else if (cmd.op == 4) {
			CHECK(cmd.offset >= 4096);
			memset(q + cmd.offset - 4096, cmd.value, cmd.len);
		} else
			CHECK(0);
		CHECK(send(sock, &value, 1, 0) == 1);
	}
	CHECK(!munmap(q, len - 4096) && !munmap(p, len) && !close(fd) &&
	      !close(file) && !close(sock));
	return 0;
}
static void send_command(int sock, uint64_t op, size_t offset, size_t len)
{
	struct command c = { .op = op, .offset = offset, .len = len };
	CHECK(send(sock, &c, sizeof(c), 0) == sizeof(c));
}
static void reply(int sock, unsigned char expected)
{
	unsigned char value;
	CHECK(recv(sock, &value, 1, 0) == 1 && value == expected);
}
static void fault(int sock, int fd, uintptr_t base, size_t offset, int minor,
		  unsigned char value)
{
	send_command(sock, 1, offset + 17, 0);
	struct pollfd p = { .fd = fd, .events = POLLIN };
	struct uffd_msg m;
	CHECK(poll(&p, 1, 10000) == 1 && (p.revents & POLLIN));
	CHECK(read(fd, &m, sizeof(m)) == sizeof(m));
	CHECK(m.event == UFFD_EVENT_PAGEFAULT &&
	      m.arg.pagefault.address == base + offset &&
	      m.arg.pagefault.flags == (minor ? UFFD_PAGEFAULT_FLAG_MINOR : 0));
	if (minor) {
		struct uffdio_continue c = { .range = { base + offset, 4096 } };
		CHECK(!ioctl(fd, UFFDIO_CONTINUE, &c) && c.mapped == 4096);
	} else
		copy_leaf(fd, base + offset, value);
	reply(sock, value);
}
static unsigned long pmd_mapped(unsigned char *p)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	CHECK(f);
	char *line = NULL;
	size_t cap = 0;
	unsigned long a, b, kb, result = 0;
	int selected = 0;
	while (getline(&line, &cap, f) > 0) {
		if (sscanf(line, "%lx-%lx", &a, &b) == 2)
			selected = a == (uintptr_t)p;
		else if (selected &&
			 sscanf(line, "ShmemPmdMapped: %lu kB", &kb) == 1)
			result = kb * 1024;
	}
	free(line);
	fclose(f);
	return result;
}
static int collapse(unsigned char *p, size_t len)
{
	int ret;
	unsigned tries = 0;
	do {
		errno = 0;
		ret = madvise(p, len, MADV_COLLAPSE);
		if (!ret || errno != EAGAIN)
			break;
		usleep(20000);
	} while (++tries < 10);
	printf("# collapse ret=%d errno=%d PMD-mapped=%lu\n", ret,
	       ret ? errno : 0, pmd_mapped(p));
	return ret;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 6 && !strcmp(argv[1], "--worker"))
		return worker(atoi(argv[2]), atoi(argv[3]),
			      strtoul(argv[4], NULL, 10),
			      strtoul(argv[5], NULL, 10));
	if (getpid() != 1 || argc != 1)
		return 2;
	alarm(120);
	cpu(0);
	CHECK(!mount("proc", "/proc", "proc", 0, NULL) &&
	      !mount("sysfs", "/sys", "sysfs", 0, NULL));
	CHECK(!mount("tmpfs", "/tmp", "tmpfs", 0,
		     "huge=within_size,size=256m"));
	size_t native = getauxval(AT_PAGESZ), len = 0;
	FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size",
			"r");
	CHECK(f && fscanf(f, "%zu", &len) == 1);
	fclose(f);
	CHECK(native == 16384 && len == 32 * 1024 * 1024);
	int file = open("/tmp/collapse", O_CREAT | O_RDWR | O_TRUNC, 0600);
	CHECK(file >= 0 && !ftruncate(file, 4 * native));
	unsigned char *initial = map(file, 4 * native, 4 * native, 0);
	memset(initial, 0x61, 4 * native);
	CHECK(entry("/proc/kpageflags",
		    mapped_pfn(getpid(), (uintptr_t)initial, native)) &
	      (1ULL << KPF_COMPOUND_HEAD));
	CHECK(!munmap(initial, 4 * native) && !unlink("/tmp/collapse") &&
	      !ftruncate(file, len));
	int sock[2];
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sock));
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		char fd[24], so[24], ns[24], ls[24];
		close(sock[0]);
		snprintf(fd, sizeof(fd), "%d", file);
		snprintf(so, sizeof(so), "%d", sock[1]);
		snprintf(ns, sizeof(ns), "%zu", native);
		snprintf(ls, sizeof(ls), "%zu", len);
		CHECK(!prctl(SET_EXEC_PAGE_SIZE, 4096, 0UL, 0UL, 0UL));
		execl("/init", "/init", "--worker", fd, so, ns, ls, NULL);
		_exit(3);
	}
	close(sock[1]);
	struct ready ready;
	struct iovec v = { &ready, sizeof(ready) };
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = { 0 };
	struct msghdr msg = { .msg_iov = &v,
			      .msg_iovlen = 1,
			      .msg_control = control.bytes,
			      .msg_controllen = sizeof(control.bytes) };
	CHECK(recvmsg(sock[0], &msg, 0) == sizeof(ready) &&
	      !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	CHECK(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS);
	int uffd;
	memcpy(&uffd, CMSG_DATA(c), sizeof(uffd));
	unsigned char *p = map(file, len, len, 0);
	int native_uffd = register_range(p, len, 0);
	CHECK(collapse(p, len) == -1 && errno == EINVAL && pmd_mapped(p) == 0);
	fault(sock[0], uffd, ready.base, 0, 1, 0x61);
	fault(sock[0], uffd, ready.base, len - native, 1, 0x71);
	uint64_t head = mapped_pfn(pid, ready.base, 4096) * 4096 / native;
	CHECK(head != ready.head &&
	      (entry("/proc/kpageflags", head) & (1ULL << KPF_COMPOUND_HEAD)));
	CHECK(mapped_pfn(pid, ready.base + len - native, 4096) * 4096 /
		      native ==
	      head + len / native - 1);
	for (size_t i = 1; i < len / native; i++)
		CHECK(entry("/proc/kpageflags", head + i) &
		      (1ULL << KPF_COMPOUND_TAIL));
	/* Collapse retained the absent native unit and independent fine holes. */
	fault(sock[0], uffd, ready.base, native, 0, 0x92);
	fault(sock[0], uffd, ready.base, 5 * native + 4096, 0, 0xa3);
	puts("ok - PMD cache collapse preserves missing/minor state and neighbour data");
	/* Every native leaf now contains data: coarse exposure may populate zero subunits. */
	CHECK(!collapse(p, len) && pmd_mapped(p) == len);
	fault(sock[0], uffd, ready.base, 6 * native + 4096, 1, 0);
	struct uffdio_copy duplicate = { .dst = ready.base + 6 * native + 8192,
					 .src = (uintptr_t)p,
					 .len = 4096 };
	errno = 0;
	CHECK(ioctl(uffd, UFFDIO_COPY, &duplicate) == -1 && errno == EEXIST);
	puts("ok - native PMD promotion exposes coarse leaf zeros without overwriting fine data");
	/* A new fine hole must invalidate the covering native PMD mapping. */
	send_command(sock[0], 2, 7 * native + 4096, 4096);
	reply(sock[0], 0);
	CHECK(pmd_mapped(p) == 0);
	fault(sock[0], uffd, ready.base, 7 * native + 4096, 0, 0xb4);
	CHECK(p[7 * native] == 0x71 && p[7 * native + 4096] == 0xb4 &&
	      p[7 * native + 8192] == 0);
	CHECK(p[native] == 0x92 && p[5 * native + 4096] == 0xa3);
	puts("ok - fine hole splits PMD alias and preserves populated neighbouring leaves");
	for (unsigned round = 0; round < 8; round++) {
		CHECK(!collapse(p, len) && pmd_mapped(p) == len);
		size_t off = (16 + round) * native + 4096;
		/* Prime CPU0's PMD translation before CPU1 punches the fine hole. */
		CHECK(*(volatile unsigned char *)(p + off) == 0);
		send_command(sock[0], 3, off + 17, 0);
		reply(sock[0], 0);
		send_command(sock[0], 2, off, 4096);
		reply(sock[0], 0);
		CHECK(pmd_mapped(p) == 0);
		if (round % 3) {
			/* Stale native or shifted fine translations would miss promotion. */
			if (round % 3 == 1)
				memset(p + off, 0xc0 + round, 4096);
			else {
				struct command write = { 4, off, 4096,
							 0xc0 + round };
				CHECK(send(sock[0], &write, sizeof(write), 0) ==
				      sizeof(write));
				reply(sock[0], 0);
			}
			struct uffdio_copy collision = { .dst = ready.base +
								off,
							 .src = (uintptr_t)p,
							 .len = 4096 };
			errno = 0;
			CHECK(ioctl(uffd, UFFDIO_COPY, &collision) == -1 &&
			      errno == EEXIST);
			fault(sock[0], uffd, ready.base, off, 1, 0xc0 + round);
		} else
			fault(sock[0], uffd, ready.base, off, 0, 0xc0 + round);
		CHECK(p[off - 4096] == 0x71 && p[off] == 0xc0 + round &&
		      p[off + 4096] == 0);
		send_command(sock[0], 3, off + 17, 0);
		reply(sock[0], 0xc0 + round);
	}
	CHECK(!collapse(p, len) && pmd_mapped(p) == len);
	for (size_t i = 0; i < len; i++) {
		size_t page = i / native, low = i % native;
		unsigned char expected =
			page < 4 ?
				(page == 1 ? (low < 4096 ? 0x92 : 0) : 0x61) :
				(low < 4096 ? 0x71 : 0);
		if (low >= 4096 && low < 8192) {
			if (page == 5)
				expected = 0xa3;
			if (page == 7)
				expected = 0xb4;
			if (page >= 16 && page < 24)
				expected = 0xc0 + page - 16;
		}
		CHECK(p[i] == expected);
	}
	puts("ok - eight cross-CPU PMD cycles, native/shifted-fine writes and complete 32MiB bytes");
	send_command(sock[0], 0, 0, 0);
	int status;
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	CHECK(!close(uffd) && !close(native_uffd) && !munmap(p, len) &&
	      !close(file) && !close(sock[0]));
	puts("SHMEM COLLAPSE CONTRACT PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
