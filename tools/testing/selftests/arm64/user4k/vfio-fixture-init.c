// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "../../../../../include/uapi/linux/prctl.h"
#define __user
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../../../../include/uapi/linux/vfio.h"
#include "../../../../../drivers/vfio/vfio_iommu_type1_test.h"

#define IOVA (256UL * 1024 * 1024)
static unsigned int number, failures;

static void result(int ok, const char *name)
{
	printf("%s %u - %s (errno=%d)\n", ok ? "ok" : "not ok", ++number, name,
	       errno);
	failures += !ok;
}

static int state(int fd, struct vfio_type1_test_cmd *cmd)
{
	memset(cmd, 0, sizeof(*cmd));
	return ioctl(fd, VFIO_TYPE1_TEST_STATE, cmd);
}

static int domain(int fd, unsigned int which, int add, size_t page, size_t fail)
{
	struct vfio_type1_test_cmd cmd = {
		.domain = which,
		.size = page,
		.fail_after = fail,
	};

	return ioctl(fd,
		     add ? VFIO_TYPE1_TEST_DOMAIN_ADD :
			   VFIO_TYPE1_TEST_DOMAIN_DEL,
		     &cmd);
}

static int rw(int fd, size_t offset, void *data, size_t size,
	      unsigned int flags)
{
	struct vfio_type1_test_cmd cmd = {
		.iova = IOVA + offset,
		.size = size,
		.buffer = (uintptr_t)data,
		.flags = flags,
	};

	return ioctl(fd, VFIO_TYPE1_TEST_RW, &cmd);
}

static int map(int fd, void *data, size_t size)
{
	struct vfio_iommu_type1_dma_map cmd = {
		.argsz = sizeof(cmd),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.iova = IOVA,
		.vaddr = (uintptr_t)data,
		.size = size,
	};

	return ioctl(fd, VFIO_IOMMU_MAP_DMA, &cmd);
}

static int unmap(int fd, size_t size)
{
	struct vfio_iommu_type1_dma_unmap cmd = {
		.argsz = sizeof(cmd),
		.iova = IOVA,
		.size = size,
	};
	int ret = ioctl(fd, VFIO_IOMMU_UNMAP_DMA, &cmd);

	return ret ? ret : cmd.size == size ? 0 : -2;
}

static int dirty(int fd, unsigned int flags)
{
	struct vfio_iommu_type1_dirty_bitmap cmd = { .argsz = sizeof(cmd),
						     .flags = flags };

	return ioctl(fd, VFIO_IOMMU_DIRTY_PAGES, &cmd);
}

static int bitmap(int fd, size_t size, uint64_t *bits)
{
	unsigned char
		buffer[sizeof(struct vfio_iommu_type1_dirty_bitmap) +
		       sizeof(struct vfio_iommu_type1_dirty_bitmap_get)] = {};
	struct vfio_iommu_type1_dirty_bitmap *cmd = (void *)buffer;
	struct vfio_iommu_type1_dirty_bitmap_get *range = (void *)cmd->data;

	cmd->argsz = sizeof(buffer);
	cmd->flags = VFIO_IOMMU_DIRTY_PAGES_FLAG_GET_BITMAP;
	range->iova = IOVA;
	range->size = size;
	range->bitmap.pgsize = 4096;
	range->bitmap.size = sizeof(*bits);
	range->bitmap.data = (void *)bits;
	*bits = 0;
	return ioctl(fd, VFIO_IOMMU_DIRTY_PAGES, buffer);
}

static int all_bytes(const unsigned char *data, size_t size,
		     unsigned char value)
{
	for (size_t i = 0; i < size; i++)
		if (data[i] != value)
			return 0;
	return 1;
}

static void external_span(int backing, size_t native)
{
	const size_t fine = 4096, count = native / fine;
	uint64_t bits;

	for (unsigned int mapped = 0; mapped < 2; mapped++) {
		struct vfio_type1_test_cmd cmd;
		struct vfio_iommu_type1_dma_unmap remove = {
			.argsz = sizeof(remove),
			.iova = IOVA,
			.size = fine,
		};
		int fd = open("/dev/vfio-test", O_RDWR), status = 0;
		pid_t child;

		result(fd >= 0 && (!mapped || !domain(fd, 0, 1, fine, 0)),
		       "span fixture opens lazy or mapped type1 container");
		if (fd < 0)
			continue;
		result(!dirty(fd, VFIO_IOMMU_DIRTY_PAGES_FLAG_START),
		       "span dirty tracking starts before fine mappings");
		child = fork();
		if (!child) {
			unsigned char *data = mmap(NULL, native,
						   PROT_READ | PROT_WRITE,
						   MAP_SHARED, backing, 0);

			if (data == MAP_FAILED)
				_exit(1);
			for (size_t i = 0; i < count; i++) {
				struct vfio_iommu_type1_dma_map area = {
					.argsz = sizeof(area),
					.flags = VFIO_DMA_MAP_FLAG_READ |
						 VFIO_DMA_MAP_FLAG_WRITE,
					.iova = IOVA + i * fine,
					.vaddr = (uintptr_t)data + i * fine,
					.size = fine,
				};

				if (ioctl(fd, VFIO_IOMMU_MAP_DMA, &area))
					_exit(2);
			}
			cmd = (struct vfio_type1_test_cmd){ .iova = IOVA + 17 };
			_exit(ioctl(fd, VFIO_TYPE1_TEST_PIN, &cmd) ? 3 : 0);
		}
		result(child > 0 && waitpid(child, &status, 0) == child &&
			       WIFEXITED(status) && !WEXITSTATUS(status),
		       "source pins native page across independent fine MAP ioctls");
		result(!state(fd, &cmd) && !cmd.owner_users &&
			       cmd.external_refs == 1 &&
			       cmd.locked_bytes == count * native &&
			       cmd.owner_locked_bytes == count * native,
		       "every fine DMA area retains its own charge after source exit");
		cmd = (struct vfio_type1_test_cmd){ .iova = IOVA + 31 };
		result(!ioctl(fd, VFIO_TYPE1_TEST_PIN, &cmd) &&
			       !state(fd, &cmd) && cmd.external_refs == 2 &&
			       cmd.locked_bytes == count * native,
		       "cross-area duplicate pin reuses all cached constituent pins");
		result(!bitmap(fd, native, &bits) &&
			       bits == ((1ULL << count) - 1),
		       "cross-area dirty bitmap marks only each mapping's fine extent");
		result(!ioctl(fd, VFIO_IOMMU_UNMAP_DMA, &remove) &&
			       remove.size == fine && !state(fd, &cmd) &&
			       !cmd.external_refs && cmd.notifications == 1 &&
			       cmd.locked_bytes ==
				       (mapped ? (count - 1) * native : 0),
		       "unmapping one constituent drains references in all page constituents");
		remove.iova = IOVA + fine;
		remove.size = native - fine;
		result(!ioctl(fd, VFIO_IOMMU_UNMAP_DMA, &remove) &&
			       remove.size == native - fine &&
			       !state(fd, &cmd) && !cmd.locked_bytes &&
			       !cmd.mapped_bytes[0],
		       "remaining fine mappings unmap without pins or translations");
		close(fd);
	}
}

static void native_owner(int fd, int backing, unsigned char *data,
			 size_t native)
{
	struct vfio_type1_test_cmd cmd = {};
	struct vfio_iommu_type1_dma_unmap invalidate = {
		.argsz = sizeof(invalidate),
		.flags = VFIO_DMA_UNMAP_FLAG_VADDR,
		.iova = IOVA,
		.size = 2 * native,
	};
	struct vfio_iommu_type1_dma_map update = {
		.argsz = sizeof(update),
		.flags = VFIO_DMA_MAP_FLAG_VADDR,
		.iova = IOVA,
		.size = 2 * native,
		.vaddr = (uintptr_t)data,
	};
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3
	};
	struct __user_cap_data_struct saved[2], reduced[2];
	struct rlimit limit, empty;
	unsigned char bytes[64];
	int file_flags = fcntl(backing, F_GETFD), status = 0;
	pid_t child;
	int budget_ready, budget_captured, transferred;

	result(!ioctl(fd, VFIO_TYPE1_TEST_EMULATED, &cmd) &&
		       !domain(fd, 0, 1, 4096, 0),
	       "VADDR fixture retains production group restrictions");
	result(file_flags >= 0 &&
		       !fcntl(backing, F_SETFD, file_flags & ~FD_CLOEXEC),
	       "native source inherits exact backing descriptor");
	child = fork();
	if (!child) {
		char fd_arg[24], file_arg[24], native_arg[24];

		snprintf(fd_arg, sizeof(fd_arg), "%d", fd);
		snprintf(file_arg, sizeof(file_arg), "%d", backing);
		snprintf(native_arg, sizeof(native_arg), "%zu", native);
		if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE, 0UL, 0UL, 0UL, 0UL))
			_exit(98);
		execl("/init", "/init", "--native-source", fd_arg, file_arg,
		      native_arg, NULL);
		_exit(99);
	}
	if (file_flags >= 0)
		fcntl(backing, F_SETFD, file_flags);
	result(child > 0 && waitpid(child, &status, 0) == child &&
		       WIFEXITED(status) && !WEXITSTATUS(status) &&
		       !state(fd, &cmd) && !cmd.owner_users &&
		       !(cmd.flags & VFIO_TYPE1_TEST_STATE_FRAGMENTS) &&
		       cmd.locked_bytes == 2 * native &&
		       cmd.mapped_bytes[0] == 2 * native,
	       "exited native source owns legacy GUP pins before transfer");
	result(!ioctl(fd, VFIO_IOMMU_UNMAP_DMA, &invalidate),
	       "canonical VADDR invalidation leaves device translations alive");
	budget_captured = !getrlimit(RLIMIT_MEMLOCK, &limit) &&
			  !syscall(SYS_capget, &header, saved);
	budget_ready = budget_captured;
	if (budget_ready) {
		memcpy(reduced, saved, sizeof(saved));
		reduced[CAP_IPC_LOCK / 32].effective &=
			~(1U << (CAP_IPC_LOCK % 32));
		empty = (struct rlimit){ .rlim_cur = 0,
					 .rlim_max = limit.rlim_max };
		budget_ready = !setrlimit(RLIMIT_MEMLOCK, &empty) &&
			       !syscall(SYS_capset, &header, reduced);
	}
	errno = 0;
	result(budget_ready && ioctl(fd, VFIO_IOMMU_MAP_DMA, &update) < 0 &&
		       errno == ENOMEM && !state(fd, &cmd) &&
		       !cmd.owner_users &&
		       !(cmd.flags & VFIO_TYPE1_TEST_STATE_FRAGMENTS) &&
		       cmd.locked_bytes == 2 * native &&
		       cmd.mapped_bytes[0] == 2 * native,
	       "failed owner credit preserves native pins, metadata and charge");
	if (budget_captured) {
		result(!syscall(SYS_capset, &header, saved) &&
			       !setrlimit(RLIMIT_MEMLOCK, &limit),
		       "restore original memlock budget and effective capability");
	} else {
		result(0, "owner budget setup failed");
	}
	transferred = !ioctl(fd, VFIO_IOMMU_MAP_DMA, &update) &&
		      !state(fd, &cmd) && cmd.owner_users &&
		      (cmd.flags & VFIO_TYPE1_TEST_STATE_FRAGMENTS) &&
		      cmd.locked_bytes == 2 * native &&
		      cmd.owner_locked_bytes == 2 * native;
	result(transferred,
	       "canonical VADDR retry adopts native pins for alternative owner");
	/* Dependent CPU access is prohibited while VADDR remains invalid. */
	if (!transferred)
		return;
	result(!rw(fd, 0, bytes, sizeof(bytes), 0) &&
		       all_bytes(bytes, sizeof(bytes), 0x64),
	       "new owner accesses retained native buffer with its own mm authorization");
	cmd = (struct vfio_type1_test_cmd){ .flags = 1 };
	result(!ioctl(fd, VFIO_TYPE1_TEST_EMULATED, &cmd),
	       "external consumer rejoins after VADDR update");
	cmd = (struct vfio_type1_test_cmd){ .iova = IOVA + 17 };
	result(!ioctl(fd, VFIO_TYPE1_TEST_PIN, &cmd) && !state(fd, &cmd) &&
		       cmd.locked_bytes == 2 * native && cmd.external_refs == 1,
	       "external page array reuses adopted pins without new charge");
	result(!unmap(fd, 2 * native) && !state(fd, &cmd) &&
		       !cmd.locked_bytes && !cmd.owner_locked_bytes &&
		       !cmd.external_refs && !cmd.mapped_bytes[0],
	       "owner transfer final cleanup refunds recipient and clears domain");
	result(!domain(fd, 0, 0, 0, 0), "owner transfer domain detaches");
}

static int exercise(size_t native)
{
	struct vfio_type1_test_cmd cmd;
	struct vfio_iommu_type1_info info = { .argsz = sizeof(info) };
	struct vfio_iommu_type1_dma_map bad = { .argsz = 8 };
	unsigned char bytes[64], disk[64];
	uint64_t bits = 0;
	int fd = open("/dev/vfio-test", O_RDWR),
	    backing = memfd_create("vfio", MFD_CLOEXEC);
	unsigned char *data = MAP_FAILED;
	int status = 0;
	pid_t child;
	size_t length = native + 4096;

	printf("TAP version 13\n# real type1 ioctl ABI=%luK native=%zuK\n",
	       getauxval(AT_PAGESZ) / 1024, native / 1024);
	result(fd >= 0 && backing >= 0 && !ftruncate(backing, 2 * native),
	       "open privileged recording-domain fixture and backing");
	if (fd < 0 || backing < 0 || failures)
		goto out;
	data = mmap(NULL, 2 * native, PROT_READ | PROT_WRITE, MAP_SHARED,
		    backing, 0);
	result(data != MAP_FAILED,
	       "map native-sized shmem backing in actual process mm");
	if (data == MAP_FAILED)
		goto out;
	memset(data, 0x64, 2 * native);
	result(!ioctl(fd, VFIO_IOMMU_GET_INFO, &info) &&
		       (info.iova_pgsizes & 4096),
	       "type1 GET_INFO reports fine IOVA capability");
	errno = 0;
	result(ioctl(fd, VFIO_IOMMU_MAP_DMA, &bad) < 0 && errno == EINVAL,
	       "canonical MAP rejects truncated argument");
	errno = 0;
	result(ioctl(fd, VFIO_IOMMU_MAP_DMA, (void *)1) < 0 && errno == EFAULT,
	       "canonical MAP rejects inaccessible userspace argument");
	result(!map(fd, data, 2 * native), "lazy full-range MAP from real EL0");
	result(!state(fd, &cmd) && !cmd.locked_bytes && !cmd.mapped_bytes[0],
	       "lazy MAP acquires no DMA pins or translations");
	result(!rw(fd, 0, bytes, sizeof(bytes), 0) &&
		       all_bytes(bytes, sizeof(bytes), 0x64),
	       "CPU-only GET reads actual source bytes");
	result(!state(fd, &cmd) && !cmd.locked_bytes,
	       "CPU GET does not charge long-term pins");
	result(!dirty(fd, VFIO_IOMMU_DIRTY_PAGES_FLAG_START),
	       "canonical dirty tracking starts");
	cmd = (struct vfio_type1_test_cmd){ .iova = IOVA + 17 };
	result(!ioctl(fd, VFIO_TYPE1_TEST_PIN, &cmd) && !state(fd, &cmd) &&
		       cmd.locked_bytes == native && cmd.external_refs == 1,
	       "external page array preserves unaligned caller byte offset");
	cmd = (struct vfio_type1_test_cmd){ .iova = IOVA + 31 };
	result(!ioctl(fd, VFIO_TYPE1_TEST_PIN, &cmd) && !state(fd, &cmd) &&
		       cmd.locked_bytes == native && cmd.external_refs == 2,
	       "duplicate external pin charges physical native page once");
	result(!bitmap(fd, 2 * native, &bits) &&
		       bits == ((1ULL << (native / 4096)) - 1),
	       "dirty bitmap covers every fine unit in native external page");
	result(!unmap(fd, 2 * native),
	       "canonical UNMAP drains callback-held external refs");
	result(!state(fd, &cmd) && !cmd.locked_bytes && !cmd.external_refs &&
		       cmd.notifications == 1,
	       "external-unpin callback refunds all pins");
	result(!dirty(fd, VFIO_IOMMU_DIRTY_PAGES_FLAG_STOP),
	       "dirty tracking stops");
	result(!domain(fd, 0, 1, 4096, 0),
	       "recording4K domain attaches before child MAP");
	child = fork();
	if (!child) {
		unsigned char *source = mmap(NULL, 2 * native,
					     PROT_READ | PROT_WRITE, MAP_SHARED,
					     backing, 0);
		int ret = source == MAP_FAILED ? -1 :
						 map(fd, source + 4096, length);

		_exit(ret ? 1 : 0);
	}
	result(child > 0 && waitpid(child, &status, 0) == child &&
		       WIFEXITED(status) && !WEXITSTATUS(status),
	       "fine-offset MAP retains real child pins after exit");
	result(!state(fd, &cmd) && !cmd.owner_users &&
		       cmd.locked_bytes == 2 * native &&
		       cmd.owner_locked_bytes == 2 * native &&
		       cmd.mapped_bytes[0] == length,
	       "exited source mm retains exact physical-page charge and translation length");
	errno = 0;
	result(rw(fd, 0, bytes, sizeof(bytes), 0) < 0 && errno == EFAULT,
	       "foreign process CPU access preserves source-mm authorization");
	result(!rw(fd, 0, bytes, sizeof(bytes), VFIO_TYPE1_TEST_WORKER) &&
		       all_bytes(bytes, sizeof(bytes), 0x64),
	       "kernel consumer reads exited source pins");
	errno = 0;
	result(domain(fd, 1, 1, 4096, 4096) < 0 && errno == ENOMEM &&
		       !state(fd, &cmd) && !cmd.mapped_bytes[1] &&
		       cmd.mapped_bytes[0] == length &&
		       cmd.locked_bytes == 2 * native,
	       "failed replay leaves existing domain and pins intact");
	result(!domain(fd, 1, 1, 4096, 0) && !state(fd, &cmd) &&
		       cmd.mapped_bytes[0] == length &&
		       cmd.mapped_bytes[1] == length &&
		       cmd.locked_bytes == 2 * native,
	       "retry replays cached pins without resurrecting source mm");
	result(!dirty(fd, VFIO_IOMMU_DIRTY_PAGES_FLAG_START),
	       "fine DMA dirty tracking starts");
	memset(bytes, 0x9a, sizeof(bytes));
	result(!rw(fd, 4096 - 32, bytes, sizeof(bytes),
		   VFIO_TYPE1_TEST_WRITE | VFIO_TYPE1_TEST_WORKER) &&
		       pread(backing, disk, sizeof(disk), 8192 - 32) ==
			       sizeof(disk) &&
		       all_bytes(disk, sizeof(disk), 0x9a),
	       "cached worker write dirties original shmem bytes");
	result(!bitmap(fd, length, &bits) && bits == 3,
	       "canonical bitmap returns exact two4K units touched by worker write");
	result(!unmap(fd, length),
	       "fine UNMAP synchronizes both recording domains");
	result(!state(fd, &cmd) && !cmd.locked_bytes && !cmd.mapped_bytes[0] &&
		       !cmd.mapped_bytes[1],
	       "source-exit cleanup refunds pins and clears all translations");
	result(!domain(fd, 1, 0, 0, 0) && !domain(fd, 0, 0, 0, 0),
	       "both domains detach after complete cleanup");
	external_span(backing, native);
	if (getauxval(AT_PAGESZ) != native)
		native_owner(fd, backing, data, native);
out:
	if (data != MAP_FAILED)
		munmap(data, 2 * native);
	if (backing >= 0)
		close(backing);
	if (fd >= 0)
		close(fd);
	printf("1..%u\n# type1 ioctl failures=%u\n", number, failures);
	return !!failures;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 5 && !strcmp(argv[1], "--native-source")) {
		size_t native = strtoul(argv[4], NULL, 10);
		int fd = atoi(argv[2]), backing = atoi(argv[3]);
		void *data;

		if (getauxval(AT_PAGESZ) != native)
			return 2;
		data = mmap(NULL, 2 * native, PROT_READ | PROT_WRITE,
			    MAP_SHARED, backing, 0);
		return data == MAP_FAILED || map(fd, data, 2 * native) ? 1 : 0;
	}
	if (argc == 4 && !strcmp(argv[1], "--test") && getppid() == 1) {
		size_t expected = strtoul(argv[3], NULL, 10);

		if (getauxval(AT_PAGESZ) != expected)
			return 2;
		return exercise(strtoul(argv[2], NULL, 10));
	}
	if (getpid() != 1 || argc != 1)
		return 2;
	if (mount("proc", "/proc", "proc", 0, NULL) ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL))
		return 3;
	FILE *dev = fopen("/sys/class/misc/aurora-vfio-type1-test/dev", "r");
	unsigned int major, minor;

	if (!dev || fscanf(dev, "%u:%u", &major, &minor) != 2)
		return 4;
	fclose(dev);
	if (mknod("/dev/vfio-test", S_IFCHR | 0600, makedev(major, minor)))
		return 5;
	size_t native = getauxval(AT_PAGESZ),
	       targets[] = { native, 4096, 16384 };
	int ok = 1;

	int compat_only = getenv("VFIO_COMPAT_ONLY") != NULL;

	for (unsigned int i = 0; i < 3; i++) {
		if (compat_only)
			break;
		size_t target = targets[i];
		pid_t child;
		int status = 0;

		if (target > native || (i && target == native))
			continue;
		child = fork();
		if (!child) {
			char native_arg[24], target_arg[24];

			snprintf(native_arg, sizeof(native_arg), "%zu", native);
			snprintf(target_arg, sizeof(target_arg), "%zu", target);
			if (prctl(PR_AURORA_SET_EXEC_PAGE_SIZE,
				  target == native ? 0UL : target, 0UL, 0UL,
				  0UL))
				_exit(3);
			execl("/init", "/init", "--test", native_arg,
			      target_arg, NULL);
			_exit(4);
		}
		ok &= child > 0 && waitpid(child, &status, 0) == child &&
		      WIFEXITED(status) && !WEXITSTATUS(status);
		printf("# actual ABI=%zuK child_status=%d\n", target / 1024,
		       status);
	}
	const char *expected_arg = getenv("VFIO_COMPAT_PAGES");
	size_t expected = expected_arg ? strtoul(expected_arg, NULL, 10) : 4096;
	pid_t compat = fork();
	int compat_status = 0;

	if (!compat) {
		char native_arg[24], page_arg[24];

		snprintf(native_arg, sizeof(native_arg), "%zu", native);
		snprintf(page_arg, sizeof(page_arg), "%zu", expected);
		execl("/compat-vfio", "/compat-vfio", native_arg, page_arg,
		      NULL);
		_exit(98);
	}
	ok &= compat > 0 && waitpid(compat, &compat_status, 0) == compat &&
	      WIFEXITED(compat_status) && !WEXITSTATUS(compat_status);
	printf("# AArch32 expected=%zuK child_status=%d\n", expected / 1024,
	       compat_status);
	printf("VFIO TYPE1 IOCTL %s\n", ok ? "PASS" : "FAIL");
	reboot(RB_POWER_OFF);
	return !ok;
}
