// SPDX-License-Identifier: GPL-2.0-only
/* Disposable PID1: perf ring layout and sampling with per-mm page sizes. */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <errno.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <setjmp.h>
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
#include <sys/wait.h>
#include <unistd.h>

static void check(int ok, const char *expression, int line)
{
	if (!ok) {
		fprintf(stderr, "FAIL line %d: %s errno=%d\n", line, expression,
			errno);
		exit(1);
	}
}

#define CHECK(x) check(!!(x), #x, __LINE__)

static int no4k;

static int wait_child(pid_t child)
{
	int status;
	pid_t got;

	if (child < 0)
		return 1;
	do {
		got = waitpid(child, &status, 0);
	} while (got < 0 && errno == EINTR);
	return got != child || !WIFEXITED(status) || WEXITSTATUS(status);
}

static int event_open(int sampling)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_PAGE_FAULTS,
		.disabled = 1,
		.sample_period = sampling ? 1 : 0,
		.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
			       PERF_SAMPLE_ADDR | PERF_SAMPLE_PERIOD,
	};
	int fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);

	CHECK(fd >= 0);
	return fd;
}

static sigjmp_buf fault_return;
static volatile sig_atomic_t fault_signal;

static void fault_handler(int signal)
{
	fault_signal = signal;
	siglongjmp(fault_return, 1);
}

static void readonly_data(void *address)
{
	struct sigaction action = { .sa_handler = fault_handler }, previous;

	CHECK(!sigemptyset(&action.sa_mask));
	CHECK(!sigaction(SIGBUS, &action, &previous));
	fault_signal = 0;
	if (!sigsetjmp(fault_return, 1))
		*(volatile unsigned char *)address = 0;
	CHECK(fault_signal == SIGBUS);
	CHECK(!sigaction(SIGBUS, &previous, NULL));
}

static void copy_record(void *destination, const unsigned char *ring,
			unsigned long capacity, uint64_t position, size_t size)
{
	unsigned long offset = position & (capacity - 1);
	size_t first = capacity - offset;

	if (first > size)
		first = size;
	memcpy(destination, ring + offset, first);
	memcpy((unsigned char *)destination + first, ring, size - first);
}

static void sampling(unsigned long pages)
{
	unsigned long page = getauxval(AT_PAGESZ), capacity = pages * page;
	unsigned long length = page + capacity;
	int fd = event_open(1);
	struct perf_event_mmap_page *map =
		mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	unsigned char *region, *ring;
	uint64_t tail = 0, samples = 0, wrapped = 0, region_samples = 0;

	CHECK(map != MAP_FAILED);
	CHECK(map->data_offset == page && map->data_size == capacity);
	map->data_tail = 0;
	ring = (unsigned char *)map + map->data_offset;
	for (unsigned long i = 0; i < capacity; i += page)
		CHECK(ring[i] == 0);
	readonly_data(ring);
	region = mmap(NULL, 16 * page, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(region != MAP_FAILED);
	for (unsigned int batch = 0; batch < 10000 && tail < 3 * capacity;
	     batch++) {
		CHECK(!madvise(region, 16 * page, MADV_DONTNEED));
		CHECK(!ioctl(fd, PERF_EVENT_IOC_ENABLE, 0));
		for (unsigned int i = 0; i < 16; i++)
			*(volatile unsigned char *)(region + i * page) = 1;
		CHECK(!ioctl(fd, PERF_EVENT_IOC_DISABLE, 0));
		uint64_t head =
			__atomic_load_n(&map->data_head, __ATOMIC_ACQUIRE);

		CHECK(head >= tail && head - tail <= capacity);
		while (tail < head) {
			struct perf_event_header header;
			struct {
				struct perf_event_header header;
				uint32_t pid, tid;
				uint64_t time, address, period;
			} record;

			copy_record(&header, ring, capacity, tail,
				    sizeof(header));
			CHECK(header.size >= sizeof(header) &&
			      header.size <= head - tail);
			if (header.type == PERF_RECORD_SAMPLE) {
				CHECK(header.size == sizeof(record));
				copy_record(&record, ring, capacity, tail,
					    sizeof(record));
				CHECK(record.pid == (uint32_t)getpid());
				CHECK(record.tid == (uint32_t)getpid());
				if (record.address >= (uintptr_t)region &&
				    record.address <
					    (uintptr_t)region + 16 * page)
					region_samples++;
				CHECK(record.period == 1);
				samples++;
				if ((tail & (capacity - 1)) + header.size >
				    capacity)
					wrapped++;
			} else {
				CHECK(header.type == PERF_RECORD_THROTTLE ||
				      header.type == PERF_RECORD_UNTHROTTLE);
			}
			tail += header.size;
		}
		__atomic_store_n(&map->data_tail, tail, __ATOMIC_RELEASE);
	}
	CHECK(tail >= 3 * capacity && samples && wrapped);
	/* Library/stack faults may also occur while a batch is enabled. */
	CHECK(region_samples > samples * 9 / 10);
	CHECK(!munmap(region, 16 * page));
	CHECK(!close(fd));
	CHECK(map->data_head == tail);
	CHECK(!munmap(map, length));
	printf("ok - perf page=%lu ring=%lu samples=%llu region=%llu wrapped-records=%llu\n",
	       page, capacity, (unsigned long long)samples,
	       (unsigned long long)region_samples, (unsigned long long)wrapped);
}

static void layout(unsigned long native)
{
	unsigned long page = getauxval(AT_PAGESZ);
	int fd = event_open(0);
	struct perf_event_mmap_page *map =
		mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	CHECK(map != MAP_FAILED);
	CHECK(map->data_offset == page && map->data_size == 0);
	map->data_tail = 0;
	CHECK(!munmap(map, page));
	CHECK(!close(fd));
	fd = event_open(0);
	errno = 0;
	CHECK(mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) ==
	      MAP_FAILED);
	CHECK(errno == EINVAL);
	map = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(map != MAP_FAILED);
	struct perf_event_mmap_page *alias =
		mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(alias != MAP_FAILED);
	map->data_tail = 7;
	CHECK(alias->data_tail == 7);
	map->data_tail = 0;
	CHECK(!munmap(alias, 3 * page));
	/* A software event has no AUX provider. Check byte-exact offset routing. */
	map->aux_offset = 3 * page;
	map->aux_size = native;
	errno = 0;
	CHECK(mmap(NULL, native, PROT_READ, MAP_SHARED, fd, map->aux_offset) ==
	      MAP_FAILED);
	CHECK(errno == EOPNOTSUPP);
	unsigned long sizes[] = { 4096, 16384, 65536 };

	for (unsigned int i = 0; i < 3; i++) {
		if (sizes[i] > native || sizes[i] == page ||
		    (no4k && sizes[i] == 4096))
			continue;
		pid_t child = fork();

		if (!child) {
			char f[32], s[32];

			snprintf(f, sizeof(f), "%d", fd);
			snprintf(s, sizeof(s), "%lu", sizes[i]);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, sizes[i], 0UL,
				  0UL, 0UL))
				_exit(2);
			execl("/init", "/init", "--alias", f, s, NULL);
			_exit(3);
		}
		CHECK(!wait_child(child));
	}
	CHECK(map->data_size == 2 * page && map->data_offset == page);
	CHECK(!munmap(map, 3 * page));
	CHECK(!close(fd));
	printf("ok - perf page=%lu metadata, invalid size, aliases, AUX routing\n",
	       page);
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ);
	int failed = 0, compat_only = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	no4k = getenv("PERF_TEST_NO4K") != NULL;
	if (argc == 4 && !strcmp(argv[1], "--alias")) {
		unsigned long page = strtoul(argv[3], NULL, 10);

		CHECK(getauxval(AT_PAGESZ) == page);
		errno = 0;
		CHECK(mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED,
			   atoi(argv[2]), 0) == MAP_FAILED);
		CHECK(errno == EINVAL);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "--case")) {
		native = strtoul(argv[2], NULL, 10);
		CHECK(getauxval(AT_PAGESZ) == strtoul(argv[3], NULL, 10));
		layout(native);
		sampling(1);
		sampling(2);
		sampling(2 * native / getauxval(AT_PAGESZ));
		return 0;
	}
	CHECK(getpid() == 1);
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no4k")) {
			no4k = 1;
			CHECK(!setenv("PERF_TEST_NO4K", "1", 1));
		}
		if (!strcmp(argv[i], "--compat-only"))
			compat_only = 1;
	}
	unsigned long sizes[] = { 4096, 16384, 65536 };

	for (unsigned int i = 0; i < 3; i++) {
		if (compat_only || sizes[i] > native ||
		    (no4k && sizes[i] == 4096))
			continue;
		pid_t child = fork();

		if (!child) {
			char n[32], s[32];

			snprintf(n, sizeof(n), "%lu", native);
			snprintf(s, sizeof(s), "%lu", sizes[i]);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, sizes[i], 0UL,
				  0UL, 0UL))
				_exit(2);
			execl("/init", "/init", "--case", n, s, NULL);
			_exit(3);
		}
		int bad = wait_child(child);

		printf("%s - perf native=%lu process=%lu\n",
		       bad ? "not ok" : "ok", native, sizes[i]);
		failed |= bad;
	}
	if (compat_only)
		CHECK(access("/compat-perf", X_OK) == 0);
	if (access("/compat-perf", X_OK) == 0) {
		pid_t child = fork();

		if (!child) {
			char n[32], s[32];

			snprintf(n, sizeof(n), "%lu", native);
			snprintf(s, sizeof(s), "%lu", no4k ? native : 4096UL);
			execl("/compat-perf", "/compat-perf", n, s, NULL);
			_exit(3);
		}
		int bad = wait_child(child);

		printf("%s - perf AArch32 native=%lu process=%lu\n",
		       bad ? "not ok" : "ok", native, no4k ? native : 4096UL);
		failed |= bad;
	}
	printf("PERF RING GRANULE %s\n", failed ? "FAIL" : "PASS");
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
