// SPDX-License-Identifier: GPL-2.0-only
/* Disposable two-node guest: policy ranges and native physical placement. */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <errno.h>
#include <linux/mempolicy.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int good, const char *expr, int line)
{
	if (!good) {
		fprintf(stderr, "FAIL line %d: %s errno=%d\n", line, expr,
			errno);
		exit(1);
	}
}
#define CHECK(x) check(!!(x), #x, __LINE__)

static long bind_to(void *addr, unsigned long bytes, int node,
		    unsigned int flags)
{
	unsigned long mask = 1UL << node;

	return syscall(SYS_mbind, addr, bytes, MPOL_BIND, &mask, 64UL, flags);
}

static void policy_at(void *addr, int node)
{
	unsigned long mask = 0;
	int mode = -1;

	CHECK(!syscall(SYS_get_mempolicy, &mode, &mask, 64UL, addr,
		       MPOL_F_ADDR));
	CHECK(mode == MPOL_BIND && mask == (1UL << node));
}

static void node_at(void *addr, int expected)
{
	int node = -1;

	CHECK(!syscall(SYS_get_mempolicy, &node, NULL, 0UL, addr,
		       MPOL_F_NODE | MPOL_F_ADDR));
	if (node != expected)
		fprintf(stderr, "node=%d expected=%d addr=%p\n", node, expected,
			addr);
	CHECK(node == expected);
}

static void exercise(unsigned long native, int baseline)
{
	unsigned long page = getauxval(AT_PAGESZ), allowed = 0;
	unsigned char *reserve, *base, *shared, *alias;
	int mode, fd;

	CHECK(!syscall(SYS_get_mempolicy, &mode, &allowed, 64UL, NULL,
		       MPOL_F_MEMS_ALLOWED));
	CHECK(allowed ==
	      3); /* Require two real memory nodes, not a silent skip. */
	reserve = mmap(NULL, 4 * native, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(reserve != MAP_FAILED);
	base = (void *)(((uintptr_t)reserve + native - 1) & ~(native - 1));
	CHECK(!bind_to(base, 2 * native, 0, 0));
	if (baseline) {
		errno = 0;
		long ret = bind_to(base + page, 1, 1, 0);

		CHECK(page < native ? ret == -1 && errno == EINVAL : ret == 0);
		printf("ok - baseline native=%lu page=%lu mbind=%ld errno=%d\n",
		       native, page, ret, errno);
		CHECK(!munmap(reserve, 4 * native));
		return;
	}
	CHECK(!bind_to(base + page, 1, 1, 0));
	policy_at(base, 0);
	policy_at(base + page, 1);
	if (page < native)
		policy_at(base + 2 * page, 0);
	CHECK(!syscall(SYS_set_mempolicy_home_node, base + page, 1UL, 1UL,
		       0UL));
	errno = 0;
	CHECK(bind_to(base + 1, page, 1, 0) == -1 && errno == EINVAL);
	for (unsigned long off = 0; off < 2 * native; off += page) {
		base[off + 17] = 0x31 + off / page;
		node_at(base + off + 17, off == page ? 1 : 0);
	}
	CHECK(!bind_to(base + page, page, 1, MPOL_MF_STRICT));
	errno = 0;
	CHECK(bind_to(base, 2 * native, 1, MPOL_MF_STRICT) == -1 &&
	      errno == EIO);
	policy_at(base, 0);
	CHECK(!munmap(reserve, 4 * native));

	/* A split anonymous VMA must carry its native interleave index correctly. */
	reserve = mmap(NULL, 4 * native, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(reserve != MAP_FAILED);
	base = (void *)(((uintptr_t)reserve + native - 1) & ~(native - 1));
	unsigned long mask = 3, inset = page < native ? page : 0;

	CHECK(!syscall(SYS_mbind, base + inset, 2 * native - 2 * inset,
		       MPOL_INTERLEAVE, &mask, 64UL, 0));
	for (unsigned long off = inset; off < 2 * native - inset; off += page) {
		base[off + 17] = 0x43;
		node_at(base + off + 17, ((uintptr_t)base + off) / native % 2);
	}
	CHECK(!munmap(reserve, 4 * native));

	/* Moving a fragment moves its native physical owner, retaining bytes. */
	reserve = mmap(NULL, 4 * native, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(reserve != MAP_FAILED);
	base = (void *)(((uintptr_t)reserve + native - 1) & ~(native - 1));
	CHECK(!bind_to(base, 2 * native, 0, 0));
	for (unsigned long off = 0; off < 2 * native; off += page)
		base[off + 17] = 0x51 + off / page;
	CHECK(!bind_to(base + native - page, page, 1,
		       MPOL_MF_MOVE_ALL | MPOL_MF_STRICT));
	for (unsigned long off = 0; off < 2 * native; off += page) {
		CHECK(base[off + 17] == (unsigned char)(0x51 + off / page));
		node_at(base + off + 17, off < native ? 1 : 0);
	}
	CHECK(!bind_to(base, 2 * native, 0, MPOL_MF_MOVE_ALL | MPOL_MF_STRICT));
	for (unsigned long off = 0; off < 2 * native; off += page)
		node_at(base + off + 17, 0);
	CHECK(!munmap(reserve, 4 * native));

	fd = memfd_create("numa-shared", 0);
	CHECK(fd >= 0 && !ftruncate(fd, 2 * native));
	shared = mmap(NULL, 2 * native, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		      0);
	CHECK(shared != MAP_FAILED);
	CHECK(!bind_to(shared, native, 0, 0));
	CHECK(!bind_to(shared + native, native, 1, 0));
	if (page < native) {
		alias = mmap(NULL, 2 * native - page, PROT_READ | PROT_WRITE,
			     MAP_SHARED, fd, page);
		CHECK(alias != MAP_FAILED);
		policy_at(alias, 0);
		policy_at(alias + native - page, 1);
		errno = 0;
		CHECK(bind_to(alias, page, 1, 0) == -1 && errno == EINVAL);
		policy_at(alias, 0);
		policy_at(shared, 0);
		CHECK(!bind_to(alias + native - page, native, 1, 0));
		for (unsigned long off = 0; off < 2 * native - page;
		     off += page) {
			alias[off + 17] = 0x72;
			node_at(alias + off + 17, off + page < native ? 0 : 1);
			CHECK(shared[off + page + 17] == 0x72);
		}
		CHECK(!munmap(alias, 2 * native - page));
	}
	CHECK(!munmap(shared, 2 * native) && !close(fd));
	printf("ok - NUMA native=%lu page=%lu policy, placement, migration and shared offsets\n",
	       native, page);
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ),
		      sizes[] = { 4096, 16384, 65536 };
	int failed = 0, baseline = 0, no4;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 5 && !strcmp(argv[1], "--case")) {
		CHECK(getauxval(AT_PAGESZ) == strtoul(argv[3], NULL, 10));
		exercise(strtoul(argv[2], NULL, 10), atoi(argv[4]));
		return 0;
	}
	CHECK(getpid() == 1);
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	FILE *cmdline = fopen("/proc/cmdline", "r");
	char line[4096];

	CHECK(cmdline && fgets(line, sizeof(line), cmdline));
	baseline = strstr(line, "numa_fixture_baseline=1") != NULL;
	no4 = strstr(line, "id_aa64mmfr0.tgran4=f") != NULL;
	CHECK(!fclose(cmdline));
	for (unsigned int i = 0; i < 3; i++) {
		int status;
		pid_t child;

		if (sizes[i] > native || (no4 && sizes[i] == 4096))
			continue;
		child = fork();
		CHECK(child >= 0);
		if (!child) {
			char n[32], s[32], b[4];

			snprintf(n, sizeof(n), "%lu", native);
			snprintf(s, sizeof(s), "%lu", sizes[i]);
			snprintf(b, sizeof(b), "%d", baseline);
			CHECK(!prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, sizes[i],
				     0UL, 0UL, 0UL));
			execl("/init", "/init", "--case", n, s, b, NULL);
			_exit(2);
		}
		CHECK(waitpid(child, &status, 0) == child);
		failed |= !WIFEXITED(status) || WEXITSTATUS(status);
	}
	if (!baseline) {
		int status;
		pid_t child = fork();

		CHECK(child >= 0);
		if (!child) {
			char n[32], s[32];

			snprintf(n, sizeof(n), "%lu", native);
			snprintf(s, sizeof(s), "%lu", no4 ? native : 4096UL);
			execl("/compat-numa-probe", "/compat-numa-probe", n, s,
			      NULL);
			_exit(2);
		}
		CHECK(waitpid(child, &status, 0) == child);
		failed |= !WIFEXITED(status) || WEXITSTATUS(status);
		printf("A32 NUMA expected-page=%lu status=%d\n",
		       no4 ? native : 4096UL, status);
	}
	printf("NUMA GRANULE %s\n", failed ? "FAIL" : "PASS");
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
