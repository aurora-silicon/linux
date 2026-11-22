// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * DRM_IOCTL_ASAHI_GEM_USERPTR on the GPU's render node: what it accepts and
 * refuses, and the accounting (VmPin in /proc/self/status, the client's
 * drm-total-system in its fdinfo). No GPU work is submitted. Skipped where
 * the render node is not this driver's or lacks the feature, and where the
 * caller has neither CAP_IPC_LOCK nor a RLIMIT_MEMLOCK hard limit with room
 * for the four pages a test pins.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/capability.h>
#include <drm/asahi_drm.h>

#include "../../../kselftest_harness.h"

#define PAGE 0x4000UL

static int ioc(int fd, unsigned long cmd, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, cmd, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret ? -errno : 0;
}

/* VmPin of this process (mm->pinned_vm) in kB. */
static long vm_pin_kb(void)
{
	char line[256];
	long kb = -1;
	FILE *f = fopen("/proc/self/status", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "VmPin: %ld kB", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

/* drm-total-system of the client behind @fd, in bytes: the objects it created. */
static long long client_total(int fd)
{
	char path[64], line[256], unit[8];
	long long value = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unit[0] = 0;
		if (sscanf(line, "drm-total-system: %lld %7s", &value, unit) < 1)
			continue;
		if (!strcmp(unit, "KiB"))
			value <<= 10;
		else if (!strcmp(unit, "MiB"))
			value <<= 20;
		break;
	}
	fclose(f);
	return value;
}

static int userptr(int fd, void *ptr, uint64_t size, uint32_t flags,
		   uint32_t vm, uint32_t *handle)
{
	struct drm_asahi_gem_userptr args = {
		.userptr = (uintptr_t)ptr,
		.size = size,
		.flags = flags,
		.vm_id = (flags & DRM_ASAHI_GEM_VM_PRIVATE) ? vm : 0,
	};
	int ret = ioc(fd, DRM_IOCTL_ASAHI_GEM_USERPTR, &args);

	if (!ret)
		*handle = args.handle;
	return ret;
}

static int bind(int fd, uint32_t vm, uint32_t handle, uint64_t addr,
		uint64_t size, uint32_t flags)
{
	struct drm_asahi_gem_bind_op op = {
		.flags = flags, .handle = handle, .range = size, .addr = addr,
	};
	struct drm_asahi_vm_bind args = {
		.vm_id = vm, .num_binds = 1, .stride = sizeof(op),
		.userptr = (uintptr_t)&op,
	};

	return ioc(fd, DRM_IOCTL_ASAHI_VM_BIND, &args);
}

static int unbind(int fd, uint32_t vm, uint64_t addr, uint64_t size)
{
	return bind(fd, vm, 0, addr, size, DRM_ASAHI_BIND_UNBIND);
}

static void close_handle(int fd, uint32_t handle)
{
	struct drm_gem_close args = { .handle = handle };

	ioc(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

/* Whether the driver behind @fd is this one. */
static int is_asahi(int fd)
{
	char name[16] = "";
	struct drm_version version = {
		.name = name, .name_len = sizeof(name) - 1,
	};

	return !ioc(fd, DRM_IOCTL_VERSION, &version) && !strcmp(name, "aurora");
}

/*
 * Clears CAP_IPC_LOCK in the effective set of this process, which is always
 * allowed. Returns 0 once the process is without it.
 */
static int drop_cap_ipc_lock(void)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];

	if (syscall(SYS_capget, &header, data))
		return -errno;
	data[CAP_TO_INDEX(CAP_IPC_LOCK)].effective &= ~CAP_TO_MASK(CAP_IPC_LOCK);
	return syscall(SYS_capset, &header, data) ? -errno : 0;
}

/* The largest range a test binds, and the most one pins outside memlock_limit. */
#define BIND_MAX (4 * PAGE)

/*
 * Whether this process may pin BIND_MAX more: it has CAP_IPC_LOCK, or
 * RLIMIT_MEMLOCK has room for it, after raising the soft limit to the hard
 * one if that takes it.
 */
static int memlock_room(void)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	long pin = vm_pin_kb();
	uint64_t need = (pin > 0 ? (uint64_t)pin << 10 : 0) + BIND_MAX;
	struct rlimit lim;

	if (!syscall(SYS_capget, &header, data) &&
	    (data[CAP_TO_INDEX(CAP_IPC_LOCK)].effective &
	     CAP_TO_MASK(CAP_IPC_LOCK)))
		return 1;
	if (getrlimit(RLIMIT_MEMLOCK, &lim))
		return 0;
	if (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur >= need)
		return 1;
	if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < need)
		return 0;
	lim.rlim_cur = lim.rlim_max;
	return !setrlimit(RLIMIT_MEMLOCK, &lim);
}

/*
 * A GPU address at which BIND_MAX bytes can be bound. The driver keeps
 * ranges of every VM for its own mappings, where a bind fails with EINVAL,
 * and GET_PARAMS does not list them: take the first address, in steps of
 * 4 GiB below the kernel range, at which the ordinary buffer object @handle
 * (of BIND_MAX bytes) binds.
 */
static uint64_t find_va(int fd, uint32_t vm, uint32_t handle, uint64_t start,
			uint64_t end)
{
	uint64_t va;

	for (va = start + (4ULL << 30); va + BIND_MAX <= end; va += 4ULL << 30) {
		if (bind(fd, vm, handle, va, BIND_MAX, DRM_ASAHI_BIND_READ))
			continue;
		return unbind(fd, vm, va, BIND_MAX) ? 0 : va;
	}
	return 0;
}

FIXTURE(asahi)
{
	int fd;
	struct drm_asahi_params_global params;
	uint32_t vm;
	uint64_t va;
};

FIXTURE_SETUP(asahi)
{
	struct drm_asahi_get_params get = {
		.pointer = (uintptr_t)&self->params,
		.size = sizeof(self->params),
	};

	self->fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (self->fd < 0 || !is_asahi(self->fd))
		SKIP(return, "renderD128 is not an asahi render node");
	memset(&self->params, 0, sizeof(self->params));
	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GET_PARAMS, &get));
	if (!(self->params.features & DRM_ASAHI_FEATURE_USERPTR))
		SKIP(return, "no DRM_ASAHI_FEATURE_USERPTR");
	if (!memlock_room())
		SKIP(return, "RLIMIT_MEMLOCK has no room for the %lu bytes a test pins",
		     BIND_MAX);

	uint64_t kernel = self->params.vm_kernel_min_size > (32ULL << 30) ?
			  self->params.vm_kernel_min_size : 32ULL << 30;
	struct drm_asahi_vm_create vm = {
		.kernel_start = self->params.vm_end - kernel,
		.kernel_end = self->params.vm_end,
	};
	/* Lives as long as the file, so that the client's total stays put. */
	struct drm_asahi_gem_create probe = {
		.size = BIND_MAX, .flags = DRM_ASAHI_GEM_VM_PRIVATE,
	};

	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_VM_CREATE, &vm));
	self->vm = vm.vm_id;
	probe.vm_id = self->vm;
	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_CREATE, &probe));
	self->va = find_va(self->fd, self->vm, probe.handle,
			   self->params.vm_start, vm.kernel_start);
	ASSERT_NE(0, self->va);
}

FIXTURE_TEARDOWN(asahi)
{
	close(self->fd);
}

TEST_F(asahi, anonymous_memory_round_trip)
{
	const uint64_t size = 4 * PAGE;
	long pin0 = vm_pin_kb();
	long long total0 = client_total(self->fd);
	void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint32_t h = 0;

	ASSERT_NE(MAP_FAILED, mem);
	ASSERT_GE(total0, 0);
	ASSERT_EQ(0, userptr(self->fd, mem, size, DRM_ASAHI_GEM_VM_PRIVATE,
			     self->vm, &h));
	EXPECT_EQ(pin0 + (long)(size >> 10), vm_pin_kb());
	EXPECT_EQ(total0 + (long long)size, client_total(self->fd));
	EXPECT_EQ(0, bind(self->fd, self->vm, h, self->va, size,
			  DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE));

	/* No CPU mapping, advice, export or special binding through DRM. */
	struct drm_asahi_gem_mmap_offset off = { .handle = h };
	struct drm_asahi_gem_madvise madv = {
		.handle = h, .madv = DRM_ASAHI_GEM_MADV_DONTNEED,
	};
	struct drm_prime_handle prime = { .handle = h, .flags = DRM_CLOEXEC };
	struct drm_asahi_gem_bind_object ts = {
		.op = DRM_ASAHI_BIND_OBJECT_OP_BIND,
		.flags = DRM_ASAHI_BIND_OBJECT_USAGE_TIMESTAMPS,
		.handle = h, .range = PAGE,
	};

	EXPECT_EQ(-EINVAL, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &off));
	EXPECT_EQ(-EINVAL, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_MADVISE, &madv));
	EXPECT_NE(0, ioc(self->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime));
	EXPECT_NE(0, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_BIND_OBJECT, &ts));

	/* Unmapped while bound: the object keeps the pages until it goes. */
	ASSERT_EQ(0, munmap(mem, size));
	EXPECT_EQ(pin0 + (long)(size >> 10), vm_pin_kb());
	EXPECT_EQ(0, unbind(self->fd, self->vm, self->va, size));
	close_handle(self->fd, h);
	EXPECT_EQ(pin0, vm_pin_kb());
	EXPECT_EQ(total0, client_total(self->fd));
}

TEST_F(asahi, bad_ranges)
{
	char *mem = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	long pin0 = vm_pin_kb();
	uint32_t h = 0;

	ASSERT_NE(MAP_FAILED, mem);
	ASSERT_EQ(0, munmap(mem + 2 * PAGE, PAGE));
	EXPECT_EQ(-EINVAL, userptr(self->fd, mem + 4096, PAGE, 0, 0, &h));
	EXPECT_EQ(-EINVAL, userptr(self->fd, mem, PAGE + 4096, 0, 0, &h));
	EXPECT_EQ(-EINVAL, userptr(self->fd, mem, 0, 0, 0, &h));
	EXPECT_EQ(-EINVAL, userptr(self->fd, mem, PAGE, 1u << 0 /* WRITEBACK */, 0, &h));
	EXPECT_EQ(-EINVAL, userptr(self->fd, mem, PAGE, 1u << 7, 0, &h));
	/* Two pages pin, the third is a hole: nothing stays pinned. */
	EXPECT_EQ(-EFAULT, userptr(self->fd, mem, 3 * PAGE, 0, 0, &h));
	EXPECT_EQ(pin0, vm_pin_kb());
	/* A CPU mapping of another buffer object is a PFN mapping. */
	struct drm_asahi_gem_create bo = { .size = PAGE };
	struct drm_asahi_gem_mmap_offset off = { 0 };
	void *bo_map;

	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_CREATE, &bo));
	off.handle = bo.handle;
	ASSERT_EQ(0, ioc(self->fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &off));
	bo_map = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED,
		      self->fd, off.offset);
	ASSERT_NE(MAP_FAILED, bo_map);
	EXPECT_EQ(-EFAULT, userptr(self->fd, bo_map, PAGE, 0, 0, &h));
	EXPECT_EQ(-EFAULT, userptr(self->fd, bo_map, PAGE,
				   DRM_ASAHI_GEM_USERPTR_READ_ONLY, 0, &h));
	munmap(bo_map, PAGE);
	close_handle(self->fd, bo.handle);
	EXPECT_EQ(pin0, vm_pin_kb());
	munmap(mem, 2 * PAGE);
}

/* A read-only shared mapping of a file the caller cannot write. */
TEST_F(asahi, read_only_file_pages)
{
	const char *path = "/usr/bin/sh";
	int file = open(path, O_RDONLY | O_CLOEXEC);
	uint32_t h = 0;

	ASSERT_GE(file, 0);
	if (lseek(file, 0, SEEK_END) < (off_t)(2 * PAGE))
		SKIP(return, "%s is smaller than two pages", path);
	void *mem = mmap(NULL, 2 * PAGE, PROT_READ, MAP_SHARED, file, 0);

	ASSERT_NE(MAP_FAILED, mem);
	EXPECT_EQ(-EFAULT, userptr(self->fd, mem, 2 * PAGE, 0, 0, &h));
	ASSERT_EQ(0, userptr(self->fd, mem, 2 * PAGE,
			     DRM_ASAHI_GEM_USERPTR_READ_ONLY, 0, &h));
	EXPECT_EQ(-EACCES, bind(self->fd, self->vm, h, self->va, 2 * PAGE,
				DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE));
	EXPECT_EQ(-EACCES, bind(self->fd, self->vm, h, self->va, 2 * PAGE,
				DRM_ASAHI_BIND_WRITE));
	EXPECT_EQ(0, bind(self->fd, self->vm, h, self->va, 2 * PAGE,
			  DRM_ASAHI_BIND_READ));
	EXPECT_EQ(0, unbind(self->fd, self->vm, self->va, 2 * PAGE));
	close_handle(self->fd, h);
	munmap(mem, 2 * PAGE);
	close(file);
}

/*
 * RLIMIT_MEMLOCK bounds the pages of a caller without CAP_IPC_LOCK. The test
 * gives the capability up first (the harness runs each test in a process of
 * its own), so a request above the limit has to fail whoever runs it. It
 * lowers the soft limit to what the process has pinned already plus 16 pages,
 * or as many as the hard limit has room for, and is skipped where that is
 * less than two.
 */
TEST_F(asahi, memlock_limit)
{
	struct rlimit old, lim;
	long pin0 = vm_pin_kb();
	uint64_t pinned = pin0 > 0 ? (uint64_t)pin0 << 10 : 0;
	uint64_t pages = 16, size;
	uint32_t h = 0;
	void *mem;

	ASSERT_EQ(0, drop_cap_ipc_lock());
	ASSERT_EQ(0, getrlimit(RLIMIT_MEMLOCK, &old));
	if (old.rlim_max != RLIM_INFINITY) {
		uint64_t room = old.rlim_max > pinned ?
				(old.rlim_max - pinned) / PAGE : 0;

		if (room < pages)
			pages = room;
	}
	if (pages < 2)
		SKIP(return, "no room for two pages in the RLIMIT_MEMLOCK hard limit of %llu bytes",
		     (unsigned long long)old.rlim_max);
	size = 4 * pages * PAGE;
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, mem);
	lim.rlim_cur = pinned + pages * PAGE;
	lim.rlim_max = old.rlim_max;
	ASSERT_EQ(0, setrlimit(RLIMIT_MEMLOCK, &lim));
	ASSERT_EQ(-ENOMEM, userptr(self->fd, mem, size, 0, 0, &h));
	EXPECT_EQ(pin0, vm_pin_kb());
	EXPECT_EQ(0, userptr(self->fd, mem, pages / 2 * PAGE, 0, 0, &h));
	/* The pages already pinned count against the limit too. */
	EXPECT_EQ(-ENOMEM, userptr(self->fd, (char *)mem + pages / 2 * PAGE,
				   pages * PAGE, 0, 0, &h));
	close_handle(self->fd, h);
	setrlimit(RLIMIT_MEMLOCK, &old);
	munmap(mem, size);
}

/* After fork the parent keeps its pinned pages; the child writes a copy. */
TEST_F(asahi, fork_keeps_the_parent_pages)
{
	uint32_t *mem = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint32_t h = 0;
	int status;

	ASSERT_NE(MAP_FAILED, mem);
	mem[0] = 0x1234;
	ASSERT_EQ(0, userptr(self->fd, mem, PAGE, 0, 0, &h));
	pid_t pid = fork();

	ASSERT_GE(pid, 0);
	if (pid == 0) {
		mem[0] = 0xdead;
		_exit(mem[0] == 0xdead ? 0 : 1);
	}
	ASSERT_EQ(pid, waitpid(pid, &status, 0));
	EXPECT_EQ(0, WEXITSTATUS(status));
	EXPECT_EQ(0x1234, mem[0]);
	close_handle(self->fd, h);
	munmap(mem, PAGE);
}

TEST_HARNESS_MAIN
