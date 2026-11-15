/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>
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
static int write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	size_t len = strlen(s);
	int ret = write(fd, s, len) == (ssize_t)len ? 0 : -1;
	close(fd);
	return ret;
}
static int enable(const char *event, int on)
{
	char path[160];
	snprintf(path, sizeof(path), TRACE "events/aurora_up/%s/enable", event);
	return write_file(path, on ? "1" : "0");
}
static uint64_t pagemap(void *p, size_t ps)
{
	uint64_t bits = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd >= 0) {
		if (pread(fd, &bits, 8, (uintptr_t)p / ps * 8) != 8)
			bits = 0;
		close(fd);
	}
	return bits;
}
static unsigned records(const char *event)
{
	FILE *f = fopen(TRACE "trace", "r");
	char *line = NULL;
	size_t len = 0;
	unsigned count = 0;
	char match[96];
	snprintf(match, sizeof(match), "%s:", event);
	if (!f)
		return 0;
	while (getline(&line, &len, f) > 0)
		if (strstr(line, match))
			count++;
	free(line);
	fclose(f);
	return count;
}
static int xol_geometry(size_t native)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char *line = NULL;
	size_t len = 0;
	int ok = 0;
	if (!f)
		return 0;
	while (getline(&line, &len, f) > 0)
		if (strstr(line, "[uprobes]")) {
			unsigned long a, b;
			if (sscanf(line, "%lx-%lx", &a, &b) == 2) {
				printf("# XOL %#lx-%#lx native=%zu\n", a, b,
				       native);
				ok = !(a % (unsigned long)native) &&
				     b - a == native;
			}
		}
	free(line);
	fclose(f);
	return ok;
}
static int exercise(size_t ps, size_t native, unsigned shifted)
{
	CHECK(getauxval(AT_PAGESZ) == ps);
	alarm(20);
	printf("# start ps=%zu shifted=%u\n", ps, shifted);
	int fd = memfd_create("aurora-uprobe-code", 0);
	CHECK(fd >= 0);
	size_t length = 8 * ps;
	unsigned char *blob = malloc(length);
	CHECK(blob);
	memset(blob, 0x71, length);
	size_t code_off = 3 * ps + 64, ctr_off = 5 * ps + 32;
	size_t map_off = shifted ? 2 * ps : 0,
	       map_len = shifted ? 2 * ps : 4 * ps;
	uint32_t insns[] = { 0x11001c00, 0xd65f03c0 };
	memcpy(blob + code_off, insns, sizeof(insns));
	memset(blob + ctr_off, 0, 2);
	CHECK(write(fd, blob, length) == (ssize_t)length);
	unsigned char *code = mmap((void *)(0x20000000 + (shifted ? ps : 0)),
				   map_len, PROT_READ | PROT_EXEC,
				   MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd,
				   map_off);
	unsigned char *counter =
		mmap((void *)(0x30000000 + 2 * ps), ps, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 5 * ps);
	unsigned char *other = mmap((void *)0x40000000, ps,
				    PROT_READ | PROT_EXEC,
				    MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	CHECK(code != MAP_FAILED && counter != MAP_FAILED &&
	      other != MAP_FAILED);
	unsigned long (*target)(unsigned long) =
		(void *)(code + code_off - map_off);
	volatile unsigned short *ctr = (void *)(counter + 32);
	CHECK(*ctr == 0);
	CHECK(target(11) == 18);
	/* Occupy the default top-of-address-space XOL hint, forcing a new placement. */
	void *guard = mmap((void *)((1UL << 48) - native), ps, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			   -1, 0);
	CHECK(guard != MAP_FAILED);
	char entry[64], retname[64], cmd[256];
	snprintf(entry, sizeof(entry), "entry_%zu_%u", ps, shifted);
	snprintf(retname, sizeof(retname), "return_%zu_%u", ps, shifted);
	snprintf(cmd, sizeof(cmd),
		 "p:aurora_up/%s /proc/self/fd/%d:0x%zx(0x%zx)\n", entry, fd,
		 code_off, ctr_off);
	CHECK(!write_file(TRACE "uprobe_events", cmd));
	snprintf(cmd, sizeof(cmd),
		 "r:aurora_up/%s /proc/self/fd/%d:0x%zx(0x%zx)\n", retname, fd,
		 code_off, ctr_off);
	CHECK(!write_file(TRACE "uprobe_events", cmd));
	printf("# enabling entry at line %d\n", __LINE__);
	CHECK(!enable(entry, 1));
	printf("# entry enabled\n");
	CHECK(*ctr == 1);
	printf("# enabling return\n");
	CHECK(!enable(retname, 1));
	printf("# return enabled\n");
	CHECK(*ctr == 1);
	uint32_t disk[2];
	CHECK(pread(fd, disk, sizeof(disk), code_off) == sizeof(disk));
	CHECK(!memcmp(disk, insns, sizeof(insns)));
	CHECK(!memcmp(other, blob, ps));
	CHECK(!memcmp(counter, blob + 5 * ps, 32));
	CHECK(!memcmp(counter + 34, blob + 5 * ps + 34, ps - 34));
	int tracefd = open(TRACE "trace", O_WRONLY | O_TRUNC);
	CHECK(tracefd >= 0);
	close(tracefd);
	printf("# invoking probes\n");
	for (unsigned i = 0; i < 16; i++)
		CHECK(target(i) == i + 7);
	printf("# invocations complete\n");
	CHECK(xol_geometry(native));
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		int ok = *ctr == 1;
		for (unsigned i = 0; i < 8; i++)
			ok &= target(100 + i) == 107 + i;
		_exit(ok ? 0 : 3);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
	CHECK(*ctr == 1);
	unsigned entries = records(entry), returns = records(retname);
	printf("# ABI=%zu entry=%u return=%u\n", ps, entries, returns);
	CHECK(entries == 24 && returns == 24);
	CHECK(!enable(entry, 0));
	CHECK(*ctr == 1);
	CHECK(!enable(retname, 0));
	CHECK(*ctr == 0);
	/* Identical restored text is dropped so it can be shared with the file again. */
	CHECK(!(pagemap((void *)target, ps) >> 63));
	CHECK(target(10) == 17);
	CHECK(!memcmp(code, blob + map_off, map_len));
	CHECK(!memcmp(other, blob, ps));
	CHECK(!memcmp(counter, blob + 5 * ps, ps));
	/* A pipe GET must prevent dropping the mapped code slot on removal. */
	printf("# enabling entry at line %d\n", __LINE__);
	CHECK(!enable(entry, 1));
	printf("# entry enabled\n");
	CHECK(*ctr == 1);
	int pipefd[2];
	CHECK(!pipe(pipefd));
	struct iovec iov = { .iov_base =
				     (void *)((uintptr_t)target & ~(ps - 1)),
			     .iov_len = ps };
	CHECK(vmsplice(pipefd[1], &iov, 1, 0) == (ssize_t)ps);
	CHECK(!enable(entry, 0));
	CHECK(*ctr == 0);
	CHECK(pagemap((void *)target, ps) >> 63);
	unsigned char *held = malloc(ps);
	CHECK(held);
	CHECK(read(pipefd[0], held, ps) == (ssize_t)ps);
	CHECK(!memcmp(held, blob + (code_off & ~(ps - 1)), ps));
	free(held);
	close(pipefd[0]);
	close(pipefd[1]);
	printf("# enabling entry at line %d\n", __LINE__);
	CHECK(!enable(entry, 1));
	printf("# entry enabled\n");
	CHECK(!enable(entry, 0));
	CHECK(*ctr == 0);
	/* Native folio-wide references may defer this optional optimization. */
	if (ps < native)
		CHECK(!(pagemap((void *)target, ps) >> 63));
	CHECK(target(20) == 27);
	snprintf(cmd, sizeof(cmd), "-:aurora_up/%s\n", entry);
	CHECK(!write_file(TRACE "uprobe_events", cmd));
	snprintf(cmd, sizeof(cmd), "-:aurora_up/%s\n", retname);
	CHECK(!write_file(TRACE "uprobe_events", cmd));
	CHECK(!munmap(code, map_len));
	CHECK(!munmap(guard, ps));
	CHECK(!munmap(counter, ps));
	CHECK(!munmap(other, ps));
	close(fd);
	free(blob);
	printf("ok - %zuK uprobe/uretprobe, byte offsets, counter, fork, restore and file sharing\n",
	       ps / 1024);
	return 0;
}
static int wait_bounded(pid_t child, int *status)
{
	for (unsigned i = 0; i < 250; i++) {
		pid_t got = waitpid(child, status, WNOHANG);
		if (got == child)
			return 1;
		if (got < 0)
			return 0;
		if (i == 50 || i == 200) {
			char path[64], buf[4096];
			snprintf(path, sizeof(path), "/proc/%d/stack", child);
			int fd = open(path, O_RDONLY);
			ssize_t n = fd < 0 ? -1 :
					     read(fd, buf, sizeof(buf) - 1);
			if (fd >= 0)
				close(fd);
			if (n > 0) {
				buf[n] = 0;
				printf("# child %d stack:\n%s", child, buf);
			} else
				printf("# child %d stack unavailable errno=%d\n",
				       child, errno);
		}
		if (i == 200) {
			printf("# killing stalled child %d\n", child);
			kill(child, SIGKILL);
		}
		usleep(100000);
	}
	return 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 5 && !strcmp(argv[1], "--exercise")) {
		if (getppid() != 1)
			return 2;
		return exercise(strtoul(argv[2], NULL, 10),
				strtoul(argv[3], NULL, 10),
				strtoul(argv[4], NULL, 10));
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
	for (unsigned run = 0; run < (sizes[0] > 16384 ? 6U : 4U); run++) {
		unsigned i = run / 2, shifted = run & 1;
		pid_t c = fork();
		int status = 0;
		if (!c) {
			char expected[24], native[24], mode[8];
			snprintf(mode, sizeof(mode), "%u", shifted);
			snprintf(expected, sizeof(expected), "%zu", sizes[i]);
			snprintf(native, sizeof(native), "%zu", sizes[0]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(4);
			execl("/init", "/init", "--exercise", expected, native,
			      mode, NULL);
			_exit(5);
		}
		int ok = c > 0 && wait_bounded(c, &status) &&
			 WIFEXITED(status) && !WEXITSTATUS(status);
		printf("%s - ABI %zuK uprobes shifted=%u status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, shifted, status);
		fail += !ok;
	}
done:
	printf("UPROBES %s\n", fail ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
