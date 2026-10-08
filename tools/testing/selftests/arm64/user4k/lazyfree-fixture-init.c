// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
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
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

static unsigned long stat_value(const char *path, const char *key)
{
	char line[256], name[100];
	unsigned long value = 0;
	FILE *f = fopen(path, "re");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%99s %lu", name, &value) == 2 &&
		    !strcmp(name, key))
			break;
	fclose(f);
	return value;
}

static bool all_bytes(const unsigned char *p, size_t len, unsigned char value)
{
	for (size_t i = 0; i < len; i++)
		if (p[i] != value)
			return false;
	return true;
}

static unsigned long lazy_kb(void *address)
{
	FILE *f = fopen("/proc/self/smaps", "re");
	char line[256];
	unsigned long first, last, value = ~0UL;
	bool found = false;

	if (!f)
		return value;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &first, &last) == 2) {
			if (found)
				break;
			found = (uintptr_t)address >= first &&
				(uintptr_t)address < last;
		} else if (found &&
			   sscanf(line, "LazyFree: %lu", &value) == 1) {
			break;
		}
	}
	fclose(f);
	return value;
}

static uint64_t pagemap(void *p, size_t ps)
{
	uint64_t entry = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd < 0 ||
	    pread(fd, &entry, sizeof(entry), (uintptr_t)p / ps * 8) != 8)
		ksft_exit_fail_msg("pagemap: %s\n", strerror(errno));
	close(fd);
	return entry;
}

static bool pageout(void *p, size_t len)
{
	cpu_set_t allowed, one;
	bool ok = true;

	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		return false;
	for (unsigned int i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, &allowed))
			continue;
		CPU_ZERO(&one);
		CPU_SET(i, &one);
		ok &= !sched_setaffinity(0, sizeof(one), &one);
		ok &= !madvise(p, len, MADV_COLD);
		ok &= !madvise(p, len, MADV_PAGEOUT);
	}
	return !sched_setaffinity(0, sizeof(allowed), &allowed) && ok;
}

static bool mark_free(void *p, size_t len)
{
	cpu_set_t allowed, one;
	bool ok = true;

	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		return false;
	for (unsigned int i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, &allowed))
			continue;
		CPU_ZERO(&one);
		CPU_SET(i, &one);
		ok &= !sched_setaffinity(0, sizeof(one), &one);
		ok &= !madvise(p, len, MADV_FREE);
		ok &= !madvise(p, len, MADV_COLD);
	}
	return !sched_setaffinity(0, sizeof(allowed), &allowed) && ok;
}

static int exercise(unsigned long native)
{
	const size_t ps = getauxval(AT_PAGESZ), len = 4 * ps;
	unsigned char *reservation = mmap(NULL, len + 2 * native, PROT_NONE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char *p =
		(void *)(((uintptr_t)reservation + native - 1) & ~(native - 1));
	bool ok;
	unsigned long writes;
	uint64_t entry;

	if (reservation == MAP_FAILED ||
	    mprotect(p, len, PROT_READ | PROT_WRITE))
		ksft_exit_fail_msg("mapping\n");
	ksft_print_header();
	ksft_set_plan(8);
	for (unsigned int i = 0; i < 4; i++)
		memset(p + i * ps, 0x40 + i, ps);
	ok = mark_free(p + ps, ps);
	if (!ok)
		ksft_exit_fail_msg("%zuK MADV_FREE: %s\n", ps / 1024,
				   strerror(errno));
	ksft_test_result(all_bytes(p + ps, ps, 0x41),
			 "%zuK FREE retains bytes before reclaim\n", ps / 1024);
	ksft_test_result(lazy_kb(p) == ps / 1024,
			 "%zuK partial LazyFree accounting\n", ps / 1024);
	ok = pageout(p, len);
	entry = pagemap(p + ps, ps);
	ok &= !(entry & ((1ULL << 63) | (1ULL << 62)));
	ok &= all_bytes(p + ps, ps, 0) && all_bytes(p, ps, 0x40) &&
	      all_bytes(p + 2 * ps, ps, 0x42) &&
	      all_bytes(p + 3 * ps, ps, 0x43);
	ksft_test_result(ok, "%zuK partial FREE reclaims only advised bytes\n",
			 ps / 1024);
	/* Drop old swap/cache state, then test cancellation by a real EL0 write. */
	ok = !madvise(p, len, MADV_DONTNEED);
	memset(p, 0x62, len);
	ok &= mark_free(p + ps, ps);
	memset(p + ps, 0xa5, ps);
	ksft_test_result(lazy_kb(p) == 0,
			 "%zuK dirty leaf no longer reported LazyFree\n",
			 ps / 1024);
	ok &= pageout(p, len);
	ok &= all_bytes(p + ps, ps, 0xa5) && all_bytes(p, ps, 0x62) &&
	      all_bytes(p + 2 * ps, 2 * ps, 0x62);
	ksft_test_result(ok, "%zuK write after FREE cancels quarter discard\n",
			 ps / 1024);
	ok = !madvise(p, len, MADV_DONTNEED);
	memset(p, 0x83, len);
	writes = stat_value("/proc/vmstat", "pswpout");
	ok &= mark_free(p, len) && pageout(p, len);
	for (unsigned int i = 0; i < 4; i++)
		ok &= !(pagemap(p + i * ps, ps) &
			((1ULL << 63) | (1ULL << 62)));
	printf("# full FREE pswpout=%lu->%lu, entries=%#llx %#llx %#llx %#llx\n",
	       writes, stat_value("/proc/vmstat", "pswpout"),
	       (unsigned long long)pagemap(p, ps),
	       (unsigned long long)pagemap(p + ps, ps),
	       (unsigned long long)pagemap(p + 2 * ps, ps),
	       (unsigned long long)pagemap(p + 3 * ps, ps));
	ok &= stat_value("/proc/vmstat", "pswpout") == writes &&
	      all_bytes(p, len, 0);
	ksft_test_result(
		ok, "%zuK fully free backing reclaimed without swap writes\n",
		ps / 1024);
	ok = !madvise(p, len, MADV_DONTNEED);
	memset(p, 0xb7, len);
	ok &= pageout(p, len);
	ok &= !!(pagemap(p + ps, ps) & (1ULL << 62));
	ok &= mark_free(p + ps, ps);
	ok &= !(pagemap(p + ps, ps) & ((1ULL << 63) | (1ULL << 62)));
	ok &= all_bytes(p + ps, ps, 0) && all_bytes(p, ps, 0xb7) &&
	      all_bytes(p + 2 * ps, 2 * ps, 0xb7);
	ksft_test_result(
		ok,
		"%zuK FREE drops selected swap entry without neighbor loss\n",
		ps / 1024);
	{
		int gate[2], status = 0;
		pid_t pid;
		char ready;

		ok = !madvise(p, len, MADV_DONTNEED);
		memset(p, 0xc8, len);
		if (pipe(gate))
			ksft_exit_fail_msg("pipe\n");
		pid = fork();
		if (!pid) {
			close(gate[1]);
			if (read(gate[0], &ready, 1) != 1 ||
			    !all_bytes(p, len, 0xc8))
				_exit(2);
			_exit(0);
		}
		close(gate[0]);
		ok &= mark_free(p + ps, ps) && pageout(p, len);
		ok &= write(gate[1], "x", 1) == 1;
		close(gate[1]);
		ok &= pid > 0 && waitpid(pid, &status, 0) == pid &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
		ksft_test_result(
			ok,
			"%zuK FREE after fork preserves other process data\n",
			ps / 1024);
	}
	munmap(reservation, len + 2 * native);
	ksft_print_cnts();
	return ksft_get_fail_cnt() ? 1 : 0;
}

int main(int argc, char **argv)
{
	bool ok = true;
	unsigned long native = getauxval(AT_PAGESZ);
	unsigned long granules[] = { native, 4096, 16384 };
	unsigned int nr = native > 16384 ? 3 : 2;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 4 && !strcmp(argv[1], "--test") && getppid() == 1) {
		if (getauxval(AT_PAGESZ) != strtoul(argv[2], NULL, 10))
			return 42;
		return exercise(strtoul(argv[3], NULL, 10));
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Run only as PID 1 in a disposable VM\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    swapon("/dev/vda", 0))
		return 3;
	for (unsigned int small = 0; small < nr; small++) {
		pid_t pid = fork();
		int status = 0;

		if (!pid) {
			char size[32], host_size[32];

			snprintf(size, sizeof(size), "%lu", granules[small]);
			snprintf(host_size, sizeof(host_size), "%lu", native);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, granules[small],
				  0UL, 0UL, 0UL))
				_exit(3);
			execl("/init", "/init", "--test", size, host_size,
			      NULL);
			_exit(4);
		}
		ok &= pid > 0 && waitpid(pid, &status, 0) == pid &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
	}
	printf("%s - lazyfree fixture complete\n", ok ? "ok" : "not ok");
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
