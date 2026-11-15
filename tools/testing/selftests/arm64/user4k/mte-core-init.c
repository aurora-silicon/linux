// SPDX-License-Identifier: GPL-2.0-only
/* MTE core-file validation: PID 1 and its children in a disposable VM only. */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <asm/hwcap.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PT_AARCH64_MEMTAG_MTE
#define PT_AARCH64_MEMTAG_MTE (PT_LOPROC + 0x2)
#endif
#define TAGGED_BASE 0x200000000UL
#define ZERO_BASE 0x210000000UL
#define EXCLUDED_BASE 0x220000000UL
#define REQUIRE(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s errno=%d\n", __LINE__, #condition, errno); \
		exit(1); \
	} \
} while (0)

static void write_setting(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);

	REQUIRE(fd >= 0);
	REQUIRE(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
	REQUIRE(!close(fd));
}

static unsigned char *tagged_map(unsigned long address, size_t size)
{
	void *p = mmap((void *)address, size, PROT_READ | PROT_WRITE | PROT_MTE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	REQUIRE(p == (void *)address);
	return p;
}

static void set_tags(unsigned char *p, size_t size, unsigned int tag)
{
	for (size_t i = 0; i < size; i += 16) {
		uintptr_t address = (uintptr_t)(p + i) | ((uintptr_t)tag << 56);

		asm volatile(".arch_extension memtag\n\tstg %0, [%0]"
			     : : "r" (address) : "memory");
	}
}

static void child(unsigned long ps)
{
	struct stat self, init;
	struct rlimit limit = { 64UL << 20, 64UL << 20 };
	unsigned char *p;

	REQUIRE(getppid() == 1);
	REQUIRE(!stat("/proc/self/exe", &self) && !stat("/proc/1/exe", &init));
	REQUIRE(self.st_dev == init.st_dev && self.st_ino == init.st_ino);
	REQUIRE(getauxval(AT_PAGESZ) == ps);
	REQUIRE(!setrlimit(RLIMIT_CORE, &limit));
	REQUIRE(!prctl(PR_SET_DUMPABLE, 1UL, 0UL, 0UL, 0UL));
	REQUIRE(!prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE, 0UL, 0UL, 0UL));
	write_setting("/proc/self/coredump_filter", "1");
	p = tagged_map(TAGGED_BASE, 3 * ps);
	for (unsigned int i = 0; i < 3; i++) {
		memset(p + i * ps, 0x40 + i, ps);
		set_tags(p + i * ps, ps, i + 1);
	}
	/* Split metadata at a non-native boundary and retain the physical offset. */
	REQUIRE(!mprotect(p + ps, ps, PROT_READ | PROT_MTE));
	p = tagged_map(ZERO_BASE, ps);
	*p = 0;
	/* Keep anonymous dump eligibility while leaving the leaf absent. */
	REQUIRE(!madvise(p, ps, MADV_DONTNEED));
	p = tagged_map(EXCLUDED_BASE, ps);
	memset(p, 0x55, ps);
	set_tags(p, ps, 7);
	REQUIRE(!madvise(p, ps, MADV_DONTDUMP));
	raise(SIGABRT);
	_exit(2);
}

static void inspect_core(pid_t pid, unsigned long ps)
{
	char path[80];
	Elf64_Ehdr eh;
	struct stat st;
	unsigned int tagged = 0, loads = 0, zero = 0, excluded = 0;
	unsigned char *bytes = malloc(ps);
	int fd;

	REQUIRE(bytes);
	snprintf(path, sizeof(path), "/tmp/core.%d", pid);
	fd = open(path, O_RDONLY);
	REQUIRE(fd >= 0 && !fstat(fd, &st));
	REQUIRE(pread(fd, &eh, sizeof(eh), 0) == sizeof(eh));
	REQUIRE(!memcmp(eh.e_ident, ELFMAG, SELFMAG));
	REQUIRE(eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_type == ET_CORE);
	REQUIRE(eh.e_phentsize == sizeof(Elf64_Phdr) && eh.e_phnum < 4096);
	for (unsigned int n = 0; n < eh.e_phnum; n++) {
		Elf64_Phdr ph;
		unsigned char expected;
		unsigned int index;
		size_t length;

		REQUIRE(pread(fd, &ph, sizeof(ph), eh.e_phoff + n * sizeof(ph)) == sizeof(ph));
		if (ph.p_type != PT_LOAD && ph.p_type != PT_AARCH64_MEMTAG_MTE)
			continue;
		if (ph.p_vaddr >= TAGGED_BASE && ph.p_vaddr < TAGGED_BASE + 3 * ps) {
			index = (ph.p_vaddr - TAGGED_BASE) / ps;
			REQUIRE(ph.p_vaddr == TAGGED_BASE + index * ps && ph.p_memsz == ps);
			if (ph.p_type == PT_LOAD) {
				loads++;
				length = ps;
				expected = 0x40 + index;
			} else {
				tagged++;
				length = ps / 32;
				expected = (index + 1) * 0x11;
			}
		} else if (ph.p_vaddr == ZERO_BASE && ph.p_type == PT_AARCH64_MEMTAG_MTE) {
			zero++;
			length = ps / 32;
			expected = 0;
			REQUIRE(ph.p_memsz == ps);
		} else if (ph.p_vaddr == EXCLUDED_BASE && ph.p_type == PT_AARCH64_MEMTAG_MTE) {
			excluded++;
			REQUIRE(ph.p_filesz == 0 && ph.p_memsz == ps);
			continue;
		} else {
			continue;
		}
		if (ph.p_filesz != length)
			fprintf(stderr, "segment type=%#x address=%#lx size=%lu expected=%zu\n",
				ph.p_type, (unsigned long)ph.p_vaddr,
				(unsigned long)ph.p_filesz, length);
		REQUIRE(ph.p_filesz == length);
		REQUIRE(ph.p_offset <= (uint64_t)st.st_size && length <= st.st_size - ph.p_offset);
		REQUIRE(pread(fd, bytes, length, ph.p_offset) == (ssize_t)length);
		for (size_t i = 0; i < length; i++)
			REQUIRE(bytes[i] == expected);
	}
	REQUIRE(tagged == 3 && loads == 3 && zero == 1 && excluded == 1);
	REQUIRE(!close(fd) && !unlink(path));
	free(bytes);
	printf("MTE CORE CASE PASS page-size=%lu\n", ps);
}

int main(int argc, char **argv)
{
	unsigned long native = getauxval(AT_PAGESZ);
	unsigned long sizes[] = { native, 4096, 16384 };
	unsigned int count = native > 16384 ? 3 : native > 4096 ? 2 : 1;

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	if (argc == 3 && !strcmp(argv[1], "--child")) {
		child(strtoul(argv[2], NULL, 10));
		return 1;
	}
	if (getpid() != 1) {
		fprintf(stderr, "SKIP: disposable guest PID1 only\n");
		return 4;
	}
	REQUIRE(getauxval(AT_HWCAP2) & HWCAP2_MTE);
	mkdir("/proc", 0755);
	mkdir("/tmp", 0700);
	REQUIRE(!mount("proc", "/proc", "proc", 0, NULL));
	REQUIRE(!mount("tmpfs", "/tmp", "tmpfs", 0, "mode=0700"));
	write_setting("/proc/sys/kernel/core_pattern", "/tmp/core.%p");
	for (unsigned int i = 0; i < count; i++) {
		pid_t pid = fork();
		int status;

		REQUIRE(pid >= 0);
		if (!pid) {
			char value[32];

			snprintf(value, sizeof(value), "%lu", sizes[i]);
			REQUIRE(!prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL));
			execl("/init", "/init", "--child", value, NULL);
			_exit(3);
		}
		REQUIRE(waitpid(pid, &status, 0) == pid);
		REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && WCOREDUMP(status));
		inspect_core(pid, sizes[i]);
	}
	printf("MTE CORE PASS cases=%u\n", count);
	reboot(RB_POWER_OFF);
	return 0;
}
