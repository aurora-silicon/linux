// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../../../include/uapi/linux/prctl.h"
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include "../../kselftest.h"

static unsigned char value(size_t off)
{
	return 0x31 + (off / 4096) % 128;
}
static bool check(unsigned char *p, size_t len, size_t off)
{
	for (size_t i = 0; i < len; i++)
		if (p[i] != value(i + off)) {
			ksft_print_msg("offset=%zu got=%u expected=%u\n",
				       i + off, p[i], value(i + off));
			return false;
		}
	return true;
}
static int exercise(size_t native)
{
	size_t ps = getauxval(AT_PAGESZ),
	       len = native < 8192 ? 65536 : 8 * native;
	int fd = open("/dev/vgem", O_RDWR);
	struct drm_mode_create_dumb create = { .width = 4096,
					       .height = len / (4096 * 4),
					       .bpp = 32 };
	struct drm_mode_map_dumb map = {};
	struct drm_prime_handle prime = { .flags = DRM_RDWR };
	unsigned char *p = MAP_FAILED, *q = MAP_FAILED, *alias = MAP_FAILED;
	bool ok = fd >= 0 && !ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	ksft_print_header();
	ksft_set_plan(12);
	ksft_print_msg("ABI=%zuK\n", ps / 1024);
	map.handle = create.handle;
	ok &= !ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) && create.size >= len;
	if (ok)
		p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 map.offset);
	ok &= p != MAP_FAILED;
	ksft_test_result(ok, "full GEM shmem object mapping\n");
	q = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 map.offset);
	bool separate = q != MAP_FAILED;
	if (separate) {
		memset(q, 0x39, ps);
		memset(q + ps, 0xa2, ps);
		for (size_t i = 0; i < ps; i++)
			separate &= q[i] == 0x39 && q[ps + i] == 0xa2;
		munmap(q, 2 * ps);
		q = MAP_FAILED;
	}
	ksft_test_result(separate,
			 "adjacent user leaves map distinct backing bytes\n");
	if (ok) {
		for (size_t i = 0; i < len; i++)
			p[i] = value(i);
		ok = check(p, len, 0);
	}
	ksft_test_result(ok, "full object has distinct quarter data\n");
	if (ok) {
		q = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 map.offset);
		ok = q != MAP_FAILED && !mprotect(q + ps, ps, PROT_READ);
		if (ok)
			ok = check(q, len, 0);
		if (q != MAP_FAILED)
			munmap(q, len);
		q = MAP_FAILED;
	}
	ksft_test_result(
		ok,
		"cold faults after mprotect VMA splitting preserve object offsets\n");
	if (p != MAP_FAILED) {
		q = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 map.offset);
		ok = q != MAP_FAILED && !munmap(q + ps, ps);
		if (ok)
			ok = check(q + 2 * ps, len - 2 * ps, 2 * ps) &&
			     check(q, ps, 0);
		if (q != MAP_FAILED)
			munmap(q, len);
		q = MAP_FAILED;
	} else
		ok = false;
	ksft_test_result(
		ok,
		"cold faults after partial unmap preserve object offsets\n");
	prime.handle = create.handle;
	ok = !ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (ok) {
		alias = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
			     prime.fd, 0);
		ok = alias != MAP_FAILED && check(alias, len, 0);
	}
	ksft_test_result(ok, "PRIME exported full mapping shares GEM data\n");
	if (alias != MAP_FAILED) {
		q = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED, prime.fd,
			 3 * ps);
		ok = q != MAP_FAILED;
		if (ok)
			ok = check(q, ps, 3 * ps);
		if (q != MAP_FAILED)
			munmap(q, ps);
		q = MAP_FAILED;
	} else
		ok = false;
	ksft_test_result(
		ok, "PRIME partial mapping uses requested buffer offset\n");
	if (alias != MAP_FAILED) {
		q = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
			 prime.fd, 0);
		void *target = mmap(NULL, len, PROT_NONE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		ok = q != MAP_FAILED && target != MAP_FAILED;
		if (ok) {
			void *moved = mremap(q, len, len,
					     MREMAP_MAYMOVE | MREMAP_FIXED,
					     target);
			ok = moved == target;
			if (ok) {
				q = target;
				ok = check(q, len, 0);
			}
		}
		if (q != MAP_FAILED)
			munmap(q, len);
		if (target != MAP_FAILED && target != q)
			munmap(target, len);
		q = MAP_FAILED;
	} else
		ok = false;
	ksft_test_result(
		ok,
		"cold GEM faults after fixed mremap retain object offsets\n");
	if (alias != MAP_FAILED) {
		pid_t child = fork();
		int status = 0;
		if (!child) {
			unsigned char *cold = mmap(NULL, len,
						   PROT_READ | PROT_WRITE,
						   MAP_SHARED, prime.fd, 0);
			bool pass = cold != MAP_FAILED && check(cold, len, 0);
			if (pass)
				cold[0] = 0xe1;
			_exit(pass ? 0 : 1);
		}
		ok = child > 0 && waitpid(child, &status, 0) == child &&
		     WIFEXITED(status) && !WEXITSTATUS(status) && p[0] == 0xe1;
		p[0] = value(0);
	} else
		ok = false;
	ksft_test_result(
		ok, "forked GEM mapping shares bytes and retains backing\n");
	if (alias != MAP_FAILED) {
		q = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_SHARED,
			 prime.fd, len - ps);
		ok = q == MAP_FAILED && errno == EINVAL;
		if (q != MAP_FAILED)
			munmap(q, 2 * ps);
		q = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 map.offset + ps);
		ok &= q == MAP_FAILED && errno == EINVAL;
		if (q != MAP_FAILED)
			munmap(q, ps);
		q = MAP_FAILED;
	} else
		ok = false;
	ksft_test_result(ok, "PRIME bounds and exact GEM offset rejection\n");
	if (alias != MAP_FAILED) {
		size_t targets[] = { native, 4096, 16384 };

		ok = true;
		for (unsigned int t = 0; t < 3; t++) {
			size_t target = targets[t];
			pid_t child;
			int status = 0;
			bool pair;

			if (target > native || target == ps ||
			    (t && target == native))
				continue;
			child = fork();
			if (!child) {
				char number[24], length[24], pages[24];

				snprintf(number, sizeof(number), "%d",
					 prime.fd);
				snprintf(length, sizeof(length), "%zu", len);
				snprintf(pages, sizeof(pages), "%zu", target);
				if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
					  target == native ? 0UL : target, 0UL,
					  0UL, 0UL))
					_exit(3);
				execl("/init", "/init", "--shared", number,
				      length, pages, NULL);
				_exit(4);
			}
			pair = child > 0 &&
			       waitpid(child, &status, 0) == child &&
			       WIFEXITED(status) && !WEXITSTATUS(status);
			for (size_t i = 0; i < len; i += 4096) {
				pair &= p[i] ==
					(unsigned char)(0x80 + i / 4096);
				p[i] = value(i);
			}
			ksft_print_msg(
				"PRIME sharing %zuK -> %zuK status=%d bytes=%zu\n",
				ps / 1024, target / 1024, status, len);
			ok &= pair;
		}
	} else {
		ok = false;
	}
	ksft_test_result(
		ok,
		"PRIME backing shared between native and alternative processes\n");
	struct drm_gem_close close_arg = { .handle = create.handle };
	ok = !ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
	close(fd);
	if (alias != MAP_FAILED) {
		close(prime.fd);
		ok &= check(alias, len, 0);
		munmap(alias, len);
	} else
		ok = false;
	if (p != MAP_FAILED) {
		ok &= check(p, len, 0);
		munmap(p, len);
	} else
		ok = false;
	ksft_test_result(
		ok,
		"mappings retain object after handles and descriptors close\n");
	ksft_print_cnts();
	return ksft_get_fail_cnt() ? 1 : 0;
}
int main(int argc, char **argv)
{
	bool ok = true;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 5 && !strcmp(argv[1], "--shared")) {
		struct stat init, self;
		int fd = atoi(argv[2]);
		size_t len = strtoul(argv[3], NULL, 10);
		if (stat("/proc/1/exe", &init) ||
		    stat("/proc/self/exe", &self) ||
		    init.st_ino != self.st_ino || init.st_dev != self.st_dev ||
		    len < 65536 || len > 8 * 65536 ||
		    getauxval(AT_PAGESZ) != strtoul(argv[4], NULL, 10))
			return 2;
		unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, 0);
		bool pass = p != MAP_FAILED && check(p, len, 0);
		if (pass)
			for (size_t i = 0; i < len; i += 4096)
				p[i] = 0x80 + i / 4096;
		if (p != MAP_FAILED)
			munmap(p, len);
		return pass ? 0 : 1;
	}
	if (argc == 3 && !strcmp(argv[1], "--test") && getppid() == 1)
		return exercise(strtoul(argv[2], NULL, 10));
	if (getpid() != 1 || argc != 1)
		return 2;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL))
		return 3;
	FILE *dev = fopen("/sys/class/drm/card0/dev", "r");
	unsigned maj, min;
	if (!dev || fscanf(dev, "%u:%u", &maj, &min) != 2)
		return 4;
	fclose(dev);
	if (mknod("/dev/vgem", S_IFCHR | 0600, makedev(maj, min)))
		return 5;
	size_t native = getauxval(AT_PAGESZ),
	       targets[] = { native, 4096, 16384 };

	for (unsigned int t = 0; t < 3; t++) {
		size_t target = targets[t];
		pid_t pid;
		int status = 0;

		if (target > native || (t && target == native))
			continue;
		pid = fork();
		if (!pid) {
			char number[24];

			snprintf(number, sizeof(number), "%zu", native);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
				  target == native ? 0UL : target, 0UL, 0UL,
				  0UL))
				_exit(3);
			execl("/init", "/init", "--test", number, NULL);
			_exit(4);
		}
		ok &= pid > 0 && waitpid(pid, &status, 0) == pid &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
		printf("# GEM child ABI=%zuK native=%zuK status=%d\n",
		       target / 1024, native / 1024, status);
	}
	printf("%s - GEM fixture complete\n", ok ? "ok" : "not ok");
	reboot(RB_POWER_OFF);
	return ok ? 0 : 1;
}
