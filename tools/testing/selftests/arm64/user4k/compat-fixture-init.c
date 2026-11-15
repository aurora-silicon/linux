/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
static int failed;
static unsigned long native_size;
static void result(int ok, const char *s)
{
	printf("%s - launcher %s\n", ok ? "ok" : "not ok", s);
	if (!ok)
		failed++;
}
static int launch(const char *path, char **args, int request, int expect_errno)
{
	pid_t child = fork();
	int status = 0;
	if (!child) {
		if (request && prctl(SET_EXEC_PAGE_SIZE, 4096UL, 0UL, 0UL, 0UL))
			_exit(95);
		unsigned char *canary =
			mmap(NULL, native_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (canary == MAP_FAILED)
			_exit(94);
		memset(canary, 0x69, native_size);
		int before = prctl(0x41555002, 0UL, 0UL, 0UL, 0UL);
		for (int i = 0; i < (expect_errno ? 64 : 1); i++) {
			execv(path, args);
			if (!expect_errno || errno != expect_errno)
				_exit(96);
			if (getauxval(AT_PAGESZ) != native_size ||
			    prctl(0x41555002, 0UL, 0UL, 0UL, 0UL) != before)
				_exit(93);
			for (int j = 0; j < native_size; j++)
				if (canary[j] != 0x69)
					_exit(92);
		}
		_exit(0);
	}
	if (child < 0 || waitpid(child, &status, 0) != child)
		return 0;
	if (status)
		printf("# child %s status=%#x\n", path, status);
	return WIFEXITED(status) && !WEXITSTATUS(status);
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--returned"))
		return getauxval(AT_PAGESZ) ==
				       strtoul(getenv("AURORA_NATIVE_PAGE_SIZE"),
					       NULL, 10) ?
			       0 :
			       1;
	if (argc > 1 && !strcmp(argv[1], "--returned4k"))
		return getauxval(AT_PAGESZ) == 4096 ? 0 : 1;
	if (getpid() != 1) {
		fprintf(stderr, "Disposable VM PID1 only\n");
		return 2;
	}
	native_size = getauxval(AT_PAGESZ);
	char native_text[24];
	snprintf(native_text, sizeof(native_text), "%lu", native_size);
	setenv("AURORA_NATIVE_PAGE_SIZE", native_text, 1);
	setvbuf(stdout, NULL, _IONBF, 0);
	mount("proc", "/proc", "proc", 0, NULL);
	int no4k = argc > 1 && !strcmp(argv[1], "--no4k"),
	    fd = memfd_create("compat-offsets", 0);
	if (no4k) {
		errno = 0;
		result(prctl(SET_EXEC_PAGE_SIZE, 4096UL, 0UL, 0UL, 0UL) == -1 &&
			       errno == EOPNOTSUPP,
		       "4KB capability is actually masked");
	}
	result(fd >= 0 && !ftruncate(fd, 0x100000000LL + 4 * native_size),
	       "sparse file fixture above 4GB");
	unsigned char page[4096];
	for (unsigned q = 0; q < 4 * native_size / 4096; q++) {
		memset(page, 0x40 + q, sizeof(page));
		if (pwrite(fd, page, sizeof(page), q * 4096) != sizeof(page))
			failed++;
		memset(page, 0x80 + q, sizeof(page));
		if (pwrite(fd, page, sizeof(page), 0x100000000LL + q * 4096) !=
		    sizeof(page))
			failed++;
	}
	char fdstr[24];
	snprintf(fdstr, sizeof(fdstr), "%d", fd);
	char *arg = malloc(90001);
	for (unsigned i = 0; i < 90000; i++)
		arg[i] = 'a' + i % 23;
	arg[90000] = 0;
	char *args[] = { "/probe32-64k", no4k ? native_text : "4096", fdstr,
			 arg, NULL };
	result(launch(args[0], args, 0, 0),
	       "64KB-aligned ELF32 automatically selects available ABI");
	if (!no4k) {
		args[0] = "/probe32-4k";
		result(launch(args[0], args, 0, 0),
		       "4KB-aligned ELF32 executes with 4KB translation");
		args[0] = "/probe32-64k";
		result(launch(args[0], args, 1, 0),
		       "explicit pending 4KB request allows ELF32");
	}
	args[0] = "/script32";
	result(launch(args[0], args, 0, 0),
	       "script final ELF class determines page granule");
	args[0] = "/probe32-dynamic";
	result(launch(args[0], args, 0, 0),
	       "ELF32 interpreter mapping uses selected page granule");
	char *chain[] = {
		"/probe32-64k", "--exec32", no4k ? native_text : "4096",
		fdstr,		arg,	    NULL
	};
	result(launch(chain[0], chain, 0, 0),
	       "ELF32 to ELF32 restages arguments and selects ABI");
	char *back[] = { "/probe32-64k", "--exec64", NULL };
	result(launch(back[0], back, 0, 0),
	       "ELF32 to ELF64 exec restores native default");
	if (!no4k) {
		back[1] = "--exec64-4k";
		result(launch(back[0], back, 0, 0),
		       "ELF32 can request alternative ELF64 exec granule");
	}
	char *big = malloc(200001);
	memset(big, 'x', 200000);
	big[200000] = 0;
	args[0] = "/probe32-64k";
	args[3] = big;
	if (!no4k)
		result(launch(args[0], args, 0, E2BIG),
		       "64 oversized transfers return E2BIG and preserve old mm and request");
	printf("COMPAT FIXTURE %s mode=%s\n", failed ? "FAIL" : "PASS",
	       no4k ? "no4k-native-fallback" : "auto4k");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
