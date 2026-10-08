/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <sys/auxv.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
__attribute__((constructor)) static void check_abi(void)
{
	char path[1024];
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n < 0)
		return;
	path[n] = 0;
	if (!strstr(path, "FEXInterpreter"))
		return;
	unsigned long ps = getauxval(AT_PAGESZ);
	dprintf(2, "FEX NATIVE HOST PROCESS PAGE SIZE %lu\n", ps);
	if (ps != 4096)
		_exit(90);
}
