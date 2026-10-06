// SPDX-License-Identifier: GPL-2.0-only
/* Disposable VM PID1: real IOMMUFD ioctls against mock AMDv1 page tables. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/iommufd.h>
#include "../../../../../drivers/iommu/iommufd/iommufd_test.h"

#define SET_EXEC_PAGE_SIZE 0x41555001
#define VA 0x200000000UL
#define IOVA 0x10000000UL
#define ALIAS 0x12000000UL
#define MAP_FLAGS                                              \
	(IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_READABLE | \
	 IOMMU_IOAS_MAP_WRITEABLE)
#define CHECK(c)                                                            \
	do {                                                                \
		if (!(c)) {                                                 \
			printf("FAIL line %d: %s errno=%d\n", __LINE__, #c, \
			       errno);                                      \
			return 1;                                           \
		}                                                           \
	} while (0)

static size_t native_ps, user_ps;
struct context {
	int fd, access;
	uint32_t ioas, hwpt[2], dev[2];
};

static int drop_pin_cap(void)
{
	struct __user_cap_header_struct h = {
		.version = _LINUX_CAPABILITY_VERSION_3
	};
	struct __user_cap_data_struct d[2];
	unsigned int mask = 1U << (CAP_IPC_LOCK % 32);

	if (syscall(SYS_capget, &h, d))
		return -1;
	d[CAP_IPC_LOCK / 32].effective &= ~mask;
	d[CAP_IPC_LOCK / 32].permitted &= ~mask;
	return syscall(SYS_capset, &h, d);
}

static int limit_bytes(size_t bytes)
{
	struct rlimit r;

	if (getrlimit(RLIMIT_MEMLOCK, &r))
		return -1;
	r.rlim_cur = bytes;
	return setrlimit(RLIMIT_MEMLOCK, &r);
}

static long stat_kb(pid_t pid, const char *key)
{
	char path[80], line[256];
	long result = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key))) {
			result = strtol(line + strlen(key), NULL, 10);
			break;
		}
	fclose(f);
	return result;
}

static int new_domain(struct context *c, unsigned int index)
{
	struct iommu_test_cmd cmd = { .size = sizeof(cmd),
				      .op = IOMMU_TEST_OP_MOCK_DOMAIN,
				      .id = c->ioas };
	int rc = ioctl(c->fd, IOMMU_TEST_CMD, &cmd);

	if (!rc) {
		c->hwpt[index] = cmd.mock_domain.out_hwpt_id;
		c->dev[index] = cmd.mock_domain.out_idev_id;
	}
	return rc;
}

static int context_open_common(struct context *c, unsigned int mm_account,
			       bool domain, bool pins)
{
	struct iommu_ioas_alloc a = { .size = sizeof(a) };
	struct iommu_option o = { .size = sizeof(o),
				  .option_id = IOMMU_OPTION_RLIMIT_MODE,
				  .op = IOMMU_OPTION_OP_SET,
				  .val64 = mm_account };
	struct iommu_test_cmd access = { .size = sizeof(access),
					 .op = IOMMU_TEST_OP_CREATE_ACCESS };

	memset(c, 0, sizeof(*c));
	c->fd = open("/dev/iommu", O_RDWR);
	if (c->fd < 0 || ioctl(c->fd, IOMMU_OPTION, &o) ||
	    ioctl(c->fd, IOMMU_IOAS_ALLOC, &a))
		return -1;
	c->ioas = a.out_ioas_id;
	if (domain && new_domain(c, 0))
		return -1;
	access.id = c->ioas;
	access.create_access.flags =
		pins ? MOCK_FLAGS_ACCESS_CREATE_NEEDS_PIN_PAGES : 0;
	if (ioctl(c->fd, IOMMU_TEST_CMD, &access))
		return -1;
	c->access = access.create_access.out_access_fd;
	return 0;
}

static int context_open(struct context *c, unsigned int mm_account)
{
	return context_open_common(c, mm_account, true, false);
}

static void context_close(struct context *c)
{
	close(c->access);
	close(c->fd);
}

static int map_range(struct context *c, void *p, size_t len, unsigned long iova)
{
	struct iommu_ioas_map m = { .size = sizeof(m),
				    .flags = MAP_FLAGS,
				    .ioas_id = c->ioas,
				    .user_va = (uintptr_t)p,
				    .length = len,
				    .iova = iova };

	return ioctl(c->fd, IOMMU_IOAS_MAP, &m);
}

static int unmap_range(struct context *c, size_t len, unsigned long iova)
{
	struct iommu_ioas_unmap u = { .size = sizeof(u),
				      .ioas_id = c->ioas,
				      .iova = iova,
				      .length = len };
	int rc = ioctl(c->fd, IOMMU_IOAS_UNMAP, &u);

	return rc ?: (u.length == len ? 0 : -1);
}

static int check_pa(struct context *c, unsigned int dom, void *p, size_t len,
		    unsigned long iova)
{
	struct iommu_test_cmd cmd = { .size = sizeof(cmd),
				      .op = IOMMU_TEST_OP_MD_CHECK_MAP,
				      .id = c->hwpt[dom],
				      .check_map = { .iova = iova,
						     .length = len,
						     .uptr = (uintptr_t)p } };

	return ioctl(c->fd, IOMMU_TEST_CMD, &cmd);
}

static int access_rw(struct context *c, void *p, size_t len, unsigned long iova,
		     unsigned int flags)
{
	struct iommu_test_cmd cmd = { .size = sizeof(cmd),
				      .op = IOMMU_TEST_OP_ACCESS_RW,
				      .id = c->access,
				      .access_rw = { .iova = iova,
						     .length = len,
						     .uptr = (uintptr_t)p,
						     .flags = flags } };

	return ioctl(c->fd, IOMMU_TEST_CMD, &cmd);
}

static bool filled(const void *ptr, unsigned char byte, size_t n)
{
	const unsigned char *p = ptr;

	for (size_t i = 0; i < n; i++)
		if (p[i] != byte)
			return false;
	return true;
}

static void *relocated(size_t len, unsigned char byte)
{
	void *old = mmap((void *)VA, len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
			 0);

	if (old == MAP_FAILED)
		return old;
	memset(old, byte, len);
	size_t distance = len > 32 * native_ps ? len + native_ps :
						 32 * native_ps;

	return mremap(old, len, len, MREMAP_MAYMOVE | MREMAP_FIXED,
		      (void *)(VA + distance + user_ps));
}

static int limits_and_lifetime(unsigned int mode)
{
	struct context c;
	size_t len = 2 * native_ps;
	unsigned char *p = relocated(3 * native_ps, 0x31), *buf = malloc(len);
	struct iommu_ioas_copy copy;

	CHECK(p != MAP_FAILED && buf);
	CHECK(!context_open(&c, mode));
	CHECK(!limit_bytes(len));
	CHECK(map_range(&c, p, 3 * native_ps, IOVA) == -1 && errno == ENOMEM);
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	CHECK(!map_range(&c, p, len, IOVA));
	CHECK(stat_kb(getpid(), "VmPin:") == (long)len / 1024);
	CHECK(stat_kb(getpid(), "VmLck:") == (mode ? (long)len / 1024 : 0));
	CHECK(!check_pa(&c, 0, p, len, IOVA));
	CHECK(map_range(&c, p + len, native_ps, ALIAS) == -1 &&
	      errno == ENOMEM);
	CHECK(stat_kb(getpid(), "VmPin:") == (long)len / 1024);
	copy = (struct iommu_ioas_copy){ .size = sizeof(copy),
					 .flags = MAP_FLAGS,
					 .src_ioas_id = c.ioas,
					 .dst_ioas_id = c.ioas,
					 .src_iova = IOVA + user_ps,
					 .dst_iova = ALIAS,
					 .length = len - user_ps };
	CHECK(!ioctl(c.fd, IOMMU_IOAS_COPY, &copy));
	CHECK(!new_domain(&c, 1));
	CHECK(!check_pa(&c, 1, p + user_ps, len - user_ps, ALIAS));
	CHECK(stat_kb(getpid(), "VmPin:") == (long)len / 1024);
	CHECK(mmap(p, len, PROT_READ | PROT_WRITE,
		   MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == p);
	memset(p, 0x5a, len);
	CHECK(check_pa(&c, 0, p, len, IOVA) == -1 && errno == EINVAL);
	CHECK(!access_rw(&c, buf, len, IOVA, MOCK_ACCESS_RW_SLOW_PATH));
	CHECK(filled(buf, 0x31, len));
	CHECK(!access_rw(&c, buf, len, IOVA, 0));
	CHECK(filled(buf, 0x5a, len));
	CHECK(!unmap_range(&c, len, IOVA));
	CHECK(stat_kb(getpid(), "VmPin:") ==
	      (long)(user_ps < native_ps ? len : len - user_ps) / 1024);
	CHECK(!unmap_range(&c, len - user_ps, ALIAS));
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	/* Fresh anonymous backing follows the shifted VA's native linear index.
	 * A non-native-aligned replacement spans three physical backing pages.
	 */
	size_t replacement_pages = user_ps < native_ps ? 3 : 2;

	if (replacement_pages == 3) {
		CHECK(map_range(&c, p, len, IOVA) == -1 && errno == ENOMEM);
		CHECK(stat_kb(getpid(), "VmPin:") == 0);
	}
	CHECK(!limit_bytes(replacement_pages * native_ps));
	CHECK(!map_range(&c, p, len, IOVA));
	CHECK(stat_kb(getpid(), "VmPin:") ==
	      (long)(replacement_pages * native_ps / 1024));
	CHECK(!unmap_range(&c, len, IOVA));
	CHECK(!munmap(p, 3 * native_ps));
	context_close(&c);
	free(buf);
	return 0;
}

static int holes_and_shared(unsigned int mode)
{
	struct context c;
	size_t len = 3 * user_ps, file_len = 4 * native_ps;
	unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
		      *buf = malloc(file_len);
	int file = memfd_create("iommu-shared", 0);

	CHECK(p != MAP_FAILED && buf && file >= 0);
	CHECK(!context_open(&c, mode) && !limit_bytes(8 * native_ps));
	memset(p, 0x31, len);
	CHECK(!munmap(p + user_ps, user_ps));
	CHECK(map_range(&c, p, len, IOVA) == -1 && errno == EFAULT);
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	CHECK(mmap(p + user_ps, user_ps, PROT_READ,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		   0) == p + user_ps);
	CHECK(map_range(&c, p, len, IOVA) == -1 && errno == EFAULT);
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	CHECK(!mprotect(p + user_ps, user_ps, PROT_READ | PROT_WRITE));
	CHECK(!map_range(&c, p, len, IOVA) && !check_pa(&c, 0, p, len, IOVA));
	CHECK(!unmap_range(&c, len, IOVA) && !munmap(p, len));
	CHECK(!ftruncate(file, file_len));
	p = mmap(NULL, 2 * user_ps, PROT_READ | PROT_WRITE, MAP_SHARED, file,
		 user_ps);
	CHECK(p != MAP_FAILED);
	CHECK(!map_range(&c, p, 2 * user_ps, IOVA));
	CHECK(!check_pa(&c, 0, p, 2 * user_ps, IOVA));
	CHECK(!munmap(p, 2 * user_ps));
	memset(buf, 0x93, 2 * user_ps);
	CHECK(!access_rw(&c, buf, 2 * user_ps, IOVA,
			 MOCK_ACCESS_RW_WRITE | MOCK_ACCESS_RW_SLOW_PATH));
	CHECK(!unmap_range(&c, 2 * user_ps, IOVA));
	CHECK(pread(file, buf, file_len, 0) == (ssize_t)file_len);
	CHECK(filled(buf, 0, user_ps));
	CHECK(filled(buf + user_ps, 0x93, 2 * user_ps));
	CHECK(filled(buf + 3 * user_ps, 0, file_len - 3 * user_ps));
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	close(file);
	context_close(&c);
	free(buf);
	return 0;
}

static int producer(int argc, char **argv)
{
	struct context c = { .fd = atoi(argv[4]),
			     .ioas = strtoul(argv[5], NULL, 0) };
	int ready = atoi(argv[6]), go = atoi(argv[7]);
	char byte = 'R';
	void *p;

	(void)argc;
	CHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL));
	CHECK(!drop_pin_cap());
	p = relocated(2 * native_ps, 0x6d);
	CHECK(p != MAP_FAILED);
	CHECK(!map_range(&c, p, 2 * native_ps, IOVA));
	CHECK(write(ready, &byte, 1) == 1);
	CHECK(read(go, &byte, 1) == 1 && byte == 'G');
	return 0;
}

static int destroy_id(struct context *c, uint32_t id)
{
	struct iommu_destroy cmd = { .size = sizeof(cmd), .id = id };

	return ioctl(c->fd, IOMMU_DESTROY, &cmd);
}

static int create_viommu(struct context *c, uint32_t *id, uint32_t *parent)
{
	struct iommu_hwpt_alloc hwpt = { .size = sizeof(hwpt),
					 .flags = IOMMU_HWPT_ALLOC_NEST_PARENT,
					 .dev_id = c->dev[0],
					 .pt_id = c->ioas };
	struct iommu_viommu_alloc cmd = { .size = sizeof(cmd),
					  .type = IOMMU_VIOMMU_TYPE_SELFTEST,
					  .dev_id = c->dev[0] };

	if (ioctl(c->fd, IOMMU_HWPT_ALLOC, &hwpt))
		return -1;
	*parent = hwpt.out_hwpt_id;
	cmd.hwpt_id = *parent;
	if (ioctl(c->fd, IOMMU_VIOMMU_ALLOC, &cmd))
		return -1;
	*id = cmd.out_viommu_id;
	return 0;
}

static int queue_alloc(struct context *c, uint32_t viommu, unsigned int index,
		       unsigned long iova, size_t length, uint32_t *id)
{
	struct iommu_hw_queue_alloc cmd = { .size = sizeof(cmd),
					    .viommu_id = viommu,
					    .type = IOMMU_HW_QUEUE_TYPE_SELFTEST,
					    .index = index,
					    .nesting_parent_iova = iova,
					    .length = length };
	int rc = ioctl(c->fd, IOMMU_HW_QUEUE_ALLOC, &cmd);

	if (!rc)
		*id = cmd.out_hw_queue_id;
	return rc;
}

static int queue_after_exit(struct context *c)
{
	uint32_t viommu, parent, id;

	CHECK(!create_viommu(c, &viommu, &parent));
	CHECK(!queue_alloc(c, viommu, 0, IOVA + native_ps / 4 + 3,
			   native_ps / 4, &id));
	CHECK(unmap_range(c, 2 * native_ps, IOVA) == -1 && errno == EBUSY);
	CHECK(!destroy_id(c, id));
	CHECK(!destroy_id(c, viommu));
	CHECK(!destroy_id(c, parent));
	return 0;
}

static int queue_ranges(unsigned int mode)
{
	struct context c;
	unsigned char *p = relocated(2 * native_ps, 0x71);
	uint32_t viommu, parent, q0, q1, unused;
	size_t offset = user_ps / 4 + 3, length = user_ps / 2 + 17;
	int file;

	CHECK(p != MAP_FAILED && !context_open(&c, mode));
	CHECK(!limit_bytes(8 * native_ps) &&
	      !create_viommu(&c, &viommu, &parent));
	/* Map only one user leaf: queue pinning must not require surrounding IOVA. */
	CHECK(!map_range(&c, p, user_ps, IOVA));
	CHECK(!check_pa(&c, 0, p, user_ps, IOVA));
	CHECK(queue_alloc(&c, viommu, 1, IOVA + offset, length, &unused) ==
		      -1 &&
	      errno == EIO);
	CHECK(!queue_alloc(&c, viommu, 0, IOVA + offset, length, &q0));
	CHECK(queue_alloc(&c, viommu, 0, IOVA + offset, length, &unused) ==
		      -1 &&
	      errno == EEXIST);
	CHECK(unmap_range(&c, user_ps, IOVA) == -1 && errno == EBUSY);
	CHECK(stat_kb(getpid(), "VmPin:") == (long)native_ps / 1024);
	/* Pins keep the old physical bytes after replacing the source VMA. */
	CHECK(mmap(p, user_ps, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == p);
	memset(p, 0x5a, user_ps);
	CHECK(!queue_alloc(&c, viommu, 1, IOVA + offset + 7, length - 7, &q1));
	CHECK(destroy_id(&c, q0) == -1 && errno == EBUSY);
	CHECK(!destroy_id(&c, q1));
	CHECK(unmap_range(&c, user_ps, IOVA) == -1 && errno == EBUSY);
	CHECK(!destroy_id(&c, q0));
	CHECK(!unmap_range(&c, user_ps, IOVA));
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	CHECK(queue_alloc(&c, viommu, 0, IOVA + offset, length, &unused) ==
		      -1 &&
	      errno == ENOENT);
	CHECK(!munmap(p, 2 * native_ps));

	/* Separate mappings of the same file byte range cannot form a physical run.
	 * The second area must fail after the first area's access was registered.
	 */
	file = memfd_create("queue-alias", 0);
	CHECK(file >= 0 && !ftruncate(file, native_ps));
	p = mmap((void *)VA, 2 * user_ps, PROT_NONE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	CHECK(p != MAP_FAILED);
	CHECK(mmap(p, user_ps, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
		   file, 0) == p);
	CHECK(mmap(p + user_ps, user_ps, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_FIXED, file, 0) == p + user_ps);
	CHECK(!map_range(&c, p, user_ps, IOVA));
	CHECK(queue_alloc(&c, viommu, 0, IOVA, 2 * user_ps, &unused) == -1 &&
	      errno == ENOENT);
	CHECK(!map_range(&c, p + user_ps, user_ps, IOVA + user_ps));
	CHECK(queue_alloc(&c, viommu, 0, IOVA, 2 * user_ps, &unused) == -1 &&
	      errno == EFAULT);
	CHECK(!unmap_range(&c, user_ps, IOVA));
	CHECK(!unmap_range(&c, user_ps, IOVA + user_ps));
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	/* The same discontinuity inside one provider must also release its access. */
	CHECK(!map_range(&c, p, 2 * user_ps, IOVA));
	CHECK(queue_alloc(&c, viommu, 0, IOVA, 2 * user_ps, &unused) == -1 &&
	      errno == EFAULT);
	CHECK(!unmap_range(&c, 2 * user_ps, IOVA));
	if (user_ps < native_ps) {
		/* Adjacent file quarters form a real physical run across two areas. */
		CHECK(mmap(p + user_ps, user_ps, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_FIXED, file,
			   user_ps) == p + user_ps);
		CHECK(!map_range(&c, p, user_ps, IOVA));
		CHECK(!map_range(&c, p + user_ps, user_ps, IOVA + user_ps));
		CHECK(!queue_alloc(&c, viommu, 0, IOVA + 11, 2 * user_ps - 22,
				   &q0));
		CHECK(unmap_range(&c, user_ps, IOVA) == -1 && errno == EBUSY);
		CHECK(unmap_range(&c, user_ps, IOVA + user_ps) == -1 &&
		      errno == EBUSY);
		CHECK(!destroy_id(&c, q0));
		CHECK(!unmap_range(&c, user_ps, IOVA));
		CHECK(!unmap_range(&c, user_ps, IOVA + user_ps));
	}
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	CHECK(!munmap(p, 2 * user_ps));
	close(file);
	CHECK(!destroy_id(&c, viommu) && !destroy_id(&c, parent));
	context_close(&c);
	printf("ok - hardware queue byte spans %zuK/%zuK %s, "
	       "relocated/overlap/rollback/unmap locks\n",
	       user_ps / 1024, native_ps / 1024, mode ? "MM" : "USER");
	return 0;
}

static int cross_process(unsigned int mode)
{
	struct context c;
	int ready[2], go[2], status;
	pid_t child;
	size_t child_ps = user_ps == native_ps ? 4096 : native_ps;
	size_t len = 2 * native_ps;
	unsigned char *p = mmap((void *)(VA + 128 * native_ps), len,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS |
					MAP_FIXED_NOREPLACE,
				-1, 0),
		      *buf = malloc(len);
	char byte;

	CHECK(p != MAP_FAILED && buf && !pipe(ready) && !pipe(go));
	CHECK(!context_open(&c, mode) && !limit_bytes(len));
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		char ps[24], native[24], fd[24], ioas[24], r[24], g[24];

		close(ready[0]);
		close(go[1]);
		snprintf(ps, sizeof(ps), "%zu", child_ps);
		snprintf(native, sizeof(native), "%zu", native_ps);
		snprintf(fd, sizeof(fd), "%d", c.fd);
		snprintf(ioas, sizeof(ioas), "%u", c.ioas);
		snprintf(r, sizeof(r), "%d", ready[1]);
		snprintf(g, sizeof(g), "%d", go[0]);
		if (prctl(SET_EXEC_PAGE_SIZE, child_ps, 0UL, 0UL, 0UL))
			_exit(4);
		execl("/init", "/init", "--producer", ps, native, fd, ioas, r,
		      g, NULL);
		_exit(5);
	}
	close(ready[1]);
	close(go[0]);
	CHECK(read(ready[0], &byte, 1) == 1 && byte == 'R');
	CHECK(stat_kb(child, "VmPin:") == (long)len / 1024);
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	if (!mode)
		CHECK(map_range(&c, p, native_ps, ALIAS) == -1 &&
		      errno == ENOMEM);
	else {
		CHECK(!map_range(&c, p, native_ps, ALIAS));
		CHECK(!unmap_range(&c, native_ps, ALIAS));
	}
	CHECK(!access_rw(&c, buf, len, IOVA, MOCK_ACCESS_RW_SLOW_PATH));
	CHECK(filled(buf, 0x6d, len));
	byte = 'G';
	CHECK(write(go[1], &byte, 1) == 1);
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	CHECK(!new_domain(&c, 1));
	CHECK(!access_rw(&c, buf, len, IOVA, MOCK_ACCESS_RW_SLOW_PATH));
	CHECK(filled(buf, 0x6d, len));
	CHECK(!queue_after_exit(&c));
	if (!mode)
		CHECK(map_range(&c, p, native_ps, ALIAS) == -1 &&
		      errno == ENOMEM);
	CHECK(!unmap_range(&c, len, IOVA));
	CHECK(!map_range(&c, p, len, ALIAS) && !unmap_range(&c, len, ALIAS));
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	close(ready[0]);
	close(go[1]);
	CHECK(!munmap(p, len));
	context_close(&c);
	free(buf);
	return 0;
}

struct stress_arg {
	struct context *c;
	void *p;
	unsigned int index;
	atomic_int *failed;
};
static void *stress_thread(void *opaque)
{
	struct stress_arg *a = opaque;
	unsigned long iova = IOVA + a->index * 0x100000UL;
	size_t len = 2 * native_ps;

	for (unsigned int round = 0; round < 64; round++) {
		if (map_range(a->c, a->p, len, iova) ||
		    (!(round % 16) && check_pa(a->c, 1, a->p, len, iova)) ||
		    unmap_range(a->c, len, iova)) {
			printf("FAIL concurrent map thread=%u round=%u errno=%d\n",
			       a->index, round, errno);
			atomic_store(a->failed, 1);
			break;
		}
	}
	return NULL;
}

static int concurrent_maps(unsigned int mode)
{
	struct context c;
	pthread_t threads[4];
	struct stress_arg args[4];
	atomic_int failed = 0;
	void *p = relocated(2 * native_ps, 0x47);

	CHECK(p != MAP_FAILED && !context_open(&c, mode));
	CHECK(!limit_bytes(32 * native_ps) && !new_domain(&c, 1));
	for (unsigned int i = 0; i < 4; i++) {
		args[i] = (struct stress_arg){ &c, p, i, &failed };
		CHECK(!pthread_create(&threads[i], NULL, stress_thread,
				      &args[i]));
	}
	for (unsigned int i = 0; i < 4; i++)
		CHECK(!pthread_join(threads[i], NULL));
	CHECK(!atomic_load(&failed));
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	CHECK(!munmap(p, 2 * native_ps));
	context_close(&c);
	return 0;
}

static int write_attribute(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, strlen(value));
	close(fd);
	return n == (ssize_t)strlen(value) ? 0 : -1;
}

static int allocation_failures(unsigned int mode)
{
	struct context c;
	void *p = relocated(2 * native_ps, 0x62);
	int nth = open("/proc/self/fail-nth", O_RDWR);
	unsigned int hit = 0, n;

	CHECK(nth >= 0 && p != MAP_FAILED);
	CHECK(!context_open(&c, mode) && !limit_bytes(8 * native_ps));
	CHECK(!new_domain(&c, 1));
	CHECK(!write_attribute("/sys/kernel/debug/failslab/ignore-gfp-wait",
			       "N"));
	CHECK(!write_attribute(
		"/sys/kernel/debug/fail_page_alloc/ignore-gfp-wait", "N"));
	CHECK(!write_attribute(
		"/sys/kernel/debug/fail_page_alloc/ignore-gfp-highmem", "N"));
	CHECK(!write_attribute("/sys/kernel/debug/failslab/verbose", "0"));
	CHECK(!write_attribute("/sys/kernel/debug/fail_page_alloc/verbose",
			       "0"));
	CHECK(!write_attribute("/sys/kernel/debug/fail_iommufd/verbose", "0"));
	for (n = 1; n <= 512; n++) {
		char number[24], remaining[24] = {};
		int bytes = snprintf(number, sizeof(number), "%u", n);
		ssize_t got, disabled;
		int rc;

		CHECK(pwrite(nth, number, bytes, 0) == bytes);
		rc = map_range(&c, p, 2 * native_ps, IOVA);
		got = pread(nth, remaining, sizeof(remaining) - 1, 0);
		if (got < 0 && errno == EFAULT) {
			strcpy(remaining, "1");
			got = 1;
		}
		disabled = pwrite(nth, "0", 1, 0);
		if (disabled < 0 && errno == EFAULT) {
			disabled = pwrite(nth, "0", 1, 0);
			strcpy(remaining, "1");
		}
		CHECK(disabled == 1 && got > 0);
		if (!rc)
			CHECK(!unmap_range(&c, 2 * native_ps, IOVA));
		CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
		      stat_kb(getpid(), "VmLck:") == 0);
		/* An injected failure must not poison a successful retry or translation. */
		CHECK(!map_range(&c, p, 2 * native_ps, IOVA));
		CHECK(!check_pa(&c, 0, p, 2 * native_ps, IOVA));
		CHECK(!check_pa(&c, 1, p, 2 * native_ps, IOVA));
		CHECK(!unmap_range(&c, 2 * native_ps, IOVA));
		CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
		      stat_kb(getpid(), "VmLck:") == 0);
		if (strtoul(remaining, NULL, 0))
			break;
		hit++;
	}
	CHECK(hit > 0 && n <= 512);
	printf("ok - IOMMUFD %zuK %s accounting allocation-failure sweep: %u sites\n",
	       user_ps / 1024, mode ? "MM" : "USER", hit);
	CHECK(!munmap(p, 2 * native_ps));
	close(nth);
	context_close(&c);
	return 0;
}

static int queue_failure_sweep(unsigned int mode)
{
	struct context c;
	void *p = relocated(2 * native_ps, 0x4c);
	int nth = open("/proc/self/fail-nth", O_RDWR);
	uint32_t viommu, parent, id;
	unsigned int n, hit = 0;

	CHECK(p != MAP_FAILED && nth >= 0 && !context_open(&c, mode));
	CHECK(!limit_bytes(8 * native_ps) &&
	      !create_viommu(&c, &viommu, &parent));
	for (n = 1; n <= 512; n++) {
		char number[24], remaining[24] = {};
		int bytes = snprintf(number, sizeof(number), "%u", n), rc;
		ssize_t got, disabled;

		CHECK(!map_range(&c, p, user_ps, IOVA));
		CHECK(pwrite(nth, number, bytes, 0) == bytes);
		rc = queue_alloc(&c, viommu, 0, IOVA + user_ps / 4 + 3,
				 user_ps / 2, &id);
		got = pread(nth, remaining, sizeof(remaining) - 1, 0);
		if (got < 0 && errno == EFAULT) {
			strcpy(remaining, "1");
			got = 1;
		}
		disabled = pwrite(nth, "0", 1, 0);
		if (disabled < 0 && errno == EFAULT) {
			disabled = pwrite(nth, "0", 1, 0);
			strcpy(remaining, "1");
		}
		CHECK(got > 0 && disabled == 1);
		if (!rc)
			CHECK(!destroy_id(&c, id));
		/* A failed queue must not leave an internal access blocking unmap. */
		CHECK(!unmap_range(&c, user_ps, IOVA));
		CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
		      stat_kb(getpid(), "VmLck:") == 0);
		CHECK(!map_range(&c, p, user_ps, IOVA));
		CHECK(!queue_alloc(&c, viommu, 0, IOVA + user_ps / 4 + 3,
				   user_ps / 2, &id));
		CHECK(!destroy_id(&c, id) && !unmap_range(&c, user_ps, IOVA));
		CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
		      stat_kb(getpid(), "VmLck:") == 0);
		if (strtoul(remaining, NULL, 0))
			break;
		hit++;
	}
	CHECK(hit > 0 && n <= 512);
	CHECK(!destroy_id(&c, viommu) && !destroy_id(&c, parent));
	context_close(&c);
	close(nth);
	CHECK(!munmap(p, 2 * native_ps));
	printf("ok - hardware queue %zuK %s failure sweep: %u sites, no stale locks/pins\n",
	       user_ps / 1024, mode ? "MM" : "USER", hit);
	return 0;
}

static int page_access(struct context *c, void *p, size_t len,
		       unsigned long iova, uint32_t *id)
{
	struct iommu_test_cmd cmd = {
		.size = sizeof(cmd),
		.op = IOMMU_TEST_OP_ACCESS_PAGES,
		.id = c->access,
		.access_pages = { .iova = iova,
				  .length = len,
				  .uptr = (uintptr_t)p,
				  .flags = MOCK_FLAGS_ACCESS_WRITE }
	};
	int rc = ioctl(c->fd, IOMMU_TEST_CMD, &cmd);

	if (!rc)
		*id = cmd.access_pages.out_access_pages_id;
	return rc;
}

static int page_release(struct context *c, uint32_t id)
{
	struct iommu_test_cmd cmd = { .size = sizeof(cmd),
				      .op = IOMMU_TEST_OP_DESTROY_ACCESS_PAGES,
				      .id = c->access,
				      .destroy_access_pages = {
					      .access_pages_id = id } };

	return ioctl(c->fd, IOMMU_TEST_CMD, &cmd);
}

static int page_ranges(unsigned int mode)
{
	struct context c;
	int file = memfd_create("native-page-array", 0);
	unsigned char *p;
	uint32_t a, b, d, e;
	size_t len = 3 * native_ps, tail = native_ps + user_ps / 2 + 3;

	CHECK(file >= 0 && !ftruncate(file, 5 * native_ps));
	p = mmap((void *)VA, len, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED_NOREPLACE, file, native_ps);
	CHECK(p != MAP_FAILED && !limit_bytes(4 * native_ps));
	memset(p, 0x67, len);
	CHECK(!context_open_common(&c, mode, false, true));
	/* Separate areas exercise native output stride with alternative indices. */
	CHECK(!map_range(&c, p, native_ps, IOVA));
	CHECK(!map_range(&c, p + native_ps, 2 * native_ps, IOVA + native_ps));
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	CHECK(!page_access(&c, p, len, IOVA, &a));
	CHECK(stat_kb(getpid(), "VmPin:") == (long)len / 1024);
	CHECK(!page_access(&c, p, len, IOVA, &b));
	CHECK(!page_access(&c, p + native_ps, tail, IOVA + native_ps, &d));
	CHECK(stat_kb(getpid(), "VmPin:") == (long)len / 1024);
	/* Source VA can move without changing its retained physical offset. */
	unsigned char *moved = mremap(p, len, len,
				      MREMAP_MAYMOVE | MREMAP_FIXED,
				      (void *)(VA + 32 * native_ps + user_ps));
	CHECK(moved != MAP_FAILED);
	CHECK(!page_access(&c, moved, len, IOVA, &e) && !page_release(&c, e));
	CHECK(!page_release(&c, a) && !page_release(&c, b));
	CHECK(stat_kb(getpid(), "VmPin:") == (long)(2 * native_ps) / 1024);
	CHECK(!page_release(&c, d));
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	CHECK(mremap(moved, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, p) == p);
	CHECK(!page_access(&c, p, len, IOVA, &a));
	/* Unmap callback releases an access covering both independent areas. */
	CHECK(!unmap_range(&c, len, IOVA));
	CHECK(page_release(&c, a) == -1 && errno == ENOENT);
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	CHECK(!munmap(p, len));

	/* The second area is absent: first area's pin/accounting must roll back. */
	p = mmap((void *)VA, native_ps, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED_NOREPLACE, file, native_ps);
	CHECK(p != MAP_FAILED && !map_range(&c, p, native_ps, IOVA));
	CHECK(page_access(&c, NULL, 2 * native_ps, IOVA, &a) == -1 &&
	      errno == ENOENT);
	CHECK(stat_kb(getpid(), "VmPin:") == 0);
	CHECK(!page_access(&c, p, native_ps, IOVA, &a) && !page_release(&c, a));
	CHECK(!unmap_range(&c, native_ps, IOVA) && !munmap(p, native_ps));

	p = mmap((void *)VA, 2 * native_ps, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED_NOREPLACE, file, native_ps);
	CHECK(p != MAP_FAILED);
	CHECK(mmap(p + native_ps, native_ps, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_FIXED, file, native_ps) == p + native_ps);
	CHECK(!map_range(&c, p, 2 * native_ps, IOVA));
	CHECK(!page_access(&c, p, 2 * native_ps, IOVA, &a));
	CHECK(stat_kb(getpid(), "VmPin:") ==
	      (long)((user_ps == native_ps ? 2 : 1) * native_ps) / 1024);
	CHECK(!page_release(&c, a) && !unmap_range(&c, 2 * native_ps, IOVA));
	CHECK(!munmap(p, 2 * native_ps));

	if (user_ps < native_ps) {
		/* Native-sized logical range whose physical base has a fragment offset. */
		p = mmap((void *)VA, native_ps, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_FIXED_NOREPLACE, file, user_ps);
		CHECK(p != MAP_FAILED && !map_range(&c, p, native_ps, IOVA));
		CHECK(page_access(&c, p, native_ps, IOVA, &a) == -1 &&
		      errno == EINVAL);
		CHECK(stat_kb(getpid(), "VmPin:") == 0);
		CHECK(!unmap_range(&c, native_ps, IOVA) &&
		      !munmap(p, native_ps));

		/* Aligned first fragment followed by an alias, not consecutive bytes. */
		p = mmap((void *)VA, 2 * native_ps, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_FIXED_NOREPLACE, file, 0);
		CHECK(p != MAP_FAILED);
		CHECK(mmap(p + native_ps + user_ps, user_ps,
			   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file,
			   native_ps) == p + native_ps + user_ps);
		CHECK(!map_range(&c, p, native_ps, IOVA));
		CHECK(!map_range(&c, p + native_ps, native_ps,
				 IOVA + native_ps));
		CHECK(page_access(&c, p, 2 * native_ps, IOVA, &a) == -1 &&
		      errno == EINVAL);
		CHECK(stat_kb(getpid(), "VmPin:") == 0);
		CHECK(!page_access(&c, p, native_ps, IOVA, &a));
		CHECK(!unmap_range(&c, 2 * native_ps, IOVA));
		CHECK(page_release(&c, a) == -1 && errno == ENOENT);
		CHECK(!munmap(p, 2 * native_ps));
	}
	context_close(&c);
	close(file);
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	printf("ok - native page arrays %zuK/%zuK %s, lazy/partial/moved/rollback/callback\n",
	       user_ps / 1024, native_ps / 1024, mode ? "MM" : "USER");
	return 0;
}

static int page_lifetime(unsigned int mode)
{
	struct context c;
	int ready[2], go[2], status;
	pid_t child;
	size_t child_ps = user_ps == native_ps ? 4096 : native_ps;
	size_t len = 2 * native_ps;
	unsigned char *p = mmap((void *)(VA + 128 * native_ps), len,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS |
					MAP_FIXED_NOREPLACE,
				-1, 0),
		      *buf = malloc(len);
	uint32_t a, b;
	char byte;

	CHECK(p != MAP_FAILED && buf && !pipe(ready) && !pipe(go));
	memset(p, 0x42, len);
	CHECK(!context_open_common(&c, mode, false, true) && !limit_bytes(len));
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		char ps[24], native[24], fd[24], ioas[24], r[24], g[24];

		close(ready[0]);
		close(go[1]);
		snprintf(ps, sizeof(ps), "%zu", child_ps);
		snprintf(native, sizeof(native), "%zu", native_ps);
		snprintf(fd, sizeof(fd), "%d", c.fd);
		snprintf(ioas, sizeof(ioas), "%u", c.ioas);
		snprintf(r, sizeof(r), "%d", ready[1]);
		snprintf(g, sizeof(g), "%d", go[0]);
		if (prctl(SET_EXEC_PAGE_SIZE, child_ps, 0UL, 0UL, 0UL))
			_exit(4);
		execl("/init", "/init", "--producer", ps, native, fd, ioas, r,
		      g, NULL);
		_exit(5);
	}
	close(ready[1]);
	close(go[0]);
	CHECK(read(ready[0], &byte, 1) == 1 && byte == 'R');
	CHECK(stat_kb(child, "VmPin:") == 0);
	CHECK(!page_access(&c, NULL, len, IOVA, &a));
	CHECK(stat_kb(child, "VmPin:") == (long)len / 1024);
	CHECK(!access_rw(&c, buf, len, IOVA, MOCK_ACCESS_RW_SLOW_PATH));
	CHECK(filled(buf, 0x6d, len));
	byte = 'G';
	CHECK(write(go[1], &byte, 1) == 1);
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	      !WEXITSTATUS(status));
	/* Reuse retained anonymous fragments after source mm_users reaches zero. */
	CHECK(!page_access(&c, NULL, native_ps, IOVA, &b));
	CHECK(!page_release(&c, a));
	CHECK(!access_rw(&c, buf, native_ps, IOVA, MOCK_ACCESS_RW_SLOW_PATH));
	CHECK(filled(buf, 0x6d, native_ps));
	CHECK(!map_range(&c, p, len, ALIAS));
	if (!mode)
		CHECK(page_access(&c, p, len, ALIAS, &a) == -1 &&
		      errno == ENOMEM);
	else
		CHECK(!page_access(&c, p, len, ALIAS, &a) &&
		      !page_release(&c, a));
	/* Final access release refunds even a dead provider's account. */
	CHECK(!page_release(&c, b) && !unmap_range(&c, len, IOVA));
	CHECK(!page_access(&c, p, len, ALIAS, &a) && !page_release(&c, a));
	CHECK(!unmap_range(&c, len, ALIAS));
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	CHECK(!munmap(p, len));
	context_close(&c);
	close(ready[0]);
	close(go[1]);
	free(buf);
	printf("ok - native page array anonymous source %zuK -> %zuK consumer, %s exit/refund\n",
	       child_ps / 1024, user_ps / 1024, mode ? "MM" : "USER");
	return 0;
}

static int page_failure_sweep(unsigned int mode)
{
	struct context c;
	int file = memfd_create("page-array-failures", 0);
	int nth = open("/proc/self/fail-nth", O_RDWR);
	unsigned char *p;
	uint32_t id;
	unsigned int n, hit = 0;

	CHECK(file >= 0 && nth >= 0 && !ftruncate(file, 2 * native_ps));
	p = mmap((void *)VA, 2 * native_ps, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED_NOREPLACE, file, 0);
	CHECK(p != MAP_FAILED);
	memset(p, 0x53, 2 * native_ps);
	CHECK(!context_open_common(&c, mode, false, true) &&
	      !limit_bytes(2 * native_ps));
	CHECK(!map_range(&c, p, native_ps, IOVA));
	CHECK(!map_range(&c, p + native_ps, native_ps, IOVA + native_ps));
	for (n = 1; n <= 512; n++) {
		char number[24], remaining[24] = {};
		int bytes = snprintf(number, sizeof(number), "%u", n), rc;
		ssize_t got, disabled;

		CHECK(pwrite(nth, number, bytes, 0) == bytes);
		rc = page_access(&c, p, 2 * native_ps, IOVA, &id);
		got = pread(nth, remaining, sizeof(remaining) - 1, 0);
		if (got < 0 && errno == EFAULT) {
			strcpy(remaining, "1");
			got = 1;
		}
		disabled = pwrite(nth, "0", 1, 0);
		if (disabled < 0 && errno == EFAULT) {
			disabled = pwrite(nth, "0", 1, 0);
			strcpy(remaining, "1");
		}
		CHECK(got > 0 && disabled == 1);
		if (!rc)
			CHECK(!page_release(&c, id));
		CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
		      stat_kb(getpid(), "VmLck:") == 0);
		CHECK(!page_access(&c, p, 2 * native_ps, IOVA, &id) &&
		      !page_release(&c, id));
		CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
		      stat_kb(getpid(), "VmLck:") == 0);
		if (strtoul(remaining, NULL, 0))
			break;
		hit++;
	}
	CHECK(hit > 0 && n <= 512);
	CHECK(!unmap_range(&c, 2 * native_ps, IOVA) &&
	      !munmap(p, 2 * native_ps));
	context_close(&c);
	close(file);
	close(nth);
	printf("ok - native page arrays %zuK %s failure sweep: %u sites, refunds/retries\n",
	       user_ps / 1024, mode ? "MM" : "USER", hit);
	return 0;
}

struct page_race {
	struct context *c;
	void *p;
	pthread_barrier_t barrier;
	atomic_int failed, pinned, absent;
};

static void *page_pin_thread(void *opaque)
{
	struct page_race *r = opaque;

	pthread_barrier_wait(&r->barrier);
	for (unsigned int i = 0; i < 256; i++) {
		uint32_t id;

		if (page_access(r->c, r->p, 2 * native_ps, IOVA, &id)) {
			/* Unmap can mark the area inaccessible before notifying drivers. */
			if (errno != ENOENT && errno != EINVAL)
				goto fail;
			atomic_fetch_add(&r->absent, 1);
		} else {
			atomic_fetch_add(&r->pinned, 1);
			sched_yield();
			if (page_release(r->c, id) && errno != ENOENT)
				goto fail;
		}
	}
	return NULL;
fail:
	printf("FAIL page pin/unmap race errno=%d\n", errno);
	atomic_store(&r->failed, 1);
	return NULL;
}

static void *page_unmap_thread(void *opaque)
{
	struct page_race *r = opaque;

	pthread_barrier_wait(&r->barrier);
	for (unsigned int i = 0; i < 256; i++) {
		if (unmap_range(r->c, 2 * native_ps, IOVA))
			goto fail;
		sched_yield();
		if (map_range(r->c, r->p, 2 * native_ps, IOVA))
			goto fail;
	}
	return NULL;
fail:
	printf("FAIL page unmap/remap race errno=%d\n", errno);
	atomic_store(&r->failed, 1);
	return NULL;
}

static int page_concurrency(unsigned int mode)
{
	struct context c;
	struct page_race r = { .c = &c };
	pthread_t threads[4];
	int file = memfd_create("page-array-race", 0);

	CHECK(file >= 0 && !ftruncate(file, 2 * native_ps));
	r.p = mmap((void *)VA, 2 * native_ps, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_FIXED_NOREPLACE, file, 0);
	CHECK(r.p != MAP_FAILED);
	memset(r.p, 0x49, 2 * native_ps);
	CHECK(!context_open_common(&c, mode, false, true) &&
	      !limit_bytes(8 * native_ps));
	CHECK(!map_range(&c, r.p, 2 * native_ps, IOVA));
	CHECK(!pthread_barrier_init(&r.barrier, NULL, 4));
	for (unsigned int i = 0; i < 4; i++)
		CHECK(!pthread_create(
			&threads[i], NULL,
			i == 3 ? page_unmap_thread : page_pin_thread, &r));
	for (unsigned int i = 0; i < 4; i++)
		CHECK(!pthread_join(threads[i], NULL));
	CHECK(!atomic_load(&r.failed) && atomic_load(&r.pinned) > 0);
	CHECK(!unmap_range(&c, 2 * native_ps, IOVA));
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	CHECK(!munmap(r.p, 2 * native_ps));
	context_close(&c);
	close(file);
	CHECK(!pthread_barrier_destroy(&r.barrier));
	printf("ok - native page arrays %zuK %s race: %d pins, %d unavailable, 256 remaps\n",
	       user_ps / 1024, mode ? "MM" : "USER", atomic_load(&r.pinned),
	       atomic_load(&r.absent));
	return 0;
}

static int dirty_ioctl(struct context *c, uint32_t hwpt, void *bitmap,
		       unsigned long iova, size_t length, size_t granule,
		       unsigned int flags)
{
	struct iommu_hwpt_get_dirty_bitmap cmd = { .size = sizeof(cmd),
						   .hwpt_id = hwpt,
						   .flags = flags,
						   .iova = iova,
						   .length = length,
						   .page_size = granule,
						   .data = (uintptr_t)bitmap };

	return ioctl(c->fd, IOMMU_HWPT_GET_DIRTY_BITMAP, &cmd);
}

static int dirty_errors(unsigned int mode)
{
	struct context c;
	size_t len = 66 * native_ps;
	unsigned char *data = relocated(len, 0x34);
	unsigned char *out = mmap(NULL, 2 * user_ps, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint64_t *bitmap = (void *)(out + user_ps - 8), seed[2] = { 1, 2 };
	struct iommu_option option = { .size = sizeof(option),
				       .option_id = IOMMU_OPTION_HUGE_PAGES,
				       .op = IOMMU_OPTION_OP_SET,
				       .val64 = 0 };
	struct iommu_hwpt_alloc alloc = {
		.size = sizeof(alloc), .flags = IOMMU_HWPT_ALLOC_DIRTY_TRACKING
	};
	struct iommu_hwpt_set_dirty_tracking enable = {
		.size = sizeof(enable),
		.flags = IOMMU_HWPT_DIRTY_TRACKING_ENABLE
	};
	struct iommu_test_cmd mark = { .size = sizeof(mark),
				       .op = IOMMU_TEST_OP_DIRTY,
				       .dirty = { .iova = IOVA,
						  .length = len,
						  .page_size = native_ps,
						  .uptr = (uintptr_t)seed } };
	unsigned int no_clear = IOMMU_HWPT_GET_DIRTY_BITMAP_NO_CLEAR;
	int failures = 0, rc, error;

	CHECK(data != MAP_FAILED && out != MAP_FAILED);
	CHECK(!context_open(&c, mode) && !limit_bytes(128 * native_ps));
	option.object_id = c.ioas;
	CHECK(!ioctl(c.fd, IOMMU_OPTION, &option));
	CHECK(!map_range(&c, data, len, IOVA));
	alloc.dev_id = c.dev[0];
	alloc.pt_id = c.ioas;
	CHECK(!ioctl(c.fd, IOMMU_HWPT_ALLOC, &alloc));
	enable.hwpt_id = alloc.out_hwpt_id;
	CHECK(!ioctl(c.fd, IOMMU_HWPT_SET_DIRTY_TRACKING, &enable));
	mark.id = alloc.out_hwpt_id;
	CHECK(!ioctl(c.fd, IOMMU_TEST_CMD, &mark) &&
	      mark.dirty.out_nr_dirty == 2);
	CHECK(!dirty_ioctl(&c, alloc.out_hwpt_id, bitmap, IOVA, len, native_ps,
			   no_clear));
	CHECK(bitmap[0] == 1 && bitmap[1] == 2);
	CHECK(filled(out, 0, user_ps - 8) &&
	      filled(out + user_ps + 8, 0, user_ps - 8));

#define EXPECT_DIRTY_ERROR(call, expected, label)                             \
	do {                                                                  \
		errno = 0;                                                    \
		rc = (call);                                                  \
		error = errno;                                                \
		if (rc != -1 || error != (expected)) {                        \
			printf("FAIL dirty %s: rc=%d errno=%d expected=%d\n", \
			       label, rc, error, expected);                   \
			failures++;                                           \
		}                                                             \
	} while (0)
	EXPECT_DIRTY_ERROR(dirty_ioctl(&c, alloc.out_hwpt_id, bitmap,
				       IOVA + len, native_ps, native_ps,
				       no_clear),
			   EINVAL, "unmapped IOVA");
	EXPECT_DIRTY_ERROR(dirty_ioctl(&c, alloc.out_hwpt_id, (void *)1, IOVA,
				       len, native_ps, no_clear),
			   EFAULT, "invalid output pointer");
	EXPECT_DIRTY_ERROR(dirty_ioctl(&c, alloc.out_hwpt_id, bitmap, IOVA,
				       4 * native_ps, 3 * native_ps, no_clear),
			   EINVAL, "non-power-of-two granule");
	CHECK(!mprotect(out, 2 * user_ps, PROT_READ));
	EXPECT_DIRTY_ERROR(dirty_ioctl(&c, alloc.out_hwpt_id, bitmap, IOVA, len,
				       native_ps, no_clear),
			   EFAULT, "read-only output");
	CHECK(!mprotect(out, 2 * user_ps, PROT_READ | PROT_WRITE));
	memset(out, 0, 2 * user_ps);
	CHECK(!munmap(out + user_ps, user_ps));
	EXPECT_DIRTY_ERROR(dirty_ioctl(&c, alloc.out_hwpt_id, bitmap, IOVA, len,
				       native_ps, no_clear),
			   EFAULT, "output hole after valid prefix");
	CHECK(bitmap[0] == 1);
	CHECK(mmap(out + user_ps, user_ps, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
		   0) == out + user_ps);
	/* NO_CLEAR errors retain dirty state, so fixing the buffer permits retry. */
	CHECK(!dirty_ioctl(&c, alloc.out_hwpt_id, bitmap, IOVA, len, native_ps,
			   0));
	CHECK(bitmap[0] == 1 && bitmap[1] == 2);
	memset(out, 0, 2 * user_ps);
	CHECK(!dirty_ioctl(&c, alloc.out_hwpt_id, bitmap, IOVA, len, native_ps,
			   no_clear));
	CHECK(bitmap[0] == 0 && bitmap[1] == 0);
	CHECK(!unmap_range(&c, len, IOVA));
	CHECK(!munmap(data, len) && !munmap(out, 2 * user_ps));
	context_close(&c);
	CHECK(stat_kb(getpid(), "VmPin:") == 0 &&
	      stat_kb(getpid(), "VmLck:") == 0);
	CHECK(!failures);
#undef EXPECT_DIRTY_ERROR
	return 0;
}

static int exercise(void)
{
	CHECK(!drop_pin_cap());
	for (unsigned int mode = 0; mode < 2; mode++) {
		CHECK(!limits_and_lifetime(mode));
		CHECK(!holes_and_shared(mode));
		CHECK(!queue_ranges(mode));
		CHECK(!page_ranges(mode));
		CHECK(!page_lifetime(mode));
		CHECK(!cross_process(mode));
		CHECK(!concurrent_maps(mode));
		CHECK(!allocation_failures(mode));
		CHECK(!queue_failure_sweep(mode));
		CHECK(!page_failure_sweep(mode));
		CHECK(!page_concurrency(mode));
		CHECK(!dirty_errors(mode));
		printf("ok - IOMMUFD %zuK userspace/%zuK kernel %s accounting, limits/lifetime/shared/cross-process/256 concurrent cycles\n",
		       user_ps / 1024, native_ps / 1024, mode ? "MM" : "USER");
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fail = 0;

	setbuf(stdout, NULL);
	if (argc >= 4) {
		user_ps = strtoul(argv[2], NULL, 0);
		native_ps = strtoul(argv[3], NULL, 0);
		CHECK(getauxval(AT_PAGESZ) == user_ps);
		if (!strcmp(argv[1], "--exercise"))
			return exercise();
		if (!strcmp(argv[1], "--producer") && argc == 8)
			return producer(argc, argv);
		return 2;
	}
	CHECK(getpid() == 1);
	CHECK(!mount("proc", "/proc", "proc", 0, NULL));
	CHECK(!mount("devtmpfs", "/dev", "devtmpfs", 0, NULL));
	CHECK(!mount("sysfs", "/sys", "sysfs", 0, NULL));
	CHECK(!mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL));
	native_ps = getauxval(AT_PAGESZ);
	size_t sizes[] = { native_ps, 4096, 16384 };

	for (unsigned int i = 0; i < (native_ps > 16384 ? 3U : 2U); i++) {
		pid_t child = fork();
		int status = 0, ok = 0;

		if (!child) {
			char size[24], native[24];

			snprintf(size, sizeof(size), "%zu", sizes[i]);
			snprintf(native, sizeof(native), "%zu", native_ps);
			if (prctl(SET_EXEC_PAGE_SIZE, sizes[i], 0UL, 0UL, 0UL))
				_exit(4);
			execl("/init", "/init", "--exercise", size, native,
			      NULL);
			_exit(5);
		}
		for (unsigned int ticks = 0; child > 0 && ticks < 1800;
		     ticks++) {
			if (waitpid(child, &status, WNOHANG) == child) {
				ok = WIFEXITED(status) && !WEXITSTATUS(status);
				break;
			}
			if (ticks == 1799) {
				kill(child, SIGKILL);
				waitpid(child, &status, 0);
			}
			usleep(100000);
		}
		printf("%s - IOMMUFD ABI %zuK status=%#x\n",
		       ok ? "ok" : "not ok", sizes[i] / 1024, status);
		fail += !ok;
	}
	printf("IOMMUFD IOCTL CONTRACT %s\n", fail ? "FAIL" : "PASS");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
