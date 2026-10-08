// SPDX-License-Identifier: GPL-2.0-only
/* Disposable PID1: legacy AIO ring relocation with per-process page sizes. */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <errno.h>
#include <linux/aio_abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct aio_ring_header {
	unsigned int id, nr, head, tail;
	unsigned int magic, compat_features, incompat_features, header_length;
};

static void check(int good, const char *expression, int line)
{
	if (!good) {
		fprintf(stderr, "FAIL line %d: %s errno=%d\n", line, expression,
			errno);
		exit(1);
	}
}
#define CHECK(x) check(!!(x), #x, __LINE__)

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

static void complete(aio_context_t context, struct iocb *request,
		     unsigned long bytes, int event_fd)
{
	struct io_event event;
	struct timespec timeout = { .tv_sec = 5 };
	uint64_t notifications;

	CHECK(syscall(SYS_io_getevents, context, 1, 1, &event, &timeout) == 1);
	CHECK(event.data == request->aio_data &&
	      event.obj == (uintptr_t)request);
	CHECK(event.res == (long long)bytes && event.res2 == 0);
	CHECK(read(event_fd, &notifications, sizeof(notifications)) ==
	      sizeof(notifications));
	CHECK(notifications == 1);
}

static void exercise(unsigned long native, unsigned int requested)
{
	unsigned long page = getauxval(AT_PAGESZ), bytes = 2 * page + 37;
	aio_context_t context = 0;
	int file = memfd_create("aio-granule", 0);
	int event_fd = eventfd(0, EFD_NONBLOCK);
	unsigned char *source, *destination, *reserve, *target;
	struct aio_ring_header *header;
	unsigned long ring_size;
	struct iocb request = { 0 }, *requests[] = { &request };
	unsigned char residency;

	CHECK(file >= 0 && event_fd >= 0);
	CHECK(!ftruncate(file, 4 * page));
	source = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	destination = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(source != MAP_FAILED && destination != MAP_FAILED);
	for (unsigned long i = 0; i < bytes; i++)
		source[page - 19 + i] = i * 17 + i / page;
	CHECK(!syscall(SYS_io_setup, requested, &context));
	header = (void *)(uintptr_t)context;
	CHECK(header->magic == 0xa10a10a1 &&
	      header->header_length == sizeof(*header));
	CHECK(header->nr >= requested);
	ring_size =
		header->header_length + header->nr * sizeof(struct io_event);
	CHECK(ring_size && !(ring_size % native));
	reserve = mmap(NULL, ring_size + 4 * native, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(reserve != MAP_FAILED);
	target = (void *)(((uintptr_t)reserve + native - 1) & ~(native - 1));
	target += native + (page < native ? page : 0);
	target[-1] = 0x51;
	target[ring_size] = 0x72;
	CHECK(!munmap(target, ring_size));
	request.aio_data = 0x12345678;
	request.aio_lio_opcode = IOCB_CMD_PWRITE;
	request.aio_fildes = file;
	request.aio_buf = (uintptr_t)(source + page - 19);
	request.aio_nbytes = bytes;
	request.aio_offset = 7;
	request.aio_flags = IOCB_FLAG_RESFD;
	request.aio_resfd = event_fd;
	CHECK(syscall(SYS_io_submit, context, 1, requests) == 1);
	/* Retain a queued completion while relocating the ring. */
	CHECK(mremap(header, ring_size, ring_size,
		     MREMAP_MAYMOVE | MREMAP_FIXED, target) == target);
	context = (uintptr_t)target;
	header = (void *)target;
	CHECK(header->magic == 0xa10a10a1);
	CHECK(target[-1] == 0x51 && target[ring_size] == 0x72);
	complete(context, &request, bytes, event_fd);
	request.aio_data = 0x87654321;
	request.aio_lio_opcode = IOCB_CMD_PREAD;
	request.aio_buf = (uintptr_t)(destination + page - 11);
	CHECK(syscall(SYS_io_submit, context, 1, requests) == 1);
	complete(context, &request, bytes, event_fd);
	CHECK(!memcmp(source + page - 19, destination + page - 11, bytes));
	CHECK(!syscall(SYS_io_destroy, context));
	errno = 0;
	CHECK(mincore(target, page, &residency) == -1 && errno == ENOMEM);
	CHECK(target[-1] == 0x51 && target[ring_size] == 0x72);
	CHECK(!munmap(reserve, ring_size + 4 * native));
	CHECK(!munmap(source, 4 * page) && !munmap(destination, 4 * page));
	CHECK(!close(file) && !close(event_fd));
	printf("ok - AIO page=%lu requested=%u ring=%lu relocation, I/O, eventfd and teardown\n",
	       page, requested, ring_size);
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ);
	int failed = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 4 && !strcmp(argv[1], "--case")) {
		native = strtoul(argv[2], NULL, 10);
		CHECK(getauxval(AT_PAGESZ) == strtoul(argv[3], NULL, 10));
		exercise(native, 16);
		exercise(native, 4096);
		return 0;
	}
	CHECK(getpid() == 1);
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	unsigned long sizes[] = { 4096, 16384, 65536 };

	for (unsigned int i = 0; i < 3; i++) {
		if (sizes[i] > native)
			continue;
		pid_t child = fork();

		if (!child) {
			char n[32], s[32];

			snprintf(n, sizeof(n), "%lu", native);
			snprintf(s, sizeof(s), "%lu", sizes[i]);
			CHECK(!prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, sizes[i],
				     0UL, 0UL, 0UL));
			execl("/init", "/init", "--case", n, s, NULL);
			_exit(2);
		}
		int bad = wait_child(child);

		printf("%s - AIO native=%lu process=%lu\n",
		       bad ? "not ok" : "ok", native, sizes[i]);
		failed |= bad;
	}
	printf("AIO GRANULE %s\n", failed ? "FAIL" : "PASS");
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
