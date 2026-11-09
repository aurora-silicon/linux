// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Aurora Silicon */

#include <errno.h>
#include <fcntl.h>
#include <linux/apple-fvread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int hex(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int uuid_parse(const char *text, unsigned char out[16])
{
	static const unsigned int dash[] = { 8, 13, 18, 23 };
	unsigned int i, j = 0, d = 0;

	if (strlen(text) != 36)
		return -1;
	for (i = 0; i < 36;) {
		int hi, lo;

		if (d < sizeof(dash) / sizeof(dash[0]) && i == dash[d]) {
			if (text[i++] != '-')
				return -1;
			d++;
			continue;
		}
		hi = hex(text[i++]);
		lo = hex(text[i++]);
		if (hi < 0 || lo < 0 || j >= 16)
			return -1;
		out[j++] = (unsigned char)((hi << 4) | lo);
	}
	return j == 16 ? 0 : -1;
}

static void usage(const char *name)
{
	fprintf(stderr,
		"usage: %s --device BLOCKDEV --volume UUID --exec COMMAND [ARG...]\n",
		name);
}

int main(int argc, char **argv)
{
	struct apple_fvread_unlock request = { .flags = APPLE_FVREAD_UNLOCK_FV_OFF };
	const char *device = NULL, *volume = NULL;
	int device_fd = -1, control_fd = -1, command = -1, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else if (!strcmp(argv[i], "--volume") && i + 1 < argc)
			volume = argv[++i];
		else if (!strcmp(argv[i], "--exec") && i + 1 < argc) {
			command = i + 1;
			break;
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!device || !volume || command < 0 || uuid_parse(volume, request.volume_uuid)) {
		usage(argv[0]);
		return 2;
	}

	device_fd = open(device, O_RDONLY | O_CLOEXEC);
	if (device_fd < 0) {
		perror("open APFS container");
		return 1;
	}
	control_fd = open("/dev/apple-fvread", O_RDONLY | O_CLOEXEC);
	if (control_fd < 0) {
		perror("open /dev/apple-fvread");
		return 1;
	}
	request.device_fd = device_fd;
	if (ioctl(control_fd, APPLE_FVREAD_IOC_UNLOCK, &request) < 0) {
		perror("APPLE_FVREAD_IOC_UNLOCK");
		return 1;
	}
	close(device_fd);
	if (control_fd != 3) {
		if (dup2(control_fd, 3) < 0) {
			perror("dup2 control fd");
			return 1;
		}
		close(control_fd);
		control_fd = 3;
	}
	if (fcntl(control_fd, F_SETFD, 0) < 0 ||
	    setenv("FVREAD_CONTROL_FD", "3", 1) < 0 ||
	    setenv("FVREAD_VOLUME_UUID", volume, 1) < 0) {
		perror("prepare APFS reader environment");
		return 1;
	}
	execvp(argv[command], &argv[command]);
	perror("exec APFS reader");
	return 1;
}
