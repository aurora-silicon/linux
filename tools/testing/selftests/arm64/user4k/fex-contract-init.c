/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void)
{
	if (getpid() != 1)
		return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	int failed = 0;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) {
		perror("mount");
		failed = 1;
		goto done;
	}
	pid_t child = fork();
	if (!child) {
		setenv("HOME", "/root", 1);
		setenv("PATH", "/usr/bin:/bin", 1);
		setenv("XDG_RUNTIME_DIR", "/run", 1);
		setenv("FEX_ROOTFS", "/", 1);
		setenv("LD_PRELOAD", "/usr/lib/libfex-abi.so", 1);
		if (prctl(PR_AURORA_SET_DEFAULT_PAGE_SIZE, 4096UL, 0UL, 0UL,
			  0UL) ||
		    prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, 4096UL, 0UL, 0UL, 0UL))
			_exit(3);
		execl("/usr/bin/FEXInterpreter", "/usr/bin/FEXInterpreter",
		      "/x86-contract", NULL);
		perror("FEX exec");
		_exit(4);
	}
	int status = 0;
	int ok = child > 0 && waitpid(child, &status, 0) == child &&
		 WIFEXITED(status) && !WEXITSTATUS(status);
	printf("%s - FEX 4K native-process / x86 workload status=%#x\n",
	       ok ? "ok" : "not ok", status);
	failed |= !ok;
done:
	printf("GRANULE FEX RUNTIME %s\n", failed ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
