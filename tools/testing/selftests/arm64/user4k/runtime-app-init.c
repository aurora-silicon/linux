/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define SET_EXEC_PAGE_SIZE 0x41555001
int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "--exec")) {
		if (prctl(SET_EXEC_PAGE_SIZE, strtoul(argv[2], NULL, 10), 0UL,
			  0UL, 0UL))
			return 8;
		execv(argv[3], argv + 3);
		perror("exec");
		return 9;
	}
	if (getpid() != 1)
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	int failed = 0;
	pid_t children[3];
	unsigned long sizes[] = { getauxval(AT_PAGESZ), 4096, 16384 };
	unsigned count = sizes[0] > 16384 ? 3 : 2;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) {
		perror("mount");
		failed = 1;
		goto done;
	}
	mkdir("/dev/shm", 0777);
	mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);
	for (unsigned i = 0; i < count; i++) {
		children[i] = fork();
		if (!children[i]) {
			char ps[32];
			snprintf(ps, sizeof(ps), "%lu", sizes[i]);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(3);
			execl("/usr/bin/python3", "/usr/bin/python3", "-I",
			      "/runtime-app.py", ps, NULL);
			perror("exec Python");
			_exit(4);
		}
	}
	for (unsigned i = 0; i < count; i++) {
		int status = 0;
		int ok = children[i] > 0 &&
			 waitpid(children[i], &status, 0) == children[i] &&
			 WIFEXITED(status) && !WEXITSTATUS(status);
		printf("%s - distro Python ABI %luK status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		failed |= !ok;
	}
done:
	if (failed) {
		FILE *f = fopen("/trace4k", "r");
		if (f) {
			char line[2048];
			while (fgets(line, sizeof(line), f))
				fputs(line, stdout);
			fclose(f);
		}
	}
	printf("GRANULE DISTRO RUNTIME %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
