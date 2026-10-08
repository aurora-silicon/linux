// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/userfaultfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
#define CG "/sys/fs/cgroup/pressure"
#define CHECK(x)                                                             \
	do {                                                                 \
		if (!(x)) {                                                  \
			printf("not ok - shmem pressure line=%d errno=%d\n", \
			       __LINE__, errno);                             \
			exit(1);                                             \
		}                                                            \
	} while (0)
static void put(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	CHECK(fd >= 0);
	CHECK(write(fd, value, strlen(value)) == (ssize_t)strlen(value) &&
	      !close(fd));
}
static unsigned long long get(const char *path, const char *key)
{
	FILE *f = fopen(path, "r");
	CHECK(f);
	char name[128];
	unsigned long long value, result = 0;
	if (!key)
		CHECK(fscanf(f, "%llu", &result) == 1);
	else
		while (fscanf(f, "%127s %llu", name, &value) == 2)
			if (!strcmp(key, name)) {
				result = value;
				break;
			}
	fclose(f);
	return result;
}
static void copy_leaf(int uffd, unsigned char *dst, unsigned char *src,
		      size_t ps, unsigned char value)
{
	memset(src, value, ps);
	struct uffdio_copy c = { .dst = (uintptr_t)dst,
				 .src = (uintptr_t)src,
				 .len = ps };
	int ret = ioctl(uffd, UFFDIO_COPY, &c);
	if (ret)
		printf("# COPY destination=%p errno=%d transferred=%lld\n", dst,
		       errno, (long long)c.copy);
	CHECK(!ret && c.copy == (long long)ps);
}
static void checkpoint(int out, int in)
{
	char token;
	CHECK(write(out, "R", 1) == 1 && read(in, &token, 1) == 1 &&
	      token == 'G');
}
static int worker(size_t native, size_t ps, int out, int in)
{
	CHECK(getauxval(AT_PAGESZ) == ps);
	alarm(120);
	size_t records = (16UL << 20) / native,
	       stride = ps < native ? native : 2 * native;
	size_t len = records * stride;
	int file = memfd_create("pressure-partial-cache", 0);
	CHECK(file >= 0 && !ftruncate(file, len));
	unsigned char *p =
		mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	unsigned char *source = malloc(stride), *back = malloc(stride);
	CHECK(p != MAP_FAILED && source && back);
	CHECK(!madvise(p, len, MADV_NOHUGEPAGE));
	int uffd = syscall(SYS_userfaultfd,
			   O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	CHECK(uffd >= 0);
	struct uffdio_api api = { .api = UFFD_API };
	CHECK(!ioctl(uffd, UFFDIO_API, &api));
	struct uffdio_register reg = { .range = { (uintptr_t)p, len },
				       .mode = UFFDIO_REGISTER_MODE_MISSING };
	CHECK(!ioctl(uffd, UFFDIO_REGISTER, &reg));
	for (size_t i = 0; i < records; i++)
		copy_leaf(uffd, p + i * stride, source, ps, 0xa1);
	checkpoint(out, in);
	/* Revisit in permuted order, filling holes in reclaimed cache backing. */
	for (size_t i = 0; i < records; i++) {
		size_t slot = (i * 4051) & (records - 1);
		copy_leaf(uffd, p + slot * stride + ps, source, ps, 0xb2);
		for (size_t j = 0; j < ps; j++)
			CHECK(p[slot * stride + j] == 0xa1 &&
			      p[slot * stride + ps + j] == 0xb2);
	}
	checkpoint(out, in);
	for (size_t i = 0; i < records; i += 2) {
		CHECK(!fallocate(file,
				 FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				 i * stride, ps));
		copy_leaf(uffd, p + i * stride, source, ps, 0xc3);
	}
	checkpoint(out, in);
	for (size_t i = 0; i < records; i++) {
		size_t slot = (i * 4051 + 31) & (records - 1);
		CHECK(pread(file, back, stride, slot * stride) ==
		      (ssize_t)stride);
		for (size_t j = 0; j < stride; j++) {
			unsigned char expected =
				j < ps	   ? (slot & 1 ? 0xa1 : 0xc3) :
				j < 2 * ps ? 0xb2 :
					     0;
			CHECK(back[j] == expected);
		}
		struct uffdio_copy duplicate = {
			.dst = (uintptr_t)(p + slot * stride),
			.src = (uintptr_t)source,
			.len = ps
		};
		errno = 0;
		CHECK(ioctl(uffd, UFFDIO_COPY, &duplicate) == -1 &&
		      errno == EEXIST && duplicate.copy == -EEXIST);
	}
	checkpoint(out, in);
	CHECK(!close(uffd) && !munmap(p, len) && !close(file));
	free(source);
	free(back);
	printf("ok - shmem pressure %zuK on %zuK partial-cache refill, holes, duplicate rejection and every byte\n",
	       ps / 1024, native / 1024);
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 6 && !strcmp(argv[1], "--worker"))
		return worker(strtoul(argv[2], NULL, 10),
			      strtoul(argv[3], NULL, 10), atoi(argv[4]),
			      atoi(argv[5]));
	if (getpid() != 1 || argc != 1)
		return 2;
	CHECK(!mount("proc", "/proc", "proc", 0, NULL) &&
	      !mount("sysfs", "/sys", "sysfs", 0, NULL));
	CHECK(!mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) &&
	      !mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL));
	put("/sys/fs/cgroup/cgroup.subtree_control", "+memory");
	CHECK(!mkdir(CG, 0755));
	put(CG "/memory.max", "8388608");
	put(CG "/memory.swap.max", "134217728");
	struct stat st;
	for (unsigned i = 0; i < 100 && stat("/dev/vda", &st); i++)
		usleep(20000);
	CHECK(!swapon("/dev/vda", 0));
	size_t native = getauxval(AT_PAGESZ);
	for (size_t ps = 4096; ps <= native; ps *= 4) {
		int ready[2], go[2];
		CHECK(!pipe(ready) && !pipe(go));
		pid_t pid = fork();
		CHECK(pid >= 0);
		if (!pid) {
			close(ready[0]);
			close(go[1]);
			char n[24], p[24], r[24], g[24];
			snprintf(n, sizeof(n), "%zu", native);
			snprintf(p, sizeof(p), "%zu", ps);
			snprintf(r, sizeof(r), "%d", ready[1]);
			snprintf(g, sizeof(g), "%d", go[0]);
			put(CG "/cgroup.procs", "0");
			CHECK(!prctl(SET_EXEC_PAGE_SIZE, ps, 0, 0, 0));
			execl("/init", "init", "--worker", n, p, r, g, NULL);
			_exit(127);
		}
		close(ready[1]);
		close(go[0]);
		for (unsigned phase = 0; phase < 4; phase++) {
			char token;
			CHECK(read(ready[0], &token, 1) == 1 && token == 'R');
			unsigned long long swap = get(CG "/memory.swap.current",
						      NULL),
					   max = get(CG "/memory.events",
						     "max");
			unsigned long long scan = get(CG "/memory.stat",
						      "pgscan"),
					   steal = get(CG "/memory.stat",
						       "pgsteal");
			printf("# pressure %zuK phase=%u resident=%llu swap=%llu max=%llu scan=%llu steal=%llu\n",
			       ps / 1024, phase,
			       get(CG "/memory.current", NULL), swap, max, scan,
			       steal);
			CHECK(swap && max && scan && steal &&
			      !get(CG "/memory.events", "oom") &&
			      !get(CG "/memory.events", "oom_kill"));
			CHECK(write(go[1], "G", 1) == 1);
		}
		int status;
		CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		      !WEXITSTATUS(status));
		close(ready[0]);
		close(go[1]);
		/* Closed/unmapped file identity must release all of its cache/swap. */
		for (unsigned i = 0;
		     i < 3000 && (get(CG "/memory.swap.current", NULL) ||
				  get(CG "/memory.stat", "shmem"));
		     i++)
			usleep(20000);
		CHECK(!get(CG "/memory.swap.current", NULL) &&
		      !get(CG "/memory.stat", "shmem"));
	}
	CHECK(!swapoff("/dev/vda"));
	puts("SHMEM PRESSURE CONTRACT PASS");
	sync();
	reboot(RB_POWER_OFF);
	return 0;
}
