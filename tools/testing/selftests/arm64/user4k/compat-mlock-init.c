// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
struct result {
	uint32_t pagesize, passed, failed, error;
};
int main(int argc, char **argv)
{
	struct result out = { 0 };
	unsigned long expected;
	int pipes[2], status = -1;
	pid_t pid;
	size_t done = 0;
	int ok;

	if (getpid() != 1 ||
	    (argc != 1 && (argc != 2 || strcmp(argv[1], "--no4k"))))
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	expected = argc == 2 ? getauxval(AT_PAGESZ) : 4096;
	if (mount("proc", "/proc", "proc", 0, NULL) || pipe(pipes))
		return 3;
	pid = fork();
	if (!pid) {
		char value[32];
		close(pipes[0]);
		if (dup2(pipes[1], 1) < 0)
			_exit(4);
		close(pipes[1]);
		snprintf(value, sizeof(value), "%lu", expected);
		execl("/compat-mlock", "/compat-mlock", value, NULL);
		_exit(5);
	}
	close(pipes[1]);
	while (done < sizeof(out)) {
		ssize_t n =
			read(pipes[0], (char *)&out + done, sizeof(out) - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += n;
	}
	close(pipes[0]);
	ok = pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	     !WEXITSTATUS(status) && done == sizeof(out) &&
	     out.pagesize == expected && out.passed == 6 && !out.failed;
	printf("%s - AArch32 memlock expected=%lu actual=%u passed=%u stage=%u error=%d status=%d\n",
	       ok ? "ok" : "not ok", expected, out.pagesize, out.passed,
	       out.failed, (int32_t)out.error, status);
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
