// SPDX-License-Identifier: GPL-2.0-only
/* Core-dump regression runner: PID 1 in a disposable VM only. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../kselftest.h"

#define ANON 0x100000000UL
#define FILEMAP 0x102000000UL
#define SPARSE 0x104000000UL
#define EXCLUDED 0x106000000UL
#define SWAPPED 0x108000000UL
#define SWAPPED_SIZE (1UL << 20)
#define SPARSE_SIZE (8UL << 20)

static void die(const char *what)
{
	perror(what);
	_exit(120);
}

static void write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	size_t len = strlen(text);

	if (fd < 0 || write(fd, text, len) != (ssize_t)len)
		die(path);
	close(fd);
}

static void *map_at(unsigned long address, size_t len, int prot, int fd,
		    off_t off)
{
	void *p = mmap((void *)address, len, prot,
		       MAP_PRIVATE | MAP_FIXED_NOREPLACE |
			       (fd < 0 ? MAP_ANONYMOUS : 0),
		       fd, off);

	if (p == MAP_FAILED)
		die("mmap");
	return p;
}

static void crash(bool headers, int bounded)
{
	unsigned long ps = getauxval(AT_PAGESZ);
	struct rlimit limit = { 64UL << 20, 64UL << 20 };
	struct stat self, init;
	unsigned char *p, *data;
	int fd;

	if (getppid() != 1 || stat("/proc/self/exe", &self) ||
	    stat("/proc/1/exe", &init) || self.st_dev != init.st_dev ||
	    self.st_ino != init.st_ino)
		_exit(121);
	if (bounded)
		limit.rlim_cur = limit.rlim_max = ps - (bounded < 0);
	if (setrlimit(RLIMIT_CORE, &limit))
		die("rlimit");
	write_text("/proc/self/coredump_filter", headers ? "0x10" : "0x1f");
	p = map_at(ANON, 4 * ps, PROT_READ | PROT_WRITE, -1, 0);
	for (unsigned int i = 0; i < 4; i++)
		memset(p + i * ps, 0x31 + i, ps);
	if (mprotect(p + ps, ps, PROT_NONE) ||
	    mprotect(p + 2 * ps, ps, PROT_READ))
		die("mprotect");
	p = map_at(SPARSE, SPARSE_SIZE, PROT_READ | PROT_WRITE, -1, 0);
	memset(p, 0x65, ps);
	/* Also exercise an existing read-only zero PTE. */
	if (*(volatile unsigned char *)(p + ps) != 0)
		_exit(122);
	p = map_at(EXCLUDED, ps, PROT_READ | PROT_WRITE, -1, 0);
	memset(p, 0xa7, ps);
	if (madvise(p, ps, MADV_DONTDUMP))
		die("dontdump");
	fd = open("/core-input", O_CREAT | O_TRUNC | O_RDWR, 0700);
	data = malloc(19 * ps);
	if (fd < 0 || !data)
		die("file setup");
	for (unsigned int i = 0; i < 19; i++)
		memset(data + i * ps, 0x81 + i, ps);
	memcpy(data, ELFMAG, SELFMAG);
	if (write(fd, data, 19 * ps) != (ssize_t)(19 * ps))
		die("file data");
	/* Offset one user page: three mappings select non-native quarters. */
	map_at(FILEMAP, ps, PROT_READ, fd, headers ? 0 : ps);
	map_at(FILEMAP + 2 * ps, 2 * ps, PROT_READ, fd, 2 * ps);
	map_at(FILEMAP + 8 * ps, ps, PROT_READ, fd, 17 * ps);
	close(fd);
	fd = open("/core-library", O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd < 0 || write(fd, data, ps) != (ssize_t)ps)
		die("non-executable ELF file");
	map_at(FILEMAP + 12 * ps, ps, PROT_READ, fd, 0);
	close(fd);
	free(data);
	if (!headers && !bounded) {
		unsigned int missing = 0;
		uint64_t entry;
		int pm = open("/proc/self/pagemap", O_RDONLY);

		p = map_at(SWAPPED, SWAPPED_SIZE, PROT_READ | PROT_WRITE, -1,
			   0);
		memset(p, 0xc4, SWAPPED_SIZE);
		for (unsigned int tries = 0; tries < 200; tries++) {
			if (madvise(p, SWAPPED_SIZE, MADV_COLD) ||
			    madvise(p, SWAPPED_SIZE, MADV_PAGEOUT))
				die("pageout");
			missing = 0;
			for (unsigned long i = 0; i < SWAPPED_SIZE / ps; i++) {
				if (pread(pm, &entry, sizeof(entry),
					  (SWAPPED / ps + i) * 8) != 8)
					die("pagemap");
				missing += !(entry & (1ULL << 62));
			}
			if (!missing)
				break;
			usleep(10000);
		}
		close(pm);
		if (missing)
			_exit(124);
		printf("# %luK dump precondition: all %lu leaves swapped\n",
		       ps / 1024, SWAPPED_SIZE / ps);
	}
	/* The file VMAs remain unfaulted, so dumping must retrieve file data. */
	raise(SIGABRT);
	_exit(123);
}

static Elf64_Phdr *find_load(unsigned char *data, Elf64_Ehdr *elf,
			     unsigned long addr)
{
	Elf64_Phdr *ph = (void *)(data + elf->e_phoff);

	for (unsigned int i = 0; i < elf->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD && addr >= ph[i].p_vaddr &&
		    addr - ph[i].p_vaddr < ph[i].p_memsz)
			return ph + i;
	return NULL;
}

static bool bytes_at(unsigned char *data, Elf64_Ehdr *elf, size_t len,
		     unsigned long addr, size_t count, unsigned char expected)
{
	Elf64_Phdr *p = find_load(data, elf, addr);
	unsigned long off;

	if (!p || addr - p->p_vaddr + count > p->p_filesz)
		return false;
	off = p->p_offset + addr - p->p_vaddr;
	if (off > len || count > len - off)
		return false;
	for (size_t i = 0; i < count; i++)
		if (data[off + i] != expected)
			return false;
	return true;
}

static void inspect(pid_t pid, unsigned long ps, bool headers)
{
	char path[64];
	struct stat st;
	unsigned char *data;
	Elf64_Ehdr *elf;
	Elf64_Phdr *ph, *p;
	bool geometry = true, aux = false;
	unsigned int file = 0;
	unsigned long vdso = 0;
	int fd;

	snprintf(path, sizeof(path), "/core.%d", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || st.st_size < (off_t)sizeof(*elf))
		ksft_exit_fail_msg("missing/short core %s\n", path);
	data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (data == MAP_FAILED)
		die("core read");
	elf = (void *)data;
	if (memcmp(elf->e_ident, ELFMAG, SELFMAG) || elf->e_type != ET_CORE ||
	    elf->e_phentsize != sizeof(*ph) ||
	    elf->e_phoff > (uint64_t)st.st_size ||
	    elf->e_phnum * sizeof(*ph) > (uint64_t)st.st_size - elf->e_phoff)
		ksft_exit_fail_msg("invalid core ELF\n");
	ph = (void *)(data + elf->e_phoff);
	for (unsigned int i = 0; i < elf->e_phnum; i++) {
		if (ph[i].p_type == PT_LOAD) {
			geometry &= ph[i].p_align == ps &&
				    ph[i].p_filesz <= ph[i].p_memsz &&
				    ph[i].p_offset % ps == ph[i].p_vaddr % ps;
		} else if (ph[i].p_type == PT_NOTE) {
			size_t at = ph[i].p_offset, end = at + ph[i].p_filesz;

			if (end > (size_t)st.st_size || end < at)
				ksft_exit_fail_msg("invalid notes\n");
			while (at + sizeof(Elf64_Nhdr) <= end) {
				Elf64_Nhdr *n = (void *)(data + at);
				size_t desc = at + sizeof(*n) +
					      ((n->n_namesz + 3UL) & ~3UL);
				uint64_t *words = (void *)(data + desc);

				if (desc > end || n->n_descsz > end - desc)
					ksft_exit_fail_msg(
						"invalid note length\n");
				if (n->n_type == NT_AUXV) {
					for (size_t j = 0;
					     j + 1 < n->n_descsz / 8; j += 2) {
						if (words[j] == AT_PAGESZ)
							aux = words[j + 1] ==
							      ps;
						else if (words[j] ==
							 AT_SYSINFO_EHDR)
							vdso = words[j + 1];
					}
				}
				if (n->n_type == NT_FILE && n->n_descsz >= 16 &&
				    words[0] <= (n->n_descsz / 8 - 2) / 3 &&
				    words[1] == ps) {
					for (size_t j = 0; j < words[0]; j++) {
						unsigned long addr =
							words[2 + 3 * j];
						unsigned long offset =
							words[4 + 3 * j];

						if (addr == FILEMAP &&
						    offset == (headers ? 0UL :
									 1UL))
							file |= 1;
						if (addr == FILEMAP + 2 * ps &&
						    offset == 2)
							file |= 2;
						if (addr == FILEMAP + 8 * ps &&
						    offset == 17)
							file |= 4;
					}
				}
				at = desc + ((n->n_descsz + 3UL) & ~3UL);
			}
		}
	}
	ksft_test_result(geometry, "%luK core PT_LOAD granule/alignment%s\n",
			 ps / 1024, headers ? " headers-only" : "");
	ksft_test_result(aux && file == 7,
			 "%luK core AUXV and NT_FILE offsets%s\n", ps / 1024,
			 headers ? " headers-only" : "");
	if (headers) {
		bool skipped;

		p = find_load(data, elf, FILEMAP + 2 * ps);
		skipped = p && !p->p_filesz;
		p = find_load(data, elf, FILEMAP + 8 * ps);
		ksft_test_result(
			skipped && p && !p->p_filesz,
			"%luK header filter excludes nonzero quarter/native file offsets\n",
			ps / 1024);
		p = find_load(data, elf, FILEMAP + 12 * ps);
		ksft_test_result(
			p && p->p_filesz == ps &&
				p->p_offset + ps <= (uint64_t)st.st_size &&
				!memcmp(data + p->p_offset, ELFMAG, SELFMAG),
			"%luK header filter recognizes non-executable ELF file\n",
			ps / 1024);
		p = find_load(data, elf, FILEMAP);
		ksft_test_result(
			p && p->p_filesz == ps &&
				p->p_offset + ps <= (uint64_t)st.st_size &&
				!memcmp(data + p->p_offset, ELFMAG, SELFMAG),
			"%luK executable file header limited to one user page\n",
			ps / 1024);
	} else {
		bool anon = true, files = true;

		p = find_load(data, elf, vdso);
		ksft_test_result(p && p->p_filesz >= SELFMAG &&
					 p->p_offset + p->p_filesz <=
						 (uint64_t)st.st_size &&
					 !memcmp(data + p->p_offset, ELFMAG,
						 SELFMAG),
				 "%luK core vDSO ELF bytes\n", ps / 1024);
		ksft_test_result(bytes_at(data, elf, st.st_size, SWAPPED,
					  SWAPPED_SIZE, 0xc4),
				 "%luK core swapped anonymous bytes\n",
				 ps / 1024);

		for (unsigned int i = 0; i < 4; i++)
			anon &= bytes_at(data, elf, st.st_size, ANON + i * ps,
					 ps, 0x31 + i);
		for (unsigned int i = 1; i < 4; i++)
			files &= bytes_at(data, elf, st.st_size,
					  FILEMAP + (i == 1 ? 0 : i * ps), ps,
					  0x81 + i);
		ksft_test_result(
			anon, "%luK core anonymous and PROT_NONE/READ bytes\n",
			ps / 1024);
		files &= bytes_at(data, elf, st.st_size, FILEMAP + 8 * ps, ps,
				  0x81 + 17);
		ksft_test_result(files, "%luK core unfaulted file quarters\n",
				 ps / 1024);
		ksft_test_result(
			bytes_at(data, elf, st.st_size, SPARSE, ps, 0x65) &&
				bytes_at(data, elf, st.st_size, SPARSE + ps,
					 SPARSE_SIZE - ps, 0),
			"%luK core sparse and zero-page bytes\n", ps / 1024);
		ksft_test_result(
			st.st_blocks * 512 < st.st_size / 2,
			"%luK core keeps untouched anonymous range sparse\n",
			ps / 1024);
		p = find_load(data, elf, EXCLUDED);
		ksft_test_result(p && !p->p_filesz,
				 "%luK core DONTDUMP excludes data\n",
				 ps / 1024);
	}
	munmap(data, st.st_size);
	close(fd);
	unlink(path);
}

int main(int argc, char **argv)
{
	unsigned long native_size = getauxval(AT_PAGESZ);
	unsigned long sizes[] = { native_size, 4096, 16384 };
	unsigned int modes = native_size == 65536 ? 3 : 2;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 2 && !strcmp(argv[1], "--crash"))
		crash(false, 0);
	if (argc == 2 && !strcmp(argv[1], "--headers"))
		crash(true, 0);
	if (argc == 2 && !strcmp(argv[1], "--limit"))
		crash(false, 1);
	if (argc == 2 && !strcmp(argv[1], "--below-limit"))
		crash(false, -1);
	if (getpid() != 1 || argc != 1) {
		fprintf(stderr, "Use only as PID 1 in a disposable VM\n");
		return 2;
	}
	if (mount("proc", "/proc", "proc", 0, NULL) && errno != EBUSY)
		die("proc");
	if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
	    swapon("/dev/vda", 0))
		die("VM swap setup");
	write_text("/proc/sys/kernel/core_pattern", "/core.%p");
	ksft_print_header();
	ksft_set_plan(16 * modes);
	for (unsigned int mode = 0; mode < modes; mode++) {
		unsigned long ps = sizes[mode];
		for (unsigned int headers = 0; headers < 4; headers++) {
			pid_t pid = fork();
			int status = 0;

			if (!pid) {
				if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
					  ps == native_size ? 0UL : ps, 0UL,
					  0UL, 0UL))
					die("exec size");
				execl("/init", "/init",
				      headers == 3 ? "--below-limit" :
				      headers == 2 ? "--limit" :
				      headers	   ? "--headers" :
						     "--crash",
				      NULL);
				die("exec");
			}
			if (pid < 0 || waitpid(pid, &status, 0) != pid ||
			    !WIFSIGNALED(status) ||
			    WTERMSIG(status) != SIGABRT ||
			    (headers < 3 && !WCOREDUMP(status)))
				ksft_exit_fail_msg(
					"child/core failure status=%#x\n",
					status);
			if (headers >= 2) {
				char path[64];
				struct stat st;
				bool exists, no_data = false;

				snprintf(path, sizeof(path), "/core.%d", pid);
				exists = !stat(path, &st);
				if (exists && headers == 2) {
					Elf64_Ehdr elf;
					Elf64_Phdr ph;
					uint64_t data_start = UINT64_MAX;
					int fd = open(path, O_RDONLY);

					if (fd < 0 ||
					    pread(fd, &elf, sizeof(elf), 0) !=
						    sizeof(elf))
						die("limited core header");
					for (unsigned int i = 0;
					     i < elf.e_phnum; i++) {
						if (pread(fd, &ph, sizeof(ph),
							  elf.e_phoff +
								  i * sizeof(ph)) !=
						    sizeof(ph))
							die("limited core phdr");
						if (ph.p_type == PT_LOAD &&
						    ph.p_offset < data_start)
							data_start =
								ph.p_offset;
					}
					close(fd);
					/* Sparse note padding is not charged to RLIMIT_CORE. */
					no_data = data_start != UINT64_MAX &&
						  st.st_size <=
							  (off_t)data_start;
					printf("# limited core: bytes=%lld first-data=%llu limit=%lu\n",
					       (long long)st.st_size,
					       (unsigned long long)data_start,
					       ps);
				}
				ksft_test_result(
					headers == 2 ?
						exists && st.st_size > 0 &&
							no_data :
						!exists && errno == ENOENT,
					"%luK core RLIMIT_CORE %s one user page\n",
					ps / 1024,
					headers == 2 ? "at" : "below");
				unlink(path);
			} else {
				inspect(pid, ps, headers);
			}
		}
	}
	ksft_print_cnts();
	printf("%s - core fixture complete\n",
	       ksft_get_fail_cnt() ? "not ok" : "ok");
	reboot(RB_POWER_OFF);
	return ksft_get_fail_cnt() ? 1 : 0;
}
