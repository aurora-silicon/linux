// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
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
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define CHECK(x)                                                         \
	do {                                                             \
		if (!(x)) {                                              \
			printf("not ok - shmem hole line=%d errno=%d\n", \
			       __LINE__, errno);                         \
			exit(1);                                         \
		}                                                        \
	} while (0)
struct access {
	volatile unsigned char *p;
	unsigned char value;
};
static void *read_byte(void *opaque)
{
	struct access *a = opaque;
	a->value = *a->p;
	return NULL;
}
static uint64_t pagemap(pid_t pid, uintptr_t address, size_t granule)
{
	char name[80];
	uint64_t entry;
	snprintf(name, sizeof(name), "/proc/%d/pagemap", pid);
	int fd = open(name, O_RDONLY);
	CHECK(fd >= 0 && pread(fd, &entry, 8, address / granule * 8) == 8);
	CHECK(!close(fd));
	return entry;
}
static void fill(int fd, unsigned char *dst, unsigned char *src, size_t ps)
{
	struct uffdio_copy c = { .dst = (uintptr_t)dst,
				 .src = (uintptr_t)src,
				 .len = ps };
	CHECK(!ioctl(fd, UFFDIO_COPY, &c) && c.copy == (long long)ps);
}
static void resolve(int fd, unsigned char *p, size_t ps, int minor,
		    unsigned char value)
{
	struct access a = { .p = p + 17 };
	pthread_t thread;
	CHECK(!pthread_create(&thread, NULL, read_byte, &a));
	struct pollfd pollfd = { .fd = fd, .events = POLLIN };
	struct uffd_msg msg;
	CHECK(poll(&pollfd, 1, 10000) == 1 && (pollfd.revents & POLLIN));
	CHECK(read(fd, &msg, sizeof(msg)) == sizeof(msg));
	CHECK(msg.event == UFFD_EVENT_PAGEFAULT &&
	      msg.arg.pagefault.address == (uintptr_t)p &&
	      msg.arg.pagefault.flags ==
		      (minor ? UFFD_PAGEFAULT_FLAG_MINOR : 0));
	if (minor) {
		struct uffdio_continue c = { .range = { (uintptr_t)p, ps } };
		CHECK(!ioctl(fd, UFFDIO_CONTINUE, &c) &&
		      c.mapped == (long long)ps);
	} else {
		unsigned char *source = malloc(ps);
		CHECK(source);
		memset(source, value, ps);
		fill(fd, p, source, ps);
		free(source);
	}
	CHECK(!pthread_join(thread, NULL) && a.value == value);
}
struct command {
	uint64_t op, offset, len, value;
};
static int alias_worker(int file, int input, int output, size_t len,
			size_t native)
{
	CHECK(getauxval(AT_PAGESZ) == native);
	unsigned char *p =
		mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	CHECK(p != MAP_FAILED);
	/* Establish every coarse PTE before the fine process punches its hole. */
	for (size_t i = 0; i < len; i++)
		CHECK(p[i] == 0x61);
	uint64_t base = (uintptr_t)p;
	CHECK(write(output, &base, sizeof(base)) == sizeof(base));
	struct command cmd;
	while (read(input, &cmd, sizeof(cmd)) == sizeof(cmd)) {
		CHECK(cmd.offset <= len && cmd.len <= len - cmd.offset);
		if (!cmd.op)
			break;
		if (cmd.op == 2)
			memset(p + cmd.offset, cmd.value, cmd.len);
		for (size_t i = 0; i < cmd.len; i++)
			CHECK(p[cmd.offset + i] == cmd.value);
		CHECK(write(output, "y", 1) == 1);
	}
	CHECK(!munmap(p, len));
	return 0;
}
static void tell(int input, int output, uint64_t op, size_t offset, size_t len,
		 unsigned value)
{
	struct command cmd = { op, offset, len, value };
	char reply;
	CHECK(write(input, &cmd, sizeof(cmd)) == sizeof(cmd));
	if (op)
		CHECK(read(output, &reply, 1) == 1 && reply == 'y');
}
static unsigned long swapouts(void)
{
	FILE *f = fopen("/proc/vmstat", "r");
	char key[80];
	unsigned long value, result = 0;
	CHECK(f);
	while (fscanf(f, "%79s %lu", key, &value) == 2)
		if (!strcmp(key, "pswpout"))
			result = value;
	fclose(f);
	return result;
}
static unsigned long mapping_swap(void *address)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	char *line = NULL;
	size_t cap = 0;
	unsigned long start, end, value, result = 0;
	int selected = 0;
	CHECK(f);
	while (getline(&line, &cap, f) > 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2)
			selected = start == (uintptr_t)address;
		else if (selected && sscanf(line, "Swap: %lu kB", &value) == 1)
			result = value;
	}
	free(line);
	fclose(f);
	return result;
}
static void swapped_missing(size_t ps, size_t native)
{
	int file = memfd_create("partial-uffd-swap", 0);
	CHECK(file >= 0 && !ftruncate(file, native));
	unsigned char *p =
		mmap(NULL, native, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	unsigned char *source = malloc(ps);
	CHECK(p != MAP_FAILED && source);
	memset(source, 0x71, ps);
	int fd = syscall(SYS_userfaultfd,
			 O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(fd >= 0);
	struct uffdio_api api = { .api = UFFD_API,
				  .features = UFFD_FEATURE_MINOR_SHMEM };
	CHECK(!ioctl(fd, UFFDIO_API, &api));
	struct uffdio_register reg = { .range = { (uintptr_t)p, native },
				       .mode = UFFDIO_REGISTER_MODE_MISSING |
					       UFFDIO_REGISTER_MODE_MINOR };
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &reg));
	fill(fd, p, source, ps);
	unsigned long before = swapouts(), usage = 0, written;
	unsigned tries;
	for (tries = 0; tries < 100; tries++) {
		CHECK(!madvise(p, native, MADV_PAGEOUT));
		usleep(20000);
		usage = mapping_swap(p);
		if (usage >= native / 1024 && swapouts() > before)
			break;
	}
	written = swapouts() - before;
	CHECK(usage >= native / 1024 && written > 0);
	/* The selected cache index is swapped; its unfilled neighbour stays missing. */
	resolve(fd, p + ps, ps, 0, 0x92);
	resolve(fd, p, ps, 1, 0x71);
	for (size_t i = 0; i < ps; i++)
		CHECK(p[i] == 0x71 && p[ps + i] == 0x92);
	CHECK(mapping_swap(p) == 0);
	printf("ok - shared uffd %zuK on %zuK swapped cache=%lukB writes=%lu attempts=%u missing COPY preserves neighbour\n",
	       ps / 1024, native / 1024, usage, written, tries + 1);
	CHECK(!close(fd) && !munmap(p, native) && !close(file));
	free(source);
}
static int worker(size_t ps, size_t native)
{
	CHECK(getauxval(AT_PAGESZ) == ps);
	alarm(60);
	if (getenv("SHMEM_HOLE_SWAP") && ps < native)
		swapped_missing(ps, native);
	size_t len = 4 * native;
	int file = memfd_create("hole-contract", 0);
	CHECK(file >= 0 && !ftruncate(file, len));
	unsigned char *source = malloc(len);
	CHECK(source);
	memset(source, 0x61, len);
	CHECK(pwrite(file, source, len, 0) == (ssize_t)len);
	unsigned char *p =
		mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	unsigned char *q = mmap(NULL, len - ps, PROT_READ | PROT_WRITE,
				MAP_SHARED, file, ps);
	unsigned char *cow =
		mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0);
	CHECK(p != MAP_FAILED && q != MAP_FAILED && cow != MAP_FAILED);
	for (size_t i = 0; i < len; i += ps)
		CHECK(p[i] == 0x61);
	for (size_t i = 0; i < len - ps; i += ps)
		CHECK(q[i] == 0x61);
	memset(cow + ps, 0xe1, ps);
	int commands[2], replies[2];
	CHECK(!pipe(commands) && !pipe(replies));
	pid_t alias = fork();
	CHECK(alias >= 0);
	if (!alias) {
		char f[24], in[24], out[24], size[24], ns[24];
		close(commands[1]);
		close(replies[0]);
		snprintf(f, sizeof(f), "%d", file);
		snprintf(in, sizeof(in), "%d", commands[0]);
		snprintf(out, sizeof(out), "%d", replies[1]);
		snprintf(size, sizeof(size), "%zu", len);
		snprintf(ns, sizeof(ns), "%zu", native);
		CHECK(!prctl(SET_EXEC_PAGE_SIZE, native, 0UL, 0UL, 0UL));
		execl("/init", "/init", "--alias", f, in, out, size, ns, NULL);
		_exit(3);
	}
	close(commands[0]);
	close(replies[1]);
	uint64_t alias_base;
	CHECK(read(replies[0], &alias_base, sizeof(alias_base)) ==
	      sizeof(alias_base));
	int fd = syscall(SYS_userfaultfd,
			 O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(fd >= 0);
	struct uffdio_api api = { .api = UFFD_API,
				  .features = UFFD_FEATURE_MINOR_SHMEM };
	CHECK(!ioctl(fd, UFFDIO_API, &api));
	struct uffdio_register reg = { .range = { (uintptr_t)p, len },
				       .mode = UFFDIO_REGISTER_MODE_MISSING |
					       UFFDIO_REGISTER_MODE_MINOR };
	CHECK(!ioctl(fd, UFFDIO_REGISTER, &reg));
	CHECK(pagemap(alias, alias_base + ps, native) & (1ULL << 63));
	CHECK(!fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, ps,
			 ps));
	CHECK(!(pagemap(getpid(), (uintptr_t)p + ps, ps) & (1ULL << 63)));
	CHECK(!(pagemap(getpid(), (uintptr_t)q, ps) & (1ULL << 63)));
	CHECK(!(pagemap(alias, alias_base + ps, native) & (1ULL << 63)));
	/* Fine neighbour PTEs survive, while COW retains its independent bytes. */
	CHECK(pagemap(getpid(), (uintptr_t)p, ps) & (1ULL << 63));
	CHECK(pagemap(getpid(), (uintptr_t)p + 2 * ps, ps) & (1ULL << 63));
	CHECK(p[0] == 0x61 && p[2 * ps] == 0x61 && cow[ps] == 0xe1);
	resolve(fd, p + ps, ps, 0, 0xa2);
	CHECK(q[0] == 0xa2 && cow[ps] == 0xe1);
	tell(commands[1], replies[0], 1, ps, ps, 0xa2);
	tell(commands[1], replies[0], 1, 0, ps, 0x61);
	tell(commands[1], replies[0], 1, 2 * ps, ps, 0x61);
	/* Refaulting a coarse writable alias makes its covered units populated. */
	CHECK(!fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, ps,
			 ps));
	tell(commands[1], replies[0], 2, ps, ps, 0xb7);
	struct uffdio_copy collision = { .dst = (uintptr_t)p + ps,
					 .src = (uintptr_t)source,
					 .len = ps };
	errno = 0;
	CHECK(ioctl(fd, UFFDIO_COPY, &collision) == -1 && errno == EEXIST);
	resolve(fd, p + ps, ps, 1, 0xb7);
	printf("ok - shmem hole %zuK on %zuK precise PTEs, shifted alias, COW, coarse revoke/refault and COPY collision\n",
	       ps / 1024, native / 1024);
	/* Stop the alias before resize, so it cannot intentionally promote holes. */
	tell(commands[1], replies[0], 0, 0, 0, 0);
	close(commands[1]);
	close(replies[0]);
	int status;
	CHECK(waitpid(alias, &status, 0) == alias && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	size_t cut = 2 * ps + ps / 2;
	CHECK(!ftruncate(file, cut) && !ftruncate(file, len));
	/* Partial final user leaf remains valid, with only truncated bytes zero. */
	if (!(pagemap(getpid(), (uintptr_t)p + 2 * ps, ps) & (1ULL << 63)))
		resolve(fd, p + 2 * ps, ps, 1, 0x61);
	CHECK(p[2 * ps] == 0x61 && p[cut - 1] == 0x61 && p[cut] == 0 &&
	      p[3 * ps - 1] == 0);
	resolve(fd, p + 3 * ps, ps, 0, 0xc3);
	CHECK(q[2 * ps] == 0xc3);
	CHECK(cow[ps] == 0xe1);
	printf("ok - shmem resize %zuK on %zuK partial tail retained, shrink/regrow missing leaf refilled\n",
	       ps / 1024, native / 1024);
	CHECK(!close(fd) && !munmap(p, len) && !munmap(q, len - ps) &&
	      !munmap(cow, len) && !close(file));
	free(source);
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 7 && !strcmp(argv[1], "--alias"))
		return alias_worker(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
				    strtoul(argv[5], NULL, 10),
				    strtoul(argv[6], NULL, 10));
	if (argc == 4 && !strcmp(argv[1], "--worker"))
		return worker(strtoul(argv[2], NULL, 10),
			      strtoul(argv[3], NULL, 10));
	int swap_test = argc == 2 && !strcmp(argv[1], "--swap");
	if (getpid() != 1 || (argc != 1 && !swap_test))
		return 2;
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	if (swap_test) {
		struct stat st;
		CHECK(!mount("devtmpfs", "/dev", "devtmpfs", 0, NULL));
		for (unsigned i = 0; i < 100 && stat("/dev/vda", &st); i++)
			usleep(20000);
		CHECK(!stat("/dev/vda", &st) && S_ISBLK(st.st_mode) &&
		      !swapon("/dev/vda", 0));
		CHECK(!setenv("SHMEM_HOLE_SWAP", "1", 1));
	}
	unsigned long native = getauxval(AT_PAGESZ),
		      sizes[] = { native, 4096, 16384 };
	int failed = 0;
	for (unsigned i = 0; i < (native == 65536 ? 3U : 2U); i++) {
		pid_t pid = fork();
		CHECK(pid >= 0);
		if (!pid) {
			char ps[24], ns[24];
			snprintf(ps, sizeof(ps), "%lu", sizes[i]);
			snprintf(ns, sizeof(ns), "%lu", native);
			CHECK(!prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL,
				     0UL));
			execl("/init", "/init", "--worker", ps, ns, NULL);
			_exit(3);
		}
		int status = 0;
		int ok = waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
			 !WEXITSTATUS(status);
		printf("%s - shmem hole ABI %luK status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		failed |= !ok;
	}
	if (swap_test)
		CHECK(!swapoff("/dev/vda"));
	printf("SHMEM HOLE CONTRACT %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
