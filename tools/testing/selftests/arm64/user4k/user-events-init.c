/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/user_events.h>
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
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define SET_EXEC_PAGE_SIZE 0x41555001
#define TRACE "/sys/kernel/tracing/"
#define CHECK(c)                                                            \
	do {                                                                \
		if (!(c)) {                                                 \
			printf("FAIL line %d: %s errno=%d\n", __LINE__, #c, \
			       errno);                                      \
			return 1;                                           \
		}                                                           \
	} while (0)

static int toggle(const char *name, int enabled)
{
	char path[192];
	snprintf(path, sizeof(path), TRACE "events/user_events/%s/enable",
		 name);
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int ret = write(fd, enabled ? "1" : "0", 1) == 1 ? 0 : -1;
	close(fd);
	return ret;
}
static uint64_t value(void *p, unsigned bytes)
{
	return bytes == 4 ? __atomic_load_n((uint32_t *)p, __ATOMIC_ACQUIRE) :
			    __atomic_load_n((uint64_t *)p, __ATOMIC_ACQUIRE);
}
static int wait_value(void *p, unsigned bytes, uint64_t expected)
{
	for (unsigned i = 0; i < 3000; i++) {
		if (value(p, bytes) == expected)
			return 1;
		usleep(1000);
	}
	return 0;
}
static int check_bytes(unsigned char *p, size_t size, unsigned char fill,
		       void *word, unsigned bytes)
{
	for (size_t i = 0; i < size; i++)
		if ((!word || (uintptr_t)(p + i) < (uintptr_t)word ||
		     (uintptr_t)(p + i) >= (uintptr_t)word + bytes) &&
		    p[i] != fill) {
			printf("byte mismatch at %zu got=%#x expected=%#x\n", i,
			       p[i], fill);
			return 0;
		}
	return 1;
}
static int event_recorded(const char *name)
{
	char buf[16384];
	size_t n = 0;
	int fd = open(TRACE "trace", O_RDONLY);
	if (fd < 0)
		return 0;
	while (n < sizeof(buf) - 1) {
		ssize_t got = read(fd, buf + n, sizeof(buf) - 1 - n);
		if (got < 0) {
			close(fd);
			return 0;
		}
		if (!got)
			break;
		n += got;
	}
	close(fd);
	buf[n] = 0;
	char *record = strstr(buf, name),
	     *payload = record ? strstr(record, "payload=") : NULL;
	int ok = payload && strtoul(payload + 8, NULL, 0) == 0x12345678;
	if (!ok)
		printf("trace mismatch:\n%s\n", buf);
	return ok;
}
static int exercise_case(size_t ps, unsigned bytes, unsigned shared)
{
	const size_t len = 4 * ps;
	unsigned char *src, *dst;
	int memfd = -1;
	if (shared) {
		memfd = memfd_create("user-event-bitmap", 0);
		CHECK(memfd >= 0);
		CHECK(!ftruncate(memfd, len));
	}
	src = mmap((void *)0x20000000, len, PROT_READ | PROT_WRITE,
		   (shared ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS) |
			   MAP_FIXED_NOREPLACE,
		   memfd, 0);
	dst = mmap((void *)0x30000000, len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	CHECK(src != MAP_FAILED && dst != MAP_FAILED);
	for (unsigned i = 0; i < 4; i++)
		memset(src + i * ps, 0x61 + i, ps);
	memset(dst, 0x91, len);
	CHECK(mremap(src + 3 * ps, ps, ps, MREMAP_MAYMOVE | MREMAP_FIXED,
		     dst + ps) == dst + ps);
	unsigned char *leaf = dst + ps;
	void *word = leaf + ps - bytes;
	uint64_t mask = bytes == 4 ? 1ULL << 31 : 1ULL << 63;
	memset(word, 0, bytes);
	char name[80], spec[120];
	snprintf(name, sizeof(name), "aurora_ue_%zu_%u_%u", ps, bytes, shared);
	snprintf(spec, sizeof(spec), "%s u32 payload", name);
	int tracefd = open(TRACE "trace", O_WRONLY | O_TRUNC);
	CHECK(tracefd >= 0);
	close(tracefd);
	int fd = open(TRACE "user_events_data", O_RDWR);
	CHECK(fd >= 0);
	struct user_reg reg = { .size = sizeof(reg),
				.enable_bit = bytes * 8 - 1,
				.enable_size = bytes,
				.enable_addr = (uintptr_t)word,
				.name_args = (uintptr_t)spec };
	CHECK(!ioctl(fd, DIAG_IOCSREG, &reg));
	CHECK(wait_value(word, bytes, 0));
	CHECK(!toggle(name, 1));
	CHECK(wait_value(word, bytes, mask));
	struct {
		uint32_t index, payload;
	} event = { reg.write_index, 0x12345678 };
	CHECK(write(fd, &event, sizeof(event)) == sizeof(event));
	CHECK(event_recorded(name));
	CHECK(!toggle(name, 0));
	CHECK(wait_value(word, bytes, 0));
	/* Every other byte, including the adjacent half of the kernel word, is stable. */
	CHECK(check_bytes(leaf, ps, 0x64, word, bytes));
	CHECK(check_bytes(dst, ps, 0x91, NULL, 0));
	CHECK(check_bytes(dst + 2 * ps, 2 * ps, 0x91, NULL, 0));
	int commands[2], reply[2];
	CHECK(!pipe(commands) && !pipe(reply));
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(commands[1]);
		close(reply[0]);
		char token;
		int ok = 1;
		for (unsigned round = 0; round < 32; round++) {
			if (read(commands[0], &token, 1) != 1) {
				ok = 0;
				break;
			}
			ok = wait_value(word, bytes, token == '1' ? mask : 0) &&
			     check_bytes(leaf, ps, 0x64, word, bytes);
			token = ok ? 'y' : 'n';
			if (write(reply[1], &token, 1) != 1 || !ok)
				break;
		}
		_exit(ok ? 0 : 3);
	}
	close(commands[0]);
	close(reply[1]);
	int ok = 1;
	for (unsigned round = 0; round < 32; round++) {
		int on = !(round & 1);
		char token = on ? '1' : '0';
		if (toggle(name, on) ||
		    !wait_value(word, bytes, on ? mask : 0) ||
		    write(commands[1], &token, 1) != 1 ||
		    read(reply[0], &token, 1) != 1 || token != 'y') {
			ok = 0;
			break;
		}
	}
	close(commands[1]);
	close(reply[0]);
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(ok && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(check_bytes(leaf, ps, 0x64, word, bytes));
	for (unsigned i = 0; i < 3; i++)
		CHECK(check_bytes(src + i * ps, ps, 0x61 + i, NULL, 0));
	/* Force the asynchronous missing-PTE fixup without unregistering. */
	CHECK(!madvise(leaf, ps, MADV_DONTNEED));
	CHECK(!toggle(name, 1));
	CHECK(wait_value(word, bytes, mask));
	CHECK(check_bytes(leaf, ps, shared ? 0x64 : 0, word, bytes));
	CHECK(!toggle(name, 0));
	CHECK(wait_value(word, bytes, 0));
	struct user_unreg unreg = { .size = sizeof(unreg),
				    .disable_bit = reg.enable_bit,
				    .disable_addr = (uintptr_t)word };
	CHECK(!ioctl(fd, DIAG_IOCSUNREG, &unreg));
	close(fd);
	CHECK(!munmap(src, 3 * ps));
	CHECK(!munmap(dst, len));
	if (memfd >= 0)
		close(memfd);
	printf("ok - %zuK %u-bit %s relocated bitmap, adjacent bytes, event write and fork toggles\n",
	       ps / 1024, bytes * 8, shared ? "shared" : "private");
	return 0;
}
static int exercise(size_t expected)
{
	size_t ps = getauxval(AT_PAGESZ);
	CHECK(ps == expected);
	for (unsigned shared = 0; shared < 2; shared++)
		for (unsigned bytes = 4; bytes <= 8; bytes += 4)
			CHECK(!exercise_case(ps, bytes, shared));
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 3 && !strcmp(argv[1], "--exercise")) {
		if (getppid() != 1)
			return 2;
		return exercise(strtoul(argv[2], NULL, 10));
	}
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Disposable VM PID1 only\n");
		return 2;
	}
	int fail = 0;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    mount("tracefs", TRACE, "tracefs", 0, NULL)) {
		perror("mount");
		fail = 1;
		goto done;
	}
	size_t sizes[] = { getauxval(AT_PAGESZ), 4096, 16384 };
	for (unsigned i = 0; i < (sizes[0] > 16384 ? 3U : 2U); i++) {
		pid_t c = fork();
		int status = 0;
		if (!c) {
			char expected[24];
			snprintf(expected, sizeof(expected), "%zu", sizes[i]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(4);
			execl("/init", "/init", "--exercise", expected, NULL);
			_exit(5);
		}
		int ok = c > 0 && waitpid(c, &status, 0) == c &&
			 WIFEXITED(status) && !WEXITSTATUS(status);
		printf("%s - ABI %zuK user events status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		fail += !ok;
	}
done:
	printf("USER EVENTS %s\n", fail ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
